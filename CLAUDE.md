# CLAUDE.md

Guidance for Claude Code working in this repository.

## What this is

Five ESP-IDF example projects that capture Wi-Fi Channel State Information on
an **ESP32-C5**, one shared component, `components/csi_c5`, that hides what is
specific to this chip, and [NOTES-C5-vs-C6.md](NOTES-C5-vs-C6.md), which
records what is specific and where each fact was read.

Two standing facts decide how to work here. The README's "Accuracy" section
states both, and is the place to change them:

- **It builds.** CI compiles every project for `esp32c5`.
- **It has never been run.** Nothing here has met a radio. Compiling is not
  evidence that the parsing is right or that any example produces data.

So a change is checked by building it, and a statement about what happens on
hardware is one nobody has tested. Do not let sample output read like a
recording, and do not remove "never been run" unless someone has run it and
the README says on what.

## The rule this repository exists for

**Do not write ESP32-C5 CSI code from memory, from an ESP32-C6 example, or from
the ESP-IDF CSI documentation.** All three are wrong for this chip:

- The C5 is `SOC_WIFI_MAC_VERSION_NUM` 3 and the C6 is 2. Under the same type
  names they have different `wifi_csi_acquire_config_t` and `rx_ctrl` layouts,
  which is why code written for the C6 or the classic ESP32 does not compile
  unchanged here.
- ESP-IDF's documentation describes one buffer packing and applies it to every
  record. On the C5 the L-LTF record defaults to 12-bit packing, four bytes per
  tone. Read as byte pairs it fails silently: it parses cleanly and plots
  plausibly.

Read [NOTES-C5-vs-C6.md](NOTES-C5-vs-C6.md) before touching the component or
an example. A new API fact is read from the ESP-IDF sources, not recalled, and
its source goes into the "Sources" list at the end of that file. Where upstream
documents nothing — `val_scale_cfg` is the standing case — say so and leave the
default; do not invent an explanation.

## Layout

| path | what it is |
|---|---|
| `01_sniffer`, `03_router`, `04_motion`, `05_band_sweep` | one standalone ESP-IDF project each, with its own `README.md` |
| `02_espnow_pair/tx`, `02_espnow_pair/rx` | two projects, one per board |
| `components/csi_c5` | start and stop, parking on a channel, the record type, readers for both packings, the CSV printer |
| `.github/workflows/build.yml` | every project against pinned ESP-IDF tags, on push and pull request |
| `.github/workflows/drift.yml` | every project against moving ESP-IDF refs, weekly |

Each project finds the component through `EXTRA_COMPONENT_DIRS` in its own
`CMakeLists.txt`. `02_espnow_pair/tx` deliberately has none: it only transmits,
and pointing at `components/` would make ESP-IDF build `csi_c5` for it anyway,
because component discovery goes by directory and not by `REQUIRES`.

## Building

ESP-IDF **v5.5.2 or newer**: that is the floor the README claims, with its
reasons, and the floor CI builds against. From inside one project directory:

    idf.py set-target esp32c5
    idf.py build

Without ESP-IDF at hand, CI is the build: `build.yml` runs on every push to
`main` and on every pull request, and can be started by hand. Do not describe
a change as building until one of the two has built it.

For the projects that use the component, a green build also proves CSI is
enabled, because `csi_c5.h` has an `#error` on `!CONFIG_ESP_WIFI_CSI_ENABLED`.
Keep that guard and the other three at the top of the header: each stands for
a way this code would otherwise misbehave quietly instead of failing.

## Adding or changing an example

- A new project is a numbered directory with `CMakeLists.txt`,
  `sdkconfig.defaults`, `main/` with a `Kconfig.projbuild`, and a `README.md`
  shaped like the others: what it is, how to run it, what to expect.
- `sdkconfig.defaults` carries `CONFIG_IDF_TARGET="esp32c5"` and, in every
  project that measures CSI, `CONFIG_ESP_WIFI_CSI_ENABLED=y`. The transmitter
  in `02_espnow_pair/tx` leaves it out on purpose.
- Never commit an `sdkconfig`: it silently overrides the defaults for the next
  person, which is the usual way CSI ends up switched off.
- **Add the project to the matrix in both workflows.** A project in neither is
  one nothing builds, and the badge stays green.
- Add it to the table in the README.
- Read tones through `csi_c5_amplitude()`, `csi_c5_phase()` and
  `csi_c5_first_subcarrier()`. Indexing `buf` directly is how the packing
  mistake gets back in.
- The call order after `esp_wifi_start()` is fixed, and the README's "Nothing
  appears" list gives it. `esp_wifi_set_band_mode()` in particular fails before
  Wi-Fi is started.

## Style

C, with the two SPDX lines at the top of every source file
(`SPDX-FileCopyrightText: 2026 scwan`, `SPDX-License-Identifier: Apache-2.0`)
followed by a comment saying what the file is for. Comments carry the reason
and sit beside the line that has the trap; that is most of the value of this
tree, so match it.

Line endings are LF everywhere, pinned by `.gitattributes`, which also records
why: a search-and-replace anchored on `$` skipped every CRLF file, and so did
the search written to check it. After any tree-wide edit, confirm it reached
every file by some means other than the same pattern.

## Committing

Straight to `main`. Subjects are a plain sentence about what changed, such as
"Add a weekly drift build against moving ESP-IDF refs".

**This repository is public.** Only its own work belongs in it: nothing from
another project, and no host names, addresses, paths on someone's machine or
credentials, in code, comments, documentation or commit messages.

## Gotchas

- **`CONFIG_ESP_WIFI_CSI_ENABLED` off gives a working build that delivers no
  callbacks and reports no error.** The component's `#error` is the only thing
  that says so.
- **A stale `sdkconfig` wins over `sdkconfig.defaults`.** Delete it after
  changing the defaults.
- **The C5 receives 20 MHz only, except in 11n.** Traffic on 80 or 160 MHz is
  not received at all, so a busy 5 GHz channel can look silent. That is a
  hardware limit, not a bug to fix here.
- **Enabling CSI forces HE-SIG-B dumping off**, whatever menuconfig says.
- **Out of the box only 5 GHz channels 36 to 60 are permitted.** Higher ones
  need `esp_wifi_set_country()` with the manual policy.
- **`first_word_invalid` means four bytes, not four values**: two tones under
  one packing, one under the other.
- **The protocol bitmap is per band** and has to be set again after a band
  change.
- **`build.yml` pins immutable tags, `drift.yml` uses moving refs, and they
  answer different questions.** A red drift run can be upstream's doing; a red
  build run is this repository's.
- **The CI action is pinned to a tag on purpose.** Its `v1` is a branch.
- **GitHub disables a scheduled workflow after 60 days without repository
  activity.** If the drift run stops, that is why.
