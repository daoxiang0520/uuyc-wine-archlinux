# `uuyc-propvariant-poc` — 针对窗口属性存储未初始化出参的特化验证程序

## 它验证的那个缺陷

`SHGetPropertyStoreForWindow()` 返回的 `IPropertyStore`，其 `GetValue` 在返回
`E_NOTIMPL` 时**不碰 `*var`**（`dlls/shell32/shell32_main.c`）：

```c
static HRESULT WINAPI window_prop_store_GetValue(IPropertyStore *iface,
        const PROPERTYKEY *key, PROPVARIANT *var)
{
    FIXME(...);
    return E_NOTIMPL;          /* *var 保持调用方放进去的样子 */
}
```

检查了 `HRESULT` 的调用方不受影响。**危害只针对不检查的调用方**：它读到的不是随机
垃圾，而是**它自己上一次留在那个槽里的值** —— 正因为如此，后果可以确定性复现，而不是
碰运气。

## 为什么需要"替身"

本机两个 Wine 的 `shell32.dll` **都已打补丁**（见 `winepatch/`），真实存储已经不会
触发这个行为。所以程序内置了一个**行为与 Wine 缺陷桩逐条一致**的替身实现，M2–M4 全部
跑在它上面 —— 演示因此不依赖"你面前这个 Wine 有没有打补丁"，也不依赖栈上恰好是什么。

## 四个模式

| | 内容 | 确定性 |
| --- | --- | --- |
| **M1** | 跑**真实**存储，报告出参有没有被碰 | 取决于该 Wine 是否已修 |
| **M2** | 陈旧读：调用方复用一个 `PROPVARIANT`，第二次查询失败后读到的仍是第一次的值 | **确定** |
| **M3** | 释放后使用：槽里留的是调用方已 `CoTaskMemFree` 的指针 | **确定** |
| **M4** | 访问冲突：槽里指向一块已 `VirtualFree` 的页，无守卫的解引用真的会缺页 | **确定** |

## 实测输出

常规模式见 `poc-output.txt`；M4 见 `poc-fault-output.txt`，核心两行是：

```
Unhandled exception: page fault on read access to 0x00007ffffecf0000
=>0 ucrtbase+0x6434b: cmpw $0, (%rsi)      <- 宽字符串扫描
   3 uuyc-propvariant-poc (+0x29ab)        <- 不检查 HRESULT 的调用方
```

**缺页地址正是 `pwszVal` 的值**，说明这条链是：未初始化出参 → 调用方读到自己的旧指针
→ 解引用 → 真实访问冲突。

## 这个程序**不**证明什么

* **不证明任何具体应用受影响。** 是否真的会去读那个值取决于调用方，而目前**没有观察到
  任何已发布应用这样做** —— 我们实测的那个调用方（`GameViewer.exe`）拿到
  `E_NOTIMPL` 后就回退到另一个 API 继续运行。
* **不构成安全漏洞。** 槽里的内容是调用方自己放的，攻击者不可控；最坏后果是同用户
  进程内的崩溃。
* **不回答契约问题。** COM 的通行规则是失败时出参不保证被设置、调用方必须检查
  `HRESULT`，按这条 Wine 可能是合规的。要判定只能实测 Windows 在失败路径下的行为。

换句话说：这个程序演示的是**"调用方不检查 `HRESULT` 会付出什么代价"**，而不是
"Wine 一定有错"。

## 构建与运行

```bash
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -o uuyc-propvariant-poc.exe \
    uuyc-propvariant-poc.c -lshell32 -lole32 -luuid

WINEPREFIX=<prefix> wine uuyc-propvariant-poc.exe            # M1..M3
WINEPREFIX=<prefix> wine uuyc-propvariant-poc.exe --fault    # M4
```

SPDX-License-Identifier: 0BSD
