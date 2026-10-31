#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Tiny fake WADs (just a header and lump directory) for the menu harness: enough for
# k15_wad.c to classify them like the real thing.
import struct
import sys


def wad(path, kind, lumps, pad=0):
    dir_ofs = 12 + pad  # sparse gap: realistic sizes without writing them
    d = b"".join(struct.pack("<ii8s", 12, 0, n.encode()) for n in lumps)
    with open(path, "wb") as f:
        f.write(kind + struct.pack("<ii", len(lumps), dir_ofs))
        f.seek(dir_ofs)
        f.write(d)


KNOWN = {
    "doom2.wad": (b"IWAD", ["PLAYPAL", "MAP01", "MAP02"], 14 << 20),
    "doom1.wad": (b"IWAD", ["PLAYPAL", "E1M1"], 4 << 20),
    "plutonia.wad": (b"IWAD", ["PLAYPAL", "MAP01"], 17 << 20),
    "freedoom1.wad": (b"IWAD", ["PLAYPAL", "E1M1", "E2M1", "E4M1", "FREEDOOM"], 27 << 20),
    "scythe.wad": (b"PWAD", ["MAP01", "MAP02"], 2 << 20),
}

# fakewads.py <dir> [name.wad ...]  (default: doom2, doom1, plutonia, scythe)
out = sys.argv[1]
for name in sys.argv[2:] or ["doom2.wad", "doom1.wad", "plutonia.wad", "scythe.wad"]:
    wad(f"{out}/{name}", *KNOWN[name])
