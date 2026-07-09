![Chimera](docs/banner.png)

# Chimera

CV-addressable breakbeat slicer for the Expert Sleepers **Disting NT** — one beast stitched from two breaks. Inspired by [amen](https://norns.community/amen/) (norns) and zeptocore/[ectocore](https://getectocore.com/).

**[Read the manual](https://visti.github.io/disting-chimera/)**

## What it does

- Loads one or two WAV loops (**Lion** and **Goat**) into DRAM, slices each into 4–32 pieces on an equal grid or snapped to transients
- Fires slices from CV (linear or 1V/oct select), triggers, clock, random pulses, ratchet gates, or MIDI notes
- Rolls effect dice per slice event, amen style: reverse, pitch up/down, stutter, stretch, gate, filter sweep, dub-delay send — each a 0–100 % probability, all scaled by the **Break** macro
- Per-loop pre-FX trims (level, varispeed rate, semitone pitch, bipolar filter) to match unlike breaks
- **Blend** intermingles the heads by chance or crossfade; **Quarrel** lets them fight over the fader; **Serpent** is the tail — a clock-chasing dub echo
- Clock sync via granular stretch or repitch, tempo parsed from `name_<bpm>.wav` filenames
- Octatrack-style slice editor: zoom, nudge, snap to onset or zero crossing, lock slices to always play straight
- SP-1200-calibrated crush and soft-clip drive on the master bus

## Building

Requires the [distingNT_API](https://github.com/expertsleepersltd/distingNT_API) and the ARM GNU toolchain (`arm-none-eabi-c++`).

```sh
make                    # builds plugins/chimera.o
make check              # host-side syntax check (clang++, no ARM toolchain needed)
```

`NT_API_PATH` defaults to `../distingNT_API`; override on the command line if it lives elsewhere.

## Installing

Copy `plugins/chimera.o` to the SD card under `/programs/plug-ins/` and add the algorithm on the NT. The **Max length** specification (1–32 s per loop) sets the DRAM reserved when you add it.
