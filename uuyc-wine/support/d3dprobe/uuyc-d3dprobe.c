/*
 * uuyc-d3dprobe.c -- ask Wine's D3D11 what video formats it really supports.
 *
 * Why this exists: the UU Remote Windows client enumerates 50+ DXGI formats
 * with ID3D11Device::CheckFormatSupport before it starts a remote session, and
 * Wine implements that method as a *partial stub*. The client then drops the
 * session and the D3D11 device goes to "removed", which is what makes the
 * session window close with no picture.
 *
 * This probe prints Wine's own answer for the formats that matter, so the
 * question "is Wine the limiting factor?" gets a yes/no instead of a guess.
 *
 * Build (Arch Linux):
 *   sudo pacman -S mingw-w64-gcc
 *   x86_64-w64-mingw32-gcc -O2 -o uuyc-d3dprobe.exe uuyc-d3dprobe.c \
 *       -ld3d11 -ldxgi
 * Run:
 *   wine uuyc-d3dprobe.exe            # hardware device
 *   wine uuyc-d3dprobe.exe --warp     # software rasterizer
 *   wine uuyc-d3dprobe.exe --all      # every driver type
 *
 * SPDX-License-Identifier: 0BSD
 */

/* Instantiate the GUIDs in this translation unit instead of relying on
 * -ldxguid: DEFINE_GUID only declares them unless INITGUID is defined. */
#define INITGUID
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <string.h>

/* Driver types, spelled out because older mingw headers may lack the enum. */
#define DRV_HARDWARE 0
#define DRV_WARP     5

struct format_entry {
    DXGI_FORMAT format;
    const char *name;
    int is_video;
};

static const struct format_entry formats[] = {
    { DXGI_FORMAT_NV12,          "NV12           ", 1 },
    { DXGI_FORMAT_P010,          "P010           ", 1 },
    { DXGI_FORMAT_P016,          "P016           ", 1 },
    { DXGI_FORMAT_YUY2,          "YUY2           ", 1 },
    { DXGI_FORMAT_420_OPAQUE,    "420_OPAQUE     ", 1 },
    { DXGI_FORMAT_AYUV,          "AYUV           ", 1 },
    { DXGI_FORMAT_B8G8R8A8_UNORM,"B8G8R8A8_UNORM ", 0 },
    { DXGI_FORMAT_R8G8B8A8_UNORM,"R8G8B8A8_UNORM ", 0 },
    { DXGI_FORMAT_R10G10B10A2_UNORM, "R10G10B10A2    ", 0 },
    { DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16F ", 0 },
};
static const unsigned format_count = sizeof(formats) / sizeof(formats[0]);

struct flag_name {
    UINT bit;
    const char *name;
};

static const struct flag_name flag_names[] = {
    { D3D11_FORMAT_SUPPORT_BUFFER,                        "BUFFER" },
    { D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER,               "IA_VERTEX_BUFFER" },
    { D3D11_FORMAT_SUPPORT_IA_INDEX_BUFFER,                "IA_INDEX_BUFFER" },
    { D3D11_FORMAT_SUPPORT_SO_BUFFER,                      "SO_BUFFER" },
    { D3D11_FORMAT_SUPPORT_TEXTURE1D,                      "TEXTURE1D" },
    { D3D11_FORMAT_SUPPORT_TEXTURE2D,                      "TEXTURE2D" },
    { D3D11_FORMAT_SUPPORT_TEXTURE3D,                      "TEXTURE3D" },
    { D3D11_FORMAT_SUPPORT_TEXTURECUBE,                    "TEXTURECUBE" },
    { D3D11_FORMAT_SUPPORT_SHADER_LOAD,                    "SHADER_LOAD" },
    { D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,                  "SHADER_SAMPLE" },
    { D3D11_FORMAT_SUPPORT_SHADER_SAMPLE_COMPARISON,       "SHADER_SAMPLE_COMPARISON" },
    { D3D11_FORMAT_SUPPORT_SHADER_SAMPLE_MONO_TEXT,        "SHADER_SAMPLE_MONO_TEXT" },
    { D3D11_FORMAT_SUPPORT_MIP,                            "MIP" },
    { D3D11_FORMAT_SUPPORT_MIP_AUTOGEN,                    "MIP_AUTOGEN" },
    { D3D11_FORMAT_SUPPORT_RENDER_TARGET,                  "RENDER_TARGET" },
    { D3D11_FORMAT_SUPPORT_BLENDABLE,                      "BLENDABLE" },
    { D3D11_FORMAT_SUPPORT_DEPTH_STENCIL,                  "DEPTH_STENCIL" },
    { D3D11_FORMAT_SUPPORT_CPU_LOCKABLE,                   "CPU_LOCKABLE" },
    { D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE,            "MULTISAMPLE_RESOLVE" },
    { D3D11_FORMAT_SUPPORT_DISPLAY,                        "DISPLAY" },
    { D3D11_FORMAT_SUPPORT_CAST_WITHIN_BIT_LAYOUT,         "CAST_WITHIN_BIT_LAYOUT" },
    { D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET,       "MULTISAMPLE_RENDERTARGET" },
    { D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD,               "MULTISAMPLE_LOAD" },
    { D3D11_FORMAT_SUPPORT_SHADER_GATHER,                  "SHADER_GATHER" },
    { D3D11_FORMAT_SUPPORT_BACK_BUFFER_CAST,               "BACK_BUFFER_CAST" },
    { D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW,    "TYPED_UAV" },
    { D3D11_FORMAT_SUPPORT_SHADER_GATHER_COMPARISON,       "SHADER_GATHER_COMPARISON" },
    { D3D11_FORMAT_SUPPORT_DECODER_OUTPUT,                 "DECODER_OUTPUT" },
    { D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT,         "VIDEO_PROCESSOR_OUTPUT" },
    { D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT,          "VIDEO_PROCESSOR_INPUT" },
    { D3D11_FORMAT_SUPPORT_VIDEO_ENCODER,                  "VIDEO_ENCODER" },
};
static const unsigned flag_count = sizeof(flag_names) / sizeof(flag_names[0]);

static void print_flags(UINT flags)
{
    unsigned i;
    int printed = 0;

    printf("        raw = 0x%08X\n", flags);
    if (flags == 0) {
        printf("        (nothing claimed)\n");
        return;
    }
    printf("       ");
    for (i = 0; i < flag_count; i++) {
        if (flags & flag_names[i].bit) {
            printf(" %s", flag_names[i].name);
            printed++;
            if ((printed % 3) == 0)
                printf("\n       ");
        }
    }
    printf("\n");
}

/* Wine returns DXGI/D3D HRESULTs for device creation; decode the common ones so
 * the output does not need a lookup table on the reader's side. */
static const char *hresult_name(HRESULT hr)
{
    switch ((unsigned long)hr) {
    case 0x887A0001: return "DXGI_ERROR_INVALID_CALL";
    case 0x887A0002: return "DXGI_ERROR_NOT_FOUND";
    case 0x887A0004: return "DXGI_ERROR_UNSUPPORTED (no such driver/format)";
    case 0x887A0005: return "DXGI_ERROR_DEVICE_REMOVED";
    case 0x887A0006: return "DXGI_ERROR_DEVICE_HUNG";
    case 0x887A0007: return "DXGI_ERROR_DEVICE_RESET";
    case 0x887A0020: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    case 0x80070057: return "E_INVALIDARG";
    case 0x80004005: return "E_FAIL";
    case 0x8007000E: return "E_OUTOFMEMORY";
    case 0x80070005: return "E_ACCESSDENIED";
    default:         return "(unlisted HRESULT)";
    }
}

static const char *driver_type_name(UINT type)
{
    switch (type) {
    case DRV_HARDWARE: return "HARDWARE";
    case DRV_WARP:     return "WARP (software)";
    default:           return "OTHER";
    }
}

static int probe_driver(unsigned driver_type)
{
    ID3D11Device *device = NULL;
    IDXGIDevice *dxgi_device = NULL;
    IDXGIAdapter *adapter = NULL;
    DXGI_ADAPTER_DESC desc;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                   D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got = 0;
    HRESULT hr;
    unsigned i;
    int video_decode_ok = 0, video_proc_ok = 0;

    printf("\n=== D3D_DRIVER_TYPE_%s ===\n", driver_type_name(driver_type));

    hr = D3D11CreateDevice(NULL, driver_type, NULL, 0, levels,
                           sizeof(levels) / sizeof(levels[0]),
                           D3D11_SDK_VERSION, &device, &got, NULL);
    if (FAILED(hr) || !device) {
        printf("  D3D11CreateDevice failed: 0x%08lX  %s\n",
               (unsigned long)hr, hresult_name(hr));
        if (device) ID3D11Device_Release(device);
        return 1;
    }
    printf("  device created, feature level 0x%04X\n", (unsigned)got);

    hr = ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi_device);
    if (SUCCEEDED(hr) && dxgi_device) {
        if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi_device, &adapter)) && adapter) {
            ZeroMemory(&desc, sizeof(desc));
            if (SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &desc)))
                printf("  adapter: %ls  vendor 0x%04X device 0x%04X\n",
                       desc.Description, desc.VendorId, desc.DeviceId);
            IDXGIAdapter_Release(adapter);
        }
        IDXGIDevice_Release(dxgi_device);
    }

    printf("\n  %-16s %-10s %s\n", "FORMAT", "SUPPORT", "FLAGS");
    for (i = 0; i < format_count; i++) {
        UINT flags = 0;
        hr = ID3D11Device_CheckFormatSupport(device, formats[i].format, &flags);
        if (FAILED(hr)) {
            printf("  %-16s FAILED     0x%08lX  %s\n",
                   formats[i].name, (unsigned long)hr, hresult_name(hr));
            continue;
        }
        printf("  %-16s 0x%08X\n", formats[i].name, flags);
        print_flags(flags);
        if (formats[i].is_video) {
            if (flags & D3D11_FORMAT_SUPPORT_DECODER_OUTPUT)
                video_decode_ok = 1;
            if (flags & D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT)
                video_proc_ok = 1;
        }
    }

    printf("\n  summary for this device:\n");
    printf("    video decoder output format supported : %s\n", video_decode_ok ? "YES" : "NO");
    printf("    video processor output format supported: %s\n", video_proc_ok ? "YES" : "NO");

    ID3D11Device_Release(device);
    return 0;
}

int main(int argc, char **argv)
{
    int all = 0, warp_only = 0, i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--all")) all = 1;
        else if (!strcmp(argv[i], "--warp")) warp_only = 1;
        else if (!strcmp(argv[i], "--help")) {
            printf("usage: %s [--warp | --all]\n", argv[0]);
            return 0;
        }
    }

    printf("uuyc-d3dprobe -- what does this D3D11 implementation claim?\n");
    printf("(Wine answers CheckFormatSupport as a partial stub; this prints that answer)\n");

    if (all) {
        probe_driver(DRV_HARDWARE);
        probe_driver(DRV_WARP);
    } else if (warp_only) {
        probe_driver(DRV_WARP);
    } else {
        probe_driver(DRV_HARDWARE);
    }

    printf("\nHow to read this:\n");
    printf("  NV12 / P010 with DECODER_OUTPUT missing  -> no hardware decode path;\n");
    printf("      a client that needs one will fail its session handshake.\n");
    printf("  Formats listed with real flags           -> Wine negotiates normally.\n");
    return 0;
}
