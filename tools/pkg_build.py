#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a K15 homebrew update zip in FiiO's official U-disk OTA layout.

Layout (zip root), mirroring K15_update_*_v159.zip:
  ota_config.in                       current_version=<NEW>, recovery_version=4
  md5_file_info.txt                   "<md5>  <relpath>" for every other file
  main_os/ota_v<NEW>/ota_update.in    kernel (stock, unchanged) + rootfs (homebrew)
  main_os/ota_v<NEW>/ota_v<NEW>.ok    empty marker
  main_os/ota_v<NEW>/ota_md5_<img>.<img_md5>   one chunk-md5 per line
  main_os/ota_v<NEW>/<img>.<NNNN>.<md5 of previous chunk; chunk0 -> img_md5>
  recovery/ota_v4/...                 copied byte-for-byte from stock (skipped by device:
                                      installed recovery_version 4 >= 4)
"""
import argparse, datetime, hashlib, os, shutil, zipfile

CHUNK = 1024 * 1024
md5 = lambda b: hashlib.md5(b).hexdigest()


def md5_file(p):
    h = hashlib.md5()
    with open(p, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def chunk_image(img_path, name, outdir):
    data = open(img_path, "rb").read()
    img_md5 = md5(data)
    prev, sums = img_md5, []
    for i in range(0, len(data), CHUNK):
        part = data[i:i + CHUNK]
        with open(os.path.join(outdir, f"{name}.{i // CHUNK:04d}.{prev}"), "wb") as f:
            f.write(part)
        prev = md5(part)
        sums.append(prev)
    with open(os.path.join(outdir, f"ota_md5_{name}.{img_md5}"), "w") as f:
        f.write("".join(s + "\n" for s in sums))
    return len(data), img_md5


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stock", required=True, help="extracted official update zip")
    ap.add_argument("--stock-ver", type=int, default=159)
    ap.add_argument("--rootfs", required=True, help="rootfs.squashfs to ship")
    ap.add_argument("--version", type=int, default=160)
    ap.add_argument("--out", default="out")
    ap.add_argument("--zipname", help="default: K15_update_<today UTC>_v<version>.zip")
    a = ap.parse_args()
    if not a.zipname:
        today = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d")
        a.zipname = f"K15_update_{today}_v{a.version}.zip"

    tree = os.path.join(a.out, "tree")
    shutil.rmtree(tree, ignore_errors=True)
    mdir = os.path.join(tree, "main_os", f"ota_v{a.version}")
    os.makedirs(mdir)

    # Stock kernel: copy its chunks + md5 list verbatim.
    sdir = os.path.join(a.stock, "main_os", f"ota_v{a.stock_ver}")
    kinfo = None
    for fn in os.listdir(sdir):
        if fn.startswith("xImage.") or fn.startswith("ota_md5_xImage."):
            shutil.copy2(os.path.join(sdir, fn), mdir)
    stock_upd = open(os.path.join(sdir, "ota_update.in")).read()
    blocks = [b for b in stock_upd.split("\n\n") if "img_type=kernel" in b]
    kinfo = blocks[0].strip()

    rsize, rmd5 = chunk_image(a.rootfs, "rootfs.squashfs", mdir)
    with open(os.path.join(mdir, "ota_update.in"), "w") as f:
        f.write(f"ota_version={a.version}\n\n{kinfo}\n\n"
                f"img_type=rootfs\nimg_name=rootfs.squashfs\nimg_size={rsize}\nimg_md5={rmd5}\n\n")
    open(os.path.join(mdir, f"ota_v{a.version}.ok"), "w").close()

    shutil.copytree(os.path.join(a.stock, "recovery"), os.path.join(tree, "recovery"))
    stock_cfg = open(os.path.join(a.stock, "ota_config.in")).read()
    rec_ver = [l for l in stock_cfg.splitlines() if l.startswith("recovery_version=")][0]
    if int(rec_ver.split("=", 1)[1]) > 4:
        raise SystemExit(f"refusing to build: {rec_ver} > 4 would rewrite the recovery slot")
    with open(os.path.join(tree, "ota_config.in"), "w") as f:
        f.write(f"current_version={a.version}\n{rec_ver}\n")

    files = []
    for root, _, fns in os.walk(tree):
        for fn in fns:
            files.append(os.path.relpath(os.path.join(root, fn), tree))
    files.sort()
    with open(os.path.join(tree, "md5_file_info.txt"), "w") as f:
        for rel in files:
            f.write(f"{md5_file(os.path.join(tree, rel))}  {rel}\n")

    zpath = os.path.join(a.out, a.zipname)
    if os.path.exists(zpath):
        os.remove(zpath)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for root, dirs, fns in os.walk(tree):
            dirs.sort()
            rel_root = os.path.relpath(root, tree)
            if rel_root != ".":
                z.write(root, rel_root + "/")
            for fn in sorted(fns):
                p = os.path.join(root, fn)
                z.write(p, os.path.relpath(p, tree))
    print(f"rootfs size={rsize} md5={rmd5}")
    print(f"wrote {zpath} ({os.path.getsize(zpath)} bytes)")


if __name__ == "__main__":
    main()
