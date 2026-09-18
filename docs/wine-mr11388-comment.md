# 给 MR !11388 的评论稿（独立复现证据）

> 目的：为 [win32u: Return outline metrics for bitmap-only SFNT fonts (!11388)](https://gitlab.winehq.org/wine/wine/-/merge_requests/11388)
> 和 [Bug 53795](https://bugs.winehq.org/show_bug.cgi?id=53795) 补一条来自**另一个真实应用**
> 的机制级证据。这不是新 issue，是对已有报告的支持。
>
> 语言：上游用英文，下面正文即英文，可直接粘贴。

---

## 正文（可直接提交）

**Independent confirmation, with the in-process call trace**

A second application hits this, and this time the two `GetOutlineTextMetricsW`
calls and the resulting invalid pointer were both measured from inside the
crashing process rather than inferred from a backtrace.

Application: NetEase UU Remote 4.40.1 (`GameViewer.exe`, a Qt 5.15.12 shell that
embeds WebView2), running under Wine 11.17 on Arch Linux. Clicking "enter desktop"
killed it in 1-3 s, every time, at

```
EXCEPTION 0xc0000005 at Qt5Core.dll+0xbf11b
  0xbf0f0 = ?fromUtf16@QString@@SA?AV1@PEBGH@Z   -> fault is +0x2b
  1800bf11b: cmp WORD PTR [rdx],cx               ; rdx = unmapped, differs every run
```

The caller chain came out of the Sentry minidump (`context.rip` equals the
exception record's address, so the context is the faulting one), and the two
return-address slots are fixed by the prologues rather than guessed --
`fromWCharArray` is `push rbx; sub rsp,0x30`, its call pushes 8, `fromUtf16` is
`push rbx; sub rsp,0x40`, so `0x38 + 8 + 0x48 = 0x88`:

| stack slot | value | meaning |
| --- | --- | --- |
| `[rsp+0x48]` | `Qt5Core+0x4dcb` | return into `?fromWCharArray@QString@@` at `0x4db0`, +0x1b |
| `[rsp+0x88]` | `qwindows.dll+0x704f0` | return into the caller |

`qwindows.dll+0x704ea` is an IAT call to `QString::fromWCharArray`, so the
arguments are still in registers at that point:

```asm
1800704d5:  mov  rdx,QWORD PTR [rdi+0xe0]     ; (char *)otm + otm->otmpFullName
1800704e1:  add  rdx,rdi
1800704e4:  mov  r8d,0xffffffff               ; size = -1, NUL-terminated mode
1800704ea:  call QWORD PTR [Qt5Core!?fromWCharArray@QString@@SA?AV1@PEB_WH@Z]
```

`rdi` is set once, right after `malloc`, from the size the query returned
(`180070431: mov rdi,rax`), and rdi is non-volatile, so it still holds the OTM
buffer at the call. Hooking the import from inside the process shows what those
two calls actually returned:

```
[GetOutlineTextMetricsW] QUERY caller=qwindows+0x7041f cbData=0   otm=NULL             -> ret=0 FAILED, face=[Noto Color Emoji]
[GetOutlineTextMetricsW] FILL  caller=qwindows+0x7043a cbData=0   otm=00007F0EDD83FCA0 -> ret=0 FAILED, face=[Noto Color Emoji]
```

`00007F0EDD83FCA0` is byte-for-byte the `rdi` observed at the blocked
`fromWCharArray` call, which pins the whole sequence:

1. Qt builds a font engine for **Noto Color Emoji** (bitmap-only SFNT).
2. `GetOutlineTextMetricsW(dc, 0, NULL)` returns **0**.
3. Qt does not test it -- the disassembly is `mov ebx,eax` followed directly by
   `mov ecx,eax; call malloc`, i.e. a `malloc(0)`.
4. `GetOutlineTextMetricsW(dc, 0, buf)` returns 0 again and writes nothing.
5. Qt does not test that either, reads `otm->otmpFullName` out of the zero-size
   allocation (heap leftovers: `u64@0xc8 = 0x2223000000000073` is `s\0\0\0"#`,
   recycled text, not an offset), computes `buf + garbage`, and hands it to
   `QString::fromWCharArray(..., -1)`.

Over one session: 470 queries and 470 fills, every pair agreeing, except the two
for this font where both return 0.

**Scope of what this does and does not say.** Wine's failure signal here is
well-formed and consistent -- it returns 0, twice, and never reports a size it
then refuses to honour. The defect is that returning 0 at all diverges from
Windows for this class of font, which is exactly what this MR addresses. Qt's
missing check is a separate robustness bug that turns the divergence into a
process kill; on Windows the same code path is never taken, because the emoji
face there (`Segoe UI Emoji`) has outlines.

**A standalone reproducer that does not need the application** is attached:
`uuyc-otmprobe.c` runs Qt's exact protocol (query size -> `malloc(size)` -> fill
with `cbData == size`) over every font in a prefix and validates that the four
name offsets land inside the buffer that was allocated. On the current build it
passes for 1260/1260 font instances, and it is what ruled out the alternative
hypothesis that Wine fills the offsets wrongly. It can be used to assert the
post-fix behaviour for bitmap-only faces without a GUI.

Attachments:
* `uuyc-otmprobe.c` -- the probe above (build: `x86_64-w64-mingw32-gcc -O2 -o uuyc-otmprobe.exe uuyc-otmprobe.c -lgdi32`)
* `probe-output.txt` -- its full output on this machine
* `qt-call-sequence.txt` -- the QUERY/FILL trace with the face name

---

## 提交前检查

- [ ] 先注册/登录 <https://gitlab.winehq.org>（用 GitHub 账号可登录）
- [ ] 在 **MR !11388** 下回复（不要新开 issue），或作为 Bug 53795 的 comment
- [ ] 附件三个文件：探针源码、探针输出、调用序列
- [ ] 语气：只陈述观测，不指定修法；明确承认 Qt 侧漏检是另一个问题
- [ ] 不要提 `window_prop_store_GetValue` —— 那是**另一个独立缺陷**，混进来会让
      维护者误以为这份证据链不可靠（我们自己也正是在这里翻过车）
- [ ] 不要提"anv 把 Vulkan Video 限制在 Gen12+" —— 该说法已自查推翻

## 为什么这比新开 issue 有价值

MR !11388 目前只有：作者自造的 EBDT/EBLC 测试字体 + `AotR_Launcher.exe` 一个应用。
它缺的正是"Qt 到底怎么走到越界读"的机制证据，以及"这不是构建配置问题"的第二来源。
上面这份把 QUERY/FILL 的返回值、`rdi` 的逐位对应、以及 Qt 两次漏检的反汇编都补齐了。
