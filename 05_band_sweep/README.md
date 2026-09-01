# 05_band_sweep

A survey: which channels around you carry traffic, and what kind.

The board walks every channel the C5 recognises — 4 on 2.4 GHz and 28 on
5 GHz — dwelling on each, counting CSI records and tallying which PHY produced
them. One pass at the default 800 ms dwell takes about 26 seconds.

This is the example that only makes sense on a C5. Every other Espressif chip
with CSI is 2.4 GHz only, so the same sweep covers a handful of channels; here
there are 32.

Run it first when you arrive somewhere new. It answers "where should I point
`01_sniffer`" in one pass, which is otherwise a guessing game.

## Run it

```bash
idf.py set-target esp32c5 menuconfig
```

Pick the bands and the dwell under **CSI band sweep configuration**.

```bash
idf.py -p PORT flash monitor
```

## Output

```
SWEEP pass 3
  ch band  records  11b  11g/a   11n  11ac  11ax  bestRSSI  badCE
   1  2G4      184   12     47   125     0     0       -52      2
   6  2G4       31    0     14    17     0     0       -78      0
  11  2G4      402    8     96   298     0     0       -44      5
  36   5G      288    0     31   107    64    86       -39      1
  40   5G        0    0      0     0     0     0         0      0
 ...
 149   5G       77    0      9    41    12    15       -61      0
```

`bestRSSI` is the strongest record seen on that channel, so it is a rough
proxy for how close the nearest transmitter is. `badCE` counts records where
`rx_channel_estimate_info_vld` was 0 — the frame decoded but the channel
estimate did not, which is what a marginal signal looks like.

The column worth looking at is `11ax`. Those are the channels where you can
capture HE-LTF CSI at 245 sub-carriers rather than the 52 or 56 a legacy or HT
frame gives you.

## It is passive

The sweep counts what other people transmit. A short dwell on a quiet channel
proves nothing, so a zero row means "nothing was heard in 800 ms", not "this
channel is empty". Raise the dwell to 2000 ms or more if you actually want to
establish that a channel is unused. Any AP will emit beacons about every 100 ms,
so a few hundred milliseconds is enough to find one if it is there.

Channels 52 to 144 are DFS. Receiving there is fine — the board only listens —
but many APs avoid them, so empty rows are expected.

The other reason for an empty row is a hard limit rather than an absence: the C5
can only **receive** 20 MHz outside 11n. A 5 GHz AP running 80 MHz HE — the
common case — is invisible to it. So a zero in the `11ax` column does not mean
nobody nearby is using Wi-Fi 6; it can equally mean they are using it too wide
to capture. See [NOTES-C5-vs-C6.md](../NOTES-C5-vs-C6.md).

## Expect most of 100–177 to be unreachable at first

`could not tune channel 132, skipping` is not a bug. ESP-IDF's default country
is `"01"`, world safe mode, which permits only channels 36–64, and the C5's
default `wifi_5g_channel_mask` of `0xfe` narrows that further to 36, 40, 44, 48,
52, 56 and 60. `esp_wifi_set_channel()` correctly refuses the rest.

To reach the others, call `esp_wifi_set_country()` with
`WIFI_COUNTRY_POLICY_MANUAL` and a wider `wifi_5g_channel_mask` — set for
wherever the board actually is. `esp_wifi_set_country_code()` cannot do it; it
has no 5 GHz parameter. Details in [NOTES-C5-vs-C6.md](../NOTES-C5-vs-C6.md).

## The interesting part is the band crossing

Hopping within a band is one `esp_wifi_set_channel()`. Crossing between bands is
not, and this example shows what it takes:

```c
csi_c5_stop();                                   /* CSI + promiscuous off */
esp_wifi_set_band_mode(five ? WIFI_BAND_MODE_5G_ONLY : WIFI_BAND_MODE_2G_ONLY);
esp_wifi_set_protocol(WIFI_IF_STA, protocols_for(five));
csi_c5_start(&s_cfg);                            /* re-park, CSI back on */
```

Two things are load-bearing there.

**No `esp_wifi_stop()`.** The obvious-looking version of this wraps the band
change in a stop/start cycle. That is exactly wrong: both
`esp_wifi_set_band_mode()` and `esp_wifi_set_channel()` document
`ESP_ERR_WIFI_NOT_STARTED`, so a stop/start would put both calls in the one
state where they are guaranteed to fail. Dropping CSI and promiscuous mode first
leaves a plain started station, which is the state they expect.

**`esp_wifi_set_protocol()` is per band** — 11b/g/n/ax on 2.4 GHz, 11a/n/ac/ax
on 5 GHz — and it is the call that gets forgotten. Cross bands without
reapplying it and the receiver quietly stops decoding whole PHYs on the new
band. That produces no error: it produces a row of zeros, and you conclude the
channel is quiet.
