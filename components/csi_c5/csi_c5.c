/*
 * SPDX-FileCopyrightText: 2026 scwan
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdio.h>

#include "csi_c5.h"

#include "esp_log.h"
#include "esp_wifi.h"

#if defined(CONFIG_SOC_WIFI_HE_SUPPORT)
#include "esp_wifi_he_types.h"
#endif

static const char *TAG = "csi_c5";

static QueueHandle_t   s_queue;
static uint32_t        s_dropped;
static csi_c5_config_t s_cfg;

/* ------------------------------------------------------------------------ *
 * Channels
 * ------------------------------------------------------------------------ */

bool csi_c5_is_5ghz(uint8_t channel)
{
    return channel >= 36;
}

bool csi_c5_channel_valid(uint8_t channel)
{
    if (channel >= 1 && channel <= 14) {
        return true;                                    /* 2.4 GHz */
    }
    if (channel >= 36 && channel <= 64 && ((channel - 36) % 4) == 0) {
        return true;                                    /* UNII-1 / UNII-2A */
    }
    if (channel >= 100 && channel <= 144 && ((channel - 100) % 4) == 0) {
        return true;                                    /* UNII-2C (DFS) */
    }
    if (channel >= 149 && channel <= 177 && ((channel - 149) % 4) == 0) {
        return true;                                    /* UNII-3 / UNII-4 */
    }
    return false;
}

esp_err_t csi_c5_park(uint8_t channel, wifi_second_chan_t second)
{
    if (!csi_c5_channel_valid(channel)) {
        ESP_LOGE(TAG, "channel %u is not a channel this chip knows", channel);
        return ESP_ERR_INVALID_ARG;
    }

    const bool five = csi_c5_is_5ghz(channel);

    /*
     * Band mode gates which channels are legal. Pin it to one band rather than
     * WIFI_BAND_MODE_AUTO: esp_wifi_set_protocol(), esp_wifi_set_bandwidth()
     * and their getters all return ESP_ERR_NOT_SUPPORTED under AUTO, so a
     * single-band mode keeps the rest of the API usable.
     */
    const wifi_band_mode_t mode = five ? WIFI_BAND_MODE_5G_ONLY : WIFI_BAND_MODE_2G_ONLY;
    esp_err_t err = esp_wifi_set_band_mode(mode);
    if (err != ESP_OK) {
        /*
         * Not fatal by itself - if the radio is already in the right band the
         * channel set below still succeeds. It is fatal if we are crossing
         * bands, and then esp_wifi_set_channel() reports it.
         */
        ESP_LOGW(TAG, "esp_wifi_set_band_mode(%d): %s", mode, esp_err_to_name(err));
    }

    if (five && second != WIFI_SECOND_CHAN_NONE) {
        ESP_LOGD(TAG, "5 GHz: hardware derives the second channel, ignoring the requested one");
        second = WIFI_SECOND_CHAN_NONE;
    }

    err = esp_wifi_set_channel(channel, second);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_channel(%u): %s", channel, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "parked on channel %u (%s)", channel, five ? "5 GHz" : "2.4 GHz");
    return ESP_OK;
}

/* ------------------------------------------------------------------------ *
 * Reception format names
 * ------------------------------------------------------------------------ */

const char *csi_c5_bb_format_str(uint32_t cur_bb_format)
{
    switch (cur_bb_format) {
    /* RX_BB_FORMAT_11A is an alias of RX_BB_FORMAT_11G (both == 1), so it
     * cannot appear as a separate case label. On the C5 a value of 1 means
     * 11g on 2.4 GHz and 11a on 5 GHz. */
    case RX_BB_FORMAT_11B:     return "11b";
    case RX_BB_FORMAT_11G:     return "11g/11a";
    case RX_BB_FORMAT_HT:      return "11n";
    case RX_BB_FORMAT_VHT:     return "11ac";
    case RX_BB_FORMAT_HE_SU:   return "11ax-SU";
    case RX_BB_FORMAT_HE_MU:   return "11ax-MU";
    case RX_BB_FORMAT_HE_ERSU: return "11ax-ERSU";
    case RX_BB_FORMAT_HE_TB:   return "11ax-TB";
    case RX_BB_FORMAT_VHT_MU:  return "11ac-MU";
    default:                   return "unknown";
    }
}

/* ------------------------------------------------------------------------ *
 * The callback
 *
 * Runs in the Wi-Fi task. The ESP-IDF Wi-Fi guide: "do not do lengthy
 * operations in the callback function. Instead, post necessary data to a queue
 * and handle it from a lower priority task." Blocking here stalls the whole
 * Wi-Fi stack, so the queue send uses a zero timeout and drops on overflow.
 * ------------------------------------------------------------------------ */

static void csi_rx_cb(void *ctx, wifi_csi_info_t *info)
{
    (void) ctx;

    if (info == NULL || info->buf == NULL || info->len == 0) {
        return;
    }

    static csi_c5_record_t rec;     /* Wi-Fi task stacks are small; do not put 500 bytes on it */

    rec.rx_ctrl            = info->rx_ctrl;
    rec.rx_seq             = info->rx_seq;
    rec.first_word_invalid = info->first_word_invalid;

    /*
     * Which packing this record uses is decided by the acquisition config, not
     * by anything in the record itself - there is no flag in wifi_csi_info_t to
     * read it back from. Forcing L-LTF puts the chip in its 12-bit mode, so
     * that is what the config flag implies.
     */
    rec.packing = s_cfg.force_lltf ? CSI_C5_PACK_LLTF12 : CSI_C5_PACK_IQ8;
    memcpy(rec.src_mac, info->mac,  6);
    memcpy(rec.dst_mac, info->dmac, 6);

    uint16_t len = info->len;
    if (len > CSI_C5_MAX_CSI_BYTES) {
        /* Should not happen on the C5 - the documented maximum is 490 - but a
         * silent overrun here would corrupt the queue, so clamp and say so. */
        ESP_LOGW(TAG, "CSI record of %u bytes exceeds the %d byte buffer, truncating",
                 (unsigned) len, CSI_C5_MAX_CSI_BYTES);
        len = CSI_C5_MAX_CSI_BYTES;
    }
    rec.len = len;
    memcpy(rec.buf, info->buf, len);

    if (s_queue == NULL || xQueueSend(s_queue, &rec, 0) != pdTRUE) {
        s_dropped++;
    }
}

/* ------------------------------------------------------------------------ *
 * Lifecycle
 * ------------------------------------------------------------------------ */

esp_err_t csi_c5_start(const csi_c5_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_queue != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_cfg     = *cfg;
    s_dropped = 0;

    s_queue = xQueueCreate(s_cfg.queue_len ? s_cfg.queue_len : 16, sizeof(csi_c5_record_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "out of memory for a %u record queue (%u bytes each)",
                 (unsigned) s_cfg.queue_len, (unsigned) sizeof(csi_c5_record_t));
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err;

    /*
     * Promiscuous mode first. Without it a station only ever sees frames
     * addressed to it, which after association means a trickle from the AP and
     * before association means nothing at all. The Wi-Fi guide recommends it
     * for exactly this reason.
     */
    if (s_cfg.promiscuous) {
        const wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL };
        err = esp_wifi_set_promiscuous_filter(&filter);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_promiscuous_filter: %s", esp_err_to_name(err));
            goto fail;
        }

        /* Control frames are filtered separately, and ACKs are control frames.
         * dump_ack_en below only has anything to work with if they get past
         * this filter. */
        const wifi_promiscuous_filter_t ctrl = { .filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ALL };
        err = esp_wifi_set_promiscuous_ctrl_filter(&ctrl);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_promiscuous_ctrl_filter: %s", esp_err_to_name(err));
            goto fail;
        }

        err = esp_wifi_set_promiscuous(true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_promiscuous: %s", esp_err_to_name(err));
            goto fail;
        }
    }

    if (s_cfg.channel != 0) {
        err = csi_c5_park(s_cfg.channel, s_cfg.second);
        if (err != ESP_OK) {
            goto fail;
        }
    } else {
        /* Associated: the AP owns the channel. Moving it here would drop the
         * connection, and the reconnect storm that follows looks exactly like
         * a CSI bug. */
        uint8_t            primary = 0;
        wifi_second_chan_t second  = WIFI_SECOND_CHAN_NONE;
        if (esp_wifi_get_channel(&primary, &second) == ESP_OK) {
            ESP_LOGI(TAG, "leaving the radio where it is: channel %u", primary);
        }
    }

    err = esp_wifi_set_csi_rx_cb(csi_rx_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_csi_rx_cb: %s", esp_err_to_name(err));
        goto fail;
    }

    /*
     * The C5 acquisition config. Field-for-field this is the
     * SOC_WIFI_MAC_VERSION_NUM == 3 branch of wifi_csi_acquire_config_t.
     *
     * lltf_bit_mode is deliberately not mentioned. Omitted designated
     * initialisers are zeroed, and zero is its documented default (12-bit), so
     * naming it buys nothing - and it does not exist before ESP-IDF v5.5, which
     * added it and shrank the reserved field from 15 bits to 14 to make room.
     */
    wifi_csi_config_t csi = {
        .enable                   = 1,
        .acquire_csi_legacy       = 1,   /* L-LTF from 11a/11g PPDUs */
        .acquire_csi_force_lltf   = s_cfg.force_lltf ? 1 : 0,
        .acquire_csi_ht20         = 1,
        .acquire_csi_ht40         = 1,
        .acquire_csi_vht          = 1,   /* C5 only; the C6 struct has no such field */
        .acquire_csi_su           = 1,   /* HE20 single user */
        .acquire_csi_mu           = 1,   /* HE20 multi user */
        .acquire_csi_dcm          = 1,
        .acquire_csi_beamformed   = 1,
        .acquire_csi_he_stbc_mode = ESP_CSI_ACQUIRE_STBC_SAMPLE_HELTFS,
        .val_scale_cfg            = s_cfg.val_scale,   /* 0..8 on this chip */
        .dump_ack_en              = s_cfg.dump_ack ? 1 : 0,
    };

    err = esp_wifi_set_csi_config(&csi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_csi_config: %s "
                      "(ESP_ERR_WIFI_NOT_STARTED here means esp_wifi_start() has not run yet)",
                 esp_err_to_name(err));
        goto fail;
    }

    err = esp_wifi_set_csi(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_csi: %s", esp_err_to_name(err));
        goto fail;
    }

    ESP_LOGI(TAG, "CSI enabled: channel %u, promiscuous %s, dump_ack %s, val_scale %u",
             s_cfg.channel,
             s_cfg.promiscuous ? "on" : "off",
             s_cfg.dump_ack ? "on" : "off",
             (unsigned) s_cfg.val_scale);
    ESP_LOGI(TAG, "  LTF: %s",
             s_cfg.force_lltf
                 ? "L-LTF forced -> 12-bit packing, 4 bytes per tone, 106-byte records"
                 : "HT/VHT/HE-LTF -> 8-bit int8 pairs, 2 bytes per tone");
    return ESP_OK;

fail:
    vQueueDelete(s_queue);
    s_queue = NULL;
    return err;
}

void csi_c5_stop(void)
{
    esp_wifi_set_csi(false);
    esp_wifi_set_csi_rx_cb(NULL, NULL);
    if (s_cfg.promiscuous) {
        esp_wifi_set_promiscuous(false);
    }
    if (s_queue != NULL) {
        QueueHandle_t q = s_queue;
        s_queue = NULL;             /* stop the callback touching it first */
        vQueueDelete(q);
    }
}

BaseType_t csi_c5_receive(csi_c5_record_t *out, TickType_t ticks_to_wait)
{
    if (s_queue == NULL || out == NULL) {
        return pdFALSE;
    }
    return xQueueReceive(s_queue, out, ticks_to_wait);
}

uint32_t csi_c5_dropped(void)
{
    return s_dropped;
}

/* ------------------------------------------------------------------------ *
 * CSV
 * ------------------------------------------------------------------------ */

void csi_c5_print_csv_header(void)
{
    printf("%s\n", CSI_C5_CSV_HEADER);
}

void csi_c5_print_csv(const csi_c5_record_t *r)
{
    /* One line can reach roughly 490 * 5 characters of payload plus the
     * metadata. Built in a static buffer and written once, because a printf per
     * sub-carrier is slow enough to become the bottleneck. Single-task use
     * only, which is what the header promises. */
    static char line[3584];

    int n = snprintf(line, sizeof(line),
                     "CSI_DATA,%u,"
                     "%02x:%02x:%02x:%02x:%02x:%02x,%02x:%02x:%02x:%02x:%02x:%02x,"
                     "%d,%u,%d,%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%s,%d,%u,\"[",
                     (unsigned) r->rx_seq,
                     r->src_mac[0], r->src_mac[1], r->src_mac[2],
                     r->src_mac[3], r->src_mac[4], r->src_mac[5],
                     r->dst_mac[0], r->dst_mac[1], r->dst_mac[2],
                     r->dst_mac[3], r->dst_mac[4], r->dst_mac[5],
                     (int) r->rx_ctrl.rssi,
                     (unsigned) r->rx_ctrl.rate,
                     (int) r->rx_ctrl.noise_floor,
                     csi_c5_bb_format_str(r->rx_ctrl.cur_bb_format),
                     (unsigned) r->rx_ctrl.channel,
                     (unsigned) r->rx_ctrl.second,
                     (unsigned) r->rx_ctrl.is_group,
                     (unsigned) r->rx_ctrl.timestamp,
                     (unsigned) r->rx_ctrl.sig_len,
                     (unsigned) r->rx_ctrl.rx_state,
                     (unsigned) r->rx_ctrl.rx_channel_estimate_len,
                     (unsigned) r->rx_ctrl.rx_channel_estimate_info_vld,
                     (unsigned) r->first_word_invalid,
                     csi_c5_packing_str(r->packing),
                     csi_c5_subcarriers(r),
                     (unsigned) r->len);

    /* snprintf returns the length it WOULD have written, so n can run past the
     * buffer on truncation and then "sizeof(line) - n" underflows into a huge
     * size_t. The clamp costs nothing and removes the need to prove it cannot
     * happen. (It cannot, at 492 bytes times five characters, but proofs like
     * that stop holding the moment someone changes a constant.) */
    const int limit = (int) sizeof(line) - 8;
    const int count = r->len;
    for (int i = 0; i < count && n < limit; i++) {
        n += snprintf(line + n, sizeof(line) - (size_t) n, "%s%d", i ? "," : "", (int) r->buf[i]);
        if (n > limit) {
            n = limit;
            break;
        }
    }

    snprintf(line + n, sizeof(line) - (size_t) n, "]\"");
    printf("%s\n", line);
}
