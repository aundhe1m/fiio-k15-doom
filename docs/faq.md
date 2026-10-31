# FAQ

## The basics

### Why?

Why not?

### Can it run DOOM?

Yes. It's the law: everything with a screen must, and your DAC is now compliant. Crysis is a different question, and the answer is no.

### AI slop?

Yes, very much. Lots of AI went into the reverse engineering, the code and these docs. A human held the K15, pressed the buttons and said "no, not like that" a lot.

### Can I run it on a K17?

No idea. The builder only accepts the K15 v159 firmware and patches exact addresses in it. If you figure out a K17 (or a newer K15 firmware), fork it or open a PR.

### Is this hi-res audio?

Hi-res audio, 1993 edition. DOOM's sound effects are 8-bit samples at 11 kHz, and the K15 plays them through the same two AK4497S DACs as your FLACs. The DAC will reproduce every one of those 8 bits faithfully. It won't make the imps any prettier.

### Is it bit-perfect?

No. DOOM mixes the sound effects and emulates the OPL music itself before handing the result to the DAC path. It's OPL-perfect, which is a different religion. For bit-perfect playback, quit DOOM: the launcher switches back to your previous input.

### Does the DAC still work normally?

Yes. Apart from the two DOOM entries in the UI, everything is FiiO's v159 firmware, unchanged. We checked the USB DAC, Local Play, streaming, Bluetooth, EQ and the volume knob after flashing. While DOOM runs, the K15 is on Local Play (or Streaming) for the sound. When you quit, your previous input is switched back.

## Safety and support

### Will this break my unit?

Probably not, but no guarantees. The update uses FiiO's own USB updater, which checks every file before writing, and the recovery partition is never touched. We flashed it back and forth many times. Going back to stock is easy: [flash the official v159](restore.md). See [SECURITY.md](../SECURITY.md) for the full disclaimer.

### Is my K15 exposed on my network?

The stock firmware already runs a root adb shell on TCP port 5555, without a password. Anyone on your network can connect. This project neither adds nor removes it (we used it to develop, see [development](technical/development.md)).

One thing this project does add: **while DOOM runs**, it listens on **UDP port 6666** for key presses (the [UDP keyboard](technical/development.md#driving-doom-from-your-pc-the-udp-keyboard)). Anyone on your network can press keys in the game and the DOOM menu. Worst case, someone else plays your DOOM (or deletes an installed WAD). It's closed as soon as DOOM exits.

Keep the K15 on a network you trust. More in [SECURITY.md](../SECURITY.md).

### Does FiiO support this?

No. FiiO has nothing to do with this project. Flash the [official firmware](restore.md) before contacting their support.

## WADs and updates

### Where do I get a WAD?

The shareware `doom1.wad` (episode 1) is free and easy to find. The full games are on Steam and GOG: the `.wad` files are in the install folder. [Freedoom](https://freedoom.github.io/) is a free, complete replacement. More in [WADs](doom/wads.md).

### Will a FiiO update remove DOOM?

Yes. The DOOM firmware still reports v159, so the next official update (v160 or later) installs normally and replaces it. Your DOOM settings, saves and installed WADs stay in `/usr/data/doom/`, so they're back if you flash DOOM again.

### Can I share the built zip?

No. It's FiiO's firmware with DOOM added, and FiiO's firmware isn't ours to share. Share this repository instead: everyone builds their own from FiiO's official download.

## Troubleshooting

### Where are the logs?

On the K15, in `/usr/data/doom/`:

- `launch.log`: the launcher (input switching, which binary ran, how it exited).
- `doom.log`: the last DOOM run (the one before is `doom.log.1`). Switch on DEBUG LOG under [OPTIONS → FEATURES](doom/options.md#features) for more detail.

Read them over adb, see [development](technical/development.md).
