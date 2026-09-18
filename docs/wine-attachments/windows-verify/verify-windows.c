/*
 * verify-windows.c -- settle, on real Windows, the question Wine Bug 60346 left
 * open: does the window property store initialise its output parameter when
 * GetValue fails?
 *
 * Wine's window_prop_store_GetValue returns E_NOTIMPL without touching *var.
 * Whether that is a divergence from Windows, or matches it, cannot be decided
 * from Linux. This program answers it by poisoning the PROPVARIANT and printing
 * whether the poison survived.
 *
 * Read the output like this:
 *
 *   vt = 43947 (0xABAB)   Windows left it untouched  ->  Wine matches Windows,
 *                                                         the report is invalid
 *   vt = 0    (VT_EMPTY)  Windows initialised it     ->  Wine diverges, the
 *                                                         initialisation patch is
 *                                                         the right behaviour
 *   anything else         Windows returned a value   ->  Wine is missing more
 *                                                         than initialisation
 *
 * The control case matters as much as the poisoned one: if a value that was set
 * on the store cannot be read back either, then the store simply does not work in
 * this environment and the poisoned result means nothing.
 *
 * Build (MSVC):   cl /W4 /D_WIN32_WINNT=0x0601 verify-windows.c
 *                 /link shell32.lib ole32.lib propsys.lib uuid.lib
 * Build (MinGW):  x86_64-w64-mingw32-gcc -O2 -o verify-windows.exe verify-windows.c \
 *                     -lshell32 -lole32 -luuid
 *
 * SPDX-License-Identifier: 0BSD
 */
#define _WIN32_WINNT 0x0601
#define NTDDI_VERSION 0x06010000
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <propsys.h>
#include <appmodel.h>
#include <stdio.h>
#include <string.h>

static const PROPERTYKEY key_aumid = {
    { 0x9f4c2855, 0x9f79, 0x4b39, { 0xa8, 0xd0, 0xe1, 0xd4, 0x2d, 0xe1, 0xd5, 0xf3 } }, 5
};

#define POISON_VT 0xABABu

static IPropertyStore *open_store(HWND hwnd, const char *label)
{
    IPropertyStore *store = NULL;
    HRESULT hr;

    hr = SHGetPropertyStoreForWindow(hwnd, &IID_IPropertyStore, (void **)&store);
    printf("  SHGetPropertyStoreForWindow(%-14s) -> 0x%08lx  store=%p\n",
           label, (unsigned long)hr, (void *)store);
    if (FAILED(hr)) return NULL;
    return store;
}

/* The question the bug report turns on. */
static void test_poisoned(IPropertyStore *store)
{
    PROPVARIANT var;
    HRESULT hr;

    printf("\n=== 1. GetValue with a poisoned PROPVARIANT (the actual question) ===\n");
    memset(&var, 0xAB, sizeof(var));
    var.vt = POISON_VT;

    hr = store->lpVtbl->GetValue(store, &key_aumid, &var);
    printf("  GetValue(PKEY_AppUserModel_ID) -> 0x%08lx\n", (unsigned long)hr);
    printf("  vt        = %u\n", var.vt);
    printf("  pwszVal   = %p\n", (void *)var.pwszVal);

    if (var.vt == POISON_VT)
        printf("  VERDICT: untouched -- Windows behaves like Wine; the report is invalid\n");
    else if (var.vt == VT_EMPTY)
        printf("  VERDICT: initialised to VT_EMPTY -- Wine diverges; the patch is right\n");
    else
        printf("  VERDICT: vt=%u -- Windows returned a value, Wine is missing more\n", var.vt);
}

/*
 * The other two methods of the same store. The patch has to cover only what is
 * actually measured here, so they get their own probe rather than being assumed
 * to behave like GetValue.
 */
static void test_count_and_at(IPropertyStore *store)
{
    DWORD count;
    PROPERTYKEY key;
    HRESULT hr;

    printf("\n=== 1b. GetCount / GetAt on the same store ===\n");

    count = POISON_VT;
    hr = store->lpVtbl->GetCount(store, &count);
    printf("  GetCount -> 0x%08lx  count=%lu", (unsigned long)hr, (unsigned long)count);
    printf("%s\n", count == POISON_VT ? "   (untouched)" : "   (written)");

    memset(&key, 0xAB, sizeof(key));
    hr = store->lpVtbl->GetAt(store, 0, &key);
    printf("  GetAt    -> 0x%08lx  pid=%lu", (unsigned long)hr, (unsigned long)key.pid);
    printf("%s\n", key.pid == 0xABABABABu ? "   (untouched)" : "   (written)");
}

/* Control: can the store round-trip a value at all here? */
static void test_roundtrip(IPropertyStore *store)
{
    PROPVARIANT set, got;
    static const WCHAR marker[] = L"Verify.AUMID.Roundtrip";
    HRESULT hr;

    printf("\n=== 2. control: can the store return a value that was set on it? ===\n");

    PropVariantInit(&set);
    set.vt = VT_LPWSTR;
    set.pwszVal = (LPWSTR)marker;

    hr = store->lpVtbl->SetValue(store, &key_aumid, &set);
    printf("  SetValue -> 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) {
        printf("  control unusable: the store refuses SetValue here\n");
        PropVariantInit(&set);
        return;
    }

    memset(&got, 0xAB, sizeof(got));
    got.vt = POISON_VT;
    hr = store->lpVtbl->GetValue(store, &key_aumid, &got);
    printf("  GetValue -> 0x%08lx  vt=%u", (unsigned long)hr, got.vt);
    if (got.vt == VT_LPWSTR && got.pwszVal)
        printf("  value=\"%ls\"\n", got.pwszVal);
    else
        printf("\n");

    if (got.vt == VT_LPWSTR && got.pwszVal && !lstrcmpW(got.pwszVal, marker))
        printf("  control OK: the store works, so test 1's result is meaningful\n");
    else
        printf("  control INCONCLUSIVE: a set value did not come back\n");

    PropVariantInit(&got);
    PropVariantInit(&set);
}

/* The second half of the question: what should an app with no explicit AUMID get? */
static void test_aumid_apis(void)
{
    UINT32 len;
    LONG rc;
    WCHAR buf[256];

    printf("\n=== 3. GetCurrentApplicationUserModelId, no explicit AUMID set ===\n");

    len = 0;
    rc = GetCurrentApplicationUserModelId(&len, NULL);
    printf("  (len=NULL)        -> %ld   len=%u\n", (long)rc, len);

    len = 256;
    rc = GetCurrentApplicationUserModelId(&len, buf);
    printf("  (len=256, buffer) -> %ld   len=%u", (long)rc, len);
    if (rc == 0) printf("  value=\"%ls\"\n", buf); else printf("\n");

    printf("  Wine returns %d (APPMODEL_ERROR_NO_APPLICATION) for both\n",
           (int)APPMODEL_ERROR_NO_APPLICATION);

    /*
     * The interesting case. Wine implements the explicit setter and getter
     * (dlls/shcore/main.c), storing the id in the PEB -- but
     * GetCurrentApplicationUserModelId is a stub that never looks there. If the
     * set succeeds and the explicit getter returns the value while
     * GetCurrentApplicationUserModelId still says "no application", Wine
     * contradicts itself, and no Windows reference is needed to say so.
     */
    printf("\n=== 4. after SetCurrentProcessExplicitAppUserModelID ===\n");

    if (SetCurrentProcessExplicitAppUserModelID(L"Verify.Explicit.AUMID") != S_OK) {
        printf("  SetCurrentProcessExplicitAppUserModelID failed\n");
        return;
    }
    printf("  Set...ExplicitAppUserModelID(\"Verify.Explicit.AUMID\") -> S_OK\n");

    {
        WCHAR *explicit_id = NULL;
        HRESULT hr = GetCurrentProcessExplicitAppUserModelID(&explicit_id);
        printf("  Get...ExplicitAppUserModelID -> 0x%08lx", (unsigned long)hr);
        if (SUCCEEDED(hr) && explicit_id) printf("  value=\"%ls\"\n", explicit_id);
        else printf("  (null)\n");
        CoTaskMemFree(explicit_id);
    }

    len = 256;
    rc = GetCurrentApplicationUserModelId(&len, buf);
    printf("  GetCurrentApplicationUserModelId -> %ld", (long)rc);
    if (rc == 0) printf("  value=\"%ls\"\n", buf); else printf("  (no value)\n");

    printf("\n  On Windows the last call should return 0 with \"Verify.Explicit.AUMID\",\n");
    printf("  because an explicitly set id is retrievable. If it fails here while\n");
    printf("  the explicit getter succeeds, Wine's two APIs disagree.\n");
}

int main(void)
{
    IPropertyStore *store;
    HWND console, desktop, created;
    OSVERSIONINFOEXW os;

    setvbuf(stdout, NULL, _IONBF, 0);

    memset(&os, 0, sizeof(os));
    os.dwOSVersionInfoSize = sizeof(os);
    GetVersionExW((OSVERSIONINFOW *)&os);

    printf("verify-windows -- window property store out-parameter behaviour\n");
    printf("host: Windows %lu.%lu build %lu, %s\n",
           (unsigned long)os.dwMajorVersion, (unsigned long)os.dwMinorVersion,
           (unsigned long)os.dwBuildNumber,
#ifdef _WIN64
           "x64"
#else
           "x86"
#endif
    );

    /* Try several window handles: whichever works is fine. */
    console = GetConsoleWindow();
    desktop = GetDesktopWindow();
    created = CreateWindowExW(0, L"STATIC", L"uuyc-verify", WS_OVERLAPPED,
                              0, 0, 10, 10, NULL, NULL, NULL, NULL);

    printf("\n=== 0. obtaining a property store ===\n");
    store = open_store(console, "console");
    if (!store) store = open_store(desktop, "desktop");
    if (!store) store = open_store(created, "created");
    if (!store) {
        printf("\n  no window property store available; nothing to test\n");
        return 2;
    }

    test_poisoned(store);
    test_count_and_at(store);
    test_roundtrip(store);
    store->lpVtbl->Release(store);
    test_aumid_apis();

    if (created) DestroyWindow(created);
    printf("\n=== done ===\n");
    return 0;
}
