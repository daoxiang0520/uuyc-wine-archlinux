#!/usr/bin/bash
# End-to-end smoke test for the uuyc-wine launcher against a throwaway prefix.
#
#   tests/smoke-prefix.sh /tmp/prefix-root
#
# The test only builds the prefix (--setup-only): it installs the vendor payload,
# writes the wevtapi shim and registers GameViewerService. Installing WebView2
# downloads ~150 MB from Microsoft, so it is skipped unless UUYC_SMOKE_WEBVIEW=1
# is set. Use UUYC_SMOKE_REUSE=1 to re-check an already prepared prefix.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
target=${1:?usage: smoke-prefix.sh PREFIX_ROOT}

# Stage the same layout the package installs under /usr/share/uuyc-wine, so the
# launcher finds its data files without a system install.
stage="$target/share"
rm -rf "$stage"
mkdir -p "$stage"
pkgver=$(sed -n "s/^pkgver=\(.*\)$/\1/p" "$root/uuyc-wine/PKGBUILD" | head -1)
pkgrel=$(sed -n "s/^pkgrel=\(.*\)$/\1/p" "$root/uuyc-wine/PKGBUILD" | head -1)
ln -sf "$root/uuyc-wine/local/uuyc-installer.exe" "$stage/uuyc-installer.exe"
cp "$root/uuyc-wine/wevtapi.dll" "$stage/wevtapi.dll" 2>/dev/null || \
    cp "$root/uuyc-wine/src/wevtapi.dll" "$stage/wevtapi.dll" 2>/dev/null || true
if [[ ! -f $stage/wevtapi.dll ]]; then
    printf 'building wevtapi.dll for the smoke test ...\n'
    ( cd "$stage" &&
      as --64 "$root/uuyc-wine/wevtapi.S" -o wevtapi-elf.o &&
      objcopy --remove-section=.note.gnu.property -O pe-x86-64 wevtapi-elf.o wevtapi-coff.o &&
      ld -mi386pep --dll --no-insert-timestamp --entry=DllMain --subsystem windows \
        -o wevtapi.dll wevtapi-coff.o "$root/uuyc-wine/wevtapi.def" \
        -L/usr/lib/wine/x86_64-windows -lkernel32 )
fi
printf '%s\n' "$pkgver" >"$stage/upstream-version"
printf '%s-%s\n' "$pkgver" "$pkgrel" >"$stage/package-release"

export UUYC_WINE_SHARE_DIR="$stage"
export UUYC_WINE_PREFIX="$target/prefix"
export WINEDEBUG=-all
if [[ ${UUYC_SMOKE_WEBVIEW:-0} != 1 ]]; then
    export UUYC_WINE_SKIP_WEBVIEW=1
fi
# Keep every writable location inside the test root, so the smoke test also runs
# in a sandbox whose HOME is read-only.
export XDG_DATA_HOME="$target/xdg-data"
export XDG_STATE_HOME="$target/xdg-state"
mkdir -p "$target" "$XDG_DATA_HOME" "$XDG_STATE_HOME"
if [[ ${UUYC_SMOKE_REUSE:-0} != 1 ]]; then
    rm -rf "$UUYC_WINE_PREFIX"
fi

printf '=== launcher: version ===\n'
bash "$root/uuyc-wine/uuyc-wine" --version

printf '\n=== launcher: prefix setup (this creates the Wine prefix) ===\n'
bash "$root/uuyc-wine/uuyc-wine" --setup-only

printf '\n=== verifying installed payload ===\n'
install_dir="$UUYC_WINE_PREFIX/drive_c/Program Files/Netease/GameViewer"
status=0
for file in GameViewer.exe bin/GameViewer.exe bin/GameViewerServer.exe \
            bin/GameViewerHealthd.exe bin/wevtapi.dll bin/MicrosoftEdgeWebview2Setup.exe; do
    if [[ -f $install_dir/$file ]]; then
        printf '  ok      %s\n' "$file"
    else
        printf '  MISSING %s\n' "$file"
        status=1
    fi
done

printf '\n=== verifying service registration ===\n'
if WINEPREFIX="$UUYC_WINE_PREFIX" wine reg query \
    'HKLM\System\CurrentControlSet\Services\GameViewerService' 2>/dev/null | grep -q GameViewerService; then
    printf '  ok      GameViewerService is registered\n'
else
    printf '  MISSING GameViewerService registration\n'
    status=1
fi

printf '\nfile count: %s\n' "$(find "$install_dir" -type f | wc -l)"
exit "$status"
