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
#   clip.sh FILE                    put FILE on the clipboard
#   clip.sh -                       put stdin on the clipboard
#   clip.sh --strip-comments FILE   drop the leading '#' instruction block first
#   clip.sh --show                  print what is currently on the clipboard
#
# From inside nvim:
#   :%w !clip.sh --strip-comments -     copy the whole buffer
#   :'<,'>w !clip.sh -                  copy the visual selection
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

file=${1:?usage: clip.sh [--strip-comments] FILE|-}

if [[ $file == - ]]; then
    content=$(cat)
    label="<stdin>"
else
    [[ -r $file ]] || { printf 'cannot read %s\n' "$file" >&2; exit 1; }
    content=$(cat "$file")
    label=$file
fi

if (( strip )); then
    # drop the '#' instruction block and the blank lines that follow it
    content=$(printf '%s\n' "$content" | grep -v '^#' | sed '/./,$!d')
fi

[[ -n $content ]] || { printf '%s is empty after filtering\n' "$label" >&2; exit 1; }

# Command substitution strips trailing newlines on both sides, so the comparison
# below is between equally normalised strings.
"$qdbus" org.kde.klipper /klipper setClipboardContents "$content" >/dev/null

back=$("$qdbus" org.kde.klipper /klipper getClipboardContents)
if [[ $back == "$content" ]]; then
    printf "clipboard set and verified: %s (%d 字节, %d 行)\n" \
        "$label" "${#content}" "$(printf '%s\n' "$content" | wc -l)"
else
    printf 'clipboard write did not verify -- read back %d bytes, expected %d\n' \
        "${#back}" "${#content}" >&2
    exit 1
fi
