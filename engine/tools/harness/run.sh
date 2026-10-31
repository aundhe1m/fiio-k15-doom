#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Host harness for the DOOM home menu: build it with the host gcc, run one input script
# against a throw-away config dir with fake WADs, and write the frames as PNG (if python3
# has PIL) or PPM.
#
#   doom-fw/engine/tools/harness/run.sh <outdir> "<script>" [conf lines...]
#   e.g.  run.sh /tmp/menu "d e" "K15_DEFAULT_WAD=doom2.wad"
#
# Env: HARNESS_KBD=1 / HARNESS_PAD=1 pretend a keyboard / pad is plugged in,
#      HARNESS_WADS="doom2.wad ..." installed WADs (default: doom2, doom1, plutonia,
#        scythe; "none" for none), HARNESS_USB_WADS="..." WADs on a fake USB stick
#        (fakewads.py has the names), PYTHON=<python with PIL> for PNG output.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/../../src
OUT=$1
SCRIPT=$2
shift 2
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
rm -f "$OUT"/frame*.ppm "$OUT"/frame*.png

BIN=$OUT/.menu
gcc -std=gnu11 -O1 -g -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-format-truncation -DK15_VERSION='"harness"' \
    -I"$SRC" -o "$BIN" "$HERE/harness.c" "$SRC/k15_menu.c" "$SRC/k15_gfx.c" \
    "$SRC/k15_wad.c" "$SRC/k15_cfg.c" "$SRC/k15_touch.c"

HOME_DIR=$OUT/.home
rm -rf "$HOME_DIR"
mkdir -p "$HOME_DIR/wads"
if [ "${HARNESS_WADS:-}" != none ]; then
    # shellcheck disable=SC2086
    "${PYTHON:-python3}" "$HERE/fakewads.py" "$HOME_DIR/wads" ${HARNESS_WADS:-}
fi
USB_DIR=
if [ -n "${HARNESS_USB_WADS:-}" ]; then
    USB_DIR=$OUT/.usb
    rm -rf "$USB_DIR"
    mkdir -p "$USB_DIR"
    # shellcheck disable=SC2086
    "${PYTHON:-python3}" "$HERE/fakewads.py" "$USB_DIR" $HARNESS_USB_WADS
fi
for line in "$@"; do
    echo "$line" >>"$HOME_DIR/k15.conf"
done

K15_HOME=$HOME_DIR/ HARNESS_OUT=$OUT HARNESS_USB=$USB_DIR "$BIN" "$SCRIPT"
if "${PYTHON:-python3}" -c "import PIL" 2>/dev/null; then
    "${PYTHON:-python3}" - "$OUT" <<'EOF'
import glob, os, sys
from PIL import Image
for p in sorted(glob.glob(os.path.join(sys.argv[1], "frame*.ppm"))):
    Image.open(p).save(p[:-4] + ".png")
    os.remove(p)
EOF
fi
[ -f "$HOME_DIR/k15.conf" ] && { echo "--- k15.conf after the run:"; grep -v '^#' "$HOME_DIR/k15.conf" | tr '\n' ' '; echo; }
ls "$OUT" | grep -c frame | sed 's/$/ frames/'
