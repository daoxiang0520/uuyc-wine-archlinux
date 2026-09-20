/*
 * iathook.c -- keep QString::fromWCharArray away from pointers Qt cannot walk.
 *
 * The crash this exists for
 * -------------------------
 * Clicking "enter desktop" in the client killed GameViewer.exe in 1-3s, every time,
 * at Qt5Core.dll+0xbf11b -- QString::fromUtf16+0x2b, the `cmp WORD PTR [rdx],cx`
 * that starts the NUL-terminated scan. The caller chain recovered from the Sentry
 * minidump is
 *
 *   qwindows.dll+0x704f0   (QWindowsFontEngine construction)
 *     mov rdx,[rdi+0xe0]        ; (char *)otm + otm-><0xe0>
 *     add rdx,rdi
 *     mov r8d,0xffffffff        ; size = -1 -> NUL-terminated mode
 *     call [Qt5Core!?fromWCharArray@QString@@SA?AV1@PEB_WH@Z]
 *
 * 0xe0 is OUTLINETEXTMETRICW.otmpFullName, and those four name fields are byte
 * offsets from the start of the structure, so Qt is following the documented rule.
 * The structure is what is wrong: for a bitmap-only SFNT face such as Noto Color
 * Emoji, Wine's GetOutlineTextMetricsW returns 0 and writes nothing, Qt does not
 * test the return, allocates a zero-size buffer and reads otmpFullName out of it.
 * The offset is then heap leftovers, and Qt walks them.
 *
 * Reported upstream as Wine bug 53795, with a fix under review in MR !11388
 * ("win32u: Return outline metrics for bitmap-only SFNT fonts"). Until that lands,
 * this file is what keeps the client alive.
 *
 * What it does
 * ------------
 * Replaces QString::fromWCharArray in every module that imports it and validates
 * the pointer before Qt is allowed to walk it:
 *
 *   size >= 0   the whole size * sizeof(wchar_t) range must be committed and readable
 *   size <  0   a NUL terminator must exist inside the committed regions reachable
 *               from the pointer, since that is the only thing Qt's qu_strlen stops at
 *
 * When validation fails the real function is called with NULL, which Qt answers with
 * the null QString without dereferencing anything, and the caller and value are
 * appended to C:\uuyc-qtguard.log. The affected font falls back; the process lives.
 *
 * Verifying it is working: the client survives "enter desktop", and the log has two
 * BLOCKED lines, both from qwindows.dll+0x704f0. Without it the process dies in
 * about a second.
 *
 * Two implementation details that matter:
 *
 *  - qwindows.dll is a PLUGIN. Qt loads it at QGuiApplication construction, long
 *    after this DLL's DllMain, so a one-shot patch at load time misses the exact
 *    slot that crashes. The patch is therefore re-run from a polling thread.
 *  - The patch must be idempotent. Without the "already ours" check the second pass
 *    records the replacement as the original and the guard calls itself forever.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>

#define LOG_PATH "C:\\uuyc-qtguard.log"

/* --------------------------------- logging -------------------------------- */

static void log_line(const char *s)
{
    HANDLE h = CreateFileA(LOG_PATH, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written;

    if (h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, s, (DWORD)strlen(s), &written, NULL);
    CloseHandle(h);
}

/* "module.dll+0x1234" for an address inside an image, else the raw value. */
static void describe(const void *addr, char *out, size_t out_size)
{
    MEMORY_BASIC_INFORMATION mbi;
    char path[MAX_PATH];
    char *base;

    if (VirtualQuery(addr, &mbi, sizeof(mbi)) && mbi.AllocationBase
            && GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, sizeof(path))) {
        base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        snprintf(out, out_size, "%s+0x%llx", base,
                 (unsigned long long)((const char *)addr - (const char *)mbi.AllocationBase));
    } else {
        snprintf(out, out_size, "0x%p", addr);
    }
}

/* ----------------------------- pointer checks ----------------------------- */

/* Is [addr, addr+len) fully committed and readable? */
static int range_ok(const void *addr, size_t len)
{
    MEMORY_BASIC_INFORMATION mbi;
    const char *p = (const char *)addr;
    const char *end = p + len;

    if (len == 0)
        return 1;
    while (p < end) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi)))
            return 0;
        if (mbi.State != MEM_COMMIT)
            return 0;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
            return 0;
        p = (const char *)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}

/*
 * Can Qt safely strlen() this as a NUL-terminated wchar_t string?
 *
 * Qt's fromUtf16 does `if (size < 0) size = qu_strlen(str);` and that walk never
 * stops until it finds a 0 wchar_t, so a pointer is only safe when a terminator
 * exists inside the committed region(s) reachable from it. Scanning regions that
 * VirtualQuery just declared accessible cannot fault.
 */
static int wchar_terminated(const wchar_t *s)
{
    MEMORY_BASIC_INFORMATION mbi;
    const wchar_t *p = s;
    int regions;

    for (regions = 0; regions < 64; regions++) {
        const char *region_end;
        size_t avail;
        const wchar_t *end;

        if (!VirtualQuery(p, &mbi, sizeof(mbi)))
            return 0;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return 0;

        region_end = (const char *)mbi.BaseAddress + mbi.RegionSize;
        avail = (size_t)(region_end - (const char *)p) & ~(size_t)1;
        if (avail < sizeof(wchar_t))
            return 0;
        for (end = (const wchar_t *)((const char *)p + avail); p < end; p++)
            if (*p == 0)
                return 1;
        /* no terminator in this region: keep looking in the next one */
    }
    return 0;
}

/* -------------------------------- hooks ----------------------------------- */

typedef void *(__cdecl *from_utf16_fn)(void *ret, const unsigned short *str, int size);
typedef void *(__cdecl *from_wchar_fn)(void *ret, const wchar_t *str, int size);
static from_utf16_fn real_from_utf16;
static from_wchar_fn real_from_wchar;

static void * __cdecl hooked_from_wchar(void *ret, const wchar_t *str, int size)
{
    char caller[320], buf[768];
    int size_ok = 1;

    if (str) {
        if (size >= 0)
            size_ok = range_ok(str, (size_t)size * sizeof(wchar_t));
        else
            size_ok = wchar_terminated(str);
    }

    if (str && !size_ok) {
        describe(__builtin_return_address(0), caller, sizeof(caller));
        snprintf(buf, sizeof(buf),
                 "[fromWCharArray] BLOCKED caller=%-44s str=%p size=%-6d "
                 "-> passing NULL so Qt cannot walk it\r\n",
                 caller, (const void *)str, size);
        log_line(buf);
        return real_from_wchar(ret, NULL, 0);
    }
    return real_from_wchar(ret, str, size);
}

/* The same protection on the sibling entry point, for the same reason. */
static void * __cdecl hooked_from_utf16(void *ret, const unsigned short *str, int size)
{
    char caller[320], buf[768];
    int size_ok = 1;

    if (str) {
        if (size >= 0)
            size_ok = range_ok(str, (size_t)size * sizeof(unsigned short));
        else
            size_ok = wchar_terminated((const wchar_t *)str);
    }
    if (str && !size_ok) {
        describe(__builtin_return_address(0), caller, sizeof(caller));
        snprintf(buf, sizeof(buf),
                 "[fromUtf16] BLOCKED caller=%-44s str=%p size=%d\r\n",
                 caller, (const void *)str, size);
        log_line(buf);
        return real_from_utf16(ret, NULL, 0);
    }
    return real_from_utf16(ret, str, size);
}

/* --------------------------- import table patch --------------------------- */

/* Replace one imported symbol's IAT slot across every loaded module. */
static int hook_symbol(const char *want_module, const char *want_symbol, void *replacement,
                       void **original)
{
    HMODULE mods[512];
    DWORD needed = 0, count, m;
    int hits = 0;

    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        return 0;
    count = needed / sizeof(mods[0]);
    if (count > sizeof(mods) / sizeof(mods[0]))
        count = sizeof(mods) / sizeof(mods[0]);

    for (m = 0; m < count; m++) {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)mods[m];
        IMAGE_NT_HEADERS *nt;
        IMAGE_IMPORT_DESCRIPTOR *imp;
        DWORD rva;

        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            continue;
        nt = (IMAGE_NT_HEADERS *)((char *)dos + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            continue;
        rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        if (!rva)
            continue;

        for (imp = (IMAGE_IMPORT_DESCRIPTOR *)((char *)dos + rva); imp->Name; imp++) {
            const char *dll = (const char *)dos + imp->Name;
            IMAGE_THUNK_DATA *thunk, *orig;
            DWORD old_protect;

            if (_stricmp(dll, want_module))
                continue;
            thunk = (IMAGE_THUNK_DATA *)((char *)dos + imp->FirstThunk);
            orig = imp->OriginalFirstThunk
                 ? (IMAGE_THUNK_DATA *)((char *)dos + imp->OriginalFirstThunk) : thunk;

            for (; orig->u1.AddressOfData; orig++, thunk++) {
                IMAGE_IMPORT_BY_NAME *name;
                char symbol[256];

                if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                    continue;
                name = (IMAGE_IMPORT_BY_NAME *)((char *)dos + orig->u1.AddressOfData);
                snprintf(symbol, sizeof(symbol), "%s", (const char *)name->Name);
                if (strcmp(symbol, want_symbol))
                    continue;

                /* Already ours: the installer runs repeatedly, and without this
                 * check the second pass would record the replacement as the
                 * original and recurse forever. */
                if (thunk->u1.Function == (ULONG_PTR)replacement) {
                    hits++;
                    continue;
                }
                if (*original == NULL)
                    *original = (void *)thunk->u1.Function;
                if (VirtualProtect(&thunk->u1.Function, sizeof(void *),
                                   PAGE_READWRITE, &old_protect)) {
                    thunk->u1.Function = (ULONG_PTR)replacement;
                    VirtualProtect(&thunk->u1.Function, sizeof(void *), old_protect, &old_protect);
                    hits++;
                    {
                        char cbuf[128], buf[512];
                        describe((const void *)mods[m], cbuf, sizeof(cbuf));
                        snprintf(buf, sizeof(buf), "[iathook] patched %s in %s\r\n",
                                 symbol, cbuf);
                        log_line(buf);
                    }
                }
            }
        }
    }
    return hits;
}

static void install_once(void)
{
    int a, b;

    a = hook_symbol("Qt5Core.dll", "?fromWCharArray@QString@@SA?AV1@PEB_WH@Z",
                    (void *)hooked_from_wchar, (void **)&real_from_wchar);
    b = hook_symbol("Qt5Core.dll", "?fromUtf16@QString@@SA?AV1@PEBGH@Z",
                    (void *)hooked_from_utf16, (void **)&real_from_utf16);
    if (a || b) {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "[iathook] fromWCharArray in %d module(s), fromUtf16 in %d module(s)\r\n",
                 a, b);
        log_line(buf);
    }
}

/*
 * qwindows.dll is a Qt PLUGIN: Qt loads it at QGuiApplication construction, well
 * after this DLL's DllMain has run, so the import slot that crashes does not exist
 * yet at load time. Keep re-running the (idempotent) patch until the plugin has
 * appeared, then once more.
 */
static DWORD WINAPI install_thread(LPVOID param)
{
    int i;
    int seen_qwindows = 0;

    (void)param;
    for (i = 0; i < 600; i++) {          /* 600 * 100ms = 60s */
        install_once();
        if (!seen_qwindows && GetModuleHandleA("qwindows.dll")) {
            seen_qwindows = 1;
            log_line("[iathook] qwindows.dll appeared; re-patched its imports\r\n");
        }
        if (seen_qwindows && i > 0)
            break;                        /* patched after the plugin loaded */
        Sleep(100);
    }
    return 0;
}

void uuyc_fromutf16_hook_install(void)
{
    static int started;

    install_once();
    if (!started) {
        started = 1;
        CreateThread(NULL, 0, install_thread, NULL, 0, NULL);
    }
}
