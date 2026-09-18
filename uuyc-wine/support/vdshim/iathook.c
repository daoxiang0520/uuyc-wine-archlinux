/*
 * iathook.c -- contain the qwindows.dll QString crash and identify its caller.
 *
 * Established from the Sentry minidump (e8ef8316-...dmp) by reading the
 * faulting CONTEXT out of MINIDUMP_EXCEPTION_STREAM (rip == the exception
 * record's address, so the context is the real faulting one):
 *
 *   fault  EXCEPTION 0xc0000005 at Qt5Core.dll + 0xbf11b
 *          0xbf0f0 is ?fromUtf16@QString@@SA?AV1@PEBGH@Z, so +0x2b is
 *              cmp WORD PTR [rdx],cx        <- the strlen loop entry
 *   rdx    the string pointer, unmapped garbage, different every run
 *   r8     0 -- because 0xbf118 is `mov r8d,ecx`, i.e. the callee already
 *          overwrote the incoming size; the branch that reaches 0xbf11b is
 *          only taken when the incoming size was NEGATIVE (NUL-terminated mode)
 *
 * Walking the stack then gave the caller chain exactly:
 *
 *   [rsp+0x48] Qt5Core.dll+0x4dcb   = ?fromWCharArray@QString@@ at 0x4db0, +0x1b
 *   [rsp+0x88] qwindows.dll+0x704f0 = the caller of fromWCharArray
 *
 * (0x88 is not a guess: fromWCharArray is `push rbx; sub rsp,0x30`, the call
 * pushes 8, fromUtf16 is `push rbx; sub rsp,0x40`, so the outer return address
 * sits 0x38+8+0x48 = 0x88 above fromUtf16's rsp. And qwindows.dll+0x704ea is
 * `call [Qt5Core!?fromWCharArray@QString@@SA?AV1@PEB_WH@Z]`, an IAT call, so
 * the return address lands on +0x704f0 with the arguments still in registers:)
 *
 *   1800704d5  mov rdx,[rdi+0xe0]        ; qwindows-internal object field
 *   1800704e1  add rdx,rdi               ; (char *)obj + *(qint64 *)(obj+0xe0)
 *   1800704e4  mov r8d,0xffffffff        ; size = -1  -> strlen mode
 *   1800704ea  call [IAT Qt5Core!QString::fromWCharArray]
 *
 * So qwindows.dll hands QString::fromWCharArray a non-NUL-terminated,
 * non-mapped pointer and asks Qt to strlen it. Nothing in Qt can survive that.
 *
 * This file does two things and nothing else:
 *   1. hooks QString::fromWCharArray in every module that imports it, validates
 *      the pointer before letting Qt walk it, and substitutes a null string
 *      when it is unmapped. The client then survives and keeps going.
 *   2. logs caller + arguments, so the remaining defect is named rather than
 *      guessed at.
 *
 * IMPORTANT: this is containment, not an explanation. The bad pointer still
 * comes from qwindows.dll; logging its value only says which call path is
 * broken.
 *
 * Because qwindows.dll is a PLUGIN loaded long after this DLL's DllMain, the
 * import patch is re-run from a polling thread; see install_thread().
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
 * Qt's fromUtf16 does `if (size < 0) size = qu_strlen(str);` and that walk
 * never stops until it finds a 0 wchar_t. So a pointer is only safe when a
 * terminator exists inside the committed region(s) reachable from it. Scanning
 * the region we already proved readable is fine; the walk is bounded by the
 * terminator or by the region end, and each step stays inside a region that
 * VirtualQuery just declared accessible.
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

/*
 * The crashing call site (qwindows.dll+0x704d5) builds its argument from rdi:
 *
 *     mov rdx,[rdi+0xe0]
 *     add rdx,rdi
 *
 * which is the documented "those four name fields are byte offsets, not
 * pointers" rule for OUTLINETEXTMETRICW -- 0xe0 is otmpFullName. rdi is set once,
 * right after malloc, from the size the size query returned, and rdi is
 * non-volatile in the Microsoft x64 ABI, so it still holds that pointer when
 * fromWCharArray is entered; neither fromWCharArray nor fromUtf16 writes it.
 *
 * Capturing it separates "the pointer is garbage" from "the structure was never
 * filled": if rdi really is the OTM buffer then *(UINT *)rdi is otmSize and
 * *(UINT64 *)(rdi+0xe0) is otmpFullName. It has to be read before this hook's own
 * prologue can reuse rdi, hence the naked entry.
 */
unsigned long long uuyc_caller_rdi;

void * __cdecl hooked_from_wchar(void *ret, const wchar_t *str, int size);

/*
 * The decisive one. qwindows.dll calls this twice:
 *   size = GetOutlineTextMetricsW(dc, 0, NULL);      <- the query
 *   otm  = malloc(size);
 *   GetOutlineTextMetricsW(dc, size, otm);           <- the fill
 * Logging both calls with their return value shows whether the size the caller
 * was told equals the size the filler demanded, and whether the fill actually
 * happened.
 */
typedef UINT (__cdecl *get_otm_fn)(HDC hdc, UINT cbData, OUTLINETEXTMETRICW *otm);
static get_otm_fn real_get_otm;

static UINT __cdecl hooked_get_otm(HDC hdc, UINT cbData, OUTLINETEXTMETRICW *otm)
{
    char caller[320], buf[512];
    UINT ret;

    describe(__builtin_return_address(0), caller, sizeof(caller));
    ret = real_get_otm(hdc, cbData, otm);
    if (ret == 0) {
        /* The documented failure value. Name the font, so the failure can be
         * attributed: a DC holding a non-scalable (bitmap) face has no outline
         * metrics at all, which is legitimate and is what Windows reports too. */
        WCHAR face[LF_FACESIZE];
        face[0] = 0;
        GetTextFaceW(hdc, LF_FACESIZE, face);
        snprintf(buf, sizeof(buf),
                 "[GetOutlineTextMetricsW] %-11s caller=%-40s cbData=%-6u otm=%p -> ret=0 "
                 "FAILED, face=[%ls]\r\n",
                 otm ? "FILL" : "QUERY", caller, cbData, (void *)otm, face);
    } else {
        snprintf(buf, sizeof(buf),
                 "[GetOutlineTextMetricsW] %-11s caller=%-40s cbData=%-6u otm=%p -> ret=%u\r\n",
                 otm ? "FILL" : "QUERY", caller, cbData, (void *)otm, ret);
    }
    log_line(buf);
    return ret;
}

__attribute__((naked)) static void *from_wchar_entry(void)
{
    __asm__ __volatile__ (
        "movq %rdi, uuyc_caller_rdi(%rip)\n\t"
        "jmp  hooked_from_wchar\n\t"
    );
}

#define TRACE_LIMIT 40
static int wchar_trace_left = TRACE_LIMIT;

/*
 * Read the OTM fields the crash site is about. Safe by construction: the caller
 * already dereferenced [rdi+0xe0] successfully before calling us, so the same
 * read cannot fault here where it did not fault there.
 */
static void describe_otm(unsigned long long rdi, char *out, size_t out_size)
{
    MEMORY_BASIC_INFORMATION mbi;

    if (!rdi) {
        snprintf(out, out_size, "rdi=NULL");
        return;
    }
    if (!VirtualQuery((const void *)rdi, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT
            || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        snprintf(out, out_size, "rdi=0x%llx (unreadable)", rdi);
        return;
    }
    snprintf(out, out_size,
             "rdi=0x%llx u32@0x00=%lu u64@0xc8=0x%llx u64@0xe0=0x%llx",
             rdi, (unsigned long)*(const unsigned int *)rdi,
             (unsigned long long)*(const unsigned long long *)(rdi + 0xc8),
             (unsigned long long)*(const unsigned long long *)(rdi + 0xe0));
}

void * __cdecl hooked_from_wchar(void *ret, const wchar_t *str, int size)
{
    char caller[320], buf[1024], otm[256];
    void *retaddr = __builtin_return_address(0);
    unsigned long long rdi = uuyc_caller_rdi;
    int size_ok = 1;

    if (str) {
        if (size >= 0)
            size_ok = range_ok(str, (size_t)size * sizeof(wchar_t));
        else
            size_ok = wchar_terminated(str);
    }

    if (str && !size_ok) {
        describe(retaddr, caller, sizeof(caller));
        describe_otm(rdi, otm, sizeof(otm));
        snprintf(buf, sizeof(buf),
                 "[fromWCharArray] BLOCKED caller=%-44s str=%p size=%-6d %s "
                 "-> passing NULL so Qt cannot walk it\r\n",
                 caller, (const void *)str, size, otm);
        log_line(buf);
        return real_from_wchar(ret, NULL, 0);
    }

    if (wchar_trace_left > 0) {
        wchar_trace_left--;
        describe(retaddr, caller, sizeof(caller));
        describe_otm(rdi, otm, sizeof(otm));
        snprintf(buf, sizeof(buf),
                 "[fromWCharArray] ok      caller=%-44s str=%p size=%-6d first4=%04x %04x %04x %04x  %s\r\n",
                 caller, (const void *)str, size,
                 str ? str[0] : 0, str ? str[1] : 0, str ? str[2] : 0, str ? str[3] : 0,
                 otm);
        log_line(buf);
    }
    return real_from_wchar(ret, str, size);
}

static void * __cdecl hooked_from_utf16(void *ret, const unsigned short *str, int size)
{
    char caller[320], buf[768];
    void *retaddr = __builtin_return_address(0);
    int size_ok = 1;

    if (str) {
        if (size >= 0)
            size_ok = range_ok(str, (size_t)size * sizeof(unsigned short));
        else
            size_ok = wchar_terminated((const wchar_t *)str);
    }
    if (str && !size_ok) {
        describe(retaddr, caller, sizeof(caller));
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
                 * check the second pass would record `replacement` as the
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
                    (void *)from_wchar_entry, (void **)&real_from_wchar);
    b = hook_symbol("Qt5Core.dll", "?fromUtf16@QString@@SA?AV1@PEBGH@Z",
                    (void *)hooked_from_utf16, (void **)&real_from_utf16);
    hook_symbol("GDI32.dll", "GetOutlineTextMetricsW",
                (void *)hooked_get_otm, (void **)&real_get_otm);
    if (a || b) {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "[iathook] fromWCharArray in %d module(s), fromUtf16 in %d module(s)\r\n",
                 a, b);
        log_line(buf);
    }
}

/*
 * qwindows.dll is a Qt PLUGIN: Qt loads it at QGuiApplication construction,
 * well after this DLL's DllMain has run. A one-shot patch at load time would
 * therefore miss the very import slot that crashes, so keep re-running the
 * (idempotent) patch until qwindows.dll shows up, then a little longer.
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
