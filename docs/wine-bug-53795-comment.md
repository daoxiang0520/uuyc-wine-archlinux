# Bug 53795 的评论稿 + CC 请求

> 目标：<https://bugs.winehq.org/show_bug.cgi?id=53795>
> 状态 UNCONFIRMED，最后修改 2022-10-15（提交当天），CC 列表 **0 人**。
> 上游修复 MR !11388 已存在但与该 bug **没有互相关联**。这份材料的作用是把两头接上。

## 提交前要做的三件事

1. 注册/登录 <https://bugs.winehq.org/createaccount.cgi>（与 GitLab 账号**不通用**）
2. 打开 bug 53795，点 **CC** 列表旁的编辑，把自己加进去（当前 0 人，加进去才有通知）
3. 在评论框贴上下面正文；用 *Add an attachment* 附上探针源码与输出

## 正文（英文，可直接粘贴）

Still reproducible on Wine 11.17, and the mechanism can now be measured from inside
the failing process rather than inferred from a backtrace.

Second application, independent of Naver LINE: **NetEase UU Remote 4.40.1**
(`GameViewer.exe`, a Qt 5.15.12 shell). Under Wine 11.17 on Arch Linux, clicking
"enter desktop" terminates it within 1-3 s, every time. The fault is the same one
described here:

```
EXCEPTION 0xc0000005 at Qt5Core.dll+0xbf11b
  0xbf0f0 = ?fromUtf16@QString@@SA?AV1@PEBGH@Z  -> +0x2b is `cmp WORD PTR [rdx],cx`
  rdx = an unmapped pointer, different on every run
```

Hooking `GetOutlineTextMetricsW` inside the process shows what the two calls Qt
makes actually return. Qt calls it twice -- once to size the buffer, once to fill
it (`qwindows.dll+0x7041f` and `+0x7043a`):

```
[GetOutlineTextMetricsW] QUERY caller=qwindows+0x7041f cbData=0 otm=NULL             -> ret=0 FAILED, face=[Noto Color Emoji]
[GetOutlineTextMetricsW] FILL  caller=qwindows+0x7043a cbData=0 otm=00007F0EDD83FCA0 -> ret=0 FAILED, face=[Noto Color Emoji]
```

So the query returns 0, Qt does not test it and calls `malloc(0)`, the fill writes
nothing, and Qt then reads `otm->otmpFullName` out of that zero-size block and
hands `otm + otmpFullName` to `QString::fromWCharArray(..., -1)`. The buffer
pointer in the `FILL` line above is byte-for-byte the `rdi` observed at the
crashing call, which is what pins the sequence.

The font is the one already named in this report: **Noto Color Emoji**. It is a
bitmap-only SFNT face, FreeType reports it as not scalable, and Wine consequently
returns 0. Removing the font makes the crash disappear here as well.

**There is a fix under review:** <https://gitlab.winehq.org/wine/wine/-/merge_requests/11388>
("win32u: Return outline metrics for bitmap-only SFNT fonts") reads `unitsPerEm`
from the `head` table and builds `OUTLINETEXTMETRIC` for these faces. It is still
a draft with no review, and it is not linked from this report, which is why the
connection is worth making explicitly.

Attached: a standalone probe that reproduces the protocol without any application
(`uuyc-otmprobe.c`, build with `x86_64-w64-mingw32-gcc -O2 -o uuyc-otmprobe.exe
uuyc-otmprobe.c -lgdi32`) and its full output. The probe runs Qt's exact two-call
protocol over every font in a prefix and validates that the four name offsets land
inside the buffer that was allocated; it can assert the post-fix behaviour for
bitmap-only faces without a GUI.

## 不要写进这份评论的东西

* **不要**提 `window_prop_store_GetValue` / PROPVARIANT。那是另一个独立缺陷，
  与本 bug 无关；混在一起会让维护者怀疑整份证据链（我们自己也正是在这里翻过车）。
* **不要**写"anv 把 Vulkan Video 限制在 Gen12+"。该说法已自查推翻
  （见 `../VERIFY.md` 5.2u）。
* **不要**下断言说"Wine 一定错"或"Qt 一定错"。事实是：Wine 对这类字体返回 0 与
  Windows 不一致（这正是 MR !11388 在修的），Qt 两次不检查返回值则是另一个健壮性
  问题。陈述观测，把判断留给维护者。

## 为什么这一步值得做

一个四年零关注、零 CC、仍是 UNCONFIRMED 的 bug，配一个无人 review 的 Draft MR，
缺的正是"这是真问题、影响不止一个应用、机制已经测清楚"的证据。这三样我们都有，
而 Bugzilla 的 CC 是让维护者真正看到它的唯一开关。
