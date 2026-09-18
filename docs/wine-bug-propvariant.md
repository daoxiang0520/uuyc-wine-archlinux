# Wine bug: `window_prop_store_GetValue` leaves its output PROPVARIANT uninitialised

Status: **real defect, fix written, built and verified against the real
application.**

> **Scope correction.** Earlier revisions of this file presented the client's
> "enter desktop" crash as the consequence of this bug and used the drop in crash
> rate after patching Wine as the causal link. **That attribution was wrong.** The
> crash is `qwindows.dll` handing `QString::fromWCharArray` an unset
> `OUTLINETEXTMETRICW.otmpFullName` offset; it reproduces identically on stock Wine
> and on this patched build, and it is contained by the shim described in
> `qwindows-fontname-crash.md`. The defect below stands on its own -- it is
> reproducible in isolation with a poison test that needs nothing but Wine, and any
> caller that ignores the `HRESULT` reads uninitialised memory.

## Summary

`IPropertyStore::GetValue()` must initialise `*var` on every path, success or
failure. Wine's window property store returns `E_NOTIMPL` without touching it, so
any caller that does not check the `HRESULT` reads uninitialised stack memory as a
`PROPVARIANT`.

Faulting code:

```c
/* dlls/shell32/shell32_main.c */
static HRESULT WINAPI window_prop_store_GetValue(IPropertyStore *iface, const PROPERTYKEY *key, PROPVARIANT *var)
{
    FIXME("%p, {%s,%lu}, %p\n", iface, debugstr_guid(&key->fmtid), key->pid, var);
    return E_NOTIMPL;
}
```

## Impact

Any application that reads `var.pwszVal` after ignoring this store's `HRESULT`
dereferences whatever the caller's stack happened to contain. Nothing in the
client's own logs ties this to its crash -- see the scope correction above and
`qwindows-fontname-crash.md`, where the crashing call is traced to
`qwindows.dll` and `GetOutlineTextMetricsW`. The value of this bug is that it is
demonstrable in isolation:

```
EXCEPTION: 0xc0000005 ACCESS_VIOLATION
faulting : Qt5Core.dll + 0xbf11b        (QString::fromUtf16, RVA 0xbf0f0 + 0x2b)
registers: rdx = 0x373795e55bf0c513     (string pointer that is not mapped)
```

The shape is the same -- an uninitialised string pointer reaching Qt's UTF-16
scan -- which is exactly why it was mistaken for the cause. The same fault address
reproduces with the Wine patch applied, and the caller chain recovered from the
minidump belongs to `qwindows.dll`, not to any `IPropertyStore` caller.

## Reproduced in isolation

A 30-line test program calls `SHGetPropertyStoreForWindow`, poisons the
`PROPVARIANT` with `0xAB`, then calls `GetValue`. On unpatched Wine:

```text
=== SHGetPropertyStoreForWindow ===
  -> 0  store=00007FFFFE8E0F30

=== GetValue(PKEY_AppUserModel_ID) with a poisoned PROPVARIANT ===
  GetValue -> 0x80004001
  vt      = 43947
  RESULT: PROPVARIANT WAS NOT TOUCHED -- the bug is present
  (no string value; pwszVal=ABABABABABABABAB)
```

`vt = 43947 = 0xABAB` and `pwszVal = 0xABABABABABABABAB` are the poison, untouched.
**Any caller that reads `pwszVal` as a string dereferences `0xABABABABABABABAB`.**
The test source is `support/shshim/testshshim.c` in the uuyc-archlinux tree, and it
needs nothing but Wine to run.

## Minimal fix

```c
static HRESULT WINAPI window_prop_store_GetValue(IPropertyStore *iface, const PROPERTYKEY *key, PROPVARIANT *var)
{
    FIXME("%p, {%s,%lu}, %p\n", iface, debugstr_guid(&key->fmtid), key->pid, var);
    PropVariantInit(var);
    return E_NOTIMPL;
}
```

`S_OK` with an empty variant would be defensible too, but `E_NOTIMPL` plus
initialisation is the smaller behavioural change. The same audit applies to the
other methods of that store (`GetCount`, `GetAt`) and to `SHGetPropertyStoreForWindow`,
which is a `FIXME` stub returning this store.

## Evidence bundle

### 1. The call happening, with its arguments

```
030c:fixme:shell:SHGetPropertyStoreForWindow (00000000000300E0 00006FFFF1E9D520 00007FFFFE206BE0) stub!
030c:fixme:shell:window_prop_store_GetValue 00007FCE52EFF0A0, {{9f4c2855-9f79-4b39-a8d0-e1d42de1d5f3},5}, 00007FFFFE206BC0
030c:fixme:ver:GetCurrentApplicationUserModelId (00007FFFFE206BE0 00006EE0000D4140): stub
```

`{9f4c2855-9f79-4b39-a8d0-e1d42de1d5f3}, 5` is `PKEY_AppUserModel_ID`.
The FIXME prints the `var` pointer, so the address of the uninitialised buffer is
in the log.

### 2. The fault

```
030c:trace:seh:dispatch_exception code=c0000005 (EXCEPTION_ACCESS_VIOLATION) addr=00006FFFF3B3F11B
030c:trace:seh:dispatch_exception  info[1]=FFFFFFFFFFFFFFFF
030c:trace:seh:dispatch_exception rip=00006ffff3b3f11b rsp=00007ffffe205b90 rbp=000000001d410060
030c:trace:seh:dispatch_exception rdx=373795e55bf0c513
```

### 3. Identifying the faulting function

`Qt5Core.dll` is the vendor's own Qt 5.15.12 build and carries 8192 exports, so the
name comes straight from its export table:

```
RVA 0xbf11b -> nearest export at or before it: RVA 0xbf0f0
  ?fromUtf16@QString@@SA?AV1@PEBGH@Z   =  QString::fromUtf16(const ushort *, int)
```

Disassembly at the fault, matching Qt's documented behaviour of scanning for the
terminator when `size < 0`:

```asm
1800bf0fb:  test %rdx,%rdx          ; Qt's NULL check on the input pointer
1800bf0fe:  jne  0x1800bf113        ; a garbage non-NULL pointer takes this branch
1800bf113:  test %r8d,%r8d          ; size
1800bf116:  jns  0x1800bf12f        ; size < 0 -> scan
1800bf118:  mov  %ecx,%r8d
1800bf11b:  cmp  %cx,(%rdx)         ; <-- faults here
1800bf123:  inc  %r8d
1800bf126:  lea  0x2(%rax),%rax     ; 2 bytes per step: UTF-16
```

## Honest caveat

The `GetValue` call and the crash are both from the same process
(`GameViewer.exe`) in the same run, and the failure shape matches an uninitialised
string pointer exactly -- which is why this was the first hypothesis. It is now
disproven: the caller chain is recoverable from the minidump (the faulting context
*is* stored, and the stack is dumped in full), and it ends at
`qwindows.dll+0x704f0` reading `OUTLINETEXTMETRICW.otmpFullName`, not at any
`IPropertyStore` caller. The bug above stands on its own -- an uninitialised
out-parameter is a defect regardless of what it does or does not cause.

## Verified fix

Wine 11.17 was patched, built (`support/winepatch/build-patched-wine.sh`) and run
against the client. The patch is verified against the defect itself, on the same
machine and prefix:

| | before | after |
| --- | --- | --- |
| Poison test (`support/shshim/testshshim.c`) | `vt=0xABAB pwszVal=0xABABABABABABABAB` -- "PROPVARIANT WAS NOT TOUCHED" | `vt=0 pwszVal=NULL` -- "PROPVARIANT WAS INITIALISED" |

The "no crash after patching" row that earlier revisions of this table used to
claim causality is **withdrawn**: that A/B was run before the crash had a known
caller, the crash rate is dominated by the unrelated `qwindows.dll` defect, and the
crash reproduces on the patched build. What the patch demonstrably fixes is the
uninitialised out-parameter and nothing else.

## Why this has to be fixed in Wine (both workarounds were tried and do not work)

The obvious workaround on the application side is to supply a corrected
`shell32.dll`. Two routes were attempted against the real client; neither can
intercept this call, which is why the fix belongs in Wine:

1. **A native shell32 shim.** A drop-in `shell32.dll` was built that exports all
   364 names Wine's shell32 does -- 363 pass-through trampolines plus its own
   `SHGetPropertyStoreForWindow`. Wine refuses to load it: `shell32` is listed in
   `HKLM\System\CurrentControlSet\Control\Session Manager\KnownDLLs`, and the
   loader resolves it from the system directory as `builtin` regardless of a
   per-application `shell32=n,b` override. Deleting the KnownDLLs entry and
   restarting the wineserver does not change the outcome:

   ```text
   trace:loaddll:build_module Loaded L"C:\\windows\\system32\\SHELL32.dll" at ...: builtin
   ```

2. **Patching the import table.** Hooking the IAT entry would sidestep KnownDLLs,
   but no module in the application imports the symbol: a scan of every PE in the
   prefix finds `SHGetPropertyStoreForWindow` only inside Wine's own shell32
   import metadata. The caller resolves it dynamically, so there is no IAT slot to
   redirect.

Both were verified, so the one-line change above is the only practical fix.

## Reproducing the context (not required for the bug)

Wine 11.17, Arch Linux, Intel Comet Lake-U (Gen9.5), the NetEase UU Remote Windows
client 4.40.1 in a private prefix. Only the `SHGetPropertyStoreForWindow` stub plus
one `GetValue` call is needed to see the uninitialised `var`; any test program that
calls `IPropertyStore::GetValue` on the window store and ignores the `HRESULT` will
show it.

## 已提交：Bugzilla Bug 60346（2026-09-18）

<https://bugs.winehq.org/show_bug.cgi?id=60346>

| 字段 | 值 |
| --- | --- |
| Summary | `shell32: the window property store returns E_NOTIMPL without initialising its output parameters` |
| Component / Version | `shell32` / `11.17` |
| 状态 | UNCONFIRMED（提交当日） |
| 附件 | `propvariant-poison-test.c`、`propvariant-poison-test-output.txt`、`propvariant-0001-shell32-init-out-params.patch` |

报告正文与三个附件在 `docs/wine-attachments/`。**注意该 bug 的 CC 列表仍是空的** ——
不加自己就不会收到任何后续回复。

## 上游现状与建议的报告形式（2026-09-18 核对）

直接取 `master` 的 `dlls/shell32/shell32_main.c` 核对，三个方法**至今未改**：

```c
static HRESULT WINAPI window_prop_store_GetCount(IPropertyStore *iface, DWORD *count)
{
    FIXME("%p, %p\n", iface, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI window_prop_store_GetAt(IPropertyStore *iface, DWORD prop, PROPERTYKEY *key)
{
    FIXME("%p, %lu,%p\n", iface, prop, key);
    return E_NOTIMPL;
}

static HRESULT WINAPI window_prop_store_GetValue(IPropertyStore *iface, const PROPERTYKEY *key, PROPVARIANT *var)
{
    FIXME("%p, {%s,%lu}, %p\n", iface, debugstr_guid(&key->fmtid), key->pid, var);
    return E_NOTIMPL;
}
```

这段写法来自 Hans Leidekker 2019-11-01 引入该桩的补丁
（<https://list.winehq.org/hyperkitty/list/wine-devel@list.winehq.org/thread/XDAY46HSO6FODADKRF7RH2K4T4UKHREM/>），
不是有人在修的中间态。因此在已检索范围内**没有重复报告**。

**建议直接发 MR 而不是 issue。** Wine 的惯例是有测试的补丁优先；本仓库的
`support/shshim/testshshim.c` 已经是可直接改造的测试素材——把毒化断言写成
`dlls/shell32/tests/` 里的 `ok(vt == VT_EMPTY, ...)` 形式即可。

### 提交时必须与客户端崩溃切割

`window_prop_store_GetValue` 与「点进桌面崩溃」是**两个独立缺陷**，后者根因是
`GetOutlineTextMetricsW` 对 bitmap-only SFNT 字体返回 0 而 Qt 不检查返回值
（见 `qwindows-fontname-crash.md`，上游对应 Bug 53795 / MR !11388）。

本文件早期版本把两者混为一谈，并据此宣称"打完补丁崩溃就消失了"——那个因果链
已被实测推翻。**报告里绝不能再出现这种关联**，否则维护者一旦发现证伪，整份报告
的可信度都会受损。
