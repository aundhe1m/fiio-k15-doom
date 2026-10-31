# FIIO K15 DOOM EDITION

**Custom Firmware builder for running DOOM on K15**

[![Watch: powering on the K15 and starting DOOM](https://img.youtube.com/vi/agoL8goGUv4/maxresdefault.jpg)](https://www.youtube.com/watch?v=agoL8goGUv4)


The FiiO K15 is a very nice desktop DAC. It has a dual-core MIPS chip running Linux and a 1020×240 touchscreen. It plays music beautifully. It does not, out of the box, play DOOM.

We fixed that.

This repository turns the official FiiO K15 v159 update (which you download yourself) into the same firmware **plus DOOM**. Flash it from a USB stick, tap the new DOOM tile, rip and tear. Your DAC still does DAC things in between.

## Features

- **A DOOM tile** in the source grid, and a **DOOM entry** in the volume-knob input carousel.
- **True widescreen**: the full 1020-pixel-wide screen, with a HUD made from the WAD's own art.
- **Sound effects and OPL music** through the K15's own DAC. Yes, the AK4497s. For DOOM. We have not A/B-tested the shotgun against a $500 cable.
- **Touch controls** with four layouts, plus **USB keyboards and gamepads** (XInput too), hot-pluggable.
- **A WAD manager**: play WADs from a USB stick or install them on the K15. DOOM, DOOM II, Final DOOM, [Freedoom](https://freedoom.github.io/), add-on PWADs.
- **A cheat menu**, because nobody has time to type `iddqd` on a touchscreen.
- **A DOOM boot animation** (pick one of four, or keep FiiO's).
- **Bring your own firmware and WAD**: no FiiO code and no game data in this repository. Cables sold separately; the BFG is built in.

## TL;DR

You need Docker, FiiO's official v159 zip and a FAT32/exFAT USB stick:

```sh
./build.sh ~/Downloads/K15_update_20261012_v159.zip   # Windows: .\build.ps1 <zip>
# copy out/K15_update_*_v160.zip (plus a .wad) to the root of the stick
# K15: Settings → Others → FW update → SOC local upgrade
```

Then tap the DOOM tile. The [getting started guide](docs/README.md) has the details.

## Get going

- **[Getting started](docs/README.md)**: from the official firmware to DOOM in a few steps.
- **[Technical](docs/technical/README.md)**: how it works, and how we developed it live over Wi-Fi.
- **[FAQ](docs/faq.md)**: "why?", "will it break?" and friends.

> ⚠️ No warranty, not affiliated with FiiO or id Software. Read [SECURITY.md](SECURITY.md) before you flash.
