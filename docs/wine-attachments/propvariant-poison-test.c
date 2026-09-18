/*
 * testshshim -- prove that the property store initialises its output.
 *
 * The PROPVARIANT is poisoned with 0xAB before the call. A correct
 * implementation overwrites it; Wine's stub returns E_NOTIMPL and leaves the
 * poison in place, which is the defect: a caller reading var.pwszVal as a string
 * then dereferences 0xABABABABABABABAB.
 */
#define INITGUID
#include <windows.h>
#include <shobjidl.h>
#include <propsys.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    IPropertyStore *store = NULL;
    HRESULT hr;
    PROPVARIANT pv;
    PROPERTYKEY key;
    unsigned char *raw = (unsigned char *)&pv;
    size_t i;

    printf("=== SHGetPropertyStoreForWindow ===\n");
    hr = SHGetPropertyStoreForWindow(GetConsoleWindow(), &IID_IPropertyStore, (void **)&store);
    printf("  -> %#lx  store=%p\n", (unsigned long)hr, (void *)store);
    if (FAILED(hr) || !store) {
        printf("  no store to test\n");
        return 1;
    }

    printf("\n=== GetValue(PKEY_AppUserModel_ID) with a poisoned PROPVARIANT ===\n");
    memset(&pv, 0xAB, sizeof(pv));
    key.fmtid.Data1 = 0x9f4c2855; key.fmtid.Data2 = 0x9f79; key.fmtid.Data3 = 0x4b39;
    memcpy(key.fmtid.Data4, "\xa8\xd0\xe1\xd4\x2d\xe1\xd5\xf3", 8);
    key.pid = 5;

    hr = store->lpVtbl->GetValue(store, &key, &pv);
    printf("  GetValue -> %#lx\n", (unsigned long)hr);
    printf("  vt      = %u\n", (unsigned) pv.vt);

    {
        int poisoned = 1;
        for (i = 0; i < sizeof(pv); i++)
            if (raw[i] != 0xAB) { poisoned = 0; break; }
        if (poisoned) {
            printf("  RESULT: PROPVARIANT WAS NOT TOUCHED -- the bug is present\n");
        } else {
            printf("  RESULT: PROPVARIANT WAS INITIALISED -- fixed\n");
        }
    }

    if (pv.vt == VT_LPWSTR && pv.pwszVal) {
        printf("  AppUserModelID = %ls\n", pv.pwszVal);
        CoTaskMemFree(pv.pwszVal);
    } else {
        printf("  (no string value; pwszVal=%p)\n", (void *)pv.pwszVal);
    }

    store->lpVtbl->Release(store);
    return 0;
}
