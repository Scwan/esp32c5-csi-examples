# 04_motion

`03_router` plus a detector, so the board reports *that something changed*
rather than dumping numbers for you to look at. Same stimulus: associate with
the AP, ping the gateway, take CSI from the echo replies.

It learns a per-sub-carrier amplitude baseline over the first few seconds, then
scores every subsequent record by how far it deviates from that baseline. A
person moving through the link changes multipath, multipath changes those
amplitudes, and the score goes up.

## Run it

```bash
idf.py set-target esp32c5 menuconfig
```

Set the SSID and password under **CSI motion detector configuration**.

```bash
idf.py -p PORT flash monitor
```

**Stay out of the path between the board and the AP for the first five
seconds.** That is the calibration window. Calibrate your own presence into the
baseline and the detector will report motion when you *leave*.

```
I (3200) csi_motion: calibrating over 250 records - keep still and out of the link
I (8300) csi_motion: baseline ready over 114 sub-carriers - watching
MOTION,251,0.0181,0,-43,11n
MOTION,252,0.0206,0,-43,11n
...
I (21400) csi_motion: motion  (score 0.212)
I (23900) csi_motion: still   (score 0.061)
```

The `MOTION` columns are: record number, score, state (0/1), RSSI, PHY.

## Tuning the threshold

The default of 15 percent is a starting point, not a setting. The score depends
on how far the board is from the AP, what is in the room, and which band you are
on, so it is not portable between installations — not even between two rooms in
the same flat.

Watch the score with the room empty, then with someone walking through, and set
the threshold between the two. There is hysteresis at 60 percent of the
threshold and a 1.5 second hold, so the state does not chatter.

## How it works, and what that costs

- **Baseline**: a running mean during calibration, then a slow EWMA
  (`BASELINE_ALPHA` 0.004) so it drifts with the room. Slow enough that a person
  walking through is not absorbed into "normal" within a second; not frozen, so
  moving a chair once does not leave the detector stuck on forever.
- **Score**: mean of `|amplitude - baseline| / (baseline + 1)` over the usable
  sub-carriers, then smoothed with an EWMA at 0.25.
- The `+ 1` in the denominator matters. The DC bin and the guard bins either
  side carry almost no energy, so their baseline sits near zero and a raw ratio
  there explodes. One unit of amplitude floors the ratio without affecting
  sub-carriers that carry real signal.
- Records whose shape does not match the baseline are **discarded, not
  compared**, and counted as `wrong-shape` in the summary. Comparing a 490-byte
  HE20 record against a 234-byte HT40 baseline element by element produces a
  large deviation that has nothing to do with the room — a false alarm nobody
  ever tracks down, because it looks exactly like a real one.
- Whichever shape arrives first after boot becomes the baseline shape, so the
  detector effectively locks onto whatever PHY the AP was using at that moment.
  If that turns out to be a rare one, `wrong-shape` will dominate; reboot.

## On record shape and forcing L-LTF

`Report L-LTF instead of HT/VHT/HE-LTF` would pin every record at 106 bytes and
drive `wrong-shape` to near zero, which sounds like exactly what a detector
comparing sub-carrier *k* against baseline *k* wants.

It is **off** by default anyway, for two reasons.

First, on the C5 forcing L-LTF also switches the buffer to 12-bit packing — four
bytes per tone instead of two, 26 tones instead of 53 — which ESP-IDF does not
document at all. The shared component decodes both correctly, so it is safe
here, but it is not the innocuous knob it looks like.

Second, Espressif recommend against it for this exact application:

> 234 subcarriers are sufficient to capture human body state features […] 8-bit
> precision is completely adequate for detecting sitting/standing/walking

If `wrong-shape` is high, the better fix is to make the link itself stable — an
11n 40 MHz connection gives a steady 234 tones — rather than to force L-LTF.

## What this is not

It is a threshold on a change statistic. It is not presence detection, not
localisation, and not a trained model. It will fire on the AP switching rate or
bandwidth, on a microwave oven, and on someone moving in the flat upstairs.

Read the score as "the channel changed" and keep the interpretation separate.
Everything past that — telling a person from a fan, counting people, working out
where they are — is a machine learning problem, and this example is the sensor
you would feed it, not the answer.
