#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Content-diff two extracted rootfs trees (run as root so owners are real).

  treediff.py <stock-dir> <new-dir> [--allow <relpath> ...]

Compares type, mode, uid/gid, size, content (MD5) and symlink targets of every entry.
Prints one line per difference: "A path" (added), "D path" (deleted), "M path (what)".
With --allow, exits 1 if anything other than the allowed paths changed, or if an allowed
path did not change. Directory mtimes/sizes are ignored (squashfs layout, not content).
"""
import hashlib
import os
import stat
import sys


def scan(root):
    out = {}
    for d, dirs, files in os.walk(root):
        for n in dirs + files:
            p = os.path.join(d, n)
            rel = os.path.relpath(p, root)
            st = os.lstat(p)
            info = {"type": stat.S_IFMT(st.st_mode), "mode": stat.S_IMODE(st.st_mode),
                    "owner": (st.st_uid, st.st_gid)}
            if stat.S_ISLNK(st.st_mode):
                info["target"] = os.readlink(p)
            elif stat.S_ISREG(st.st_mode):
                info["size"] = st.st_size
                h = hashlib.md5()
                with open(p, "rb") as f:
                    for blk in iter(lambda: f.read(1 << 20), b""):
                        h.update(blk)
                info["md5"] = h.hexdigest()
            out[rel] = info
    return out


def main():
    args = sys.argv[1:]
    allow = None
    if "--allow" in args:
        i = args.index("--allow")
        allow = set(args[i + 1:])
        args = args[:i]
    a, b = scan(args[0]), scan(args[1])
    changed = {}
    for rel in sorted(set(a) | set(b)):
        if rel not in b:
            changed[rel] = "D"
        elif rel not in a:
            changed[rel] = "A"
        else:
            diffs = [k for k in ("type", "mode", "owner", "target", "size", "md5")
                     if a[rel].get(k) != b[rel].get(k)]
            if diffs:
                changed[rel] = "M (" + ",".join(diffs) + ")"
    for rel, what in changed.items():
        print(what[0], rel, what[1:].strip())
    print(f"{len(a)} stock entries, {len(b)} new entries, {len(changed)} changed")
    if allow is None:
        return 0
    bad = sorted(set(changed) - allow)
    missing = sorted(allow - set(changed))
    for rel in bad:
        print("UNEXPECTED", rel)
    for rel in missing:
        print("NOT CHANGED", rel)
    print("CONTENT-DIFF:", "OK" if not bad and not missing else "FAIL")
    return 1 if bad or missing else 0


if __name__ == "__main__":
    sys.exit(main())
