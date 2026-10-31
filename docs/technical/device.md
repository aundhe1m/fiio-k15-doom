# The device

What we learned about the K15's insides while getting DOOM onto it. Everything here was found on a v159 unit, mostly read-only. Addresses refer to the stock v159 binaries.

## More computers than a DAC needs

Three parts matter for this project:

| Part | What it does |
|---|---|
| **Ingenic X2000** (2× XBurst2 MIPS32r2 @ ~1.2 GHz, 128 MB RAM) | Runs **Linux 4.4** (kernel `4.4.94`): the touchscreen UI (`mq_ui`), the player (`mq_player`), streaming, network, USB sticks. **DOOM runs here.** |
| **ESP32-S3** | The front-panel and housekeeping MCU: volume knob, DSP/PEQ routing, DAC/XMOS/Bluetooth chores. Talks to the X2000 over SPI. The rear "RS232" USB-C port is its serial console (a CH343 bridge, 57600 8N1) |
| **2× AKM AK4497S** | The DAC. Fed by the X2000 (Local Play, streaming), the XMOS USB receiver, optical/coax, or Bluetooth |

The box also holds an XMOS XU316 (USB audio input), a Qualcomm QCC5125 (Bluetooth input) and a Broadcom Wi-Fi module. DOOM doesn't touch those.

DOOM only touches the X2000 side. No MCU firmware is changed, and no MCU command is sent directly (the input switch goes through the stock player, like a tap would).

## Linux side

- **Flash layout:** NAND with u-boot, a main kernel + rootfs, a recovery kernel + rootfs, an OTA state partition, and a writable **UBIFS `/usr/data`** (~67 MB, ~50 MB free). The rootfs is **read-only squashfs** (LZO, 128 KB blocks). Everything persistent that isn't firmware lives in `/usr/data`.
- **Init:** `/etc/init.d/S98FIIO` starts `/usr/project/fiio_init.sh`, a watchdog that runs `mq_ui` and `mq_player` and restarts both if either dies. Killing `mq_ui` is harmless (the UI is back in seconds); killing `mq_player` reboots the box.
- **adb:** `S90adb` starts a root `adbd` on USB and TCP 5555, unless `/usr/data/disableadb` exists. See [development](development.md).
- **UI ↔ player:** two POSIX message queues, `/player` (UI → player) and `/ui` (player → UI), with ASCII messages like `0657000C0008` ("set input 8"). `mq_ui` logs everything it sends in `/usr/data/fiio/log/fiio_ui.log`. The launcher's `k15-mqsend` sends the same input-switch message the UI sends.

## Display

- The panel is **240×1020 portrait, mounted 90° counter-clockwise**, so the UI is 1020×240 landscape. `/dev/fb0` is 32 bpp (BGRA) with a virtual height of **3 pages**.
- `mq_ui` only ever draws page 0 and never pans. DOOM draws into **pages 1 and 2** and page-flips with `FBIOPAN_DISPLAY`. The stock UI keeps running underneath, and is visible again the instant DOOM pans back to page 0 (on every exit path, including crashes; `doomk15 --restore` does it by hand).
- The hardware overlay layers are a dead end: layer 0 (the UI) has the top z-order, and changing that means rewriting the whole frame config.
- DOOM renders **1020×200 "Hor+" widescreen** (the vertical view matches vanilla, the horizontal field of view is ~145°) and scales it ×1.2 vertically to keep DOOM's tall pixels. The status bar is replaced by a translucent HUD drawn from the WAD's own graphics.

## Firmware updates (the OTA package)

There is **no signature** in the main-OS update chain, only MD5s, which we can regenerate. That's why this project is possible at all.

- *Settings → Others → FW update → SOC local upgrade*: `mq_player` finds `K15_update_…v<N>.zip` in the stick's root, checks the version, unzips it to `K15_update_images/` on the stick and reboots into **recovery**.
- **Recovery** checks every line of `md5_file_info.txt`, reads `ota_config.in` (`current_version`, `recovery_version`) and writes each image through a chunk chain: 1 MB chunks named `<img>.<i>.<md5 of chunk i-1>`, each checked against a list.
- The main OS is only ever written from recovery, and recovery only from the main OS. The recovery update is skipped when the package's `recovery_version` isn't higher than the installed one (4). We always ship 4.

Package layout (the builder mirrors the official zip exactly):

```text
ota_config.in                              current_version=160, recovery_version=4
md5_file_info.txt                          MD5 of every other file
main_os/ota_v160/ota_update.in             image list, sizes, MD5s
main_os/ota_v160/ota_v160.ok               empty marker
main_os/ota_v160/ota_md5_xImage.<md5>      + xImage.000N.<chain>        (stock kernel)
main_os/ota_v160/ota_md5_rootfs.squashfs.<md5> + rootfs.squashfs.00NN.<chain>  (ours)
recovery/ota_v4/...                        copied verbatim, skipped on the device
```

The rootfs must be repacked **as root** (setuid busybox, non-root owners), with `mksquashfs -comp lzo -b 131072 -noappend -no-xattrs`. The builder does it in Docker.

**Boot animation:** `/usr/project/config/img/start_up/0.jpg … 96.jpg`, 1024×240 baseline JPEG. `mq_ui` runs an LVGL animation 0→96 over exactly 3000 ms and shows frames 0–95 (~32 fps); 96 is never shown. It's time-based, so slow decodes drop frames. Decoding is LVGL's TJpgDec at ~10 ms + 0.27 ms per KB, so frames are kept under 32 KB.

## Patching `mq_ui`

The DOOM tile and the knob-carousel entry are binary patches to `mq_ui` (MIPS, no source, no decompiler, just radare2 and objdump). The approach:

1. **Find the list builders.** The UI builds its lists from JSON files in `/usr/project/config/ui/`. The builder sizes everything from the JSON, so an extra item renders without code changes. The source grid (`right_input_mode.json`) has 15 items in an 8×2 grid, so a 16th fills the free cell.
2. **Find the click handlers.** They do `if (idx < 15) jump_table[idx]()`, so the extra item is silently ignored. We patch the branch to jump into a small **code cave** that checks `idx == 15` and calls `system("/usr/bin/k15-doom-launch … &")`.
3. **Keep the tile enabled.** Row-2 tiles are grayed out and made unclickable unless Local Play is selected; two more branches keep tile 15 always enabled.
4. **The knob carousel** shows 8 inputs with `% 8` hard-wired in a dozen places. Instead of patching each, every path that touches the highlighted index is **redirected** into new code that counts modulo 9 and launches DOOM when index 8 is committed (knob push, tap or the 3 s timeout). The old 208-byte cave was full, so the binary's spare `PT_NULL` program header became a new executable `PT_LOAD` segment at `0x8c0000`.
5. **Be paranoid.** The patcher checks the stock MD5, asserts the original bytes at every site and that the caves are empty, so it refuses anything but stock v159 and refuses to patch twice.

The full address-level description is in [design §1](design.md#1-launch-hook-patched-mq_ui--proven-on-hardware). Tip: try patches with a bind mount first ([development](development.md#swapping-in-a-patched-mq_ui-no-flash-no-reboot)).

## Knob and front panel

- The front panel is mostly the MCU's, but `x2000_key` (`/dev/input/event1`) gets a few key codes:
  - **249**: the menu button (opens the input carousel), and the knob push while the carousel is open (commit);
  - **251 / 252**: knob clockwise / counter-clockwise while the carousel is open;
  - **254**: a knob turn in normal mode, with no direction;
  - **258**: a knob push in normal mode (screen off/on).
- **The volume itself goes through the MCU**, so the volume knob keeps working while DOOM has grabbed all inputs. In DOOM the knob push is USE.
- The **IR remote** (`gpio_ir_recv`) sends no key events at all, only `MSC_SCAN` codes (NEC address `0xBEBB`, commands `0x00`–`0x0F`), because the kernel keymap doesn't match FiiO's remote. Mapping those is an open item.
- Touch is `axs_ts` (`event2`), multitouch protocol B in portrait coordinates. `EVIOCGRAB` on all inputs keeps DOOM's taps away from the UI underneath.

## Audio

- One ALSA card with five playback PCMs (`hw:0,0…4`) behind a crossbar. The stock player plays on **`hw:0,4`, S24_3LE, 44.1 kHz, stereo**.
- Writing there is **silent until GPIO PC28 is high**. PC28 is the SoC → DAC path enable; the player raises it whenever it plays. DOOM sets it while it runs and restores it after.
- That path only reaches the DAC on the **Local Play** and **Streaming** inputs, hence the launcher's input switch. A **paused** song keeps `hw:0,4` open (`state: RUNNING`), so the launcher flips between the two SoC inputs to make the player close it.
- Don't touch crossbar routes under a running stream: it can stall the stock player until it reopens the PCM.
- DOOM mixes sound effects and Chocolate Doom's OPL music emulation itself and pipes the result to `aplay`. Video costs ~33 % of one core, ~48 % with music.

## USB input

- **Only the back USB-A port is a USB host** (`usb1`, high speed). The two USB-C ports aren't (one is the USB DAC input, the other the MCU serial port). Stick + keyboard + pad at once needs a hub (untested).
- Keyboards are plain `usbhid`; `mdev` creates `/dev/input/eventN` on hotplug. The stock software ignores them completely.
- **XInput pads have no kernel driver** on the K15. DOOM drives them from userspace through usbfs: find the `ff/5d/01` interface in sysfs, claim it, send the LED packet `01 03 02` (the pad switches to "connected"), and read 20-byte reports with async interrupt URBs in the same `poll()` loop as evdev.
- **Classify input devices by capabilities**, not by node or interface number: a keyboard can be `event3` wired and `event4` via its dongle, and one wired keyboard exposes a HID collection that advertises every multitouch axis. Touch = `INPUT_PROP_DIRECT` or `BTN_TOUCH`; pad = `BTN_GAMEPAD`/`BTN_JOYSTICK`; the rest are keys.
- Tested: 8BitDo Ultimate 2 Wireless (XInput, wired), Keychron V1 Max (wired and via its 2.4 GHz dongle).
