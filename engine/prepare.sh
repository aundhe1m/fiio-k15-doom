#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Assemble the DOOM engine source tree for the K15 from pinned upstreams + our patches.
# Runs in the doomfw-builder image (git, patch, python3), called by tools/build-all.sh.
#
#   doom-fw/engine/prepare.sh            -> doom-fw/build/src/{doomgeneric,choco,k15}
#
# Upstreams are fetched once into doom-fw/build/upstream (git clone), then each pinned commit
# is exported with `git archive`, so the tree is rebuilt from scratch every time.
set -eu

DOOMGENERIC_URL=https://github.com/ozkl/doomgeneric.git
DOOMGENERIC_REV=e257a09527062bfb986b6e5e0780839b6cbd5b71
CHOCO_URL=https://github.com/chocolate-doom/chocolate-doom.git
CHOCO_REV=939cbfeb115338cf7868e516019a4ae832b1ebde # tag chocolate-doom-2.3.0

ENGINE=$(cd "$(dirname "$0")" && pwd)
BUILD=${DOOMFW_BUILD:-$ENGINE/../build}
UP=$BUILD/upstream
SRC=$BUILD/src
mkdir -p "$UP"

fetch() { # name url rev
    if [ ! -d "$UP/$1/.git" ]; then
        git clone -q "$2" "$UP/$1"
    fi
    if ! git -C "$UP/$1" cat-file -e "$3^{commit}" 2>/dev/null; then
        git -C "$UP/$1" fetch -q origin
    fi
    git -C "$UP/$1" cat-file -e "$3^{commit}"
}

export_tree() { # name rev dest [paths...]
    name=$1 rev=$2 dest=$3
    shift 3
    mkdir -p "$dest"
    git -C "$UP/$name" archive "$rev" "$@" | tar -x -C "$dest"
}

apply_patches() { # dir patchdir
    for p in "$2"/*.patch; do
        echo "  patch $(basename "$p")"
        patch -s -p1 -d "$1" --forward --no-backup-if-mismatch < "$p"
    done
}

fetch doomgeneric "$DOOMGENERIC_URL" "$DOOMGENERIC_REV"
fetch chocolate-doom "$CHOCO_URL" "$CHOCO_REV"

rm -rf "$SRC"
echo "doomgeneric ${DOOMGENERIC_REV}"
export_tree doomgeneric "$DOOMGENERIC_REV" "$SRC/doomgeneric" doomgeneric LICENSE
apply_patches "$SRC/doomgeneric" "$ENGINE/patches/doomgeneric"

echo "chocolate-doom ${CHOCO_REV}"
export_tree chocolate-doom "$CHOCO_REV" "$SRC/choco" COPYING opl src/i_oplmusic.c \
    src/midifile.c src/midifile.h
apply_patches "$SRC/choco" "$ENGINE/patches/chocolate-doom"
python3 "$ENGINE/tools/port_opl.py" "$SRC/choco/opl/opl_sdl.c" "$SRC/choco/opl/opl_k15.c"

mkdir -p "$SRC/k15"
cp "$ENGINE"/src/*.c "$ENGINE"/src/*.h "$SRC/k15/"
printf '%s\n' "doomgeneric $DOOMGENERIC_REV" "chocolate-doom $CHOCO_REV" > "$SRC/UPSTREAM"
echo "prepared $SRC"
