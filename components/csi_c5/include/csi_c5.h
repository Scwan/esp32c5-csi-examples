/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * csi_c5 - shared helpers for the ESP32-C5 Wi-Fi CSI examples.
 *
 * Everything here is written against the ESP32-C5 specifically. The C5 is an
 * HE-class chip with SOC_WIFI_MAC_VERSION_NUM == 3, which gives it a CSI
 * acquisition struct and an rx_ctrl struct that differ from BOTH the classic
 * ESP32/S3/C3 family AND the ESP32-C6. See NOTES-C5-vs-C6.md at the repo root.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ *
 * Build-time target guards.
 *
 * These are not decoration. Each one corresponds to a way this code misbehaves
 * quietly, rather than failing loudly, if its assumption is broken.
 * ------------------------------------------------------------------------ */

#if !defined(CONFIG_SOC_WIFI_CSI_SUPPORT)
#error "csi_c5: this target reports no Wi-Fi CSI support (SOC_WIFI_CSI_SUPPORT unset). ESP32-C5 sets it to y."
#endif

/*
 * Note what this guard is for. The five CSI functions are declared
 * unconditionally in esp_wifi.h, so code using them compiles and links whether
 * or not CSI is enabled in Kconfig. What the option actually controls is
 * WIFI_CSI_ENABLED, which WIFI_INIT_CONFIG_DEFAULT() assigns to csi_enable - so
 * with it off you get a working build that delivers zero callbacks and reports
 * no error at all. This turns that into a compile failure instead.
 */
#if !defined(CONFIG_ESP_WIFI_CSI_ENABLED)
#error "csi_c5: CSI is disabled in this project's configuration. Put CONFIG_ESP_WIFI_CSI_ENABLED=y in sdkconfig.defaults (menuconfig: Component config -> Wi-Fi -> WiFi CSI(Channel State Information)). Without it WIFI_INIT_CONFIG_DEFAULT() leaves csi_enable at 0 and no callback ever fires - silently, which is why this is an error rather than a warning."
#endif

#if !defined(CONFIG_SOC_WIFI_HE_SUPPORT)
#error "csi_c5: expects an HE-class chip, where wifi_csi_config_t is a typedef of wifi_csi_acquire_config_t. On non-HE chips wifi_csi_config_t has entirely different members (lltf_en/htltf_en/...) and this code will not compile."
#endif

#if CONFIG_SOC_WIFI_MAC_VERSION_NUM != 3
#warning "csi_c5: written for SOC_WIFI_MAC_VERSION_NUM == 3 (ESP32-C5). The ESP32-C6 is version 2: it lacks acquire_csi_force_lltf and acquire_csi_vht, spells the STBC field acquire_csi_he_stbc rather than acquire_csi_he_stbc_mode, and caps val_scale_cfg at 3 instead of 8. Expect compile errors below."
#endif

/* ------------------------------------------------------------------------ *
 * Sizes
 * ------------------------------------------------------------------------ */

/*
 * Largest CSI record the C5 produces, from the ESP-IDF Wi-Fi guide's C5 table:
 * HE20 SU is 245 sub-carriers (-122..-1, 0..122) at 2 bytes each = 490 bytes.
 * HT40 STBC (468), HT20 STBC (228) and non-HT (106) are all smaller. Rounded up
 * to a multiple of four because the driver aligns the record length.
 */
#define CSI_C5_MAX_CSI_BYTES        (492)
#define CSI_C5_MAX_SUBCARRIERS      (CSI_C5_MAX_CSI_BYTES / 2)

/* ------------------------------------------------------------------------ *
 * One captured CSI record, detached from the driver's buffer.
 *
 * esp_wifi.h is explicit that wifi_csi_info_t::buf "will be deallocated after
 * callback function returns", so anything outliving the callback must be a
 * copy. This struct is that copy.
 * ------------------------------------------------------------------------ */
/*
 * How the bytes in buf are packed. This is NOT constant on the C5, and getting
 * it wrong is the single most likely way to end up with plausible-looking
 * nonsense - see the long comment above the accessors below.
 */
typedef enum {
    CSI_C5_PACK_IQ8,      /* 2 bytes per tone: int8 imaginary, then int8 real */
    CSI_C5_PACK_LLTF12,   /* 4 bytes per tone: two 12-bit LE words, real then imaginary */
} csi_c5_packing_t;

typedef struct {
    wifi_pkt_rx_ctrl_t rx_ctrl;                 /* on the C5 this is esp_wifi_rxctrl_t */
    uint8_t            src_mac[6];
    uint8_t            dst_mac[6];
    uint16_t           rx_seq;
    bool               first_word_invalid;      /* the first 4 bytes of buf are junk */
    csi_c5_packing_t   packing;
    uint16_t           len;                     /* valid bytes in buf */
    int8_t             buf[CSI_C5_MAX_CSI_BYTES];
} csi_c5_record_t;

/* ------------------------------------------------------------------------ *
 * Configuration
 * ------------------------------------------------------------------------ */
typedef struct {
    /*
     * 1..14 (2.4 GHz) or 36..177 step 4 (5 GHz).
     *
     * 0 means "do not touch the channel". Use it whenever the station is
     * associated: the channel is then the AP's, and calling
     * esp_wifi_set_channel() would move the radio off it and drop the
     * connection.
     */
    uint8_t            channel;
    wifi_second_chan_t second;       /* ignored on 5 GHz, see csi_c5_park() */
    bool               promiscuous;  /* also capture frames not addressed to us */
    /*
     * dump_ack_en. The header says "enable to dump 802.11 ACK frame"; that is
     * the entirety of what any Espressif source says about it - the string
     * does not appear in the ESP-IDF documentation at all, and both of
     * Espressif's own CSI reference examples set it false. Treat it as a knob
     * to try, not as a mechanism to rely on.
     */
    bool               dump_ack;

    /*
     * acquire_csi_force_lltf. This selects which LTF you get from an HT/VHT/HE
     * frame, and on the C5 it also changes the BUFFER FORMAT:
     *
     *   false -> HT/VHT/HE-LTF, 8-bit int8 pairs, len 114/228/234/468/490
     *   true  -> L-LTF, which defaults to 12-bit packing, len 106, 26 tones
     *
     * The accessors below handle both, but only because csi_c5_start() records
     * which one is in force. Espressif recommends false for human-sensing work.
     */
    bool               force_lltf;
    /*
     * val_scale_cfg. ESP-IDF documents exactly one thing about this field: the
     * range, which is 0-8 on the C5 (the C6 field is two bits and stops at 3).
     * It scales the reported I/Q values. Anything beyond that - including
     * whether 0 means "automatic" - is not stated anywhere in the headers or
     * the Wi-Fi guide, so treat it empirically: leave it at 0, and if your
     * amplitudes clip at +/-127 or sit down in the noise, sweep it.
     */
    uint8_t            val_scale;
    uint8_t            queue_len;    /* records buffered between the Wi-Fi task and yours */
} csi_c5_config_t;

#define CSI_C5_CONFIG_DEFAULT() (csi_c5_config_t) {  \
    .channel     = 1,                                \
    .second      = WIFI_SECOND_CHAN_NONE,            \
    .promiscuous = true,                             \
    .dump_ack    = true,                             \
    .force_lltf  = false,                            \
    .val_scale   = 0,                                \
    .queue_len   = 16,                               \
}

/* ------------------------------------------------------------------------ *
 * Lifecycle
 *
 * Call order matters and the driver enforces part of it: esp_wifi_set_csi_config()
 * and esp_wifi_set_csi() both return ESP_ERR_WIFI_NOT_STARTED unless Wi-Fi has
 * been started or promiscuous mode is on. csi_c5_start() must therefore be
 * called after esp_wifi_start().
 * ------------------------------------------------------------------------ */

/* Registers the callback, allocates the queue, parks the radio, enables CSI. */
esp_err_t csi_c5_start(const csi_c5_config_t *cfg);

/* Disables CSI and frees the queue. */
void csi_c5_stop(void);

/* Blocks up to ticks_to_wait for the next record. pdTRUE if one was written. */
BaseType_t csi_c5_receive(csi_c5_record_t *out, TickType_t ticks_to_wait);

/* Count of records the driver produced that did not fit in the queue. */
uint32_t csi_c5_dropped(void);

/* ------------------------------------------------------------------------ *
 * Radio helpers
 * ------------------------------------------------------------------------ */

/* True for a 5 GHz channel number. */
bool csi_c5_is_5ghz(uint8_t channel);

/*
 * True if the C5 recognises this channel number at all. The set is taken from
 * CHANNEL_TO_BIT_NUMBER() in esp_wifi_types_generic.h:
 * 1..14, 36..64 step 4, 100..144 step 4, 149..177 step 4.
 */
bool csi_c5_channel_valid(uint8_t channel);

/*
 * Park the radio on one channel, switching band if needed.
 *
 * esp_wifi.h says of esp_wifi_set_band(): "It is recommended not to use this
 * API. If you want to change the current band, you can use esp_wifi_set_channel
 * instead." So this sets the band *mode* (which gates which channels are legal)
 * and then sets the channel, letting the band follow.
 *
 * On 5 GHz the second channel is picked by hardware from the primary per the
 * 802.11 standard, and esp_wifi_set_channel() documents that "any manually
 * configured second channel will be ignored". It is forced to
 * WIFI_SECOND_CHAN_NONE here so the logs do not claim otherwise.
 */
esp_err_t csi_c5_park(uint8_t channel, wifi_second_chan_t second);

/* Human-readable name for rx_ctrl.cur_bb_format (a wifi_rx_bb_format_t). */
const char *csi_c5_bb_format_str(uint32_t cur_bb_format);

/* ------------------------------------------------------------------------ *
 * Sub-carrier access
 *
 * READ THIS BEFORE TOUCHING buf DIRECTLY. The ESP-IDF documentation is WRONG
 * for one of the two cases below, and has been since the C5 shipped.
 *
 * What the Wi-Fi guide says, in the esp32c5 section: "Each channel frequency
 * response of sub-carrier is recorded by two bytes of signed characters. The
 * first one is imaginary part and the second one is real part."
 *
 * That is true for HT-LTF, VHT-LTF and HE-LTF. It is NOT true for L-LTF on this
 * chip. esp_wifi_he_types.h carries a field the docs never mention:
 *
 *     uint32_t lltf_bit_mode : 1;   0 : 12-bit, 1 : 8-bit, default : 12-bit
 *
 * So L-LTF defaults to 12-bit I/Q, and in that mode four int8 elements make one
 * complex value: buf[i] and buf[i+1] are a little-endian 16-bit word whose bits
 * [11:0] are the signed real part, and buf[i+2]/buf[i+3] the imaginary part.
 * A 106-byte L-LTF record is therefore 26 tones, not 53.
 *
 * Reading that as int8 pairs does not crash and does not look wrong: Espressif
 * closed esp-idf issue #18493 ("CSI IQ buffer contains static data on ESP32-C5
 * 5GHz channel") as exactly this mistake, and the reporter of the still-open
 * #18982 puts it best - "the values parse cleanly and plot plausibly". The odd
 * bytes just quietly only ever take four distinct values.
 *
 * These accessors handle both. Use them rather than indexing buf yourself.
 * ------------------------------------------------------------------------ */

/* One 12-bit little-endian component at component index c, sign-extended.
 * Same expression Espressif's own decoder uses. */
static inline int16_t csi_c5_w12(const int8_t *buf, int c)
{
    const uint16_t raw = (uint16_t) (((uint16_t) (uint8_t) buf[2 * c + 1] << 8) |
                                      (uint16_t) (uint8_t) buf[2 * c]);
    return (int16_t) ((int16_t) (raw << 4) >> 4);
}

/* Number of complex tones in this record. */
static inline int csi_c5_subcarriers(const csi_c5_record_t *r)
{
    if (r->packing == CSI_C5_PACK_LLTF12) {
        /* (len - 2) / 2 components, two components per tone. The trailing two
         * bytes are padding and are dropped, as in Espressif's decoder. */
        return (r->len >= 2) ? (int) ((r->len - 2) / 4) : 0;
    }
    return r->len / 2;
}

/* First tone index that is safe to read: first_word_invalid means the first
 * four BYTES are junk, which is two tones at 8-bit and one tone at 12-bit. */
static inline int csi_c5_first_subcarrier(const csi_c5_record_t *r)
{
    if (!r->first_word_invalid) {
        return 0;
    }
    return (r->packing == CSI_C5_PACK_LLTF12) ? 1 : 2;
}

static inline float csi_c5_real(const csi_c5_record_t *r, int k)
{
    if (r->packing == CSI_C5_PACK_LLTF12) {
        return (float) csi_c5_w12(r->buf, 2 * k);
    }
    return (float) r->buf[2 * k + 1];
}

static inline float csi_c5_imag(const csi_c5_record_t *r, int k)
{
    if (r->packing == CSI_C5_PACK_LLTF12) {
        return (float) csi_c5_w12(r->buf, 2 * k + 1);
    }
    return (float) r->buf[2 * k];
}

static inline float csi_c5_amplitude(const csi_c5_record_t *r, int k)
{
    const float im = csi_c5_imag(r, k);
    const float re = csi_c5_real(r, k);
    return sqrtf(im * im + re * re);
}

static inline float csi_c5_phase(const csi_c5_record_t *r, int k)
{
    return atan2f(csi_c5_imag(r, k), csi_c5_real(r, k));
}

static inline const char *csi_c5_packing_str(csi_c5_packing_t p)
{
    return (p == CSI_C5_PACK_LLTF12) ? "lltf12" : "iq8";
}

/* ------------------------------------------------------------------------ *
 * CSV output
 *
 * One line per record. The column set is C5-specific: it uses the fields that
 * esp_wifi_rxctrl_t actually has. Code ported from an ESP32 CSI example that
 * prints sig_mode, mcs, cwb, ampdu_cnt, secondary_channel or ant does not
 * compile here, because none of those members exist on this chip.
 * ------------------------------------------------------------------------ */
#define CSI_C5_CSV_HEADER \
    "CSI_DATA,seq,src_mac,dst_mac,rssi,rate,noise_floor,bb_format,channel,second," \
    "is_group,timestamp,sig_len,rx_state,ce_len,ce_valid,first_word_invalid," \
    "packing,tones,len,data"

void csi_c5_print_csv_header(void);

/* Prints one CSV line. Call from your own task, never from the CSI callback. */
void csi_c5_print_csv(const csi_c5_record_t *r);

#ifdef __cplusplus
}
#endif
