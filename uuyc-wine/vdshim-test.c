/* Minimal loader test: does the shim intercept QueryInterface(ID3D11VideoDevice),
 * report profiles, and patch CheckFormatSupport?
 *
 * DXGI_FORMAT values are hardcoded to Wine's numbering on purpose: mingw-w64's
 * dxgiformat.h still uses the pre-DXGI-1.2 numbering (NV12 = 103), while Wine
 * follows the current public numbering (NV12 = 87). Ask Wine: 87 answers S_OK,
 * 103 answers E_FAIL. */
#define WINE_FMT_NV12 87u   /* NOT mingw's DXGI_FORMAT_NV12 (103) */
#define WINE_FMT_P010 104u
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>

int main(void)
{
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    ID3D11VideoDevice *vd = NULL;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got = 0;
    HRESULT hr;
    UINT count = 0;
    GUID profile;
    BOOL supported = FALSE;
    UINT flags = 0;

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, 2,
                           D3D11_SDK_VERSION, &dev, &got, &ctx);
    printf("D3D11CreateDevice -> %#lx  device=%p (feature level %#x)\n",
           (unsigned long)hr, (void *)dev, (unsigned)got);
    if (FAILED(hr) || !dev)
        return 1;

    hr = ID3D11Device_QueryInterface(dev, &IID_ID3D11VideoDevice, (void **)&vd);
    printf("QueryInterface(ID3D11VideoDevice) -> %#lx  vd=%p\n", (unsigned long)hr, (void *)vd);

    /*
     * Regression test for the crash this shim once caused.
     *
     * The device returned by D3D11CreateDevice* must be WINE'S OWN object, not a
     * substitute: Wine's methods begin with impl_from_ID3D11Device(iface) and read
     * fixed offsets, so a foreign struct makes them fault. The first method the
     * client calls after creating a device is GetFeatureLevel, so calling it here
     * reproduces that failure exactly (STATUS_ACCESS_VIOLATION, WRITE at 0x0)
     * instead of leaving it for the application to hit.
     */
    {
        D3D_FEATURE_LEVEL fl = ID3D11Device_GetFeatureLevel(dev);
        ID3D11DeviceContext *ctx = NULL;

        printf("GetFeatureLevel -> %#x   <- crashes if the device is not Wine's own object\n", fl);
        ID3D11Device_GetImmediateContext(dev, &ctx);
        printf("GetImmediateContext -> %p\n", (void *)ctx);
        if (ctx)
            ID3D11DeviceContext_Release(ctx);
    }
    if (FAILED(hr) || !vd)
        return 2;

    count = ID3D11VideoDevice_GetVideoDecoderProfileCount(vd);
    printf("GetVideoDecoderProfileCount -> %u\n", count);
    if (count) {
        hr = ID3D11VideoDevice_GetVideoDecoderProfile(vd, 0, &profile);
        printf("GetVideoDecoderProfile(0) -> %#lx  {%08lx-%04x-...}\n",
               (unsigned long)hr, profile.Data1, profile.Data2);
        hr = ID3D11VideoDevice_CheckVideoDecoderFormat(vd, &profile, (DXGI_FORMAT)WINE_FMT_NV12, &supported);
        printf("CheckVideoDecoderFormat(NV12) -> %#lx  supported=%d\n",
               (unsigned long)hr, (int)supported);
    }

    hr = ID3D11Device_CheckFormatSupport(dev, (DXGI_FORMAT)WINE_FMT_NV12, &flags);
    printf("CheckFormatSupport(Wine NV12=87) -> %#lx  flags=%#x\n", (unsigned long)hr, flags);
    hr = ID3D11Device_CheckFormatSupport(dev, (DXGI_FORMAT)WINE_FMT_P010, &flags);
    printf("CheckFormatSupport(Wine P010=104) -> %#lx  flags=%#x\n", (unsigned long)hr, flags);

    /* The application's own numbering: it asks for NV12 as 103. The shim must
     * translate that to Wine's 87 when it forwards. */
    hr = ID3D11VideoDevice_CheckVideoDecoderFormat(vd, &profile, (DXGI_FORMAT)103, &supported);
    printf("CheckVideoDecoderFormat(legacy NV12=103) -> %#lx  supported=%d\n",
           (unsigned long)hr, (int)supported);

    /* GetVideoDecoderConfigCount: Wine answers E_NOTIMPL, the shim publishes one
     * configuration so the caller can proceed. */
    {
        D3D11_VIDEO_DECODER_DESC d;
        D3D11_VIDEO_DECODER_CONFIG c;
        UINT n = 0;
        memset(&d, 0, sizeof(d));
        d.Guid = profile;
        d.SampleWidth = 1920;
        d.SampleHeight = 1080;
        d.OutputFormat = (DXGI_FORMAT)103;          /* application numbering */
        hr = ID3D11VideoDevice_GetVideoDecoderConfigCount(vd, &d, &n);
        printf("GetVideoDecoderConfigCount -> %#lx  count=%u\n", (unsigned long)hr, n);
        hr = ID3D11VideoDevice_GetVideoDecoderConfig(vd, &d, 0, &c);
        printf("GetVideoDecoderConfig -> %#lx  BitstreamRaw=%u\n",
               (unsigned long)hr, c.ConfigBitstreamRaw);
    }

    ID3D11VideoDevice_Release(vd);
    if (ctx) ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    printf("done\n");
    return 0;
}
