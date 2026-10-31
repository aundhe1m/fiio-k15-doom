#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Append the DOOM entry to stock v159 UI list definitions in an extracted rootfs.

  patch_json.py <rootfs-dir> [--others]

  right_input_mode.json  swipe-right source grid: 15 stock tiles -> 16th (free) cell "DOOM"
  other_set.json         Settings -> Others: 7 stock rows -> 8th row "DOOM" (only with
                         --others; needs the matching patch_mq_ui.py hook)

The list builder in mq_ui sizes everything from the JSON, so an extra item renders without
code changes; patch_mq_ui.py makes the extra index launch DOOM. Refuses anything but the
stock item counts (so it can't run twice). Output keeps the stock 4-space layout.
"""
import json
import os
import sys

UI = "usr/project/config/ui"
SOURCES = ("right_input_mode.json", 15, "D:/usr/project/config/img/input_setting/doom.png")
OTHERS = ("other_set.json", 7, "D:/usr/project/config/img/menu_setting/other/doom.png")
LABEL = "DOOM"
LANGS = ("zh", "en", "ja")


def patch(path, count, icon):
    raw = open(path, "rb").read()
    j = json.loads(raw)
    dump = lambda o: (json.dumps(o, indent=4, ensure_ascii=False) + "\n").encode()
    if dump(j) != raw:
        sys.exit(f"{path}: unexpected formatting, not the stock file")
    for lang in LANGS:
        items = j["lable"][lang]["obj_items"]
        if len(items) != count:
            sys.exit(f"{path}: {lang} has {len(items)} items, expected stock {count}")
        items.append(LABEL)
    if len(j["img"]) != count:
        sys.exit(f"{path}: img has {len(j['img'])} entries, expected stock {count}")
    j["img"].append(icon)
    open(path, "wb").write(dump(j))
    print(f"patched {path}: item {count} = {LABEL}, icon {icon}")


def main():
    args = sys.argv[1:]
    others = "--others" in args
    args = [a for a in args if a != "--others"]
    if len(args) != 1:
        sys.exit(__doc__)
    for name, count, icon in (SOURCES, OTHERS) if others else (SOURCES,):
        patch(os.path.join(args[0], UI, name), count, icon)


if __name__ == "__main__":
    main()
