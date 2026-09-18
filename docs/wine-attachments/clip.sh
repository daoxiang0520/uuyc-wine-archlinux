#!/usr/bin/bash
# clip.sh -- put a file on the KDE clipboard through the session bus.
#
# Why this exists: on this desktop none of the usual routes work.
#   * vim is built -clipboard / -xterm_clipboard, so `"+y` and `:%y+` fail
#     (setreg("+", ...) returns 1 and the register stays empty);
#   * xclip, xsel, wl-copy and wl-paste are not installed;
#   * python3's tkinter is broken (libtk8.6.so missing).
#
# Plasma still exposes the clipboard over D-Bus, and that does work -- verified
# with a write/read round trip. This wraps it.
#
# Usage:
#   clip.sh FILE                 put FILE on the clipboard
#   clip.sh --strip-comments FILE   drop leading '#' lines first (the prepared
#                                   comment bodies start with paste instructions)
#   clip.sh --show               print what is currently on the clipboard
#
# SPDX-License-Identifier: 0BSD
set -euo pipefail

qdbus=${QDBUS:-qdbus6}
command -v "$qdbus" >/dev/null || { printf 'qdbus6 not found\n' >&2; exit 1; }

if [[ ${1:-} == --show ]]; then
    "$qdbus" org.kde.klipper /klipper getClipboardContents
    exit 0
fi

strip=0
if [[ ${1:-} == --strip-comments ]]; then
    strip=1
    shift
fi

file=${1:?usage: clip.sh [--strip-comments] FILE}
[[ -r $file ]] || { printf 'cannot read %s\n' "$file" >&2; exit 1; }

if (( strip )); then
    # drop the '#' instruction block and the blank line that follows it
    content=$(grep -v '^#' "$file" | sed '/./,$!d')
else
    content=$(cat "$file")
fi

[[ -n $content ]] || { printf '%s is empty after filtering\n' "$file" >&2; exit 1; }

# Command substitution strips trailing newlines on both sides, so the comparison
# below is between equally normalised strings.
"$qdbus" org.kde.klipper /klipper setClipboardContents "$content" >/dev/null

back=$("$qdbus" org.kde.klipper /klipper getClipboardContents)
if [[ $back == "$content" ]]; then
    printf 'clipboard set and verified: %s (%d 字节, %d 行)\n' \
        "$file" "${#content}" "$(printf '%s\n' "$content" | wc -l)"
else
    printf 'clipboard write did not verify -- read back %d bytes, expected %d\n' \
        "${#back}" "${#content}" >&2
    exit 1
fi
