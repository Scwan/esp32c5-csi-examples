# 02_espnow_pair

Two boards. One transmits, one measures. This is the setup to use when you
actually care about the measurement, because it is the only one where you
control the stimulus.

`tx/` sends an ESP-NOW broadcast every 20 ms at a pinned PHY and rate. `rx/`
sits on the same channel in promiscuous mode and reports CSI for those frames,
optionally filtered to that one transmitter's MAC.

Everything on the transmit side is held fixed — channel, PHY, rate, interval —
so anything that moves in the CSI moved in the room. In `01` and `03` you are at
the mercy of somebody else's rate control, and a change in the data might just
be the AP deciding to use a different MCS.

ESP-NOW is used because it needs no association, no AP and no DHCP: power both
boards and they are talking.

## Build both halves

They are two separate projects. From `02_espnow_pair`:

```bash
cd tx && idf.py set-target esp32c5 menuconfig && idf.py -p PORT_A flash monitor
```

```bash
cd rx && idf.py set-target esp32c5 menuconfig && idf.py -p PORT_B flash monitor
```

**`CSI_TX_CHANNEL` and `CSI_RX_CHANNEL` must be identical.** ESP-NOW refuses to
send when the peer channel and the radio channel disagree, and reports
`ESP_ERR_ESPNOW_CHAN`.

The transmitter prints its own MAC at boot:

```
I (312) csi_tx: transmitting as 60:55:f9:aa:bb:cc on channel 36 (5 GHz)
```

Paste that into the receiver's **Only report CSI from this transmitter MAC** to
ignore everything else on the channel. Leave it at `00:00:00:00:00:00` while you
are still checking that anything arrives at all.

## What you see on the receiver

Alongside the CSV, a one-line-per-record summary:

```
AMP,1043,11ax-SU,243,-41,37.82
```

That is sequence number, PHY, usable sub-carriers, RSSI, and mean amplitude
across the sub-carriers. Watch the last number while you move a hand through the
line between the boards — it is the fastest way to convince yourself that CSI is
measuring the room and not just the radio.

## HE20 or legacy

`CSI_TX_PHY_HE20` (default on) pins the transmitter to 802.11ax HE20 MCS0. The
receiver then gets 245 sub-carriers in a 490-byte record: the most frequency
resolution this chip can give you, and a thing no other Espressif CSI part can
do at all.

Turn it off for legacy 6 Mbps, which gives 52 sub-carriers in a 106-byte record.
Smaller and lower resolution, but directly comparable with CSI captured from an
older ESP32, which matters if you are reproducing published work.

If `esp_now_set_peer_rate_config()` fails, the transmitter logs a warning and
carries on — but the rate then adapts, and record length starts varying between
frames. That warning is worth reading rather than scrolling past.

## Why the receiver has `dump_ack` off

Broadcasts are never acknowledged at the 802.11 level, so there are no ACKs from
this transmitter to capture. Leaving `dump_ack_en` on would only mix in CSI from
unrelated traffic, which is the opposite of what this example is for.

## Rate

20 ms gives 50 records per second, enough to see a person cross the link. Drop
to 10 ms for gesture work. Below that the console becomes the bottleneck before
the radio does — an HE20 CSV line is about 2 kB, so 100 records per second is
roughly 200 kB/s, above what 921600 baud carries. Turn off
`CSI_RX_PRINT_CSV` and keep the `AMP` lines if you need the rate more than the
raw data.
