#!/usr/bin/bash
# deploy.sh -- install the shell32 property-store shim into the Wine prefix.
#
#   bash deploy.sh [install|remove|status]
#
# What it fixes
# -------------
# Wine's SHGetPropertyStoreForWindow returns S_OK and a store whose every method
# is E_NOTIMPL *without initialising its out-parameter*:
#
#   dlls/shell32/shell32_main.c
#     window_prop_store_GetCount(iface, DWORD *count)                     { return E_NOTIMPL; }
#     window_prop_store_GetAt(iface, DWORD prop, PROPERTYKEY *key)        { return E_NOTIMPL; }
#     window_prop_store_GetValue(iface, const PROPERTYKEY *key, PROPVARIANT *var)
#                                                                        { return E_NOTIMPL; }
#
# A caller that does not check the HRESULT reads uninitialised stack memory. The
# NetEase UU Remote client read `var.pwszVal` as a string and passed the garbage
# pointer to QString::fromUtf16(), which killed the process:
#
#   EXCEPTION 0xc0000005, Qt5Core.dll + 0xbf11b (QString::fromUtf16)
#
# The shim replaces that one export with a store that initialises every
# out-parameter. All 363 other shell32 exports are pass-through trampolines.
#
# IMPORTANT: only the executables listed below get the override. A global
# WINEDLLOVERRIDES=shell32=n,b would load this DLL into every process in the
# prefix, which is not something a partially-implemented system DLL should do.
#
# SPDX-License-Identifier: 0BSD

set -uo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
prefix="${UUYC_WINE_PREFIX:-${XDG_DATA_HOME:-$HOME/.local/share}/uuyc-wine/wineprefix}"
root="$prefix/drive_c/Program Files/Netease/GameViewer"
dllname=shell32
# Wine matches AppDefaults by executable file name only. A key carrying a path is
# accepted by reg add but never applied, which silently disables the shim.
exes=(
    'StreamerCodecDetector.exe'
    'GameViewer.exe'
    'GameViewerServer.exe'
)
stale_keys=(
    'bin/StreamerCodecDetector.exe'
    'bin/GameViewer.exe'
    'bin/GameViewerServer.exe'
)
action="${1:-install}"

[[ -d $root ]] || { printf 'no GameViewer install in %s\n' "$prefix" >&2; exit 1; }

# Remove ONLY this shim's own value. Deleting the whole AppDefaults\<name> key
# would also wipe the d3d11 shim's override for the same executable, silently
# disabling the other shim.
registry_del() {
    local name=$1
    WINEPREFIX="$prefix" WINEDEBUG=-all wine reg delete \
        "HKCU\\Software\\Wine\\AppDefaults\\${name}\\DllOverrides" \
        /v "$dllname" /f >/dev/null 2>&1
}

# The path-form keys from an earlier deployment are genuinely dead, so those do
# get removed wholesale.
registry_del_key() {
    local name=$1
    WINEPREFIX="$prefix" WINEDEBUG=-all wine reg delete \
        "HKCU\\Software\\Wine\\AppDefaults\\${name}" /f >/dev/null 2>&1
}

# The shim is a complete drop-in only if it exports everything Wine's shell32
# does: a PE that imports a missing symbol fails to load, which looks like the
# whole application dying instantly.
check_exports() {
    local dll=$1
    command -v objdump >/dev/null 2>&1 || return 0
    local have want miss
    have=$(objdump -p "$dll" 2>/dev/null | sed -n '/Ordinal\/Name Pointer/,/^$/p' | grep -E '^\s*\[[ 0-9]+\] \+base\[' | awk 'NF{print $NF}' | sort -u)
    want=$(objdump -p /usr/lib/wine/x86_64-windows/shell32.dll 2>/dev/null | sed -n '/Ordinal\/Name Pointer/,/^$/p' | grep -E '^\s*\[[ 0-9]+\] \+base\[' | awk 'NF{print $NF}' | sort -u)
    miss=$(comm -23 <(printf '%s\n' "$want") <(printf '%s\n' "$have") | grep -c .) || true
    if [[ ${miss:-0} -gt 0 ]]; then
        printf 'WARNING: %s is missing %s export(s) Wine provides.\n' "$dll" "$miss"
        printf 'Processes importing them will fail to start. Regenerate with:\n'
        printf '  bash ../vdshim/gen-forwarders.sh /usr/lib/wine/x86_64-windows/shell32.dll \\\n'
        printf '      "%s/uuyc-shshim-forwarders" SHGetPropertyStoreForWindow\n' "$here"
        return 1
    fi
    printf 'export table: complete (%s symbols)\n' "$(printf '%s\n' "$have" | grep -c .)"
    return 0
}

case $action in
install)
    [[ -r $here/uuyc-shshim.dll ]] || { printf 'uuyc-shshim.dll missing next to this script\n' >&2; exit 1; }
    for dir in "$root/bin"; do
        cp -f "$here/uuyc-shshim.dll" "$dir/$dllname.dll" \
            && printf 'installed     -> %s/%s.dll\n' "$dir" "$dllname"
    done
    # Clean leftovers from any earlier wrong deployment FIRST: registry_del wipes
    # a whole AppDefaults key, so doing this after the loop would undo it.
    for key in "${stale_keys[@]}"; do registry_del_key "$key"; done
    for name in "${exes[@]}"; do
        WINEPREFIX="$prefix" WINEDEBUG=-all wine reg add \
            "HKCU\\Software\\Wine\\AppDefaults\\${name}\\DllOverrides" \
            /v "$dllname" /t REG_SZ /d 'n,b' /f >/dev/null 2>&1 \
            && printf 'override set  -> %s: %s=n,b\n' "$name" "$dllname"
    done
    check_exports "$root/bin/$dllname.dll" || exit 1
    cat <<'EOF'

Do NOT export WINEDLLOVERRIDES for this. The per-app AppDefaults keys above are
the whole mechanism -- launch normally:

    uuyc-wine

Shim log: C:\uuyc-shshim.log  (i.e. <prefix>/drive_c/uuyc-shshim.log)

To compare against the unfixed behaviour, set UUYC_SH_AUMID=empty when launching:
the store then still initialises the PROPVARIANT but reports E_NOTIMPL.
EOF
    ;;
remove)
    for dir in "$root/bin"; do
        rm -f "$dir/$dllname.dll" && printf 'removed       -> %s/%s.dll\n' "$dir" "$dllname"
    done
    for name in "${exes[@]}"; do
        registry_del "$name" && printf 'override gone -> %s\n' "$name"
    done
    for key in "${stale_keys[@]}"; do registry_del_key "$key"; done
    ;;
status)
    for f in "$root/bin/$dllname.dll"; do
        printf 'dll:         %s  %s\n' "$([[ -f $f ]] && echo present || echo missing)" "$f"
    done
    for name in "${exes[@]}"; do
        printf 'override %-30s ' "$name:"
        WINEPREFIX="$prefix" WINEDEBUG=-all wine reg query \
            "HKCU\\Software\\Wine\\AppDefaults\\${name}\\DllOverrides" /v "$dllname" 2>/dev/null \
            | grep -q 'n,b' && echo "set ($dllname=n,b)" || echo missing
    done
    log="$prefix/drive_c/uuyc-shshim.log"
    printf 'shim log:    %s\n' "$([[ -f $log ]] && stat -c '%s bytes, %y' "$log" || echo missing)"
    [[ -f $root/bin/$dllname.dll ]] && check_exports "$root/bin/$dllname.dll"
    ;;
*)
    printf 'usage: %s [install|remove|status]\n' "$0" >&2
    exit 2
    ;;
esac
