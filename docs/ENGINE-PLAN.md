# Reverbicon 224: an engine of our own (investigation, 2026-11-16)

Why: the current engine (cineolX `native-hall/core`) and its ROM importer carry no licence (temaniak/cineolX#24, open, no reply).
Reflexion is now `joelanders/reflecticon-224`, **AGPL-3.0**, and runs the 224's own firmware, but on the Live III it costs about 53% mean /
105% peak of an A76 core (roadmap note), and the Force cannot run it.

## What the 224 actually is (from reflecticon-224's ISA-level model, `isa-level-cpp/lexicon224x.hpp`)
- An **8080 CPU** (the SBC) runs the firmware: the front panel / LARC, the programs, and it writes the DSP's microprogram.
- The **DSP** runs a 128-word **WCS** (writable control store) microprogram: **100 rows per sample at 34.13 kHz** (3.41 M rows/s). Each row is
  one microinstruction: a 6-bit coefficient (c/32), multiply-accumulate (add or subtract) into ACC, transfer to RR, a delay-memory read or
  write at an offset (16K words), and OPER rows that move ADC / XREG / RR onto the bus and write the DACs.
- The ARU is pipelined: a multiply loaded in one row finishes in the next. Converters: 12-bit DAC with gain ranging, 12-bit ADC.

## Measured here (v4.4 ROM1-5, `emulator/probe`, x86 desktop)
- **The firmware rewrites the WCS while it runs.** Three snapshots one second apart: the same 128 words except **two pairs** (rows 17-18 and
  66-67). Each pair is two adjacent delay taps whose coefficients sum to 32/32 and whose offsets creep by one: a **moving, linearly
  interpolated tap**, the 224's chorus/"random" modulation, driven by the 8080. Everything else stays put until a control moves.
- **The row-accurate DSP alone** (`isa-level-cpp/bench`, the stock program): **45.8 ns/row = 6.4x realtime** on the desktop, i.e. ~15% of a
  desktop core; on an A76 that is roughly 35-45%. This, not the 8080, is most of reflecticon's cost: it models the T&C's time slots and the
  ARU's three clocks per row, and the full emulator adds a tick scheduler and cycle-exact WCS access.
- The whole machine (`probe`, 8080 + DSP + scheduler): 14 s of machine time in 4 s, 3.5x realtime on the desktop.

## The plan
1. **Keep the firmware, drop the bank helper.** Run the real v4.4 firmware on a plain instruction-level 8080 (cycle-counted, interleaved
   with the DSP at its real clock ratio per sample). Its WCS writes go straight to our DSP at the next sample. The 8080 then drives the
   modulation, the programs and every control exactly as the hardware does, and the five ROMs load directly: **no bank build, no 5-17
   minute wait** (and no `bankhelper`, no cineolX importer).
2. **A fast DSP of our own.** Decode the 128 microinstructions once, whenever the 8080 writes the WCS, into a compact table; run the 100-row
   pass per sample as a tight loop that keeps the ARU's one-row multiply latency, ACC / RR / XREG, the delay memory and the converter rows
   (no time slots, no bus timing). Target **under 5 ns/row** (10x reflecticon's), about 5% of an A76 for the DSP.
3. **Converters and I/O:** the ADC / DAC with the 224's 12-bit gain ranging, then 34.13 kHz <-> the host rate (the resampler the other
   emulator ports use) with an anti-alias / reconstruction filter.
4. **Verify against reflecticon-224:** render the same input through both (its `static_run` event format already exists) for every stock
   program and require matching DAC codes for a static WCS; then a listening check with the modulation running.
5. **The plugin keeps everything else:** skin, presets, Q-Links, the six programs, the controls (they become the firmware's own controls:
   the LARC / panel inputs reflecticon already maps). Redraw the two cineolX sprites (keys, fader caps).

## Licence
AGPL-3.0 for the repo (done 2026-11-17), as the roadmap already plans: the code is ours, but we read reflecticon-224 (AGPL) closely and test against
it, and could reuse pieces of it directly (its 8080 interface, the panel / LARC mapping) with attribution. No ROMs, no bank, nothing of
cineolX's in the release.

## Estimate and risks
- **Cost:** 8080 a few % + DSP ~5% + resampling: roughly **10% of an A76** on the Live III (vs 53%); the Force (Cortex-A9) plausibly 25-35%:
  to be measured.
- **Fidelity risks:** the microinstruction displaced while the 8080 writes the WCS (a one-row no-op in hardware; we apply writes between
  samples: expected inaudible, to be compared); the exact converter / analog behaviour; the 224's input and output filtering.
- **Size of the job:** the 8080 core and the firmware I/O map (panel, LARC, timers, converter control) are the bulk; the DSP loop is small.
  A first sound (one program, static WCS, offline, bit-compared) is the first milestone; then the 8080 driving it live.
