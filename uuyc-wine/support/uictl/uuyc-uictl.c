/*
 * uuyc-uictl -- drive a Wine application's windows from inside the prefix.
 *
 * Why this exists
 * ---------------
 * The host session can be locked, in which case the X screen is covered by the
 * screen locker and any host-side capture or input goes to the locker instead of
 * the application. Nothing at the X level can reach the app then.
 *
 * Win32 is unaffected by that: PrintWindow() renders the window through its own
 * message handling, and PostMessage() puts input straight into the target
 * window's queue. Both work with the session locked.
 *
 *   uictl list
 *       every top-level window: hwnd, pid, class, title, client size
 *   uictl shot   <match> <out.bmp> [print|blt]
 *       Capture a window to BMP. "print" uses PrintWindow (correct for normal
 *       windows); "blt" copies the window DC directly, which is what works for
 *       Chromium/WebView2 surfaces because they never answer WM_PRINT and
 *       PrintWindow returns a blank image with success.
 *   uictl click  <match> <x> <y>
 *   uictl dclick <match> <x> <y>
 *       post a click at client-relative coordinates
 *   uictl move   <match> <x> <y>
 *       post a mouse move
 *   uictl text   <match> <string>
 *       post WM_CHAR for each character
 *   uictl key    <match> <vk-decimal>
 *       post WM_KEYDOWN/WM_KEYUP
 *
 * <match> is matched case-insensitively against the window title; "*" matches
 * any window. BMP is used for output because writing PNG would need zlib.
 *
 * SPDX-License-Identifier: 0BSD
 */

#include <windows.h>
#include <oleacc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_WINDOWS 512

/* Window titles are UTF-16; the console is not. Convert so Chinese titles come
 * out readable instead of as "??????". */
static void utf8(const WCHAR *src, char *dst, int dst_size)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dst_size, NULL, NULL))
        dst[0] = 0;
}

static void get_title_utf8(HWND hwnd, char *dst, int dst_size)
{
    WCHAR wide[512];

    wide[0] = 0;
    GetWindowTextW(hwnd, wide, (int)(sizeof(wide) / sizeof(wide[0])));
    utf8(wide, dst, dst_size);
}

struct win_info {
    HWND hwnd;
    DWORD pid;
    RECT rect;
    RECT client;
    char title[256];
    char klass[128];
};

static struct win_info wins[MAX_WINDOWS];
static int win_count;

static BOOL CALLBACK enum_proc(HWND hwnd, LPARAM param)
{
    struct win_info *w;
    POINT origin = { 0, 0 };

    (void)param;
    if (win_count >= MAX_WINDOWS)
        return FALSE;
    w = &wins[win_count];
    memset(w, 0, sizeof(*w));
    w->hwnd = hwnd;
    GetWindowThreadProcessId(hwnd, &w->pid);
    get_title_utf8(hwnd, w->title, sizeof(w->title));
    GetClassNameA(hwnd, w->klass, sizeof(w->klass));
    GetWindowRect(hwnd, &w->rect);
    GetClientRect(hwnd, &w->client);
    ClientToScreen(hwnd, &origin);
    win_count++;
    return TRUE;
}

static void refresh(void)
{
    win_count = 0;
    EnumWindows(enum_proc, 0);
}

static int title_matches(const char *title, const char *match)
{
    const char *t, *m;

    if (!strcmp(match, "*"))
        return 1;
    /* "visible-with-title" is the common case; allow matching by that too. */
    for (t = title; *t; t++) {
        for (m = match; *m; m++) {
            char a = *t, b = *m;
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b)
                break;
            t++;
        }
        if (!*m)
            return 1;
    }
    return 0;
}

static HWND find_window(const char *match)
{
    int i;

    /* Allow an explicit hwnd, e.g. "0x300de", so a specific child window can be
     * targeted when the interesting content lives below the top level. */
    if (!strncmp(match, "0x", 2) || !strncmp(match, "0X", 2)) {
        HWND h = (HWND)(ULONG_PTR)strtoull(match, NULL, 16);
        if (IsWindow(h))
            return h;
    }

    refresh();
    /* Prefer a visible window with a non-empty title. */
    for (i = 0; i < win_count; i++)
        if (wins[i].title[0] && IsWindowVisible(wins[i].hwnd) && title_matches(wins[i].title, match))
            return wins[i].hwnd;
    for (i = 0; i < win_count; i++)
        if (wins[i].title[0] && title_matches(wins[i].title, match))
            return wins[i].hwnd;
    return NULL;
}

/* ------------------------------------------------------------- accessibility -- */

/*
 * Chromium answers WM_GETOBJECT with a full IAccessible tree, and Wine's
 * AccessibleObjectFromWindow forwards exactly that. So the WebView2 UI can be
 * read -- and driven -- even though its pixels are composited through
 * DirectComposition and never appear in any window DC.
 */

static const char *role_name(LONG role)
{
    switch (role) {
    case ROLE_SYSTEM_PUSHBUTTON: return "button";
    case ROLE_SYSTEM_LINK:       return "link";
    case ROLE_SYSTEM_TEXT:       return "text";
    case ROLE_SYSTEM_LIST:       return "list";
    case ROLE_SYSTEM_LISTITEM:   return "listitem";
    case ROLE_SYSTEM_GROUPING:   return "group";
    case ROLE_SYSTEM_PANE:       return "pane";
    case ROLE_SYSTEM_DOCUMENT:   return "document";
    case ROLE_SYSTEM_CLIENT:     return "client";
    case ROLE_SYSTEM_GRAPHIC:    return "image";
    case ROLE_SYSTEM_STATICTEXT: return "label";
    case ROLE_SYSTEM_OUTLINE:    return "outline";
    case ROLE_SYSTEM_OUTLINEITEM:return "outlineitem";
    case ROLE_SYSTEM_TABLE:      return "table";
    case ROLE_SYSTEM_CELL:       return "cell";
    case ROLE_SYSTEM_CHECKBUTTON:return "checkbox";
    case ROLE_SYSTEM_COMBOBOX:   return "combobox";
    case ROLE_SYSTEM_MENUITEM:   return "menuitem";
    case ROLE_SYSTEM_TOOLBAR:    return "toolbar";
    case ROLE_SYSTEM_SEPARATOR:  return "separator";
    default:                     return "?";
    }
}

/* One BSTR field of a child, as UTF-8. */
static void acc_field_string(IAccessible *acc, VARIANT *child, HRESULT (STDMETHODCALLTYPE *get)(IAccessible *, VARIANT, BSTR *),
                             char *out, int out_size)
{
    BSTR bstr = NULL;

    out[0] = 0;
    if (SUCCEEDED(get(acc, *child, &bstr)) && bstr) {
        utf8(bstr, out, out_size);
        SysFreeString(bstr);
    }
}

static void acc_dump(IAccessible *acc, int depth, int max_depth);
static int acc_nodes;

static void acc_dump_children(IAccessible *acc, int depth, int max_depth)
{
    long count = 0;

    if (depth >= max_depth || acc_nodes > 4000)
        return;
    if (FAILED(acc->lpVtbl->get_accChildCount(acc, &count)) || count <= 0)
        return;

    for (long i = 1; i <= count && acc_nodes <= 4000; i++) {
        VARIANT child;
        IAccessible *sub = NULL;
        IDispatch *disp = NULL;
        char name[512], value[512];
        VARIANT role_v, state_v;
        LONG role = 0, state = 0;
        LONG left = 0, top = 0, width = 0, height = 0;
        int j;

        VariantInit(&child);
        child.vt = VT_I4;
        child.lVal = i;

        acc_field_string(acc, &child, acc->lpVtbl->get_accName, name, sizeof(name));

        VariantInit(&role_v);
        if (SUCCEEDED(acc->lpVtbl->get_accRole(acc, child, &role_v)))
            role = (role_v.vt == VT_I4) ? role_v.lVal : (role_v.vt == VT_BSTR && role_v.bstrVal ? 0x2A : 0);
        VariantClear(&role_v);

        VariantInit(&state_v);
        if (SUCCEEDED(acc->lpVtbl->get_accState(acc, child, &state_v)) && state_v.vt == VT_I4)
            state = state_v.lVal;
        VariantClear(&state_v);

        acc->lpVtbl->accLocation(acc, &left, &top, &width, &height, child);
        acc_field_string(acc, &child, acc->lpVtbl->get_accValue, value, sizeof(value));

        /* get_accChild is declared to return IDispatch; ask it for IAccessible. */
        if (acc->lpVtbl->get_accChild(acc, child, &disp) == S_OK && disp) {
            if (FAILED(disp->lpVtbl->QueryInterface(disp, &IID_IAccessible, (void **)&sub)))
                sub = NULL;
            disp->lpVtbl->Release(disp);
        }
        if (sub) {
            /* Recurse through the child's own interface, using CHILDID_SELF. */
            for (j = 0; j < depth; j++) putchar(' ');
            printf("[%ld] %s name=%s value=%s at=(%ld,%ld) %ldx%ld state=0x%lx <child interface>\n",
                   i, role_name(role), name[0] ? name : "-", value[0] ? value : "-",
                   left, top, width, height, (unsigned long)state);
            acc_nodes++;
            acc_dump(sub, depth + 1, max_depth);
            sub->lpVtbl->Release(sub);
        } else {
            for (j = 0; j < depth; j++) putchar(' ');
            if (name[0] || width || height)
                printf("[%ld] %s name=%s value=%s at=(%ld,%ld) %ldx%ld state=0x%lx\n",
                       i, role_name(role), name[0] ? name : "-", value[0] ? value : "-",
                       left, top, width, height, (unsigned long)state);
        }
        acc_nodes++;
    }
}

static void acc_dump(IAccessible *acc, int depth, int max_depth)
{
    long count = 0;

    if (depth >= max_depth || acc_nodes > 4000)
        return;
    if (FAILED(acc->lpVtbl->get_accChildCount(acc, &count)))
        return;
    for (int j = 0; j < depth; j++) putchar(' ');
    printf("childCount=%ld\n", count);
    acc_dump_children(acc, depth, max_depth);
}

static int cmd_a11y(const char *match, int max_depth)
{
    HWND hwnd = find_window(match);
    IAccessible *acc = NULL;
    HRESULT hr;

    if (!hwnd) {
        printf("no window matching '%s'\n", match);
        return 2;
    }
    hr = AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, &IID_IAccessible, (void **)&acc);
    printf("AccessibleObjectFromWindow(%p) -> %#lx acc=%p\n", (void *)hwnd, (unsigned long)hr, (void *)acc);
    if (FAILED(hr) || !acc)
        return 3;
    acc_nodes = 0;
    acc_dump(acc, 1, max_depth);
    acc->lpVtbl->Release(acc);
    return 0;
}

static void print_one(HWND hwnd, int depth)
{
    RECT rc, client;
    char title[128], klass[96];
    POINT origin = { 0, 0 };
    int i;

    get_title_utf8(hwnd, title, sizeof(title));
    GetClassNameA(hwnd, klass, sizeof(klass));
    GetWindowRect(hwnd, &rc);
    GetClientRect(hwnd, &client);
    ClientToScreen(hwnd, &origin);
    for (i = 0; i < depth; i++) putchar(' ');
    printf("%*shwnd=%p vis=%d client=%ldx%ld screen=(%ld,%ld) class=%s title=%s\n",
           depth * 2, "", (void *)hwnd, IsWindowVisible(hwnd) ? 1 : 0,
           client.right, client.bottom, origin.x, origin.y, klass, title);
}

static BOOL CALLBACK child_proc(HWND hwnd, LPARAM param)
{
    int depth = (int)param;

    print_one(hwnd, depth);
    EnumChildWindows(hwnd, child_proc, depth + 1);
    return TRUE;
}

static int cmd_tree(const char *match)
{
    HWND hwnd = find_window(match);

    if (!hwnd) {
        printf("no window matching '%s'\n", match);
        return 2;
    }
    print_one(hwnd, 0);
    EnumChildWindows(hwnd, child_proc, 1);
    return 0;
}

static int cmd_list(void)
{
    int i;

    refresh();
    printf("%d top-level windows\n", win_count);
    for (i = 0; i < win_count; i++) {
        printf("hwnd=%p pid=%-6lu vis=%d client=%ldx%ld at=(%ld,%ld) class=%s\n    title=%s\n",
               (void *)wins[i].hwnd, (unsigned long)wins[i].pid,
               IsWindowVisible(wins[i].hwnd) ? 1 : 0,
               wins[i].client.right, wins[i].client.bottom,
               wins[i].rect.left, wins[i].rect.top,
               wins[i].klass, wins[i].title);
    }
    return 0;
}

/* BITMAPFILEHEADER + BITMAPINFOHEADER, 24bpp bottom-up. */
static int save_bmp(const char *path, int width, int height, const unsigned char *bgra, int stride)
{
    FILE *f = fopen(path, "wb");
    int row, pad = (4 - (width * 3 % 4)) % 4;
    unsigned int image_size = (unsigned int)((width * 3 + pad) * height);
    unsigned char file_header[14] = { 'B', 'M' };
    unsigned char info_header[40] = { 0 };
    unsigned int file_size = 14 + 40 + image_size;

    if (!f)
        return -1;
    memcpy(file_header + 2, &file_size, 4);
    {
        unsigned int offset = 54;
        memcpy(file_header + 10, &offset, 4);
    }
    {
        unsigned int hs = 40;
        int w = width, h = height;
        unsigned short planes = 1, bpp = 24;
        memcpy(info_header, &hs, 4);
        memcpy(info_header + 4, &w, 4);
        memcpy(info_header + 8, &h, 4);
        memcpy(info_header + 12, &planes, 2);
        memcpy(info_header + 14, &bpp, 2);
        memcpy(info_header + 20, &image_size, 4);
    }
    fwrite(file_header, 1, 14, f);
    fwrite(info_header, 1, 40, f);
    for (row = height - 1; row >= 0; row--) {
        const unsigned char *p = bgra + (size_t)row * stride;
        int x;
        for (x = 0; x < width; x++) {
            unsigned char bgr[3] = { p[x * 4 + 0], p[x * 4 + 1], p[x * 4 + 2] };
            fwrite(bgr, 1, 3, f);
        }
        if (pad) {
            static const unsigned char zeros[4] = { 0 };
            fwrite(zeros, 1, pad, f);
        }
    }
    fclose(f);
    return 0;
}

static int cmd_shot(const char *match, const char *out, int use_blt)
{
    HWND hwnd = find_window(match);
    RECT rc;
    HDC win_dc, mem_dc;
    HBITMAP bmp, old;
    BITMAPINFOHEADER bi;
    unsigned char *bits = NULL;
    int width, height;
    BOOL ok;

    if (!hwnd) {
        printf("no window matching '%s'\n", match);
        return 2;
    }
    GetClientRect(hwnd, &rc);
    width = rc.right - rc.left;
    height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) {
        printf("window %p has an empty client area\n", (void *)hwnd);
        return 3;
    }

    win_dc = GetDC(hwnd);
    mem_dc = CreateCompatibleDC(win_dc);
    bmp = CreateCompatibleBitmap(win_dc, width, height);
    old = SelectObject(mem_dc, bmp);

    if (use_blt) {
        /* Chromium/WebView2 never answer WM_PRINT, so PrintWindow "succeeds"
         * with a blank bitmap. Its window DC does hold the composited frame. */
        ok = BitBlt(mem_dc, 0, 0, width, height, win_dc, 0, 0, SRCCOPY);
    } else {
        ok = PrintWindow(hwnd, mem_dc, 2);   /* PW_RENDERFULLCONTENT */
        if (!ok)
            ok = PrintWindow(hwnd, mem_dc, 0);
    }

    memset(&bi, 0, sizeof(bi));
    bi.biSize = sizeof(bi);
    bi.biWidth = width;
    bi.biHeight = -height;          /* negative: top-down */
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    bits = (unsigned char *)malloc((size_t)width * height * 4);
    if (bits) {
        HDC dump_dc = CreateCompatibleDC(win_dc);
        BITMAPINFO bi_full;
        memset(&bi_full, 0, sizeof(bi_full));
        bi_full.bmiHeader = bi;
        GetDIBits(dump_dc, bmp, 0, height, bits, &bi_full, DIB_RGB_COLORS);
        DeleteDC(dump_dc);
    }

    SelectObject(mem_dc, old);
    DeleteObject(bmp);
    DeleteDC(mem_dc);
    ReleaseDC(hwnd, win_dc);

    if (!bits)
        return 4;
    if (save_bmp(out, width, height, bits, width * 4) != 0) {
        free(bits);
        printf("cannot write %s\n", out);
        return 5;
    }
    free(bits);
    printf("shot %p (%dx%d) %s=%d -> %s\n", (void *)hwnd, width, height,
           use_blt ? "BitBlt" : "PrintWindow", ok, out);
    return 0;
}

static LPARAM mk_lparam(int x, int y)
{
    return (LPARAM)(((y & 0xffff) << 16) | (x & 0xffff));
}

static int cmd_click(const char *match, int x, int y, int twice)
{
    HWND hwnd = find_window(match);
    int i;

    if (!hwnd) {
        printf("no window matching '%s'\n", match);
        return 2;
    }
    SetForegroundWindow(hwnd);
    PostMessage(hwnd, WM_MOUSEMOVE, 0, mk_lparam(x, y));
    for (i = 0; i < (twice ? 2 : 1); i++) {
        PostMessage(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, mk_lparam(x, y));
        PostMessage(hwnd, WM_LBUTTONUP, 0, mk_lparam(x, y));
        if (twice)
            Sleep(90);
    }
    printf("%s at (%d,%d) in %p\n", twice ? "dclick" : "click", x, y, (void *)hwnd);
    return 0;
}

static int cmd_move(const char *match, int x, int y)
{
    HWND hwnd = find_window(match);

    if (!hwnd) {
        printf("no window matching '%s'\n", match);
        return 2;
    }
    PostMessage(hwnd, WM_MOUSEMOVE, 0, mk_lparam(x, y));
    return 0;
}

static int cmd_text(const char *match, const char *text)
{
    HWND hwnd = find_window(match);
    const char *p;

    if (!hwnd) {
        printf("no window matching '%s'\n", match);
        return 2;
    }
    for (p = text; *p; p++) {
        PostMessage(hwnd, WM_CHAR, (WPARAM)(unsigned char)*p, 0);
        Sleep(30);
    }
    printf("typed %d chars into %p\n", (int)strlen(text), (void *)hwnd);
    return 0;
}

static int cmd_key(const char *match, int vk)
{
    HWND hwnd = find_window(match);

    if (!hwnd) {
        printf("no window matching '%s'\n", match);
        return 2;
    }
    PostMessage(hwnd, WM_KEYDOWN, (WPARAM)vk, 0);
    PostMessage(hwnd, WM_KEYUP, (WPARAM)vk, 0);
    printf("key vk=%d into %p\n", vk, (void *)hwnd);
    return 0;
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "list";
    FILE *log;

    /* Also write everything to a file: the console is often invisible under Wine. */
    log = fopen("C:\\uuyc-uictl.log", "a");

    if (!strcmp(cmd, "list"))
        return cmd_list();
    if (!strcmp(cmd, "tree") && argc == 3)
        return cmd_tree(argv[2]);
    if (!strcmp(cmd, "a11y") && (argc == 3 || argc == 4))
        return cmd_a11y(argv[2], argc == 4 ? atoi(argv[3]) : 12);
    if (!strcmp(cmd, "shot") && (argc == 4 || argc == 5))
        return cmd_shot(argv[2], argv[3], argc == 5 && !strcmp(argv[4], "blt"));
    if (!strcmp(cmd, "click") && argc == 5)
        return cmd_click(argv[2], atoi(argv[3]), atoi(argv[4]), 0);
    if (!strcmp(cmd, "dclick") && argc == 5)
        return cmd_click(argv[2], atoi(argv[3]), atoi(argv[4]), 1);
    if (!strcmp(cmd, "move") && argc == 5)
        return cmd_move(argv[2], atoi(argv[3]), atoi(argv[4]));
    if (!strcmp(cmd, "text") && argc == 4)
        return cmd_text(argv[2], argv[3]);
    if (!strcmp(cmd, "key") && argc == 4)
        return cmd_key(argv[2], atoi(argv[3]));

    if (log) {
        fprintf(log, "usage error\n");
        fclose(log);
    }
    printf("usage: uictl list|tree <match>|a11y <match> [depth]|shot <match|0xhwnd> <out.bmp> [print|blt]|click <match> <x> <y>|"
           "dclick <match> <x> <y>|move <match> <x> <y>|text <match> <s>|key <match> <vk>\n");
    return 2;
}
