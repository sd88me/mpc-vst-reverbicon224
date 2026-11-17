# Reverbicon 224: handoff (2026-11-17)

## Where it stands
The plugin runs the Lexicon 224's own v4.4 firmware on our emulator (`src/`, see README "How it works" and `docs/ENGINE-PLAN.md`).
cineolX's engine, its bank helper and its artwork are gone; nothing of cineolX is used or fetched any more. The repo is AGPL-3.0
because `src/third_party/reflecticon/` (reflecticon-224, AGPL-3.0) is included.

| Device | Checked |
|---|---|
| MPC Live III (aarch64) | installed from `tools/package_vst2.py`'s folder: boots READY, all six programs, sliders through the firmware, readouts and LED digits from the firmware's display, pre-delay over each program's range. CPU (test/render_test.c on core 4): mean 17.5%, p99 23%, worst 34% of a block. Output bit-identical to x86. |
| Force (armv7) | not tried. The armv7 build's output is identical to x86 (run in 32-bit mode on the Live III: 23%). Expect ~50% of a Force core. |

## Open items
1. **Force:** install, listen, measure (`test/render_test.c` cross-built for armv7; `tools/bench.sh` measures pass-through, see 2).
2. **bench.sh** drives blocks back to back, so it finishes before the firmware's 0.6 s boot and measures pass-through. Use render_test.
3. **CPU:** the 8080 and the analog chains are the rest of the cost beside the DSP (29 ns a row on the A76). Ideas: run the 8080 only
   when the DSP is about to need it, fewer input-chain steps between converter loads.
4. **Program change** cuts the tail and mutes ~50 ms (the firmware reloads the DSP), as the hardware does. A spillover would need a second machine.
5. **PRESET menu name** after a project reload shows the first preset's name (wrapper behaviour), though the sound is restored.
6. **The uid `Cn24`** is kept from the earlier name "Cineol 224".

## Build, test, deploy
```
python3 vst/art/make_art.py && MPC_VST=<mpc-vst-plugins> vst/build.sh && python3 tools/package_vst2.py
<mpc-vst-plugins>/tools/test_port.sh vst/vst.json [aarch64]
```
Device: stage the new files in a temp folder, swap the `.so` files (and `Plugin Skins/` for a skin change), **keep `Content/`** (the ROMs),
then restart MPC (ask first; it loses an unsaved project).

## Release recipe (mpc-vst-plugins)
`release.py --so <so> --skin "vst/build/skin/sd88me - VST - Reverbicon 224" --entry vst/build/pluginlist-entry.xml --version X.Y.Z
--repo sd88me/mpc-vst-reverbicon224 --license AGPL-3.0-only --user-data Content/ROMs --user-files release/user-files.json`, once per
`.so` (armv7 and aarch64), `tools/sdk_zip.py` for the `-mpc-sdk.zip`, all three on a draft release, then `catalog/plugins/reverbicon-224.json`.
Never ship ROMs.
