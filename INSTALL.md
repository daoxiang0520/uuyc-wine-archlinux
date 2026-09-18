# 安装与运行

本文档对应本工作区中的两个官方安装包：

* `uuyc_4.40.1.exe` → 包 `uuyc-wine`（Wine 前缀方式运行 Windows 客户端）
* `uuyc_4.40.0.apk` → 包 `uuyc-android`（Waydroid 容器方式运行 Android 客户端）

> 文中 `sudo` 步骤需要你自己在终端执行；本仓库的脚本不会替你提权（除非你显式运行
> `scripts/install.sh`，它内部调用 `sudo pacman -U`）。

---

## 0. 主机要求

```bash
# 基础工具
sudo pacman -S --needed base-devel git 7zip unzip python

# Wine 客户端（A 部分需要）
sudo pacman -S --needed wine wine-mono wine-gecko

# 安卓客户端（B 部分需要）
sudo pacman -S --needed waydroid
```

* 显示服务：Wine 走 X11/Wayland（本机 `DISPLAY=:1`、`WAYLAND_DISPLAY=wayland-0`）；
  Waydroid **必须** 在 Wayland 会话下运行。
* 中文字体：Wine 客户端界面为中文，建议 `sudo pacman -S noto-fonts-cjk`。
* 本机实测环境：Arch Linux rolling、`wine 11.17-1`、`waydroid 1.6.3-1`、内核无
  binder 模块、无 `/dev/kvm`（详见 `VERIFY.md` 第 4 节）。

---

## 1. 构建

```bash
cd /home/daoxiang/ds-workplace/uuyc-archlinux
./scripts/build.sh              # 两个包都构建
./scripts/build.sh wine         # 只构建 uuyc-wine
./scripts/build.sh android      # 只构建 uuyc-android
```

产物：

```text
uuyc-wine/uuyc-wine-4.40.1-1-x86_64.pkg.tar.zst      (~90 MB)
uuyc-android/uuyc-android-4.40.0-1-x86_64.pkg.tar.zst (~24 MB)
```

`makepkg` 会校验 `local/` 里官方安装包的 SHA-256；文件不匹配时构建直接失败，
不会产出不可用的包。若你替换了上游版本，请同步更新 `PKGBUILD` 中的 `pkgver` 与
`sha256sums`。

---

## 2. A 部分：Windows 客户端（`uuyc-wine`）

### 2.1 安装

```bash
sudo pacman -U uuyc-wine/uuyc-wine-4.40.1-1-x86_64.pkg.tar.zst
# 或者用脚本（会同时提示安卓侧步骤）
./scripts/install.sh --wine-only
```

### 2.2 首次启动

```bash
uuyc-wine
```

首次启动会依次完成（全部记录在 `${XDG_STATE_HOME:-$HOME/.local/state}/uuyc-wine/setup.log`）：

1. 创建 64 位 Wine 前缀 `${XDG_DATA_HOME:-$HOME/.local/share}/uuyc-wine/wineprefix`，
   并设置为 Windows 10；
2. 静默运行官方安装包 `/S /launchapp=0 /autorun=0`；
3. 写入 `wevtapi.dll` 兼容层；若安装器没有完成 `GameViewerService` 注册
   （实测静默安装会跳过这一步），启动器用 `wine sc create` 补注册并设为自启动；
4. 安装 WebView2 运行时（取自安装包自带的引导器，**需要联网**，约 150 MB）；
5. 启动 `GameViewerService` → `GameViewerHealthd.exe` → `GameViewerServer.exe` →
   `GameViewer.exe`。

整个过程几分钟到十几分钟（取决于磁盘和网络）。装配完成后再次启动是秒级的。

#### 需要代理时（WebView2 下载）

WebView2 引导器走 WinHTTP / WinINet，会跟随 Wine 继承的代理环境变量。若本机需要
代理（例如本机可用 `127.0.0.1:7897`），在启动前导出即可，代理只对本次进程生效：

```bash
export http_proxy=http://127.0.0.1:7897
export https_proxy=http://127.0.0.1:7897
uuyc-wine

# 先验证引导器目标站可达：
curl -x http://127.0.0.1:7897 -o /dev/null -w '%{http_code}\n' https://aka.ms/
```

无法联网时可以先跳过 WebView2 校验把前缀装配完（客户端 UI 仍需 WebView2 才能正常
显示，联网后重跑 `--repair` 即可补齐）：

```bash
UUYC_WINE_SKIP_WEBVIEW=1 uuyc-wine --setup-only
uuyc-wine --repair
```

### 2.3 常用命令

```bash
uuyc-wine --setup-only     # 只装配前缀，不启动界面
uuyc-wine --repair         # 重新检查并修复前缀
uuyc-wine --stop           # 停掉该前缀里的 wineserver 与全部客户端
uuyc-wine --uri 'uuremote:...'   # 通过 Wine 注册的处理器打开 URI
uuyc-wine --version        # 打印上游版本（4.40.1）

UUYC_WINE_PREFIX=/path/to/prefix uuyc-wine    # 使用自定义前缀
tail -f "${XDG_STATE_HOME:-$HOME/.local/state}/uuyc-wine/setup.log"
```

### 2.4 已知排障

| 现象 | 处理 |
| --- | --- |
| `GameViewerServer.exe did not start` | `uuyc-wine --repair`；确认 `usr/share/uuyc-wine/wevtapi.dll` 与前缀内 `bin/wevtapi.dll` 一致 |
| 界面白屏/登录页空白 | WebView2 未就绪：删掉前重跑 `--repair`；日志里搜索 `WebView2` |
| 字体乱码/方块 | 安装 `noto-fonts-cjk`，然后 `uuyc-wine --stop && uuyc-wine` |
| 点设备后窗口闪退回列表 | 见下方「进入桌面就退回」小节；先 `uuyc-wine-doctor > report.txt` |
| 串流画面黑屏 | Wine 的 D3D 路径问题；试 `WINE_D3D_CONFIG=renderer=gl uuyc-wine`，或改用安卓客户端 |

#### 进入桌面就退回 / 连不上（会话建立失败）

会话画面由 `bin/streamer.dll` 解码渲染，它的导入表硬依赖
`d3d11.dll`/`dxgi.dll`/`d2d1.dll`/`dWrite.dll`。实测判据来自客户端自己的 Sentry
记录（明文 JSON，未加密），一行就能看出来是**崩溃**还是**连不上**：

```bash
P=~/.local/share/uuyc-wine/wineprefix
cat "$P/drive_c/Program Files/Netease/GameViewer/log/client/Sentry/last_crash"
grep -rhoE '\{"init":true[^}]*\}' \
  "$P/drive_c/Program Files/Netease/GameViewer/log/client/Sentry" | tail -1
```

* `"status":"crashed"` + `"duration":4.x` → 进程几秒内崩了：渲染管线问题；
* 进程活着但提示连不上 → 视频管线建不起来，客户端主动放弃会话（**不是**防火墙）。

**先确认 GPU 没被"环境"藏起来**（这是最容易踩的坑，实测过）：

```bash
uuyc-wine-gpu          # 一行告诉你 GPU 是否可见
```

判定标准很简单：

```bash
ls -l /dev/dri/renderD*        # 必须存在且对你的用户可读写
vainfo | head -5               # 必须列出 iHD 驱动和一个设备
```

如果这两条失败，而 `lspci -nnk | grep -A2 VGA` 显示 `Kernel driver in use: i915`，
那就是**运行环境**把 `/dev/dri` 隐藏了，不是驱动没装。典型情况：

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `/dev/dri` 不存在，但 `lsmod` 有 `i915` | 进程跑在 `bwrap --dev /dev` 之类的沙箱/容器里（精简 `/dev` 不含 GPU 节点） | 从**普通桌面终端**或桌面菜单启动；或 `systemd-run --user --scope uuyc-wine` |
| `/dev/dri/renderD128` 存在但不可写 | 用户不在 `video` 组 | `sudo usermod -aG video "$USER"` 后重新登录 |
| Docker 里 | 没做设备直通 | 容器加 `--device /dev/dri` |

`libva error: vaGetDriverNames() failed with operation failed` 正是"列到了驱动、
但找不到任何设备"的报错；`Xlib: extension "DRI2" missing on display ":1"` 只是
Xwayland 早就去掉了 DRI2（现代 Xwayland 只有 DRI3），本身无害，别被它带偏。

**渲染器选择是有效旋钮**（`WINE_D3D_CONFIG` 是 man wine 里的正式变量）。有 GPU 时
优先试 `vulkan`——Wine 11.17 自带 `winevulkan.dll/so`，你的 `intel_icd.json` 也在：

```bash
WINE_D3D_CONFIG=renderer=vulkan  uuyc-wine   # 1) 首选：真 Intel GPU（ICD 已装）
WINE_D3D_CONFIG=renderer=gl      uuyc-wine   # 2) OpenGL / Mesa
WINE_D3D_CONFIG=renderer=gdi     uuyc-wine   # 3) 纯软件 GDI，最保守
WINE_D3D_CONFIG=renderer=no3d    uuyc-wine   # 4) 关 3D：能进界面但连不上
uuyc-wine --safe-graphics                    # 等价于 4)，启动器内置开关
```

启动器现在会在启动前检查 `/dev/dri/renderD*`：看不到 GPU 时直接给出警告（并指向
`uuyc-wine-gpu`），不会再让你白等一次必然失败的会话。

Vulkan 路线需要的 ICD 与 Intel 驱动本机已具备（`vulkan-intel`、`vulkan-icd-loader`、
`intel_media_driver`，`/usr/share/vulkan/icd.d/intel_icd.json` 存在）。缺的话：

```bash
sudo pacman -S vulkan-intel vulkan-icd-loader lib32-vulkan-intel
```

**四个都崩时**，用启动器内置的开关抓 Wine 级回溯，再用分析器取关键几节：

```bash
uuyc-wine --debug-crash        # 日志写到 <state>/crash-debug.log
uuyc-wine-crashlog             # 抽取：未处理异常 / backtrace / 图形错误
uuyc-wine-doctor               # 看 session graphics 一节
```

`uuyc-wine-doctor` 的 `session graphics` 会直接给出结论性指标：

```text
backend actually used : wined3d OpenGL (adapter_gl)
adapter_vk_*          : 0
swapchain presents    : 4
device removed reason : 22 call(s)
```

如果这里显示 **`device removed reason` 有值、但没有 GL/GPU 错误、也没有未处理异常**，
那就说明客户端建立了 D3D11 设备与会话交换链、渲染了几帧之后设备进入 removed 状态并
退出 —— Wine 在应用请求 wined3d 提供不了的能力时就是这样报"设备被移除"的。
**这种情况换渲染器没用**（本项目实测：默认 gl / gdi / vulkan 三者的结果一致，
只有 `no3d` 不崩但也因此无法串流）。

结论：该 Windows 客户端的**远程会话渲染路径在 Wine 下不可用**。此时可行选择是

* 用 `uuyc-android`（Waydroid）跑安卓客户端来串流；
* 或把 `uuyc-wine` 仅用于登录 / 设备列表 / 文件传输（`uuyc-wine --safe-graphics`）。

另一种方式：

```bash
paru -S minidump-stackwalk
minidump-stackwalk --human \
  "$P/drive_c/Program Files/Netease/GameViewer/log/client/Sentry/reports/"*.dmp
```

**别去查防火墙。** 网络层是好的：崩溃发生在会话建立阶段，且客户端在该阶段
会主动放弃时不会留下 `status=crashed` 记录。

---

### 3.6 可修方向：让 wined3d 走 Vulkan 后端（Wine 的 H.264 硬解只在这里）

对 Wine 的 `wined3d.dll` 做符号级检查后发现：**Wine 有完整的 H.264 硬解实现，但只挂在
Vulkan 适配器上**（`wined3d_decoder_vk_decode_h264` 等）；`adapter_gl_*` 与
`adapter_no3d_*` 只有 `create_video_decoder_output_view`，**没有解码器**。

Wine 也**没有 VA-API 视频后端**（所以宿主的 `vainfo` 通过并不等于 Wine 能硬解）。
它唯一依赖的是宿主的 **Vulkan Video 扩展**，而在 Intel 的 ANV 驱动上这些扩展**默认关闭**：

> **重要修正**：本项目早前把"扩展没暴露"解释成"Vulkan Video 需要 Gen12+，Gen9.5 不支持"。
> 核对 Mesa 源码后确认那是错的：`anv_physical_device.c` 里
> `.KHR_video_decode_h264 = VIDEO_CODEC_H264DEC && video_decode_enabled`，
> **没有世代判断**（世代判断只在 AV1 那一行），也不要求 HuC（只有 H.265 要求）。
> 真正的开关是 `ANV_DEBUG=video-decode`。详见 `VERIFY.md` 5.2u。

所以先跑**决定性的一步**：

```bash
# 1) 驱动到底暴不暴露 Vulkan Video 硬解？（包内工具，无需 vulkan-tools）
uuyc-wine-vkvideo
#   期望：section 3 显示 VK_KHR_video_decode_h264 IS available with ANV_DEBUG=video-decode
```

然后再按后端配置验证：

```bash
# 2) 持久地把渲染器设为 Vulkan（环境变量此前被静默回退，改用注册表）
WINEPREFIX=~/.local/share/uuyc-wine/wineprefix wine reg add \
  'HKCU\Software\Wine\Direct3D' /v renderer /t REG_SZ /d vulkan /f

# 3) 用刚确认过的开关启动客户端
uuyc-wine --hw-decode

# 4) 确认后端真的变了（关键：应出现 adapter_vk_*）
uuyc-wine --debug-crash            # 复现一次
uuyc-wine-crashlog | grep -E "adapter_(vk|gl)"

# 5) 硬件适配器上跑探针（注意：不要加 --warp，WARP 没有解码器）
uuyc-wine-d3dprobe
#   期望：NV12 / P010 行出现 DECODER_OUTPUT
```

判定：

| 结果 | 含义 |
| --- | --- |
| `uuyc-wine-vkvideo` 报 H.264 YES + `adapter_vk_*` 出现 + 探针 NV12 带 `DECODER_OUTPUT` | **可以硬解**：保留 Vulkan 渲染器与 `--hw-decode` 即可 |
| `uuyc-wine-vkvideo` 报 H.264 YES，但 `adapter_vk_*` 仍为 0 | wined3d 的 Vulkan 后端绑定失败，查 `+wined3d` 日志里它为什么回退 |
| `uuyc-wine-vkvideo` 报 H.264 no | 驱动侧就没开：确认启动的是 `uuyc-wine --hw-decode`，并用 `ANV_DEBUG=video-decode vulkaninfo \| grep -i video` 交叉验证 |
| `adapter_vk_*` 出现但 NV12 仍 `E_FAIL` | 回到 5.2l / 5.2p 的 DXGI 格式编号（103↔87）问题，属 wined3d 侧 |

### 3.7 探针用法（`uuyc-wine-d3dprobe`）

包内带一个自编译的 D3D11 探针，用来判定"Wine 到底能不能硬解"。
**注意：必须跑硬件路径**（不加 `--warp`）—— 软件光栅化（WARP）与 GL 后端本来就没有
解码器，在它们上面测视频格式必然失败，不代表硬件路径的结果。本机（沙箱只有 WARP）
实测输出如下，**仅作示例**：

```text
NV12             FAILED     0x80004005  E_FAIL      <-- 硬解标准输出格式
P010 / P016      FAILED     0x80004005  E_FAIL
420_OPAQUE/AYUV  FAILED     0x80004005  E_FAIL
YUY2             0x02000000  (只有 TYPED_UAV)
B8G8R8A8_UNORM   0x02E4F3F3  (TEXTURE2D / RENDER_TARGET / BLENDABLE / …)
summary: video decoder output format supported : NO
         video processor output format supported: NO
```

**读法**：普通渲染格式齐全 → 界面能显示；视频格式全部 `E_FAIL` → 会话拿不到解码
管线 → 设备 removed → 关窗。这就是「点进入桌面就退回且没有画面」的机制。

```bash
uuyc-wine-d3dprobe           # 硬件路径（你机器上跑这个）
uuyc-wine-d3dprobe --all     # 再跑软件光栅化对比
# 完整输出：${XDG_STATE_HOME:-$HOME/.local/state}/uuyc-wine/d3dprobe.txt
```

若要向上游报 Wine bug，附上这份输出 ＋ `uuyc-wine-crashlog` 的 3a 节即可。
Wine 升级后可重新编译探针复查（`/usr/share/uuyc-wine/d3dprobe/README.md` 有命令）。

---

## 4. 卸载

```bash
uuyc-wine --stop
sudo pacman -Rns uuyc-wine uuyc-android

# 可选：清理用户数据（Wine 前缀与 Waydroid 应用数据）
rm -rf -- "${XDG_DATA_HOME:-$HOME/.local/share}/uuyc-wine" \
          "${XDG_STATE_HOME:-$HOME/.local/state}/uuyc-wine" \
          "${XDG_STATE_HOME:-$HOME/.local/state}/uuyc-android"
uuyc-android --clear-data      # 需在卸载前执行
```

`uuyc-wine.install` 在卸载后会再次打印上述清理命令；pacman 不会自动删除用户数据。
