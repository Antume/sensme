#!/bin/sh
# Build the static x86_64 Linux binary with musl (Alpine's native C library).
# Run it inside Alpine:
#
#   docker run --rm -v "$PWD:/src:ro" -v "$PWD/dist:/out" \
#              alpine:3.20 sh /src/scripts/build-musl.sh x86_64 /out
#
# args: <arch-label> <outdir>
set -e

ARCH=${1:-$(uname -m)}
OUTDIR=${2:-dist}
OUTDIR=$(mkdir -p "$OUTDIR" && cd "$OUTDIR" && pwd)
MPG_VER=1.32.10

SRC=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(sed -n 's/^VERSION ?= //p' "$SRC/Makefile" | head -1)

apk add --no-cache build-base curl pkgconf tar bzip2 >/dev/null

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# static libmpg123 (release builds only ship a shared library)
curl -fsSL "https://downloads.sourceforge.net/project/mpg123/mpg123/${MPG_VER}/mpg123-${MPG_VER}.tar.bz2" \
    | tar xj -C "$work"
cd "$work/mpg123-${MPG_VER}"
./configure --prefix="$work/prefix" --disable-shared --enable-static \
    --disable-lfs --with-cpu=generic >"$work/configure.log" 2>&1 \
    || { tail -30 "$work/configure.log"; exit 1; }
make -j"$(nproc)" >"$work/make.log" 2>&1
make install >"$work/install.log" 2>&1

export PKG_CONFIG_LIBDIR="$work/prefix/lib/pkgconfig"
cp -r "$SRC" "$work/src"
cd "$work/src"
make STATIC=1

BIN="sensme-${VERSION}-linux-${ARCH}"
cp sensme "$OUTDIR/$BIN"
strip "$OUTDIR/$BIN" || true
file "$OUTDIR/$BIN"
