#!/usr/bin/bash
# sweep.sh -- run a set of decoder-configuration combinations against the client.
#
#   bash sweep.sh            # all combinations below
#   bash sweep.sh 0 1 2      # only these ConfigDecoderSpecific values
#
# Each run writes its own log slice into sweep/, so the results can be compared
# without re-running anything. The shim must already be deployed (deploy.sh).
#
# SPDX-License-Identifier: 0BSD

set -uo pipefail

prefix="${UUYC_WINE_PREFIX:-${XDG_DATA_HOME:-$HOME/.local/share}/uuyc-wine/wineprefix}"
root="$prefix/drive_c/Program Files/Netease/GameViewer"
shim_log="$root/bin/uuyc-vdshim.log"
out=${1:-all}
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

[[ -f $root/bin/d3d11.dll ]] || { printf 'shim not deployed; run deploy.sh install first\n' >&2; exit 1; }

mkdir -p "$here/sweep"

run_one() {
    local label=$1 raw=$2 specific=$3 count=$4
    local mark log

    printf '\n=== %s (RAW=%s SPECIFIC=%s COUNT=%s) ===\n' "$label" "$raw" "$specific" "$count"
    log="$here/sweep/${label}.log"
    mark=$(wc -l <"$shim_log" 2>/dev/null || echo 0)

    UUYC_VD_CONFIG_RAW="$raw" UUYC_VD_CONFIG_SPECIFIC="$specific" UUYC_VD_CONFIG_COUNT="$count" \
    WINEDLLOVERRIDES='d3d11=n,b' \
        timeout 240 uuyc-wine >/dev/null 2>&1 &
    local pid=$!
    printf '    click into a device now, then close the window (up to 240s) ...\n'
    wait $pid 2>/dev/null

    tail -n +"$((mark + 1))" "$shim_log" 2>/dev/null >"$log"
    local decoded
    decoded=$(grep -c 'CreateVideoDecoder profile=' "$log" 2>/dev/null || echo 0)
    printf '    log: %s\n' "$log"
    printf '    CreateVideoDecoder calls: %s\n' "$decoded"
    if ((decoded > 0)); then
        printf '    >>> HIT: the client created a decoder, see the parameters there\n'
        grep 'CreateVideoDecoder profile=' "$log" | head -3 | sed 's/^/        /'
    fi
}

if [[ $out == all ]]; then
    # raw bitstream first (the documented H.264 mode), then the sweep
    run_one raw1-spec0 1 0 1
    run_one raw1-spec1 1 1 1
    run_one raw1-spec2 1 2 1
    run_one raw1-spec3 1 3 1
    run_one raw0-spec0 0 0 1
    run_one raw1-spec0-count4 1 0 4
else
    for spec in "$@"; do
        run_one "raw1-spec${spec}" 1 "$spec" 1
    done
fi

printf '\n=== summary ===\n'
for f in "$here"/sweep/*.log; do
    [[ -f $f ]] || continue
    n=$(grep -c 'CreateVideoDecoder profile=' "$f" 2>/dev/null || echo 0)
    printf '  %-28s CreateVideoDecoder=%s\n' "$(basename "$f")" "$n"
done
printf '\nSend the sweep/ directory (or just the summary plus any HIT log).\n'
