/*
 * SPDX-FileCopyrightText: 2026 scwan
 * SPDX-License-Identifier: Apache-2.0
 *
 * 02_espnow_pair / rx - the receiver half.
 *
 * Sits on one channel in promiscuous mode and reports CSI, optionally only for
 * frames from one transmitter MAC. Paired with the tx board this gives a CSI
 * time series where the transmitter, the channel, the PHY and the interval are
 * all held fixed, so anything that moves in the data moved in the room.
 *
 * Beyond the raw CSV it also prints a compact per-record line: the mean
 * amplitude over the usable sub-carriers, which is the single number most
 * worth watching while you wave a hand between the two boards.
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

static const char *TAG = "csi_rx";

/* An unticked Kconfig bool is undefined, not 0. Safe inside #if, a compile
 * error when assigned to a variable. */
#ifndef CONFIG_CSI_RX_FORCE_LLTF
#define CONFIG_CSI_RX_FORCE_LLTF 0
#endif

static uint8_t s_filter[6];
static bool    s_filtering;

/* Parses "aa:bb:cc:dd:ee:ff". Returns false for the all-zero address, which is
 * how the Kconfig default says "accept every transmitter". */
static bool parse_mac(const char *s, uint8_t out[6])
{
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    uint8_t acc = 0;
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t) v[i];
        acc |= out[i];
    }
    return acc != 0;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    const uint8_t channel = CONFIG_CSI_RX_CHANNEL;
    if (!csi_c5_channel_valid(channel)) {
        ESP_LOGE(TAG, "channel %u is not a channel this chip knows", channel);
        return;
    }
    const bool five = csi_c5_is_5ghz(channel);

    s_filtering = parse_mac(CONFIG_CSI_RX_FILTER_MAC, s_filter);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Band mode only works once Wi-Fi is started (it documents
     * ESP_ERR_WIFI_NOT_STARTED), and the protocol bitmap only once a single
     * band is selected. */
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(five ? WIFI_BAND_MODE_5G_ONLY
                                                : WIFI_BAND_MODE_2G_ONLY));

    /* Must include 11AX, or an HE20 frame from the transmitter is not decoded
     * and produces no CSI at all. */
    const uint8_t protocols = five
        ? (WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX)
        : (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N  | WIFI_PROTOCOL_11AX);
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, protocols));

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    csi_c5_config_t cfg = CSI_C5_CONFIG_DEFAULT();
    cfg.channel     = channel;
    cfg.promiscuous = true;
    /* The transmitter broadcasts, and broadcasts are never acknowledged, so
     * there are no ACKs to dump here. Leaving it on would only add CSI from
     * unrelated traffic. */
    cfg.dump_ack    = false;
    cfg.force_lltf  = CONFIG_CSI_RX_FORCE_LLTF;
    cfg.val_scale   = CONFIG_CSI_RX_VAL_SCALE;
    cfg.queue_len   = 24;

    ESP_ERROR_CHECK(csi_c5_start(&cfg));

    if (s_filtering) {
        ESP_LOGI(TAG, "only reporting CSI from %02x:%02x:%02x:%02x:%02x:%02x",
                 s_filter[0], s_filter[1], s_filter[2],
                 s_filter[3], s_filter[4], s_filter[5]);
    } else {
        ESP_LOGI(TAG, "no MAC filter - reporting CSI from every transmitter on channel %u",
                 channel);
    }

#if CONFIG_CSI_RX_PRINT_CSV
    csi_c5_print_csv_header();
#endif

    csi_c5_record_t rec;
    uint32_t matched = 0, skipped = 0;
    TickType_t next_summary = xTaskGetTickCount() + pdMS_TO_TICKS(5000);

    for (;;) {
        if (csi_c5_receive(&rec, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (s_filtering && memcmp(rec.src_mac, s_filter, 6) != 0) {
                skipped++;
            } else {
                matched++;

#if CONFIG_CSI_RX_PRINT_CSV
                csi_c5_print_csv(&rec);
#endif

                /* Mean amplitude over the usable sub-carriers. Skipping the
                 * first two when first_word_invalid is set matters here: those
                 * two carry junk, and junk in a mean is not visible, it just
                 * quietly biases every reading. */
                const int first = csi_c5_first_subcarrier(&rec);
                const int n     = csi_c5_subcarriers(&rec);
                float sum = 0.0f;
                for (int k = first; k < n; k++) {
                    sum += csi_c5_amplitude(&rec, k);
                }
                const float mean = (n > first) ? sum / (float) (n - first) : 0.0f;

                printf("AMP,%u,%s,%d,%d,%.2f\n",
                       (unsigned) rec.rx_seq,
                       csi_c5_bb_format_str(rec.rx_ctrl.cur_bb_format),
                       n - first,
                       (int) rec.rx_ctrl.rssi,
                       mean);
            }
        }

        if (xTaskGetTickCount() >= next_summary) {
            next_summary = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
            ESP_LOGI(TAG, "ch %u | %" PRIu32 " matched | %" PRIu32 " from others | %" PRIu32 " dropped",
                     channel, matched, skipped, csi_c5_dropped());
            if (matched == 0) {
                ESP_LOGW(TAG, "nothing from the transmitter yet. Both boards must be on "
                              "channel %u, and the receiver must have 11AX enabled if the "
                              "transmitter is pinned to HE20.", channel);
            }
        }
    }
}
