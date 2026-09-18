/*
 * uuyc-otmprobe.c -- ask Wine directly whether GetOutlineTextMetricsW fills the
 * four name offsets it promises.
 *
 * Why this exists
 * ---------------
 * Clicking "enter desktop" in the UU Remote client killed GameViewer.exe in
 * Qt5Core.dll+0xbf11b, i.e. QString::fromUtf16+0x2b, the `cmp WORD PTR [rdx],cx`
 * that starts the NUL-terminated scan. The caller chain recovered from the
 * minidump is
 *
 *   qwindows.dll+0x704f0   (QWindowsFontEngine construction)
 *     mov rdx,[rdi+0xe0]        ; (char *)otm + otm-><0xe0>
 *     add rdx,rdi
 *     mov r8d,0xffffffff        ; size = -1 -> NUL-terminated mode
 *     call [Qt5Core!?fromWCharArray@QString@@SA?AV1@PEB_WH@Z]
 *
 * 0xe0 is OUTLINETEXTMETRICW.otmpFullName. Those four name fields are documented
 * to hold BYTE OFFSETS from the start of the structure, not pointers, which is
 * why Qt adds the structure base. So the question is whether Wine leaves the
 * offset pointing somewhere it did not fill.
 *
 * Qt's protocol, read off qwindows.dll+0x70260:
 *
 *   size = GetOutlineTextMetricsW(dc, 0, NULL);   // query
 *   otm  = malloc(size);                          // NOT checked for 0
 *   GetOutlineTextMetricsW(dc, size, otm);        // fill, cbData == what Wine asked for
 *   full = (char *)otm + otm->otmpFullName;       // then walked as a string
 *
 * This probe runs exactly that, with the buffer poisoned first, over every font
 * family the prefix enumerates, and reports any offset that does not land inside
 * the buffer it was told to use.
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -o uuyc-otmprobe.exe uuyc-otmprobe.c -lgdi32
 * Run:
 *   WINEPREFIX=<prefix> wine uuyc-otmprobe.exe
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int fonts_seen, fonts_ok, fonts_zero_size, fonts_bad_offset, fonts_unfilled;

/* Truncation pass: what does Wine report when the caller's buffer is smaller
 * than the size Wine itself just returned? A caller that follows the documented
 * protocol never does this, so this pass exists only to characterise the
 * behaviour, not to blame Qt. */
static int trunc_tested, trunc_zero, trunc_offsets_past_end, trunc_examples;

static void check_offset(const char *field, const char *name, const char *off,
                         const char *base, UINT size)
{
    ULONG_PTR v = (ULONG_PTR)off;
    const WCHAR *s;
    UINT i, room;

    if (v == 0) {
        printf("    %-16s = NULL\n", field);
        return;
    }
    if (v >= size) {
        printf("    %-16s = %llu   *** PAST THE END of the %u-byte buffer ***\n",
               field, (unsigned long long)v, size);
        fonts_bad_offset++;
        return;
    }
    s = (const WCHAR *)(base + v);
    room = (size - (UINT)v) / sizeof(WCHAR);
    for (i = 0; i < room; i++)
        if (s[i] == 0) break;
    if (i == room)
        printf("    %-16s = %llu   *** no NUL terminator inside the buffer ***\n",
               field, (unsigned long long)v);
    else
        printf("    %-16s = %-6llu \"%ls\"\n", field, (unsigned long long)v, s);
    (void)name;
}

static void check_font(HDC dc, const LOGFONTW *lf)
{
    UINT size, ret;
    OUTLINETEXTMETRICW *otm;

    size = GetOutlineTextMetricsW(dc, 0, NULL);
    fonts_seen++;

    if (size == 0) {
        /* Qt does not test this: it calls malloc(0) and then reads the four
         * offsets out of the returned block. */
        printf("  [%ls]  query returned 0 -- Qt would malloc(0) and read it\n",
               lf->lfFaceName);
        fonts_zero_size++;
        return;
    }

    otm = (OUTLINETEXTMETRICW *)malloc(size);
    if (!otm) {
        printf("  [%ls]  malloc(%u) failed\n", lf->lfFaceName, size);
        return;
    }
    memset(otm, 0xAB, size);

    ret = GetOutlineTextMetricsW(dc, size, otm);
    if (ret == 0) {
        printf("  [%ls]  size=%u but the fill call returned 0 (buffer untouched)\n",
               lf->lfFaceName, size);
        fonts_unfilled++;
        free(otm);
        return;
    }

    printf("  [%ls]  size=%u ret=%u otmSize=%u\n",
           lf->lfFaceName, size, ret, otm->otmSize);
    check_offset("otmpFamilyName", NULL, (const char *)otm->otmpFamilyName, (const char *)otm, size);
    check_offset("otmpFaceName",   NULL, (const char *)otm->otmpFaceName,   (const char *)otm, size);
    check_offset("otmpStyleName",  NULL, (const char *)otm->otmpStyleName,  (const char *)otm, size);
    check_offset("otmpFullName",   NULL, (const char *)otm->otmpFullName,   (const char *)otm, size);

    fonts_ok++;
    free(otm);
}

/*
 * Truncation pass.
 *
 * NtGdiGetOutlineTextMetricsInternalW, when the caller's buffer is smaller than
 * the required size, builds the full structure in a temporary buffer and then:
 *
 *     memcpy(lpOTM, output, cbData);
 *     free(output);
 *     ret = cbData;
 *
 * The appended name strings live *after* the structure, so they are not part of
 * that copy, yet the four offsets copied into the caller's buffer point at them
 * -- i.e. past the end of the caller's allocation. And `ret = cbData` reports
 * that as success.
 *
 * A caller following the documented protocol (query the size, allocate exactly
 * that) never reaches this. Reporting how often it happens, and what it returns,
 * is what distinguishes "Wine violates the contract" from "the caller ignored
 * the contract".
 */
static void check_truncation(HDC dc, const LOGFONTW *lf)
{
    UINT size, ret, cb;
    OUTLINETEXTMETRICW *buf;
    ULONG_PTR full;

    size = GetOutlineTextMetricsW(dc, 0, NULL);
    if (size == 0)
        return;
    cb = sizeof(OUTLINETEXTMETRICW);
    if (cb >= size)                       /* nothing to truncate for this font */
        return;

    buf = (OUTLINETEXTMETRICW *)malloc(cb);
    if (!buf)
        return;
    memset(buf, 0xAB, cb);

    trunc_tested++;
    ret = GetOutlineTextMetricsW(dc, cb, buf);

    if (ret == 0) {
        trunc_zero++;
        free(buf);
        return;
    }
    full = (ULONG_PTR)buf->otmpFullName;
    if (full >= cb) {
        trunc_offsets_past_end++;
        if (trunc_examples < 3) {
            trunc_examples++;
            printf("  [%ls] required=%u buffer=%u ret=%u -> otmpFullName=%llu, "
                   "i.e. %llu bytes PAST the end of the buffer\n",
                   lf->lfFaceName, size, cb, ret, (unsigned long long)full,
                   (unsigned long long)(full - cb));
        }
    }
    free(buf);
}

static int CALLBACK enum_cb(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM lp)
{
    HDC dc = (HDC)(ULONG_PTR)lp;
    HFONT old, font;

    (void)tm;
    if (type & RASTER_FONTTYPE)          /* only scalable fonts have outline metrics */
        return 1;
    if (lf->lfFaceName[0] == '@')        /* vertical variants: skip, same outlines */
        return 1;

    font = CreateFontIndirectW(lf);
    if (!font)
        return 1;
    old = (HFONT)SelectObject(dc, font);
    check_font(dc, lf);
    check_truncation(dc, lf);
    SelectObject(dc, old);
    DeleteObject(font);
    return 1;
}

int main(void)
{
    HDC dc;
    LOGFONTW lf;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== GetOutlineTextMetricsW: are the name offsets inside the buffer? ===\n");
    printf("protocol: query size -> malloc(size) -> fill with cbData == size "
           "(exactly what qwindows.dll does)\n\n");

    dc = CreateCompatibleDC(NULL);
    if (!dc) {
        printf("CreateCompatibleDC failed\n");
        return 1;
    }

    memset(&lf, 0, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    EnumFontFamiliesExW(dc, &lf, enum_cb, (LPARAM)(ULONG_PTR)dc, 0);

    printf("\n=== summary ===\n");
    printf("  fonts checked              : %d\n", fonts_seen);
    printf("  filled and offsets in range: %d\n", fonts_ok);
    printf("  query returned 0           : %d\n", fonts_zero_size);
    printf("  fill call returned 0       : %d\n", fonts_unfilled);
    printf("  offsets outside the buffer : %d\n", fonts_bad_offset);
    printf("\n--- truncation pass (buffer smaller than the reported size) ---\n");
    printf("  fonts tested               : %d\n", trunc_tested);
    printf("  reported failure (ret == 0): %d\n", trunc_zero);
    printf("  reported success but the offsets point past the buffer: %d\n",
           trunc_offsets_past_end);

    DeleteDC(dc);
    if (fonts_bad_offset)
        printf("\nVERDICT: Wine returned offsets that do not address anything it filled.\n");
    else if (fonts_zero_size)
        printf("\nVERDICT: no bad offsets, but the query returned 0 for %d font(s), which the\n"
               "         documented protocol does not allow -- a caller that trusts it\n"
               "         (Qt does) allocates nothing and then reads the block.\n", fonts_zero_size);
    else
        printf("\nVERDICT: Wine filled every font's offsets inside the buffer it asked for.\n");
    return 0;
}
