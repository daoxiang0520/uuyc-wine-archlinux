# uuyc-d3dprobe — D3D11 format-support probe

The UU Remote Windows client enumerates 50+ DXGI formats with
`ID3D11Device::CheckFormatSupport` before it starts a remote session. Wine
implements that method as a **partial stub**, and the client then abandons the
session, which shows up as a D3D11 device going to "removed" and the session
window closing with no picture.

This probe prints Wine's own answer for the formats that matter, so the question
"is Wine the limiting factor?" becomes a yes/no instead of an inference.

## Use the packaged binary

```bash
uuyc-wine-d3dprobe           # hardware device
uuyc-wine-d3dprobe --all     # hardware, then the software rasterizer
```

The full output is also saved to
`${XDG_STATE_HOME:-$HOME/.local/state}/uuyc-wine/d3dprobe.txt`.

## Rebuild after a Wine upgrade

```bash
sudo pacman -S mingw-w64-gcc 7zip
cd /usr/share/uuyc-wine/d3dprobe
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -o uuyc-d3dprobe.exe \
    uuyc-d3dprobe.c -ld3d11 -ldxgi
```

`INITGUID` is defined in the source, so no `-ldxguid` is needed.

## How to read the result

| Observation | Meaning |
| --- | --- |
| `NV12` / `P010` rows carry `DECODER_OUTPUT` | Wine claims a hardware decode output path; a session failure after this is a wined3d bug worth reporting |
| `NV12` / `P010` rows lack `DECODER_OUTPUT` | Wine has no hardware decode path at all: this is the root cause, and no client-side setting can work around it |
| `D3D11CreateDevice failed: 0x887A0004` | no usable adapter here (missing `/dev/dri`, or the software rasterizer is disabled) — check with `uuyc-wine-gpu` first |

Attach this output together with `uuyc-wine-crashlog` section 3a when reporting
upstream.

## Licence

0BSD, same as the rest of the packaging in this repository.
