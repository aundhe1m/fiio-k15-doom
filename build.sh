#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# DOOM Firmware Builder for the FiiO K15 (Linux, macOS, WSL; on Windows use build.ps1).
#
#   ./build.sh <official K15_update_YYYYMMDD_v159.zip> [--date YYYYMMDD] [--time HHMM]
#              [--version N] [--animation melt|hellfire|demons|terminal|stock]
#   ./build.sh --engine-only     # dev: just build/out/doomk15 + k15-mqsend
#
# Takes the official FiiO v159 update you downloaded yourself and writes
# out/K15_update_<date>_<time>_v<N>.zip (UTC build time, default N=160): the same firmware
# plus DOOM (a DOOM tile in the swipe-right source grid and a DOOM entry in the volume-knob
# input carousel) and a DOOM boot animation (default melt; stock keeps FiiO's).
# Nothing from FiiO and no WAD is stored in this repository; the builder patches your copy.
#
# Needs only Docker. Everything else runs in the doomfw-builder image (docker/Dockerfile),
# built on the first run (~10 minutes), which also needs internet for the pinned
# doomgeneric + Chocolate Doom sources.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
IMAGE=doomfw-builder
usage() { sed -n '3,17p' "$0"; }

# Pass every option through; swap the zip path for its path inside the container.
ZIP=
ENGINE_ONLY=
n=$#
while [ "$n" -gt 0 ]; do
    a=$1
    shift
    n=$((n - 1))
    case "$a" in
        -h|--help) usage; exit 0 ;;
        --date|--time|--version|--animation)
            [ "$n" -gt 0 ] || { echo "$a needs a value" >&2; exit 1; }
            set -- "$@" "$a" "$1"
            shift
            n=$((n - 1)) ;;
        --engine-only) ENGINE_ONLY=1; set -- "$@" "$a" ;;
        -*) echo "unknown option $a" >&2; usage >&2; exit 1 ;;
        *) [ -z "$ZIP" ] || { echo "more than one zip given" >&2; exit 1; }
           ZIP=$a
           set -- "$@" "/in/$(basename "$a")" ;;
    esac
done
if [ -z "$ENGINE_ONLY" ]; then
    [ -n "$ZIP" ] && [ -f "$ZIP" ] || { usage >&2; exit 1; }
    ZIPDIR=$(cd "$(dirname "$ZIP")" && pwd)
fi

command -v docker >/dev/null || { echo "docker not found" >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "the Docker daemon isn't running" >&2; exit 1; }
REV=$(sed -n 's/^LABEL doomfw.builder.rev="\(.*\)"/\1/p' "$HERE/docker/Dockerfile")
HAVE=$(docker image inspect -f '{{index .Config.Labels "doomfw.builder.rev"}}' "$IMAGE" 2>/dev/null || true)
if [ "$HAVE" != "$REV" ]; then
    echo "### building the $IMAGE image (rev $REV; the first time takes ~10 minutes)"
    docker build -t "$IMAGE" "$HERE/docker"
fi

# The version comes from the VERSION file; DOOMFW_VERSION, if set, overrides it (dev builds).
HOST_UID=$(id -u)
HOST_GID=$(id -g)
export HOST_UID HOST_GID
if [ -n "$ENGINE_ONLY" ]; then
    exec docker run --rm -e DOOMFW_VERSION -e HOST_UID -e HOST_GID \
        --mount "type=bind,source=$HERE,target=/w" \
        "$IMAGE" sh /w/tools/build-all.sh "$@"
fi
exec docker run --rm -e DOOMFW_VERSION -e HOST_UID -e HOST_GID \
    --mount "type=bind,source=$HERE,target=/w" \
    --mount "type=bind,source=$ZIPDIR,target=/in,readonly" \
    "$IMAGE" sh /w/tools/build-all.sh "$@"
