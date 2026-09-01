# 03_router

One board, your own access point, a steady CSI stream.

The board associates with your AP and pings the gateway every 20 ms. Each echo
reply is a downlink 802.11 data frame addressed to this station, and each of
those yields a CSI record. The link keeps itself busy, so unlike `01_sniffer`
you are not waiting for somebody else to transmit.

If your AP is dual-band and you pin this to 5 GHz, you are measuring CSI on a
5 GHz link — which is the thing the C5 can do and no other Espressif chip can.

## Run it

```bash
idf.py set-target esp32c5 menuconfig
```

Set **CSI from a router configuration → Access point SSID / password**, and
optionally pin the band.

```bash
idf.py -p PORT flash monitor
```

You should see it associate, report the channel, and start counting:

```
I (3120) csi_router: associated on channel 44 (5 GHz)
I (3140) csi_router: pinging 192.168.1.1 every 20 ms - each reply is one downlink frame
I (8140) csi_router: 231 records (231 from the AP) | ping 248 replies, 0 timeouts | 0 dropped
```

## Why ping

Because it is the one stimulus that *guarantees* a frame comes back, and it is
the topology Espressif documents first — "ESP32 sends a Ping packet to the
router, and receives the CSI information carried in the Ping Replay returned by
the router".

The tempting alternative is to fire UDP at a dead port and harvest the 802.11
ACKs the AP sends for each one. That does not depend on anything listening,
which sounds robust, but it rests entirely on `dump_ack_en` — a field whose
total documentation is the seven words in its own header comment, which appears
nowhere in the ESP-IDF docs, and which both of Espressif's own CSI examples set
to `false`. It is left on here as a bonus source, not as the mechanism.

If the summary reports ping timeouts and no replies, your router drops ICMP.
Point the example at a host on the LAN that answers, or fall back to the ACKs
alone.

## Band choice

`WIFI_BAND_MODE_AUTO` (the default) lets the C5 find the AP on either band.
Nothing in this example needs `esp_wifi_set_protocol()` or
`esp_wifi_set_bandwidth()`, the two APIs that return `ESP_ERR_NOT_SUPPORTED`
under AUTO, so AUTO is safe here in a way it is not in `01` or `05`.

Pin the band when your AP advertises the same SSID on 2.4 and 5 GHz and you want
to know which one you measured.

A caveat specific to 5 GHz: the C5 can only *receive* 20 MHz outside 11n. If
your AP is running 80 MHz HE on 5 GHz — very common — those frames are not
received at all. You will still get CSI, because the AP drops to a narrower PPDU
for the small management and ICMP frames, but do not expect to capture its bulk
traffic.

## The channel is the AP's, not yours

This example never calls `esp_wifi_set_channel()`. Once associated, the AP owns
the channel; moving the radio drops the link, and the reconnect loop that
follows looks exactly like a CSI bug. The shared component takes
`cfg.channel = 0` to mean "leave the radio where it is", and that is what this
example passes.

Related, and the reason `esp_wifi_connect()` is called explicitly from
`app_main()` rather than from the `WIFI_EVENT_STA_START` handler: that event
fires from inside `esp_wifi_start()`, which is before `esp_wifi_set_band_mode()`
may legally be called. Connecting there races the band selection.

## On forcing L-LTF

Traffic from a real AP arrives in whatever PHY its rate control picked, so
record length keeps changing — 490 bytes for HE20, 234 for HT40, 114 for HT20.
`Report L-LTF instead of HT/VHT/HE-LTF` pins it at 106.

It does something else too, which the ESP-IDF documentation does not mention:
on the C5, L-LTF defaults to **12-bit** packing, four bytes per tone rather than
two, so a 106-byte record is 26 tones and not 53. The shared component decodes
both and records which applied in the `packing` CSV column — but anything
downstream has to respect that column. See
[NOTES-C5-vs-C6.md](../NOTES-C5-vs-C6.md).
