# 网易UU远程 for Arch Linux

网易UU远程（NetEase UU Remote，`uuyc`）官方只发布 Windows、macOS、iOS 和 Android
版本，没有 Linux 客户端。本仓库把你工作区里的两个官方安装包适配成可在 Arch Linux
上安装、运行的本地包：

| 上游文件 | 适配产物 | 运行方式 |
| --- | --- | --- |
| `uuyc_4.40.1.exe`（Windows 安装包，NSIS，x86 PE 引导 + x86-64 载荷） | `uuyc-wine 4.40.1-21` | 独立 Wine 前缀，`uuyc-wine` 启动 |
| `uuyc_4.40.0.apk`（Android 客户端，`com.netease.uuremote`，仅 arm64-v8a） | `uuyc-android 4.40.0-1` | Waydroid 容器，`uuyc-android` 启动 |

两个包都已在本机用 `makepkg` 完整构建；Wine 侧跑通了从零装配前缀的端到端回归
（`tests/smoke-prefix.sh`，退出码 0），安卓侧受本机内核（无 `binder_linux`、无
`/dev/kvm`）限制只完成静态与前置条件验证。全部证据与诚实的边界说明见 `VERIFY.md`。

构建产物：

```text
uuyc-wine-4.40.1-21-x86_64.pkg.tar.zst   90,997,150 字节
  sha256 60958564be80fda40f1046a17584e29f73dc6c61075ce990d75d9b087baeb0d1
  pkgrel  21          源文件 48 项（校验和全部随包记录于 PKGBUILD）
uuyc-android-4.40.0-1-x86_64.pkg.tar.zst 23,795,972 字节
  sha256 bb7490888196a5a8964374b7de76a0da916d1db7f0d08d46b9bdbb6b589f204d
  pkgrel  1           源文件 8 项（APK 以官方原始字节打包）
```

---

## 1. 适配思路

### 1.1 Windows 安装包 → `uuyc-wine`

官方 Windows 客户端是原生 x86-64 PE（`GameViewer.exe` + `bin/GameViewerServer.exe` +
Qt5 + `streamer.dll`），没有 Electron/Qt-Linux 通用层可以替换，因此适配方式是把官方
安装包原样装进一个**专属 Wine 前缀**，并补齐 Wine 缺少的 Windows 组件：

1. **`wevtapi.dll` 兼容层（本仓库编译）**
   `bin/GameViewerServer.exe` 直接导入 `wevtapi.dll` 并查询 Windows 事件日志。
   Wine 内置实现在 `EvtOpenPublisherMetadata` 处走进未实现的桩函数，会直接终止
   服务进程。`uuyc-wine/support/wevtapi-shim/` 用小段 x86-64 汇编（`as` +
   `objcopy` + `ld -mi386pep`）编译出一个只导出 5 个符号的原生 DLL，让这些调用
   干净地返回失败，服务因此可以常驻。

2. **WebView2 运行时**
   客户端 UI 依赖 Microsoft Edge WebView2。包内不带运行时，首次启动时用安装包
   自带的 `bin/MicrosoftEdgeWebview2Setup.exe /silent /install` 装进前缀，并为
   `msedgewebview2.exe` 单独设置 Windows 8 兼容模式。

3. **服务与前后台进程编排**
   Wine 的服务进程无法拉起交互式子进程，所以启动器先把 `GameViewerService` 置为
   `RUNNING`，再显式拉起 `GameViewerHealthd.exe`（守护）和 `GameViewerServer.exe`
   （串流/控制服务），最后才启动 `GameViewer.exe` 界面。

4. **桌面集成隔离**
   关闭 `winemenubuilder`，把前缀内的 Desktop 目录指向前缀自己，避免安装器往
   宿主机桌面写 `.lnk`；`.desktop`、图标、`uuremote:` URI 处理器全部由包提供。

### 1.2 Wine 源码改动与进程内兼容层

「适配 Arch Linux」在这条路线上分成两类改动，必须分开理解：一类真的改了 Wine 源码，
另一类只存在于客户端进程内部、不动 Wine。

#### 1.2.1 Wine 源码改动：`shell32` 窗口属性存储的出参初始化

补丁只有一处，改 `dlls/shell32/shell32_main.c` 里窗口属性存储的三个方法，全部是
**在返回 `E_NOTIMPL` 之前先初始化出参**：

| 函数 | 改动 | 改动前 |
| --- | --- | --- |
| `window_prop_store_GetCount` | `*count = 0;` | 直接 `return E_NOTIMPL`，`count` 原样不动 |
| `window_prop_store_GetAt` | `memset(key, 0, sizeof(*key));` | `key` 原样不动 |
| `window_prop_store_GetValue` | `PropVariantInit(var);` | `var` 原样不动 |

`IPropertyStore::GetValue()` 的契约要求**无论成功失败都初始化 `*var`**。Wine 不碰它，
于是不检查 `HRESULT` 的调用方会把未初始化的栈内存当成 `PROPVARIANT` 读；当 `vt` 被
解释成 `VT_LPWSTR` 时，`pwszVal` 就是一个任意指针。

**这一条为什么必须改在 Wine 里**（两条应用侧旁路都试过，都不通）：

1. 造一个原生 `shell32.dll` 顶替 —— `shell32` 在
   `HKLM\System\CurrentControlSet\Control\Session Manager\KnownDLLs` 里，加载器
   永远从系统目录取 builtin，逐应用的 `shell32=n,b` 覆盖被忽略；
2. 打导入表 —— 全前缀扫描下来，**没有任何客户端模块静态导入**
   `SHGetPropertyStoreForWindow`，调用方是动态解析的，没有 IAT 槽可改。

复现与构建（脚本随包发布）：

```bash
# 复现缺陷本身：30 行毒化测试，只需要 Wine，不需要客户端
WINEPREFIX=<prefix> wine /usr/share/uuyc-wine/shshim/testshshim.exe
#   未修复：vt=0xABAB  pwszVal=0xABABABABABABABAB  -> "PROPVARIANT WAS NOT TOUCHED"
#   已修复：vt=0        pwszVal=NULL                -> "PROPVARIANT WAS INITIALISED"

# 重新编译打过补丁的 Wine（winehq 官方 wine-11.17 源码，仅 x86_64）
/usr/share/uuyc-wine/winepatch/build-patched-wine.sh

# 两种落地方式，任选
UUYC_WINE_BIN=~/wine-patched/bin/wine uuyc-wine    # 私有前缀，不动系统
/usr/share/uuyc-wine/winepatch/install-into-system-wine.sh install   # 只换 shell32.dll，可 --revert
```

构建脚本里记了两个必须绕开的坑：`dlls/opencl` 缺 OpenCL 头文件必然编译失败，所以用
`make -k`；而 `make -k install` **不会**安装 33 个 unix 侧 `.so`，漏掉就会
`wine: could not load ntdll.so`，脚本手动补齐。

本机实测：系统 Wine 的 `shell32.dll` 已换成补丁版本（sha256 `fa72502f…`，与编译产物
一致），毒化测试输出 `vt = 0` / `PROPVARIANT WAS INITIALISED -- fixed`。

> **明确边界**：这一个补丁**不是**「点进桌面就崩」的原因。打不打它，崩溃表现完全
> 一致。它修的是"未初始化出参"这个独立缺陷。

#### 1.2.2 进程内兼容层（不改 Wine）

下面两组改动通过 `HKLM\...\AppDefaults\<exe>\DllOverrides` 按进程注入，只影响
客户端自己的进程，不污染前缀里其它程序：

| 兼容层 | 形态 | 作用 |
| --- | --- | --- |
| `wevtapi.dll` | 原生 x86-64 DLL（`as`+`objcopy`+`ld -mi386pep`） | 让 `GameViewerServer.exe` 的事件日志查询干净地失败，服务得以常驻 |
| `d3d11.dll`（vdshim） | Wine d3d11 的完整转发代理（45/45 导出） | 代理 `ID3D11VideoDevice`，把硬解引到 Vulkan Video；同时承载下面这个守卫 |

**`QString::fromWCharArray` 守卫**（`support/vdshim/iathook.c`）——「点进桌面」崩溃的
真正遏制手段，根因见 [docs/qwindows-fontname-crash.md](docs/qwindows-fontname-crash.md)：

* 崩溃点：`qwindows.dll+0x704f0`（`QWindowsFontEngine` 构造）把
  `(char *)otm + otm->otmpFullName` 交给 `QString::fromWCharArray(..., -1)`；
* 触发字体是 **`Noto Color Emoji`**：CBDT/CBLC 纯位图字体，`FT_IS_SCALABLE` 为假，
  所以 `GetOutlineTextMetricsW` 按文档返回 **0**；Qt 两次都没检查返回值 ——
  `malloc(0)` 之后直接读 `otmpFullName`，读到堆残留，算出未映射指针；
* 守卫在任何模块的 IAT 里替换 `QString::fromWCharArray`，调用前校验指针
  （`size < 0` 时要求可达区间内存在 NUL 终止符），不合法就以 `NULL` 调用真函数并
  记录到 `C:\uuyc-qtguard.log`。守卫本身不修 Wine、也不修 Qt，只是不让一个字体名
  取不到演变成进程死亡。

`support/otmprobe/uuyc-otmprobe.c` 是独立复现工具：不需要客户端，按 Qt 完全相同的
两段式协议（查询尺寸 → `malloc` → 按该尺寸填充）遍历前缀里全部字体并校验四个名字
偏移，本机 1260 个字体实例全部通过。

### 1.3 Android 客户端 → `uuyc-android`

APK 里的原生代码只有 `arm64-v8a`（19 个 `.so`，含 17 MB 的 `libstreamer.so`、
`libinputmanager.so`、`libsqlcipher.so` 等），**不存在 x86/x86_64 版本**，也没有
公开源码可以重新编译，所以无法把它“移植”成 glibc 原生程序。可行且可维护的路线
是把 APK 跑在 Waydroid（Android in container）里：

1. 包内直接安装官方 APK（不重打包、不重签名，保持官方签名和 `extractNativeLibs=false`
   的页对齐布局）。
2. `uuyc-android` 启动器负责：检查 Wayland 会话 / binder 模块 / Waydroid 镜像、
   按需启动 session、首次安装 APK、APK 内容变化时 `-r` 覆盖安装（保留登录态）、
   用 `cmd package resolve-activity` 解析启动 Activity 后拉起应用。
3. x86_64 主机需要 ARM 翻译层：`uuyc-android-arm` 会安装 libndk 并写入
   `ro.dalvik.vm.nativebridge` 等属性。
4. `uuyc-android-verify` 输出逐项检查结果；`apk-info.py` 直接解析 APK，报告包名、
   版本、ABI、dex 数量（本机实测：`com.netease.uuremote` / 4.40.0 / arm64-v8a / 3 个 dex）。

---

## 2. 目录结构

```text
uuyc-archlinux/
├── build.sh                     顶层构建入口（等价于 scripts/build.sh）
├── scripts/build.sh             makepkg 构建两个包
├── scripts/install.sh           pacman -U 安装 + Waydroid 初始化提示
├── uuyc-wine/
│   ├── PKGBUILD                 4.40.1-21
│   ├── winepatch/               补丁 + 可复现的 Wine 构建脚本 + 单文件安装脚本
│   ├── vdshim/                  d3d11 转发代理 + fromWCharArray 守卫（含源码）
│   ├── otmprobe/                独立复现工具：校验 GetOutlineTextMetricsW 的名字偏移
│   ├── shshim/                  shell32 出参缺陷的毒化测试（只需 Wine）
│   ├── uuyc-wine                启动器（前缀装配、服务编排、--stop/--repair）
│   ├── uuyc-wine.desktop        Desktop Entry（含 uuremote: URI）
│   ├── uuyc-wine.install        pacman 卸载提示
│   ├── wevtapi.S / wevtapi.def  wevtapi.dll 兼容层源码（as/objcopy/ld 编译）
│   ├── LICENSE                  0BSD（打包脚本与兼容层）
│   └── local/uuyc-installer.exe 官方 Windows 安装包（90,790,272 字节）
├── tests/smoke-prefix.sh        从零装配前缀的端到端回归（Wine 侧）
├── uuyc-android/
│   ├── PKGBUILD                 4.40.0-1
│   ├── uuyc-android             Waydroid 启动器
│   ├── uuyc-android-arm         ARM 翻译层安装助手（libndk/libhoudini）
│   ├── uuyc-android-verify      环境自检
│   ├── apk-info.py              APK 元数据解析（无需 aapt）
│   ├── uuyc-android.desktop     Desktop Entry
│   ├── icon.png                 256×256 图标
│   ├── LICENSE                  0BSD
│   └── local/com.netease.uuremote.apk  官方 APK（26,492,307 字节）
└── support/uuyc.desktop         合并入口（默认 Wine，附加“安卓客户端”动作）
```

上游文件校验值（用于确认你手上的文件与本仓库构建时一致）：

```text
64f918b80a99a570dd43a0ca0c8c4fac5b9b734ad6308e0f5c69bc0be6ce5167  uuyc_4.40.1.exe
2cc14a4a3fb18066ac0f38ccbc08b17bc05e6d72fb150517269e74316554fef9  uuyc_4.40.0.apk
```

---

## 3. 快速开始

```bash
cd uuyc-archlinux
./scripts/build.sh          # 产出两个 .pkg.tar.zst
./scripts/install.sh        # sudo pacman -U，并打印 Waydroid 后续步骤

uuyc-wine                   # Windows 客户端（首次启动需数分钟装配前缀）
uuyc-android --status       # 查看安卓侧前置条件
uuyc-android                # 安卓客户端（需要 Waydroid 就绪）
```

详细的安装、运行与排障步骤见 **`INSTALL.md`**；构建与测试证据见 **`VERIFY.md`**；
会话崩溃的完整根因与取证过程见 **`docs/qwindows-fontname-crash.md`**（含两次自我更正
的记录：先错怪 PROPVARIANT，再错怪 Wine）。

远程会话打不开或点了设备就退回？先跑诊断脚本，它会一次性收集前缀、显卡、
WebView2、崩溃转储与日志位置，并给出按可能性排序的试错命令：

```bash
uuyc-wine-doctor > report.txt
uuyc-wine-d3dprobe --all      # Wine 到底支持哪些视频格式（实证工具）
uuyc-wine-vkvideo             # 驱动到底暴不暴露 Vulkan Video 硬解（决定性问题）
```

---

## 4. 能力边界（重要）

* **当控制端**（本机去控制别的机器）：核心问题是 Wine 的 D3D11 硬解**只挂 Vulkan Video**
  一个后端（wined3d 没有 VA-API 后端），而 Intel 的 ANV 驱动**默认不暴露**
  `VK_KHR_video_decode_h264`，要用 `ANV_DEBUG=video-decode` 打开：

  ```bash
  uuyc-wine-vkvideo        # 先确认：加 flag 后 VK_KHR_video_decode_h264 是否变 YES
  uuyc-wine --hw-decode    # 确认后再这样启动客户端
  ```

  > **修正记录**：本项目早前把这个现象解释成“Vulkan Video 需要 Gen12+，Gen9.5 不支持”。
  > 那是错的 —— Mesa 源码里 H.264 解码**没有世代门槛**，世代判断只在 AV1 那一行；
  > H.264 也不要求 HuC（只有 H.265 要求）。详见 `VERIFY.md` 5.2u。
  > 因此“控制端在 Wine 下不可用”这一条**不再是定论**，需要按上面的两步实测。
* **会话建立后客户端会崩溃（当前主要障碍）**：进入会话约 4 秒后 `GameViewer.exe`
  在 `Qt5Core.dll` 的 `QString::fromUtf16` 里崩溃，原因是它收到了一个**未初始化的
  字符串指针**。已用 A/B 对照确认**与 `uuyc-wine-vdshim` 无关**（移除 shim 后崩溃地址
  完全相同）。发生在客户端自身 + WebView2 + Qt 这条链上，不属于本项目能修的层。
  日志与寄存器证据见 `VERIFY.md` 5.2u.4i；路径上确认的一个 Wine 缺陷已整理成可上报的
  报告：`docs/wine-bug-propvariant.md`。
* **当被控端**（别的设备来控制本机）：**无画面 + 噪音** —— 采集侧依赖 Windows 内核
  驱动（`GameViewerIddDriver` 虚拟显示器、`VirtualAudio` 虚拟声卡、`ViGEmBus`
  虚拟手柄），Wine 无法加载，因此没有画面源也没有音频源。这是架构限制。
* 完整证据链见 `VERIFY.md` 5.2k ～ 5.2u。
* **备选串流路径用 `uuyc-android`（Waydroid）**，它绕开 Wine 图形栈与客户端 UI，
  是当前最可能可用的方案；`uuyc-wine` 至少可用于登录、设备列表与文件传输。
* **`uuyc-wine` 是“控制端”**。它让你在 Arch 上登录、查看设备列表、发起/接受远程
  会话、传文件。让 **Arch 本身被远程控制** 需要 UU 远程的桌面捕获与虚拟显示驱动
  （`bin/drivers/GameViewerIddDriver`、`ViGEmBus`、`VirtualAudio` 都是 Windows
  内核驱动），Wine 无法加载，本适配不支持该方向。
* **`uuyc-android` 的原生代码只有 arm64-v8a**。x86_64 主机必须启用 ARM 翻译层，
  串流解码性能会明显低于原生 ARM 设备或 Windows 客户端。
* 启动器不遍历 `/proc/*/environ`：检测前缀里是否有 Wine 进程时只检查本用户的
  `wineserver`/`wine*` 进程，因此不会出现 `/proc/<pid>/environ: Permission denied`
  （该报错已在本版修复，见 `VERIFY.md` 5.2b）。
* 两个包都**不含**上游运行时数据：`uuyc-wine` 首次启动时在前缀里安装官方
  MSI/EXE 载荷与 WebView2（需要联网），`uuyc-android` 首次启动时把 APK 安装进
  Waydroid。
* **WebView2 需要能正常 TLS 出网的前缀**。本机实测：Wine 的 `secur32` 在缺少
  `SamSs` 时证书校验失败（`cert verify failed: 12045`），引导器自带的证书自修复
  也随之失败，遂无法自动安装 WebView2。可先 `export https_proxy=...` 后重试
  `uuyc-wine --repair`；临时跳过用 `UUYC_WINE_SKIP_WEBVIEW=1 uuyc-wine --setup-only`。
* Waydroid 需要 `binder_linux`/`ashmem_linux` 内核模块与 Wayland 会话；本机当前
  环境缺少 binder 模块与 `/dev/kvm`，因此安卓侧的运行验证只做到静态与前置条件检查
  （见 `VERIFY.md` 的诚实说明）。

---

## 5. 许可证

* 上游 `网易UU远程` 客户端为网易专有软件，随附其最终用户许可协议，此处按官方安装包
  原样分发以供个人在 Arch Linux 上运行。
* 本仓库新增的打包脚本、启动器与 `wevtapi` 兼容层采用 **0BSD**（见各包内
  `LICENSE`）。`uuyc-wine` 的启动器结构参考了 AUR `uuyc-wine`（同为 0BSD）并针对
  4.40.1 重写，`wevtapi` 兼容层汇编源自同一 0BSD 项目，均在文件头注明。
