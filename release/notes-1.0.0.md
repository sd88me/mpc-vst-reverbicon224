# Reverbicon 224 1.0.0

The first release: the Lexicon 224's own v4.4 firmware running on an emulated 8080, DSP and converters, as a native MPC effect.

## What it is
- **The real firmware, not a model of its sound.** The 224's control computer runs the v4.4 ROMs and programs the DSP row by row;
  audio goes through models of its gain-ranging converters and its input and output boards. The emulator is checked against
  reflecticon-224, bit-exact where it can be.
- **The 224's front panel, worked for you.** Program keys load programs through the firmware, sliders move its pots, DIFFUSION is on
  its SHIFT page, Mode Enhancement and Decay Optimization are its PROG 7 and 8.
- **Its own display.** The red LED readout shows the last slider you moved as the 224 shows it ("2.6 SEC", "8.80 KHZ", "024 MS"), and
  the slider readouts use the same text.
- Six programs, the four output pairs, Dirt (our optional converter/analog character), Input and Mix, a headroom meter, sixteen presets,
  Q-Links on every slider.

## You need your own ROMs
Lexicon 224 **v4.4** ROM1 to ROM5 (2048 bytes each), from a 224 you own. They are not included. Put them in the plugin's `Content/ROMs/`
folder, or pick them in the desktop installer's "Your ROMs and files" step. The display shows `STARTING`, then the program name.

## Install
- `Reverbicon-224-1.0.0-mpc-sdk.zip`: copy the folder `sd88me - VST - Reverbicon 224` into a `VST2 Plugins` folder on a drive of the MPC
  (MPC OS 3.10.1 or later, with External Plugin Support on), and your ROMs into its `Content/ROMs/`.
- `Reverbicon-224-1.0.0-mpc-aarch64.zip` / `-mpc-armv7.zip`: run `install.sh` on the device as root, or use the Plugin Manager.

## Notes
- Tested on an MPC Live III: about 18% of one core (p99 23%). The Force and 32-bit MPCs build is checked offline only (its output is
  identical); expect about half a Force core.
- A program change mutes the reverb for about 50 ms while the firmware loads the DSP, as on the hardware.
- Licensed AGPL-3.0. Credits: reflecticon-224 by Joe Landers.
