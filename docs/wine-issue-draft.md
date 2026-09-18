# Wine issue draft — D3D11 video decode has no fallback when the driver does not expose Vulkan Video

**Status:** ready to submit. The evidence bundle is in `wine-issue-evidence/`.

## Where to submit

Wine's bug tracker is GitLab (the old Bugzilla is retired):

1. Create or sign in to a [gitlab.winehq.org](https://gitlab.winehq.org) account.
2. Open a new issue on **wine / wine** → *Issues* → *New issue*:
   <https://gitlab.winehq.org/wine/wine/-/issues/new>
3. Paste the sections below (from "Title" through "Questions for maintainers") as the
   issue body.
4. Attach the files produced by `wine-issue-evidence/collect.sh`:
   `00-host.txt`, `01-vulkaninfo.txt`, `02-vainfo.txt`, `03-d3dprobe.txt`,
   `05-wine-log-excerpt.txt`, plus `uuyc-d3dprobe.c` so maintainers can build the probe.
5. Tag it `d3d11` if the label exists; otherwise leave labels alone.

Notes on tone that matter for Wine: the report states observations and asks questions
rather than asserting what the fix should be, and it does not claim the application is
correct — it reports what Wine's D3D11 layer returns and why that is unusable on this
hardware class.

---

## Title

`d3d11`/`wined3d`: `GetVideoDecoderProfileCount()` returns 0 and D3D11VA applications abort on GPUs without `VK_KHR_video_decode_*` support (e.g. Intel Gen9.5)

---

## Summary

Wine's D3D11 video decode stack is implemented only on top of Vulkan Video. When the
Vulkan driver does not expose the `VK_KHR_video_*` extensions — which is the normal
case on Intel Gen9.5 (Skylake/Kaby/Coffee/Comet Lake), because anv gates Vulkan Video
to Gen12+ — `ID3D11VideoDevice::GetVideoDecoderProfileCount()` returns 0.

A Windows application that enumerates decoder profiles and treats "no profile" as fatal
then aborts. The GPU itself is fully capable of H.264/HEVC decode on this hardware; the
hardware exposes it through VA-API (`iHD_drv_video.so`), which Wine's D3D11 video path
does not use. So on such systems there is no working path at all, and no error beyond
"0 profiles" that an application could react to sanely.

This report contains the observations only; whether Wine wants a VA-API based backend,
a software decode fallback, or just different behaviour when no Vulkan Video support is
present is a maintainer decision.

## Environment

```text
Wine        : wine-11.17 (Arch Linux `extra` package, built from wine 11.17)
Kernel      : 7.x, x86_64
GPU         : Intel CometLake-U GT2 [UHD Graphics], PCI 8086:9b41, i915 driver
Mesa        : 26.2.2, anv (libvulkan_intel.so)
Vulkan ICD  : /usr/share/vulkan/icd.d/intel_icd.json  (api_version 1.4.354)
VA-API      : intel-media-driver 26.2.4 (iHD_drv_video.so present), libva 2.24.1
Session     : Wayland (Xwayland on :1 for the Wine app)
Renderer    : wined3d Vulkan backend forced via HKCU\Software\Wine\Direct3D renderer=vulkan
```

## Steps to reproduce

1. On a machine whose Vulkan driver exposes no `VK_KHR_video_decode_*` extensions
   (Intel Gen9.5 is one such machine), create a D3D11 device.
2. Query the video interfaces:

```c
D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, ..., &device, ...);
ID3D11VideoDevice *vd;
device->QueryInterface(&IID_ID3D11VideoDevice, (void **)&vd);
UINT count = 0, profiles[16];
vd->GetVideoDecoderProfileCount(&count);          /* observed: 0 */
vd->GetVideoDecoderProfile(0, profiles);          /* observed: fails, count is 0 */
```

3. Query the decode output formats:

```c
UINT flags = 0;
device->CheckFormatSupport(DXGI_FORMAT_NV12, &flags);   /* observed: E_FAIL */
device->CheckFormatSupport(DXGI_FORMAT_P010, &flags);   /* observed: E_FAIL */
```

## Observed behaviour

### 1. Wine's Vulkan backend is active and working for rendering

With `renderer=vulkan` set, wined3d uses the Vulkan adapter and D3D rendering works
normal render targets are reported with full support:

```text
trace:d3d:wined3d_adapter_vk_init adapter_vk 00007FFFFF51A280, ordinal 0, ...
adapter_vk_* occurrences in the log : 2422
adapter_gl_* occurrences in the log : 0
```

### 2. But wined3d enables no video extensions, because the driver does not expose them

```text
occurrences of "VK_KHR_video_queue" / "VK_KHR_video_decode_queue" /
"VK_KHR_video_decode_h264" in wined3d's reported device-extension list : 0
total video-related entries in that list : 0
```

`/usr/lib/libvulkan_intel.so` contains the extension name strings (they are compiled in),
but anv does not advertise them on this GPU generation. `vulkaninfo` on the same machine
likewise lists no `VK_KHR_video_*` extension.

### 3. Consequently profile enumeration returns nothing

From a real application (UU Remote, `gameviewer.exe`) that opens a remote-desktop
session, with `WINEDEBUG=+d3d,+d3d11,+dxgi,+wined3d`:

```text
trace:d3d:wined3d_device_get_video_decode_profile_count device 00007FFFFE83BF20.   (16 times)
fixme:d3d11:d3d11_device_CheckFormatSupport iface 00007FFFFEF48830, format 87, ...   partial-stub!
warn:d3d11:d3d11_device_GetDeviceRemovedReason iface 00007FFFFEF48830 stub!
warn:d3d:wined3d_swapchain_cleanup Something's still holding back buffer 0 (00007FFFFF131120).
```

The application queried 57 distinct format values, clustered like this (counts in
parentheses): 2 (5), 10 (5), 24 (3), 28 (8), 29 (5), 45 (4), 56 (2), 58 (2),
**87 (8), 88 (4), 93 (4)**, 103 (5), 115 (5).

The application creates a session window (swapchain 953x1034), presents a few frames,
then the device goes to "removed" and the window closes. `GetDeviceRemovedReason` being
a stub means the application cannot even read the reason.

### 3b. Wine's answers for the video format values, measured directly

A probe that hardcodes the format values (so a header's enum numbering cannot skew
the result) shows which values Wine accepts and what it reports for them:

```text
  value  mingw/old-SDK name   current public name   Wine HRESULT   Wine flags
     28  R8G8B8A8_UNORM       R8G8B8A8_UNORM        S_OK           0x02E4F3F3
     87  B8G8R8A8_UNORM       NV12                  S_OK           0x02E4F3F3
     88  B8G8R8X8_UNORM       420_OPAQUE            S_OK           0x00E4F3F1
     93  (unassigned)         P208                  S_OK           0x00E4F3F1
    100  AYUV                 P010                  E_FAIL         0x00000000
    103  NV12                 Y210                  E_FAIL         0x00000000
    104  P010                 Y216                  E_FAIL         0x00000000
    107  YUY2                 IA44                  S_OK           0x02000000
    115  (unassigned)         P016                  S_OK           0x008013F1
    999  (invalid)            (invalid)             E_FAIL         0x00000000
```

Two things stand out:

1. Wine follows the **current public** numbering (`NV12 = 87`, `420_OPAQUE = 88`,
   `P208 = 93`), not the pre-DXGI-1.2 numbering that mingw-w64's `dxgiformat.h`
   still carries (`NV12 = 103`). The probe therefore uses hardcoded values.
2. For `87` (NV12) and `88` (420_OPAQUE) Wine answers `S_OK` with a **plain
   texture / render-target capability mask** (`0x02E4F3F3` = BUFFER,
   IA_VERTEX_BUFFER, TEXTURE2D, SHADER_LOAD, SHADER_SAMPLE, RENDER_TARGET,
   BLENDABLE, ...) and **no `DECODER_OUTPUT` / `VIDEO_PROCESSOR_OUTPUT` bit**.
   `CheckFormatSupport` is a `partial-stub!`, which is consistent with the answer
   coming from a generic table rather than from video capability probing.

So an application that asks "can NV12 be a decoder output?" is told "NV12 is a
render target, no".

### 4. A standalone probe shows the same, and shows that plain rendering formats are fine

A small program using `ID3D11Device::CheckFormatSupport` (source is short enough to
paste if wanted):

With the format values hardcoded as above:

```text
FORMAT           SUPPORT    FLAGS
NV12   (87)      0x02E4F3F3  render-target mask, NO DECODER_OUTPUT
420_OPAQUE (88)  0x00E4F3F1  same class of answer
P208   (93)      0x00E4F3F1
P010  (100)      E_FAIL
P016  (115)      S_OK but flags 0x008013F1
YUY2  (107)      S_OK but flags 0x02000000
R8G8B8A8_UNORM   0x02E4F3F3
R10G10B10A2      0x02E4F3F3
R16G16B16A16F    0x02E4F3F3

video decoder output format supported : NO
video processor output format supported : NO
```

Rendering formats answer completely; no video format ever reports
`D3D11_FORMAT_SUPPORT_DECODER_OUTPUT` or `..._VIDEO_PROCESSOR_OUTPUT`.

## Analysis of the current source layout (for reference, not a request)

From `strings`/symbol inspection of the installed Wine build:

| Backend | Video decoder implementation present |
| --- | --- |
| `adapter_vk` | `wined3d_decoder_vk_create`, `_decode_h264`, `_is_h264_decode_supported`, `_create_image`, `_create_layered_image`, `_cs_init`, `_prepare_image` — a full Vulkan Video H.264 decode path |
| `adapter_gl` | only `adapter_gl_create_video_decoder_output_view` / `_destroy_...` |
| `adapter_no3d` | only `adapter_no3d_create_video_decoder_output_view` / `_destroy_...` |

and no `adapter_*_*video_processor*` symbols on any of the three backends.

`wined3d.so` does contain a VA-API helper set loaded with `dlopen`
(`open_va_display`, `vaGetDisplayDRM`, `vaInitialize`, `vaQueryConfigProfiles`,
`vaCreateConfig`, `vaCreateContext`, `vaCreateSurfaces`, `vaCreateBuffer`,
`vaBeginPicture`, `vaRenderPicture`, `vaEndPicture`, `vaSyncSurface`,
`vaExportSurfaceHandle`, and functions named `va_decoder_create_vk` / `va_decoder_decode`),
i.e. VA-API is currently used as a helper for the Vulkan path rather than as an
independent decode backend.

Relevant upstream work for context: MR7480 "wined3d: Vulkan H.264 decode" (the
implementation this build already contains).

## Why this matters beyond one application

Any Windows application built on the D3D11VA / Media Foundation hardware-decode path
(recent remote-desktop and streaming clients, media players, capture tools) will see
zero decoder profiles on such a machine and either abort like the application above or
silently fall back to software. Because `GetDeviceRemovedReason` is also a stub, the
failure is very hard for an application (or a user) to diagnose.

## Questions for maintainers

1. Is a VA-API based D3D11 video decode backend planned, or is Vulkan Video intended to
   be the only implementation?
2. If Vulkan Video is the only path, would a patch be accepted that makes the situation
   visible rather than silent — for example `CheckFormatSupport` returning
   `DXGI_FORMAT_SUPPORT_DECODER_OUTPUT` only when a real backend is available (it already
   does) plus a `wined3d` warning on the `d3d` channel, and/or implementing
   `GetDeviceRemovedReason` so applications can see `DXGI_ERROR_UNSUPPORTED` instead of a
   stub value?
3. Is the D3D11 video processor intended to be implemented independently of Vulkan Video?
   A software video-processor fallback would at least keep pipelines alive on GPUs
   without Vulkan Video.

## Attachments (generated by `wine-issue-evidence/collect.sh`)

| File | Content |
| --- | --- |
| `00-host.txt` | kernel, Wine version and package, GPU + PCI id, DRM nodes, Mesa/Vulkan/VA-API packages, renderer registry value |
| `01-vulkaninfo.txt` | `vulkaninfo --summary` plus every extension containing "video" (expected: none) |
| `02-vainfo.txt` | `vainfo` output — the hardware decode capability the GPU *does* have via VA-API |
| `03-d3dprobe.txt` | `uuyc-d3dprobe --all`: `CheckFormatSupport` for NV12/P010/P016/420_OPAQUE/AYUV/YUY2 and the plain render formats |
| `05-wine-log-excerpt.txt` | the filtered wined3d/d3d11 log excerpts quoted above (the raw log is ~6.9 MB of unrelated application noise) |
| `uuyc-d3dprobe.c` | probe source, ~246 lines; builds with `x86_64-w64-mingw32-gcc -O2 -o uuyc-d3dprobe.exe uuyc-d3dprobe.c -ld3d11 -ldxgi` |

## Pre-submission checklist

- [ ] Ran `wine-issue-evidence/collect.sh` **in a normal terminal** (not inside a
      sandbox/container), so that `/dev/dri` is visible and the probe reaches the
      hardware adapter.
- [ ] `03-d3dprobe.txt` shows a created device and the NV12 row; if it shows
      `D3D11CreateDevice failed: 0x887A0004`, the probe did not reach hardware and the
      output must not be attached as-is.
- [ ] Skimmed `00-host.txt` for anything private before posting.
- [ ] Retested with the newest Wine (`wine --version`) so the report is not against an
      outdated build; Wine moves fast and MR7480 landed recently.
