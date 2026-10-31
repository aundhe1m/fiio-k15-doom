#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Developer helper for a K15 on your network: root shell, file transfer, screenshots and
the DOOM dev-binary override, over the adb (TCP 5555) the stock firmware exposes.

  k15dev.py shell ["<command>"]          run a command (default: id)
  k15dev.py push <local> <remote>        copy a file to the K15
  k15dev.py pull <remote> <local>        copy a file from the K15
  k15dev.py shot [out.png]               screenshot of what the screen shows (landscape)
  k15dev.py deploy [doomk15]             install a dev engine as /usr/data/doom/doomk15
                                         (default: build/out/doomk15 from ./build.sh --engine-only)
  k15dev.py undeploy                     remove the dev engine (back to /usr/bin/doomk15)

Env: K15_HOST (required, the K15's IP address), K15_PORT (default 5555),
     ADB_KEY (default ~/.android/adbkey, the same key Android's adb uses; created if missing).
Needs: pip install -r dev/requirements.txt  (adb-shell, Pillow)

Everything here touches only RAM (/tmp) and /usr/data/doom; a reboot without the dev engine
is stock. Plain `adb connect <ip>:5555` + `adb shell/push/pull` works as well.
"""
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DOOM_DIR = "/usr/data/doom"
DEV_ENGINE = f"{DOOM_DIR}/doomk15"


def connect():
    try:
        from adb_shell.adb_device import AdbDeviceTcp
        from adb_shell.auth.keygen import keygen
        from adb_shell.auth.sign_pythonrsa import PythonRSASigner
    except ImportError:
        sys.exit("missing adb-shell: pip install -r dev/requirements.txt")
    host = os.environ.get("K15_HOST")
    if not host:
        sys.exit("set K15_HOST to the K15's IP address")
    key = Path(os.environ.get("ADB_KEY", Path.home() / ".android" / "adbkey"))
    if not key.exists():
        key.parent.mkdir(parents=True, exist_ok=True)
        keygen(str(key))
    signer = PythonRSASigner(Path(str(key) + ".pub").read_text(), key.read_text())
    dev = AdbDeviceTcp(host, int(os.environ.get("K15_PORT", "5555")),
                       default_transport_timeout_s=10)
    dev.connect(rsa_keys=[signer], auth_timeout_s=30)
    return dev


def sh(dev, cmd, timeout=60):
    return dev.shell(cmd, timeout_s=timeout, read_timeout_s=timeout)


def shot(dev, out):
    try:
        from PIL import Image
    except ImportError:
        sys.exit("missing Pillow: pip install -r dev/requirements.txt")
    # The panel is 240x1020 portrait with several buffers stacked in fb0; copy the one
    # that is panned in (sysfs "pan" = xoffset,yoffset).
    info = sh(dev, "cd /sys/class/graphics/fb0 && cat pan stride bits_per_pixel modes").split()
    yoff = int(info[0].split(",")[1])
    stride, bpp = int(info[1]), int(info[2])
    w, h = (int(v) for v in info[3].split(":")[1].split("p")[0].split("x"))
    if bpp != 32:
        sys.exit(f"unexpected framebuffer depth {bpp}")
    want = stride * h
    skip = 0
    if yoff == 0:
        # The K15's adbd can end a silent command's process group early: finish with output.
        size = sh(dev, f"dd if=/dev/fb0 of=/tmp/k15shot.raw bs={stride} count={h} "
                       "2>/dev/null; wc -c < /tmp/k15shot.raw").strip()
    else:
        # A read() of /dev/fb0 only reaches page 0 (DOOM draws on pages 1-2), and
        # /proc/<pid>/mem can't read a framebuffer mapping. The offset column of DOOM's fb0
        # mapping is the framebuffer's physical address, so read the page from /dev/mem
        # (read-only), in whole 4 KB blocks.
        pid = sh(dev, "pidof doomk15").split()
        if not pid:
            sys.exit(f"fb0 is panned to y={yoff} but DOOM isn't running")
        maps = sh(dev, f"grep /dev/fb0 /proc/{pid[0]}/maps").split()
        if len(maps) < 3:
            sys.exit("no fb0 mapping found in DOOM")
        addr = int(maps[2], 16) + yoff * stride
        skip = addr % 4096
        count = (skip + want + 4095) // 4096
        size = sh(dev, f"dd if=/dev/mem of=/tmp/k15shot.raw bs=4096 "
                       f"skip={addr // 4096} count={count} 2>/dev/null; "
                       "wc -c < /tmp/k15shot.raw").strip()
        want = count * 4096
    if size != str(want):
        sys.exit(f"framebuffer copy is {size} bytes, expected {want}")
    raw = Path(str(out) + ".raw")
    try:
        dev.pull("/tmp/k15shot.raw", str(raw))
        sh(dev, "rm -f /tmp/k15shot.raw; echo ok")
        data = raw.read_bytes()[skip:skip + stride * h]
        img = Image.frombytes("RGB", (w, h), data, "raw", "BGRX", stride)
        img.transpose(Image.Transpose.ROTATE_90).save(out)
    finally:
        raw.unlink(missing_ok=True)
    print(out)


def deploy(dev, local):
    if not Path(local).is_file():
        sys.exit(f"{local} not found: run ./build.sh --engine-only first")
    # A running DOOM re-execs /proc/self/exe when a game ends, i.e. the old binary.
    if sh(dev, "pidof doomk15").strip():
        sys.exit("DOOM is running on the K15: quit it first")
    sh(dev, f"mkdir -p {DOOM_DIR}")
    dev.push(local, f"{DEV_ENGINE}.new")
    sh(dev, f"chmod 755 {DEV_ENGINE}.new && mv -f {DEV_ENGINE}.new {DEV_ENGINE}")
    print(f"deployed {local} -> {DEV_ENGINE} (the DOOM menu now shows DEV BUILD)")


def main():
    a = sys.argv[1:]
    if not a or a[0] not in ("shell", "push", "pull", "shot", "deploy", "undeploy"):
        sys.exit(__doc__)
    cmd, args = a[0], a[1:]
    if cmd in ("push", "pull") and len(args) != 2:
        sys.exit(f"usage: k15dev.py {cmd} <from> <to>")
    dev = connect()
    try:
        if cmd == "shell":
            sys.stdout.write(sh(dev, " ".join(args) or "id"))
        elif cmd == "push":
            dev.push(args[0], args[1])
            print("pushed", args[0], "->", args[1])
        elif cmd == "pull":
            dev.pull(args[0], args[1])
            print("pulled", args[0], "->", args[1])
        elif cmd == "shot":
            shot(dev, args[0] if args else "k15shot.png")
        elif cmd == "deploy":
            deploy(dev, args[0] if args else str(HERE.parent / "build" / "out" / "doomk15"))
        elif cmd == "undeploy":
            sh(dev, f"rm -f {DEV_ENGINE}")
            print(f"removed {DEV_ENGINE}")
    finally:
        dev.close()


if __name__ == "__main__":
    main()
