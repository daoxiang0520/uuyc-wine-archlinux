#!/usr/bin/bash
# Build every Arch Linux package in this tree.
#
#   ./scripts/build.sh            build both packages
#   ./scripts/build.sh wine       build only the Wine client package
#   ./scripts/build.sh android    build only the Android/Waydroid package
#
# Packages are written next to their PKGBUILD. Nothing is installed; run
# scripts/install.sh for that.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
readonly root

targets=("$@")
if ((${#targets[@]} == 0)); then
    targets=(wine android)
fi

for target in "${targets[@]}"; do
    case $target in
        wine) dir="$root/uuyc-wine" ;;
        android) dir="$root/uuyc-android" ;;
        *) printf 'unknown target: %s (use wine or android)\n' "$target" >&2; exit 2 ;;
    esac

    printf '\n=== building %s ===\n' "$(basename "$dir")"
    # Dependency checks stay enabled so a missing build tool fails fast with a
    # clear message instead of an obscure compiler error.
    ( cd "$dir" && makepkg --force --cleanbuild --noconfirm )
done

printf '\npackages:\n'
find "$root" -maxdepth 2 -name '*.pkg.tar.zst' -printf '  %p\n' | sort
