/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * 02_espnow_pair / tx - the transmitter half.
 *
 * This board never looks at CSI. Its whole job is to put a frame on the air at
 * a known channel, a known PHY and a known cadence, so that the receiver board
 * gets a clean, regular CSI stream instead of whatever the neighbours happen to
 * be doing. That control is the entire point of the pair: with a fixed rate and
 * a fixed interval, every change you see in the CSI came from the channel.
 *
 * ESP-NOW is used because it needs no association, no DHCP and no AP.
 */

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "csi_tx";

static const uint8_t BROADCAST[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* Recognisable on the receiver side and in a sniffer trace. */
#define CSI_TX_MAGIC 0x43354353u   /* "C5CS" */

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint8_t  pad[CONFIG_CSI_TX_PAYLOAD_PAD];
} __attribute__((packed)) csi_tx_frame_t;

static uint32_t s_sent;
static uint32_t s_failed;

/*
 * Note the signature. In ESP-IDF 5.x this callback takes an
 * esp_now_send_info_t, not the bare MAC pointer that older ESP-NOW examples
 * (and a great deal of copied-around code) still use. Using the old form here
 * compiles with an incompatible-pointer warning and then misbehaves.
 */
static void send_cb(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void) info;
    if (status == ESP_NOW_SEND_SUCCESS) {
        s_sent++;
    } else {
        s_failed++;
    }
}

static bool is_5ghz(uint8_t channel)
{
    return channel >= 36;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    const uint8_t channel = CONFIG_CSI_TX_CHANNEL;
    const bool    five    = is_5ghz(channel);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_wifi_start());

    /*
     * Band mode first, and only after start: esp_wifi_set_band_mode() documents
     * ESP_ERR_WIFI_NOT_STARTED. Protocol bitmap second, because
     * esp_wifi_set_protocol() returns ESP_ERR_NOT_SUPPORTED under
     * WIFI_BAND_MODE_AUTO and is per band.
     */
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(five ? WIFI_BAND_MODE_5G_ONLY
                                                : WIFI_BAND_MODE_2G_ONLY));

    const uint8_t protocols = five
        ? (WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX)
        : (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N  | WIFI_PROTOCOL_11AX);
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, protocols));

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    /* On 5 GHz the hardware derives the second channel itself. */
    ESP_ERROR_CHECK(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    ESP_LOGI(TAG, "transmitting as %02x:%02x:%02x:%02x:%02x:%02x on channel %u (%s)",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             channel, five ? "5 GHz" : "2.4 GHz");
    ESP_LOGI(TAG, "set CONFIG_CSI_RX_FILTER_MAC on the receiver to this address "
                  "to ignore every other transmitter");

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));

    esp_now_peer_info_t peer = {
        .channel = channel,      /* 0 would mean "whatever channel we are on"; being
                                  * explicit makes a channel mismatch an error rather
                                  * than a silent move */
        .ifidx   = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, BROADCAST, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    /*
     * Pin the PHY. Without this the rate adapts, the PPDU format changes under
     * you, and the receiver sees CSI records that change length from one frame
     * to the next - 490 bytes for an HE20 frame, 114 for HT20, 106 for legacy.
     * Comparing amplitudes across records of different shapes is meaningless,
     * so fixing the rate here is what makes the receiver's output usable.
     *
     * MCS0 is the most robust rate; raise it only if you need the airtime.
     */
#if CONFIG_CSI_TX_PHY_HE20
    esp_now_rate_config_t rate = {
        .phymode = WIFI_PHY_MODE_HE20,
        .rate    = WIFI_PHY_RATE_MCS0_LGI,
        .ersu    = false,
        .dcm     = false,
    };
    const char *phy_name = "HE20 MCS0 (11ax, 245 sub-carriers, 490 byte records)";
#else
    esp_now_rate_config_t rate = {
        .phymode = five ? WIFI_PHY_MODE_11A : WIFI_PHY_MODE_11G,
        .rate    = WIFI_PHY_RATE_6M,
        .ersu    = false,
        .dcm     = false,
    };
    const char *phy_name = "legacy 6 Mbps (L-LTF, 52 sub-carriers, 106 byte records)";
#endif

    err = esp_now_set_peer_rate_config(BROADCAST, &rate);
    if (err != ESP_OK) {
        /* Not fatal: the frames still go out, just at a rate the driver picks.
         * Worth shouting about, because it silently changes what the receiver
         * measures. */
        ESP_LOGW(TAG, "esp_now_set_peer_rate_config: %s - rate will adapt and CSI "
                      "record length will vary between frames", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "PHY pinned to %s", phy_name);
    }

    csi_tx_frame_t frame = { .magic = CSI_TX_MAGIC, .seq = 0 };
    memset(frame.pad, 0xA5, sizeof(frame.pad));

    const TickType_t period = pdMS_TO_TICKS(CONFIG_CSI_TX_INTERVAL_MS);
    TickType_t last = xTaskGetTickCount();
    TickType_t next_log = last + pdMS_TO_TICKS(5000);

    for (;;) {
        err = esp_now_send(BROADCAST, (const uint8_t *) &frame, sizeof(frame));
        if (err != ESP_OK) {
            /* ESP_ERR_ESPNOW_CHAN means the peer channel and the radio channel
             * disagree - on a dual-band chip that usually means the band mode
             * does not match the channel number. */
            ESP_LOGE(TAG, "esp_now_send: %s", esp_err_to_name(err));
        }
        frame.seq++;

        if (xTaskGetTickCount() >= next_log) {
            next_log += pdMS_TO_TICKS(5000);
            /* Broadcast ESP-NOW frames are not acknowledged at the 802.11
             * level, so "ok" here means the frame reached the air, not that
             * anybody heard it. The receiver board is the only real evidence. */
            ESP_LOGI(TAG, "seq %" PRIu32 " | %" PRIu32 " ok | %" PRIu32 " failed",
                     frame.seq, s_sent, s_failed);
        }

        /* vTaskDelayUntil keeps the cadence steady even when a send blocks,
         * which matters: an irregular transmit interval shows up directly as
         * jitter in the receiver's CSI time series. */
        vTaskDelayUntil(&last, period);
    }
}
