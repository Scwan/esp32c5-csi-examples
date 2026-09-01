/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * 01_sniffer - the smallest thing that produces CSI on an ESP32-C5.
 *
 * No AP, no second board, no association. The radio is parked on one channel in
 * promiscuous mode and reports CSI for every frame it can decode, on 2.4 GHz or
 * 5 GHz. Whether you see anything depends entirely on whether there is traffic
 * on that channel - see the "nothing appears" section of the README.
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

static const char *TAG = "sniffer";

/*
 * A Kconfig "bool" that is turned OFF is not defined as 0 - it is not defined at
 * all. That is fine inside #if, where an unknown identifier evaluates to 0, but
 * assigning one to a variable is a compile error the moment somebody unticks the
 * option. These fallbacks make the two that are used as values safe either way.
 */
#ifndef CONFIG_CSI_SNIFFER_DUMP_ACK
#define CONFIG_CSI_SNIFFER_DUMP_ACK 0
#endif
#ifndef CONFIG_CSI_SNIFFER_FORCE_LLTF
#define CONFIG_CSI_SNIFFER_FORCE_LLTF 0
#endif

/* Counters for the periodic summary line. Indexed by wifi_rx_bb_format_t, whose
 * largest value is RX_BB_FORMAT_VHT_MU == 11. */
#define BB_FORMAT_SLOTS 16
static uint32_t s_by_format[BB_FORMAT_SLOTS];
static uint32_t s_total;
static uint32_t s_invalid_ce;

static void wifi_bring_up(uint8_t channel)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    /* Nothing here should outlive a reboot, and writing the channel to NVS on
     * every run is pointless wear. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_wifi_start());

    /*
     * Everything below has to happen AFTER esp_wifi_start(), and in this order.
     * It is not the order you would guess, and getting it wrong fails at
     * runtime rather than at compile time:
     *
     *   esp_wifi_set_band_mode() documents ESP_ERR_WIFI_NOT_STARTED, so calling
     *   it before esp_wifi_start() - which reads like the natural place for a
     *   radio-wide setting - simply fails.
     *
     *   esp_wifi_set_protocol() must then come after the band mode, because it
     *   returns ESP_ERR_NOT_SUPPORTED under WIFI_BAND_MODE_AUTO and otherwise
     *   applies to whichever single band is currently selected.
     */
    const bool five = csi_c5_is_5ghz(channel);
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(five ? WIFI_BAND_MODE_5G_ONLY
                                                : WIFI_BAND_MODE_2G_ONLY));

    /*
     * Enable every PHY the chosen band supports, so the receiver decodes 11ax
     * frames and hands us HE-LTF CSI rather than only legacy CSI. These are
     * already the documented per-band defaults; they are spelled out because
     * this is the setting people trim by accident and then wonder why
     * bb_format never says 11ax.
     */
    const uint8_t protocols = five
        ? (WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX)
        : (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N  | WIFI_PROTOCOL_11AX);
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, protocols));

    /* Modem sleep gates the receiver off for part of the time and, per the
     * rx_ctrl documentation, makes the timestamp imprecise. Neither is
     * acceptable for CSI. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    const uint8_t channel = CONFIG_CSI_SNIFFER_CHANNEL;

    if (!csi_c5_channel_valid(channel)) {
        ESP_LOGE(TAG, "channel %u is not valid. 2.4 GHz: 1-14. "
                      "5 GHz: 36-64, 100-144, 149-177, all in steps of 4.", channel);
        return;
    }

    wifi_bring_up(channel);

    csi_c5_config_t cfg = CSI_C5_CONFIG_DEFAULT();
    cfg.channel     = channel;
    cfg.second      = WIFI_SECOND_CHAN_NONE;
    cfg.promiscuous = true;
    cfg.dump_ack    = CONFIG_CSI_SNIFFER_DUMP_ACK;
    cfg.force_lltf  = CONFIG_CSI_SNIFFER_FORCE_LLTF;
    cfg.val_scale   = CONFIG_CSI_SNIFFER_VAL_SCALE;
    cfg.queue_len   = CONFIG_CSI_SNIFFER_QUEUE_LEN;

    ESP_ERROR_CHECK(csi_c5_start(&cfg));

#if CONFIG_CSI_SNIFFER_PRINT_CSV
    csi_c5_print_csv_header();
#endif

    csi_c5_record_t rec;
    TickType_t next_summary = xTaskGetTickCount() + pdMS_TO_TICKS(5000);

    for (;;) {
        if (csi_c5_receive(&rec, pdMS_TO_TICKS(1000)) == pdTRUE) {
            s_total++;
            s_by_format[rec.rx_ctrl.cur_bb_format & (BB_FORMAT_SLOTS - 1)]++;

            /* The Wi-Fi guide is explicit: rx_channel_estimate_info_vld == 1
             * means the CSI data is valid, otherwise it is not. Counting the
             * bad ones is more useful than silently printing them. */
            if (!rec.rx_ctrl.rx_channel_estimate_info_vld) {
                s_invalid_ce++;
            }

#if CONFIG_CSI_SNIFFER_PRINT_CSV
            csi_c5_print_csv(&rec);
#endif
        }

        if (xTaskGetTickCount() >= next_summary) {
            next_summary = xTaskGetTickCount() + pdMS_TO_TICKS(5000);

            ESP_LOGI(TAG, "ch %u | %" PRIu32 " records | %" PRIu32 " dropped | %" PRIu32 " invalid CE",
                     channel, s_total, csi_c5_dropped(), s_invalid_ce);

            for (int f = 0; f < BB_FORMAT_SLOTS; f++) {
                if (s_by_format[f]) {
                    ESP_LOGI(TAG, "    %-10s %" PRIu32, csi_c5_bb_format_str(f), s_by_format[f]);
                }
            }

            if (s_total == 0) {
                ESP_LOGW(TAG, "no CSI yet - is anything transmitting on channel %u? "
                              "See the README section 'Nothing appears'.", channel);
            }
        }
    }
}
