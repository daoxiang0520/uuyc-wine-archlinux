# The "enter desktop" crash: Qt ignores a failed `GetOutlineTextMetricsW` and reads a zero-size buffer

Status: **root cause identified and measured in-process, contained in the shipped
shim, session verified working.** The containment is `support/vdshim/iathook.c`.

The defect is on the Qt side of the boundary: `QWindowsFontEngine` in
`qwindows.dll` does not test either return value of `GetOutlineTextMetricsW`, and
the font that makes the call fail on this machine is `Noto Color Emoji`. Wine
returns the documented failure value 0 and is not the cause; see "So which side is
wrong?" below.

## Symptom

Click 「进入桌面」 on the device card. `GameViewer.exe` dies 1-3 s later, every
time, with a Sentry minidump. A/B on the same prefix:

| renderer / flags | result |
| --- | --- |
| default (vulkan) | crash |
| `renderer=gl` | crash |
| `--sw-decode` | crash |
| `--safe-graphics` (no 3d) | survives |

`--safe-graphics` survives because it never builds the video surface, not because
graphics is the fault: the crash is in font setup, which only that path skips.

## The fault

```
EXCEPTION 0xc0000005 at Qt5Core.dll+0xbf11b
RVA 0xbf0f0 = ?fromUtf16@QString@@SA?AV1@PEBGH@Z   ->  fault is fromUtf16+0x2b
```

```asm
1800bf0f6:  mov  rbx,rcx
1800bf0f9:  xor  ecx,ecx
1800bf0fb:  test rdx,rdx
1800bf0fe:  jne  0x1800bf113        ; str != NULL takes this branch
1800bf113:  test r8d,r8d
1800bf116:  jns  0x1800bf12f        ; size >= 0 skips the scan
1800bf118:  mov  r8d,ecx            ; <- size < 0: r8d is overwritten with 0
1800bf11b:  cmp  WORD PTR [rdx],cx  ; <- faults here; rdx is not mapped
1800bf123:  inc  r8d
1800bf126:  lea  rax,[rax+2]
1800bf12a:  cmp  WORD PTR [rax],cx  ; 2 bytes per step: UTF-16
```

`rdx` is the string pointer: unmapped garbage, different on every run
(`0x373795e55bf0c513`, `0x5c05872693e996a6`, `0x5c0586bb33430096`). `+0x118`
explains why the captured `r8` reads 0 even though the crashing branch is only
reachable when the incoming `size` was negative.

### Do not read `ExceptionInformation[1]` from these dumps

Every dump this client writes reports `info[1] = 0xffffffffffffffff`, including an
unrelated d3d11 out-of-bounds crash whose real faulting address was an ordinary heap
pointer. It is a placeholder, not the access address. The minidump `CONTEXT` *is*
the real faulting context -- `context.rip == ExceptionRecord.ExceptionAddress` in
both dumps -- so registers must be read from the context and the access address
inferred from the faulting instruction.

## The caller chain

Recovered from the same minidump. The return address slots are fixed by the
prologues, not guessed: `fromWCharArray` is `push rbx; sub rsp,0x30`, its `call`
pushes 8, `fromUtf16` is `push rbx; sub rsp,0x40`, so `0x38 + 8 + 0x48 = 0x88`.

| stack slot | value | meaning |
| --- | --- | --- |
| `[rsp+0x48]` | `Qt5Core.dll+0x4dcb` | return into `fromWCharArray+0x1b` |
| `[rsp+0x88]` | `qwindows.dll+0x704f0` | return into the caller of `fromWCharArray` |

A stack scan restricted to executable sections (a plain "inside the module image"
test also matches vtables and `.rdata`, and produced a wrong answer first) confirms
it, and `qwindows.dll+0x704ea` is an IAT call, so the arguments are still in
registers at that point:

```asm
1800704d5:  mov  rdx,QWORD PTR [rdi+0xe0]
1800704dc:  lea  rcx,[rsp+0x68]        ; hidden QString return slot
1800704e1:  add  rdx,rdi               ; rdx = (char *)rdi + *(qint64 *)(rdi+0xe0)
1800704e4:  mov  r8d,0xffffffff        ; size = -1  -> NUL-terminated mode
1800704ea:  call QWORD PTR [Qt5Core.dll!?fromWCharArray@QString@@SA?AV1@PEB_WH@Z]
```

## Root cause

The enclosing function (entry `qwindows.dll+0x70260`) is `QWindowsFontEngine`'s
construction path. Its imports give it away:

```
GDI32!SelectObject          GDI32!GetFontData       GDI32!GetOutlineTextMetricsW
Qt5Gui!QFontEngine::getSfntTable(uint)              Qt5Gui!QFontEngine::getCMap(...)
Qt5Gui!QFontEngine::loadKerningPairs(QFixed)        api-ms-win-crt-heap-l1-1-0!malloc
Qt5Core!QString::fromWCharArray                     Qt5Core!QFile::encodeName(QString)
```

and it loads `mov edx,0x70616d63` -- `"cmap"`, the OpenType character-map table.

The four name fields of `OUTLINETEXTMETRICW` are **byte offsets from the start of
the structure**, not pointers, which is why the call site adds the structure base.
`otmpFullName` is at `0xe0`, exactly the field used. Qt is following the documented
Win32 contract; the question was only what the structure contained.

**The structure was never filled, because the font is `Noto Color Emoji`.**

Both `GetOutlineTextMetricsW` calls are observable from inside the process. Logging
them with the DC's current face name gives, immediately before each crash:

```
QUERY cbData=0 otm=NULL             -> ret=0 FAILED, face=[Noto Color Emoji]
FILL  cbData=0 otm=00007F0EDD83FCA0 -> ret=0 FAILED, face=[Noto Color Emoji]
```

and `00007F0EDD83FCA0` is byte-for-byte the `rdi` of the blocked `fromWCharArray`
call. The chain is therefore:

1. Qt builds a font engine for **Noto Color Emoji**, a CBDT/CBLC bitmap-only font.
   FreeType reports it as not scalable (`FT_IS_SCALABLE` false), which Wine
   propagates to `gdi_font.scalable`, so
   `freetype_set_outline_text_metrics()` takes its silent `if (!font->scalable)
   return FALSE;` path -- no FIXME is emitted, which is why a `+font` run contains
   none.
2. `GetOutlineTextMetricsW(dc, 0, NULL)` returns **0**, the documented failure
   value.
3. Qt does not test it. The disassembly shows `mov ebx,eax` followed directly by
   `mov ecx,eax ; call malloc` -- a **`malloc(0)`**.
4. `GetOutlineTextMetricsW(dc, 0, buf)` returns 0 again and writes nothing.
5. Qt does not test that either. It reads `otm->otmpFullName` out of the zero-size
   allocation, gets heap leftovers, computes `buf + garbage` and hands the result
   to `QString::fromWCharArray(..., -1)`, which walks it.

The captured garbage confirms step 5: `u64@0xc8 = 0x2223000000000073` is
`s\0\0\0"#` -- recycled text from an earlier font-name allocation, not an offset.

### So which side is wrong?

**Wine is not wrong in this path.** It returned 0 -- the documented failure -- for
both the size query and the fill, consistently, and never reported a size it then
refused to honour. 470 queries and 470 fills were logged in one session; every pair
agreed, apart from the two for this font where both returned 0.

**Qt is wrong**: `QWindowsFontEngine` ignores two documented failure returns and
then reads a field out of a zero-size allocation. The situation is only reachable
on Linux because the emoji face there is `Noto Color Emoji`, which has no outlines;
the equivalent face on Windows (`Segoe UI Emoji`) does have them, so
`GetOutlineTextMetricsW` succeeds and the unchecked path is never taken.

This also explains why the crash has nothing to do with rendering, decoding or the
GPU: `--safe-graphics` survives simply because the emoji face is never reached on
that path.

## Containment (shipped)

`support/vdshim/iathook.c` patches `QString::fromWCharArray`'s IAT slot in every
loaded module and validates the pointer before Qt is allowed to walk it:

* `size >= 0` -- the whole `size * sizeof(wchar_t)` range must be committed and
  readable;
* `size < 0` -- a NUL terminator must exist inside the committed regions reachable
  from the pointer, because that is the only thing Qt's `qu_strlen` will stop at.

When validation fails the real function is called with `NULL`, which Qt answers
with the null `QString` without dereferencing anything, and the caller and value are
appended to `C:\uuyc-qtguard.log`. The affected font falls back; the process lives.

Two details matter for this to work at all:

* **`qwindows.dll` is a plugin.** Qt loads it at `QGuiApplication` construction,
  long after a shim's `DllMain`. A one-shot IAT patch at load time therefore misses
  the exact slot that crashes, which is why the patch is re-run from a polling
  thread until the plugin has appeared.
* **The patch must be idempotent.** Re-running it records `replacement` as the
  "original" on the second pass unless a slot already pointing at the replacement is
  recognised, which would make the guard call itself forever.

## Verified

Same prefix, same client, click 「进入桌面」 at 11:17:

| | before | after |
| --- | --- | --- |
| `GameViewer.exe` after the click | gone in 1-3 s | running (`up 01:11`), process count steady |
| `Sentry/reports/` | new ~46 MB dump every attempt | **no new dump**; `last_crash` still the pre-fix 03:08:50Z |
| session window | never created | `44040204 "稻香" 1536x904 @ (192,65)` |
| stream | -- | live: timer `00:00:56` -> `00:02:12`, 23-97 fps, 2.1-7.7 Mbps, 0.0% loss |
| TCP sockets | 12-13, no session peer | 26, including the 54508/54509/54511 session port family |

The timer advancing across screenshots (not just a painted frame) and the changing
fps/bitrate/latency readout are what rule out a frozen image.

## What is still not proven

* That Windows' `GetOutlineTextMetricsW` also fails for `Noto Color Emoji` has not
  been measured on Windows -- only inferred from the font having no outlines. If
  Windows succeeded there, the same Qt build would not hit this path, which is
  consistent with it being an unnoticed vendor bug rather than a Wine-specific one.
* The guard is deliberately conservative, so any *legitimately* unterminated
  `fromWCharArray(..., size < 0)` call is also coerced to the null `QString`. None
  was observed (`BLOCKED` fired exactly twice, both from `qwindows.dll+0x704f0`),
  but the trade is "a font name may be dropped" against "the process dies".

## Correction to the record

This document has been wrong twice, in opposite directions, and both are recorded
rather than quietly edited:

1. The crash was first attributed to the uninitialised `PROPVARIANT` returned by
   `window_prop_store_GetValue`. That was withdrawn; the defect is real but
   unrelated, and the crash reproduces identically with the Wine patch applied.
2. It was then attributed to Wine's `GetOutlineTextMetricsW` "not filling
   `otmpFullName`". That is what the measurement above disproves: Wine returns 0,
   and the offset in the crash is heap leftovers because Qt never checked.

What survives from the earlier analysis is the caller chain, which is what made the
question answerable at all: `[rsp+0x48] = Qt5Core+0x4dcb` (inside
`fromWCharArray`) and `[rsp+0x88] = qwindows.dll+0x704f0`, confirmed independently
by the in-process hook reporting the same caller address.
