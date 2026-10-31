# musl-cross-make config for the K15 (Ingenic X2000) userland cross compiler.
# Mirrors b0hemia/diskOS ui/toolchain/mcm-config.mak (same SoC family) plus C++.
#
# Target ABI = the stock K15 firmware's own (verified on v159 /bin/busybox):
#   MIPS32r2, o32, hard-float with 64-bit FPU registers (FP64), IEEE 754-2008 NaN.
# Ubuntu's mipsel-linux-gnu toolchain emits legacy-NaN code, which the kernel refuses
# to exec on this CPU ("ELF binary's NaN mode not supported by CPU").
TARGET = mipsel-linux-musl
OUTPUT = /opt/k15-musl
GCC_VER = 11.2.0
MUSL_VER = 1.2.6
BINUTILS_VER = 2.44
GCC_CONFIG += --with-arch=mips32r2 --with-nan=2008 --with-fp-32=64 --with-odd-spreg-32
GCC_CONFIG += --disable-libquadmath --disable-libsanitizer --enable-languages=c,c++
COMMON_CONFIG += MAKEINFO=true
