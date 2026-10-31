# Getting started

From a stock K15 to DOOM in eight steps. Each step links to the page with the details.

**What you need:**

- A K15, and FiiO's official **v159** update zip. The builder only accepts v159.
- A computer with **Docker**.
- A **FAT32 or exFAT USB stick** with at least 250 MB free.
- A **WAD**, such as the free shareware `doom1.wad`. None is included (see [WADs](doom/wads.md)).
- About half an hour, most of it the first build.

1. **[Download the official firmware:](building.md#1-download-the-official-firmware)** FiiO's K15 **v159** update, `K15_update_20261012_v159.zip`.

2. **[Build:](building.md)** Install Docker and run one command. You get `out/K15_update_<date>_<time>_v160.zip`.

3. **[Copy to a USB stick:](flashing.md#1-prepare-the-usb-stick)** Put the firmware zip (not unzipped) and a WAD (for example the shareware `doom1.wad`) in the root of a FAT32 or exFAT stick.

4. **[Flash:](flashing.md#2-start-the-update)** Plug the stick into the back USB-A port, then *Settings → Others → FW update → SOC local upgrade*.

5. **[Wait for the reboot:](flashing.md#3-wait)** Don't pull the power. The K15 comes back with a DOOM boot animation.

6. **[Select the DOOM source:](doom/README.md#starting-doom)** Swipe right and tap the DOOM tile, or use the menu-knob carousel.

7. **[Press PLAY:](doom/wads.md)** Just one WAD on the stick? PLAY offers to install it. More than one? Pick one under WADS, and tap ☆ on an installed WAD to make it your favorite.

8. **[Rip and tear!](doom/README.md#how-to-play)**

Changed your mind? [Going back to stock](restore.md) is the same procedure with FiiO's own zip.

## All pages

- [Building](building.md): the simple way, then all the options.
- [Flashing](flashing.md): USB stick to DOOM boot animation.
- [Going back to stock](restore.md).
- [The DOOM launcher](doom/README.md): [WADs](doom/wads.md) and [options](doom/options.md).
- [FAQ](faq.md).
- [Technical](technical/README.md): development, the device, the design.
- [Contributing](../CONTRIBUTING.md) and [security / disclaimer](../SECURITY.md).
