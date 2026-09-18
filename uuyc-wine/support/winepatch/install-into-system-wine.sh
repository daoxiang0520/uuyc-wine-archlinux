#!/usr/bin/bash
# install-into-system-wine.sh -- put the shell32 fix into the system Wine.
#
# Why this is needed
# ------------------
# `uuyc-wine` runs /usr/bin/wine by default, and that Wine still has the bug:
# the window property store returns E_NOTIMPL without initialising its out
# parameter, and the client dies in QString::fromUtf16() about four seconds into
# every remote session:
#
#   EXCEPTION 0xc0000005   Qt5Core.dll + 0xbf11b   read at 0xffffffffffffffff
#
# A patched Wine was built into ~/wine-patched, but unless you launch with
# UUYC_WINE_BIN pointing at it, the client still uses the system Wine. This
# script copies the ONE file that differs -- shell32.dll -- into the system Wine
# so a plain `uuyc-wine` carries the fix.
#
# Both trees are wine-11.17 from the same source, so the PE builtins are
# ABI-compatible; only the patched function differs. shell32 has no unix-side
# .so, so a single file is enough. It is reversible, and a Wine package upgrade
# restores the original.
#
# Usage:
#   bash install-into-system-wine.sh            # install (asks for sudo)
#   bash install-into-system-wine.sh --revert   # restore the backup
#   bash install-into-system-wine.sh --check    # report which one is in place
#
# SPDX-License-Identifier: 0BSD
set -uo pipefail

src="${UUYC_PATCHED_WINE:-$HOME/wine-patched}/lib/wine/x86_64-windows/shell32.dll"
dst=/usr/lib/wine/x86_64-windows/shell32.dll
backup=/usr/lib/wine/x86_64-windows/shell32.dll.orig-uuyc

action="${1:-install}"

case $action in
check|--check)
    printf 'system  shell32.dll: %s  (%s bytes, %s)\n' "$dst" \
        "$(stat -c%s "$dst" 2>/dev/null || echo '?')" "$(stat -c%y "$dst" 2>/dev/null | cut -d. -f1)"
    printf 'patched shell32.dll: %s  (%s bytes)\n' "$src" \
        "$(stat -c%s "$src" 2>/dev/null || echo '?')"
    [[ -f $backup ]] && printf 'backup present     : %s\n' "$backup"
    printf '\nverify with the poison test (expect "PROPVARIANT WAS INITIALISED"):\n'
    printf '  cd %s && WINEPREFIX="$HOME/.local/share/uuyc-wine/wineprefix" \\\n' \
        "$(dirname "$(dirname "$src")")/../share/uuyc-wine/shshim"
    printf '    wine ./testshshim.exe\n'
    ;;
install|--install)
    [[ -r $src ]] || { printf 'patched shell32.dll not found: %s\n' "$src" >&2
        printf 'set UUYC_PATCHED_WINE, or build it with build-patched-wine.sh\n' >&2; exit 1; }
    [[ -r $dst ]] || { printf 'system shell32.dll not found: %s\n' "$dst" >&2; exit 1; }

    if [[ ! -f $backup ]]; then
        sudo cp -f "$dst" "$backup" || exit 1
        printf 'backed up  -> %s\n' "$backup"
    else
        printf 'backup already exists, keeping it\n'
    fi
    sudo cp -f "$src" "$dst" || exit 1
    printf 'installed  -> %s (%s bytes)\n' "$dst" "$(stat -c%s "$dst")"
    printf '\nNow a plain launch carries the fix:\n  uuyc-wine\n'
    ;;
revert|--revert)
    [[ -f $backup ]] || { printf 'no backup at %s\n' "$backup" >&2; exit 1; }
    sudo cp -f "$backup" "$dst" || exit 1
    printf 'reverted   -> %s (this reinstates the crash)\n' "$dst"
    ;;
*)
    printf 'usage: %s [install|--revert|--check]\n' "$0" >&2
    exit 2
    ;;
esac
