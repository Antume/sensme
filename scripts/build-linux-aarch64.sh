#!/bin/sh
# Cross-compile the static aarch64 Linux binary with Zig's bundled musl.
# Runs on any x86_64 Linux (needs curl, tar, xz, make, binutils):
#
#   sh scripts/build-linux-aarch64.sh dist
#
# The result is a fully static binary: it runs on ARM Linux boards and, because
# it does not need glibc, inside Termux on Android.
#
# args: [outdir]
set -e

OUTDIR=${1:-dist}
OUTDIR=$(mkdir -p "$OUTDIR" && cd "$OUTDIR" && pwd)
MPG_VER=1.32.10
ZIG_VER=0.14.1
TARGET=aarch64-linux-musl

SRC=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(sed -n 's/^VERSION ?= //p' "$SRC/Makefile" | head -1)

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

curl -fsSL "https://ziglang.org/download/${ZIG_VER}/zig-x86_64-linux-${ZIG_VER}.tar.xz" \
    | tar xJ -C "$work"
ZIG="$work/zig-x86_64-linux-${ZIG_VER}/zig"
# PIE is mandatory on Android 10+: a non-PIE (ET_EXEC) binary is rejected by
# the loader with "has unexpected e_type: 2". Static PIE still has no runtime
# dependencies, so the result stays distro-independent.
CC="$ZIG cc -target $TARGET -fPIE"

# static libmpg123 for the target
curl -fsSL "https://downloads.sourceforge.net/project/mpg123/mpg123/${MPG_VER}/mpg123-${MPG_VER}.tar.bz2" \
    | tar xj -C "$work"
cd "$work/mpg123-${MPG_VER}"
./configure --host="$TARGET" --prefix="$work/prefix" --disable-shared --enable-static \
    --disable-lfs --with-cpu=generic CC="$CC" >"$work/configure.log" 2>&1 \
    || { tail -30 "$work/configure.log"; exit 1; }
make -j"$(nproc)" >"$work/make.log" 2>&1
make install >"$work/install.log" 2>&1

export PKG_CONFIG_LIBDIR="$work/prefix/lib/pkgconfig"
cp -r "$SRC" "$work/src"
cd "$work/src"
make STATIC=1 CC="$CC" LDFLAGS="-static-pie -Wl,-s"

BIN="sensme-${VERSION}-linux-aarch64"
cp sensme "$OUTDIR/$BIN"
file "$OUTDIR/$BIN"
