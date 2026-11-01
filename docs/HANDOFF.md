# Reverbicon 224: handoff (2026-10-31)

State of the port, how to work on it, what is open, and what has to be settled before a public release.

## Where it stands
A Lexicon 224 (v4.4) style reverb as an MPC effect: cineolX's native 224 core inside `src/engine.cpp`, six programs, 16 presets,
the 224's controls plus Dirt / Input / Mix, a live headroom meter, a skin drawn after the original remote, and an **on-device ROM
import** (the user drops ROM1 to ROM5 in `Content/ROMs/`; a helper builds the program bank in the background and the display shows
progress). Built for armv7 (Force, Gen 1) and aarch64 (Live III, Gen 2).

| Device | Installed | Checked |
|---|---|---|
| MPC Live III (aarch64) | installed on the internal drive, plugin folder `sd88me - VST - Reverbicon 224/` | loads, sound, skin, presets, Q-Links, project reload, ROM import (337 s, bank identical to the PC's), meter shown moving, bench PASS (mean 5.1%, p99 5.3%) |
| Force (armv7, MPC 3.9.1) | `/sdcard/Synths/sd88me - VST - Reverbicon 224/`, entry in `pluginList-arm` (backup `MPC.settings.bak-reverbicon`) | loads as an insert, skin renders (screenshot), BANK READY, ROM import from `Content/ROMs/` (1020 s, bank identical), bench WARN (mean 14.5%, p99 19.4%) |

## Layout of the code
- `src/engine.cpp`: the `mpc_engine()` glue. Per block: input peaks for the meter, 44.1 -> 48 kHz (`RationalFilter<160,147>`), cineolX
  `Engine48` (48 kHz), 48 -> 44.1, flush-to-zero for the duration (`ScopedFTZ`: denormals made it 5x slower).
  One-time setup (`init_engine`) runs in `create()` / the display poll, **never in the first `process()`** (it was a 202% block on the Force).
  `poll()` (called from the wrapper's `display_rev` read, ~every 100 ms, 400 ms internal) drives the import: counts `Content/ROMs/*` files of
  2048 bytes, starts `Content/bankhelper-<arch>` with `posix_spawn` (LD_PRELOAD stripped, stdio to /dev/null), follows
  `Content/.import-progress` (`RUN|DONE|ERR <pct> <text>`), loads the bank the moment it is published, and writes the status text.
- `tools/make_bank.cpp`: the helper. Locks `.import-progress.lock` (a second copy exits 3), `nice(10)`, publishes the bank via temp file + rename.
  `tools/build_helpers.sh` builds it for x86_64 / armv7 / aarch64 (static libstdc++) from cineolX's importer + the pinned Reflexion.
- `vst/`: `vst.json`, `params.json` (never reorder: `params.lock`), `presets.json`, `layout.conf`, `art/make_art.py` (all artwork is generated;
  inputs are cineolX's key / fader-cap sprites fetched by `tools/fetch_cineolx.sh`).
- `test/import_test.cpp`: the whole import flow against the x86 helper (no ROMs, wrong ROMs, real ROMs, bank identical to a reference,
  meter thresholds). Needs your ROMs; about 3 minutes.

## Build, test, deploy (what worked)
```
tools/fetch_cineolx.sh && tools/build_helpers.sh && python3 vst/art/make_art.py
MPC_VST=<fresh origin/main worktree of mpc-vst-plugins> vst/build.sh     # not the shared checkout: it can be stale
<mpc-vst-plugins>/tools/test_port.sh vst/vst.json                       # x86 ASan host test; also `... aarch64`
<mpc-vst-plugins>/tools/bench.sh vst/build/aarch64/reverbicon224.so <ip>  # put the bank in /tmp/Content first, or it measures passthrough
```
Device: stage in a temp folder, then swap only the skin, the `.so`, and the helpers; **keep `Content/`** (ROMs and the built bank).
A changed skin needs an MPC restart (`systemctl restart acvs`); ask the user first and close any project using the plugin.
`MPC.settings` (Force): stop MPC, back up, edit whole `<PLUGIN>` elements, keep CRLF, validate as XML, start MPC; a broken list stops MPC from booting.

## Open items
1. ~~Force ROM import~~ **done 2026-11-01:** bank removed, new instance inserted, the helper started by itself (low priority), progress ran 0 to 100% in the display, finished in **1020 s (17 min)**, bank md5 identical to the PC's, status ended at BANK READY with no reload; the touch screen stayed responsive throughout (a key press lit within a second at ~15% progress). Note MPC dims and blurs the screen after ~15 minutes idle: a tap wakes it.
2. **Audio and meter on the Force.** The test track's synth was silent, so the meter and the sound were not checked there. Re-test with a loaded source.
3. **Meter refresh cost.** Every change in lit LEDs bumps `display_rev`, so the host redraws up to ~10 times a second. Check fader drags stay smooth
   on both devices while the meter moves. If not, the fallback is a coarser meter or `DISPLAY_REV_NO_UPDATE` with the status line drawn as pictures.
4. **CPU on the Force.** WARN (p99 19%): fine for one or two instances. Options: fewer taps in the host-rate converter pair, NEON flags in
   `cflags_arm`, skipping the converters' work when the input is silent. Any change to the 48 kHz core needs the bit-exact check below.
5. **Bit-exactness on ARM.** Only the bank was compared (identical). Render the same input through the x86 and ARM builds and compare;
   the build uses `-ffp-contract=off` to keep the core's strict-FP behaviour.
6. **Algorithm switches are hard cuts** (the core resets its delay memory). cineolX has a "Spillover" tail crossfade (two networks, ~2x CPU) that is not ported.
7. **Status line is small** (13 px, "BANK READY"). Make it larger or give errors more room.
8. **Preset menu name.** After a project reload the PRESET menu shows the first preset's name though the sound is restored (wrapper behaviour for presets).
9. **No state chunk** (the host test warns): the project stores the 15 parameter values only; check save/reload with changed values on the Force.
10. **Not ported:** the 22 224XL programs (cineolX's XL is still a preview), the "Dirt" circuit modelling beyond the one control, quick presets.
11. **Installer ROM step.** `release/user-files.json` declares the five ROM chips for PR #668's desktop step (extension, exactly 2048 bytes, an 8-byte per-chip start,
    marked `unverified` because only the plugin can confirm the v4.4 chip by checksum). `test/user_files_check.py` passes against the real ROMs with the PR's `user_files.py`;
    the Go side was not run here (no Go toolchain). Re-check when #668 merges, and try a real upload to a device.
12. The uid `Cn24` was kept from the earlier name "Cineol 224". Projects saved under that name will not find the plugin.

## Before a public release
- **Permission and licences.** cineolX (engine, artwork) and Reflexion (its reference emulator, 8080 cores with their own notices) carry no licence. Nothing of
  theirs is in this repo (the fetch script pulls it); a release zip would contain code built from them (`bankhelper-*`, the engine) and art derived from
  cineolX's sprites. Get the authors' written OK first, and agree the credit line.
- **ROMs.** Never ship ROMs or the bank. The release is ROM-free; the README and the in-plugin status text tell the user what to supply and where.
  Say plainly that the user must own the originals.
- **Name.** "Reverbicon 224" is the chosen name; keep "Lexicon" only as a factual reference (the README does).
- **Release recipe** (mpc-vst-plugins): `release.py` with `--repo sd88me/mpc-vst-reverbicon224 --license MIT --user-data Content/ROMs --user-files release/user-files.json`
  (the installer's "Your ROMs and files" step; mpc-vst-plugins PR #668, **open, not merged, not yet run against a device**) and the helpers shipped as extras in `Content/`, then `catalog_check.py --catalog`, a draft GitHub release for testing, and a `catalog/plugins/<id>.json` PR.
  The release must contain both `.so` files and both helpers, executable.
- **Re-run before tagging:** host test (x86 + aarch64), `import_test`, bench on both devices, a fresh install from the zip, import on the Force, project save/reload.
- Screenshots for the README (crop the plugin area from a full screenshot, as the other ports do) and a short "how to add ROMs" section for users.
