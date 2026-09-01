# 01_sniffer

The smallest ESP32-C5 program that produces CSI. One board, no AP, no
association, no second device.

It puts the radio in promiscuous mode on a single channel and prints a CSV line
for every CSI record the hardware hands up, on 2.4 GHz or 5 GHz. Use it to prove
that your board, your ESP-IDF and your `CONFIG_ESP_WIFI_CSI_ENABLED` are all
working before you build anything on top.

## Run it

```bash
idf.py set-target esp32c5 menuconfig
```

Set **CSI sniffer configuration → Channel to park on**. The default is 36, a
non-DFS 5 GHz channel that most dual-band APs use out of the box. Pick 1, 6 or
11 if the traffic you care about is on 2.4 GHz.

```bash
idf.py -p PORT flash monitor
```

## What you see

A CSV line per record, plus a summary every five seconds:

```
I (12345) sniffer: ch 36 | 412 records | 0 dropped | 3 invalid CE
I (12345) sniffer:     11g/11a   118
I (12345) sniffer:     11n       201
I (12345) sniffer:     11ax-SU    93
```

That per-format breakdown is the useful part. It tells you what the neighbours
are actually transmitting, and therefore what shape your records are: 11ax-SU
means 490-byte HE20 records with 245 sub-carriers, 11n means 114 bytes, legacy
means 106.

`invalid CE` counts records where `rx_ctrl.rx_channel_estimate_info_vld` was 0,
which the Wi-Fi guide says means the CSI data is not valid. A few is normal.
Mostly-invalid means the signal is too weak to estimate the channel from.

## It is passive, and that is the catch

This example never transmits. If nobody else is transmitting on the channel you
picked, you get nothing, and nothing is wrong. Two ways out:

- Run `05_band_sweep` first to find a channel that actually carries traffic.
- Or use `02_espnow_pair` / `03_router`, which generate their own.

The `dump_ack_en` option is on by default here on the theory that 802.11 ACKs
are short, frequent and sent at a legacy rate, so on a busy channel they should
be a dense and very regular CSI source. Be aware that this is inference rather
than documented behaviour: the entire published description of that field is the
seven words in its own header comment, and both of Espressif's own CSI examples
set it `false`. Try it both ways and watch the record count.

A second reason a busy channel can look empty, and this one is a hard limit: the
C5 can only **receive** 20 MHz outside 11n. Frames sent on 80 or 160 MHz — the
norm for 5 GHz 11ax — are not received at all. See
[NOTES-C5-vs-C6.md](../NOTES-C5-vs-C6.md).

## Worth trying

- **Turn on "Report L-LTF instead of HT/VHT/HE-LTF".** Every record becomes 106
  bytes regardless of what the transmitter used, which is the fixed shape most
  downstream processing wants. Watch the `packing` column flip from `iq8` to
  `lltf12` when you do: on the C5, L-LTF also switches to 12-bit packing, four
  bytes per tone, so 106 bytes is 26 tones rather than 53. ESP-IDF does not
  document that; the shared component handles it.
- **Compare a 2.4 GHz and a 5 GHz channel.** Same room, same board, and 5 GHz
  usually shows fewer transmitters but more 11ax.
- **Turn off "Print one CSV line per record"** to watch the summary alone while
  you move the board around.
