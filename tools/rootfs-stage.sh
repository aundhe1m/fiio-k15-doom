#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Rootfs stage of the DOOM Firmware Builder, called by tools/build-all.sh. Runs as root
# inside the doomfw-builder image (so setuid busybox and non-root owners survive), doom-fw
# mounted at /w. The tree is unpacked in the container's /tmp, never on the bind mount.
#
# In:  /w/build/work/img/rootfs.squashfs (stock v159, from the verified official zip)
#      /w/build/out/{doomk15,k15-mqsend}, /w/launcher/k15-doom-launch
#      DOOMFW_BOOTANIM: melt (default) | hellfire | demons | terminal | stock
# Out: /w/build/work/rootfs.doom.squashfs, /w/build/work/content-diff.txt
set -eu

W=/w
WORK=$W/build/work
STOCK_IMG=$WORK/img/rootfs.squashfs
OUT_IMG=$WORK/rootfs.doom.squashfs
STOCK_MQ_UI_MD5=6b91aa1d0bdd92b2b167aa8ce8e2a080
OPTS="-comp lzo -b 131072 -noappend -no-xattrs -quiet -no-progress"
ROOT=/tmp/root
STOCK=/tmp/stock
CFG=usr/project/config
BOOTANIM=${DOOMFW_BOOTANIM:-melt}
ANIMDIR=$CFG/img/start_up

[ "$(id -u)" = 0 ] || { echo "must run as root (in doomfw-builder)" >&2; exit 1; }
for f in "$STOCK_IMG" "$W/build/out/doomk15" "$W/build/out/k15-mqsend"; do
    [ -f "$f" ] || { echo "missing $f" >&2; exit 1; }
done

echo "== unsquashfs stock rootfs"
rm -rf "$ROOT" "$STOCK"
unsquashfs -q -n -d "$STOCK" "$STOCK_IMG"
unsquashfs -q -n -d "$ROOT" "$STOCK_IMG"

echo "== checks"
md5=$(md5sum "$ROOT/usr/bin/mq_ui" | cut -d' ' -f1)
if [ "$md5" != "$STOCK_MQ_UI_MD5" ]; then
    echo "mq_ui MD5 $md5 is not stock v159 ($STOCK_MQ_UI_MD5); refusing" >&2
    exit 1
fi
if [ -e "$ROOT/etc/homebrew_version" ] || [ -e "$ROOT/usr/bin/doomk15" ]; then
    echo "input rootfs is already modified; use the official FiiO zip" >&2
    exit 1
fi
echo "mq_ui $md5 (stock v159)"

echo "== patch mq_ui + UI lists"
python3 "$W/tools/patch_mq_ui.py" "$STOCK/usr/bin/mq_ui" /tmp/mq_ui.patched
install -m 0755 -o 0 -g 0 /tmp/mq_ui.patched "$ROOT/usr/bin/mq_ui"
python3 "$W/tools/patch_json.py" "$ROOT"

echo "== icon"
python3 "$W/tools/make_icon.py" icon /tmp/doom_src.png --width 62
install -m 0644 -o 0 -g 0 /tmp/doom_src.png "$ROOT/$CFG/img/input_setting/doom.png"

echo "== boot animation ($BOOTANIM)"
ANIM_ALLOW=
if [ "$BOOTANIM" != stock ]; then
    rm -rf /tmp/start_up
    PYTHONDONTWRITEBYTECODE=1 python3 "$W/tools/bootanim.py" render "$BOOTANIM" /tmp/start_up --stock "$STOCK/$ANIMDIR"
    for f in /tmp/start_up/*.jpg; do
        n=$(basename "$f")
        [ -f "$ROOT/$ANIMDIR/$n" ] || { echo "no stock $ANIMDIR/$n to replace" >&2; exit 1; }
        install -m 0755 -o 0 -g 0 "$f" "$ROOT/$ANIMDIR/$n"
        ANIM_ALLOW="$ANIM_ALLOW $ANIMDIR/$n"
    done
fi

echo "== binaries, launcher, notice"
install -m 0755 -o 0 -g 0 "$W/build/out/doomk15" "$ROOT/usr/bin/doomk15"
install -m 0755 -o 0 -g 0 "$W/build/out/k15-mqsend" "$ROOT/usr/bin/k15-mqsend"
install -m 0755 -o 0 -g 0 "$W/launcher/k15-doom-launch" "$ROOT/usr/bin/k15-doom-launch"
mkdir -p "$ROOT/usr/share/doc/doom-fw"
chmod 0755 "$ROOT/usr/share/doc" "$ROOT/usr/share/doc/doom-fw" 2>/dev/null || true
install -m 0644 -o 0 -g 0 "$W/rootfs-overlay/NOTICE" "$ROOT/usr/share/doc/doom-fw/NOTICE"
{
    echo "HOMEBREW=doom"
    echo "BASE=v159"
    echo "BUILT=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "DOOMFW=${DOOMFW_VERSION:-dev}"
    echo "BOOTANIM=$BOOTANIM"
} > "$ROOT/etc/homebrew_version"
chown 0:0 "$ROOT/etc/homebrew_version"
chmod 0644 "$ROOT/etc/homebrew_version"

echo "== content diff vs stock"
python3 "$W/tools/treediff.py" "$STOCK" "$ROOT" --allow \
    usr/bin/mq_ui usr/bin/doomk15 usr/bin/k15-mqsend usr/bin/k15-doom-launch \
    "$CFG/ui/right_input_mode.json" "$CFG/img/input_setting/doom.png" \
    etc/homebrew_version usr/share/doc/doom-fw usr/share/doc/doom-fw/NOTICE $ANIM_ALLOW \
    $( [ -d "$STOCK/usr/share/doc" ] || echo usr/share/doc ) \
    > "$WORK/content-diff.txt" || { cat "$WORK/content-diff.txt"; exit 1; }
cat "$WORK/content-diff.txt"

echo "== mksquashfs"
mksquashfs "$ROOT" "$OUT_IMG" $OPTS
unsquashfs -s "$OUT_IMG" | grep -E "Compression|Block size|xattrs" || true
rm -rf "$ROOT" "$STOCK" /tmp/mq_ui.patched /tmp/doom_src.png /tmp/start_up
chown "${HOST_UID:-0}:${HOST_GID:-0}" "$OUT_IMG" "$WORK/content-diff.txt"
ls -l "$OUT_IMG"
