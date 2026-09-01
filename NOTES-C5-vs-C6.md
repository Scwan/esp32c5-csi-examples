# Why ESP32-C5 CSI code is not ESP32-C6 CSI code

Almost every Wi-Fi CSI example you can find targets the ESP32, the ESP32-S3 or
the ESP32-C6. None of them compile unchanged on a C5, and the reason is one
number.

## The one number

`components/soc/esp32c5/include/soc/Kconfig.soc_caps.in` (ESP-IDF release/v5.5):

```
config SOC_WIFI_CSI_SUPPORT      default y
config SOC_WIFI_HE_SUPPORT       default y
config SOC_WIFI_SUPPORT_5G       default y
config SOC_WIFI_MAC_VERSION_NUM  default 3
```

For the ESP32-C6 the same file says `SOC_WIFI_MAC_VERSION_NUM default 2`, and it
has no `SOC_WIFI_SUPPORT_5G` at all.

`components/esp_wifi/include/esp_wifi_he_types.h` branches on exactly that
symbol — `#if CONFIG_SOC_WIFI_MAC_VERSION_NUM == 3` — for **both** the CSI
acquisition struct and the receive-control struct. So the C5 gets a different
struct definition from the C6, under the same type name.

## Three layers of divergence

There are three separate splits stacked on top of each other, and it is worth
seeing them as distinct because each one breaks different code:

1. **HE vs non-HE.** `esp_wifi_types_native.h` does
   `typedef wifi_csi_acquire_config_t wifi_csi_config_t` when
   `CONFIG_SOC_WIFI_HE_SUPPORT` is set, and defines a completely different
   struct otherwise. This split separates {C5, C6} from {ESP32, S2, S3, C3}.
2. **MAC v3 vs v2.** Within the HE branch, `esp_wifi_he_types.h` splits again.
   This separates C5 from C6.
3. **Dual band.** Only the C5 has `SOC_WIFI_SUPPORT_5G`, which brings in the
   band-mode API and widens some `rx_ctrl` fields.

## `wifi_csi_config_t`

| field | ESP32 etc. | ESP32-C6 | ESP32-C5 |
|---|---|---|---|
| `lltf_en`, `htltf_en`, `stbc_htltf2_en`, `ltf_merge_en`, `channel_filter_en`, `manu_scale`, `shift` | yes | — | — |
| `enable` | — | yes | yes |
| `acquire_csi_legacy` | — | yes | yes |
| `acquire_csi_force_lltf` | — | **no** | **yes** |
| `acquire_csi_ht20`, `acquire_csi_ht40` | — | yes | yes |
| `acquire_csi_vht` | — | **no** | **yes** |
| `acquire_csi_su`, `_mu`, `_dcm`, `_beamformed` | — | yes | yes |
| STBC selector | — | `acquire_csi_he_stbc` | `acquire_csi_he_stbc_mode` |
| `val_scale_cfg` | — | 2 bits, 0–3 | 4 bits, 0–8 |
| `dump_ack_en` | yes | yes | yes |
| `lltf_bit_mode` | — | — | **yes, and only from ESP-IDF v5.5** |

Note the last row. `lltf_bit_mode` is absent from `release/v5.4` and present in
`release/v5.5` and `master`. Naming it in an initialiser makes the code
v5.5-only for no benefit, since omitting a designated initialiser zeroes it and
zero is the documented default. The shared component here deliberately does not
mention it.

## `rx_ctrl`

On the C5 `wifi_pkt_rx_ctrl_t` is a typedef of `esp_wifi_rxctrl_t`. It has
nothing to do with the classic struct. These members, which appear in
practically every ESP32 CSI example ever written, **do not exist**:

```
sig_mode   mcs   cwb   smoothing   not_sounding   aggregation   stbc
fec_coding   sgi   ampdu_cnt   secondary_channel   ant
```

What you have instead:

| what you want | classic ESP32 | ESP32-C5 |
|---|---|---|
| PHY of the frame | `sig_mode` (0/1/3) | `cur_bb_format`, a `wifi_rx_bb_format_t` |
| bandwidth | `cwb` | implied by `cur_bb_format` and `second` |
| second channel | `secondary_channel` | `second` |
| RSSI | `rssi` | `rssi` |
| noise floor | `noise_floor` | `noise_floor` |
| receive status | `rx_state` | `rx_state`, plus `rxend_state` |
| is the CSI usable | — | `rx_channel_estimate_info_vld` |
| CSI length | — | `rx_channel_estimate_len` |

Going the other way — from a C6 example to a C5 — the fields that vanish are
`cur_single_mpdu` and `he_sigb_len`. Be precise about the rest: `sig_len`,
`dump_len` and `rx_state` all exist in *both* branches, so the only identifier
that is genuinely v3-exclusive is `sigb_len` (the C6 spells its analogue
`he_sigb_len`, and it is 6 bits rather than 10).

Two C5-only details worth knowing:

- `channel` and `second` are **8 bits** on the C5, against 4 bits on the C6 and
  the classic parts. They have to be: a 4-bit field cannot hold channel 149.
  This is a good sanity check that you are looking at the right struct.
- `rx_channel_estimate_info_vld` is the flag the Wi-Fi guide tells you to
  check — "if `rx_channel_estimate_info_vld` of `rx_ctrl` field is 1, indicates
  that the CSI data is valid; otherwise, the CSI data is invalid". There is no
  equivalent on the classic parts, and code ported from them never checks it.

## The record itself

`wifi_csi_info_t` is the same on every target, and its `buf` is the same
layout everywhere: two signed bytes per sub-carrier, **imaginary first, real
second**.

What differs is how many of them. From the ESP-IDF Wi-Fi guide's C5 table:

| PHY | bandwidth | sub-carriers | `len` |
|---|---|---|---|
| non-HT (11a/g) | 20 MHz | 52 (`0..26`, `-26..-1`) | 106 |
| HT (11n) | 20 MHz | 56 (`0..28`, `-28..-1`) | 114 |
| HT, STBC | 20 MHz | 112 (two LTFs) | 228 |
| HT | 40 MHz | 117 (`0..58`, `-58..-1`) | 234 |
| HT, STBC | 40 MHz | 234 (two LTFs) | 468 |
| HE (11ax) | 20 MHz | 245 (`-122..-1`, `0..122`) | **490** |
| VHT (11ac) | 20 MHz | same as HT | same as HT |

`len` exceeds sub-carriers × 2 by two bytes in the non-STBC rows because the
driver aligns the record to four bytes; the last two bytes are padding.

## The documentation is wrong about L-LTF, and wrong silently

This is the most important thing on this page.

`acquire_csi_force_lltf` — which the C6 does not have — selects which LTF you
get from an HT/VHT/HE frame. What is not documented anywhere is that on the C5
it **also changes the packing**, because of a field the docs never mention:

```c
uint32_t lltf_bit_mode : 1;   /**< 0 : 12-bit, 1 : 8-bit, default : 12-bit */
```

So:

| `acquire_csi_force_lltf` | LTF | packing | bytes/tone | `len` | tones |
|---|---|---|---|---|---|
| `false` | HT/VHT/HE-LTF | 8-bit | 2 | 114 / 228 / 234 / 468 / 490 | `len/2` |
| `true` | L-LTF | **12-bit** | **4** | 106 | **26** |

In 12-bit mode, four `int8` elements make one complex value. `buf[i]` and
`buf[i+1]` are a little-endian 16-bit word whose bits [11:0] are the signed real
part; `buf[i+2]` and `buf[i+3]` are the imaginary part. Espressif's own decoder:

```c
int16_t csi = ((int16_t)(((((uint16_t)info->buf[i + 1]) << 8) | info->buf[i]) << 4) >> 4);
```

stepping `i` by 2 from 0 to `len - 2`, giving `(len - 2) / 2` components — 52
for a 106-byte record, so 26 complex tones at sub-carrier indices
`[-26 : 2 : 26]`. The final two bytes are padding.

Read that as int8 pairs and nothing announces the mistake. The reporter of the
still-open issue #18982 put it exactly right: "the values parse cleanly and plot
plausibly". The only visible symptom is that the odd-index byte only ever takes
four distinct values. Espressif closed issue #18493 — "CSI IQ buffer contains
static data on ESP32-C5 5GHz channel", 12,000 frames yielding two unique IQ
patterns — as precisely this error in the reporter's parsing code.

As of 2026-09-01 the fix has not reached the documentation on any branch:
`lltf_bit_mode` appears nowhere in `docs/`, and the C5 CSI page still says two
bytes per sub-carrier for every case.

`csi_c5.h` handles both, and tags every record with which one applied.

**Which to use.** The argument for forcing L-LTF is stable record shape, since
otherwise the length follows whatever PHY the transmitter's rate control picked.
Espressif's advice for human sensing is nevertheless to leave it off:

> 234 subcarriers are sufficient to capture human body state features […] 8-bit
> precision is completely adequate for detecting sitting/standing/walking

with `acquire_csi_force_lltf = 0, acquire_csi_ht40 = 1` over an 11n 40 MHz link,
which yields a steady 234. That is what `04_motion` follows.

## The C5 can only receive 20 MHz, except in 11n

A hardware limit, from Espressif on issue #18493:

> This is a hardware limitation of the ESP32-C5: for HE/VHT it only supports
> 20 MHz, because regardless of the mode, it can only receive 20 MHz packets.
> For HT it supports 40 MHz, but this has to be configured — the default is
> 20 MHz.

and

> If it's being transmitted on a bandwidth greater than 20 MHz, the ESP32-C5's
> promiscuous mode won't be able to capture it.

This matters most in exactly the place you would want the C5: 5 GHz, where a
modern AP very often runs 80 MHz HE. Those frames are **invisible** to this
chip — not weak, not garbled, simply never received. A sniffer parked on such a
channel reports nothing and looks broken.

Espressif have also said they will not add it:

> As an IoT device, due to cost and hardware constraints of the C5, we will not
> support 40MHz bandwidth on 11AX.

So the widest capture available is 40 MHz on 11n, and it must be configured —
via `esp_wifi_set_bandwidths()` under `WIFI_BAND_MODE_AUTO`, or
`esp_wifi_set_bandwidth()` under a single-band mode.

## Band, and how not to lose your association

`esp_wifi.h` on `esp_wifi_set_band()`:

> It is recommended not to use this API. If you want to change the current band,
> you can use `esp_wifi_set_channel` instead.

So set the band *mode* (which band is permitted) and then set the channel, and
let the band follow.

**And set it after `esp_wifi_start()`, not before.** This is the one that costs
the most time, because a radio-wide setting reads like something you configure
during init. `esp_wifi_set_band_mode()` documents `ESP_ERR_WIFI_NOT_STARTED`, so
calling it between `esp_wifi_init()` and `esp_wifi_start()` fails — and if you
wrapped it in `ESP_ERROR_CHECK()` the app aborts at boot. The working order is:

```
esp_wifi_init()  ->  esp_wifi_set_mode()  ->  esp_wifi_start()
   ->  esp_wifi_set_band_mode()      (needs started)
   ->  esp_wifi_set_protocol()       (needs a single band, not AUTO)
   ->  esp_wifi_set_ps(WIFI_PS_NONE)
   ->  esp_wifi_set_channel()        (needs started)
   ->  CSI: set_csi_rx_cb, set_csi_config, set_csi(true)
```

A knock-on for station examples: `WIFI_EVENT_STA_START` fires from inside
`esp_wifi_start()`, which is before the band mode can legally be set. The usual
idiom of calling `esp_wifi_connect()` from that event therefore races the band
selection and can scan the wrong band. Connect explicitly after the band is
settled instead.

Two more consequences that bite:

- Under `WIFI_BAND_MODE_AUTO`, `esp_wifi_set_protocol()`,
  `esp_wifi_get_protocol()`, `esp_wifi_set_bandwidth()` and
  `esp_wifi_get_bandwidth()` all return `ESP_ERR_NOT_SUPPORTED`. Pin a single
  band when you need those, and use `esp_wifi_set_protocols()` /
  `esp_wifi_set_bandwidths()` (plural) when you genuinely want AUTO.
- The protocol bitmap is per band. Crossing bands without reapplying it leaves
  the receiver not decoding PHYs you thought you had enabled — which presents as
  "that channel is quiet", not as an error.

On 5 GHz, `esp_wifi_set_channel()` documents that the second channel is derived
from the primary per the 802.11 standard and "any manually configured second
channel will be ignored".

And the one that costs an afternoon: if the station is **associated**, do not
call `esp_wifi_set_channel()` at all. The AP owns the channel; moving the radio
drops the link, and the reconnect loop that follows looks exactly like a CSI
bug. The shared component takes `channel = 0` to mean "leave it alone".

## Not every 5 GHz channel is reachable out of the box

Having 28 five-gigahertz channels in `wifi_5g_channel_bit_t` is not the same as
being able to tune all of them. Two gates sit in front:

- ESP-IDF's default country is `"01"`, world safe mode, whose entry in
  `components/esp_wifi/regulatory/esp_wifi_regulatory.txt` allows only
  5170–5250 MHz (channels 36–48) and 5250–5330 MHz (channels 52–64), both
  flagged NO-IR — receive-only, no initiating radiation.
- `wifi_country_t` carries a `wifi_5g_channel_mask` field, and the default the
  docs give for the C5 is `0xfe`. Decoded against `wifi_5g_channel_bit_t` that
  is bits 1–7, i.e. channels 36, 40, 44, 48, 52, 56 and 60 — note it excludes
  channel 64 and includes three DFS channels.

So a passive sweep across 100–177 will mostly fail to tune, and
`esp_wifi_set_channel()` returning `ESP_ERR_INVALID_ARG` on those channels is
correct behaviour rather than a bug. `05_band_sweep` logs and skips them.

To widen the set you must call `esp_wifi_set_country()` with
`policy = WIFI_COUNTRY_POLICY_MANUAL` and a non-zero `wifi_5g_channel_mask`.
`esp_wifi_set_country_code()` cannot do it — it takes only a country string and
an 802.11d flag, with no 5 GHz parameter at all. Set the country to where the
board actually is; the restrictions are regulatory, and transmitting outside
them is not a configuration preference.

Note also that `wifi_5g_channel_mask` lives inside
`#if CONFIG_SOC_WIFI_SUPPORT_5G`, so a designated initialiser naming it fails to
compile for the C6 or the ESP32.

On DFS: the C5 supports DFS channels with passive radar detection only. As a
station it can scan and connect there and will follow an AP's channel-switch
announcement; as a SoftAP it is not permitted to operate on them.

## Enabling CSI silently disables HE-SIG-B dumping

`esp_wifi.h`:

```c
#if CONFIG_ESP_WIFI_ENABLE_DUMP_HESIGB && !WIFI_CSI_ENABLED
#define WIFI_DUMP_HESIGB_ENABLED  true
#else
#define WIFI_DUMP_HESIGB_ENABLED  false
#endif
```

feeding `.dump_hesigb_enable = WIFI_DUMP_HESIGB_ENABLED` in
`WIFI_INIT_CONFIG_DEFAULT()`. The two features are mutually exclusive and CSI
wins, with no warning: tick both in menuconfig and HE-SIG-B dumping is off.

This is C5-specific because `ESP_WIFI_ENABLE_DUMP_HESIGB` depends on
`SOC_WIFI_SUPPORT_5G`. Three sibling options are gated the same way and likewise
do not exist on a C6: `ESP_WIFI_ENABLE_DUMP_MU_CFO`,
`ESP_WIFI_ENABLE_DUMP_CTRL_NDPA` and `ESP_WIFI_ENABLE_DUMP_CTRL_BFRP`.

## Sources

Everything above was read from ESP-IDF `release/v5.5`:

- `components/soc/esp32c5/include/soc/Kconfig.soc_caps.in`
- `components/soc/esp32c6/include/soc/Kconfig.soc_caps.in`
- `components/esp_wifi/include/esp_wifi_he_types.h`
- `components/esp_wifi/include/local/esp_wifi_types_native.h`
- `components/esp_wifi/include/esp_wifi_types_generic.h`
- `components/esp_wifi/include/esp_wifi.h`
- `components/esp_wifi/Kconfig`
- `docs/en/api-guides/wifi.rst`, the `.. only:: esp32c5` block of the
  "Wi-Fi Channel State Information" section
- `components/lwip/include/apps/ping/ping_sock.h`
- `components/esp_wifi/regulatory/esp_wifi_regulatory.txt`

Plus the ESP-IDF issue tracker, where the C5-specific corrections live that the
documentation has not caught up with:

- [#18493](https://github.com/espressif/esp-idf/issues/18493) — closed; the
  12-bit L-LTF packing, and the 20 MHz receive limit
- [#18982](https://github.com/espressif/esp-idf/issues/18982) — open; the docs
  are wrong about the 106-byte layout
- [#14271](https://github.com/espressif/esp-idf/issues/14271) — open since 2024;
  HE-family sub-carrier ordering is undocumented
- [#18565](https://github.com/espressif/esp-idf/issues/18565) — open; C5 CSI
  support is version-gated

and Espressif's [esp-csi](https://github.com/espressif/esp-csi) reference
implementation, whose `examples/get-started/csi_recv` carries the only published
12-bit decoder.
