# Technical

How DOOM got onto a hi-fi DAC, and how you can poke at it yourself.

| Page | What's in it |
|---|---|
| [Development](development.md) | **Start here.** Developing live over Wi-Fi/Ethernet: adb, the dev engine override, swapping in a patched UI without a flash, the UDP keyboard, screenshots, the host menu harness and the gotchas |
| [The device](device.md) | The K15's insides: X2000 + ESP32-S3 + DACs, the display, the OTA package format, how `mq_ui` is patched, the knob, the audio path, USB input |
| [Design notes](design.md) | The full design spec and test record: addresses, MD5s, decisions and every hardware test |

Building and its pipeline are in [building](../building.md#advanced).

## Repository layout

```text
build.sh, build.ps1         entry points (Linux/macOS/WSL, Windows): build the image, run tools/build-all.sh in it
docker/Dockerfile           doomfw-builder: pinned musl-cross-make toolchain + squashfs-tools, python3/PIL/numpy,
                            fonts-ubuntu, git, patch
engine/prepare.sh           pinned upstream export + patches
engine/build.sh             compile doomk15 + k15-mqsend
engine/patches/doomgeneric/ 0001 widescreen Hor+, 0002 K15 integration (config dir, save/load, quit/error hooks,
                            per-IWAD saves), 0003 typed-char hook (WASD + cheats), 0004 full view only
engine/patches/chocolate-doom/  OPL + MIDI without SDL
engine/src/                 K15 backend: platform (fb, evdev + hotplug, usbfs XInput), home menu, settings (k15.conf),
                            touch layouts, WAD scan, HUD, sound, icons
engine/tools/               port_opl.py (generates opl_k15.c), PC keyboard clients (udpkeys.py, k15_doom_keys.py),
                            harness/ (host renderer for the home menu)
launcher/                   k15-doom-launch, k15-mqsend.c
dev/                        k15dev.py: adb shell/push/pull, screenshots, dev-engine deploy (not part of the build)
docs/                       these docs (images in docs/.assets/)
VERSION                     release version shown in the DOOM menu (1.0.666)
tools/                      build-all.sh (the pipeline, in the container), rootfs-stage.sh, patch_mq_ui.py,
                            patch_json.py, make_icon.py, bootanim.py, treediff.py, pkg_build.py, pkg_verify.py
rootfs-overlay/NOTICE       shipped as /usr/share/doc/doom-fw/NOTICE
assets/doom-logo.svg        the DOOM wordmark (public-domain text logo) used for icons, menu and boot animation
assets/readme-logo.svg      the same wordmark with a gradient fill, for the repository README header
```

## License and credits

GPL-2.0-or-later (see [COPYING](../../COPYING)).

- [doomgeneric](https://github.com/ozkl/doomgeneric) (GPL-2.0) and [Chocolate Doom](https://www.chocolate-doom.org/) (GPL-2.0-or-later) are fetched at pinned commits and patched, not vendored.
- The 8×8 menu font is [dhepper/font8x8](https://github.com/dhepper/font8x8) (public domain).
- The DOOM wordmark in `assets/doom-logo.svg` is a public-domain text logo. "DOOM" is a trademark of id Software / ZeniMax.
- The `demons` boot animation caption is set at build time in Ubuntu Sans (Ubuntu Font Licence, from the `fonts-ubuntu` package, not vendored).
- FiiO's firmware is FiiO's: never redistribute the built zip, only this source.
