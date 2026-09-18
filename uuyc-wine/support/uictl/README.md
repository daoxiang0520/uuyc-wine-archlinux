# uuyc-uictl

Drive a Wine application's windows from inside the prefix.

## Why

When the host session is locked, the X screen is covered by the screen locker:
`spectacle`/`ffmpeg` capture only the locker, and injected input goes to the
locker instead of the application. Win32 is unaffected by that, so this tool
works through Wine itself.

## Commands

```
uictl list                                   top-level windows: hwnd, pid, class, visibility, client size, title
uictl tree   <match|0xhwnd>                  recursive child-window tree
uictl a11y   <match|0xhwnd> [depth]          walk the MSAA accessibility tree
uictl shot   <match|0xhwnd> <out.bmp> [print|blt]
uictl click  <match|0xhwnd> <x> <y>          post a click at client-relative coordinates
uictl dclick <match|0xhwnd> <x> <y>
uictl move   <match|0xhwnd> <x> <y>
uictl text   <match> <string>                post WM_CHAR per character
uictl key    <match> <vk-decimal>            post WM_KEYDOWN/WM_KEYUP
```

`<match>` is case-insensitively matched against the window title; `*` matches any
titled window. An explicit `0x...` hwnd targets one window exactly.

Run it inside the same prefix as the application:

```bash
WINEPREFIX=<prefix> wine ./uuyc-uictl.exe list
```

## Two things that are easy to get wrong

* **Titles must be printed as UTF-8.** `GetWindowTextA` returns the ANSI code page,
  so a Chinese title becomes `??????` and the useful part of the tree is lost.
  This tool converts with `WideCharToMultiByte(CP_UTF8)`.
* **Use `blt`, not `print`, for Chromium/WebView2 surfaces.** Chromium does not
  answer `WM_PRINT`, so `PrintWindow` returns success with an all-white bitmap.

## Known limits (honest)

* WebView2 content here is composited through **DirectComposition**, which never
  lands in a window DC, so neither `PrintWindow` nor `BitBlt` can capture it.
* Chromium does not answer `WM_GETOBJECT` for its **hidden** Chrome windows, so
  `a11y` returns Wine's standard fallback object (`childCount=0`) rather than the
  page's tree.

Window enumeration, titles, geometry and input injection all work; seeing the
pixels does not. If the host session is unlocked, ordinary host-side capture is
the simpler route and this tool is only needed for the input side.

## Build

```bash
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -o uuyc-uictl.exe uuyc-uictl.c \
    -luser32 -lgdi32 -loleacc -lole32 -loleaut32 -luuid
```

SPDX-License-Identifier: 0BSD
