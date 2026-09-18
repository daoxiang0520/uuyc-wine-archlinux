/*
 * uuyc-shshim -- a shell32.dll shim that fixes one Wine defect.
 *
 * The defect
 * ----------
 * Wine's SHGetPropertyStoreForWindow returns S_OK and a working IPropertyStore,
 * but every method of that store returns E_NOTIMPL *without initialising its
 * output parameter*:
 *
 *   dlls/shell32/shell32_main.c
 *     window_prop_store_GetCount(iface, DWORD *count)              { return E_NOTIMPL; }
 *     window_prop_store_GetAt(iface, DWORD prop, PROPERTYKEY *key) { return E_NOTIMPL; }
 *     window_prop_store_GetValue(iface, const PROPERTYKEY *key,
 *                                PROPVARIANT *var)                 { return E_NOTIMPL; }
 *
 * A COM out-parameter must be initialised on every path, failure included.
 * Callers that do not check the HRESULT then read uninitialised stack memory:
 * reading `var.pwszVal` as a string yields a garbage pointer, and the observed
 * result was a process-killing access violation inside
 * `QString::fromUtf16()` (Qt5Core.dll) after the NetEase UU Remote client asked
 * for PKEY_AppUserModel_ID.
 *
 * What this shim does
 * -------------------
 * It replaces exactly one export, SHGetPropertyStoreForWindow, and returns its
 * OWN store whose methods initialise their out-parameters properly. Every other
 * shell32 export is a generated pass-through trampoline to Wine's real
 * shell32.dll, so the rest of shell32 is untouched.
 *
 * Always initialising `*var` is the part that matters; the AppUserModelID value
 * itself is a best-effort convenience so the application gets something usable
 * instead of an empty string.
 *
 * Environment:
 *   UUYC_SH_AUMID=empty   return E_NOTIMPL with an empty variant, to compare
 *                         behaviour against the value-returning default.
 *
 * SPDX-License-Identifier: 0BSD
 */

#define INITGUID
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <propsys.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

#include "uuyc-shshim-forwarders.h"

/* {9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3}, 5 -- PKEY_AppUserModel_ID */
static const PROPERTYKEY pkey_appusermodel_id = {
    {0x9f4c2855, 0x9f79, 0x4b39, {0xa8, 0xd0, 0xe1, 0xd4, 0x2d, 0xe1, 0xd5, 0xf3}}, 5
};

/* ---------------------------------------------------------------- logging -- */

static void raw_log(const char *s)
{
    HANDLE h;
    DWORD written;

    h = CreateFileA("C:\\uuyc-shshim.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, s, (DWORD)strlen(s), &written, NULL);
    CloseHandle(h);
}

/* No CRT in DllMain, so the getenv equivalent is the Win32 one. */
static int env_is(const char *name, const char *want)
{
    char value[64];
    DWORD got = GetEnvironmentVariableA(name, value, sizeof(value));

    return got > 0 && got < sizeof(value) && !strcmp(value, want);
}

/* ------------------------------------------------------- the property store -- */

typedef struct window_store {
    IPropertyStoreVtbl *vtbl;
    LONG ref;
} window_store;

/* mingw does not provide the IPropertyStore_* inline helpers, so go through the
 * vtable directly. `vtbl` is the first member, so the casts below are exact. */
static HRESULT store_qi(IPropertyStore *s, REFIID iid, void **out)
{
    return s->lpVtbl->QueryInterface(s, iid, out);
}

static ULONG store_addref(IPropertyStore *s)
{
    return s->lpVtbl->AddRef(s);
}

static ULONG store_release(IPropertyStore *s)
{
    return s->lpVtbl->Release(s);
}

static void pv_init(PROPVARIANT *var)
{
    memset(var, 0, sizeof(*var));
    var->vt = VT_EMPTY;
}

static int key_is_appusermodel_id(const PROPERTYKEY *key)
{
    if (!key || key->pid != pkey_appusermodel_id.pid)
        return 0;
    return !memcmp(&key->fmtid, &pkey_appusermodel_id.fmtid, sizeof(GUID));
}

/*
 * A best-effort AppUserModelID: the process image name without its extension,
 * which is what Windows ends up with for an application that never set one.
 * Returns a CoTaskMem-allocated string, as PROPVARIANT requires.
 */
static WCHAR *current_app_id(void)
{
    static const WCHAR suffix[] = L".exe";
    WCHAR path[MAX_PATH];
    WCHAR *base, *p;
    size_t len;

    if (!GetModuleFileNameW(NULL, path, MAX_PATH))
        return NULL;
    base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    len = wcslen(base);
    if (len > 4 && !_wcsicmp(base + len - 4, suffix))
        base[len - 4] = 0;
    if (!*base)
        return NULL;

    p = (WCHAR *)CoTaskMemAlloc((wcslen(base) + 1) * sizeof(WCHAR));
    if (p)
        wcscpy(p, base);
    return p;
}

static HRESULT STDMETHODCALLTYPE ws_QueryInterface(IPropertyStore *iface, REFIID iid, void **out)
{
    (void)iface;
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IPropertyStore)) {
        *out = iface;
        store_addref(iface);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE ws_AddRef(IPropertyStore *iface)
{
    return (ULONG)InterlockedIncrement(&((window_store *)iface)->ref);
}

static ULONG STDMETHODCALLTYPE ws_Release(IPropertyStore *iface)
{
    window_store *ws = (window_store *)iface;
    LONG ref = InterlockedDecrement(&ws->ref);

    if (!ref)
        free(ws);
    return (ULONG)ref;
}

static HRESULT STDMETHODCALLTYPE ws_GetCount(IPropertyStore *iface, DWORD *count)
{
    (void)iface;
    if (!count)
        return E_POINTER;
    *count = 0;                 /* initialised, unlike Wine's stub */
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ws_GetAt(IPropertyStore *iface, DWORD prop, PROPERTYKEY *key)
{
    (void)iface;
    (void)prop;
    if (key)
        memset(key, 0, sizeof(*key));
    return E_INVALIDARG;
}

static HRESULT STDMETHODCALLTYPE ws_GetValue(IPropertyStore *iface, const PROPERTYKEY *key,
        PROPVARIANT *var)
{
    (void)iface;

    if (!var)
        return E_POINTER;

    /*
     * The fix, in one line: whatever happens next, `*var` is a valid empty
     * PROPVARIANT and can never be read as uninitialised memory.
     */
    pv_init(var);

    if (!key)
        return E_INVALIDARG;
    if (!key_is_appusermodel_id(key))
        return E_NOTIMPL;

    if (!env_is("UUYC_SH_AUMID", "empty")) {
        WCHAR *id = current_app_id();

        if (id) {
            var->vt = VT_LPWSTR;
            var->pwszVal = id;
            return S_OK;
        }
    }
    return E_NOTIMPL;           /* still VT_EMPTY, never garbage */
}

static HRESULT STDMETHODCALLTYPE ws_SetValue(IPropertyStore *iface, const PROPERTYKEY *key,
        const PROPVARIANT *var)
{
    (void)iface;
    (void)key;
    (void)var;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ws_Commit(IPropertyStore *iface)
{
    (void)iface;
    return S_OK;
}

static const IPropertyStoreVtbl window_store_vtbl = {
    ws_QueryInterface,
    ws_AddRef,
    ws_Release,
    ws_GetCount,
    ws_GetAt,
    ws_GetValue,
    ws_SetValue,
    ws_Commit,
};

/* ------------------------------------------------------------------ exports -- */

__declspec(dllexport) HRESULT WINAPI SHGetPropertyStoreForWindow(HWND hwnd, REFIID riid, void **out)
{
    window_store *ws;
    HRESULT hr;

    if (!out)
        return E_POINTER;
    *out = NULL;

    ws = (window_store *)calloc(1, sizeof(*ws));
    if (!ws)
        return E_OUTOFMEMORY;
    ws->vtbl = (IPropertyStoreVtbl *)&window_store_vtbl;
    ws->ref = 1;

    hr = store_qi((IPropertyStore *)ws, riid, out);
    raw_log(hr == S_OK
            ? "[shshim] SHGetPropertyStoreForWindow -> our initialising store\r\n"
            : "[shshim] SHGetPropertyStoreForWindow -> E_NOINTERFACE\r\n");
    store_release((IPropertyStore *)ws);
    (void)hwnd;
    return hr;
}

/* Wine's real shell32, loaded by absolute path: this DLL is installed AS
 * shell32.dll, so LoadLibraryW(L"shell32.dll") would return this module and
 * every forward would recurse. */
static HMODULE real_shell32(void)
{
    static HMODULE mod;

    if (!mod) {
        mod = LoadLibraryW(L"C:\\windows\\system32\\shell32.dll");
        if (!mod)
            mod = LoadLibraryW(L"C:\\windows\\syswow64\\shell32.dll");
    }
    return mod;
}

static void *forward_resolve(const char *name)
{
    HMODULE mod = real_shell32();

    return mod ? (void *)GetProcAddress(mod, name) : NULL;
}

/* Anything unresolvable becomes a harmless failure rather than a jump to NULL. */
static LONG WINAPI forward_unavailable(void)
{
    return (LONG)0x80004001u;   /* E_NOTIMPL */
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH) {
        raw_log("==================== uuyc-shshim attached ====================\r\n");
        uuyc_forwarders_init(forward_resolve, (void *)forward_unavailable);
    }
    return TRUE;
}
