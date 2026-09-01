/*
 * SPDX-FileCopyrightText: 2026 scwan
 * SPDX-License-Identifier: Apache-2.0
 *
 * 05_band_sweep - where is there anything to measure?
 *
 * Walks every channel the C5 knows, on both bands, dwelling on each one and
 * counting the CSI records that arrive and which PHY produced them. The output
 * is a survey: which channels carry traffic here, which carry 802.11ax traffic
 * worth capturing at full HE-LTF resolution, and how strong it is.
 *
 * This is the example that only makes sense on a C5. Every other Espressif chip
 * with CSI is 2.4 GHz only, so a sweep like this covers a handful of channels;
 * the C5 has 28 more.
 *
 * It also demonstrates the one genuinely awkward operation on a dual-band chip:
 * moving the radio across the band boundary. Within a band a channel hop is one
 * call. Across bands the band mode has to change, and the per-band protocol
 * bitmap with it, so this example rebuilds the radio rather than assuming a
 * live switch will work.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "csi_c5.h"

static const char *TAG = "csi_sweep";

/* Exactly the channels CHANNEL_TO_BIT_NUMBER() in esp_wifi_types_generic.h
 * recognises, minus the 2.4 GHz channels that overlap their neighbours. */
static const uint8_t CH_24[] = { 1, 6, 11, 13 };
static const uint8_t CH_5[]  = {
    36,  40,  44,  48,  52,  56,  60,  64,
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165, 169, 173, 177,
};

#define BB_FORMAT_SLOTS 16

typedef struct {
    uint8_t  channel;
    uint32_t records;
    uint32_t by_format[BB_FORMAT_SLOTS];
    int      best_rssi;
    uint32_t invalid_ce;
} channel_stats_t;

static csi_c5_config_t s_cfg;
static bool            s_five;      /* which band the radio is currently in */

static uint8_t protocols_for(bool five)
{
    return five
        ? (WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX)
        : (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N  | WIFI_PROTOCOL_11AX);
}

/*
 * Move to a channel, rebuilding the radio if that means changing band.
 *
 * Within one band this is a single esp_wifi_set_channel(). Across bands the
 * band mode has to change first, and esp_wifi_set_protocol() has to be reapplied
 * because it is per band - forget that and the receiver silently stops decoding
 * whole PHYs on the new band, which looks like "that channel is quiet".
 */
static esp_err_t goto_channel(uint8_t channel)
{
    const bool five = csi_c5_is_5ghz(channel);

    if (five == s_five) {
        return csi_c5_park(channel, WIFI_SECOND_CHAN_NONE);
    }

    ESP_LOGI(TAG, "crossing to %s", five ? "5 GHz" : "2.4 GHz");

    /*
     * Note what this does NOT do: stop and restart Wi-Fi. Both
     * esp_wifi_set_band_mode() and esp_wifi_set_channel() require Wi-Fi to be
     * STARTED - they document ESP_ERR_WIFI_NOT_STARTED - so a stop/start cycle
     * around the band change would put the two calls in the one state where
     * they are guaranteed to fail.
     *
     * Turning CSI and promiscuous mode off first, via csi_c5_stop(), leaves a
     * plain started station, which is the state these calls expect.
     */
    csi_c5_stop();

    ESP_ERROR_CHECK(esp_wifi_set_band_mode(five ? WIFI_BAND_MODE_5G_ONLY
                                                : WIFI_BAND_MODE_2G_ONLY));

    /* Per band, and easy to forget. Skip it and the receiver quietly stops
     * decoding whole PHYs on the new band - which reads as "this channel is
     * quiet", not as an error. */
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, protocols_for(five)));

    s_five = five;
    s_cfg.channel = channel;
    return csi_c5_start(&s_cfg);
}

static void dwell(channel_stats_t *st, uint32_t ms)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    csi_c5_record_t rec;

    st->best_rssi = -128;

    for (;;) {
        const TickType_t now = xTaskGetTickCount();
        /* Signed compare, and recomputed in one read. Doing it as
         * "deadline - xTaskGetTickCount()" after a separate loop condition
         * lets a tick land in between, underflow the unsigned subtraction and
         * block for 49 days. */
        if ((int32_t) (deadline - now) <= 0) {
            break;
        }
        if (csi_c5_receive(&rec, deadline - now) != pdTRUE) {
            continue;
        }
        st->records++;
        st->by_format[rec.rx_ctrl.cur_bb_format & (BB_FORMAT_SLOTS - 1)]++;
        if (!rec.rx_ctrl.rx_channel_estimate_info_vld) {
            st->invalid_ce++;
        }
        if ((int) rec.rx_ctrl.rssi > st->best_rssi) {
            st->best_rssi = rec.rx_ctrl.rssi;
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* Build the channel list for the configured bands. */
    uint8_t list[sizeof(CH_24) + sizeof(CH_5)];
    size_t  n = 0;

#if CONFIG_CSI_SWEEP_BAND_24 || CONFIG_CSI_SWEEP_BAND_BOTH
    for (size_t i = 0; i < sizeof(CH_24); i++) {
        list[n++] = CH_24[i];
    }
#endif
#if CONFIG_CSI_SWEEP_BAND_5 || CONFIG_CSI_SWEEP_BAND_BOTH
    for (size_t i = 0; i < sizeof(CH_5); i++) {
        list[n++] = CH_5[i];
    }
#endif

    if (n == 0) {
        ESP_LOGE(TAG, "no bands selected");
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    /* Band mode after start, protocol bitmap after band mode - see
     * goto_channel() for why. */
    s_five = csi_c5_is_5ghz(list[0]);
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(s_five ? WIFI_BAND_MODE_5G_ONLY
                                                  : WIFI_BAND_MODE_2G_ONLY));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, protocols_for(s_five)));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    s_cfg             = CSI_C5_CONFIG_DEFAULT();
    s_cfg.channel     = list[0];
    s_cfg.promiscuous = true;
    s_cfg.dump_ack    = true;
    s_cfg.force_lltf  = false;   /* we want to see which PHYs are actually in use */
    s_cfg.val_scale   = 0;
    s_cfg.queue_len   = 16;

    ESP_ERROR_CHECK(csi_c5_start(&s_cfg));

    ESP_LOGI(TAG, "sweeping %u channels, %d ms each - one pass takes about %u seconds",
             (unsigned) n, CONFIG_CSI_SWEEP_DWELL_MS,
             (unsigned) ((n * CONFIG_CSI_SWEEP_DWELL_MS) / 1000));

    static channel_stats_t stats[sizeof(CH_24) + sizeof(CH_5)];
    uint32_t pass = 0;

    for (;;) {
        pass++;
        memset(stats, 0, sizeof(stats));

        for (size_t i = 0; i < n; i++) {
            stats[i].channel = list[i];

            if (goto_channel(list[i]) != ESP_OK) {
                ESP_LOGW(TAG, "could not tune channel %u, skipping", list[i]);
                continue;
            }
            dwell(&stats[i], CONFIG_CSI_SWEEP_DWELL_MS);
        }

        printf("\n");
        printf("SWEEP pass %" PRIu32 "\n", pass);
        printf("  ch band  records  11b  11g/a   11n  11ac  11ax  bestRSSI  badCE\n");

        for (size_t i = 0; i < n; i++) {
            const channel_stats_t *s = &stats[i];

            const uint32_t ax = s->by_format[RX_BB_FORMAT_HE_SU]
                              + s->by_format[RX_BB_FORMAT_HE_MU]
                              + s->by_format[RX_BB_FORMAT_HE_ERSU]
                              + s->by_format[RX_BB_FORMAT_HE_TB];
            const uint32_t ac = s->by_format[RX_BB_FORMAT_VHT]
                              + s->by_format[RX_BB_FORMAT_VHT_MU];

            printf("%4u %4s %8" PRIu32 " %4" PRIu32 " %6" PRIu32 " %5" PRIu32
                   " %5" PRIu32 " %5" PRIu32 " %9d %6" PRIu32 "\n",
                   s->channel,
                   csi_c5_is_5ghz(s->channel) ? "5G" : "2G4",
                   s->records,
                   s->by_format[RX_BB_FORMAT_11B],
                   s->by_format[RX_BB_FORMAT_11G],
                   s->by_format[RX_BB_FORMAT_HT],
                   ac, ax,
                   s->records ? s->best_rssi : 0,
                   s->invalid_ce);
        }
        printf("\n");
    }
}
