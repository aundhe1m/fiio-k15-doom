# DOOM Firmware Builder image (doomfw-builder): everything build.sh / build.ps1 need, so the
# host only needs Docker.
#
#   docker build -t doomfw-builder doom-fw/docker/
#
# Stage 1 is the K15 userland cross compiler: static mipsel-linux-musl, MIPS32r2 / FP64 /
# NaN2008. musl-cross-make is pinned to a commit and verifies every source tarball against
# its own hashes. Stage 2 adds what the rest of the pipeline runs: squashfs-tools 4.6.1 (the
# rootfs stage runs as root so setuid busybox and non-root owners survive), python3 + PIL for
# the mq_ui/JSON patchers and the icon, numpy + fonts-ubuntu for the boot animations, and
# git + patch for the pinned engine sources.
#
# The build scripts rebuild the image when the doomfw.builder.rev label differs from the one
# in this file: bump it on every change.
FROM debian:12-slim AS toolchain

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ca-certificates git wget xz-utils bzip2 patch rsync file make \
    && rm -rf /var/lib/apt/lists/*

ARG MCM_URL=https://github.com/richfelker/musl-cross-make.git
ARG MCM_COMMIT=227df8b99103f9c59f6570babf892978e293082f
COPY mcm-config.mak /tmp/mcm-config.mak
RUN git clone "$MCM_URL" /tmp/mcm \
    && cd /tmp/mcm && git checkout --detach "$MCM_COMMIT" \
    && test "$(git rev-parse HEAD)" = "$MCM_COMMIT" \
    && cp /tmp/mcm-config.mak config.mak \
    && make -j"$(nproc)" && make install \
    && cd / && rm -rf /tmp/mcm /tmp/mcm-config.mak

FROM ubuntu:24.04
LABEL doomfw.builder.rev="1"
RUN apt-get update \
    && apt-get install -y --no-install-recommends squashfs-tools python3 python3-pil \
       python3-numpy fonts-ubuntu git patch ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && git config --system --add safe.directory '*'
COPY --from=toolchain /opt/k15-musl /opt/k15-musl
ENV PATH=/opt/k15-musl/bin:$PATH
WORKDIR /w
