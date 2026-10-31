# Development: live over Wi-Fi

The whole project was developed and debugged on a running K15 over the network, with very few flashes. A reboot without a flash is 100 % back to what's in flash, so you can experiment a lot before you ever build a zip. Here's how.

## The way in: adb on port 5555

The stock K15 firmware runs `adbd` on **TCP port 5555** and gives a **root shell** to anyone who connects, no password (yes, really; see [SECURITY.md](../../SECURITY.md)). This project doesn't add it and doesn't remove it. Both firmwares have it.

Find the K15's IP address in your router (or *Settings → Network* on the K15), then either:

**Android's adb:**

```sh
adb connect 192.168.1.50:5555
adb shell
```

**or [`dev/k15dev.py`](../../dev/k15dev.py)**, a small pure-Python client with some extras:

```sh
pip install -r dev/requirements.txt          # adb-shell, Pillow
export K15_HOST=192.168.1.50                 # PowerShell: $env:K15_HOST="192.168.1.50"
python dev/k15dev.py shell "cat /etc/homebrew_version"
python dev/k15dev.py push local.file /usr/data/doom/
python dev/k15dev.py pull /usr/data/doom/doom.log .
python dev/k15dev.py shot screen.png         # screenshot, see below
python dev/k15dev.py deploy                  # dev engine, see below
python dev/k15dev.py undeploy
```

It uses the same key as Android's adb (`~/.android/adbkey`, created if missing; `ADB_KEY` points elsewhere). The first connection may need to be accepted on the K15.

**Where to write:** the rootfs is read-only squashfs. The only writable, persistent place is `/usr/data` (about 50 MB free). Use `/usr/data/doom/` for DOOM things and `/tmp` (RAM) for throw-away stuff.

## The fast loop: the dev engine override

If `/usr/data/doom/doomk15` exists and is executable, the launcher runs it instead of the firmware's `/usr/bin/doomk15`. It logs `DEV BUILD` in `launch.log`, and the DOOM menu shows a yellow **DEV BUILD** tag, so you always know which one you're looking at.

```sh
./build.sh --engine-only                     # ~1 minute: build/out/doomk15
python dev/k15dev.py deploy                  # -> /usr/data/doom/doomk15
# tap the DOOM tile on the K15
python dev/k15dev.py undeploy                # back to the firmware's engine
```

> ⚠️ **Quit DOOM before you deploy.** When a game ends, DOOM re-executes `/proc/self/exe` to go back to its menu, and that is still the *old* binary. `deploy` checks `pidof doomk15` and refuses while DOOM runs. Do the same check if you push by hand.

You can also start DOOM without touching the screen:

```sh
python dev/k15dev.py shell "setsid /usr/bin/k15-doom-launch </dev/null >/dev/null 2>&1 & sleep 1; pidof doomk15"
```

The launcher switches the input to Local Play like a tap would, and back on exit. `doomk15 -iwad <wad>` skips the menu (handy for engine tests; it doesn't switch inputs, so use it on Local Play or Streaming if you want sound). After a `kill -9`, `doomk15 --restore` pans the display back to the stock UI.

## Detaching processes: `setsid … </dev/null >log 2>&1 &`

Anything you start over adb dies with the adb session unless you detach it properly:

```sh
setsid /usr/data/mything </dev/null >/usr/data/mything.log 2>&1 & sleep 1
```

- `setsid` gives it its own session, so adbd and the `mq_ui` watchdog leave it alone.
- `</dev/null >log 2>&1` cuts it loose from adb's pipes.
- The trailing **`sleep 1`** matters: without it, adbd often tears down the process group before `setsid` even runs, and nothing starts.

The launcher does the same: `mq_ui` starts it with `system("… &")`, and it re-runs itself under `setsid`.

## Swapping in a patched `mq_ui` (no flash, no reboot)

The DOOM tile and the knob-carousel entry are patches to FiiO's UI binary `mq_ui` ([`tools/patch_mq_ui.py`](../../tools/patch_mq_ui.py), explained in [design](design.md#1-launch-hook-patched-mq_ui--proven-on-hardware)). You can try a new patch on a running K15 with a bind mount, in RAM only:

```sh
# on your PC. A full build leaves the reassembled stock rootfs in build/work/img/rootfs.squashfs;
# get the stock files out of it (unsquashfs is in the doomfw-builder image):
unsquashfs -d stock build/work/img/rootfs.squashfs usr/bin/mq_ui usr/project/config/ui/right_input_mode.json
cp stock/usr/bin/mq_ui stock/mq_ui.orig
python3 tools/patch_mq_ui.py stock/mq_ui.orig mq_ui.test --cmd "/usr/data/doom/launch.sh </dev/null >/dev/null 2>&1 &"
python3 tools/patch_json.py stock               # edits stock/.../right_input_mode.json in place

# copy mq_ui.test and right_input_mode.json to /usr/data/doom/, then on the K15:
mount -o bind /usr/data/doom/mq_ui.test /usr/bin/mq_ui
mount -o bind /usr/data/doom/right_input_mode.json /usr/project/config/ui/right_input_mode.json
kill $(pidof mq_ui)
```

`/usr/project/fiio_init.sh` is a watchdog: it notices `mq_ui` is gone and restarts the UI (and the player) within ~5–10 seconds. **No reboot.** The new UI runs your patched binary. A reboot drops the bind mounts and you're back to what's in flash.

Notes:
- `patch_mq_ui.py` refuses anything but the stock v159 `mq_ui`, and refuses to patch twice. On the DOOM firmware, start from the stock binary again.
- A running, bind-mounted binary can't be unmounted ("busy"). Stack another bind mount on top and kill `mq_ui` again.
- `--cmd` (max 79 characters) is the command the hook runs. Point it at a script in `/usr/data` that runs [`launcher/k15-doom-launch`](../../launcher/k15-doom-launch) with `K15_MQSEND=<path to k15-mqsend>`, and you can test the launcher too.
- The knob hook needs no JSON change. The source tile needs the patched `right_input_mode.json` (16 items instead of 15). Its DOOM icon path, `/usr/project/config/img/input_setting/doom.png`, only exists on the DOOM firmware; on stock, edit the JSON to point at an icon in `/usr/data` (LVGL's `D:` drive is `/`).
- **Never kill `mq_player`.** The UI is fine, the player isn't: killing it reboots the box.

## Driving DOOM from your PC: the UDP keyboard

While DOOM runs, it listens for keys on **UDP port 6666** (2-byte packets: pressed, DOOM key code). Two clients in [`engine/tools/`](../../engine/tools/):

- [`k15_doom_keys.py`](../../engine/tools/k15_doom_keys.py): a small window. While it has focus, your PC keyboard plays DOOM on the K15. Needs tkinter (Windows Python or WSL work).

  ```sh
  python engine/tools/k15_doom_keys.py 192.168.1.50
  ```

- [`udpkeys.py`](../../engine/tools/udpkeys.py): scripted key presses, great for driving the menus from a test script. Keys: `esc enter up down left right fire use tab`, or any single character.

  ```sh
  K15_HOST=192.168.1.50 python engine/tools/udpkeys.py down down enter
  ```

## Screenshots

```sh
python dev/k15dev.py shot screen.png
```

The panel is 240×1020 portrait, mounted sideways, and `fb0` holds three pages. `shot` reads which page is panned in (`/sys/class/graphics/fb0/pan`), copies it, pulls it and rotates it to a 1020×240 landscape PNG. Page 0 (the stock UI) is a plain `dd` of `/dev/fb0`. DOOM's pages 1–2 can't be read that way (a `read()` of `fb0` stops after page 0), so for those `shot` takes the framebuffer's physical address from DOOM's `fb0` mapping in `/proc/<pid>/maps` and reads the page from `/dev/mem`, read-only. It works for the stock UI and for DOOM. Every screenshot in these docs was taken this way (unless the caption says otherwise).

## The menu without a K15: the host harness

```sh
engine/tools/harness/run.sh /tmp/menu "d d e" "K15_DEFAULT_WAD=doom2.wad"
```

[`engine/tools/harness/`](../../engine/tools/harness/) compiles the DOOM home menu with your PC's gcc against fake platform functions and sparse fake WADs, replays a script and writes every frame as a PNG. Script tokens: `u d l r` (arrows), `e` (enter), `b` (back), `n`, `t=X:Y` (tap), `p=X:Y` / `R` (press and hold / release), `k=CODE` (key code). Extra arguments are `k15.conf` lines. `HARNESS_KBD=1` / `HARNESS_PAD=1` pretend a keyboard / pad is connected. `HARNESS_WADS="doom2.wad scythe.wad"` picks the installed WADs (`none` for none) and `HARNESS_USB_WADS="..."` puts WADs on a fake USB stick (names from `fakewads.py`), for example to check each PLAY state:

```sh
HARNESS_WADS=none HARNESS_USB_WADS=freedoom1.wad engine/tools/harness/run.sh /tmp/menu "t=530:65"
```

Great for iterating on menu layout without a device.

## Useful places on the K15

| Path | What |
|---|---|
| `/usr/data/doom/launch.log`, `doom.log` | DOOM's logs |
| `/usr/data/doom/k15.conf`, `default.cfg`, `savegame/` | settings and saves |
| `/usr/data/fiio/log/fiio_ui.log` | the UI's log, including every message it sends to the player (`[UI SEND]`) |
| `/usr/data/fiio/log/fiio_player.log` | the player's log (input switches, USB sticks, updates) |
| `/usr/data/fiio/log/process_failed.txt` | watchdog restarts of `mq_ui` / `mq_player` |
| `/etc/homebrew_version` | which DOOM firmware is installed |
| `/proc/bus/input/devices` | touch, front keys, IR, and any USB keyboard/pad |

## Gotchas

- **Deploying while DOOM runs.** The old binary gets re-executed; see [the warning above](#the-fast-loop-the-dev-engine-override).
- **adbd cuts off quiet commands.** A command that prints nothing can have its process group killed early. End it with something that prints (`; echo done`, `wc -c`, …), or detach it properly.
- **adb shells time out.** Long foreground commands get killed after a while, and their children with them. Detach anything long-running, and keep "undo" steps in the *same* short command as the change.
- **The device's gdb has a charset bug.** `gdb` exists on the K15, but `set charset` makes it abort and leaves the target process **stopped**. If the UI freezes after a gdb session, `kill -CONT <pid>` it. For a read-only peek at a running process, `dd if=/proc/<pid>/mem` with `skip=<address>` works without stopping it.
- **busybox has no `timeout`.** Background the command and `kill` it.
- **Don't `mq_receive` on `/player` or `/ui`.** Those POSIX message queues connect the UI and the player, and a third reader steals messages. Sending the same messages the UI sends is fine (that's how the launcher switches inputs).
- **`qemu-mipsel` and bare file names:** `qemu-mipsel-static -cpu P5600 ./mq_ui` works, `… mq_ui` (without `./`) silently exits 1 and looks like a broken binary.

## Safety rules

Two things you must never do, because they can lock you out:

- **Never create `/usr/data/disableadb`.** The adb init script refuses to start if it exists, and adb is your way back in.
- **Never ship a package with `recovery_version` > 4.** The recovery partition is your safety net. `pkg_build.py` refuses it; don't work around that.

Also: never kill `mq_player`, don't write to `/dev/mtd*` by hand, and don't run FiiO's `/etc/ota_bin/*.sh` scripts from the main OS. Everything above lives in RAM or `/usr/data` and is undone by a reboot or `rm`.
