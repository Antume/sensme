#!/bin/sh
# Build a fully static musl binary (works on any Linux distro, and on
# Android/Termux when built for aarch64).
#
# Meant to run inside Alpine, where the host C library is already musl:
#
#   docker run --rm -v "$PWD:/src:ro" -v "$PWD/dist:/out" \
#              alpine:3.20 sh /src/scripts/build-musl.sh native x86_64 /out
#
#   docker run --rm -v "$PWD:/src:ro" -v "$PWD/dist:/out" \
#              alpine:3.20 sh /src/scripts/build-musl.sh aarch64-linux-musl aarch64 /out
#
# args: <native|CROSS-PREFIX> <arch-label> <outdir>
set -e

TARGET=${1:-native}
ARCH=${2:-$(uname -m)}
OUTDIR=${3:-dist}
MPG_VER=1.32.10

SRC=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(sed -n 's/^VERSION ?= //p' "$SRC/Makefile" | head -1)

apk add --no-cache build-base curl pkgconf tar bzip2 >/dev/null

if [ "$TARGET" = native ]; then
    CC=cc
    HOSTFLAG=
    STRIP=strip
else
    curl -fsSL "https://musl.cc/${TARGET}-cross.tgz" | tar xz -C /opt
    export PATH="/opt/${TARGET}-cross/bin:$PATH"
    CC="${TARGET}-gcc"
    HOSTFLAG="--host=${TARGET}"
    STRIP="${TARGET}-strip"
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# static libmpg123 (release builds only ship a shared library)
curl -fsSL "https://downloads.sourceforge.net/project/mpg123/mpg123/${MPG_VER}/mpg123-${MPG_VER}.tar.bz2" \
    | tar xj -C "$work"
cd "$work/mpg123-${MPG_VER}"
./configure $HOSTFLAG --prefix="$work/prefix" --disable-shared --enable-static \
    --disable-lfs --with-cpu=generic CC="$CC" >"$work/configure.log" 2>&1 \
    || { tail -30 "$work/configure.log"; exit 1; }
make -j"$(nproc)" >"$work/make.log" 2>&1
make install >"$work/install.log" 2>&1

export PKG_CONFIG_LIBDIR="$work/prefix/lib/pkgconfig"
cp -r "$SRC" "$work/src"
cd "$work/src"
make STATIC=1 CC="$CC"

BIN="sensme-${VERSION}-linux-${ARCH}"
mkdir -p "$OUTDIR"
cp sensme.exe "$OUTDIR/$BIN" 2>/dev/null || cp sensme "$OUTDIR/$BIN"
"$STRIP" "$OUTDIR/$BIN" || true
file "$OUTDIR/$BIN"
