#!/usr/bin/bash
# collect.sh -- gather every attachment the Wine issue needs, in one run.
#
#   bash collect.sh [OUTPUT_DIR]     (default: ./collected)
#
# Run this on the affected machine, in a normal terminal (not inside a sandbox
# or container, so /dev/dri is visible). Nothing is modified.

set -uo pipefail

out=${1:-collected}
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$out"

prefix="${UUYC_WINE_PREFIX:-${XDG_DATA_HOME:-$HOME/.local/share}/uuyc-wine/wineprefix}"

section() { printf '\n==> %s\n' "$1"; }

section "host"
{
    echo "# host information"
    echo "date          : $(date -Is)"
    echo "kernel        : $(uname -r)"
    echo "arch          : $(uname -m)"
    echo
    echo "## wine"
    wine --version 2>/dev/null || echo "wine not found"
    pacman -Q wine 2>/dev/null || true
    echo
    echo "## GPU"
    lspci -nnk 2>/dev/null | grep -A3 -iE 'vga|3d|display'
    echo
    echo "## DRM nodes"
    ls -l /dev/dri 2>/dev/null || echo "no /dev/dri"
    echo
    echo "## Mesa / Vulkan / VA-API packages"
    pacman -Q mesa vulkan-intel vulkan-icd-loader intel-media-driver libva 2>/dev/null || true
    echo
    echo "## renderer override in the prefix"
    WINEPREFIX="$prefix" WINEDEBUG=-all wine reg query 'HKCU\\Software\\Wine\\Direct3D' /s 2>/dev/null | grep -viE 'mesa|dri2' || echo "(not set)"
} >"$out/00-host.txt" 2>&1

section "vulkaninfo"
if command -v vulkaninfo >/dev/null 2>&1; then
    {
        echo "# vulkaninfo --summary"
        vulkaninfo --summary 2>&1 | head -80
        echo
        echo "# extensions containing 'video'"
        vulkaninfo 2>/dev/null | grep -iE 'VK_KHR_video|VK_EXT_video' | sort -u
        echo "(empty above = driver exposes no Vulkan Video extension)"
    } >"$out/01-vulkaninfo.txt" 2>&1
else
    echo "vulkaninfo not installed (pacman -S vulkan-tools)" >"$out/01-vulkaninfo.txt"
fi

section "vainfo"
if command -v vainfo >/dev/null 2>&1; then
    {
        echo "# vainfo   (hardware decode capability that the GPU does have)"
        vainfo 2>&1 | head -40
    } >"$out/02-vainfo.txt" 2>&1
else
    echo "vainfo not installed (pacman -S libva-utils)" >"$out/02-vainfo.txt"
fi

section "probe"
if [[ -x $here/uuyc-d3dprobe.exe ]]; then
    {
        echo "# uuyc-d3dprobe --all   (CheckFormatSupport + GetVideoDecoderProfile probe)"
        echo "# prefix: $prefix"
        WINEPREFIX="$prefix" WINEDEBUG=-all wine "$here/uuyc-d3dprobe.exe" --all 2>&1 \
            | grep -viE 'mesa|dri2|^fixme:|^warn:'
    } >"$out/03-d3dprobe.txt" 2>&1
else
    echo "uuyc-d3dprobe.exe not found next to this script" >"$out/03-d3dprobe.txt"
fi

section "decoder profile count (minimal API check)"
cat >"$out/04-profile-count-note.txt" <<'EOF'
# minimal check if a full program is not convenient
# The probe above covers this; this note documents the two calls that matter:
#
#   ID3D11VideoDevice::GetVideoDecoderProfileCount(&count)   -> observed 0
#   ID3D11Device::CheckFormatSupport(DXGI_FORMAT_NV12, &f)   -> observed E_FAIL
EOF

section "wine log excerpt"
crash_log="${XDG_STATE_HOME:-$HOME/.local/state}/uuyc-wine/crash-debug.log"
if [[ -r $crash_log ]]; then
    {
        echo "# excerpts from $crash_log"
        echo "# produced with: uuyc-wine --debug-crash  (WINEDEBUG=+seh,+d3d,+d3d11,+dxgi,+wined3d,+vulkan,+winevulkan)"
        echo
        echo "## adapter in use"
        echo "adapter_vk_* total: $(grep -c 'adapter_vk' "$crash_log")"
        echo "adapter_gl_* total: $(grep -c 'adapter_gl' "$crash_log")"
        grep -m2 -E 'wined3d_adapter_vk_init ' "$crash_log"
        echo
        echo "## video extensions enabled by wined3d"
        echo "VK_KHR_video_* occurrences: $(grep -cE 'VK_KHR_video_(queue|decode_queue|decode_h264)' "$crash_log")"
        echo "video entries in extension list: $(grep -oE '\- \"VK_[A-Za-z0-9_]+\"' "$crash_log" | sort -u | grep -ci video)"
        echo
        echo "## application asks for decoder profiles"
        grep -m4 'get_video_decode_profile_count' "$crash_log"
        echo "total calls: $(grep -c 'get_video_decode_profile_count' "$crash_log")"
        echo
        echo "## format support queries"
        echo "CheckFormatSupport calls: $(grep -c 'CheckFormatSupport' "$crash_log")"
        echo "  format 87  (NV12) : $(grep -c 'format 87,' "$crash_log")"
        echo "  format 103 (P010) : $(grep -c 'format 103,' "$crash_log")"
        grep -m3 'CheckFormatSupport iface' "$crash_log"
        echo
        echo "## device removal"
        echo "GetDeviceRemovedReason (stub): $(grep -c 'GetDeviceRemovedReason' "$crash_log")"
        grep -m2 'GetDeviceRemovedReason' "$crash_log"
        grep -m2 'still holding back buffer' "$crash_log"
        echo
        echo "## session swapchain"
        grep -m2 'CreateSwapChainForHwnd' "$crash_log"
        grep -m2 'wined3d_swapchain_state_init Client rect' "$crash_log"
    } >"$out/05-wine-log-excerpt.txt" 2>&1
else
    echo "no $crash_log; run 'uuyc-wine --debug-crash' first" >"$out/05-wine-log-excerpt.txt"
fi

section "done"
ls -la "$out"
cat <<EOF

Attach every file in $out/ to the issue, together with:
  - uuyc-d3dprobe.c   (probe source, so maintainers can build it themselves)
  - the issue body

Before posting, skim 00-host.txt: it lists kernel, GPU and package versions, which is
what maintainers ask for first.
EOF
