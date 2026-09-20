#!/usr/bin/bash
# deploy.sh -- install the D3D11 video-decode probe shim into the Wine prefix.
#
#   bash deploy.sh [install|remove|status]
#
# Which executables need the override, and why:
#
#   StreamerCodecDetector.exe  runs the startup capability probe. Measured from a
#                              --debug-crash log, this is the process that makes
#                              every D3D11 video call: GetVideoDecoderProfileCount,
#                              GetVideoDecoderProfile, CheckVideoDecoderFormat and
#                              GetVideoDecoderConfigCount. It MUST be overridden
#                              or the shim never sees the calls it exists to fix.
#                              An earlier version of this list omitted it, so the
#                              shim was deployed into two processes that never
#                              touch D3D11 video at all.
#   GameViewer.exe             loads bin/streamer.dll, so it is where the real
#                              decode session would create its decoder.
#   GameViewerServer.exe       also loads bin/streamer.dll (the capture side).
#
# Wine matches AppDefaults by the executable's file name only: the key must be
# "StreamerCodecDetector.exe", NOT "bin/StreamerCodecDetector.exe". A key
# carrying a path is accepted by reg add but never applied.
#
# SPDX-License-Identifier: 0BSD

set -uo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
prefix="${UUYC_WINE_PREFIX:-${XDG_DATA_HOME:-$HOME/.local/share}/uuyc-wine/wineprefix}"
root="$prefix/drive_c/Program Files/Netease/GameViewer"
# Wine matches AppDefaults by the executable's file name only: the key must be
# "GameViewerServer.exe", NOT "bin/GameViewerServer.exe". A key carrying a path
# is accepted by reg add but never applied, which silently disables the shim.
exes=(
    'bin/StreamerCodecDetector.exe|StreamerCodecDetector.exe'
    'bin/GameViewer.exe|GameViewer.exe'
    'bin/GameViewerServer.exe|GameViewerServer.exe'
)
# Keys left behind by an earlier, wrong deployment attempt. These are the
# *path-form* keys, which Wine accepts but never applies. Bare names must NOT be
# listed here: registry_del() removes the whole AppDefaults key for a name, so a
# real target in this list would have its freshly-set override deleted again.
stale_keys=(
    'bin/GameViewer.exe'
    'bin/GameViewerServer.exe'
    'bin/StreamerCodecDetector.exe'
)
action="${1:-install}"

[[ -d $root ]] || { printf 'no GameViewer install in %s\n' "$prefix" >&2; exit 1; }

# Remove ONLY this shim's own value. Wiping the whole AppDefaults\<name> key
# would also take out another shim's override for the same executable.
registry_del() {
    local name=$1
    WINEPREFIX="$prefix" WINEDEBUG=-all wine reg delete \
        "HKCU\\Software\\Wine\\AppDefaults\\${name}\\DllOverrides" \
        /v d3d11 /f >/dev/null 2>&1
}

# Path-form keys from the earlier wrong deployment are dead; remove those whole.
registry_del_key() {
    local name=$1
    WINEPREFIX="$prefix" WINEDEBUG=-all wine reg delete \
        "HKCU\\Software\\Wine\\AppDefaults\\${name}" /f >/dev/null 2>&1
}

case $action in
install)
    [[ -r $here/uuyc-vdshim.dll ]] || { printf 'uuyc-vdshim.dll missing next to this script\n' >&2; exit 1; }
    # IMPORTANT: the shim must be deployed AS d3d11.dll. WINEDLLOVERRIDES only
    # affects loads of the overridden name, so overriding "uuyc-vdshim" never
    # intercepts streamer.dll's import of d3d11.dll. The override is therefore
    # d3d11=n,b and this file is what Wine finds first in the app directory.
    for dir in "$root/bin"; do
        cp -f "$here/uuyc-vdshim.dll" "$dir/d3d11.dll" && printf 'installed     -> %s/d3d11.dll\n' "$dir"
        cp -f "$here/uuyc-vdshim.dll" "$dir/uuyc-vdshim.dll" 2>/dev/null
    done
    # Remove leftovers from the earlier wrong deployment FIRST. Doing this after
    # the loop below would wipe overrides for names that appear in both lists.
    for key in "${stale_keys[@]}"; do registry_del_key "$key"; done
    for entry in "${exes[@]}"; do
        name=${entry##*|}
        WINEPREFIX="$prefix" WINEDEBUG=-all wine reg add \
            "HKCU\\Software\\Wine\\AppDefaults\\${name}\\DllOverrides" \
            /v d3d11 /t REG_SZ /d 'n,b' /f >/dev/null 2>&1 \
            && printf 'override set  -> %s: d3d11=n,b\n' "$name"
    done
    cat <<'EOF'

Do NOT export WINEDLLOVERRIDES for this. A global override applies to every
process in the prefix, and the shim is a partial d3d11 implementation; that is
what made unrelated processes fail earlier. The per-app AppDefaults keys set
above are the whole mechanism -- just launch normally:

    uuyc-wine --hw-decode
EOF
    printf '\nlogs written by the shim:\n  %s/bin/uuyc-vdshim.log\n' "$root"
    # A PE that imports a symbol this DLL lacks fails to load, which surfaces as
    # the whole application dying instantly. Verify before the user finds out.
    if command -v objdump >/dev/null 2>&1; then
        for dir in "$root/bin"; do
            have=$(objdump -p "$dir/d3d11.dll" 2>/dev/null | sed -n '/Ordinal\/Name Pointer/,/^$/p' | grep -E '^\s*\[[ 0-9]+\] \+base\[' | awk 'NF{print $NF}' | sort -u)
            want=$(objdump -p /usr/lib/wine/x86_64-windows/d3d11.dll 2>/dev/null | sed -n '/Ordinal\/Name Pointer/,/^$/p' | grep -E '^\s*\[[ 0-9]+\] \+base\[' | awk 'NF{print $NF}' | sort -u)
            miss=$(comm -23 <(printf '%s\n' "$want") <(printf '%s\n' "$have") | grep -c . || true)
            if [[ ${miss:-0} -gt 0 ]]; then
                printf '\nWARNING: the installed d3d11.dll is missing %s export(s) that Wine provides.\n' "$miss"
                printf 'Processes importing them will fail to start. Regenerate the forwarders:\n'
                printf '  bash gen-forwarders.sh && rebuild uuyc-vdshim.dll\n'
            else
                printf '\nexport table: complete (all Wine d3d11 exports present)\n'
            fi
        done
    fi
    ;;
remove)
    for dir in "$root/bin"; do
        rm -f "$dir/d3d11.dll" "$dir/uuyc-vdshim.dll" && printf 'removed       -> %s/d3d11.dll\n' "$dir"
    done
    for entry in "${exes[@]}"; do
        registry_del "${entry##*|}" && printf 'override gone -> %s\n' "${entry##*|}"
    done
    for key in "${stale_keys[@]}"; do
        registry_del_key "$key"
    done
    ;;
status)
    for f in "$root/bin/d3d11.dll" "$root/bin/uuyc-vdshim.dll"; do
        printf 'dll:         %s  %s\n' "$([[ -f $f ]] && echo present || echo missing)" "$f"
    done
    for entry in "${exes[@]}"; do
        name=${entry##*|}
        printf 'override %-30s ' "$name:"
        WINEPREFIX="$prefix" WINEDEBUG=-all wine reg query \
            "HKCU\\Software\\Wine\\AppDefaults\\${name}\\DllOverrides" /v d3d11 2>/dev/null \
            | grep -q 'n,b' && echo 'set (d3d11=n,b)' || echo missing
    done
    for log in "$root/uuyc-vdshim.log" "$root/bin/uuyc-vdshim.log" /tmp/uuyc-vdshim.log; do
        printf 'log %-10s %s\n' ":" "$([[ -f $log ]] && stat -c '%s bytes, %y' "$log" || echo missing)"
    done
    if [[ -f $root/bin/d3d11.dll ]] && command -v objdump >/dev/null 2>&1; then
        have=$(objdump -p "$root/bin/d3d11.dll" 2>/dev/null | sed -n '/Ordinal\/Name Pointer/,/^$/p' | grep -E '^\s*\[[ 0-9]+\] \+base\[' | awk 'NF{print $NF}' | sort -u)
        want=$(objdump -p /usr/lib/wine/x86_64-windows/d3d11.dll 2>/dev/null | sed -n '/Ordinal\/Name Pointer/,/^$/p' | grep -E '^\s*\[[ 0-9]+\] \+base\[' | awk 'NF{print $NF}' | sort -u)
        miss=$(comm -23 <(printf '%s\n' "$want") <(printf '%s\n' "$have") | grep -c . || true)
        printf 'export table: %s\n' "$([[ ${miss:-0} -gt 0 ]] && echo "$miss export(s) MISSING -- will crash importing PEs" || echo complete)"
    fi
    ;;
*)
    printf 'usage: %s [install|remove|status]\n' "$0" >&2
    exit 2
    ;;
esac
