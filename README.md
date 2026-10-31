# Reverbicon 224

A Lexicon 224 (original, firmware v4.4) style reverb as a native effect plugin for Akai MPC OS standalone devices
(Force and MPC Live/One/X/Key on armv7, MPC Live III on aarch64). Six programs, the 224's bass / mid / crossover /
treble-decay / depth / pre-delay / diffusion controls, Dirt, Input and Mix faders, Mode Enhancement, Decay Optimisation,
and the four output pairs. The touch skin is drawn after the original remote: program keys with LEDs, a red LED display
with a live headroom meter, black-capped faders.

The engine is [cineolX](https://github.com/temaniak/cineolX)'s native 224 core (itself validated against
[Reflexion](https://github.com/joelanders/reflexion-224)); this repo is the MPC plugin around it.

## You supply the ROMs

The plugin needs the original **Lexicon 224 v4.4 ROM1 to ROM5** (2048-byte EPROM images). They are not included and
never will be. Put them in the plugin's `Content/ROMs/` folder: the plugin then builds its program bank by itself in the
background (a helper program, `bankhelper-<arch>`, a few minutes the first time; about 5.6 minutes on a Live III) and
shows progress in the display. `BANK READY` means it is done. The bank is cached next to the ROMs; delete it to rebuild.

## Build

Needs Docker, Python 3 with Pillow, and a checkout of
[mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins) next to this one (or `MPC_VST=/path`).

```
tools/fetch_cineolx.sh          # cineolX core + artwork at a pinned commit (not redistributed here)
tools/build_helpers.sh          # bankhelper-x86_64 / -armv7 / -aarch64
python3 vst/art/make_art.py     # skin artwork
vst/build.sh                    # skin, reverbicon224.so (armv7) and build/aarch64/reverbicon224.so, plugin-list entry
```

Offline tests: `vst/build.sh` output is checked by mpc-vst-plugins' `tools/test_port.sh vst/vst.json`;
`test/import_test.cpp` runs the whole ROM-import flow against the x86 helper (needs your ROMs).

## Install (Force, MPC 3.x)

1. Copy `vst/build/skin/sd88me - VST - Reverbicon 224/` to `/sdcard/Synths/` on the device, and put the matching
   `reverbicon224.so` (armv7 for Force and Gen 1, `build/aarch64/` for a Live III), the `bankhelper-<arch>` file and your
   ROMs (`Content/ROMs/`, with the helper in `Content/`) inside that folder.
2. With MPC stopped, add `vst/build/pluginlist-entry.xml` to the `pluginList-arm` list in `MPC.settings` and start MPC.
   Edit it as whole XML elements and keep the line endings: a broken list stops MPC from starting.

## Measured

| Device | Mean | p99 | Worst block |
|---|---|---|---|
| MPC Live III (aarch64) | 5.1% | 5.3% | 32% (first block) |
| Force (armv7) | 14.5% | 19.4% | 23% |

(% of one 128-frame block at 44.1 kHz, `tools/bench.sh` of mpc-vst-plugins.) The Force figure suits one or two instances.

## Third-party material (not included)

- **cineolX** (the engine and artwork) and **Reflexion** (its reference emulator) have no licence yet. `tools/fetch_cineolx.sh`
  downloads them for your own build; nothing from them is committed here. Ask their authors before redistributing a build.
- **Titillium Web** fonts in `vst/art/fonts/` are under the SIL Open Font License (`OFL.txt`).
- Lexicon and 224 are names used only to say what is emulated; this is not affiliated with or endorsed by Lexicon.

The files in this repository are MIT licensed (`LICENSE`).
