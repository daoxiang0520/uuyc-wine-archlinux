# uuyc-vdshim — D3D11 video-decode probe shim

A proxy `d3d11.dll` that lets a Windows application believe a hardware H.264
decoder exists, purely so that its **next** calls become visible in a log. It
decodes nothing.

## Why

Wine's D3D11 video decode only works through Vulkan Video. On Intel Gen9.5 the
Vulkan driver exposes no `VK_KHR_video_*` extension, so
`ID3D11VideoDevice::GetVideoDecoderProfileCount()` returns 0 and the application
aborts the remote session before saying what it actually wanted.

This shim answers:

* does the application go on to `CreateVideoDecoder` / `CreateVideoDecoderOutputView` /
  `SubmitDecoderBuffers` once it is told a profile exists?
* with which decoder profile, output format, resolution and config flags?

The answer decides whether writing a real VA-API or software decoder backend is
worth the effort (roughly 1200–2500 lines) or not.

## What it intercepts

| Call | Behaviour |
| --- | --- |
| `D3D11CreateDevice`, `D3D11CreateDeviceAndSwapChain` | forwarded to Wine's builtin, then the returned device is wrapped |
| device `QueryInterface` | `IID_ID3D11VideoDevice` returns the shim's implementation; everything else is forwarded unchanged |
| device `AddRef` / `Release` | reference counted; the wrapped device is released with the proxy |
| device `CheckFormatSupport` | for NV12/P010/P016/420_OPAQUE/P208 it answers `TEXTURE2D \| SHADER_SAMPLE \| DECODER_OUTPUT \| VIDEO_PROCESSOR_OUTPUT` instead of Wine's rendering-format answer |
| `ID3D11VideoDevice` (20 vtable slots) | profile count/list, `CheckVideoDecoderFormat` are answered by the shim; every other method is forwarded to Wine's implementation and logged |

Only three vtable slots of the device are replaced, so all other methods run
Wine's code with the `this` pointer the application passed in.

## First real-capture result

Deployed as `bin/d3d11.dll` with `d3d11=n,b`, the shim reached
`bin/GameViewerServer.exe` and recorded:

```text
==== D3D11CreateDevice driver_type=0 flags=0x120 levels=2 ====   (0 = HARDWARE)
[video] GetVideoDecoderProfileCount: wine=0 shim=2
[video] GetVideoDecoderProfile(0) -> H264_VLD_NOFGT
[video] GetVideoDecoderProfile(1) -> H264_VLD_FGT
[video] CheckVideoDecoderFormat profile=H264_VLD_NOFGT fmt=Y210(103) [translated]
        translation: NV12 (legacy 103 -> wine 87)
[video] GetVideoDecoderConfigCount -> wine=E_NOTIMPL, shim=1
```

Two blockers are visible, and both are fixable in the shim:

1. the application uses the **legacy** DXGI numbering (NV12 = 103) while Wine uses the
   current one (NV12 = 87), so the shim translates before forwarding;
2. Wine's `GetVideoDecoderConfigCount` returns `E_NOTIMPL`, so the shim publishes one
   documented H.264 configuration to let the caller proceed.

The remaining unknown is what `CreateVideoDecoder` receives, which is what the shim now
exists to capture.

## Important: DXGI_FORMAT numbering

Do **not** use mingw-w64's `DXGI_FORMAT_*` enum values when talking to Wine.
mingw's `dxgiformat.h` still carries the pre-DXGI-1.2 numbering
(`DXGI_FORMAT_NV12 = 0x67 = 103`), while Wine follows the current public
numbering (`NV12 = 87`). Verified on the target by asking Wine itself:

| value | mingw name | Wine's answer |
| --- | --- | --- |
| 87 | `B8G8R8A8_UNORM` | `S_OK`, flags `0x02E4F3F3` (ordinary texture) |
| 88 | `B8G8R8X8_UNORM` | `S_OK`, flags `0x00E4F3F1` |
| 93 | (unassigned) | `S_OK`, flags `0x00E4F3F1` |
| 103 | `NV12` | `E_FAIL` |
| 104 | `P010` | `E_FAIL` |
| 100 | `AYUV` | `E_FAIL` |
| 107 | `YUY2` | `S_OK`, flags `0x02000000` |

The source hardcodes Wine's values for this reason.

## Build

```bash
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -shared \
  -o uuyc-vdshim.dll uuyc-vdshim.c -ld3d11 -ldxguid -lole32
```

Self-test (links against the shim, so a pass proves the intercept path):

```bash
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -shared -o uuyc-vdshim.dll uuyc-vdshim.c \
  -ld3d11 -ldxguid -lole32 -Wl,--out-implib,libuuyc-vdshim.a
x86_64-w64-mingw32-gcc -O2 -o testshim.exe testshim.c -L. -luuyc-vdshim -ld3d11 -ldxguid
WINEPREFIX=<prefix> WINEDLLOVERRIDES='uuyc-vdshim=n,b' wine testshim.exe
```

Expected output:

```text
D3D11CreateDevice -> 0  device=...
QueryInterface(ID3D11VideoDevice) -> 0  vd=...
GetVideoDecoderProfileCount -> 2
GetVideoDecoderProfile(0) -> 0  {1b81be68-a0c7-...}      (H264_VLD_NOFGT)
CheckVideoDecoderFormat(NV12) -> 0  supported=1
CheckFormatSupport(Wine NV12=87) -> 0  flags=0x18000220
CheckFormatSupport(Wine P010=104) -> 0  flags=0x18000220
```

## Deploy against the real application

Use the helper (it sets the override on the right binaries):

```bash
bash deploy.sh install      # copies the dll and sets the per-app overrides
bash deploy.sh status       # shows dll, overrides and log state
bash deploy.sh remove       # undoes both
```

### Why `bin/GameViewer.exe`, not the top-level one

The import chain decides which process must be overridden:

```text
d3d11.dll, dxgi.dll   <- imported by  bin/streamer.dll
bin/streamer.dll      <- imported by  bin/GameViewer.exe
                                     bin/GameViewerServer.exe
top-level GameViewer.exe  imports neither d3d11 nor streamer
```

So overriding the top-level executable loads nothing at all — the shim is never
even looked for, and no log appears. That mistake cost a round of debugging; it
is recorded here on purpose.

The DLL must sit next to each overridden executable (`bin/uuyc-vdshim.dll`), and
`d3d11` must stay builtin so the shim can forward to it.

### Manual equivalent

```bash
PREFIX=~/.local/share/uuyc-wine/wineprefix
ROOT="$PREFIX/drive_c/Program Files/Netease/GameViewer"
cp uuyc-vdshim.dll "$ROOT/bin/"

for exe in 'bin/GameViewer.exe' 'bin/GameViewerServer.exe'; do
    WINEPREFIX=$PREFIX wine reg add \
        "HKCU\\Software\\Wine\\AppDefaults\\${exe}\\DllOverrides" \
        /v uuyc-vdshim /t REG_SZ /d native /f
done
```

Then start the application through the launcher so that the service, supervisor
and server come up in the right order:

```bash
export WINEDLLOVERRIDES='uuyc-vdshim=n,b'
uuyc-wine
```

### If the log still does not appear

Ask Wine which module it actually loads:

```bash
PREFIX=~/.local/share/uuyc-wine/wineprefix
WINEPREFIX=$PREFIX WINEDEBUG=+loaddll WINEDLLOVERRIDES='uuyc-vdshim=n,b' \
  wine "$PREFIX/drive_c/Program Files/Netease/GameViewer/bin/GameViewer.exe" 2>&1 \
  | grep -iE 'vdshim|d3d11\.dll' | head
```

* `Loaded ... uuyc-vdshim.dll ... native` — the shim is in place; read the log.
* only `Loaded ... d3d11.dll ... builtin` — the override did not apply to that
  process; check that the DLL sits in the same directory as the executable.

The log is written to **three** places, so that a missing log unambiguously means
"the shim was never loaded" rather than "it wrote somewhere else":

```text
<exe directory>/uuyc-vdshim.log      e.g. .../GameViewer/bin/uuyc-vdshim.log
Z:\tmp\uuyc-vdshim.log               the host's /tmp/uuyc-vdshim.log
%TEMP%\uuyc-vdshim.log

Environment switches:

| Variable | Effect |
| --- | --- |
| `UUYC_VD_SHIM_PROFILES=0` | report an empty profile list (control run, expected to abort early) |
| `UUYC_VD_SHIM_FORMATS=0` | do not patch `CheckFormatSupport` |
| `UUYC_VD_SHIM_QUIET=1` | log only video calls, not every format query |

## Removing it

```bash
WINEPREFIX=$PREFIX wine reg delete 'HKCU\Software\Wine\AppDefaults\GameViewer.exe\DllOverrides' /v uuyc-vdshim /f
rm "$APPDIR/uuyc-vdshim.dll"
```

## Limits this shim does not overcome

* It does not decode. Once the application submits bitstream data, it will fail
  — the point is to see *how* it fails and what it asked for first.
* Wine's video processor has no implementation on any backend, so pipelines that
  need `VideoProcessorBlt` will still fail.
* It is a diagnostic, not a fix. A real fix is either a VA-API / software decoder
  backend in wined3d (upstream work) or using the Android client through
  Waydroid.

## Licence

0BSD, same as the rest of this repository.
