/*
 * SPDX-FileCopyrightText: 2026 scwan
 * SPDX-License-Identifier: Apache-2.0
 *
 * 04_motion - doing something with CSI rather than just printing it.
 *
 * Same self-sustaining CSI stream as 03 (associate with an AP, ping the
 * gateway, take CSI from the echo replies), plus a small amplitude-variance
 * detector on top.
 *
 * What it actually does: learns a per-sub-carrier amplitude baseline while the
 * room is still, then reports how far each new record deviates from it. A
 * person moving through the link changes multipath, which changes those
 * amplitudes, which shows up as a rise in the score.
 *
 * What it is not: this is a threshold on a change statistic, not presence
 * detection and not a trained model. It will also fire on the AP changing rate
 * or bandwidth, on a microwave oven, and on someone moving in the flat
 * upstairs. Treat the score as "the channel changed", and everything else as
 * your interpretation.
 */

#include <inttypes.h>
#include <math.h>
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

static const char *TAG = "csi_motion";

/* An unticked Kconfig bool is undefined, not 0. Safe inside #if, a compile
 * error when assigned to a variable. */
#ifndef CONFIG_CSI_MOTION_FORCE_LLTF
#define CONFIG_CSI_MOTION_FORCE_LLTF 0
#endif

#define GOT_IP_BIT BIT0

static EventGroupHandle_t s_events;
static esp_netif_t       *s_netif;

/* ---------------------------------------------------------------- *
 * Detector state
 * ---------------------------------------------------------------- */

/* Baseline amplitude per sub-carrier, learned while the room is still. */
static float s_baseline[CSI_C5_MAX_SUBCARRIERS];
static int   s_baseline_n;          /* sub-carriers the baseline was built for */
static int   s_first_sc;            /* first usable sub-carrier index */
static int   s_calibrated;          /* records folded into the baseline so far */

static float s_score;               /* smoothed deviation */
static bool  s_motion;
static uint32_t s_rejected_shape;

/*
 * How fast the baseline follows the room. Slow, so a person walking through
 * does not get absorbed into "normal" within a second, but not frozen, so
 * furniture being moved once does not leave the detector stuck on forever.
 */
#define BASELINE_ALPHA   0.004f

/* How fast the reported score follows the raw per-record deviation. Higher
 * reacts sooner and rattles more. */
#define SCORE_ALPHA      0.25f

/* ---------------------------------------------------------------- *
 * Wi-Fi plumbing (same shape as 03_router)
 * ---------------------------------------------------------------- */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg; (void) base;
    /* Not connecting on WIFI_EVENT_STA_START: that fires from inside
     * esp_wifi_start(), before the band mode can legally be set, so it would
     * race the band selection. app_main() connects explicitly. */
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *) data;
        xEventGroupClearBits(s_events, GOT_IP_BIT);
        ESP_LOGW(TAG, "disconnected, reason %d", d->reason);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg; (void) base;
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *) data;
        ESP_LOGI(TAG, "got " IPSTR ", gateway " IPSTR,
                 IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw));
        xEventGroupSetBits(s_events, GOT_IP_BIT);
    }
}

static esp_ping_handle_t s_ping;

/* Each echo reply from the gateway is a downlink frame addressed to this
 * board, and so one CSI record. See 03_router for why ping rather than
 * anything else. */
static void start_pinging(void)
{
    esp_netif_ip_info_t ip;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(s_netif, &ip));

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count       = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = CONFIG_CSI_MOTION_INTERVAL_MS;
    cfg.timeout_ms  = 500;
    cfg.data_size   = 32;
    cfg.task_prio   = 4;

    ip_addr_t target;
    memset(&target, 0, sizeof(target));
    ip_addr_set_ip4_u32(&target, ip.gw.addr);
    cfg.target_addr = target;

    ESP_ERROR_CHECK(esp_ping_new_session(&cfg, NULL, &s_ping));
    ESP_ERROR_CHECK(esp_ping_start(s_ping));

    ESP_LOGI(TAG, "pinging " IPSTR " every %d ms",
             IP2STR(&ip.gw), CONFIG_CSI_MOTION_INTERVAL_MS);
}

/* ---------------------------------------------------------------- *
 * Detector
 * ---------------------------------------------------------------- */

/*
 * Returns the mean relative deviation of this record from the baseline, or a
 * negative value if the record could not be compared.
 *
 * The +1.0f in the denominator is not cosmetic. Some sub-carriers - the DC bin
 * and the guard bins either side of it - carry essentially no energy, so their
 * baseline sits near zero and a raw relative deviation there explodes. Adding
 * one unit of amplitude floors that ratio without materially affecting
 * sub-carriers that do carry signal.
 */
static float deviation_from_baseline(const csi_c5_record_t *rec)
{
    const int first = csi_c5_first_subcarrier(rec);
    const int n     = csi_c5_subcarriers(rec);

    if (n <= first) {
        return -1.0f;
    }

    /*
     * Only compare like with like. If the transmitter's PHY changed, the record
     * has a different number of sub-carriers and they mean different
     * frequencies - comparing them element-wise produces a large deviation that
     * has nothing to do with the room. A rare wrong alarm is worse than a
     * common one, because nobody ever tracks it down.
     *
     * How often this fires depends on how stable the AP's rate control is. The
     * summary line reports the count as "wrong-shape"; if it is high, pin the
     * link rather than reaching for acquire_csi_force_lltf, which on the C5
     * also switches the buffer to 12-bit packing.
     */
    if (s_baseline_n != 0 && (n != s_baseline_n || first != s_first_sc)) {
        s_rejected_shape++;
        return -1.0f;
    }

    if (s_baseline_n == 0) {
        s_baseline_n = n;
        s_first_sc   = first;
        for (int k = first; k < n; k++) {
            s_baseline[k] = csi_c5_amplitude(rec, k);
        }
        s_calibrated = 1;
        return -1.0f;
    }

    float sum = 0.0f;
    for (int k = first; k < n; k++) {
        const float a = csi_c5_amplitude(rec, k);
        sum += fabsf(a - s_baseline[k]) / (s_baseline[k] + 1.0f);
    }
    const float dev = sum / (float) (n - first);

    /* Fold this record into the baseline. During calibration the weight is
     * 1/count, which is a plain running mean and converges fast; afterwards it
     * is the slow EWMA that lets the baseline drift with the room. */
    const bool calibrating = s_calibrated < CONFIG_CSI_MOTION_CALIBRATION_RECORDS;
    if (calibrating) {
        s_calibrated++;
        const float w = 1.0f / (float) s_calibrated;
        for (int k = first; k < n; k++) {
            s_baseline[k] += w * (csi_c5_amplitude(rec, k) - s_baseline[k]);
        }
    } else {
        for (int k = first; k < n; k++) {
            s_baseline[k] += BASELINE_ALPHA * (csi_c5_amplitude(rec, k) - s_baseline[k]);
        }
    }

    return dev;
}

/* ---------------------------------------------------------------- *
 * main
 * ---------------------------------------------------------------- */

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
    strlcpy((char *) sta.sta.ssid,     CONFIG_CSI_MOTION_SSID,     sizeof(sta.sta.ssid));
    strlcpy((char *) sta.sta.password, CONFIG_CSI_MOTION_PASSWORD, sizeof(sta.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* After start, not before: esp_wifi_set_band_mode() documents
     * ESP_ERR_WIFI_NOT_STARTED. */
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "connecting to \"%s\"...", CONFIG_CSI_MOTION_SSID);
    ESP_ERROR_CHECK(esp_wifi_connect());
    xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    csi_c5_config_t cfg = CSI_C5_CONFIG_DEFAULT();
    cfg.channel     = 0;      /* associated: do not move the radio */
    cfg.promiscuous = false;
    cfg.dump_ack    = true;
    /*
     * Forced L-LTF by default. Every record then has the same 52 sub-carriers
     * whatever PHY the AP used, which is what makes an element-wise comparison
     * against a baseline meaningful at all. Turn it off only if you are
     * prepared to bucket records by shape yourself.
     */
    cfg.force_lltf  = CONFIG_CSI_MOTION_FORCE_LLTF;
    cfg.val_scale   = CONFIG_CSI_MOTION_VAL_SCALE;
    cfg.queue_len   = 16;

    ESP_ERROR_CHECK(csi_c5_start(&cfg));

    start_pinging();

    const float enter = (float) CONFIG_CSI_MOTION_THRESHOLD_PERCENT / 100.0f;
    const float leave = enter * 0.6f;    /* hysteresis, so the state does not chatter */

    ESP_LOGI(TAG, "calibrating over %d records - keep still and out of the link",
             CONFIG_CSI_MOTION_CALIBRATION_RECORDS);

    csi_c5_record_t rec;
    uint32_t   seen = 0;
    TickType_t motion_until = 0;
    TickType_t next_summary = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
    bool       announced_ready = false;

    for (;;) {
        if (csi_c5_receive(&rec, pdMS_TO_TICKS(500)) == pdTRUE) {
            seen++;
            const float dev = deviation_from_baseline(&rec);

            if (dev >= 0.0f) {
                s_score += SCORE_ALPHA * (dev - s_score);
            }

            const bool calibrating = s_calibrated < CONFIG_CSI_MOTION_CALIBRATION_RECORDS;

            if (!calibrating) {
                if (!announced_ready) {
                    announced_ready = true;
                    ESP_LOGI(TAG, "baseline ready over %d sub-carriers - watching",
                             s_baseline_n - s_first_sc);
                }

                if (s_score > enter) {
                    /* Hold the state briefly so a person crossing the link
                     * reads as one event rather than a burst of them. */
                    motion_until = xTaskGetTickCount() +
                                   pdMS_TO_TICKS(CONFIG_CSI_MOTION_HOLD_MS);
                    if (!s_motion) {
                        s_motion = true;
                        ESP_LOGI(TAG, "motion  (score %.3f)", s_score);
                    }
                } else if (s_motion && s_score < leave &&
                           xTaskGetTickCount() > motion_until) {
                    s_motion = false;
                    ESP_LOGI(TAG, "still   (score %.3f)", s_score);
                }

                printf("MOTION,%" PRIu32 ",%.4f,%d,%d,%s\n",
                       seen, s_score, s_motion ? 1 : 0,
                       (int) rec.rx_ctrl.rssi,
                       csi_c5_bb_format_str(rec.rx_ctrl.cur_bb_format));
            }
        }

        if (xTaskGetTickCount() >= next_summary) {
            next_summary = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
            if (s_calibrated < CONFIG_CSI_MOTION_CALIBRATION_RECORDS) {
                ESP_LOGI(TAG, "calibrating %d/%d", s_calibrated,
                         CONFIG_CSI_MOTION_CALIBRATION_RECORDS);
            } else {
                ESP_LOGI(TAG, "%" PRIu32 " records | score %.3f | %s | "
                              "%" PRIu32 " wrong-shape | %" PRIu32 " dropped",
                         seen, s_score, s_motion ? "MOTION" : "still",
                         s_rejected_shape, csi_c5_dropped());
            }
        }
    }
}
