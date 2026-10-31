#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Compile doomk15 + k15-mqsend (static musl, MIPS32r2 o32 FP64 NaN2008) inside the
# doomfw-builder image, after prepare.sh. Called by tools/build-all.sh; on its own:
#
#   ./build.sh --engine-only
set -eu

W=${DOOMFW:-/w}
SRC=$W/build/src
OUT=$W/build/out
OBJ=$W/build/obj
CC=mipsel-linux-musl-gcc
DG=$SRC/doomgeneric/doomgeneric
[ -d "$DG" ] || { echo "run engine/prepare.sh first" >&2; exit 1; }
rm -rf "$OBJ"
mkdir -p "$OBJ" "$OUT"

DG_OBJS="dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode
d_net f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale
i_sound i_system i_timer memio m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu
m_misc m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj
p_plats p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp
r_data r_draw r_main r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff
s_sound tables v_video wi_stuff w_checksum w_file w_main w_wad z_zone w_file_stdc
i_input i_video doomgeneric mus2mid"

VERSION=${DOOMFW_VERSION:-dev}
CFLAGS="-O2 -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE -DK15 -DK15_VERSION=\"$VERSION\"
 -DSCREENWIDTH=1020 -DDOOMGENERIC_RESX=1020 -DDOOMGENERIC_RESY=200 -DFEATURE_SOUND -DK15_SOUND
 -I$SRC/k15 -I$SRC/choco/src -I$SRC/choco/opl -I$DG"

FILES=""
for o in $DG_OBJS; do FILES="$FILES $DG/$o.c"; done
for f in i_oplmusic midifile; do FILES="$FILES $SRC/choco/src/$f.c"; done
for f in opl opl_queue opl3 opl_k15; do FILES="$FILES $SRC/choco/opl/$f.c"; done
for f in "$SRC"/k15/*.c; do FILES="$FILES $f"; done

# Compile in parallel batches (the image has no make).
JOBS=$(nproc 2>/dev/null || echo 4)
n=0
for f in $FILES; do
    o="$OBJ/$(basename "$(dirname "$f")")_$(basename "$f" .c).o"
    case "$f" in
        "$SRC"/k15/*) WARN="-Wall -Wno-unused-parameter -Wno-sign-compare" ;; # ours: warnings on
        *) WARN="-w" ;;
    esac
    $CC $CFLAGS $WARN -c "$f" -o "$o" &
    n=$((n + 1))
    if [ $((n % JOBS)) -eq 0 ]; then wait; fi
done
wait
count=$(ls "$OBJ"/*.o | wc -l)
expected=$(echo $FILES | wc -w)
[ "$count" -eq "$expected" ] || { echo "compile failed ($count/$expected objects)" >&2; exit 1; }

$CC -static -s -o "$OUT/doomk15" "$OBJ"/*.o -lm -lpthread
$CC -static -s -O2 -Wall -o "$OUT/k15-mqsend" "$W/launcher/k15-mqsend.c"
ls -l "$OUT/doomk15" "$OUT/k15-mqsend"
