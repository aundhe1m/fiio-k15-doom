#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Container side of the DOOM Firmware Builder: the whole pipeline, run as root inside the
# doomfw-builder image with doom-fw mounted at /w. Start it with build.sh (Linux/macOS/WSL)
# or build.ps1 (Windows); they mount the input zip's folder at /in.
#
#   build-all.sh <official K15_update_YYYYMMDD_v159.zip> [--date YYYYMMDD] [--time HHMM]
#                [--version N] [--animation melt|hellfire|demons|terminal|stock]
#   build-all.sh --engine-only        # just build/out/{doomk15,k15-mqsend}
#
# Env: DOOMFW_VERSION (version shown in the DOOM menu and /etc/homebrew_version; default:
#      the VERSION file, so every build of a release says the same),
#      HOST_UID/HOST_GID (owner given to build/ and out/ at exit; unset on Windows).
set -eu

W=/w
ZIP=
NOW=$(date -u +%Y%m%d%H%M)
DATE=${NOW%????}
TIME=${NOW#????????}
VER=160
ANIM=melt
ANIMS="melt hellfire demons terminal stock"
ENGINE_ONLY=
while [ $# -gt 0 ]; do
    case "$1" in
        --date) DATE=$2; shift 2 ;;
        --time) TIME=$2; shift 2 ;;
        --version) VER=$2; shift 2 ;;
        --animation) ANIM=$2; shift 2 ;;
        --engine-only) ENGINE_ONLY=1; shift ;;
        -*) echo "unknown option $1" >&2; exit 1 ;;
        *) ZIP=$1; shift ;;
    esac
done
[ "$(id -u)" = 0 ] || { echo "must run as root (in doomfw-builder)" >&2; exit 1; }
if [ -z "$ENGINE_ONLY" ]; then
    [ -n "$ZIP" ] && [ -f "$ZIP" ] || { echo "no input zip (got '$ZIP')" >&2; exit 1; }
    case "$DATE" in [0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]) ;; *) echo "bad --date" >&2; exit 1 ;; esac
    # Digits only: the K15 reads the version with strchr(name, 'v') + "v%d.zip".
    case "$TIME" in [0-9][0-9][0-9][0-9]) ;; *) echo "bad --time" >&2; exit 1 ;; esac
    case "$VER" in ''|*[!0-9]*) echo "bad --version" >&2; exit 1 ;; esac
    [ "$VER" -gt 159 ] || { echo "--version must be > 159 or the K15 refuses it" >&2; exit 1; }
    case " $ANIMS " in *" $ANIM "*) ;; *) echo "--animation must be one of: $ANIMS" >&2; exit 1 ;; esac
fi

BUILD=$W/build
WORK=$BUILD/work
OUT=$W/out
NAME=K15_update_${DATE}_${TIME}_v${VER}.zip
DOOMFW_VERSION=${DOOMFW_VERSION:-$(cat "$W/VERSION")}
DOOMFW_BOOTANIM=$ANIM
export DOOMFW_VERSION DOOMFW_BOOTANIM PYTHONDONTWRITEBYTECODE=1

give_back() {
    if [ -n "${HOST_UID:-}" ]; then
        chown -R "$HOST_UID:${HOST_GID:-$HOST_UID}" "$BUILD" 2>/dev/null || true
        [ -d "$OUT" ] && chown -R "$HOST_UID:${HOST_GID:-$HOST_UID}" "$OUT" 2>/dev/null || true
    fi
}
trap give_back EXIT
step() { printf '\n### %s\n' "$*"; }

if [ -n "$ENGINE_ONLY" ]; then
    step "1/2 engine sources (pinned upstreams + patches)"
    DOOMFW_BUILD=$BUILD sh "$W/engine/prepare.sh"
    step "2/2 compile doomk15 + k15-mqsend ($DOOMFW_VERSION)"
    sh "$W/engine/build.sh"
    exit 0
fi

step "1/6 verify the official zip ($(basename "$ZIP"))"
rm -rf "$WORK"
mkdir -p "$WORK"
python3 "$W/tools/pkg_verify.py" "$ZIP" "$WORK/stock" --extract "$WORK/img" \
    | tee "$WORK/verify-input.txt"
grep -q "^RESULT: PASS" "$WORK/verify-input.txt" || { echo "input zip failed verification" >&2; exit 1; }
grep -qx "current_version=159" "$WORK/stock/ota_config.in" ||
    { echo "not a v159 update: only v159 is supported" >&2; exit 1; }
grep -qx "recovery_version=4" "$WORK/stock/ota_config.in" ||
    { echo "unexpected recovery_version in the input zip" >&2; exit 1; }
for f in rootfs.squashfs xImage; do
    [ -f "$WORK/img/$f" ] || { echo "input zip has no $f" >&2; exit 1; }
done

step "2/6 engine sources (pinned upstreams + patches)"
DOOMFW_BUILD=$BUILD sh "$W/engine/prepare.sh"

step "3/6 compile doomk15 + k15-mqsend ($DOOMFW_VERSION)"
sh "$W/engine/build.sh"

step "4/6 rootfs: patch, add DOOM, content-diff, mksquashfs"
sh "$W/tools/rootfs-stage.sh"

step "5/6 package $NAME"
python3 "$W/tools/pkg_build.py" --stock "$WORK/stock" --stock-ver 159 \
    --rootfs "$WORK/rootfs.doom.squashfs" --version "$VER" --out "$WORK/pkg" --zipname "$NAME"

step "6/6 verify the package like the device does"
python3 "$W/tools/pkg_verify.py" "$WORK/pkg/$NAME" "$WORK/verify" \
    rootfs.squashfs="$WORK/rootfs.doom.squashfs" xImage="$WORK/img/xImage" \
    | tee "$WORK/verify-output.txt"
grep -q "^RESULT: PASS" "$WORK/verify-output.txt" || { echo "package failed verification" >&2; exit 1; }
grep -qx "recovery_version=4" "$WORK/pkg/tree/ota_config.in" || { echo "recovery_version != 4" >&2; exit 1; }

mkdir -p "$OUT"
mv -f "$WORK/pkg/$NAME" "$OUT/$NAME"
rm -rf "$WORK/verify" "$WORK/pkg/tree"
{
    echo "package:  $NAME"
    echo "doom-fw:  $DOOMFW_VERSION"
    echo "bootanim: $ANIM"
    echo "input:    $(basename "$ZIP") md5 $(md5sum "$ZIP" | cut -d' ' -f1)"
    echo "rootfs:   md5 $(md5sum "$WORK/rootfs.doom.squashfs" | cut -d' ' -f1)"
    echo "zip:      md5 $(md5sum "$OUT/$NAME" | cut -d' ' -f1)"
    echo
    cat "$WORK/content-diff.txt"
} > "$OUT/$NAME.txt"
step "done"
cat "$OUT/$NAME.txt"
cat <<EOF

Flash: copy out/$NAME to the root of a FAT32/exFAT USB stick (no other K15_update_*.zip),
plug it into the K15, Settings -> Others -> FW Update -> USB. Don't cut power.
EOF
