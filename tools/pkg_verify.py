#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independently verify a K15 update zip the way the device consumes it.

1. Extract the zip fresh.
2. md5_file_info.txt: every listed file exists and matches (recovery's check_update_file_md5).
3. ota_config.in -> current_version N; main_os/ota_vN/{ota_vN.ok,ota_update.in} exist.
4. For each image in ota_update.in, emulate local/network_*_update_main_os.sh download_ota_img:
   walk <img>.NNNN.<pre_md5> starting at pre_md5=img_md5, compare each chunk md5 to line i of
   ota_md5_<img>.<img_md5>, stop once total >= img_size; reassemble and check size + md5.
5. Compare reassembled images to expected reference files.

  pkg_verify.py <zip> <workdir> [<img_name>=<expected file> ...] [--extract <dir>]

--extract writes each reassembled, verified image to <dir>/<img_name> (used by the DOOM
Firmware Builder to take the stock kernel/rootfs from the official zip).
"""
import hashlib, os, shutil, sys, zipfile

args = sys.argv[1:]
extract = None
if "--extract" in args:
    i = args.index("--extract")
    extract = args[i + 1]
    del args[i:i + 2]
zpath, work = args[0], args[1]
refs = dict(a.split("=", 1) for a in args[2:])  # img_name=path/to/expected.bin
md5 = lambda b: hashlib.md5(b).hexdigest()
fail = 0

shutil.rmtree(work, ignore_errors=True)
zipfile.ZipFile(zpath).extractall(work)

listed = set()
for line in open(os.path.join(work, "md5_file_info.txt")):
    h, rel = line.rstrip("\n").split("  ", 1)
    listed.add(rel)
    p = os.path.join(work, rel)
    if not os.path.isfile(p) or md5(open(p, "rb").read()) != h:
        print("MD5_FILE_INFO MISMATCH", rel); fail += 1
actual = {os.path.relpath(os.path.join(r, f), work) for r, _, fs in os.walk(work) for f in fs}
unlisted = actual - listed - {"md5_file_info.txt"}
if unlisted:
    print("UNLISTED FILES", sorted(unlisted)); fail += 1
print(f"md5_file_info.txt: {len(listed)} entries checked")

cfg = dict(l.split("=", 1) for l in open(os.path.join(work, "ota_config.in")).read().split())
ver = cfg["current_version"]
d = os.path.join(work, "main_os", f"ota_v{ver}")
assert os.path.isfile(os.path.join(d, f"ota_v{ver}.ok")), "missing .ok"
print(f"ota_config.in: current_version={ver} recovery_version={cfg['recovery_version']}")

imgs, cur = [], None
for line in open(os.path.join(d, "ota_update.in")):
    k, _, v = line.strip().partition("=")
    if k == "img_type":
        cur = {"type": v}; imgs.append(cur)
    elif cur is not None and k in ("img_name", "img_size", "img_md5"):
        cur[k] = v
    elif k == "ota_version" and v != ver:
        print("ota_version mismatch", v, ver); fail += 1

for im in imgs:
    name, size, imd5 = im["img_name"], int(im["img_size"]), im["img_md5"]
    sums = open(os.path.join(d, f"ota_md5_{name}.{imd5}")).read().split("\n")
    pre, total, i, out = imd5, 0, 0, bytearray()
    while True:
        src = os.path.join(d, f"{name}.{i:04d}.{pre}")
        if not os.path.isfile(src):
            print("CHAIN BROKEN at", os.path.basename(src)); fail += 1; break
        b = open(src, "rb").read()
        if not b or sums[i] != md5(b):
            print("CHUNK MD5 MISMATCH", i); fail += 1; break
        out += b; pre = md5(b); total += len(b); i += 1
        if total >= size:
            break
    ok = len(out) == size and md5(bytes(out)) == imd5
    ref = refs.get(name)
    refok = ref is None or md5(open(ref, "rb").read()) == imd5
    print(f"{im['type']:7} {name}: chunks={i} size={len(out)} md5={md5(bytes(out))} "
          f"self={'OK' if ok else 'BAD'} ref={'OK' if refok else 'MISMATCH'}")
    fail += (not ok) + (not refok)
    if extract and ok:
        os.makedirs(extract, exist_ok=True)
        with open(os.path.join(extract, name), "wb") as f:
            f.write(out)

print("RESULT:", "PASS" if fail == 0 else f"FAIL ({fail})")
sys.exit(1 if fail else 0)
