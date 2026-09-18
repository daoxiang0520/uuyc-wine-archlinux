/*
 * uuyc-vdshim.c -- D3D11 video-decode probe shim.
 *
 * Purpose
 * -------
 * Wine's D3D11 video decode only works through Vulkan Video. On Intel Gen9.5 the
 * Vulkan driver exposes no VK_KHR_video_* extensions, so
 * ID3D11VideoDevice::GetVideoDecoderProfileCount() returns 0, the application
 * sees "no hardware decoder" and aborts the session.
 *
 * This shim does NOT decode anything. It forwards D3D11CreateDevice* to Wine's
 * builtin d3d11, wraps the returned device so that QueryInterface for
 * ID3D11VideoDevice hands out an implementation whose profile list is populated
 * instead of empty, and logs every video call the application makes.
 *
 * That log answers the question this whole investigation needs: once the
 * application is told a decoder profile exists, does it go on to
 * CreateVideoDecoder / CreateVideoDecoderOutputView / SubmitDecoderBuffers --
 * and with which parameters? If yes, a real VA-API or software decoder backend
 * is worth writing; if no, it is not.
 *
 * It is deliberately non-invasive: the proxy only replaces QueryInterface,
 * AddRef and Release on the device, everything else still goes straight to
 * Wine's implementation, and the override is registered per-application.
 *
 * Build
 * -----
 *   x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -shared \
 *     -o uuyc-vdshim.dll uuyc-vdshim.c -ld3d11 -ldxguid -lole32
 *
 * Deploy (see README in this directory)
 * ------
 *   copy uuyc-vdshim.dll into .../GameViewer/bin/
 *   WINEDLLOVERRIDES="uuyc-vdshim=n,b" wine GameViewer.exe
 * and read uuyc-vdshim.log afterwards.
 *
 * Environment switches (optional)
 * -------------------------------
 *   UUYC_VD_SHIM_PROFILES=0     report an empty profile list (control run)
 *   UUYC_VD_SHIM_FORMATS=0      do not alter CheckFormatSupport results
 *   UUYC_VD_SHIM_QUIET=1        log only the interesting calls
 *
 * SPDX-License-Identifier: 0BSD
 */

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "uuyc-vdshim-forwarders.h"

/* ------------------------------------------------------------------ logging -- */

static FILE *log_files[3] = { NULL, NULL, NULL };
static int quiet = 0;

/* Open the log in several places on purpose: the application may be started
 * from a different directory than its executable lives in, and a missing log
 * would look exactly like "the shim never loaded". Z:\tmp is the host's /tmp,
 * which is always findable. */
static void open_one(const WCHAR *path)
{
    FILE *f;
    int i;

    for (i = 0; i < 3; i++)
        if (!log_files[i])
            break;
    if (i == 3 || !path)
        return;
    f = _wfopen(path, L"a");
    if (f)
        log_files[i] = f;
}

static void shim_open_log(void)
{
    WCHAR path[MAX_PATH];
    DWORD n;
    static int done = 0;

    if (done)
        return;
    done = 1;

    /* 1. next to the running executable */
    n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        WCHAR *slash = wcsrchr(path, L'\\');
        if (slash && wcslen(path) + 32 < MAX_PATH) {
            slash[1] = 0;
            wcscat(path, L"uuyc-vdshim.log");
            open_one(path);
        }
    }
    /* 2. Z:\tmp on the host */
    open_one(L"Z:\\tmp\\uuyc-vdshim.log");

    /* 3. TEMP */
    {
        WCHAR tmp[MAX_PATH];
        DWORD len = GetTempPathW(MAX_PATH, tmp);
        if (len && len + 32 < MAX_PATH) {
            wcscat(tmp, L"uuyc-vdshim.log");
            open_one(tmp);
        }
    }
}

static void logf_(const char *fmt, ...)
{
    va_list ap;
    int i, any = 0;

    shim_open_log();
    for (i = 0; i < 3; i++) {
        if (!log_files[i])
            continue;
        any = 1;
        va_start(ap, fmt);
        vfprintf(log_files[i], fmt, ap);
        va_end(ap);
        fputc('\n', log_files[i]);
        fflush(log_files[i]);
    }
    (void)any;
}

static const char *hr_text(HRESULT hr)
{
    switch ((unsigned long)hr) {
    case S_OK:            return "S_OK";
    case E_FAIL:          return "E_FAIL";
    case E_INVALIDARG:    return "E_INVALIDARG";
    case E_OUTOFMEMORY:   return "E_OUTOFMEMORY";
    case E_NOTIMPL:       return "E_NOTIMPL";
    case 0x887A0001:      return "DXGI_ERROR_INVALID_CALL";
    case 0x887A0004:      return "DXGI_ERROR_UNSUPPORTED";
    case 0x887A0005:      return "DXGI_ERROR_DEVICE_REMOVED";
    default:              return "?";
    }
}

/* IMPORTANT: do not use the mingw DXGI_FORMAT_* enum values here.
 *
 * mingw-w64's dxgiformat.h still carries the pre-DXGI-1.2 numbering
 * (DXGI_FORMAT_NV12 = 0x67 = 103), while Wine follows the current public
 * numbering (DXGI_FORMAT_NV12 = 87). The names below are hardcoded to Wine's
 * numbering, verified on the target by asking Wine itself: 87/88/93/115 answer
 * S_OK, while 103/104/100 answer E_FAIL.
 */
/* ---------------------------------------------------------------------------
 * Format numbering in the wild
 *
 * The application sends NV12 as 103, which is the pre-DXGI-1.2 numbering; Wine
 * follows the current numbering, where NV12 is 87. Neither side is wrong, they
 * simply disagree, and every CheckVideoDecoderFormat / decoder creation then
 * fails for a reason that looks like "no hardware decode".
 *
 * Measured on this machine (value -> Wine's answer):
 *    87  -> S_OK, ordinary texture flags     (current numbering: NV12)
 *   103  -> E_FAIL                           (app's numbering: NV12)
 *   104  -> E_FAIL                           (app's numbering: P010)
 *
 * So the shim translates the known legacy values to Wine's numbering when it
 * forwards. The mapping is printed in the log on every translated call.
 * ------------------------------------------------------------------------ */
struct format_alias {
    unsigned legacy;   /* value the application uses */
    unsigned wine;     /* value Wine understands     */
    const char *name;
};

static const struct format_alias format_aliases[] = {
    { 103, 87,  "NV12 (legacy 103 -> wine 87)" },
    { 104, 100, "P010 (legacy 104 -> wine 100)" },
    { 105, 115, "P016 (legacy 105 -> wine 115)" },
    { 106, 88,  "420_OPAQUE (legacy 106 -> wine 88)" },
    { 100, 100, "AYUV (same in both)" },
};
#define FORMAT_ALIAS_COUNT ((unsigned)(sizeof(format_aliases) / sizeof(format_aliases[0])))

static DXGI_FORMAT wine_format(DXGI_FORMAT f, const char **note)
{
    unsigned i;

    if (note)
        *note = NULL;
    for (i = 0; i < FORMAT_ALIAS_COUNT; i++) {
        if ((unsigned)f == format_aliases[i].legacy) {
            if (note && format_aliases[i].legacy != format_aliases[i].wine)
                *note = format_aliases[i].name;
            else if (note)
                *note = format_aliases[i].name;
            return (DXGI_FORMAT)format_aliases[i].wine;
        }
    }
    return f;
}

/* DXVA_NoEncrypt: mingw's headers do not define it, the value is documented. */
static const GUID dxva_no_encrypt = { 0xab987610, 0xbb5b, 0x4311,
                                       { 0x87, 0x61, 0xa3, 0x13, 0xf7, 0x1a, 0xc7, 0xf1 } };

/* A conservative H.264 VLD configuration: raw bitstream, no host residual
 * difference decoding, no encryption. Values follow the documented
 * D3D11_VIDEO_DECODER_CONFIG / DXVA_H264 requirements. */
/* Tunables, so a config mismatch can be swept without rebuilding:
 *   UUYC_VD_CONFIG_RAW        ConfigBitstreamRaw            (default 1)
 *   UUYC_VD_CONFIG_SPECIFIC   ConfigDecoderSpecific         (default 0)
 *   UUYC_VD_CONFIG_RT         ConfigMinRenderTargetBuffCount (default 1)
 *   UUYC_VD_CONFIG_COUNT      how many configs to report     (default 1)
 *   UUYC_VD_CONFIG_NONE=1     report zero configs again (control run) */
static unsigned env_uint(const char *name, unsigned dflt)
{
    const char *v = getenv(name);

    if (!v || !v[0])
        return dflt;
    return (unsigned)strtoul(v, NULL, 0);
}

static unsigned config_count_override(void)
{
    if (getenv("UUYC_VD_CONFIG_NONE") && getenv("UUYC_VD_CONFIG_NONE")[0] == '1')
        return 0;
    return env_uint("UUYC_VD_CONFIG_COUNT", 1);
}

static void build_h264_config(D3D11_VIDEO_DECODER_CONFIG *cfg, unsigned index)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->guidConfigBitstreamEncryption = dxva_no_encrypt;
    cfg->guidConfigMBcontrolEncryption = dxva_no_encrypt;
    cfg->guidConfigResidDiffEncryption = dxva_no_encrypt;
    cfg->ConfigBitstreamRaw = env_uint("UUYC_VD_CONFIG_RAW", 1);
    cfg->ConfigMBcontrolRasterOrder = 0;
    cfg->ConfigResidDiffHost = 0;
    cfg->ConfigSpatialResid8 = 0;
    cfg->ConfigResid8Subtraction = 0;
    cfg->ConfigSpatialHost8or9Clipping = 0;
    cfg->ConfigSpatialResidInterleaved = 0;
    cfg->ConfigIntraResidUnsigned = 0;
    cfg->ConfigResidDiffAccelerator = 0;
    cfg->ConfigHostInverseScan = 0;
    cfg->ConfigSpecificIDCT = 0;
    cfg->Config4GroupedCoefs = 0;
    cfg->ConfigMinRenderTargetBuffCount = (USHORT)env_uint("UUYC_VD_CONFIG_RT", 1);
    /* ConfigDecoderSpecific is decoder defined; sweep it when the caller
     * rejects the first configuration. */
    cfg->ConfigDecoderSpecific = (USHORT)(env_uint("UUYC_VD_CONFIG_SPECIFIC", 0) + index);
}

static const char *fmt_name(DXGI_FORMAT f)
{
    switch ((unsigned)f) {
    case 87:  return "NV12";
    case 88:  return "420_OPAQUE";
    case 93:  return "P208";
    case 94:  return "V208";
    case 95:  return "V408";
    case 100: return "AYUV";
    case 101: return "Y410";
    case 102: return "Y416";
    case 103: return "Y210";
    case 104: return "Y216";
    case 105: return "NV11";
    case 106: return "AI44";
    case 107: return "IA44";
    case 108: return "P8";
    case 109: return "A8P8";
    case 110: return "B4G4R4A4_UNORM";
    case 115: return "P016";
    case 28:  return "R8G8B8A8_UNORM";
    case 29:  return "R8G8B8A8_UNORM_SRGB";
    case 10:  return "R16G16B16A16_FLOAT";
    case 24:  return "R10G10B10A2_UNORM";
    case 45:  return "B8G8R8X8_UNORM_SRGB";
    case 56:  return "B8G8R8A8_TYPELESS";
    case 58:  return "B8G8R8A8_UNORM_SRGB";
    default:  return "(other)";
    }
}

/* Video formats as Wine numbers them; used to decide what to patch. */
#define WINE_FMT_NV12        87u
#define WINE_FMT_420_OPAQUE  88u
#define WINE_FMT_P208        93u
#define WINE_FMT_P010       104u
#define WINE_FMT_P016       115u
#define WINE_FMT_AYUV       100u
#define WINE_FMT_YUY2       107u

static const char *guid_text(const GUID *g)
{
    static char buf[80];

    if (!g)
        return "(null)";
    if (IsEqualGUID(g, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT))         return "H264_VLD_NOFGT";
    if (IsEqualGUID(g, &D3D11_DECODER_PROFILE_H264_VLD_FGT))           return "H264_VLD_FGT";
    if (IsEqualGUID(g, &D3D11_DECODER_PROFILE_H264_VLD_MULTIVIEW_NOFGT)) return "H264_VLD_MULTIVIEW_NOFGT";
    if (IsEqualGUID(g, &D3D11_DECODER_PROFILE_H264_VLD_STEREO_NOFGT))  return "H264_VLD_STEREO_NOFGT";
    if (IsEqualGUID(g, &D3D11_DECODER_PROFILE_H264_VLD_WITHFMOASO_NOFGT)) return "H264_VLD_WITHFMOASO_NOFGT";
    if (IsEqualGUID(g, &D3D11_DECODER_PROFILE_H264_IDCT_NOFGT))        return "H264_IDCT_NOFGT";
    if (IsEqualGUID(g, &D3D11_DECODER_PROFILE_H264_MOCOMP_NOFGT))      return "H264_MOCOMP_NOFGT";
    if (IsEqualGUID(g, &D3D11_DECODER_PROFILE_HEVC_VLD_MAIN))          return "HEVC_VLD_MAIN";
    if (IsEqualGUID(g, &D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10))        return "HEVC_VLD_MAIN10";
    if (IsEqualGUID(g, &D3D11_DECODER_PROFILE_MPEG2_VLD))              return "MPEG2_VLD";
    if (IsEqualGUID(g, &D3D11_DECODER_PROFILE_VC1_VLD))                return "VC1_VLD";
    if (IsEqualGUID(g, &D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0))       return "VP9_VLD_PROFILE0";
    sprintf(buf, "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
            g->Data1, g->Data2, g->Data3,
            g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
            g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    return buf;
}

/* --------------------------------------------------------- profile reporting -- */

/* Gen9.5 hardware decodes these (see the report); the shim advertises them so the
 * application proceeds past profile enumeration. Nothing is actually decoded. */
static const GUID *const advertised[] = {
    &D3D11_DECODER_PROFILE_H264_VLD_NOFGT,
    &D3D11_DECODER_PROFILE_H264_VLD_FGT,
};
#define ADVERTISED_COUNT ((UINT)(sizeof(advertised) / sizeof(advertised[0])))

static UINT profile_count(void)
{
    const char *v = getenv("UUYC_VD_SHIM_PROFILES");

    if (v && v[0] == '0')
        return 0;
    return ADVERTISED_COUNT;
}

static int patch_formats(void)
{
    const char *v = getenv("UUYC_VD_SHIM_FORMATS");

    return !(v && v[0] == '0');
}

static int is_decode_output_format(DXGI_FORMAT f)
{
    switch ((unsigned)f) {
    case WINE_FMT_NV12:
    case WINE_FMT_420_OPAQUE:
    case WINE_FMT_P208:
    case WINE_FMT_P010:
    case WINE_FMT_P016:
        return 1;
    default:
        return 0;
    }
}

/* ---------------------------------------------------------------------------
 * Interface identification
 *
 * The application never mentions these names as strings, so the only way to see
 * what it asks for is to log every QueryInterface. Knowing whether it requests
 * ID3D11VideoContext / ID3D11VideoProcessorEnumerator is what separates "it does
 * not use this path" from "our wrapping is incomplete".
 * ------------------------------------------------------------------------ */
static const char *iid_name(REFIID iid)
{
    static char buf[80];

    if (IsEqualGUID(iid, &IID_IUnknown))                       return "IUnknown";
    if (IsEqualGUID(iid, &IID_ID3D11Device))                   return "ID3D11Device";
    if (IsEqualGUID(iid, &IID_ID3D11DeviceContext))            return "ID3D11DeviceContext";
    if (IsEqualGUID(iid, &IID_ID3D10Device))                   return "ID3D10Device";
    if (IsEqualGUID(iid, &IID_ID3D10Multithread))              return "ID3D10Multithread";
    if (IsEqualGUID(iid, &IID_IDXGIDevice))                    return "IDXGIDevice";
    if (IsEqualGUID(iid, &IID_IDXGIObject))                    return "IDXGIObject";
    if (IsEqualGUID(iid, &IID_ID3D11VideoDevice))              return "ID3D11VideoDevice";
    if (IsEqualGUID(iid, &IID_ID3D11VideoContext))             return "ID3D11VideoContext";
    if (IsEqualGUID(iid, &IID_ID3D11VideoProcessorEnumerator)) return "ID3D11VideoProcessorEnumerator";
    if (IsEqualGUID(iid, &IID_ID3D11VideoProcessor))            return "ID3D11VideoProcessor";
    if (IsEqualGUID(iid, &IID_ID3D11VideoDecoder))             return "ID3D11VideoDecoder";
    if (IsEqualGUID(iid, &IID_ID3D11VideoDecoderOutputView))   return "ID3D11VideoDecoderOutputView";
    sprintf(buf, "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
            iid->Data1, iid->Data2, iid->Data3,
            iid->Data4[0], iid->Data4[1], iid->Data4[2], iid->Data4[3],
            iid->Data4[4], iid->Data4[5], iid->Data4[6], iid->Data4[7]);
    return buf;
}

/* ------------------------------------------------- ID3D11VideoDevice wrapper -- */

typedef struct video_device {
    ID3D11VideoDeviceVtbl *vtbl;
    LONG ref;
    ID3D11VideoDevice *real;      /* Wine's implementation, may be NULL */
    ID3D11Device *device;         /* the wrapped device, for logging */
    LONG owner;                   /* device serial this interface came from */
} video_device;

static HRESULT STDMETHODCALLTYPE vd_QueryInterface(ID3D11VideoDevice *iface, REFIID iid, void **out)
{
    video_device *vd = (video_device *)iface;

    if (!out)
        return E_POINTER;
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_ID3D11VideoDevice)) {
        vd->ref++;
        *out = vd;
        return S_OK;
    }
    if (vd->real)
        return ID3D11VideoDevice_QueryInterface(vd->real, iid, out);
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE vd_AddRef(ID3D11VideoDevice *iface)
{
    return (ULONG)InterlockedIncrement(&((video_device *)iface)->ref);
}

static ULONG STDMETHODCALLTYPE vd_Release(ID3D11VideoDevice *iface)
{
    video_device *vd = (video_device *)iface;
    LONG ref = InterlockedDecrement(&vd->ref);

    if (ref == 0) {
        if (vd->real)
            ID3D11VideoDevice_Release(vd->real);
        if (vd->device)
            ID3D11Device_Release(vd->device);
        free(vd);
    }
    return (ULONG)ref;
}

static HRESULT STDMETHODCALLTYPE vd_CreateVideoDecoder(ID3D11VideoDevice *iface,
        const D3D11_VIDEO_DECODER_DESC *desc, const D3D11_VIDEO_DECODER_CONFIG *config,
        ID3D11VideoDecoder **decoder)
{
    video_device *vd = (video_device *)iface;
    D3D11_VIDEO_DECODER_DESC local;
    const D3D11_VIDEO_DECODER_DESC *use = desc;
    const char *note = NULL;
    HRESULT hr;

    if (desc) {
        local = *desc;
        local.OutputFormat = wine_format(desc->OutputFormat, &note);
        use = &local;
    }
    hr = vd->real ? ID3D11VideoDevice_CreateVideoDecoder(vd->real, use, config, decoder)
                  : E_NOTIMPL;

    logf_("[video] dev%ld CreateVideoDecoder profile=%s fmt=%s(%u) %ux%u config=%p -> %#lx %s %s",
          (long)vd->owner,
          desc ? guid_text(&desc->Guid) : "(null)",
          desc ? fmt_name(desc->OutputFormat) : "-", desc ? (unsigned)desc->OutputFormat : 0,
          desc ? desc->SampleWidth : 0, desc ? desc->SampleHeight : 0,
          (void *)config, (unsigned long)hr, hr_text(hr),
          (hr == S_OK && decoder && *decoder) ? "(REAL DECODER)" : "(no decoder)");
    if (note)
        logf_("        OutputFormat translated: %s", note);
    if (config)
        logf_("        config: BitstreamRaw=%u MBcontrolRasterOrder=%u ResidDiffHost=%u "
              "SpatialResid8=%u Resid8Subtraction=%u SpatialHost8or9Clipping=%u "
              "SpatialResidInterleaved=%u IntraResidUnsigned=%u ResidDiffAccelerator=%u "
              "HostInverseScan=%u SpecificIDCT=%u 4GroupedCoefs=%u",
              config->ConfigBitstreamRaw, config->ConfigMBcontrolRasterOrder,
              config->ConfigResidDiffHost, config->ConfigSpatialResid8,
              config->ConfigResid8Subtraction, config->ConfigSpatialHost8or9Clipping,
              config->ConfigSpatialResidInterleaved, config->ConfigIntraResidUnsigned,
              config->ConfigResidDiffAccelerator, config->ConfigHostInverseScan,
              config->ConfigSpecificIDCT, config->Config4GroupedCoefs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE vd_CreateVideoProcessor(ID3D11VideoDevice *iface,
        ID3D11VideoProcessorEnumerator *enumr, UINT rate, ID3D11VideoProcessor **processor)
{
    video_device *vd = (video_device *)iface;
    HRESULT hr = vd->real ? ID3D11VideoDevice_CreateVideoProcessor(vd->real, enumr, rate, processor)
                          : E_NOTIMPL;

    logf_("[video] CreateVideoProcessor rate=%u -> %#lx %s", rate, (unsigned long)hr, hr_text(hr));
    return hr;
}

static HRESULT STDMETHODCALLTYPE vd_CreateAuthenticatedChannel(ID3D11VideoDevice *iface,
        D3D11_AUTHENTICATED_CHANNEL_TYPE type, ID3D11AuthenticatedChannel **channel)
{
    video_device *vd = (video_device *)iface;
    HRESULT hr = vd->real ? ID3D11VideoDevice_CreateAuthenticatedChannel(vd->real, type, channel)
                          : E_NOTIMPL;

    logf_("[video] CreateAuthenticatedChannel type=%u -> %#lx", type, (unsigned long)hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE vd_CreateCryptoSession(ID3D11VideoDevice *iface,
        const GUID *type, const GUID *keyexch, const GUID *block, ID3D11CryptoSession **session)
{
    video_device *vd = (video_device *)iface;
    HRESULT hr = vd->real ? ID3D11VideoDevice_CreateCryptoSession(vd->real, type, keyexch, block, session)
                          : E_NOTIMPL;

    logf_("[video] CreateCryptoSession -> %#lx", (unsigned long)hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE vd_CreateVideoDecoderOutputView(ID3D11VideoDevice *iface,
        ID3D11Resource *resource, const D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC *desc,
        ID3D11VideoDecoderOutputView **view)
{
    video_device *vd = (video_device *)iface;
    HRESULT hr = vd->real ? ID3D11VideoDevice_CreateVideoDecoderOutputView(vd->real, resource, desc, view)
                          : E_NOTIMPL;
    unsigned fmt = 0, dim = 0;

    if (desc) {
        fmt = (unsigned)desc->DecodeProfile.Data1; /* not the format; logged raw below */
        if (desc->ViewDimension == D3D11_VDOV_DIMENSION_TEXTURE2D)
            dim = desc->Texture2D.ArraySlice;
    }
    logf_("[video] dev%ld CreateVideoDecoderOutputView res=%p dim=%u arraySlice=%u -> %#lx %s",
          (long)vd->owner,
          (void *)resource, desc ? (unsigned)desc->ViewDimension : 0, dim,
          (unsigned long)hr, hr_text(hr));
    if (desc)
        logf_("        DecodeProfile=%s", guid_text(&desc->DecodeProfile));
    (void)fmt;
    return hr;
}

static HRESULT STDMETHODCALLTYPE vd_CreateVideoProcessorInputView(ID3D11VideoDevice *iface,
        ID3D11Resource *resource, ID3D11VideoProcessorEnumerator *enumr,
        const D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC *desc, ID3D11VideoProcessorInputView **view)
{
    video_device *vd = (video_device *)iface;
    HRESULT hr = vd->real
        ? ID3D11VideoDevice_CreateVideoProcessorInputView(vd->real, resource, enumr, desc, view)
        : E_NOTIMPL;

    logf_("[video] CreateVideoProcessorInputView res=%p -> %#lx %s",
          (void *)resource, (unsigned long)hr, hr_text(hr));
    return hr;
}

static HRESULT STDMETHODCALLTYPE vd_CreateVideoProcessorOutputView(ID3D11VideoDevice *iface,
        ID3D11Resource *resource, ID3D11VideoProcessorEnumerator *enumr,
        const D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC *desc, ID3D11VideoProcessorOutputView **view)
{
    video_device *vd = (video_device *)iface;
    HRESULT hr = vd->real
        ? ID3D11VideoDevice_CreateVideoProcessorOutputView(vd->real, resource, enumr, desc, view)
        : E_NOTIMPL;

    logf_("[video] CreateVideoProcessorOutputView res=%p -> %#lx %s",
          (void *)resource, (unsigned long)hr, hr_text(hr));
    return hr;
}

static HRESULT STDMETHODCALLTYPE vd_CreateVideoProcessorEnumerator(ID3D11VideoDevice *iface,
        const D3D11_VIDEO_PROCESSOR_CONTENT_DESC *desc, ID3D11VideoProcessorEnumerator **enumr)
{
    video_device *vd = (video_device *)iface;
    HRESULT hr = vd->real ? ID3D11VideoDevice_CreateVideoProcessorEnumerator(vd->real, desc, enumr)
                          : E_NOTIMPL;

    logf_("[video] dev%ld CreateVideoProcessorEnumerator %ux%u -> %#lx %s",
          (long)vd->owner,
          desc ? desc->InputFrameRate.Numerator : 0, desc ? desc->OutputWidth : 0,
          (unsigned long)hr, hr_text(hr));
    return hr;
}

/*
 * Profile list policy: be a faithful pass-through.
 *
 * Wine already reports the profiles it can actually instantiate
 * (DXVA_ModeH264_VLD_NoFGT, once Vulkan Video is enabled). Whatever Wine lists,
 * this returns verbatim. The synthetic list is used ONLY as a last resort, when
 * Wine reports nothing -- otherwise the application can pick a profile that
 * looks supported here and then fails in CreateVideoDecoder, which is forwarded
 * to Wine and cannot honour it. Advertising DXVA H264_VLD_FGT alongside
 * Wine's NoFGT did exactly that.
 */
static UINT STDMETHODCALLTYPE vd_GetVideoDecoderProfileCount(ID3D11VideoDevice *iface)
{
    video_device *vd = (video_device *)iface;
    UINT real_count = vd->real ? ID3D11VideoDevice_GetVideoDecoderProfileCount(vd->real) : 0;
    UINT count = real_count ? real_count : profile_count();

    logf_("[video] dev%ld GetVideoDecoderProfileCount: wine=%u -> %u (%s)",
          (long)vd->owner, real_count, count, real_count ? "passed through" : "synthetic");
    return count;
}

static HRESULT STDMETHODCALLTYPE vd_GetVideoDecoderProfile(ID3D11VideoDevice *iface,
        UINT index, GUID *profile)
{
    video_device *vd = (video_device *)iface;
    UINT real_count = vd->real ? ID3D11VideoDevice_GetVideoDecoderProfileCount(vd->real) : 0;
    HRESULT hr;

    if (!profile)
        return E_POINTER;

    /* Prefer Wine's own list for exactly the reason given above. */
    if (real_count) {
        hr = ID3D11VideoDevice_GetVideoDecoderProfile(vd->real, index, profile);
        logf_("[video] GetVideoDecoderProfile(%u) -> %s (wine, %#lx)",
              index, SUCCEEDED(hr) ? guid_text(profile) : "-", (unsigned long)hr);
        return hr;
    }

    if (index >= ADVERTISED_COUNT) {
        logf_("[video] GetVideoDecoderProfile(%u) -> E_INVALIDARG (synthetic count=%u)",
              index, (unsigned)ADVERTISED_COUNT);
        return E_INVALIDARG;
    }
    *profile = *advertised[index];
    logf_("[video] GetVideoDecoderProfile(%u) -> %s (synthetic)", index, guid_text(profile));
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vd_CheckVideoDecoderFormat(ID3D11VideoDevice *iface,
        const GUID *profile, DXGI_FORMAT format, BOOL *supported)
{
    video_device *vd = (video_device *)iface;
    const char *note = NULL;
    DXGI_FORMAT translated = wine_format(format, &note);
    BOOL real_supported = FALSE;

    if (vd->real)
        ID3D11VideoDevice_CheckVideoDecoderFormat(vd->real, profile, translated, &real_supported);
    if (supported)
        *supported = TRUE;                        /* tell the application "yes" */
    logf_("[video] dev%ld CheckVideoDecoderFormat profile=%s fmt=%s(%u)%s -> wine_impl=%d shim=TRUE",
          (long)vd->owner,
          guid_text(profile), fmt_name(format), (unsigned)format,
          note ? " [translated]" : "", (int)real_supported);
    if (note)
        logf_("        translation: %s", note);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vd_GetVideoDecoderConfigCount(ID3D11VideoDevice *iface,
        const D3D11_VIDEO_DECODER_DESC *desc, UINT *count)
{
    video_device *vd = (video_device *)iface;
    UINT real_count = 0;
    HRESULT real_hr = E_NOTIMPL;

    if (vd->real)
        real_hr = ID3D11VideoDevice_GetVideoDecoderConfigCount(vd->real, desc, &real_count);

    /* Wine reports E_NOTIMPL here, which stops the decode setup before
     * CreateVideoDecoder is ever reached. Publish one documented H.264
     * configuration so the application's next step becomes visible. */
    if (FAILED(real_hr) || real_count == 0) {
        unsigned shim_count = config_count_override();

        if (count)
            *count = shim_count;
        logf_("[video] dev%ld GetVideoDecoderConfigCount profile=%s fmt=%s %ux%u: wine=%#lx(%u) "
              "-> shim=%u",
              (long)((video_device *)iface)->owner,
              desc ? guid_text(&desc->Guid) : "-", desc ? fmt_name(desc->OutputFormat) : "-",
              desc ? desc->SampleWidth : 0, desc ? desc->SampleHeight : 0,
              (unsigned long)real_hr, real_count, shim_count);
        return S_OK;
    }
    if (count)
        *count = real_count;
    logf_("[video] GetVideoDecoderConfigCount profile=%s fmt=%s -> wine=%#lx count=%u",
          desc ? guid_text(&desc->Guid) : "-", desc ? fmt_name(desc->OutputFormat) : "-",
          (unsigned long)real_hr, real_count);
    return real_hr;
}

static HRESULT STDMETHODCALLTYPE vd_GetVideoDecoderConfig(ID3D11VideoDevice *iface,
        const D3D11_VIDEO_DECODER_DESC *desc, UINT index, D3D11_VIDEO_DECODER_CONFIG *config)
{
    video_device *vd = (video_device *)iface;
    HRESULT real_hr = E_NOTIMPL;

    if (vd->real && config)
        real_hr = ID3D11VideoDevice_GetVideoDecoderConfig(vd->real, desc, index, config);

    if (FAILED(real_hr) && config) {
        build_h264_config(config, index);
        logf_("[video] dev%ld GetVideoDecoderConfig index=%u profile=%s fmt=%s -> wine=%#lx, "
              "shim publishes Raw=%u MBraster=%u ResidHost=%u SpatialResid8=%u "
              "Resid8Sub=%u Clip=%u Interleaved=%u IntraUnsigned=%u ResidAccel=%u "
              "InvScan=%u SpecIDCT=%u Group4=%u RtBuffers=%u DecSpecific=%u",
              (long)((video_device *)iface)->owner,
              index, desc ? guid_text(&desc->Guid) : "-",
              desc ? fmt_name(desc->OutputFormat) : "-", (unsigned long)real_hr,
              config->ConfigBitstreamRaw, config->ConfigMBcontrolRasterOrder,
              config->ConfigResidDiffHost, config->ConfigSpatialResid8,
              config->ConfigResid8Subtraction, config->ConfigSpatialHost8or9Clipping,
              config->ConfigSpatialResidInterleaved, config->ConfigIntraResidUnsigned,
              config->ConfigResidDiffAccelerator, config->ConfigHostInverseScan,
              config->ConfigSpecificIDCT, config->Config4GroupedCoefs,
              config->ConfigMinRenderTargetBuffCount, config->ConfigDecoderSpecific);
        return S_OK;
    }
    logf_("[video] GetVideoDecoderConfig index=%u -> %#lx %s",
          index, (unsigned long)real_hr, hr_text(real_hr));
    return real_hr;
}

static HRESULT STDMETHODCALLTYPE vd_GetContentProtectionCaps(ID3D11VideoDevice *iface,
        const GUID *crypto, const GUID *protocol, D3D11_VIDEO_CONTENT_PROTECTION_CAPS *caps)
{
    video_device *vd = (video_device *)iface;
    HRESULT hr = vd->real ? ID3D11VideoDevice_GetContentProtectionCaps(vd->real, crypto, protocol, caps)
                          : E_NOTIMPL;

    logf_("[video] GetContentProtectionCaps -> %#lx", (unsigned long)hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE vd_CheckCryptoKeyExchange(ID3D11VideoDevice *iface,
        const GUID *crypto, const GUID *protocol, UINT index, GUID *key_exchange_type)
{
    video_device *vd = (video_device *)iface;
    HRESULT hr = vd->real
        ? ID3D11VideoDevice_CheckCryptoKeyExchange(vd->real, crypto, protocol, index, key_exchange_type)
        : E_NOTIMPL;

    logf_("[video] CheckCryptoKeyExchange -> %#lx", (unsigned long)hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE vd_SetPrivateData(ID3D11VideoDevice *iface, REFGUID guid,
        UINT size, const void *data)
{
    video_device *vd = (video_device *)iface;

    return vd->real ? ID3D11VideoDevice_SetPrivateData(vd->real, guid, size, data) : E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE vd_SetPrivateDataInterface(ID3D11VideoDevice *iface, REFGUID guid,
        const IUnknown *unknown)
{
    video_device *vd = (video_device *)iface;

    return vd->real ? ID3D11VideoDevice_SetPrivateDataInterface(vd->real, guid, unknown) : E_NOTIMPL;
}

static ID3D11VideoDeviceVtbl video_device_vtbl = {
    vd_QueryInterface,
    vd_AddRef,
    vd_Release,
    vd_CreateVideoDecoder,
    vd_CreateVideoProcessor,
    vd_CreateAuthenticatedChannel,
    vd_CreateCryptoSession,
    vd_CreateVideoDecoderOutputView,
    vd_CreateVideoProcessorInputView,
    vd_CreateVideoProcessorOutputView,
    vd_CreateVideoProcessorEnumerator,
    vd_GetVideoDecoderProfileCount,
    vd_GetVideoDecoderProfile,
    vd_CheckVideoDecoderFormat,
    vd_GetVideoDecoderConfigCount,
    vd_GetVideoDecoderConfig,
    vd_GetContentProtectionCaps,
    vd_CheckCryptoKeyExchange,
    vd_SetPrivateData,
    vd_SetPrivateDataInterface,
};


/* ------------------------------------------------------- ID3D11Device hooking -- */

/*
 * Hook strategy: vtable swap on Wine's own object -- never a substitute object.
 *
 * An earlier revision returned a small fake struct as the ID3D11Device. That is
 * broken in principle, not just in detail: Wine's methods begin with
 * impl_from_ID3D11Device(iface), i.e. they treat `this` as a `struct
 * d3d11_device` and read fields at fixed offsets. The fake struct was 0x18
 * bytes, so d3d11_device_GetFeatureLevel's `mov 0x38(%this)` read past its end,
 * got NULL, and faulted on the next dereference. The client died with
 * STATUS_ACCESS_VIOLATION right after device creation -- every method except the
 * single slot we patched was handed a bogus `this`.
 *
 * Correct approach: keep Wine's real device as `this` for everybody, and install
 * a patched copy of its vtable on the object itself. Then
 *   - every unpatched method is Wine's own code on Wine's own object;
 *   - our patched slots receive Wine's real `this`, so they can call the saved
 *     original directly and look the serial up in this side table.
 * Wine heap-allocates the device, so writing its lpVtbl is legal.
 *
 * The patched copies are intentionally never freed: they are process-lifetime,
 * and releasing them on a refcount would race with other holders.
 */
typedef struct device_hook {
    ID3D11Device *real;             /* Wine's device; also the `this` callers see */
    ID3D11DeviceVtbl *original;     /* Wine's untouched vtable */
    ID3D11DeviceVtbl *patched;      /* our copy, installed as real->lpVtbl */
    LONG serial;                    /* 1-based device number, for log correlation */
    struct device_hook *next;
} device_hook;

static device_hook *device_hooks;
static LONG device_counter;

static device_hook *hook_for(ID3D11Device *iface)
{
    device_hook *h;

    for (h = device_hooks; h; h = h->next) {
        if (h->real == iface)
            return h;
    }
    return NULL;
}

static HRESULT STDMETHODCALLTYPE dp_QueryInterface(ID3D11Device *iface, REFIID iid, void **out)
{
    device_hook *h = hook_for(iface);
    HRESULT hr;

    if (!h)
        return E_FAIL;
    if (!out)
        return E_POINTER;

    if (IsEqualGUID(iid, &IID_ID3D11VideoDevice)) {
        video_device *vd;
        ID3D11VideoDevice *real_vd = NULL;

        /* Ask Wine for its video device through the ORIGINAL vtable: going via
         * iface->lpVtbl would re-enter this function. */
        hr = h->original->QueryInterface(iface, iid, (void **)&real_vd);
        if (FAILED(hr) || !real_vd) {
            logf_("[shim] dev%ld QueryInterface(ID3D11VideoDevice) -> %#lx %s",
                  (long)h->serial, (unsigned long)hr, hr_text(hr));
            return hr;
        }

        vd = (video_device *)calloc(1, sizeof(*vd));
        if (!vd) {
            ID3D11VideoDevice_Release(real_vd);
            return E_OUTOFMEMORY;
        }
        vd->vtbl = &video_device_vtbl;
        vd->ref = 1;
        vd->real = real_vd;
        vd->owner = h->serial;
        ID3D11Device_AddRef(iface);            /* keep the device alive for vd->device */
        vd->device = iface;
        *out = vd;
        logf_("[shim] dev%ld QueryInterface(ID3D11VideoDevice) -> wrapped %p (real %p)",
              (long)h->serial, (void *)vd, (void *)real_vd);
        return S_OK;
    }

    hr = h->original->QueryInterface(iface, iid, out);
    logf_("[shim] dev%ld QueryInterface(%s) -> %#lx %s%s", (long)h->serial, iid_name(iid),
          (unsigned long)hr, hr_text(hr), out && *out ? "" : " (null)");
    return hr;
}

/* AddRef/Release are deliberately NOT hooked: Wine's own refcounting is correct,
 * and the object the application holds is Wine's object anyway. */

/* ------------------------------------------------------------ CheckFormatSupport -- */

/* The application asks about NV12/P010 before creating a decoder. Wine answers
 * E_FAIL, which makes many clients give up before they ever enumerate profiles.
 * The shim answers DECODER_OUTPUT | TEXTURE2D | SHADER_SAMPLE for those, so the
 * log shows what the application does next. Nothing is decoded. */
typedef HRESULT (STDMETHODCALLTYPE *check_format_support_fn)(ID3D11Device *, DXGI_FORMAT, UINT *);

static HRESULT STDMETHODCALLTYPE dp_CheckFormatSupport(ID3D11Device *iface, DXGI_FORMAT format, UINT *support)
{
    device_hook *h = hook_for(iface);
    check_format_support_fn real;
    HRESULT hr;

    if (!h)
        return E_FAIL;
    real = (check_format_support_fn)h->original->CheckFormatSupport;
    hr = real(iface, format, support);

    if (patch_formats() && is_decode_output_format(format)) {
        const UINT shim_flags = D3D11_FORMAT_SUPPORT_TEXTURE2D
                              | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE
                              | D3D11_FORMAT_SUPPORT_DECODER_OUTPUT
                              | D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT;
        logf_("[fmt] CheckFormatSupport(%s=%u): wine=%#lx/%s flags=%#x -> shim flags=%#x",
              fmt_name(format), (unsigned)format, (unsigned long)hr, hr_text(hr),
              support ? *support : 0, shim_flags);
        if (support)
            *support = shim_flags;
        return S_OK;
    }
    if (!quiet)
        logf_("[fmt] CheckFormatSupport(%s=%u) -> %#lx %s flags=%#x",
              fmt_name(format), (unsigned)format, (unsigned long)hr, hr_text(hr),
              support ? *support : 0);
    return hr;
}

/*
 * Install the hook and return the pointer the application must receive:
 * Wine's own device, not a proxy. Returns NULL if the hook could not be set up,
 * in which case the caller passes Wine's device through untouched.
 */
static ID3D11Device *device_hook_install(ID3D11Device *real)
{
    device_hook *h;
    ID3D11DeviceVtbl *patched;

    if (!real || !real->lpVtbl)
        return real;
    if (hook_for(real))
        return real;                       /* already hooked */

    h = (device_hook *)calloc(1, sizeof(*h));
    patched = (ID3D11DeviceVtbl *)malloc(sizeof(*patched));
    if (!h || !patched) {
        free(h);
        free(patched);
        return real;
    }

    memcpy(patched, real->lpVtbl, sizeof(*patched));
    patched->QueryInterface = dp_QueryInterface;
    patched->CheckFormatSupport = dp_CheckFormatSupport;

    h->real = real;
    h->original = real->lpVtbl;
    h->patched = patched;
    h->serial = InterlockedIncrement(&device_counter);
    h->next = device_hooks;
    device_hooks = h;

    real->lpVtbl = patched;                /* the swap that makes it all work */
    logf_("[shim] device #%ld hooked in place (this=%p, wine vtable=%p, patched=%p)",
          (long)h->serial, (void *)real, (void *)h->original, (void *)patched);
    return real;
}

/* --------------------------------------------------------------------- exports -- */

typedef HRESULT (WINAPI *create_device_fn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
        const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

typedef HRESULT (WINAPI *create_device_swapchain_fn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
        const D3D_FEATURE_LEVEL *, UINT, UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **,
        ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

/* This DLL is installed AS d3d11.dll, so LoadLibraryW(L"d3d11.dll") would return
 * this module itself and every forward would recurse. Load Wine's builtin by its
 * absolute path instead. */
static HMODULE real_d3d11(void)
{
    static HMODULE mod = NULL;

    if (!mod) {
        mod = LoadLibraryW(L"C:\\windows\\system32\\d3d11.dll");
        if (!mod)
            mod = LoadLibraryW(L"C:\\windows\\syswow64\\d3d11.dll");
        logf_("[shim] real d3d11 backend: %p (err %lu)", (void *)mod, GetLastError());
    }
    return mod;
}

static void *real_proc(const char *name)
{
    void *p = (void *)GetProcAddress(real_d3d11(), name);

    if (!p)
        logf_("[shim] FAILED to resolve %s", name);
    return p;
}

__declspec(dllexport) HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type,
        HMODULE software, UINT flags, const D3D_FEATURE_LEVEL *levels, UINT level_count, UINT sdk,
        ID3D11Device **device, D3D_FEATURE_LEVEL *obtained, ID3D11DeviceContext **context)
{
    create_device_fn fn = (create_device_fn)real_proc("D3D11CreateDevice");
    HRESULT hr;
    ID3D11Device *real = NULL;

    logf_("==== D3D11CreateDevice driver_type=%u flags=%#x levels=%u sdk=%u ====",
          (unsigned)type, flags, level_count, sdk);
    if (!fn)
        return E_FAIL;

    hr = fn(adapter, type, software, flags, levels, level_count, sdk, device ? &real : NULL, obtained, context);
    logf_("[shim] real D3D11CreateDevice -> %#lx %s device=%p", (unsigned long)hr, hr_text(hr), (void *)real);
    if (FAILED(hr)) {
        if (device)
            *device = NULL;
        return hr;
    }

    if (device && real)
        *device = device_hook_install(real);
    logf_("[shim] returning device %p", device ? (void *)*device : NULL);
    return hr;
}

__declspec(dllexport) HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter *adapter,
        D3D_DRIVER_TYPE type, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL *levels,
        UINT level_count, UINT sdk, const DXGI_SWAP_CHAIN_DESC *swap_desc, IDXGISwapChain **swap_chain,
        ID3D11Device **device, D3D_FEATURE_LEVEL *obtained, ID3D11DeviceContext **context)
{
    create_device_swapchain_fn fn = (create_device_swapchain_fn)real_proc("D3D11CreateDeviceAndSwapChain");
    HRESULT hr;
    ID3D11Device *real = NULL;

    logf_("==== D3D11CreateDeviceAndSwapChain driver_type=%u flags=%#x ====", (unsigned)type, flags);
    if (!fn)
        return E_FAIL;

    hr = fn(adapter, type, software, flags, levels, level_count, sdk, swap_desc, swap_chain,
            device ? &real : NULL, obtained, context);
    logf_("[shim] real call -> %#lx %s device=%p", (unsigned long)hr, hr_text(hr), (void *)real);
    if (FAILED(hr)) {
        if (device)
            *device = NULL;
        return hr;
    }
    if (device && real)
        *device = device_hook_install(real);
    return hr;
}

/*
 * DllMain must not use the C runtime.
 *
 * The client loads this module dynamically, and in that case the CRT may not be
 * initialised when DllMain runs. Calling fopen/fprintf from DllMain is formally
 * undefined and, in practice, crashes the host process during load -- which is
 * exactly what happened. Everything below therefore uses plain Win32 file APIs.
 */
static void raw_write_all(const char *path, const char *text)
{
    HANDLE file;
    DWORD written;
    SIZE_T len = 0;

    if (!path || !text)
        return;
    while (text[len])
        len++;

    file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return;
    WriteFile(file, text, (DWORD)len, &written, NULL);
    CloseHandle(file);
}

/* Append to the same three locations the structured logger uses. Paths are
 * built with Win32 calls only. */
static void raw_log(const char *text)
{
    char path[MAX_PATH];
    DWORD n;

    n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        char *slash = strrchr(path, '\\');
        if (slash && (slash - path) + 24 < MAX_PATH) {
            slash[1] = 0;
            strcat(path, "uuyc-vdshim.log");
            raw_write_all(path, text);
        }
    }
    raw_write_all("Z:\\tmp\\uuyc-vdshim.log", text);
}

static void raw_log_pieces(const char *a, const char *b, const char *c)
{
    char buf[1024];
    SIZE_T i = 0, k;

    const char *parts[3] = { a, b, c };
    for (k = 0; k < 3; k++) {
        const char *p = parts[k];
        if (!p)
            continue;
        while (*p && i < sizeof(buf) - 2)
            buf[i++] = *p++;
    }
    buf[i] = 0;
    raw_log(buf);
}

/* Wine's d3d11.dll exports several functions this shim does not implement.
 * A process that requires one of them would fail to load if it resolved the
 * shim instead of the real module, so forward the documented-adjacent ones
 * where the signature is stable. D3D11CoreCreateDevice is the extended variant
 * used by layered clients; its parameters mirror D3D11CreateDevice minus the
 * trailing device context. */
typedef HRESULT (WINAPI *core_create_device_fn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
        const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *);

__declspec(dllexport) HRESULT WINAPI D3D11CoreCreateDevice(IDXGIAdapter *adapter,
        D3D_DRIVER_TYPE type, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL *levels,
        UINT level_count, UINT sdk, ID3D11Device **device, D3D_FEATURE_LEVEL *obtained)
{
    core_create_device_fn fn = (core_create_device_fn)real_proc("D3D11CoreCreateDevice");
    HRESULT hr;
    ID3D11Device *real = NULL;

    logf_("==== D3D11CoreCreateDevice driver_type=%u flags=%#x levels=%u ====",
          (unsigned)type, flags, level_count);
    if (!fn)
        return E_FAIL;

    hr = fn(adapter, type, software, flags, levels, level_count, sdk,
            device ? &real : NULL, obtained);
    logf_("[shim] real D3D11CoreCreateDevice -> %#lx %s device=%p",
          (unsigned long)hr, hr_text(hr), (void *)real);
    if (FAILED(hr)) {
        if (device)
            *device = NULL;
        return hr;
    }
    if (device && real)
        *device = device_hook_install(real);
    return hr;
}

/*
 * Fallback target for any forwarded export the real d3d11.dll does not provide.
 * These entry points are never called by this client (they exist only to satisfy
 * import tables), but a NULL slot would turn a stray call into a jump to zero.
 * Returning E_NOTIMPL is the least surprising thing a stub can do.
 */
static LONG WINAPI uuyc_forward_unavailable(void)
{
    return (LONG)0x80004001u; /* E_NOTIMPL */
}

static void *uuyc_forward_resolve(const char *name)
{
    return real_proc(name);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH) {
        char exe[260];
        DWORD n;

        /* No CRT here: read the switches with GetEnvironmentVariableA. */
        {
            char value[8];
            DWORD got = GetEnvironmentVariableA("UUYC_VD_SHIM_QUIET", value, sizeof(value));
            quiet = (got > 0 && value[0] == '1');
        }

        raw_log("\n==================== uuyc-vdshim attached ====================\n");
        exe[0] = 0;
        n = GetModuleFileNameA(NULL, exe, sizeof(exe));
        if (n > 0 && n < sizeof(exe))
            raw_log_pieces("  exe=", exe, "\n");

        /* Resolve the pass-through exports now, at attach time, so that an
         * importing PE can never observe an uninitialised slot. */
        uuyc_forwarders_init(uuyc_forward_resolve, (void *)uuyc_forward_unavailable);
    } else if (reason == DLL_PROCESS_DETACH) {
        /* During detach the CRT is already gone, so the structured logger cannot
         * be used either. */
        raw_log("==================== uuyc-vdshim detached ====================\n");
        {
            int i;
            for (i = 0; i < 3; i++) {
                if (log_files[i]) {
                    fclose(log_files[i]);
                    log_files[i] = NULL;
                }
            }
        }
    }
    return TRUE;
}