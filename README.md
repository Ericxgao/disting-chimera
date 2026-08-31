![Chimera](docs/banner.png)

# Chimera

CV-addressable breakbeat slicer for the Expert Sleepers **Disting NT** — one beast stitched from two breaks. Inspired by [amen](https://norns.community/amen/) (norns) and zeptocore/[ectocore](https://getectocore.com/).

**[Read the manual](https://visti.github.io/disting-chimera/)**

## What it does

- Loads one or two WAV loops (**Lion** and **Goat**) into DRAM, slices each into 4–32 pieces on an equal grid or snapped to transients
- Fires slices from CV (linear or 1V/oct select, 0V = C0), triggers, clock or MIDI clock, random pulses, ratchet gates, or MIDI notes — plus a dedicated **MIDI trig**: pick a channel and note (Sequence page) and that note steps the sequencer exactly like a Trigger edge — set **MIDI channel** to **Off** if the chromatic note→slice mapping should stay out of the way on a shared controller
- Rolls effect dice per slice event, amen style: reverse, pitch up/down, stutter, stretch, gate, filter sweep, dub-delay send — each a 0–100 % probability, all scaled by the **Break** macro; **Backbeat** suppresses the dice on downbeats as it rises, so anchors stay steady while the offbeats stay chaotic
- Tag slices with a drum role (Kick/Snare/Perc/Hat/Crash) in the editor — feeds **Random mode: Role** and **Step mode: Pattern** (4 built-in grooves: Boom-bap, 4-floor, Two-step, Half-time), which reassemble a tagged break into that template
- Phrase helpers for steadier jungle: **Fill mode** heats up the dice at the end of a phrase, **Phrase reset** re-centres the sequencer on 1/2/4/8-bar cycles, **Ghost note** adds quiet tagged snare/perc/hat hits between sequenced steps, and **Ratchet roll** can hold a slice in repeats for two or three slice lengths
- **Beef**: layer a clean one-shot on top of any role-tagged slice, straight and untouched by the dice, with a duck that follows the one-shot's own envelope — and a per-role gate output (Kick/Snare/Perc/Hat/Crash) that fires whenever that role's one-shot plays, to trigger the rest of your rack
- Hands-on performance: hold **Button 1** to **Tame** the dice (all off) or go **Wild** (maximum chaos, with conflicting dice resolved), switchable via the Hold mode; hold **Button 2** to retrigger the current slice on the fly (a cable-free ratchet); tap **Button 3** to re-roll the Mask pattern; **Pot 3** rides **Blend** at all times
- **Players deck**: press **Button 4** to flip the encoders off Lion/Goat onto a **Sample Player** and a **Sample Player (Clocked)** (or **Chimera Looper**, below) found in the preset by name — so one page performs the breaks and the players together. Same encoder grammar on both decks: turn to browse without loading, quick-push to load (or roll a random sample), hold the push ~¾ s to flip that encoder between Sample and Folder (the line flips to `/folder` while you're still holding)
- Per-loop pre-FX trims (level, varispeed rate, semitone pitch, bipolar filter) to match unlike breaks
- **Blend** intermingles the heads by chance or crossfade; **Quarrel** lets them fight over the fader; **Serpent** is the tail — a clock-chasing dub echo
- Clock sync via granular stretch or repitch, tempo parsed from `name_<bpm>.wav` filenames — **Clock source: MIDI tempo** locks the tempo to incoming MIDI clock without letting it step the sequencer, for playing chimera by hand (MIDI trig, notes, CV) inside a clocked rig
- Octatrack-style slice editor (hold **Button 3** ~¾ s): zoom, nudge, snap to onset or zero crossing, lock slices to always play straight, preview a slice straight from the editor, tag roles
- Per-slice low-pass gate (vactrol-style), SP-1200-calibrated crush, and soft-clip drive on the master bus

## Chimera Looper

A companion plug-in (`plugins/looper.o`) — a repitch-only stand-in for the firmware's **Sample Player (Clocked)** that plays entirely from RAM instead of streaming from the MicroSD card, so chimera's WAV swaps (or any other bulk card read) can't stutter it. New samples read into a back buffer while the current loop keeps playing, and swap in exactly on the next loop boundary. Tempo comes from a CV clock (with a Clock div) or MIDI clock; the loop length auto-fits to the nearest power-of-two bar count (or is forced via **Fit**), and **Auto trigger: Loop** keeps it retriggering drift-free on clock edges. One specification, **Max length**, sizes the two 16-bit stereo buffers (~0.4 MB/s each). Chimera's players deck picks it up by name in place of the factory clocked player.

## Building

Requires the [distingNT_API](https://github.com/expertsleepersltd/distingNT_API) and the ARM GNU toolchain (`arm-none-eabi-c++`).

```sh
make                    # builds plugins/chimera.o
make check              # host-side syntax check (clang++, no ARM toolchain needed)
```

`NT_API_PATH` defaults to `../distingNT_API`; override on the command line if it lives elsewhere.

## Installing

Copy `plugins/chimera.o` to the SD card under `/programs/plug-ins/` and add the algorithm on the NT. Two specifications set the DRAM reserved when you add it: **Max length** (1–32 s per loop) and **Beef length** (1–8 s per one-shot slot).
