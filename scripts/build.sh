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

# makepkg materialises every file:// source into $startdir under the source's
# *name*. A copy left there by an earlier build is what the next build hashes, so
# editing a source without deleting its old copy fails with
#
#     vdshim-source.c ... FAILED
#
# even though the real source has the checksum recorded in the PKGBUILD. Removing
# the copies first is the difference between "edited a source, rebuilt fine" and an
# afternoon spent re-checking checksums that were never wrong. makepkg recreates
# them every build, so this is safe.
drop_materialised() {
    local pkgdir=$1 line name url target
    while IFS= read -r line; do
        line=${line%%#*}
        line=${line//[\"\']/}
        line=${line#"${line%%[![:space:]]*}"}
        [[ $line == *::file://* ]] || continue
        name=${line%%::*}
        url=${line#*::}
        target=${url#file://}
        target=$(printf '%s' "$target" | sed \
            -e "s|\$startdir|$pkgdir|g" \
            -e "s|\$_installer_dir|$pkgdir/local|g" \
            -e "s|\$_apk_dir|$pkgdir/local|g" \
            -e 's|\$_installer|uuyc-installer.exe|g' \
            -e 's|\$_apk|com.netease.uuremote.apk|g')
        target=$(readlink -m -- "$target")
        if [[ -f $pkgdir/$name && $target != "$pkgdir/$name" ]]; then
            rm -f -- "$pkgdir/$name"
        fi
    done < <(sed -n '/^source=(/,/^)/p' "$pkgdir/PKGBUILD")
}

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
    drop_materialised "$dir"
    # Dependency checks stay enabled so a missing build tool fails fast with a
    # clear message instead of an obscure compiler error.
    ( cd "$dir" && makepkg --force --cleanbuild --noconfirm )
done

printf '\npackages:\n'
find "$root" -maxdepth 2 -name '*.pkg.tar.zst' -printf '  %p\n' | sort
