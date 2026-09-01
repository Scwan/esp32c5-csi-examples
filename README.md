# Wi-Fi CSI examples for the ESP32-C5 (ESP-IDF)

[![build](https://github.com/Scwan/esp32c5-csi-examples/actions/workflows/build.yml/badge.svg)](https://github.com/Scwan/esp32c5-csi-examples/actions/workflows/build.yml)

Five ESP-IDF projects that capture Channel State Information on an ESP32-C5,
plus one shared component that hides the parts of the API that are specific to
this chip.

The C5 is the first Espressif part that does CSI on **5 GHz**, and it is a
Wi-Fi 6 radio, so an HE20 frame gives you 245 sub-carriers instead of the 52 or
56 you get from an ESP32. It is also the part with the least CSI example code in
existence, because it does not share a struct layout with either the classic
ESP32 family or the C6 — see [NOTES-C5-vs-C6.md](NOTES-C5-vs-C6.md)
for exactly what differs and why ported code fails to compile.

## The examples

| | what it does | needs |
|---|---|---|
| `01_sniffer` | Parks on one channel in promiscuous mode and prints CSI for whatever is in the air. The smallest thing that works. | one board |
| `02_espnow_pair` | A transmitter board sends ESP-NOW frames at a pinned PHY and a fixed interval; a receiver board measures CSI from them. Full control of the stimulus. | two boards |
| `03_router` | Associates with your AP and pings the gateway; every echo reply is a downlink frame, so every one is a CSI record. A self-sustaining stream from one board. | one board + an AP |
| `04_motion` | `03` plus an amplitude-variance detector: learns a baseline, then reports when the channel changes. | one board + an AP |
| `05_band_sweep` | Walks all 32 channels across both bands and reports where there is traffic and which PHYs are in use. | one board |

Start with `01` to prove the toolchain and the board work, then `05` to find a
channel worth watching, then `02` if you have two boards or `03` if you have
one.

## Requirements

- An ESP32-C5 board.
- **ESP-IDF v5.5.2 or newer** (v5.5.5 is the current v5.5 bugfix; v6.0.2 and
  v6.1 also work). The version matters more than usual here:

  | | ESP32-C5 status |
  |---|---|
  | v5.3 | preview, and the only release with the beta3/MP silicon split |
  | v5.4.x | preview, and pinned to **v0.x silicon** — it cannot target production chips |
  | v5.5.0 | still a *preview* target: `idf.py set-target esp32c5` fails without `--preview` |
  | **v5.5.2+** | supported; `ESP32C5_REV_MIN_100` (v1.0 ECO2), i.e. production silicon |

  The build system promotes `esp32c5` out of `PREVIEW_TARGETS` at v5.5.1, while
  Espressif's own developer portal says "initial mass production support …
  released in ESP-IDF v5.5.2". They disagree by one bugfix release; v5.5.2 is
  the safe reading.

  The code itself was written against `release/v5.5`, and the CSI headers are
  byte-identical there and on `master`, so the v6 lines are fine too.

- ESP-IDF ships **no CSI example of its own** — `examples/wifi/` has espnow,
  ftm, itwt, iperf and a dozen others, and nothing for CSI on any target. That
  is the gap this tree fills.

## Build and flash

```bash
idf.py set-target esp32c5
```

```bash
idf.py menuconfig
```

```bash
idf.py -p PORT flash monitor
```

Run those from inside one example directory, for example `01_sniffer`. Each
example is a standalone project; they find the shared component through
`EXTRA_COMPONENT_DIRS` in their `CMakeLists.txt`.

Every example ships an `sdkconfig.defaults` that already contains the one
setting you cannot do without:

```
CONFIG_ESP_WIFI_CSI_ENABLED=y
```

The five CSI functions are declared unconditionally in `esp_wifi.h`, so code
using them compiles and links either way. What this option actually controls is
`WIFI_CSI_ENABLED`, which `WIFI_INIT_CONFIG_DEFAULT()` assigns to `csi_enable` —
so with it off you get a perfectly good build that delivers zero callbacks and
reports no error anywhere. There is no runtime substitute; it has to be right
before you compile.

`sdkconfig.defaults` only applies when there is no `sdkconfig` yet, so if you
have already built once, delete `sdkconfig` or set it in `menuconfig` under
**Component config → Wi-Fi → WiFi CSI(Channel State Information)**.

The shared component has an `#error` for this case, so you will be told at
compile time rather than left staring at a silent console.

One side effect worth knowing, and it is C5-only: `esp_wifi.h` computes
`dump_hesigb_enable` as `CONFIG_ESP_WIFI_ENABLE_DUMP_HESIGB && !WIFI_CSI_ENABLED`.
Enabling CSI therefore forces HE-SIG-B dumping **off**, silently, whatever you
selected in menuconfig. The two are mutually exclusive and CSI wins. This cannot
happen on a C6, because `ESP_WIFI_ENABLE_DUMP_HESIGB` depends on
`SOC_WIFI_SUPPORT_5G`, which only the C5 has.

## What comes out

Examples that print raw records emit one CSV line each:

```
CSI_DATA,seq,src_mac,dst_mac,rssi,rate,noise_floor,bb_format,channel,second,
is_group,timestamp,sig_len,rx_state,ce_len,ce_valid,first_word_invalid,
packing,tones,len,data
```

`data` is the raw CSI bytes. **How to read them depends on the `packing`
column**, and this is the one thing on this page most likely to cost you a day:

- `iq8` — HT/VHT/HE-LTF. Two signed bytes per tone, **imaginary first, real
  second**. Amplitude of tone *k* is `hypot(buf[2k+1], buf[2k])`.
- `lltf12` — L-LTF, which on the C5 defaults to **12-bit** packing: four bytes
  per tone, two little-endian 16-bit words whose bits [11:0] are signed, real
  first then imaginary. A 106-byte record is 26 tones, not 53.

ESP-IDF's own documentation describes only the first case and applies it to
both. Reading `lltf12` data as `iq8` does not fail loudly — it produces numbers
that, in the words of the person who reported it upstream, "parse cleanly and
plot plausibly". Espressif closed
[issue #18493](https://github.com/espressif/esp-idf/issues/18493) as exactly
this mistake. The `csi_c5_amplitude()` / `csi_c5_phase()` helpers handle both;
use them rather than indexing `buf` yourself.

`ce_valid` is `rx_ctrl.rx_channel_estimate_info_vld`. The Wi-Fi guide says the
CSI data is valid when it is 1 and invalid otherwise — treat records where it is
0 as garbage rather than as data.

`first_word_invalid` means the **first four bytes** of `buf` are junk from a
hardware limitation. Note that is four *bytes*, not four values: two tones under
`iq8`, one under `lltf12`. The helper `csi_c5_first_subcarrier()` returns the
first index you may read, and gets that distinction right for you.

Record length tells you which PHY produced it: 106 bytes is legacy, 114 is HT20,
234 is HT40, 490 is HE20. The full table is in
[NOTES-C5-vs-C6.md](NOTES-C5-vs-C6.md).

## Nothing appears

In roughly the order worth checking:

1. **`CONFIG_ESP_WIFI_CSI_ENABLED` is not actually set.** A stale `sdkconfig`
   from an earlier build silently wins over `sdkconfig.defaults`. Check with
   `idf.py menuconfig`, or delete `sdkconfig` and rebuild. The component's
   `#error` catches this at compile time — if you got a binary, it is set.
2. **Nothing is transmitting on that channel.** `01` and `05` are passive. Run
   `05_band_sweep` to find out where the traffic actually is; a channel with an
   AP on it produces beacons roughly every 100 ms even when idle.
3. **The traffic is wider than 20 MHz.** The C5 can only *receive* 20 MHz
   except in 11n, where 40 MHz works but has to be configured. Frames sent on
   80 or 160 MHz — very common for 5 GHz 11ax — are not received at all, so a
   busy channel can look completely silent. This is a hardware limit and
   Espressif have said 40 MHz on 11ax will not be added.
4. **The two boards are not on the same channel.** For `02`, `CSI_TX_CHANNEL`
   and `CSI_RX_CHANNEL` must match exactly. ESP-NOW reports
   `ESP_ERR_ESPNOW_CHAN` when the peer channel and the radio channel disagree.
5. **11AX is not enabled on the receiver** while the transmitter is pinned to
   HE20. The frame is then not decoded at all and produces no CSI. The examples
   set the protocol bitmap explicitly for this reason — and it is per band, so
   it must be reapplied after a band change.
6. **You called something too early.** `esp_wifi_set_csi_config()`,
   `esp_wifi_set_csi()`, `esp_wifi_set_channel()` and — the surprising one —
   `esp_wifi_set_band_mode()` all require Wi-Fi to be **started**. Configuring
   the band during init looks natural and fails. The order that works is
   `esp_wifi_start()` → `esp_wifi_set_band_mode()` →
   `esp_wifi_set_protocol()` → `esp_wifi_set_ps()` → `esp_wifi_set_channel()` →
   `esp_wifi_set_csi_rx_cb()` → `esp_wifi_set_csi_config()` →
   `esp_wifi_set_csi(true)`.
7. **The 5 GHz channel is not permitted by the regulatory default.** ESP-IDF
   defaults to country `"01"` and a `wifi_5g_channel_mask` of `0xfe`, which
   between them allow only channels 36–60. Anything from 64 upward needs
   `esp_wifi_set_country()` with `WIFI_COUNTRY_POLICY_MANUAL`. See
   [NOTES-C5-vs-C6.md](NOTES-C5-vs-C6.md).
8. **Power save is eating the receiver.** Call `esp_wifi_set_ps(WIFI_PS_NONE)`.
   Modem sleep gates the radio off for part of the time and, per the `rx_ctrl`
   documentation, also makes `timestamp` imprecise.
9. **The console is the bottleneck.** An HE20 CSV line is around 2 kB. At
   115200 baud that is roughly six lines per second. The defaults set
   921600 baud; if you see drops in the summary line, that is where they are.

## Accuracy

Every API fact in this tree was read from ESP-IDF `release/v5.5` sources rather
than from memory, because there is no ESP-IDF on the machine it was written on.
The sources consulted are listed at the end of
[NOTES-C5-vs-C6.md](NOTES-C5-vs-C6.md).

**It builds.** CI compiles all six projects for `esp32c5` against ESP-IDF
v5.5.2 and v5.5.5 on every push. Two things follow that are worth more than a
green tick:

- Since `csi_c5.h` has an `#error` on `!CONFIG_ESP_WIFI_CSI_ENABLED`, a passing
  build proves `sdkconfig.defaults` actually took effect — the failure mode that
  is otherwise completely silent at runtime.
- The v5.5.2 leg passing confirms `esp32c5` is genuinely a supported, non-preview
  target there, which is the version floor claimed above.

**It has never been run.** No part of this has met a radio. Compiling is not
evidence that the CSI parsing is right, that the record shapes match the tables,
or that any example produces data. Treat the numbers in the sample output as
illustrative, not as recordings.

One field is genuinely undocumented upstream: `val_scale_cfg`. ESP-IDF states
its range (0–8 on the C5, 0–3 on the C6) and nothing else — not what the values
mean, not whether 0 is special. The examples leave it at 0 and say so rather
than inventing an explanation.

## Prior art

Espressif's own CSI project is [esp-csi](https://github.com/espressif/esp-csi),
which is worth reading and targets the earlier chips. Nothing here is copied
from it.
