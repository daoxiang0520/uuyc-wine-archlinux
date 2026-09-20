#!/usr/bin/bash
# check-abi.sh -- cross-check every hardcoded Vulkan ABI value in uuyc-vkvideo.c
# against the official Khronos headers.
#
# Why this exists
# ---------------
# The probe deliberately avoids <vulkan/vulkan.h> so it builds on a machine with
# no vulkan-headers. That means every struct size, struct-type enum and codec bit
# is written out by hand -- and a wrong value does not fail loudly. A wrong
# VkStructureType makes the loader dispatch on garbage and the process dies with
# SIGSEGV, which is indistinguishable from "the driver crashed".
#
# That happened twice during development:
#   * VkPhysicalDeviceProperties was 256 bytes instead of 824 -> stack smashing
#   * VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR was 1000023004
#     instead of 1000040003, and the H.264 codec bit was 0x2 instead of 0x1
#     -> SIGSEGV (signal 11) on the capability query
#
# So this script compiles the same table against the real headers and diffs it.
#
# Usage:
#     KHDR_DIR=/path/to/dir/with/vulkan/vulkan_core.h check-abi.sh
#
# KHDR_DIR must contain a `vulkan/` include tree (the Vulkan-Headers repo's
# include/ directory). Get it with:
#     git clone --depth 1 https://github.com/KhronosGroup/Vulkan-Headers
#     KHDR_DIR=Vulkan-Headers/include
#
# Exit status: 0 when every value matches, 1 on any mismatch or compile failure.
#
# SPDX-License-Identifier: 0BSD

set -uo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
probe=${UUYC_VKVIDEO_C:-$here/uuyc-vkvideo.c}
khdr=${KHDR_DIR:-}

if [[ ! -r $probe ]]; then
    printf 'check-abi.sh: cannot read %s\n' "$probe" >&2
    exit 1
fi
if [[ -z $khdr || ! -r $khdr/vulkan/vulkan_core.h ]]; then
    printf 'check-abi.sh: set KHDR_DIR to a Vulkan-Headers include directory\n' >&2
    printf '  expected: %s/vulkan/vulkan_core.h\n' "${khdr:-<KHDR_DIR>}" >&2
    exit 1
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# ---------------------------------------------------------------------------
# 1. What the probe claims.
# ---------------------------------------------------------------------------
# Defines of the form  #define NAME <integer>u   -- the value is everything after
# the name, minus any trailing u/U and comments.
declare -A claimed
while read -r name value; do
    [[ -n $name ]] || continue
    value=${value%%/*}          # strip a trailing comment
    value=${value//[uU]/}       # strip the unsigned suffix
    value=${value// /}
    claimed[$name]=$value
done < <(sed -n 's/^#define[[:space:]]\+\([A-Z][A-Z0-9_]*\)[[:space:]]\+\([0-9][0-9xXa-fA-F]*\).*/\1 \2/p' "$probe")

if ((${#claimed[@]} == 0)); then
    printf 'check-abi.sh: found no #define constants in %s\n' "$probe" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 2. What the official headers say, for exactly those names.
# ---------------------------------------------------------------------------
cat >"$work/ref.c" <<'EOF'
#include <stdio.h>
#include <stddef.h>
#include <vulkan/vulkan_core.h>

/* Struct sizes the probe must agree with. */
#define SZ(t) printf("sizeof %s %zu\n", #t, sizeof(t))
/* Enum/constant values, printed as unsigned so the textual diff is stable. */
#define VAL(n) printf("value %s %llu\n", #n, (unsigned long long)(n))

int main(void)
{
    SZ(VkVideoProfileInfoKHR);
    SZ(VkVideoDecodeH264ProfileInfoKHR);
    SZ(VkVideoDecodeCapabilitiesKHR);
    SZ(VkVideoDecodeH264CapabilitiesKHR);
    SZ(VkExtensionProperties);
    SZ(VkPhysicalDeviceSparseProperties);
    SZ(VkPhysicalDeviceLimits);
    SZ(VkPhysicalDeviceProperties);
    SZ(VkApplicationInfo);
    SZ(VkInstanceCreateInfo);
    SZ(VkVideoProfileListInfoKHR);

    VAL(VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR);
    VAL(VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR);
    VAL(VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR);
    VAL(VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR);
    VAL(VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR);
    VAL(VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR);
    VAL(VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR);
    VAL(VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR);
    VAL(STD_VIDEO_H264_PROFILE_IDC_HIGH);
    VAL(VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR);
    VAL(VK_MAX_PHYSICAL_DEVICE_NAME_SIZE);

    /* Field offsets the probe reads at hardcoded positions. */
    printf("offset caps.flags %zu\n", offsetof(VkVideoCapabilitiesKHR, flags));
    printf("offset caps.minBitstreamBufferSizeAlignment %zu\n",
           offsetof(VkVideoCapabilitiesKHR, minBitstreamBufferSizeAlignment));
    printf("offset profile.pNext %zu\n", offsetof(VkVideoProfileInfoKHR, pNext));
    return 0;
}
EOF

if ! gcc -I"$khdr" -o "$work/ref" "$work/ref.c" 2>"$work/cc.log"; then
    printf 'check-abi.sh: failed to compile the reference program\n' >&2
    sed 's/^/  /' "$work/cc.log" >&2
    exit 1
fi
"$work/ref" >"$work/ref.txt" || { printf 'check-abi.sh: reference run failed\n' >&2; exit 1; }

# ---------------------------------------------------------------------------
# 3. Compare. Every name the probe defines must match the header value.
# ---------------------------------------------------------------------------
declare -A official
while read -r kind name value; do
    [[ ${kind:-} == value ]] || continue
    official[$name]=$value
done <"$work/ref.txt"

# Size and offset expectations live in the probe's _Static_assert block.
declare -A size_expect
while read -r name value; do
    size_expect[$name]=$value
done < <(sed -n 's/^VK_ASSERT_SIZEOF(\([A-Za-z0-9_]*\),[[:space:]]*\([0-9]\+\)).*/\1 \2/p' "$probe")
while read -r name value; do
    size_expect[$name]=$value
done < <(sed -n 's/.*_Static_assert(sizeof(\([A-Za-z0-9_]*\)) == \([0-9]\+\).*/\1 \2/p' "$probe")

fail=0
checked=0
checked_sizes=0

# Compare numerically: the probe writes some bits as 0x... and the headers print
# plain decimals, so a textual diff produces false positives.
num() { printf '%d' "$(( $1 ))" 2>/dev/null || printf '%s' "$1"; }

for name in "${!claimed[@]}"; do
    [[ -n ${official[$name]:-} ]] || continue     # not a Vulkan constant (event types, bit counts)
    ((checked++))
    if [[ $(num "${claimed[$name]}") != $(num "${official[$name]}") ]]; then
        printf 'MISMATCH %-54s probe=%-12s header=%s\n' \
            "$name" "${claimed[$name]}" "${official[$name]}"
        fail=1
    fi
done

for name in "${!size_expect[@]}"; do
    hdr=$(awk -v n="$name" '$1=="sizeof" && $2==n {print $3}' "$work/ref.txt")
    [[ -n $hdr ]] || continue
    ((checked_sizes++))
    if [[ $(num "$hdr") != $(num "${size_expect[$name]}") ]]; then
        printf 'MISMATCH sizeof(%-36s) probe=%-12s header=%s\n' \
            "$name" "${size_expect[$name]}" "$hdr"
        fail=1
    fi
done

# Offsets the probe reads by raw position.
declare -A off_expect=(
    [caps.flags]=16
    [caps.minBitstreamBufferSizeAlignment]=32
    [profile.pNext]=8
)
for key in "${!off_expect[@]}"; do
    hdr=$(awk -v n="$key" '$1=="offset" && $2==n {print $3}' "$work/ref.txt")
    [[ -n $hdr ]] || continue
    ((checked_sizes++))
    if [[ $(num "$hdr") != $(num "${off_expect[$key]}") ]]; then
        printf 'MISMATCH offset %-40s probe=%-12s header=%s\n' \
            "$key" "${off_expect[$key]}" "$hdr"
        fail=1
    fi
done

if ((fail)); then
    printf '\ncheck-abi.sh: %d mismatch(es) -- fix uuyc-vkvideo.c before shipping.\n' 1 >&2
    exit 1
fi

printf 'check-abi.sh: OK -- %d constants and %d sizes/offsets match the official headers\n' \
    "$checked" "$checked_sizes"
if ((checked == 0)); then
    printf 'check-abi.sh: WARNING -- no constant names were cross-checked; the\n' >&2
    printf '  #define spelling in the probe may have changed. Verify manually.\n' >&2
    exit 1
fi
exit 0
