#!/bin/sh
# Build a static Windows .exe with MinGW-w64 (Debian/Ubuntu/WSL):
#
#   sudo apt-get install -y mingw-w64 pkg-config bzip2 file
#   sh scripts/build-windows.sh dist
#
# args: [outdir]
set -e

OUTDIR=${1:-dist}
OUTDIR=$(mkdir -p "$OUTDIR" && cd "$OUTDIR" && pwd)
MPG_VER=1.32.10
CROSS=x86_64-w64-mingw32
CC="${CROSS}-gcc"

SRC=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(sed -n 's/^VERSION ?= //p' "$SRC/Makefile" | head -1)

command -v "$CC" >/dev/null 2>&1 || {
    echo "error: $CC not found (apt-get install mingw-w64)" >&2
    exit 1
}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# static libmpg123 for Windows, installed into the temp dir (no sudo)
curl -fsSL "https://downloads.sourceforge.net/project/mpg123/mpg123/${MPG_VER}/mpg123-${MPG_VER}.tar.bz2" \
    | tar xj -C "$work"
cd "$work/mpg123-${MPG_VER}"
./configure --host="$CROSS" --prefix="$work/prefix" --disable-shared --enable-static \
    --disable-lfs --with-cpu=generic CC="$CC" >"$work/configure.log" 2>&1 \
    || { tail -30 "$work/configure.log"; exit 1; }
make -j"$(nproc)" >"$work/make.log" 2>&1
make install >"$work/install.log" 2>&1

export PKG_CONFIG_LIBDIR="$work/prefix/lib/pkgconfig"
cp -r "$SRC" "$work/src"
cd "$work/src"
make STATIC=1 CC="$CC"

BIN="sensme-${VERSION}-windows-x86_64.exe"
mkdir -p "$OUTDIR"
cp sensme.exe "$OUTDIR/$BIN"
"${CROSS}-strip" "$OUTDIR/$BIN" || true
file "$OUTDIR/$BIN"
