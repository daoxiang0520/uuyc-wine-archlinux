#!/usr/bin/bash
# build-patched-wine.sh -- build a Wine with the shell32 out-parameter fix.
#
# Why
# ---
# Wine's window property store returned E_NOTIMPL from GetCount/GetAt/GetValue
# without initialising the out parameter. A caller that does not check the
# HRESULT reads uninitialised stack memory; reading var.pwszVal as a string gives
# an arbitrary pointer. The NetEase UU Remote client did exactly that and died in
# QString::fromUtf16() about four seconds into every remote session:
#
#   EXCEPTION 0xc0000005, Qt5Core.dll + 0xbf11b (QString::fromUtf16)
#
# This cannot be worked around from outside Wine: shell32 is a KnownDLL and the
# loader always takes the builtin, and no application module imports
# SHGetPropertyStoreForWindow, so there is no import slot to patch either.
#
# What it does
# ------------
# Downloads the matching Wine source, applies the patch next to this script,
# builds x86_64 only (roughly half the work), and installs to a private prefix.
# Nothing system-wide is touched; run the client with
#
#   UUYC_WINE_BIN=<prefix>/bin/wine uuyc-wine
#
# Usage: build-patched-wine.sh [version] [install-prefix]
#
# SPDX-License-Identifier: 0BSD
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
version=${1:-11.17}
prefix=${2:-$HOME/wine-patched}
build=${WINE_BUILD_DIR:-$HOME/wine-build}
jobs=${JOBS:-$(nproc)}

patch=$(ls "$here"/*shell32*out-params*.patch 2>/dev/null | head -1)
[[ -r $patch ]] || { echo "no shell32 patch next to $here" >&2; exit 1; }

mkdir -p "$build"
cd "$build"
src=wine-$version
if [[ ! -d $src ]]; then
    [[ -f wine-$version.tar.xz ]] || curl -fL -o "wine-$version.tar.xz" \
        "https://dl.winehq.org/wine/source/${version%.*}.x/wine-$version.tar.xz"
    tar xf "wine-$version.tar.xz"
fi

cd "$src"
if ! grep -q 'PropVariantInit(var)' dlls/shell32/shell32_main.c; then
    patch -p1 --forward < "$patch"
fi
grep -q 'PropVariantInit(var)' dlls/shell32/shell32_main.c \
    || { echo "the fix is not present in the source" >&2; exit 1; }

# x86_64 only: every module on the session path is 64-bit, and this halves the
# build. Sound is enabled so behaviour matches the packaged Wine.
[[ -f Makefile ]] || ./configure --prefix="$prefix" --enable-archs=x86_64 --disable-tests
make -k -j"$jobs"
make -k install

# `make install` does not place the unix-side .so files (the opencl module fails
# to build without its headers, and the install target stops short). Copy them.
dst="$prefix/lib/wine/x86_64-unix"
mkdir -p "$dst"
while read -r so; do cp -f "$so" "$dst/$(basename "$so")"; done < <(find . -name '*.so' -not -path './tools/*')
"$prefix/bin/wine" --version
echo
echo "installed to $prefix"
echo "run with:  UUYC_WINE_BIN=$prefix/bin/wine uuyc-wine --hw-decode"
