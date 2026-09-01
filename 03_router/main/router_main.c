/*
 * SPDX-FileCopyrightText: 2026 scwan
 * SPDX-License-Identifier: Apache-2.0
 *
 * 03_router - CSI from an ordinary home access point, with one board.
 *
 * The sniffer in 01 depends on somebody else happening to transmit. This
 * example creates its own traffic instead: it associates with your AP and pings
 * the gateway at a fixed interval. Every echo reply is a downlink 802.11 data
 * frame addressed to this board, and every one of those yields a CSI record.
 * The result is a steady, self-sustaining CSI stream from a link you already
 * have, with no second ESP board involved.
 *
 * Ping rather than, say, UDP-to-a-dead-port because it is the one stimulus that
 * *guarantees* a frame comes back. It is also the topology Espressif documents
 * first: "ESP32 sends a Ping packet to the router, and receives the CSI
 * information carried in the Ping Replay returned by the router".
 *
 * If the AP is dual-band and you connect on 5 GHz, this is the C5 measuring a
 * 5 GHz link - which no other Espressif chip can do today.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "ping/ping_sock.h"

#include "csi_c5.h"

static const char *TAG = "csi_router";

/* An unticked Kconfig bool is undefined, not 0. Safe inside #if, a compile
 * error when assigned to a variable. */
#ifndef CONFIG_CSI_ROUTER_FORCE_LLTF
#define CONFIG_CSI_ROUTER_FORCE_LLTF 0
#endif

#define CONNECTED_BIT BIT0
#define GOT_IP_BIT    BIT1

static EventGroupHandle_t s_events;
static esp_netif_t       *s_netif;
static int                s_retries;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;

    /*
     * Deliberately NOT connecting on WIFI_EVENT_STA_START. That event fires
     * from inside esp_wifi_start(), and the band mode can only be set after
     * start - so auto-connecting there would race the band selection and
     * sometimes scan the wrong band. app_main() connects explicitly instead,
     * once the band is settled.
     */
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *) data;
        xEventGroupClearBits(s_events, CONNECTED_BIT | GOT_IP_BIT);
        ESP_LOGW(TAG, "disconnected, reason %d - retry %d", d->reason, ++s_retries);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;

    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *) data;
        ESP_LOGI(TAG, "got " IPSTR ", gateway " IPSTR,
                 IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw));
        xEventGroupSetBits(s_events, GOT_IP_BIT);
    }
}

static esp_ping_handle_t s_ping;
static uint32_t          s_replies;
static uint32_t          s_timeouts;

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    (void) hdl;
    (void) args;
    s_replies++;
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    (void) hdl;
    (void) args;
    s_timeouts++;
}

/*
 * Keeps the link busy. Each echo reply from the gateway is a downlink 802.11
 * data frame addressed to this station, which is what produces the CSI - no
 * dependence on promiscuous mode and none on dump_ack_en.
 */
static void start_pinging(void)
{
    esp_netif_ip_info_t ip;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(s_netif, &ip));

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count       = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = CONFIG_CSI_ROUTER_INTERVAL_MS;
    cfg.timeout_ms  = 500;
    cfg.data_size   = 32;
    cfg.task_prio   = 4;

    /*
     * Note the _val suffix. The pointer form, ip_addr_set_ip4_u32(&target, ..),
     * expands to a null check on its argument, and ESP-IDF builds with
     * -Werror=address, so testing the address of a local is a hard error:
     *
     *   error: the address of 'target' will always evaluate as 'true'
     *
     * lwip provides the _val form for exactly this case. It takes the object
     * rather than a pointer, has no null check in either the IPv4-only or the
     * dual-stack build, and still stamps the address type where that matters.
     */
    ip_addr_t target;
    memset(&target, 0, sizeof(target));
    ip_addr_set_ip4_u32_val(target, ip.gw.addr);
    cfg.target_addr = target;

    const esp_ping_callbacks_t cbs = {
        .cb_args         = NULL,
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
        .on_ping_end     = NULL,
    };

    ESP_ERROR_CHECK(esp_ping_new_session(&cfg, &cbs, &s_ping));
    ESP_ERROR_CHECK(esp_ping_start(s_ping));

    ESP_LOGI(TAG, "pinging " IPSTR " every %d ms - each reply is one downlink frame",
             IP2STR(&ip.gw), CONFIG_CSI_ROUTER_INTERVAL_MS);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_ip_event, NULL, NULL));

    wifi_config_t sta = { 0 };
    strlcpy((char *) sta.sta.ssid,     CONFIG_CSI_ROUTER_SSID,     sizeof(sta.sta.ssid));
    strlcpy((char *) sta.sta.password, CONFIG_CSI_ROUTER_PASSWORD, sizeof(sta.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());

    /*
     * Band mode decides which band the scan even looks at, and it has to be set
     * after esp_wifi_start() - the API documents ESP_ERR_WIFI_NOT_STARTED.
     *
     * WIFI_BAND_MODE_AUTO lets the C5 find the AP on either band, which is fine
     * here: unlike the sniffer examples, nothing in this one calls
     * esp_wifi_set_protocol() or esp_wifi_set_bandwidth(), the two APIs that
     * return ESP_ERR_NOT_SUPPORTED under AUTO.
     */
#if CONFIG_CSI_ROUTER_BAND_5G_ONLY
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY));
    ESP_LOGI(TAG, "restricted to 5 GHz");
#elif CONFIG_CSI_ROUTER_BAND_2G_ONLY
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY));
    ESP_LOGI(TAG, "restricted to 2.4 GHz");
#else
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO));
    ESP_LOGI(TAG, "either band");
#endif

    /* Power save is the difference between a CSI record per echo reply and a
     * CSI record whenever the radio happens to be awake. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "connecting to \"%s\"...", CONFIG_CSI_ROUTER_SSID);
    ESP_ERROR_CHECK(esp_wifi_connect());
    xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    uint8_t            primary = 0;
    wifi_second_chan_t second  = WIFI_SECOND_CHAN_NONE;
    ESP_ERROR_CHECK(esp_wifi_get_channel(&primary, &second));
    ESP_LOGI(TAG, "associated on channel %u (%s)",
             primary, csi_c5_is_5ghz(primary) ? "5 GHz" : "2.4 GHz");

    csi_c5_config_t cfg = CSI_C5_CONFIG_DEFAULT();
    cfg.channel     = 0;      /* associated: never move the radio, see csi_c5.h */
    cfg.promiscuous = false;  /* the ping replies are addressed to us already */
    /*
     * Left on as a bonus source, not as the mechanism. Every ping we send is
     * also ACKed by the AP, and dump_ack_en asks for CSI from those ACKs too -
     * but that field is documented by one header comment and nothing else, and
     * Espressif's own CSI examples set it false. The echo replies are what this
     * example actually relies on. Set it false if you want only those.
     */
    cfg.dump_ack    = true;
    cfg.force_lltf  = CONFIG_CSI_ROUTER_FORCE_LLTF;
    cfg.val_scale   = CONFIG_CSI_ROUTER_VAL_SCALE;
    cfg.queue_len   = 16;

    ESP_ERROR_CHECK(csi_c5_start(&cfg));

    start_pinging();

#if CONFIG_CSI_ROUTER_PRINT_CSV
    csi_c5_print_csv_header();
#endif

    csi_c5_record_t rec;
    uint32_t total = 0, from_ap = 0;
    uint8_t  bssid[6] = { 0 };

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        memcpy(bssid, ap.bssid, 6);
        ESP_LOGI(TAG, "AP BSSID %02x:%02x:%02x:%02x:%02x:%02x, RSSI %d",
                 bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], ap.rssi);
    }

    TickType_t next_summary = xTaskGetTickCount() + pdMS_TO_TICKS(5000);

    for (;;) {
        if (csi_c5_receive(&rec, pdMS_TO_TICKS(500)) == pdTRUE) {
            total++;
            if (memcmp(rec.src_mac, bssid, 6) == 0) {
                from_ap++;
            }
#if CONFIG_CSI_ROUTER_PRINT_CSV
            csi_c5_print_csv(&rec);
#endif
        }

        if (xTaskGetTickCount() >= next_summary) {
            next_summary = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
            ESP_LOGI(TAG, "%" PRIu32 " records (%" PRIu32 " from the AP) | "
                          "ping %" PRIu32 " replies, %" PRIu32 " timeouts | %" PRIu32 " dropped",
                     total, from_ap, s_replies, s_timeouts, csi_c5_dropped());
            if (total == 0 && s_replies > 0) {
                ESP_LOGW(TAG, "the gateway is replying but no CSI is arriving - check that "
                              "CONFIG_ESP_WIFI_CSI_ENABLED=y actually took effect; a stale "
                              "sdkconfig is the usual culprit.");
            } else if (s_replies == 0 && s_timeouts > 0) {
                ESP_LOGW(TAG, "the gateway is not answering pings. Some routers drop ICMP - "
                              "point CSI_ROUTER_* at a host that answers, or rely on the AP's "
                              "ACKs alone.");
            }
        }
    }
}
