/*
 * uuyc-propvariant-poc.c -- focused demonstrator for the window property store's
 * uninitialised PROPVARIANT out-parameter.
 *
 * The defect, in one sentence
 * ---------------------------
 * Wine's SHGetPropertyStoreForWindow() hands out an IPropertyStore whose GetValue
 * returns E_NOTIMPL without touching *var:
 *
 *     static HRESULT WINAPI window_prop_store_GetValue(IPropertyStore *iface,
 *             const PROPERTYKEY *key, PROPVARIANT *var)
 *     {
 *         FIXME(...);
 *         return E_NOTIMPL;          <- *var left exactly as the caller had it
 *     }
 *
 * A caller that checks the HRESULT is unaffected. The hazard is for one that does
 * not: it reads whatever the slot held before the call. That is not random stack
 * garbage -- it is the caller's own previous value, which is why the consequences
 * below are reproducible rather than a matter of luck:
 *
 *   M2  stale read        the slot still holds a value from an earlier query, and
 *                         the caller believes it came from this one
 *   M3  use-after-free    the stale value is a pointer the caller has since freed
 *   M4  access violation  the stale value points at released memory, so the
 *                         unguarded dereference faults for real
 *
 * M1 measures the real store on the running Wine; M2-M4 drive a stand-in that
 * reproduces Wine's stub exactly, so the demonstration does not depend on whether
 * the Wine in front of you is patched, nor on what happens to be on the stack.
 *
 * What this does NOT show: that any particular application is affected. Whether
 * the uninitialised value is ever read in practice depends on the caller, and no
 * shipped application has been observed doing it. The COM rule is that a caller
 * must check the HRESULT; this program exists to show what happens when one does
 * not.
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -o uuyc-propvariant-poc.exe \
 *       uuyc-propvariant-poc.c -lshell32 -lole32 -luuid
 *
 * Run:
 *   wine uuyc-propvariant-poc.exe            # M1 (real store) + M2..M5 (stand-in)
 *   wine uuyc-propvariant-poc.exe --fault    # only M4; exits abnormally by design
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
#include <stdio.h>
#include <string.h>

/* PKEY_AppUserModel_ID */
static const PROPERTYKEY key_aumid = {
    { 0x9f4c2855, 0x9f79, 0x4b39, { 0xa8, 0xd0, 0xe1, 0xd4, 0x2d, 0xe1, 0xd5, 0xf3 } }, 5
};

/* ------------------------------------------------ stand-in for Wine's stub ---
 *
 * The same behaviour as dlls/shell32/shell32_main.c: GetValue returns E_NOTIMPL
 * and does not touch *var. Keeping it in-process means the consequences can be
 * shown deterministically on any Wine, patched or not, and on Windows too.
 */
typedef struct { IPropertyStore iface; LONG ref; } stub_store;

static HRESULT WINAPI stub_QueryInterface(IPropertyStore *iface, REFIID iid, void **obj)
{
    if (!obj) return E_POINTER;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IPropertyStore)) {
        *obj = iface;
        iface->lpVtbl->AddRef(iface);
        return S_OK;
    }
    *obj = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI stub_AddRef(IPropertyStore *iface)
{
    stub_store *s = (stub_store *)iface;
    return (ULONG)InterlockedIncrement(&s->ref);
}
static ULONG WINAPI stub_Release(IPropertyStore *iface)
{
    stub_store *s = (stub_store *)iface;
    return (ULONG)InterlockedDecrement(&s->ref);
}
static HRESULT WINAPI stub_GetCount(IPropertyStore *iface, DWORD *count)
{
    (void)iface; (void)count;                  /* <- count left alone */
    return E_NOTIMPL;
}
static HRESULT WINAPI stub_GetAt(IPropertyStore *iface, DWORD prop, PROPERTYKEY *key)
{
    (void)iface; (void)prop; (void)key;        /* <- key left alone */
    return E_NOTIMPL;
}
static HRESULT WINAPI stub_GetValue(IPropertyStore *iface, const PROPERTYKEY *key,
                                    PROPVARIANT *var)
{
    (void)iface; (void)key; (void)var;         /* <- var left alone */
    return E_NOTIMPL;
}
static HRESULT WINAPI stub_SetValue(IPropertyStore *iface, const PROPERTYKEY *key,
                                    const PROPVARIANT *var)
{
    (void)iface; (void)key; (void)var;
    return S_OK;
}
static HRESULT WINAPI stub_Commit(IPropertyStore *iface) { (void)iface; return S_OK; }

static const IPropertyStoreVtbl stub_vtbl = {
    stub_QueryInterface, stub_AddRef, stub_Release,
    stub_GetCount, stub_GetAt, stub_GetValue, stub_SetValue, stub_Commit
};

static IPropertyStore *make_stub(void)
{
    static stub_store s;
    s.iface.lpVtbl = (IPropertyStoreVtbl *)&stub_vtbl;
    s.ref = 1;
    return &s.iface;
}

/* ---------------------------------------------------------------- M1: facts */

static void m1_real_store(void)
{
    PROPVARIANT var;
    IPropertyStore *store = NULL;
    HRESULT hr;

    printf("=== M1: the real store on this Wine ===\n");

    hr = SHGetPropertyStoreForWindow(GetConsoleWindow(), &IID_IPropertyStore,
                                     (void **)&store);
    if (FAILED(hr) || !store) {
        printf("  SHGetPropertyStoreForWindow -> %#lx (no store)\n\n", (unsigned long)hr);
        return;
    }

    memset(&var, 0xAB, sizeof(var));
    hr = store->lpVtbl->GetValue(store, &key_aumid, &var);
    printf("  GetValue(PKEY_AppUserModel_ID) -> %#lx\n", (unsigned long)hr);
    printf("  vt = %u\n", var.vt);
    if (var.vt == 0xABAB)
        printf("  RESULT: untouched -- the defect is present on this Wine\n\n");
    else
        printf("  RESULT: initialised to vt=%u -- this Wine already fixes it\n\n", var.vt);

    store->lpVtbl->Release(store);
}

/*
 * ------------------------------------------------- M2: stale value from before
 *
 * The realistic caller bug: one PROPVARIANT variable reused across queries. The
 * first query succeeds and fills it, the second fails and leaves it alone, and
 * the caller uses the first value believing it is the second.
 */
static void m2_stale(IPropertyStore *store)
{
    PROPVARIANT var;
    static const WCHAR planted[] = L"AppUserModelID-that-GetValue-never-returned";
    HRESULT hr;

    printf("=== M2: a caller that reuses one PROPVARIANT ===\n");

    PropVariantInit(&var);
    var.vt = VT_LPWSTR;                        /* left over from an earlier query */
    var.pwszVal = (LPWSTR)planted;

    hr = store->lpVtbl->GetValue(store, &key_aumid, &var);
    printf("  GetValue -> %#lx\n", (unsigned long)hr);

    if (var.vt == VT_LPWSTR && var.pwszVal)    /* no HRESULT check, as buggy code does */
        printf("  caller reads \"%ls\"\n", var.pwszVal);
    printf("  RESULT: the caller got a value this call never produced\n\n");

    var.pwszVal = NULL;
    PropVariantInit(&var);
}

/*
 * ---------------------------------------------------- M3: use-after-free
 *
 * Same shape, but the stale pointer is one the caller has already released --
 * what happens when a PROPVARIANT is cleared and the variable is then reused for
 * a query that fails.
 */
static void m3_use_after_free(IPropertyStore *store)
{
    PROPVARIANT var;
    LPWSTR owned;
    HRESULT hr;

    printf("=== M3: the stale value is a freed pointer ===\n");

    owned = (LPWSTR)CoTaskMemAlloc(64 * sizeof(WCHAR));
    if (!owned) { printf("  CoTaskMemAlloc failed\n\n"); return; }
    lstrcpyW(owned, L"AUMID-that-was-freed");

    PropVariantInit(&var);
    var.vt = VT_LPWSTR;
    var.pwszVal = owned;
    CoTaskMemFree(owned);
    printf("  caller freed %p; the slot still holds that address\n", (void *)owned);

    hr = store->lpVtbl->GetValue(store, &key_aumid, &var);
    printf("  GetValue -> %#lx; slot holds %p\n", (unsigned long)hr, (void *)var.pwszVal);
    if ((void *)var.pwszVal == (void *)owned)
        printf("  RESULT: the unguarded caller dereferences a dangling pointer\n\n");
    else
        printf("  RESULT: slot was overwritten (%p) -- no stale read here\n\n",
               (void *)var.pwszVal);

    PropVariantInit(&var);
}

/* ----------------------------------------------------------------- M4: fault */

static int m4_fault(IPropertyStore *store)
{
    PROPVARIANT var;
    void *page;
    HRESULT hr;

    printf("=== M4: the unguarded dereference is a real access violation ===\n");

    page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!page) { printf("  VirtualAlloc failed\n"); return 1; }
    lstrcpyW((LPWSTR)page, L"page that is about to be released");
    printf("  planted a string at %p, then released the page\n", page);

    PropVariantInit(&var);
    var.vt = VT_LPWSTR;
    var.pwszVal = (LPWSTR)page;
    VirtualFree(page, 0, MEM_RELEASE);

    hr = store->lpVtbl->GetValue(store, &key_aumid, &var);
    printf("  GetValue -> %#lx; vt=%u pwszVal=%p\n",
           (unsigned long)hr, var.vt, (void *)var.pwszVal);
    /* This is the whole point of the mode: the string scan below reads the page
     * that was just released. If it faults, Wine prints the page fault and the
     * backtrace; that output IS the result. */
    printf("  the unguarded caller now scans \"%ls\" -- the page is gone\n", var.pwszVal);

    printf("  NOTE: reaching this line means the address happened to be reused;\n");
    printf("        a page fault above instead is the expected outcome.\n");
    return 0;
}

int main(int argc, char **argv)
{
    IPropertyStore *stub = make_stub();

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("uuyc-propvariant-poc -- window property store, uninitialised out parameter\n");
    printf("the caller here is one that does not check GetValue's HRESULT\n\n");

    if (argc > 1 && !strcmp(argv[1], "--fault")) {
        int rc = m4_fault(stub);
        stub->lpVtbl->Release(stub);
        return rc;
    }

    m1_real_store();
    printf("--- M2 onwards use a stand-in that reproduces Wine's stub exactly ---\n\n");

    m2_stale(stub);
    m3_use_after_free(stub);

    printf("Run with --fault for M4: the unguarded dereference as an actual\n");
    printf("access violation rather than a claim about one.\n");

    stub->lpVtbl->Release(stub);
    return 0;
}
