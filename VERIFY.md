# 构建与验证记录

本文档记录本次适配的**实际执行结果**，以及每一项“已验证 / 未验证”的边界。
所有命令都在 `/home/daoxiang/ds-workplace` 中执行。

---

## 1. 环境

```text
NAME="Arch Linux"  PRETTY_NAME="Arch Linux"  ID=arch  BUILD_ID=rolling
kernel arch          : x86_64
session              : Wayland (WAYLAND_DISPLAY=wayland-0, DISPLAY=:1)
wine                 : wine 11.17-1（含 wine-mono / wine-gecko 检查）
waydroid             : waydroid 1.6.3-1, Vendor type MAINLINE, Session STOPPED
kernel modules       : binder_linux / ashmem_linux 未加载（lsmod 无输出），/dev/binder* 不存在
/dev/kvm             : 不存在
CJK 字体             : noto-cjk 已安装（fc-list :lang=zh 共 80 项）
makepkg              : 7.1.0，以 uid=1000(daoxiang) 运行
```

## 2. 输入文件识别

```text
uuyc_4.40.1.exe : PE32 executable for MS Windows 5.01 (GUI), Intel i386,
                  Nullsoft Installer self-extracting archive, 5 sections
                  90,790,272 字节
                  sha256 64f918b80a99a570dd43a0ca0c8c4fac5b9b734ad6308e0f5c69bc0be6ce5167
uuyc_4.40.0.apk : Zip archive（Android APK-2 签名，v2 方案）
                  26,492,307 字节
                  sha256 2cc14a4a3fb18066ac0f38ccbc08b17bc05e6d72fb150517269e74316554fef9
```

APK 侧解析结论（`uuyc-android/support/apk-info.py` 实测输出）：

```text
package            : com.netease.uuremote
version            : 4.40.0
native abis        : arm64-v8a          <- 只有 arm64，无 x86/x86_64
native libraries   : 19  (libstreamer.so 17,029,224 / libinputmanager.so / libsqlcipher.so ...)
dex files          : 3
```

Windows 载荷（安装包内，Wine 静默安装后实测）：`GameViewer.exe` 8.4 MB、
`GameViewerService.exe` 8.4 MB、`bin/GameViewerServer.exe` 33 MB（Qt5 + `streamer.dll`）、
`bin/GameViewerLauncher.exe`、`bin/GameViewerHealthd.exe`、`bin/*.dll` 75 个、总计 221 个文件 292 MB。
`bin/GameViewerServer.exe` 的导入表中确实包含 `wevtapi.dll`（这正是需要兼容层的原因）。

## 3. 构建结果

```text
$ ./scripts/build.sh
==> Making package: uuyc-wine 4.40.1-1     ==> Finished making: uuyc-wine 4.40.1-1
==> Making package: uuyc-android 4.40.0-1  ==> Finished making: uuyc-android 4.40.0-1
exit=0

uuyc-wine/uuyc-wine-4.40.1-1-x86_64.pkg.tar.zst       90,724,636 字节  安装后 90,819,698 字节
  sha256 a243a1c4028d50b4b176b59bcb7d2bbef60ab4f24d0166ecbccd0fad917d0d1a
uuyc-android/uuyc-android-4.40.0-1-x86_64.pkg.tar.zst 23,795,972 字节  安装后 26,521,720 字节
  sha256 bb7490888196a5a8964374b7de76a0da916d1db7f0d08d46b9bdbb6b589f204d
```

`uuyc-wine` 包内容：

```text
usr/bin/uuyc-wine
usr/share/applications/uuyc-wine.desktop
usr/share/applications/uuyc.desktop                         (合并入口：默认 Wine，附加“安卓客户端”动作)
usr/share/icons/hicolor/256x256/apps/uuyc-wine.png          (256x256 RGBA PNG，取自安装包 .rsrc/ICON/6)
usr/share/licenses/uuyc-wine/LICENSE
usr/share/uuyc-wine/uuyc-installer.exe                      (官方 4.40.1 安装包，原样)
usr/share/uuyc-wine/wevtapi.dll                             (本仓库用 as/objcopy/ld 编译的兼容层)
usr/share/uuyc-wine/upstream-version                        (4.40.1)
usr/share/uuyc-wine/package-release                         (4.40.1-1)
```

`uuyc-android` 包内容：

```text
usr/bin/uuyc-android            usr/share/uuyc-android/com.netease.uuremote.apk  (官方 4.40.0，原样)
usr/bin/uuyc-android-arm        usr/share/uuyc-android/apk-info.py
usr/bin/uuyc-android-verify     usr/share/uuyc-android/apk-facts.txt
usr/share/applications/uuyc-android.desktop
usr/share/icons/hicolor/256x256/apps/uuyc-android.png
usr/share/licenses/uuyc-android/LICENSE
usr/share/uuyc-android/upstream-version
```

依赖声明（`.PKGINFO`）：

```text
uuyc-wine     : wine>=11.0 hicolor-icon-theme procps-ng util-linux diffutils grep pacman
                optdepend libnotify
                makedepend binutils 7zip wine
uuyc-android  : waydroid python coreutils grep gawk
                optdepend libndk-support waydroid-image
```

## 4. 静态验证（全部通过）

| 检查项 | 命令 | 结果 |
| --- | --- | --- |
| `wevtapi.dll` 架构 | `file` | `PE32+ executable ... (DLL), x86-64` ✅ 与 `GameViewerServer.exe` 同为 x86-64 |
| `wevtapi.dll` 导出符号 | `objdump -p` | `EvtClose / EvtNext / EvtOpenPublisherMetadata / EvtQuery / EvtRender` 5 个全部存在 ✅（PKGBUILD 的 `build()` 内置该校验，缺失即构建失败） |
| 启动器语法 | `bash -n` | `uuyc-wine`、`uuyc-android`、`uuyc-android-arm`、`uuyc-android-verify` 全部 ok ✅ |
| Python 解析器 | `python3 apk-info.py` | 正确输出包名/版本/ABI/dex 数量 ✅ |
| Desktop Entry | `desktop-file-validate` | 三个 `.desktop` 均无告警 ✅ |
| APK 身份校验 | `PKGBUILD prepare()` | 包名必须是 `com.netease.uuremote`、ABI 必须是 `arm64-v8a`，否则构建失败 ✅ |
| 上游文件校验 | `sha256sums` | 两个官方安装包与全部打包脚本均写入真实校验值（不再是 `SKIP`）✅ |
| 启动器 → 包数据一致性 | `PKGBUILD build()` | 校验启动器引用 `/usr/share/uuyc-wine`、`uuyc-installer.exe`、`wevtapi.dll` / `/usr/share/uuyc-android`、APK 文件名 ✅ |
| 启动器自检 | `uuyc-wine --version` / `--help` | 输出 `4.40.1` 与完整帮助 ✅ |
| 容器自检 | `uuyc-android --status` | 输出 host arch / waydroid / session / binder / kvm / image / arm translation / apk path ✅ |

`uuyc-android --status` 在本机的真实输出：

```text
host architecture : x86_64
waydroid installed: yes
session           : stopped
binder module     : missing
kvm available     : no
image configured  : yes
arm translation   : missing
apk on disk       : /usr/share/uuyc-android/com.netease.uuremote.apk
```

## 5. 运行验证

### 5.1 Wine 端到端（已验证，`tests/smoke-prefix.sh` 退出码 0）

回归脚本按包内布局装配 `share/`，从**零**创建前缀并跑完整套装配流程：

```bash
$ bash tests/smoke-prefix.sh /home/daoxiang/ds-workplace/work/final2
=== launcher: version ===
4.40.1
=== launcher: prefix setup (this creates the Wine prefix) ===
=== verifying installed payload ===
  ok      GameViewer.exe
  ok      bin/GameViewer.exe
  ok      bin/GameViewerServer.exe
  ok      bin/GameViewerHealthd.exe
  ok      bin/wevtapi.dll
  ok      bin/MicrosoftEdgeWebview2Setup.exe
=== verifying service registration ===
  ok      GameViewerService is registered
file count: 164
SMOKE EXIT=0
```

装配后前缀 673 MB；`wineserver` 重新拉起后再查询注册表，键值确实**已落盘**
（`system.reg` 中出现 3 处 `GameViewerService`）：

```text
HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\GameViewerService
    DisplayName    REG_SZ            GameViewerService
    ImagePath      REG_EXPAND_SZ     "C:\Program Files\Netease\GameViewer\GameViewerService.exe" --service
    Start          REG_DWORD         0x2      (自启动)
    Type           REG_DWORD         0x10
```

### 5.2 验证过程中发现并修复的三个真实问题

这三处都是本机实测暴露出来的，已回写进启动器：

**(1) 静默安装器偶发“什么都没装”**
第一次端到端跑时安装器 4 分钟后返回、但 `Program Files/Netease` 完全不存在；
同一命令单独重跑 15 秒即成功。启动器现在最多重试 3 轮安装，并以文件是否存在
作为唯一判定依据（注册表 `Version` 只作参考）。

**(2) Wine 的注册表改动不会自动落盘**
Wine 把注册表编辑留在 wineserver 内存里，只有 server 退出时才写入
`system.reg`/`user.reg`。实测：装配过程中 `reg query` 能看到
`GameViewerService`，但 wineserver 被强杀后同一前缀里该键消失。
启动器新增 `sync_prefix_registry()`，在装配结束（以及 `--setup-only`）时执行
`wineserver -k` + `wineserver -w`，确保服务注册、WebView2 兼容设置真正持久化；
上面第 5.1 节的复检即为此项的验证。

**(3) 静默安装器可能不注册服务**
`wine sc create` 回退路径已单独验证可用（本机实测输出）：

```text
$ wine sc create GameViewerService \
    binPath="C:\\Program Files\\Netease\\GameViewer\\GameViewerService.exe --service" \
    start=auto DisplayName="UU Remote Service"
exit=0
    DisplayName    REG_SZ           UU Remote Service
    ImagePath      REG_EXPAND_SZ    C:\Program Files\Netease\GameViewer\GameViewerService.exe --service
    Start          REG_DWORD        0x2
    Type           REG_DWORD        0x10
```

### 5.2b 修掉的第四个问题：`/proc/<pid>/environ: Permission denied`

用户实测报错：

```text
/usr/bin/uuyc-wine: line 194: /proc/507/environ: Permission denied
```

**原因**：`process_in_prefix()` 用 `for environment in /proc/[0-9]*/environ` 遍历**所有**
进程的环境块来找 `WINEPREFIX`。`[[ -r ... ]]` 只是尽力而为的护栏：

* `-r` 的判定与随后 bash 打开重定向的时刻之间进程可能已退出；
* 以 `sudo uuyc-wine` 运行时 uid/euid 不同，`-r` 会放行实际不可读的文件；
* 一旦重定向真的失败，在 `set -e` 下就是致命错误。

**修法**：不再遍历 `/proc`。新的 `wine_procs_in_prefix()` 只检查**本用户的 Wine 进程**：

```bash
pgrep -u "$(id -u)" -x -- wineserver      # 以及 wine*
#   只对本用户进程读 /proc/<pid>/environ，用 [[ -r ]] 前置判断
#   读取失败 -> continue（不是 return），绝不触发 set -e
```

实测确认该信号有效：真实 wineserver 进程的环境块里带着
`WINEPREFIX=/home/daoxiang/ds-workplace/work/final2/prefix`，函数能正确读到它；
其余进程（bwrap/bash/grep 等）读到不匹配即跳过。

**行为验证**（用构建好的包，`UUYC_WINE_SHARE_DIR` 指向包内数据）：

```text
$ uuyc-wine --stop            # 前缀里有活跃 wineserver
UU Remote stopped.
STOP EXIT=0

$ uuyc-wine --stop            # 前缀空闲
No UU Remote wineserver is running for: /tmp/definitely-not-running
EXIT=0
```

两种路径都不再产生任何 `Permission denied` 输出，也不再依赖跨用户可读的 `/proc`。

### 5.2c 会话内「进入桌面就退回」的定位

组件侧证据（本机实测，`objdump -x` 读出）：

```text
bin/streamer.dll       -> d3d11.dll dxgi.dll d2d1.dll dWrite.dll avrt.dll msdmo.dll
bin/nrd_renderer_ipc.dll -> KERNEL32.dll RPCRT4.dll      (NVIDIA 专用渲染助手)
bin/nemua-api.dll      -> WTSAPI32.dll ... 
```

即：会话画面走的是 D3D11 / DXGI / D2D1 渲染 + DirectWrite 文本，这条链路在 Wine 里
依赖 wined3d 拿到可用的 GL/Vulkan 上下文。本机（以及任何无 `/dev/dri` 的环境）只有
llvmpipe 软件渲染、且没有任何 Vulkan ICD：

```text
dri devices     : NONE — no GPU device nodes, only software rendering
vulkan icd      : (空)
OpenGL renderer : llvmpipe (LLVM 22.1.8, 256 bits) / 4.6 Compatibility, Mesa 26.2.2
```

这就是「进入桌面」阶段最可能的断点。为此新增 `uuyc-wine-doctor`（已进包），
一行收集：主机/会话类型、`/dev/dri`、Vulkan ICD、GL 渲染器、前缀状态、应用文件、
WebView2 是否就绪、启动器写入的注册表项、生效的图形环境变量、`*.dmp` 转储、客户端
日志清单与 setup.log 尾部，并给出按可能性排序的试错命令。

`slog` 日志本身是加密格式（magic `SLOG\r\n\x1a\n`，其后为高熵数据，非
zlib/bz2/lzma），无法离线解读 —— 客户端日志只能用于时间线对照，真正的判据是
崩溃形态（无提示消失 vs 弹出错误提示）。

### 5.2d 会话期崩溃的实测证据（来自用户机器）

用户机上采集到的**明文**证据链（Sentry 的 session 记录是 JSON，未加密）：

```text
client Sentry last_crash : 2026-09-17T07:22:59.464Z   (本地 15:22:59)
crash session            : {"status":"crashed","errors":1,
                            "started":"2026-09-17T07:22:55.057Z","duration":4.409,
                            "attrs":{"release":"4.40.1","environment":"gameviewer_prod"}}
crashpad 转储            : b6bb0abb-... .dmp  50,427,928 字节  15:23:00
```

即 **客户端进程在会话开始 4.4 秒后崩溃** —— 与「一点进入桌面就卡退」的现象一致。
崩溃转储里同时加载了 `d3d11.dll` / `dxgi.dll` / `d2d1.dll` / `DWrite.dll` /
`opengl32.dll` / `wined3d` / `EmbeddedBrowserWebView.dll`，未加载 `vulkan-1.dll`。

另一次会话（`no3d` 渲染器，15:18:37 开始）没有立刻崩溃，但 streamer 把日志打成
**4 个 10 MiB 轮转文件（共 40 MiB，5 分钟内写满）**：

```text
streamer_log_controller_20260917151837616_784.1.slog   10,485,714 字节
streamer_log_controller_20260917151837616_784.2.slog   10,485,683 字节
streamer_log_controller_20260917151837616_784.4.slog   10,485,683 字节
streamer_log_controller_20260917151837616_784.5.slog   10,485,683 字节
streamer_log_controller_20260917151837616_784.slog              0 字节（被轮转清零）
```

健康的会话不会把日志写满 40 MiB，这是**每帧刷同一类错误**的死循环特征。

推论（与用户实测吻合）：

| 渲染器 | 现象 | 解释 |
| --- | --- | --- |
| 默认 / `gl` | 4.4 秒后进程崩溃 | 会话启动阶段创建 D3D11/DXGI 渲染上下文时崩 |
| `no3d` | 进程活着，但连不上 | D3D 设备不可用 → 视频管线无法建立 → 客户端放弃会话 |

`uuyc-wine-doctor` 现在会自动输出上面这段判据（`session outcome` 与
`log storm check` 两节），不需要手工翻 JSON。

### 5.2e 根因确认：会话崩溃是「运行环境藏了 GPU」

用户在自己的普通终端里跑 `uuyc-wine-gpu`，输出确认 GPU 完全正常：

```text
1. is this shell inside a sandbox?   ok  no sandbox detected at PID 1
2. DRM device nodes                  ok  /dev/dri exists
                                         crw-rw----+ root video  226,  1  card1
                                         crw-rw-rw- root render 226,128  renderD128
                                     ok  /dev/dri/renderD128 is readable and writable
3. kernel driver on the host         i915 loaded (59 users)
                                     Kernel driver in use: i915   (CometLake-U GT2)
4. VA-API stack                      intel-media-driver 26.2.4-1, libva 2.24.1-1,
                                     iHD_drv_video.so present,
                                     Vulkan ICDs: intel_hasvk_icd.json intel_icd.json
5. what wined3d will report          /dev/dri is present here, so wined3d can use it.
```

而**同一个脚本在 DSH 会话的 shell 里**报告：

```text
1. FAIL  PID 1 is a sandbox wrapper: bwrap ... --dev /dev --unshare-pid ...
2. FAIL  /dev/dri does not exist in this namespace
```

结论（已修正早期判断）：`/dev/dri` 缺失与 `libva error: vaGetDriverNames()
failed with operation failed` 都是 **bwrap 的 `--dev /dev` 只提供精简 `/dev`**
造成的——那次会话是在看不到 GPU 的环境里启动的。`Xlib: extension "DRI2"
missing on display ":1"` 则是 Xwayland 早已只提供 DRI3 所致，本身无害。

Wine 侧的 Vulkan 能力已确认可用：

```text
/usr/lib/wine/x86_64-windows/winevulkan.dll   264,230 字节
/usr/lib/wine/x86_64-unix/winevulkan.so       841,704 字节
wined3d.dll 内 vulkan 相关字符串 97 处
```

因此 `WINE_D3D_CONFIG=renderer=vulkan` 是这台机器上的正确路线（待用户在 GPU 可见
的环境里实测确认）。

**由此新增的启动器行为**：`uuyc-wine` 启动客户端前会检查
`/dev/dri/renderD*` 是否存在且可读写；不可见时打印明确警告（"environment problem,
not a driver problem"，并指向 `uuyc-wine-gpu`），而不是让人白等会话崩溃。另加
`--safe-graphics` 开关（写入 `WINE_D3D_CONFIG=renderer=no3d`），用于在无 GPU 的
环境里查看界面——但会话仍需 GPU。

### 5.2f renderer=vulkan 的实测对比（用户机器）

用户在 GPU 可见的普通终端里执行 `WINE_D3D_CONFIG=renderer=vulkan uuyc-wine`：

```text
终端仍打印：libva error: vaGetDriverNames() failed with operation failed
            Xlib: extension "DRI2" missing on display ":1".
```

但**行为发生了实质变化**：

| 指标 | `renderer=gl`（旧） | `renderer=vulkan`（本次） |
| --- | --- | --- |
| 客户端存活 | 4.4 秒后崩溃 | 36 秒（17:30:24 → 17:30:59） |
| `reports/*.dmp` | 1 个 50 MB | **0 个**（目录为空） |
| streamer 日志 | 40 MiB / 5 分钟（死循环） | 3.8 KB（无风暴） |
| client Sentry | `status=crashed` | `status=ok, errors=0` |
| 组件日志 | — | launcher/Healthd/service/server/client 全部产出 |

即：**不再是崩溃，而是会话没有维持住**（或是用户自行关闭）。这是一次明确的进步，
但离"能串流"还差最后一步，因此保留为未完成项。

已新增 `uuyc-wine-hwdecode`（进包）用于定位最后一环：一次输出 render node、
`vainfo`、`vulkaninfo`、Wine 可见的 `libva`/`libvulkan` 路径，以及客户端上次的
Sentry 记录，并给出判读规则：

* section 2（vainfo）列出 `VAProfileH264…` → 硬解可用，问题在 Wine 内部；
* section 2 仍报 `vaGetDriverNames` → 该进程用不了 render node；
* section 3（vulkaninfo）列出 Intel 设备 → `renderer=vulkan` 走的是真硬件。

> 说明：本沙箱无法访问 GPU（bwrap `--dev /dev`），因此上述对比全部来自用户机的
> 日志与 Sentry 记录，我无法在自己的环境里复现"成功串流"这一状态。

### 5.2g 会话崩溃的收敛结论（用户机多轮实测）

用户连续四轮实测（全部在 GPU 可见的普通终端里）：

| # | 渲染器/配置 | 崩溃会话时长 | 转储 | 画面 |
| --- | --- | --- | --- | --- |
| 1 | 默认（gl） | 4.409 秒 | 50 MB | 无 |
| 2 | `renderer=no3d` | 不崩 | — | 能进界面，但连不上 |
| 3 | `renderer=vulkan` | 约 36 秒后进程消失 | 未留存 | 无 |
| 4 | `renderer=vulkan` | 3.576 秒 | 50 MB | **无** |

第 4 次的转储里模块表包含 `winevulkan.dll`、`wined3d.dll`、`d3d11.dll`、
`dxgi.dll`、`d2d1.dll`、`d3d10core.dll`、`d3d9.dll`、`opengl32.dll`、
`EmbeddedBrowserWebView.dll`、`dbghelp.dll` 等 88 个模块 —— 证明
`renderer=vulkan` **确实生效**，但崩溃依旧，因此问题不在渲染器选择本身。

配合「**始终没有画面**」+ streamer 日志仅 2 934 字节（远小于有 GPU 缺失时的
40 MiB 风暴），可判定崩溃发生在**会话建立后的渲染器/解码器初始化**阶段，而非
视频解码过程中。

仍缺一环：厂商日志 `*.slog` 为加密格式（magic `SLOG\r\n\x1a\n`，其后为高熵
数据，非 zlib/bz2/lzma），无法离线解读；转储的 ExceptionStream 也不是标准
Minidump 布局，取不出异常码与 RIP。为此新增 `uuyc-wine --debug-crash`：以
`WINEDEBUG=+seh,+d3d,+d3d11,+dxgi,+wined3d,+vulkan,+winevulkan` 启动并把输出写入
`<state>/crash-debug.log`，`+seh` 会打印异常与 backtrace —— 这是唯一能拿到真实
崩溃点的途径。

> 本沙箱无法访问 GPU（bwrap `--dev /dev`），也没有 mingw 可用于编译 D3D11
> 探针，因此"Wine 的 D3D11 在该 Intel GPU 上能否创建设备"这一环必须在用户机上
> 用上面的回溯日志确认。

### 5.2h 结论：D3D11 设备被移除，三个渲染器都绕不过

用户用 `--debug-crash` 抓到的日志（6,918,258 字节 / 60,555 行）解析结果：

```text
未处理异常            : 无（进程不是崩溃退出）
Backtrace 块          : 无
err:d3d / err:dxgi    : 无
连续异常展开          : 仅 C++ 异常，handler returned 1（正常处理）
实际后端              : adapter_gl_* × 69    adapter_vk_* × 0
交换链                : CreateSwapChainForHwnd × 2（含 953×1034 会话窗口）
                        d3d11_swapchain_Present1 × 4
失败点                : d3d11_device_GetDeviceRemovedReason × 22（Wine 侧为 stub）
                        wined3d_swapchain_cleanup "Something's still holding back buffer 0" × 2
```

关键判断：

1. **`renderer=vulkan` 未生效** —— 显式设置后日志里 `adapter_vk_*` 仍为 0，全部走
   `adapter_gl_*`；说明 wined3d 的 Vulkan 后端在本机初始化失败后**静默回退到 GL**
   （日志中 `winevulkan.dll` 来自 WebView2 自己探测 Vulkan，并非 wined3d 使用）。
   Wine 的 wined3d 确实编入了 `adapter_vk_*`（strings 可见），因此这是运行时回退，
   不是缺少后端代码。
2. **进程不是崩溃，是被"设备移除"终止** —— 没有任何 GL/Vulkan/GPU 错误、没有未处理
   异常、没有 backtrace。会话窗口（953×1034）已创建、交换链已 Present 了 4 次，随后
   设备进入 removed 状态，客户端按"渲染设备丢失"逻辑退出。这正是"一点进入桌面就退回
   且没有画面"的机制。
3. **因此渲染器不是可调项**：默认(gl)、`renderer=gdi`、`renderer=vulkan` 三者结果一致；
   `no3d` 之所以"能进"，是因为设备创建失败，客户端走了降级分支（因此也无法串流）。

据此对 4.40.1 的适配给出边界结论：**该 Windows 客户端的远程会话渲染路径在
Wine 11.17 下不可用**，`uuyc-wine` 只能覆盖登录、设备列表与文件传输类功能；需要串流
时应使用 `uuyc-android`（Waydroid，已打包）或 Windows/手机端。

`uuyc-wine-doctor` 新增 `session graphics` 一节，自动报告上述指标，便于以后复查
（例如 Wine 升级后是否已修复）。

### 5.2i 根因（动态日志确认）：Wine 的 `CheckFormatSupport` 是 partial stub

对用户 `--debug-crash` 日志（6,918,258 字节）做符号级统计后，拿到了最具体的根因：

```text
warn:d3d11:d3d11_device_GetDeviceRemovedReason ... stub!            x22
fixme:d3d11:d3d11_device_CheckFormatSupport iface ..., format NN    x113  (partial-stub!)
```

客户端在会话期间查询了 **57 种 DXGI 格式 / 113 次**，其中视频解码相关的是：

| DXGI 格式号 | 名称 | 查询次数 |
| --- | --- | --- |
| 87 | NV12（硬件解码标准输出格式） | 8 |
| 88 | 420_OPAQUE | 4 |
| 93 | P208 | 4 |
| 103 | P010（10-bit 硬件解码） | 5 |

Wine 把 `d3d11_device_CheckFormatSupport` 实现为 **partial stub** —— 它无法准确回答
"这个格式是否可被硬件解码/后处理"。客户端据此认为拿到的是不支持的格式组合，随后
设备进入 removed 状态（`GetDeviceRemovedReason` 也是 stub，客户端只能得到
"设备已移除"），于是按"渲染设备丢失"逻辑退出会话 —— **没有画面、没有报错、窗口关闭**。

这条链条解释了此前全部现象，并且说明为什么三个渲染器都一样：

* 默认（gl）/ `gdi` / `vulkan` 都会创建出可用的 D3D11 设备，因此都会走到格式协商；
* `no3d` 下设备创建失败，客户端走降级分支（因此不崩，但也没有解码器，必然连不上）；
* 与 GPU 是否存在无关（用户机上 `/dev/dri/renderD128` 与 i915 都正常）。

**静态侧的旁证**：`streamer.dll` 的 D3D 导入只有 `D3D11CreateDevice`、
`CreateDXGIFactory1`、`DWriteCreateFactory` 三个；但前两个在整个 15 MB 的 `.text` 中
**0 引用**（`.pdata` 里有 38,918 个函数条目），说明调用点被保护/虚拟化，静态定位成本高
—— 动态通道追踪是更有效的路径，本次根因即由此得到。

**结论**：这不是配置问题，而是 Wine 的 D3D11 视频格式协商能力缺口。可选项：

1. `uuyc-android`（Waydroid）跑安卓客户端 —— 不经过 Wine 图形栈，无需改 Wine；
2. `uuyc-wine --safe-graphics` 仅用于登录 / 设备列表 / 文件传输；
3. 给 Wine 打补丁补齐 `CheckFormatSupport`（需自建 wine 包，成本高，未做）。

`uuyc-wine-crashlog` 新增 `3a` 节自动输出上表的格式统计，`uuyc-wine-doctor` 的
`session graphics` 节输出设备移除指标，两者配合可在 Wine 升级后一键复查。

### 5.2i-bis 重要修正：Wine 有 H.264 硬解，但只在 Vulkan 后端启用

对 5.2i / 5.2j 的结论做修正 —— 早前"Wine 完全不支持视频解码"的判断**不成立**。
对 `/usr/lib/wine/x86_64-windows/wined3d.dll` 做符号级比对：

| 后端 | 视频解码器实现 |
| --- | --- |
| `adapter_vk_*` | `wined3d_decoder_vk_create`、`wined3d_decoder_vk_decode_h264`、`wined3d_decoder_vk_is_h264_decode_supported`、`wined3d_decoder_vk_create_image`、`create_h264_params`、`get_vk_h264_level`、`VK_KHR_video_decode_h264`、`VK_KHR_video_decode_queue`、`VK_KHR_video_queue` —— **完整的 H.264 硬解路径** |
| `adapter_gl_*` | 仅 `adapter_gl_create_video_decoder_output_view` / `_destroy_...` —— **没有解码器** |
| `adapter_no3d_*` | 仅 `create/destroy_video_decoder_output_view` —— **没有解码器** |

因此 5.2j 里 NV12/P010 返回 `E_FAIL` 的那次探针运行**测的不是硬件路径**：命令用的是
`--warp`，而 WARP/GL 适配器本身没有解码器，返回 E_FAIL 是必然的，不能代表启用 Vulkan
后的行为。这是本次验证过程中的方法错误，记录下来以免误判。

**可修的方向（有依据、待用户机验证）**：

1. 在**硬件适配器**上跑探针：`uuyc-wine-d3dprobe`（不带 `--warp`），看 NV12/P010 是否
   出现 `DECODER_OUTPUT`；
2. 让 wined3d **真正切到 Vulkan 后端** —— 用户机是 Intel CML-U + `intel_icd.json`，
   `winevulkan` 已确认可用；但此前日志显示 `adapter_vk_*` 出现 0 次，说明请求被静默
   回退到 GL。改用注册表持久设置（环境变量此前没生效）：
   `wine reg add 'HKCU\Software\Wine\Direct3D' /v renderer /t REG_SZ /d vulkan /f`，
   再跑 `uuyc-wine --debug-crash` 并用 `uuyc-wine-crashlog | grep adapter_` 确认后端；
3. 若 `adapter_vk_*` 出现且 NV12 带 `DECODER_OUTPUT`，则本问题**可以通过配置修复**，
   不必改用安卓客户端。

### 5.2j 探针实证：Wine 对视频格式直接返回 E_FAIL（已在本机跑出）

新增 `uuyc-d3dprobe.exe`（mingw-w64 交叉编译，`INITGUID`，PE32+ x86-64）与包装脚本
`uuyc-wine-d3dprobe`。本机（沙箱内 `D3D_DRIVER_TYPE_WARP`）实测输出：

```text
=== D3D_DRIVER_TYPE_WARP (software) ===
  device created, feature level 0xB100
  adapter: NVIDIA GeForce GTX 470  vendor 0x10DE device 0x06CD

  FORMAT           SUPPORT    FLAGS
  NV12             FAILED     0x80004005  E_FAIL        <-- 硬解标准输出格式
  P010             FAILED     0x80004005  E_FAIL        <-- 10-bit 硬解
  P016             FAILED     0x80004005  E_FAIL
  420_OPAQUE       FAILED     0x80004005  E_FAIL
  AYUV             FAILED     0x80004005  E_FAIL
  YUY2             0x02000000  (只有 TYPED_UAV，无 DECODER_OUTPUT/VIDEO_PROCESSOR_*)
  B8G8R8A8_UNORM   0x02E4F3F3  (渲染/采样/RT 全齐)
  R8G8B8A8_UNORM   0x02E4F3F3
  R10G10B10A2      0x02E4F3F3
  R16G16B16A16F    0x02E4F3F3

  summary for this device:
    video decoder output format supported : NO
    video processor output format supported: NO
```

**这就是客户端会话失败的机制**：

* 普通渲染格式（R8G8B8A8 / B8G8R8A8 / R10G10B10A2 / R16G16B16A16F）Wine 回答得**很完整**
  （`0x02E4F3F3`：TEXTURE2D / SHADER_SAMPLE / RENDER_TARGET / BLENDABLE / …）——
  所以客户端的**界面能正常显示**；
* 而视频解码/后处理格式（NV12 / P010 / P016 / 420_OPAQUE / AYUV）Wine **直接返回
  `E_FAIL`**，YUY2 也只报一个无关的 `TYPED_UAV`，`DECODER_OUTPUT` 与
  `VIDEO_PROCESSOR_OUTPUT` 一个都没有 —— 所以**一进会话就拿不到可用的视频管线**，
  设备随即进入 removed，客户端关窗退出。

与用户机日志完全吻合：`d3d11_device_CheckFormatSupport ... partial-stub!` ×113，且
格式号 87/88/93/103（NV12/420_OPAQUE/P208/P010）被反复询问 —— 客户端正是在找这条
不存在的硬解输出路径。

**结论（已由实测闭合）**：Wine 的 D3D11 不支持任何视频解码输出格式，因此该客户端
的远程会话渲染路径在 Wine 下**不可用**，与渲染器选择、GPU 驱动、GPU 硬件均无关。

附带发现：Wine 上报的适配器名为占位符 `NVIDIA GeForce GTX 470`（与真实 Intel
ComCatLake-U 无关），`feature level 0xB100`；`uuyc-d3dprobe` 输出里同时出现了
`E_FAIL` 与完整 flag 表，说明"能建设备、能渲染 UI、但没有任何视频格式"这一组合正是
本问题的特征。

复现命令（用户机，硬件路径才是关键）：

```bash
uuyc-wine-d3dprobe --all
# 完整输出：${XDG_STATE_HOME:-$HOME/.local/state}/uuyc-wine/d3dprobe.txt
```

### 5.2k Wine 的 D3D11 硬解只走 Vulkan Video，而该扩展在本机未被暴露

> **本节结论已被 5.2u 推翻，保留原文以存档。**
> 错在"Vulkan Video 需要 Gen12+"这一句：Mesa 源码里 H.264 解码**没有**世代门槛，
> 它只是被 `ANV_DEBUG=video-decode` 这个调试开关挡住。当时的证据只能证明
> "扩展没被暴露"，我却把它读成了"硬件不支持"。向下读到 5.2u。

> **先纠正一个措辞**：这不是"Gen9.5 不能硬解"。Gen9.5（Kaby/Coffee/Comet Lake）的
> 固定功能解码器**完整支持 H.264 / HEVC(含 10bit) / VP9 / JPEG 硬解** —— 这台机器
> 装 Windows 能正常串流，正是因为 Windows 走 **DXVA2 / D3D11VA** 这条 API。
> 缺的只是 **Vulkan Video**，而**恰好** Wine 的 D3D11 视频解码只实现了
> Vulkan 这一条后端。换句话说：**GPU 有能力，Wine 没有对应的 Linux 后端。**

| 硬解 API | 本机可用性 | Wine 是否实现 |
| --- | --- | --- |
| DXVA2 / D3D11VA（Windows 原生） | 有（Windows 下） | — |
| **VA-API**（Linux，`iHD_drv_video.so` 41 MB 已装） | **有** | ❌ wined3d 没有 VA-API 视频后端 |
| **Vulkan Video**（`VK_KHR_video_decode_*`） | ❌ 本机未暴露（原因见 5.2u） | ✅ 有（`wined3d_decoder_vk_decode_h264`） |

两边正好错开：硬件有的（VA-API）Wine 没用，Wine 用的（Vulkan Video）当时没打开。


把 5.2i-bis 的假设验证到底，结论落在**硬件世代**上，而非 Wine 配置：

**第一步：Vulkan 后端已成功启用（用户机实测）**

```bash
wine reg add 'HKCU\Software\Wine\Direct3D' /v renderer /t REG_SZ /d vulkan /f
uuyc-wine --debug-crash
grep -c adapter_vk  crash-debug.log   -> 2422
grep -c adapter_gl  crash-debug.log   -> 0        # 从 GL 切到 Vulkan 成功
```

**第二步：切到 Vulkan 后会话仍然失败**

```text
GetDeviceRemovedReason : 19
swapchain Present      : 4
CheckFormatSupport     : 116   （其中 format 87 = NV12 被问 8 次）
wined3d_device_get_video_decode_profile_count : 16   <- 客户端在找硬解 profile
```

客户端调 `get_video_decode_profile_count` 说明它走的正是 D3D11 Video 硬解路径，
而它拿到的是 **0 个 profile**。

**第三步：为什么是 0 —— 驱动当时没有暴露 Vulkan Video 扩展**

```text
crash-debug.log 中 wined3d 启用的设备扩展里：
  VK_KHR_video_queue / VK_KHR_video_decode_queue / VK_KHR_video_decode_h264 出现 0 次
  扩展列表中 video 相关条目数：0
```

`/usr/lib/libvulkan_intel.so` 里确实编入了 `VK_KHR_video_decode_h264/h265/av1/vp9`
等符号，但运行时没有暴露。本节当时把原因归给"需要 Gen12+"，**这是错的**：
真正的开关是 `ANV_DEBUG=video-decode`（见 5.2u）。可以确定的是当时的因果链：
扩展未暴露 → wined3d 的 `adapter_vk` 无法启用视频解码 →
`get_video_decode_profile_count` 返回 0 → 客户端放弃会话。

**第四步：不存在可回退的替代路径**

```text
adapter_vk_*video_processor*  : 0
adapter_gl_*video_processor*  : 0
adapter_no3d_*video_processor*: 0
```

wined3d **完全没有视频处理器实现**（三个后端都是 0），因此"用视频处理器后处理"
这条备用路线也不存在。可用的只有：

| 路径 | 状态 |
| --- | --- |
| Vulkan Video 硬解 | 本机未暴露（原因见 5.2u，可修） → 当时判为不可用 |
| VA-API 硬解 | 本机 `iHD_drv_video.so` 可用，但 **wined3d 没有 VA-API 视频后端** |
| 视频处理器 / D3D11 后处理 | wined3d 三个后端均无实现 |
| 软件解码回退 | 由客户端决定；实测它在 0 profile 时仍然放弃会话 |

**本节的结论（已被 5.2u 推翻）**：当时认为这台 CometLake-U 笔记本上 Windows 客户端的
远程会话渲染路径**没有可用组合**。5.2u 表明 Vulkan Video 的缺失是**软件开关**而非硬件
世代问题，所以这个结论不成立 —— 请以 5.2u 为准。

同时记录一条方法修正：5.2j 的探针输出取自 `--warp`（软件光栅化），而 WARP/GL 后端
本就无解码器，那次数据不能用于判断硬件路径；5.2i-bis 已标注。

### 5.2l 两个必须纠正的技术错误 + 一个更硬的发现（实测）

**错误一：DXGI_FORMAT 编号有两套，我之前按现代文档读 Wine 的日志。**

mingw-w64 的 `dxgiformat.h` 仍用 DXGI 1.1 时代的编号（`NV12 = 0x67 = 103`），
而 Wine 用现行编号（`NV12 = 87`）。用编译器直接打印真值确认：

```text
mingw:       NV12=103  P010=104  P016=105  420_OPAQUE=106  AYUV=100  NV11=110
现行(微软):   NV12=87   420_OPAQUE=88  P208=93  P010=100  P016=115
```

**错误二：因此 5.2j 那份探针输出测错了格式。** 探针用 mingw 头编译，
`CheckFormatSupport(DXGI_FORMAT_NV12)` 实际传的是 103（Wine 视为 Y210），
所以"NV12 返回 E_FAIL"这个结论站不住。已把探针与 shim 的格式表改为**硬编码
Wine 的编号**，并在源码里写明原因。

**更硬的发现：让 Wine 自己当判据。**

写了一个硬编码格式号的探针，直接问 Wine 认哪些值：

```text
 值    旧SDK名            现行名          Wine 回答      Wine flags
 28   R8G8B8A8_UNORM     R8G8B8A8_UNORM   S_OK          0x02E4F3F3
 87   B8G8R8A8_UNORM     NV12             S_OK          0x02E4F3F3   <-- 关键
 88   B8G8R8X8_UNORM     420_OPAQUE       S_OK          0x00E4F3F1
 93   (未分配)            P208             S_OK          0x00E4F3F1
100   AYUV               P010             E_FAIL        0x00000000
103   NV12               Y210             E_FAIL        0x00000000
104   P010               Y216             E_FAIL        0x00000000
107   YUY2               IA44             S_OK          0x02000000
115   (未分配)            P016             S_OK          0x008013F1
```

两点结论：

1. Wine 用的是**现行编号**（87=NV12 而非 103）—— 之前按现代文档解读日志方向是对的，
   但**细节错了**（Wine 日志里 87 就是 NV12，103 是另一个号）；
2. **Wine 对 NV12(87) 回答 `S_OK` + 纯纹理/渲染目标掩码（`0x02E4F3F3`），
   `DECODER_OUTPUT` 与 `VIDEO_PROCESSOR_OUTPUT` 一个都没有。** 而
   `CheckFormatSupport` 本身是 `partial-stub!` —— 这个回答像是从通用表里给的，
   而不是真的探测过视频能力。

即：客户端问"NV12 能不能做解码输出"，Wine 回答"NV12 是渲染目标，不能"。

### 5.2m 交付：D3D11 视频解码探针 shim（端到端验证通过）

新增 `uuyc-vdshim.dll`（proxy d3d11，约 600 行 C，mingw 交叉编译）：

* 转发 `D3D11CreateDevice` / `D3D11CreateDeviceAndSwapChain` 给 Wine 内置实现；
* 只替换设备 vtable 的 3 个槽（`QueryInterface`/`AddRef`/`Release`），
  `QueryInterface(IID_ID3D11VideoDevice)` 返回 shim 实现，其余全部透传；
* `ID3D11VideoDevice` 20 槽**逐项与 mingw 头文件核对一致**（含索引 11
  `GetVideoDecoderProfileCount` 返回 `UINT` 这个易错点）；
* profile 列表按硬件能力上报（H264_VLD_NOFGT / _FGT），
  `CheckFormatSupport` 对 NV12/P010/P016/420_OPAQUE/P208 回答
  `TEXTURE2D|SHADER_SAMPLE|DECODER_OUTPUT|VIDEO_PROCESSOR_OUTPUT`；
* **每一次调用连同参数写入 `uuyc-vdshim.log`** —— 这才是它的真正产出。

本机实测（自建测试程序链接 shim 的导入库，证明拦截链路生效）：

```text
D3D11CreateDevice -> 0
QueryInterface(ID3D11VideoDevice) -> 0
GetVideoDecoderProfileCount -> 2
GetVideoDecoderProfile(0) -> {1b81be68-...}  (H264_VLD_NOFGT)
CheckVideoDecoderFormat(NV12) -> 0  supported=1
CheckFormatSupport(Wine NV12=87)  -> 0  flags=0x18000220
CheckFormatSupport(Wine P010=104) -> 0  flags=0x18000220
```

shim 日志：

```text
[video] GetVideoDecoderProfileCount: wine=0 shim=2
[video] GetVideoDecoderProfile(0) -> H264_VLD_NOFGT
[fmt] CheckFormatSupport(NV12=87): wine=0/S_OK flags=0x2e4f3f3 -> shim flags=0x18000220
```

`0x18000220` = `TEXTURE2D | SHADER_SAMPLE | DECODER_OUTPUT | VIDEO_PROCESSOR_OUTPUT`，
即把 Wine 的"渲染目标"回答替换成"解码输出"回答。

**用途**：跑在真实客户端上，观察它拿到 profile 后是否继续走
`CreateVideoDecoder` / `CreateVideoDecoderOutputView` / `SubmitDecoderBuffers`、
用的什么 profile / 格式 / 分辨率 / config —— 这决定"写真正的 VA-API 或软解后端"
（约 1200–2500 行）值不值得投入。默认不改变任何其他行为，可用
`UUYC_VD_SHIM_PROFILES=0` 做对照运行。

### 5.2n shim 的部署教训：override 必须打在真正加载 d3d11 的进程上

第一版部署说明让用户对**顶层** `GameViewer.exe` 设 DllOverrides。实测 shim 完全没被加载、
三处日志都没有生成。用 `WINEDEBUG=+loaddll` 抓到的加载链解释了原因：

```text
0778:trace:loaddll:build_module Loaded L"C:\windows\system32\d3d11.dll" ... builtin
0778:trace:loaddll:build_module Loaded L"C:\windows\system32\dxgi.dll"  ... builtin
（全程没有出现 uuyc-vdshim）
```

用导入表反查，真正的调用链是：

```text
d3d11.dll / dxgi.dll   <- 被 bin/streamer.dll 导入
bin/streamer.dll       <- 被 bin/GameViewer.exe 与 bin/GameViewerServer.exe 导入
顶层 GameViewer.exe    <- 既不导入 d3d11，也不导入 streamer
```

即：**顶层 `GameViewer.exe` 根本不用 D3D**，对它设 override 等于没设 —— Wine 连查找
`uuyc-vdshim.dll` 的动作都不会发生。

修正后的部署（`support/vdshim/deploy.sh install` 已固化）：

```text
installed dll -> .../GameViewer/uuyc-vdshim.dll
installed dll -> .../GameViewer/bin/uuyc-vdshim.dll
override set  -> bin/GameViewer.exe
override set  -> bin/GameViewerServer.exe
```

`deploy.sh status` 会一并报告 DLL 位置、两个 override 状态与三处日志是否存在。

**附带收益的排查方法**：`WINEDEBUG=+loaddll | grep -i vdshim` 是判断"shim 到底有没有被
加载"的最直接手段 —— 比翻日志更快，因为日志缺失有两种原因（没加载 / 写到了别处），
而 loaddll 只有一种解释。

### 5.2o shim 首次抓到真实客户端行为（决定性数据）

修好部署方式后（shim 必须以 **`d3d11.dll`** 为文件名部署，配合 `d3d11=n,b`；
`WINEDLLOVERRIDES='uuyc-vdshim=n,b'` 永远不可能拦截 d3d11 —— override 只对被
override 的那个名字生效），shim 成功进入 `bin/GameViewerServer.exe`，日志显示：

```text
exe=C:\Program Files\Netease\GameViewer\bin\GameViewerServer.exe
[shim] real d3d11 backend: 00006FFFF2800000
==== D3D11CreateDevice driver_type=0 flags=0x120 levels=2 ====   (0 = HARDWARE)
[shim] real D3D11CreateDevice -> 0 S_OK
[video] GetVideoDecoderProfileCount: wine=0 shim=2
[video] GetVideoDecoderProfile(0) -> H264_VLD_NOFGT
[video] GetVideoDecoderProfile(1) -> H264_VLD_FGT
[video] CheckVideoDecoderFormat profile=H264_VLD_NOFGT fmt=Y210(103): shim=TRUE
[video] GetVideoDecoderConfigCount profile=H264_VLD_NOFGT fmt=Y210 -> 0x80004001 E_NOTIMPL count=0
```

三个结论：

1. **客户端确实走 D3D11 硬解协商**（`D3D_DRIVER_TYPE_HARDWARE`，反复枚举 H.264
   profile，未询问 HEVC）。此前"它可能只是拿不到 profile 就退出"的担心不成立 —— 它
   是真的要建解码器。
2. **格式编号确实错开**：客户端用 **103 = NV12**（旧编号），Wine 用 **87 = NV12**
   （现行编号）。5.2j 里"客户端用现代编号"的判断是错的，此处更正。
3. **第二个卡点是 `GetVideoDecoderConfigCount` → `E_NOTIMPL`**（Wine 未实现），
   这一步拿不到解码配置就无法进入 `CreateVideoDecoder`。

另一项旁证：`bin/GameViewer.exe`、`StreamerCodecDetector.exe` 也加载了 shim，但
**没有任何 video 调用** —— 只有 `GameViewerServer.exe` 参与解码协商。

### 5.2p shim 增补：编号翻译 + 自造 H.264 配置（自测通过）

新增三项（约 150 行）：

| 改动 | 作用 |
| --- | --- |
| 格式别名表 | 把客户端编号（103/104/105/106）映射到 Wine 编号（87/100/115/88），并在日志中打印翻译动作 |
| `CheckVideoDecoderFormat` 翻译后转发 | `legacy 103 -> wine 87` |
| `GetVideoDecoderConfigCount` / `GetVideoDecoderConfig` | Wine 返回 `E_NOTIMPL` 时，发布一个按文档构造的 H.264 VLD 配置（`ConfigBitstreamRaw=1`、`DXVA_NoEncrypt`、其余为 0），让调用方能够继续 |

`D3D11_VIDEO_DECODER_CONFIG` 的结构按官方文档核对：100 字节，含
`ConfigMinRenderTargetBuffCount` / `ConfigDecoderSpecific`（mingw 头文件里有，但成员名
容易写错）；`DXVA_NoEncrypt` mingw 未定义，按文档值自行定义。

自测输出：

```text
CheckVideoDecoderFormat(legacy NV12=103) -> 0  supported=1
GetVideoDecoderConfigCount -> 0  count=1
GetVideoDecoderConfig -> 0  BitstreamRaw=1
```

日志：

```text
[video] CheckVideoDecoderFormat profile=H264_VLD_NOFGT fmt=Y210(103) [translated] -> shim=TRUE
        translation: NV12 (legacy 103 -> wine 87)
[video] GetVideoDecoderConfigCount ...: wine=0x80004001(0) -> shim=1
```

**下一步的目标**：让客户端走到 `CreateVideoDecoder` 并记录它传入的
profile / OutputFormat / SampleWidth / SampleHeight / config 位 —— 这决定后续真后端
该用 VA-API（硬件）还是 libavcodec（软件）。

### 5.2q 客户端接受了 profile 与 config，却仍未创建解码器

新版 shim（带编号翻译 + 自造 config）跑真实客户端后，完整协商流程如下：

```text
CheckVideoDecoderFormat profile=H264_VLD_NOFGT fmt=Y210(103) [translated]
        translation: NV12 (legacy 103 -> wine 87)                        -> shim=TRUE
GetVideoDecoderConfigCount 3840x2160: wine=0x80004001 E_NOTIMPL -> shim=1
GetVideoDecoderConfig index=0 ... shim publishes Raw=1 DecSpecific=0      -> S_OK
（同一组调用对 2560x1440 与 1920x1080 各重复一次）
CreateVideoDecoder                                                        -> 0 次
CreateVideoDecoderOutputView                                              -> 0 次
CreateVideoProcessorEnumerator                                            -> 0 次
```

调用统计：`GetVideoDecoderProfile` ×81、`GetVideoDecoderProfileCount` ×45、
`CheckVideoDecoderFormat` ×9、`GetVideoDecoderConfigCount` ×9、
`GetVideoDecoderConfig` ×3，而**所有"创建"类调用均为 0**。

两点解读：

1. 客户端按 **4K → 2K → 1080p** 逐档试探（3840x2160 / 2560x1440 / 1920x1080），
   说明它在为当前被控端分辨率挑选解码档位；
2. 我们给的答案（profile 有、格式支持、config 合法）**全部被接受**，但它仍然没有
   进入创建阶段 —— 所以要么是 config 的具体位被内部否决，要么它的硬件解码**不走
   D3D11 Video**（Media Foundation 是更可能的候选）。

**下一轮实验（`sweep.sh`）**：让 `ConfigBitstreamRaw` / `ConfigDecoderSpecific` /
`ConfigMinRenderTargetBuffCount` / 配置数量可由环境变量控制，逐组合跑一遍并记录
`CreateVideoDecoder` 何时首次出现。若全部组合都不出现，即可判定 shim 这条路线已到
尽头，需要转向 Media Foundation 硬件 MFT 路径。

### 5.2r 纠正 5.2q 的措辞，并把「覆盖漏洞」查清

5.2q 写的"客户端的硬件解码不走 D3D11 Video"**措辞过重**，需要纠正为：**它确实在走
D3D11 Video**（调用了 `GetVideoDecoderProfileCount` / `GetVideoDecoderProfile` /
`CheckVideoDecoderFormat` / `GetVideoDecoderConfigCount` / `GetVideoDecoderConfig`，
全部是 `ID3D11VideoDevice` 的方法），只是**没有走到 `CreateVideoDecoder`**。

用 `pefile` 精确核对导入表与延迟导入表后，d3d11 的消费者只有两个：

```text
bin/streamer.dll             -> d3d11.dll: D3D11CreateDevice   dxgi.dll: CreateDXGIFactory1
bin/StreamerCodecDetector.exe -> d3d11.dll: D3D11CreateDevice   dxgi.dll: CreateDXGIFactory1
其余 exe（含 GameViewerServer.exe / GameViewer.exe）: 不导入 d3d11 / dxgi / MF
```

即客户端**只能**经 `streamer.dll` 使用 D3D11，而它正是调用 `ID3D11VideoDevice` 的那一方。
因此"不走 D3D11 Video"这个说法是错的。

同时确认了一个此前未排除的可能性并把它堵上 —— Wine 的 `d3d11.dll` 导出 40+ 个函数，
其中包含另外两个创建设备的入口，而 shim 只代理了 2 个：

```text
D3D11CreateDevice               <- shim 已代理
D3D11CreateDeviceAndSwapChain   <- shim 已代理
D3D11CoreCreateDevice           <- 未代理（D3D11 的底层创建入口）
D3D11On12CreateDevice           <- 未代理
```

若客户端从这些入口拿设备，得到的将是**未包装**的设备，video 调用会绕过日志 ——
"0 次 `CreateVideoDecoder`"就可能只是覆盖不全。为此给 shim 增加了：

* **设备序号**（`device #1 wrapped`）与**每次调用归属**（`dev1 ...`）；
* **全量 `QueryInterface` 记录**（含 `ID3D11VideoContext` /
  `ID3D11VideoProcessorEnumerator` 等），用于判断客户端到底索取了哪些接口。

下一轮日志即可区分"客户端不用这条路"与"我们的包装不全"。

### 5.2s 关键修正：此前观察到的 D3D11 Video 调用全部来自 `StreamerCodecDetector.exe`

给 shim 加上设备序号与全量 `QueryInterface` 记录后，按日志里的 `exe=` 分段做归属统计：

```text
进程                        设备  QueryInterface  video调用   调用种类
StreamerCodecDetector.exe     3        10           318     GetVideoDecoderProfileCount,
                                                           CheckVideoDecoderFormat,
                                                           GetVideoDecoderConfigCount,
                                                           GetVideoDecoderConfig
GameViewerServer.exe          0         0             0     -
GameViewer.exe                0         0             0     -
```

**结论翻转**：此前所有轮次看到的"枚举 profile → 检查格式 → 读配置 → 不创建解码器"
全部来自 **`StreamerCodecDetector.exe`** —— 一个**编解码器能力探测器**。它只索取
`ID3D11VideoDevice`（外加 `IDXGIDevice`、ID3D10Multithread），**从未索取
`ID3D11VideoContext`** —— 而在 D3D11 里没有 video context 就无法解码。也就是说，
**它的设计本来就不创建解码器**，它是来"探测这台机器有什么硬解能力"并上报的。

因此：

* 5.2q 的"客户端接受 profile/config 却仍不创建解码器"是**在看错进程的基础上得出的**，
  应作废；
* 5.2r 里"它确实在走 D3D11 Video"同样只对探测器成立；
* 真正负责串流解码的进程（`GameViewerServer.exe`）在这几轮中 **d3d11 调用数为 0**，
  即**它的解码路径从未被触发过**。

要观察到 `CreateVideoDecoder`，必须真正跑起来一次会话解码。两条路：

1. **作为被控端**：从另一台设备（手机/Windows）用 UU 远程连到本机，让本机的
   `GameViewerServer.exe` 进入串流会话 —— 这是最直接的方式；
2. **作为控制端**：本机通过客户端连到一台真实在线的 Windows 被控机并进入桌面。

不具备上述条件时，本轮调查的结论应表述为"**尚未观察到解码器创建**"，而不是
"客户端不使用 D3D11 视频解码"。

经验教训：**给日志加进程/设备归属**是这次唯一有效的排查手段 —— 在它之前，我连续
三轮把不同进程的行为混在一起解读，并据此下了两次错误结论。

### 5.2t 判定：shim 只在安装时导致客户端崩溃；被控端路径不受支持

用户实测两条决定性信息：

```text
装 shim 才闪退                    -> 崩溃由 shim 引入，确认为 shim 缺陷
本机作为被控端：无画面 + 很大噪音   -> 会话能建立，但两个方向都不可用
```

#### 1. shim 崩溃（确认为工具缺陷）

排查中发现两个真实缺陷并已修：

* **`DllMain` 调用 CRT**（`getenv` + `fopen`/`fprintf`）：`bin/GameViewer.exe` 是**动态加载**
  `d3d11.dll` 的，动态加载时 CRT 可能尚未初始化，在 `DllMain` 里使用 CRT I/O 属于未定义
  行为。已改为纯 Win32（`CreateFileA` / `WriteFile` / `CloseHandle` /
  `GetEnvironmentVariableA`），并加脚本校验 `DllMain` 内无任何 CRT 调用。
* **全局环境变量 `WINEDLLOVERRIDES='d3d11=n,b'` 作用域过大**：它对该前缀内**每个进程**
  生效，而这些进程会从 app 目录拿到只导出 2 个函数的 shim，而 Wine 真正的 `d3d11.dll`
  导出 40+ 个函数 —— 依赖其它导出的进程会加载失败。已改为**只依赖 per-app 注册表
  override**，并在 `deploy.sh` 输出中明确警告不要导出该变量。

另补上 `D3D11CoreCreateDevice` 转发（Wine 的未文档化导出，用于分层设备创建）。

**结论：shim 的定位是诊断工具，不是产品**。它在"客户端当控制端"这一侧有诊断价值；
在其它进程中被加载时需要更严格的隔离。继续投入的收益已经很低。

#### 2. 被控端（本机被远程控制）不受支持 —— 架构限制，非配置问题

本机作为被控端时"无画面 + 噪音"，根因在 4.x 节已记录过、此处得到实测印证：
UU 远程的采集侧依赖 **Windows 内核驱动**：

```text
bin/drivers/GameViewerIddDriver   虚拟显示器（采集画面来源）
bin/drivers/ViGEmBus              虚拟手柄
bin/drivers/VirtualAudio          虚拟声卡（采集音频来源）
bin/drivers/gvInput, uuycinput    输入注入
```

Wine 无法加载内核驱动，因此：

* **无画面**：没有可采集的显示源（虚拟显示器不存在）；
* **噪音**：接收端解出的是无有效内容的音频流。

这与客户端是不是 Wine、装不装 shim 都无关 —— **在 Linux 上"让本机被 UU 远程控制"
这条路本身不成立**。README 的能力边界一节早已写明，本次实测予以确认。

### 5.2u 修正 5.2k：Vulkan Video 在本机被一个**调试开关**挡住，不是硬件世代问题

用户的质疑是对的。回到 Mesa 源码逐条核对后，5.2k 的核心论断"Vulkan Video decode 需要
Gen12+"**没有依据**。

#### 1. 源码证据（`src/intel/vulkan/anv_physical_device.c`）

```c
const bool video_decode_enabled = ANV_DEBUG(VIDEO_DECODE);      /* <- 唯一总开关 */

*ext = (struct vk_device_extension_table) {
   .KHR_video_queue        = video_decode_enabled || video_encode_enabled,
   .KHR_video_decode_queue = video_decode_enabled,
   .KHR_video_decode_h264  = VIDEO_CODEC_H264DEC && video_decode_enabled,
   .KHR_video_decode_h265  = VIDEO_CODEC_H265DEC && video_decode_enabled && device->has_huc,
   .KHR_video_decode_av1   = device->info.ver >= 12 && VIDEO_CODEC_AV1DEC && video_decode_enabled,
   .KHR_video_decode_vp9   = VIDEO_CODEC_VP9DEC && video_decode_enabled,
```

三点结论：

1. **H.264 这一行没有任何世代判断**，只有 `video_decode_enabled`。世代门槛的确存在，
   但只在 **AV1** 那一行（`ver >= 12`）。我把 AV1 的条件错套到了 H.264 上。
2. **H.264 不需要 HuC**。只有 H.265 带 `&& device->has_huc`。这说明"Gen9.5 没加载 HuC
   所以没有 H.264 硬解"也不成立 —— HuC 只影响 H.265 / VP9。
3. `video_decode_enabled` 就是 `ANV_DEBUG(VIDEO_DECODE)`，而 ANV 的调试开关名字表在
   `src/intel/vulkan/anv_instance.c`：

```c
static const struct debug_control debug_control[] = {
   ...
   { "video-decode",              ANV_DEBUG_VIDEO_DECODE},     /* 连字符，不是下划线 */
   { "video-encode",              ANV_DEBUG_VIDEO_ENCODE},
   { NULL,    0 }
};
```

所以正确的启动方式是 `ANV_DEBUG=video-decode`。写 `VIDEO_DECODE`、`video_decode`
或 `h264dec` 都会被 `parse_debug_string()` **静默忽略**，表现就是"和没设一样"。

这也解释了一直以来的现象：`libvulkan_intel.so` 里静态编入了
`VK_KHR_video_decode_h264` 等全部扩展名（`strings` 可见），运行时却一个都不暴露 ——
不是被世代判断砍掉，而是被这个默认关闭的开关关掉了。

#### 2. 为什么 AMD 上不觉得有问题

RADV 无条件暴露 Vulkan Video。ANV 把它做成 opt-in 调试开关，是因为解码路径在部分世代上
仍有正确性缺口，Mesa 不愿默认承诺。这不是"Intel 不支持"，而是"默认不开启"。

#### 3. 交付：`uuyc-wine-vkvideo` + `uuyc-wine --hw-decode`

新增两个可直接使用的东西：

```bash
uuyc-wine-vkvideo          # 分别在有/无 ANV_DEBUG=video-decode 下探测扩展
```

它调用一个不依赖 `vulkan-headers` 的原生探针（`dlopen("libvulkan.so.1")` + `dlsym`），
逐个打印 `VK_KHR_video_queue`、`VK_KHR_video_decode_h264` 等的 YES/no 并给出判定；
同时检查 `i915.enable_guc`（只影响 H.265/VP9）并说明与 H.264 无关。

##### 3b. 探针自身修掉的两个缺陷（首次实测就踩到）

用户机器上第一次运行的结果是 `*** stack smashing detected ***: terminated`，
而工具**把崩溃当成了"扩展没暴露"**，给出错误结论。两个缺陷都属本项目实现问题：

* **`VkPhysicalDeviceProperties` 定义被截短（真正的崩溃原因）**。为了不依赖
  `vulkan-headers`，探针手写了结构体；第一版只写到 `pipelineCacheUUID` + `_pad[256]`，
  而真实结构体是 **824 字节**（`limits` 504 + `sparseProperties` 20 都在后面）。
  ICD 按完整长度写入 → 写穿栈缓冲区 → `-fstack-protector` 报 stack smashing。
  修法：把 `VkPhysicalDeviceLimits`（108 个字段）与 `VkPhysicalDeviceSparseProperties`
  补全，把 `deviceName` 正确放到偏移 24，并加编译期断言：

  ```c
  VK_ASSERT_SIZEOF(VkPhysicalDeviceSparseProperties, 20);
  VK_ASSERT_SIZEOF(VkPhysicalDeviceProperties, 824);
  VK_ASSERT_SIZEOF(VkExtensionProperties, 260);
  ```

  另加运行期 canary：`getProps()` 后校验缓冲区尾部的哨兵值，一旦被改写就打印
  `ABI CHECK FAILED` 并退出，不再让栈破坏以 `stack smashing` 的形式暴露。
  断言在开发期确实抓到错误（`VkApplicationInfo` 被我写成 64，实际是 48）。
* **判定逻辑把"探针崩溃"读成"扩展不存在"**。现在包装脚本记录探针退出码，
  遇到 `>=128`（信号）、ABI 失败或输出里出现 `stack smashing`/`core dumped` 时，
  直接输出 `inconclusive` 并明确写"这是探针缺陷，不能读成 GPU 不支持"。

修好后的验证：把 `VK_ICD_FILENAMES` 指向机器上现成的 SwiftShader 软件 ICD
（`/usr/share/code/vk_swiftshader_icd.json`，VS Code 自带），探针能正常枚举到设备：

```text
dev[0] SwiftShader Device (Subzero)  api=1.3.0  type=4
  device extension count = 84
  VK_KHR_video_queue           no
```

`deviceName` 与 `apiVersion` 都读对了，说明结构体布局与真实 ABI 一致 —— 这是这次
修复的直接证据。注意 SwiftShader 本身没有 Vulkan Video，所以上表全 `no` 是正确的
（且此时判定为"确实没暴露"而非"inconclusive"，正是期望行为）。

还有一个打包缺陷一并修掉：包目录根部曾残留一份**未修复的** `uuyc-vkvideo.c` 旧副本，
遮蔽了 `support/vkvideo/` 下的新版本，导致前两次重建其实仍在编旧代码。已删除该副本，
并在重建后用 `grep -c VkPhysicalDeviceLimits` / `grep -c '_pad\[256\]'` 校验包内源码
确实是修复版（3 / 0）。

```bash
uuyc-wine --hw-decode      # 启动客户端时导出 ANV_DEBUG=video-decode
uuyc-wine --sw-decode      # 反向：从 ANV_DEBUG 里摘掉 video-decode
```

`--hw-decode` 采用**可选加入**而不是默认打开：H.264 解码路径在 ANV 上并非对所有流都
稳定，且开启后会改变驱动对**整个前缀内所有进程**的扩展列表。合并逻辑已做单元测试：

```text
--hw=1    ANV_DEBUG=<unset>                -> video-decode
--hw=1    ANV_DEBUG=noslab                 -> noslab,video-decode
--hw=1    ANV_DEBUG=video-decode           -> video-decode            （不重复追加）
--hw=1    ANV_DEBUG=no-slab,video-decode   -> no-slab,video-decode
--hw=0    ANV_DEBUG=<unset>                -> <unset>
--hw=0    ANV_DEBUG=video-decode           -> <unset>
--hw=0    ANV_DEBUG=no-slab,video-decode   -> no-slab                 （只摘自己那一个）
--hw=0    ANV_DEBUG=no-slab                -> no-slab
--hw=<无> ANV_DEBUG=video-decode           -> video-decode            （不干预）
--hw=<无> ANV_DEBUG=<unset>                -> <unset>
```

#### 4. 本节的验证边界（据实说明）

* Mesa 源码与调试开关名：**已核对上游源码**，确定。
* 本机是否真能开出 H.264 硬解：**尚未实测成功**。构建沙箱内 `/dev/dri` 不可见，
  `vkEnumeratePhysicalDevices` 返回 `-3`（`VK_ERROR_INITIALIZATION_FAILED`），
  连设备都枚举不到，因此无法在此判定。判定必须由用户在正常会话里跑
  `uuyc-wine-vkvideo` 得出。
* 因此准确表述是：**5.2k 的"世代不够"结论已被推翻；能否硬解取决于这个开关，
  需要一次实测确认。**

#### 4b. 用户机实测结果（决定性，推翻 5.2k）

2026-09-18 在用户机器上运行 `uuyc-wine-vkvideo`，两次探测结果对比：

```text
                        无 flag      有 ANV_DEBUG=video-decode
设备                     Intel(R) UHD Graphics (CML GT2)  api=1.4.354  type=1
设备扩展数               194          201                      （+7，正好是视频那一组）
VK_KHR_video_queue       no           YES
VK_KHR_video_decode_queue no          YES
VK_KHR_video_decode_h264 no           YES      <-- 关键
VK_KHR_video_decode_h265 no           YES
VK_KHR_video_decode_vp9  no           YES
VK_KHR_video_decode_av1  no           no       （唯一真正有世代门槛的，本机 Gen9.5 无）
VK_KHR_video_maintenance1 no          YES
```

**结论：CometLake-U (Gen9.5) 完全支持 Vulkan Video H.264 硬解，只是 ANV 默认不给。**
`AV1 no` 与源码里 `.KHR_video_decode_av1 = device->info.ver >= 12 && ...` 完全吻合 ——
这条对照进一步证明世代判断确实只作用于 AV1，而不作用于 H.264。

同时说明一个问题：**`VK_KHR_video_decode_h265` 被暴露了，但它在本机不可用**。
源码里 H.265 还要 `&& device->has_huc`，而本机 `i915.enable_guc` 未设置、HuC 未加载。
"扩展被暴露" ≠ "编解码器可用"。本机要硬解就用 H.264。

#### 4c. wined3d 侧的第二道门（源码确认）

扩展暴露只是必要条件。`dlls/wined3d/decoder.c` 里
`wined3d_decoder_vk_is_h264_decode_supported()` 是真正决定
`ID3D11VideoDevice::GetVideoDecoderProfileCount` 返回 0 还是 1 的地方：

```c
if (!vk_info->supported[WINED3D_VK_KHR_VIDEO_DECODE_H264])
    return false;                                        /* 门 1：扩展 */
fill_vk_profile_info(&profile, &DXVA_ModeH264_VLD_NoFGT, WINED3DFMT_NV12_PLANAR);
caps.pNext = &decode_caps; decode_caps.pNext = &h264_caps;
if ((vr = VK_CALL(vkGetPhysicalDeviceVideoCapabilitiesKHR(
        adapter_vk->physical_device, &profile, &caps))) != VK_SUCCESS)
    return false;                                        /* 门 2：能力查询 */
return true;
```

返回 true 时 `wined3d_decoder_vk_get_profiles()` 才写入 `DXVA_ModeH264_VLD_NoFGT`。
profile 参数是硬编码的：**H.264 High profile / progressive / NV12** —— 也就是探针现在
复现的那一组。

探针本次一并加了这道检查（`H.264 High/Progressive/NV12 query` 行）。踩到的坑：
**在未暴露视频扩展的设备上调用 `vkGetPhysicalDeviceVideoCapabilitiesKHR`，loader 会直接
abort 进程**（`ICD associated with VkPhysicalDevice does not support
GetPhysicalDeviceVideoCapabilitiesKHR`，退出码 134）。所以探针必须像 wined3d 一样先查
扩展再决定是否发起查询，否则又是一次"把崩溃当成结论"。

#### 4d. 第二次崩溃的真正原因：**是我写错的 sType**，与 ANV 无关

用户第二次运行后 section 2 变成 SIGSEGV(139)，section 1 正常。当时我从 core dump 看到
调用栈落在 `libvulkan_intel.so` 内，就写下"这是 ANV/Mesa 缺陷"——**那个结论是错的**。
修正常量后同一台机器、同一版 Mesa、同一个调用立刻返回 `SUPPORTED`，没有任何崩溃。
把 ANV 当成替罪羊是不成立的，下面记录真正的机制。

**真正的机制（`src/intel/vulkan/anv_video.c:328-337`）：**

```c
struct VkVideoDecodeH264CapabilitiesKHR *ext = (struct VkVideoDecodeH264CapabilitiesKHR *)
   vk_find_struct(pCapabilities->pNext, VIDEO_DECODE_H264_CAPABILITIES_KHR);

ext->fieldOffsetGranularity.x = 0;      /* 第 337 行：ext 为 NULL 时写 NULL+0x14 */
ext->fieldOffsetGranularity.y = 0;
ext->maxLevelIdc = STD_VIDEO_H264_LEVEL_IDC_5_1;
```

ANV **按 `sType` 在 `pCapabilities->pNext` 链里查找**子结构。我当时的
`h264_caps.sType` 写成了 `1000023005`，而正确值是 `1000040000`，于是 `vk_find_struct()`
返回 `NULL`，第 337 行直接写 `NULL + offsetof(fieldOffsetGranularity)`。

这与 core dump 的观测完全吻合：

```text
Signal: 11 (SEGV)  si_code: SEGV_MAPERR      <- 访问未映射的小地址
#0  libvulkan_intel.so + 0xb9d2              <- ext->fieldOffsetGranularity.x = 0
#2  main (uuyc-vkvideo + 0x1825)             <- 探针发起的调用
```

我此前把反汇编里的 `mov 0x10,%eax; ud2` 读成"编译器生成的 assert 陷阱序列"，据此推断
"ANV 主动断言失败"。那是误读：它只是空指针解引用落在了一个恰好形如陷阱的指令上。

**教训（已固化到工具里）**：`uuyc-wine-vkvideo` 在遇到"扩展已暴露但查询阶段崩溃"时，
现在输出的是"这是探针缺陷，先跑 `check-abi.sh`，不要报成 Mesa/ANV bug"，
而不再输出那份指控 ANV 的文字。

**四处手写常量的错误（全部按官方 `vulkan_core.h` 更正）：**

| 常量 | 原来（错） | 官方值 |
| --- | --- | --- |
| `VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR` | 1000023004 | **1000040003** |
| `VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR` | 1000023005 | **1000040000** |
| `VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR` | 1000023002 | **1000024001** |
| `VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR` | 0x2 | **0x1** |
| `VkVideoDecodeCapabilitiesKHR.flags` 偏移 | 8 | **16** |
| `VkVideoCapabilitiesKHR` | 自造 512 字节占位 | 真实 **336** 字节 |

加上首次崩溃的 `VkPhysicalDeviceProperties` 少写 568 字节：**两次崩溃同一根因 —— 手写 ABI**。

**防复发手段：**

* `support/vkvideo/check-abi.sh` 用官方头文件逐条比对探针里的每个常量、尺寸断言与硬编码
  偏移。当前结果：`OK -- 11 constants and 9 sizes/offsets match the official headers`。
  反向验证过：注入上表旧值即报 MISMATCH 并退出 1。
* `work/mockicd/` 下的最小 mock ICD 在能力查询里逐字段校验探针发来的结构，
  确认 `stdProfileIdc@16`、`flags@16`、`pNext` 链形状与官方偏移一致（非交付物）。
* PKGBUILD 的 `build()` 里断言暂存源含正确常量值，防止旧副本被 makepkg 的按名解析命中。

#### 4e. 结果：这道门现在是**通的**（用户机实测）

纠正常量后，同一台 CometLake-U / Mesa 26.2.2 上的输出：

```text
                        无 flag      有 ANV_DEBUG=video-decode
设备扩展数               194          201
VK_KHR_video_queue       no           YES
VK_KHR_video_decode_h264 no           YES
H.264 High/Progressive/NV12 query
                         SKIPPED      SUPPORTED  (vkGetPhysicalDeviceVideoCapabilitiesKHR -> 0)
```

即 `wined3d_decoder_vk_is_h264_decode_supported()` 的两个判断**都成立**，
`GetVideoDecoderProfileCount` 应返回 1（`DXVA_ModeH264_VLD_NoFGT`）而不是 0。
下一步是客户端实测，见 5.2v。

#### 4f. 客户端实测：完整因果链已定位（2026-09-18，用户机 `--hw-decode --debug-crash`）

这一次日志把整条链走通了，卡点终于定位到**具体两个 Wine 未实现函数**。

**过程归属**（按 `wined3d_dll_init Application name` 归因）：

```text
视频调用来源： StreamerCodecDetector.exe  18 次
              msedgewebview2.exe         2 次
GameViewer.exe / GameViewerServer.exe     0 次
```

**完整链路：**

| 环节 | 结果 | 证据 |
| --- | --- | --- |
| ANV 暴露扩展 | ✅ | `vulkan:init_physical_device - VK_KHR_video_decode_h264` |
| wined3d 调驱动查能力 | ✅ | `vulkan:thunk64_vkGetPhysicalDeviceVideoCapabilitiesKHR` |
| 拿到硬解 profile | ✅ | `GetVideoDecoderProfile(..., idx 0)` → `{1b81be68-a0c7-11d3-b984-00c04f2e73c5}` = `DXVA_ModeH264_VLD_NoFGT` |
| Vulkan 解码入口可用 | ✅ | `wine_vk_get_device_proc_addr Found name="vkCmdDecodeVideoKHR"` |
| **CheckVideoDecoderFormat** | ❌ | `fixme:d3d11:..._CheckVideoDecoderFormat ..., format 0x67, stub!` |
| **GetVideoDecoderConfigCount** | ❌ | `fixme:d3d11:..._GetVideoDecoderConfigCount ..., stub!` |
| 后果 | — | `d3d11_device_GetDeviceRemovedReason ... stub!` → 会话窗口关闭 |

`format 0x67` = 103 = **legacy NV12**。客户端拿到 profile 后问"它支持 NV12 吗"，Wine 的
stub 回失败，于是放弃并报设备 removed。

**关键澄清**：`GetVideoDecoderProfileCount` 此前返回 0，现在返回 1。也就是说
5.2u 的 ANV_DEBUG 修复**真的起作用了**，只是失败点后移到了这两个 stub。
`init_vulkan_format_info Unsupported format WINED3DFMT_NV12` 那条 WARN 是**红鲱鱼**：
解码路径用的是 `WINED3DFMT_NV12_PLANAR`（`decoder.c` 里硬编码），
Vulkan 表里有映射（`VK_FORMAT_G8_B8R8_2PLANE_420_UNORM`）。

**因此剩余工作全在 Wine 侧，且已有现成载体**：`support/vdshim/` 的 D3D11 代理 shim
正好实现了这两个函数。

##### 4f-1. "装 shim 就闪退"的真正原因（已修）

排查中发现 shim 的导出表**严重不完整**：

```text
Wine 的 d3d11.dll 导出 : 45 个
shim 的导出            :  3 个（D3D11CreateDevice / ...AndSwapChain / D3D11CoreCreateDevice）
缺失                   : 42 个（D3DKMT* 全家桶、OpenAdapter10、D3D11On12CreateDevice 等）
```

shim 被安装**成 `d3d11.dll`**，因此任何导入缺失符号的 PE 会**直接加载失败** ——
表现就是整个程序瞬间消失。这不是配置问题，是导出表缺项：
`bin/streamer.dll`、`Qt5Gui.dll`、`qdirect2d.dll`、`OpenConsole.exe`、`vcamp140.dll`
都导入 d3d11，其中任何一个用到缺失符号就会炸。

修法：`gen-forwarders.sh` 从宿主 `d3d11.dll` 的导出表自动生成跳转桩。
每个桩是一条 `jmp *slot_NAME(%rip)` 尾调用（完整保留寄存器与栈，被调方看到原始参数），
槽位在 `DllMain` 里用 `real_proc()` 填好，解析不到时指向返回 `E_NOTIMPL` 的兜底函数，
避免跳空指针。验证：

```text
Wine d3d11.dll: 45 exports
shim          : 45 exports
missing: NONE
D3DKMTPresent:
    jmp *uuyc_slot_D3DKMTPresent(%rip)      <- 尾调用，正确
```

PKGBUILD 的 `build()` 里加了断言：导出表页不全就构建失败（当前输出
`vdshim export table: complete (47 symbols)`）。

##### 4f-2. 部署目标修错（已修）

`deploy.sh` 原本 override 的是 `GameViewer.exe` / `GameViewerServer.exe`。
但实测这两个进程**一次视频调用都没发**，真正做能力探测的是
`StreamerCodecDetector.exe`。旧列表意味着 shim 从来没加载到需要它的进程里。
现在三个都覆盖，且清理顺序改为"先删陈旧键、再写新键" —— 旧顺序里
`stale_keys` 含裸名 `GameViewer.exe`，而 `registry_del` 会删掉整个 AppDefaults 键，
把刚写好的 override 又抹掉了。

同时删掉了 `export WINEDLLOVERRIDES=...` 这句建议：全局 override 对该前缀内**每个进程**
生效，正是它让无关进程加载不完整 shim 而崩溃。现在只有 per-app 注册表 override。

#### 4g. shim 实测：探测端全链路打通（2026-09-18，用户机）

装上修好的 shim 后重跑，`bin/uuyc-vdshim.log` 给出了决定性证据。

**先纠正我上一轮的一个误读**：日志里仍有
`fixme:d3d11:d3d11_video_device_CheckVideoDecoderFormat ... stub!`，我据此判断"shim 没加载"。
**错了** —— 那条 fixme 正是 shim **转发给 Wine** 时由 Wine 打出来的。shim 生效的铁证是
格式编号：上一轮 Wine 看到 `format 0x67`(=103)，这一轮是 `format 0x57`(=87)，
说明 shim 做了 `103 -> 87` 翻译。shim 自己的日志也确认了：

```text
==== D3D11CreateDevice driver_type=0 flags=0x120 levels=2 sdk=7 ====
[video] dev1 GetVideoDecoderProfileCount: wine=1 shim=2
[video] GetVideoDecoderProfile(0) -> H264_VLD_NOFGT
[video] dev1 CheckVideoDecoderFormat profile=H264_VLD_NOFGT fmt=Y210(103) [translated] -> wine_impl=0 shim=TRUE
        translation: NV12 (legacy 103 -> wine 87)
[video] dev1 GetVideoDecoderConfigCount profile=H264_VLD_NOFGT fmt=Y210 3840x2160: wine=0x80004001(0) -> shim=1
[video] dev1 GetVideoDecoderConfig index=0 ... shim publishes Raw=1 ... RtBuffers=1
```

`flags=0x120` 含 `D3D11_CREATE_DEVICE_VIDEO_SUPPORT`(0x100)。三种分辨率
（3840x2160 / 2560x1440 / 1920x1080）全部走通。
**客户端从此拿到完整答复：profile 有、格式支持、配置存在。**

**同时确认失败模式已消失**：整份 `crash-debug.log` 里
`887a0005`/`DEVICE_REMOVED` 出现 **0 次**（上一轮会话就是在设备 removed 处结束的）。

##### 4g-1. 新发现并修掉的缺陷：shim 多报了一个 Wine 建不出来的 profile

日志里 `wine=1 shim=2` 说明 shim 在 Wine 真实列表（NoFGT）之外**又加了一个
`H264_VLD_FGT`**。但 `CreateVideoDecoder` 是转发给 Wine 的，Wine 只会照自己的列表建解码器
—— 客户端若挑了 index 1，就会在一次"看起来受支持"的选择上失败。

改为**忠实透传**：Wine 报什么就返回什么，只有 Wine 报 0 时才退回合成列表。
现在日志会是 `wine=1 -> 1 (passed through)`。

##### 4g-2. `uuyc-wine-crashlog` 3b 节的判读修正

原判读把"问了 config"也算成功，措辞自相矛盾（`decoder creations = 0` 时仍打 ok）。
现在分三档：`CREATED a decoder`（成功）/ `asked for configs but never created`
（卡在配置步）/ `never configured`（profile 列表为空）。同时 profile 计数正则同时匹配
`GetVideoDecoderProfileCount` 与 `get_video_decode_profile_count` 两种拼写
（旧正则只匹配后者，在只有 d3d11 层的日志里会漏报为 0）。

#### 4h. 第三次实测：崩溃根因是我 shim 伪造了 ID3D11Device（已修）

用户反馈"还是无法连上"。这次改查客户端**自己的**日志与 Sentry dump，而不是 WINEDEBUG。
结果：`streamer_log_controller_*.slog` 是 **0 字节**（streamer 从未启动），
`Sentry/reports/` 里有两个 44MB dump —— **客户端在会话建立阶段崩了**。

**解析 minidump**（自己写的小解析器读 MDMP 头 + ExceptionStream + ModuleListStream）：

```text
EXCEPTION: code=0xc0000005  ACCESS_VIOLATION
崩溃地址 0x6ffff5495802
模块      C:\windows\system32\d3d11.dll  base=0x6ffff5480000  -> RVA 0x15802
同时加载  bin\d3d11.dll (shim) base=0x6ffff6600000
          bin\streamer.dll     base=0x6ffff8780000
主程序    bin\GameViewer.exe
```

**反汇编 RVA 0x15802**，落在函数 `d3d11_device_GetFeatureLevel`（从 `.rdata`
引用的字符串读出函数名与文件位置）：

```asm
1800157b4:  mov %rcx,%r10            ; r10 = this
1800157b7:  testb $0x8,[channel]     ; TRACE 是否开启
1800157c0:  mov 0x38(%r10),%rax      ; rax = this->field_0x38   <- 快路径同样会崩
1800157c4:  mov 0x20(%rax),%eax      ; [rax+0x20]
```

即 `device->某指针->某字段`，而 `this+0x38` 处是 **NULL** → 读 `[NULL+0x20]` 崩。
**说明调用方传进来的 `this` 不是 Wine 的 `struct d3d11_device`。**

##### 4h-1. 根因：shim 把 0x18 字节的假对象当成 ID3D11Device 返回

旧实现：

```c
typedef struct device_proxy {   /* 只有 0x18 字节 */
    ID3D11DeviceVtbl *vtbl;     /* +0x00 */
    ID3D11Device *real;         /* +0x08 */
    LONG ref; LONG serial;      /* +0x10 */
} device_proxy;
...
return (ID3D11Device *)dp;      /* 把这个假对象交给应用 */
```

只补丁了 `CheckFormatSupport` 一个槽位，其余方法仍指向 Wine 的真实函数 —— 而 Wine 的方法
开头都是 `impl_from_ID3D11Device(iface)`，把 `this` 当作 `struct d3d11_device` 按固定偏移读字段。
我们的结构体只有 0x18 字节，读 `+0x38` 就跑出界外拿到 NULL，下一次解引用即崩。
`GetFeatureLevel` 是应用拿到 device 后最先调的，所以必崩 —— **这是我的设计错误，
不是 Wine 的 bug，也不是驱动问题。**

##### 4h-2. 改法：就地交换 vtable，绝不伪造对象

新实现 `device_hook_install()`：

* **返回 Wine 自己的 device 指针**（`real`），因此调用方看到的 `this` 永远是 Wine 的对象；
* 分配一份 vtable **副本**，只改写 `QueryInterface` 与 `CheckFormatSupport` 两个槽位，
  然后 `real->lpVtbl = patched` 就地装上（device 由 Wine 堆分配，可写）；
* 被打补丁的方法收到的 `this` 就是 Wine 的 device，于是能通过保存的 `original` 直接调用
  原实现（走 `iface->lpVtbl` 会递归回自己）；
* `AddRef`/`Release` **不再拦截** —— Wine 自己的引用计数本来就是对的；
* `ID3D11VideoDevice` 仍是自建包装，但**每个方法都用 `vd->real` 作 `this`** 转发给 Wine，
  代码里已用严格正则复核过没有把 `vd` 本身传出去。

##### 4h-3. 回归测试（`testshim.c` 自带）

把崩溃点做成自测项，不再留给应用去踩：

```c
{ D3D_FEATURE_LEVEL fl = ID3D11Device_GetFeatureLevel(dev); ... }
```

在真实前缀里实测通过：

```text
D3D11CreateDevice -> 0  device=... (feature level 0xb000)
QueryInterface(ID3D11VideoDevice) -> 0  vd=...
GetFeatureLevel -> 0xb000   <- 修复前必崩的那一行
GetImmediateContext -> ...
GetVideoDecoderProfileCount -> 2
CheckVideoDecoderFormat(NV12) -> 0  supported=1
CheckFormatSupport(Wine NV12=87) -> 0  flags=0x18000220
```

##### 4h-4. 教训

诊断 shim 可以拦 API，**但不能替代实现 COM 对象**：只要有一个方法没被拦，
Wine 就会拿错误布局的 `this` 去按固定偏移读字段。正确做法永远是在**原对象上换 vtable**。
`sentry dump + 反汇编` 这条路径比 WINEDEBUG 有效得多 —— WINEDEBUG 里
section 1 是空的（"没有未处理异常"），因为这个崩溃被客户端自己的 crash handler 吃掉了。

#### 4i. 第四次实测：A/B 证明 Qt5Core 崩溃与 shim 无关（Wine 侧发现一个真实 bug）

**A/B 对照（关键）**：先 `uuyc-wine-vdshim remove` 移除 shim，再连一次，结果：

```text
有 shim : Qt5Core.dll + 0xbf11b   ACCESS_VIOLATION  read at 0xffffffffffffffff
无 shim : Qt5Core.dll + 0xbf11b   ACCESS_VIOLATION  read at 0xffffffffffffffff   <- 完全相同
```

**结论：这个崩溃不是 shim 引入的**，是客户端在 Wine 下的自身问题，方向必须换。

**崩溃函数已用导出表精确定位**（Qt5Core.dll 有 8192 个导出）：

```text
RVA 0xbf11b -> 最近的导出是 RVA 0xbf0f0:
  ?fromUtf16@QString@@SA?AV1@PEBGH@Z   =  QString::fromUtf16(const ushort *, int)
崩在 +0x2b
```

反汇编与该实现完全吻合，`test %rdx,%rdx; jne` 就是 Qt 的 NULL 判断；传进来的指针不是 NULL
而是垃圾值，所以绕过了检查直接解引用。寄存器佐证：

```text
rdx = 0x373795e55bf0c513   <- 字符串指针：未初始化
rbp = 0x1d410060           <- 也不是栈地址
```

**时序**（客户端自己的日志 + dump 时间）：

```text
02:26:48  connection_log_controller_..._816.slog   发起会话
02:26:48  streamer_log_controller_..._816.slog     2667 字节，streamer 已启动
02:26:52  Sentry dump                 GameViewer.exe 崩在 Qt5Core
```

即**进入会话约 4 秒后**崩溃。崩溃前客户端在做的事（日志尾部）：

```text
fixme:win:create_window_handle DPI context 0x22 not implemented     x4
fixme:kernelbase:PerfCreateInstance ... WebView2 Utility: Network Service
fixme:kernelbase:PerfCreateInstance ... WebView2 Utility: Storage Service
```

也就是**又创建了一批 WebView2 窗口**。

##### 4i-1. 路径上确认存在的一个 Wine bug（可直接上报）

崩溃前客户端查询过窗口的 `PKEY_AppUserModel_ID`，而 Wine 的实现**不初始化出参**：

```c
/* dlls/shell32/shell32_main.c */
static HRESULT WINAPI window_prop_store_GetValue(IPropertyStore *iface,
        const PROPERTYKEY *key, PROPVARIANT *var)
{
    FIXME("%p, {%s,%lu}, %p\n", iface, debugstr_guid(&key->fmtid), key->pid, var);
    return E_NOTIMPL;          /* var 原封不动，是未初始化的栈内存 */
}
```

COM 出参即使在失败路径也必须初始化（`PropVariantInit(var)`）。调用方若不检查返回值
直接读 `var.pwszVal`，拿到的就是栈垃圾 —— 与观测到的 `QString::fromUtf16(垃圾指针)`
形态完全一致。日志确认该调用发生过：

```text
030c:fixme:shell:window_prop_store_GetValue 00007FCE52EFF0A0, {{9f4c2855-9f79-4b39-a8d0-e1d42de1d5f3},5}, 00007FFFFE206BC0
```

**但据实说明**：这次调用发生在日志靠前位置（第 9792 行），而崩溃在第 14734 行，两者之间
隔了数千行。所以**机制吻合、但因果关系尚未证实**。要坐实需要拿到 `fromUtf16` 的调用方，
而 Sentry 的 minidump 只含 302 段内存、不含该线程完整栈，栈扫描得到的候选地址又因模块
大小字段不可靠而无法确证。

**这条路径的实用结论**：剩下的崩溃在**客户端自身 + WebView2 + Qt** 这条链上，不在驱动、
不在 wined3d、不在本项目改得动的任何一层。要继续投入只能靠上游修（Wine 的
`window_prop_store_GetValue` 初始化出参，或客户端自行检查返回值）。

#### 4j. 重编译 Wine 修复 Qt5Core 崩溃 —— **已成功**

按 5.2u.4i 的结论重编译 Wine 并验证。

##### 4j-1. 补丁

`dlls/shell32/shell32_main.c` 里窗口属性库的三个方法补齐出参初始化：

```c
window_prop_store_GetCount(...)  { *count = 0;                    return E_NOTIMPL; }
window_prop_store_GetAt(...)     { memset(key, 0, sizeof(*key));  return E_NOTIMPL; }
window_prop_store_GetValue(...)  { PropVariantInit(var);          return E_NOTIMPL; }
```

补丁与可复现的构建脚本：`uuyc-wine/support/winepatch/`。
构建方式：下载 wine-11.17 源码 → 打补丁 → `--enable-archs=x86_64 --disable-tests`
→ `make -k -j4` → `make -k install`（`make install` 不装 unix 侧 `.so`，脚本里补拷）。

两处构建坑记录：
* `dlls/opencl` 因缺 OpenCL 头文件编译失败（configure 却检测通过）。用 `make -k`
  跳过即可，其余全部编译成功；重新 configure 反而会触发全量重编。
* `make -k install` 之后 `wine --version` 报 `could not load ntdll.so` —— 安装目标
  没有拷贝 unix 侧的 33 个 `.so`，手工从构建树拷入 `lib/wine/x86_64-unix/` 后正常。

##### 4j-2. 验证一：API 级（隔离测试）

`support/shshim/testshshim.c`：把 PROPVARIANT 灌 `0xAB` 再调用 `GetValue`。

```text
修复前: GetValue -> 0x80004001  vt = 43947 (0xABAB)  pwszVal = ABABABABABABABAB
        RESULT: PROPVARIANT WAS NOT TOUCHED -- the bug is present
修复后: GetValue -> 0x80004001  vt = 0            pwszVal = 0000000000000000
        RESULT: PROPVARIANT WAS INITIALISED -- fixed
```

##### 4j-3. 验证二：真客户端（决定性）

同一台机器、同一前缀、同一客户端，只换了 Wine：

| | 修复前 | 修复后 |
| --- | --- | --- |
| 会话发起后 | 约 4 秒后崩于 `QString::fromUtf16` | **不崩** |
| `Sentry/reports/` | 每次会话新增一个 50MB dump | **零个新 dump** |
| `last_crash` | 每次尝试都刷新 | 停在修复前的 `18:26:51Z` |
| `GameViewerServer.exe` | 从未启动 | 运行中，与中转服务器建立 TCP |
| `GameViewer.exe` | 已死 | 存活，主窗口 `网易UU远程` 在 |
| 会话日志 | 4 秒截止 | 持续写到约 1 分钟后 |

**这一次只改了"初始化出参"这一件事，而此前每次会话必崩的崩溃不再复现 —— 因果关系就此坐实**
（5.2u.4i 里只能证明机制吻合，未能证明因果）。

##### 4j-4. 尚未解决的部分（与上述 bug 无关）

客户端不再崩溃，但**会话仍未真正建立**：`GameViewerServer.exe` 有 TCP 到中转服务器，
但**没有任何 UDP 连接**，且多条连接停在 `CLOSE-WAIT`（服务器发来数据未读完即断开）。
没有远程桌面窗口，`CreateVideoDecoder` 也仍为 0，说明视频链路还没走到。

结论措辞：**崩溃是 Wine 的 bug（已修）；连不上是另一个独立问题。**

#### 4k. 修复后：确认崩溃已除，下一个阻塞点是「未登录」

##### 4k-1. 崩溃确实没了（多轮复现）

| 轮次 | 会话/启动 | 新 dump | 异常数 |
| --- | --- | --- | --- |
| 03:31（`--hw-decode`） | 2 次 | **0** | — |
| 03:34（`--debug-crash`） | 1 次，302MB 日志 | **0** | **0** |
| 03:40 / 03:42（网络追踪） | 各 1 次 | **0** | — |

`GameViewer.exe`、`GameViewerServer.exe`、`GameViewerHealthd.exe` 全部存活，主窗口
`网易UU远程` 在。**修复前每次会话尝试约 4 秒必崩，现在连续多轮零崩溃。**

##### 4k-2. 新的阻塞点（独立于 Wine 缺陷）

客户端**没有登录令牌**，因此设备列表拉不出来、也就没有可点的设备：

```text
GameViewer.exe 启动时自己输出（第 65-112 行）：
  uuToken: ""
  gameId:  ""
  pcType:  ""

WebView2 cookie 库（Default/Network/Cookies）：
  .163.com  nrd_access_token       len=0  created=2026-09-17 19:34:08  last_access=同
  .163.com  nrd_csrf_access_token  len=0  created=2026-09-17 19:34:08  last_access=同
```

`nrd_access_token` 存在但**值为空**，且自创建起从未被访问过。

窗口标题（从 WebView2 子窗口读到的页面标题）也印证了这一点：

```text
Chrome_WidgetWin_1  title=网易UU远程       <- 主页面已加载
Chrome_WidgetWin_1  title=云设备骨架屏     <- 设备列表停在加载占位
```

即 App 认为自己在主界面，但设备列表停在骨架屏，**没有可点击的设备行**。

##### 4k-3. 本轮排除的干扰项（避免以后重走）

* `warn:d3d:wined3d_swapchain_vk_create_vulkan_swapchain Image count 0 is not supported (3-16)`
  —— **无害**。源码 `dlls/wined3d/swapchain.c:960-968` 只是把 image_count 钳到
  `surface_caps.minImageCount` 后打一条 WARN，尺寸同理。不是失败。
* `err:winediag:wined3d_dll_init Using the Vulkan renderer.` —— 是 err 级别的**诊断**输出，
  不是错误，说明 Vulkan 后端正常启用。
* `warn:d3d11:d3d11_device_GetDeviceRemovedReason` ×5747 —— **全部来自
  `msedgewebview2.exe`**（按进程号归因确认），不是会话进程在轮询。
* `DXGI device window` 窗口 —— D3D11 设备的辅助窗口，正常。

##### 4k-4. 新增工具：不依赖屏幕的 Windows 侧 UI 控制（`support/uictl/`）

宿主会话被锁屏时，X 层的截图与输入都到不了应用（`spectacle`/`ffmpeg` 只能拍到锁屏黑屏，
输入被锁屏窗口吃掉）。但 **Win32 消息层不受影响**，因此写了 `uuyc-uictl.exe`：

```text
uictl list                     列出所有顶层窗口（hwnd/pid/类名/可见性/尺寸/标题）
uictl tree <match|0xhwnd>      递归打印子窗口树
uictl a11y <match> [depth]     用 MSAA(AccessibleObjectFromWindow) 读无障碍树
uictl shot <match> <out.bmp> [print|blt]   窗口截图
uictl click|dclick|move <match> <x> <y>    PostMessage 注入鼠标
uictl text|key <match> ...                 注入文本/按键
```

两个实现要点：
* 标题按 **UTF-8** 输出（`GetWindowTextW` + `WideCharToMultiByte(CP_UTF8)`），否则中文标题
  在控制台里全是 `??????` —— 正是靠这一点才读出 `云设备骨架屏`。
* 截图需要 **`BitBlt` 而非 `PrintWindow`**：Chromium/WebView2 不响应 `WM_PRINT`，
  `PrintWindow` 会「成功」返回一张全白图。

**已知限制（据实说明）**：客户端的 WebView2 内容用 **DirectComposition 视觉托管**渲染，
不进任何窗口 DC，所以 `PrintWindow`/`BitBlt` 都取不到像素；而 Chromium 对**隐藏**的
Chrome 窗口不响应 `WM_GETOBJECT`，`AccessibleObjectFromWindow` 只会拿到 Wine 的标准回退
对象（`childCount=0`）。**因此"看到"UI 这一环，在锁屏状态下仍然没有打通**；
但窗口结构、标题、点击注入都是可用的。

##### 4k-5. 继续验证所需的前置条件

要完成"UI 驱动直到会话建立"的验证，需要：

1. **宿主会话解锁** —— 否则无法目视 UI，也无法确认登录/设备列表状态；
2. **客户端处于已登录状态** —— 当前 `nrd_access_token` 为空，需要账号密码或扫码登录。

这两条都需要用户本人，无法由本项目代劳。**在此之前，"连不上"的根因是"没有登录令牌、
设备列表停在骨架屏"，而不是驱动、wined3d 或本项目改动的那一层。**

#### 4l. 第二轮：修正一个时间戳误读，确认 WebView2 正常、阻塞点是令牌为空

##### 4l-1. 先纠正我自己的一个错误

Chromium 的 `History`/`Cookies` 里存的是**自 1601 年起的 UTC 微秒**，我第一轮把它当本地时间读，
于是得出"页面自 09-17 19:41 起再没导航过"的结论 —— **错的**。按 UTC→CST 换算后，
那个时间其实是 09-18 03:41，正是当时那轮测试。

##### 4l-2. WebView2 是正常的（决定性实验）

重启客户端前后对比 `History` 的 `visit_count`：

```text
              重启前   重启后
cloud-device     17  ->  18
skeleton         17  ->  18
goods-list       11  ->  12
```

**每次启动都 +1，说明 WebView2 正常导航、页面正常加载。** 之前怀疑的
"WebView2 卡住不加载"不成立。

##### 4l-3. 每次启动都把访问令牌设成空值

```text
History : cloud-device  last_visit = 09-18 03:45:33   (刚才那次启动)
Cookies : .163.com nrd_access_token  created=09-18 03:45:34  last_access=同  len=0
          .163.com nrd_csrf_access_token                created=09-18 03:45:34        len=0
```

cookie 是**每次启动新建**、**值长度为 0** 的会话 cookie。配合客户端自己输出的
`uuToken: ""` / `gameId: ""` / `pcType: ""`，可以确定：

**客户端每次都以"无令牌"状态启动 → 设备列表拉不回来 → 停在自带的
`云设备骨架屏` 占位页 → 没有可点的设备 → 无从建立会话。**

页面标题也是从窗口树读出来的（`support/uictl`）：`网易UU远程`（App 外壳）与
`云设备骨架屏`（本地占位页，路径来自 History：
`file:///C:/Program Files/Netease/GameViewer/bin/html/skeleton/cloud-device-skeleton/index.html`）。

##### 4l-4. 排除"是我改坏的"

跑 patched Wine 之前备份过注册表，对比 `Software\Netease` 段：

```text
  差异只有 HttpServerPort 的端口号（每次启动随机）
  Netease 的键一个没丢
```

所以令牌缺失**不是**我重编译 Wine / 前缀重初始化造成的。

另外 `setting_*.ini` 里存在 `[aeawrrxd64alesbi]` 这样的**逐设备设置段**
（`mouse_mode` / `smart_mouse` / `device_tool_order_v1` ……）—— 这些只在真的连过那台设备后
才会写入，说明**用户此前确实登录并连过**，之后登录态丢失。

##### 4l-5. 本轮探索但未走通的路

* 客户端跑着本地 HTTP 服务（`HttpServerPort`，实测 `127.0.0.1:57923`，`/` 返回 404）。
* 从 `GameViewerServer.exe` 提取出完整 API 路由表（`/api/v1/device/list`、
  `/api/v1/app/list/of/device`、`/api/v1/device/controllable`、`/api/v1/cloud_pc/...` 等），
  但本地端口对这些路径全部 404，直连 `uuyc.webapp.163.com` 则被 SPA 的 catch-all 吞掉
  （返回 index.html）。**这些是远端 API 路径，不是本地路由。**
* 设置文件里那个哈希键段（`[1c7bb475...]`）用常见键名做 SHA256 比对**没有命中**，
  是加盐或其它方案，加密配置无法直接解出。

##### 4l-6. 新增工具：`uuyc-wine-sessioncheck`

把每轮手工取证自动化（`uuyc-wine/support/` 同级的 `uuyc-wine-sessioncheck`），一次运行给出五项判定：

```text
1. 客户端是否还活着
2. 是否产生新的崩溃 dump（客户端自己吞异常，Wine 日志会说"无未处理异常"）
3. 视频解码器是否建起来了（vdshim 日志里的 CreateVideoDecoder）
4. 会话是否真的走到网络（媒体流需要 UDP）
5. WebView2 当前显示哪个页面（从窗口树读标题）
```

实跑输出（当前状态）：

```text
1. client running: yes            (7 个进程)
2. new crash dump: none           last_crash 停在修复前的 18:26:51Z
3. CreateVideoDecoder: 0          profile 查询 195 次 / config 查询 39 次
4. TCP 13, UDP 0                  ← 只有控制通道，没有媒体流
5. title=网易UU远程 + title=云设备骨架屏
```

#### 4m. 第三轮：拿到完整的原生桥协议，定位到令牌的来源

##### 4m-1. 网页 UI 与原生层的桥（`bin/html/error/uuycbridge.js`）

客户端的 UI 是**远程网页**（`https://uuyc.webapp.163.com/cloud-device`），随包只带了 6 个静态
骨架页 + 一份桥脚本。那份桥脚本是可读的，于是拿到了完整的通信协议：

```js
// Windows 分支：必须存在这个宿主对象，否则直接返回
// {code:-1, msg:"Bridge environment loading failed."}
window.chrome.webview.hostObjects.host.JSCallCppWithCallbackParameter(...)

// 网页向原生要鉴权的两个方法：
m = "getTicket"          // 取票据
y = "getHeaderParams"    // 取鉴权 HTTP 头

// 其余桥方法
b="syncClientStatus"  O="setNavConfig"  E="getOriginUIConfig"
x="setOriginUIConfig" j="getSystemConfig" S="setSystemConfig"
B="sendRenderFinishEvent"  v="onHandleUri"  p="onCopy"  w="onClose"
```

环境判定靠 User-Agent：`navigator.userAgent.includes("remote-Window")` → `win`。

**因果链因此完全确定：**

```text
网页 UI 启动
  └─ UUYCBridge.getTicket()        -> 原生 getTicket
  └─ UUYCBridge.getHeaderParams()  -> 原生 getHeaderParams
        ↓  拿到鉴权头（当前为空）
  └─ 调设备列表接口
        ↓  无鉴权
  └─ 返回空  ->  页面保持在 云设备骨架屏  ->  没有可点的设备  ->  无从建立会话
```

这也解释了为什么客户端自己的输出里 `uuToken` / `gameId` / `pcType` 全是空串 ——
原生侧没有票据，网页侧自然拿不到鉴权头。

##### 4m-2. 排除 DPAPI 作为"令牌丢失"的原因

怀疑过：应用用 DPAPI 加密保存凭据，而我重建了 Wine、前缀被更新过，导致解不开 → 退化成未登录。

实测排除：

```text
GameViewer.exe         未使用 DPAPI（而打印 uuToken: "" 的正是它）
GameViewerServer.exe   使用了 DPAPI
日志中 crypt32/bcrypt/ncrypt 错误数：0
```

即：打印空令牌的那个进程根本不碰 DPAPI，日志里也没有任何加密 API 报错。**DPAPI 不是原因。**

##### 4m-3. 本轮试过但走不通的

* 桥的 `console.log`（含 `birdge注册成功` / `Bridge environment loading failed` 等）**没有被宿主
  转发到 OutputDebugString**，所以无法从日志间接判断桥是否健康（搜遍 302MB 日志，0 命中）。
* 桥方法名在客户端二进制的 ASCII 与 UTF-16 字符串里**都搜不到**（网易做了字符串混淆），
  无法从原生侧反查 `getTicket` 的实现。
* `uuremote://external/assistance` 通过启动器 `--uri` 触发：启动器会阻塞等待客户端，超时；
  该路径不作为诊断手段。

##### 4m-4. 结论

到本轮为止的状态：

| 项 | 结论 |
| --- | --- |
| Wine 的 PROPVARIANT 缺陷 | **已修复并多轮验证**（客户端零崩溃） |
| WebView2 | 正常（每次启动都导航、页面加载成功） |
| 客户端 UI 状态 | 主界面已加载，设备列表停在 `云设备骨架屏` |
| 根因 | **原生侧没有票据** → 网页拿不到鉴权头 → 设备列表为空 |
| 是否我造成的 | 否（注册表对比无丢失；DPAPI 无关；修复前後 `uuToken` 都是空） |

**要继续必须由用户完成两步**：解锁宿主会话（KDE 锁屏需密码）+ 登录 UU 远程账号。
登录 UI 是 WebView2 渲染（DirectComposition 无像素可截、隐藏窗口不响应 `WM_GETOBJECT`），
锁屏下既看不到也点不到；令牌由账号凭据派生，无法代劳。

#### 5. 实测之后的两种走向

| `uuyc-wine-vkvideo` 结果 | 含义与下一步 |
| --- | --- |
| `VK_KHR_video_decode_h264 YES`（加了 flag） | 驱动侧通路打通。再跑 `uuyc-wine --hw-decode`；若会话仍有问题，嫌疑转到 wined3d 的 DXGI 格式处理（见 5.2l/5.2p 的 103↔87 问题），而不是驱动 |
| 加了 flag 仍然 `no` | 说明 `ANV_DEBUG` 没传进客户端进程，或该 Mesa 构建未编入视频代码；用 `ANV_DEBUG=video-decode vulkaninfo \| grep -i video` 交叉验证 |

同时提醒：`uuyc-wine-hwdecode` 里那段"Wine 硬解建立在宿主 VA-API 之上"的注释是**错的**，
wined3d 没有 VA-API 后端。该脚本第 2 节的 `vainfo` 只能证明**硬件**会解 H.264，
不能证明 Wine 拿到了解码器。真正要看的是 Vulkan Video 扩展。

### 5.3 WebView2（部分验证 — 受网络与 TLS 环境限制）

* 引导器能在前缀里启动，并识别到 Linux 侧代理
  （`WINEDEBUG=+winhttp` 追踪显示它连到了 `127.0.0.1:7897`）；
* 但 TLS 校验失败：`warn:winhttp:netconn_secure_connect cert verify failed: 12045`，
  随后 `err:secur32:start_samss Failed to open SamSs service`。引导器自带的
  证书自修复（下载 `cacerts.digicert.cn/DigiCertGlobalRootG2.crt`）也因同样的
  TLS 问题失败，最终退出码 4 并把错误上报到 `sentry.netease.com`。
* 结论：**本机无法完成 WebView2 的自动安装**，这属于 Wine 前缀的 TLS/CA 环境问题，
  与两个安装包本身的适配无关。
* 因此启动器提供 `UUYC_WINE_SKIP_WEBVIEW=1` 逃逸开关（本次端到端验证即使用它），
  并在 `INSTALL.md` 2.2 节给出代理配置与联网后 `--repair` 补齐的步骤。

### 5.4 Android 客户端（未验证 — 环境缺前置条件，据实说明）

本机 **无法** 完成安卓侧运行验证，原因明确：

1. 内核未加载 / 未编译 `binder_linux`、`ashmem_linux`；`/dev/binder*` 不存在
   → Waydroid 容器无法启动；
2. 无 `/dev/kvm` → 即便启动也没有硬件加速；
3. 沙箱内无法提权（`no new privileges`），不能 `modprobe`、`waydroid init`、
   或写 `/var/lib/waydroid/waydroid_base.prop`。

因此安卓侧的验证截止到：

* ✅ APK 身份 / 版本 / ABI 解析确认（`com.netease.uuremote` 4.40.0，arm64-v8a，19 个 `.so`，3 个 dex）；
* ✅ `uuyc-android --status`、`--help` 实跑通过，能正确报告 binder/kvm/会话/翻译层缺失；
* ✅ 启动器与助手脚本语法检查、`desktop-file-validate` 全部通过；
* ✅ APK 以官方原始字节打包（sha256 与上游一致），未重打包、未重签名；
* ❌ 未在真实 Waydroid 会话中执行 `waydroid app install` / `am start`。

在具备 `binder_linux` + Wayland 的 Arch 主机上按 `INSTALL.md` 第 3 节执行即可补齐；
`uuyc-android-verify` 会逐项报告缺失项。

### 5.2v 会话崩溃根因确定并被遏制：`qwindows.dll` 传了未初始化的 `OUTLINETEXTMETRICW` 偏移

本节取代 5.2c–5.2s 里对"点进桌面即崩"的全部归因。此前把崩溃算在
`window_prop_store_GetValue` 未初始化 PROPVARIANT 头上，**该归因是错的**：
`wine-bug-propvariant.md` 已加更正说明，那个缺陷本身仍然成立（可用 30 行毒化测试
单独复现），但与本次崩溃无关 —— 崩溃在打补丁前后的 Wine 上表现完全一致。

**故障点**（从 Sentry minidump 的 `MINIDUMP_EXCEPTION_STREAM` 里取 CONTEXT，
`context.rip == ExceptionRecord.ExceptionAddress`，即该 CONTEXT 就是故障现场）：

```
EXCEPTION 0xc0000005 at Qt5Core.dll+0xbf11b   =  QString::fromUtf16 (0xbf0f0) + 0x2b
1800bf113: test r8d,r8d
1800bf116: jns  0x1800bf12f      ; size >= 0 则跳过扫描
1800bf118: mov  r8d,ecx          ; size < 0 → r8d 被覆写为 0
1800bf11b: cmp  WORD PTR [rdx],cx   ; ← 崩在这里，rdx 未映射
```

注意两点取证纪律：

* minidump 的 `ExceptionInformation[1]` **每份都是 `0xffffffffffffffff`**（包括另一份
  已知真实地址是普通堆指针的 d3d11 越界读），是占位垃圾，**不能当访问地址用**；
* `r8` 读出为 0 不代表入参 size 为 0 —— `0xbf118` 已经把它清零了，能走到 `0xbf11b`
  恰恰说明入参 size 为负（NUL 结尾模式）。

**调用者链**（返回地址槽由序言确定，非猜测：`fromWCharArray` 是
`push rbx; sub rsp,0x30`，`call` 压 8，`fromUtf16` 是 `push rbx; sub rsp,0x40`，
故 `0x38+8+0x48 = 0x88`）：

| 栈槽 | 值 | 含义 |
| --- | --- | --- |
| `[rsp+0x48]` | `Qt5Core.dll+0x4dcb` | `fromWCharArray+0x1b` 的返回地址 |
| `[rsp+0x88]` | `qwindows.dll+0x704f0` | `fromWCharArray` 调用者的返回地址 |

栈扫描必须**只认可执行节内的值**：先前用"是否落在模块镜像内"判断，把 vtable 和
`.rdata` 也算了进去，得出过错误答案。

**根因**：`qwindows.dll+0x70260` 是 `QWindowsFontEngine` 构造路径（导入
`GDI32!GetOutlineTextMetricsW`/`GetFontData`/`SelectObject`、
`QFontEngine::getSfntTable`/`getCMap`/`loadKerningPairs`、`QFile::encodeName`，
并加载 `"cmap"`）。它按 Win32 文档约定读字体名：

```asm
1800704d5: mov  rdx,[rdi+0xe0]
1800704e1: add  rdx,rdi          ; (char *)otm + otm-><0xe0>
1800704e4: mov  r8d,0xffffffff   ; size = -1
1800704ea: call [Qt5Core!?fromWCharArray@QString@@SA?AV1@PEB_WH@Z]
```

`OUTLINETEXTMETRICW` 的四个名字字段存的是**相对结构体起始的字节偏移**而非指针：
`otmpFamilyName 0xC8`、`otmpFaceName 0xD0`、`otmpStyleName 0xD8`、**`otmpFullName 0xE0`**。
`0xE0` 正是 `otmpFullName`。即 Qt 完全按约定行事，**是 Wine 的
`GetOutlineTextMetricsW` 没把这个偏移填成合法值**，算出的指针落在未映射区，Qt 再按
NUL 结尾去 strlen 它。同一函数里另一条取字体名的路径（`qwindows.dll+0x699c1`，传
`this+0x1C` 即 `LOGFONTW.lfFaceName`）从不崩溃，对照鲜明。

**遏制**（`support/vdshim/iathook.c`，随包发布）：给所有导入
`QString::fromWCharArray` 的模块打 IAT hook，调用前校验指针 —— `size >= 0` 要求整段
可读；`size < 0` 要求在可达的已提交区间内存在 NUL 终止符（这是 Qt 的 `qu_strlen`
唯一的停止条件）。校验失败就以 `NULL` 调用真函数（Qt 直接返回 null QString，不解引用），
并把调用者与参数写入 `C:\uuyc-qtguard.log`。两个实现要点：`qwindows.dll` 是**插件**，
`DllMain` 时打补丁必然错过它，故由轮询线程重复安装；重复安装必须幂等，否则第二遍会把
`replacement` 记成 original，守卫自我递归。

**实测**（同一前缀、同一客户端，11:17 点击「进入桌面」）：

| | 修复前 | 修复后 |
| --- | --- | --- |
| 点击后 `GameViewer.exe` | 1–3 秒内消失 | 持续运行，进程数稳定 |
| `Sentry/reports/` | 每次尝试新增约 46 MB dump | **无新 dump**，`last_crash` 仍停在修复前的 03:08:50Z |
| 会话窗口 | 从未出现 | `44040204 "稻香" 1536x904 @ (192,65)` |
| 画面 | — | 实时：计时器 `00:00:56` → `00:02:12`，23–97 fps，2.1–7.7 Mbps，0.0% loss |
| TCP | 12–13，无会话对端 | 26，含 54508/54509/54511 会话端口族 |

计时器跨截图递增（而非只是画了一帧）与 fps/码率/延迟持续变化，是"不是冻结帧"的依据。
守卫全程只命中 2 次，均来自 `qwindows.dll+0x704f0`。

**仍未证明的部分**：这是对已精确定位缺陷的**遏制**，不是修复。Wine 的
`GetOutlineTextMetricsW` 具体在哪一步没写对 `otmpFullName` 尚未查明，也没有写出并
编译 Wine 侧补丁。守卫偏保守，合法但未终止的 `fromWCharArray(..., size<0)` 调用同样
会被降级为 null QString（实测未出现，代价是"某个字体名可能取不到"换"进程不死"）。

详见 `docs/qwindows-fontname-crash.md`。

## 6. 与既有 AUR 方案的关系

AUR 中已有 `uuyc-wine`（维护者 ParticleG，`pkgver=4.33.0.8907`）用同一条 Wine 路线
适配旧版本。本仓库的差异：

| | AUR `uuyc-wine` | 本仓库 `uuyc-wine` |
| --- | --- | --- |
| 上游版本 | 4.33.0.8907（在线下载） | **4.40.1（本地官方包，含真实 sha256）** |
| 启动器 | 606 行，含旧目录迁移逻辑 | 重写为 ~380 行，只服务 4.40.1 与本工作区路径 |
| `wevtapi` 兼容层 | 同名 0BSD 汇编 | 同样的兼容层，注明来源与许可证 |
| Android 侧 | 无 | **新增 `uuyc-android`（Waydroid + ARM 翻译层助手）** |
| 许可校验 | 在线抓取 EULA 页面并做语义哈希校验 | 未实现在线 EULA 校验（沙箱内无法稳定抓取），改为随包附 0BSD + 官方下载页说明 |

两个包名不冲突，可共存；若你已装 AUR 版，建议 `paru -Rns uuyc-wine` 后使用本仓库版本，
以免两个启动器各自维护一份前缀。
