/*
 * uuyc-dxgiprobe -- does Wine fill in the DXGI adapter description string?
 *
 * Hypothesis: the client calls IDXGIAdapter::GetDesc (or GetDesc1/GetDesc3) and
 * passes DXGI_ADAPTER_DESC.Description to QString::fromUtf16() without checking
 * anything. If Wine leaves that field uninitialised, the app dereferences stack
 * garbage -- which is exactly the observed crash:
 *
 *   EXCEPTION 0xc0000005 in QString::fromUtf16, read at 0xffffffffffffffff
 *
 * This poisons the whole DXGI_ADAPTER_DESC with 0xAB first, so a field Wine does
 * not write is obvious.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <string.h>

static void show(const char *what, const WCHAR *desc, int chars)
{
    char utf8[512];
    int wide = 1, i;

    for (i = 0; i < chars && desc[i]; i++) {
        if (desc[i] < 0x20 || desc[i] == 0x7f) { wide = 0; break; }
    }
    if (WideCharToMultiByte(CP_UTF8, 0, desc, -1, utf8, sizeof(utf8), NULL, NULL) <= 0)
        strcpy(utf8, "<conversion failed>");
    printf("  %-22s poisoned=%s  first=%04x  text=\"%s\"\n", what,
           (unsigned short)desc[0] == 0xABAB ? "YES" : "no",
           (unsigned)desc[0], utf8);
}

int main(void)
{
    IDXGIFactory *factory = NULL;
    IDXGIAdapter *adapter = NULL;
    HRESULT hr;
    UINT i;

    hr = CreateDXGIFactory(&IID_IDXGIFactory, (void **)&factory);
    printf("CreateDXGIFactory -> %#lx\n", (unsigned long)hr);
    if (FAILED(hr) || !factory) return 1;

    for (i = 0; ; i++) {
        hr = IDXGIFactory_EnumAdapters(factory, i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        printf("adapter %u: EnumAdapters -> %#lx\n", i, (unsigned long)hr);
        if (FAILED(hr) || !adapter) continue;

        {
            DXGI_ADAPTER_DESC desc;
            memset(&desc, 0xAB, sizeof(desc));
            hr = IDXGIAdapter_GetDesc(adapter, &desc);
            printf("  GetDesc -> %#lx\n", (unsigned long)hr);
            show("DXGI_ADAPTER_DESC", desc.Description, 128);
            printf("  %-22s VendorId=%04x DeviceId=%04x SubSys=%08x Rev=%02x DedicatedVideoMem=%llu\n",
                   "ids", desc.VendorId, desc.DeviceId, desc.SubSysId, desc.Revision,
                   (unsigned long long)desc.DedicatedVideoMemory);
        }
        {
            IDXGIAdapter1 *a1 = NULL;
            if (SUCCEEDED(IDXGIAdapter_QueryInterface(adapter, &IID_IDXGIAdapter1, (void **)&a1)) && a1) {
                DXGI_ADAPTER_DESC1 d1;
                memset(&d1, 0xAB, sizeof(d1));
                hr = IDXGIAdapter1_GetDesc1(a1, &d1);
                printf("  GetDesc1 -> %#lx\n", (unsigned long)hr);
                show("DXGI_ADAPTER_DESC1", d1.Description, 128);
                IDXGIAdapter1_Release(a1);
            } else {
                printf("  no IDXGIAdapter1\n");
            }
        }
        IDXGIAdapter_Release(adapter);
        adapter = NULL;
    }
    IDXGIFactory_Release(factory);
    return 0;
}
