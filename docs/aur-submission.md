# 放进 AUR 的可行性调研（2026-09-18）

## 结论先说：能，但**不能新开一个包**

AUR 官方准则原文（<https://wiki.archlinux.org/title/AUR_submission_guidelines>）：

> Check the AUR if the package already exists. **If it is currently maintained, changes
> can be submitted in a comment for the maintainer's attention.** If it is unmaintained
> or the maintainer is unresponsive, the package can be adopted and updated as required.
> **Do not create duplicate packages.**

而 `uuyc-wine` 已存在且**有人在维护**：

| 字段 | 值 |
| --- | --- |
| 版本 | `4.33.0.8907-9` |
| 维护者 | `Particle_G`（particle_g@outlook.com） |
| 提交 / 最后修改 | 2026-07-21 / 2026-08-01 |
| 票数 / 热度 | 1 / 0.37 |
| 是否标过期 | 否 |
| 依赖 | `wine>=11.1` `hicolor-icon-theme` `procps-ng` `util-linux` `diffutils` |
| 许可 | `LicenseRef-UU-Remote-EULA AND 0BSD` |

值得注意的是**他们已经解决了两个最麻烦的问题**：下载地址，以及用
`as --64` + `objcopy -O pe-x86-64` + `ld -mi386pep` 从汇编源码编译 `wevtapi.dll`
（与 `support/wevtapi-shim/` 完全同路）。所以这不是"从零做一个包"，而是"把增量并进去"。

## 三条可选路径

| 路径 | 做法 | 评价 |
| --- | --- | --- |
| **A. 交给现有维护者** | 在 AUR 包页评论 + 邮件 `particle_g@outlook.com`，说明我们有的增量 | 最合规。但他们的包不依赖我们的 shim，合并方式需要讨论 |
| **B. 申请弃养后接手** | 维护者两周不响应可提 orphan 请求；被标过期满 180 天自动通过 | 接手后名字仍是 `uuyc-wine`，工作直接进主线，**最干净的结局** |
| **C. 另起包名** | 按规则须带 `-bin` 后缀（预编译产物 + 非自由软件），如 `uuyc-wine-bin` | 风险：同一软件另起名很可能被当作重复包删除 |

## 如果确实要发，我们的包必须先改造

| 问题 | 现状 | AUR 要求 |
| --- | --- | --- |
| 安装包来源 | `uuyc-wine/local/uuyc-installer.exe`（86 MB 本地文件） | `source=` 必须是可下载 URL + sha256 ✅ 见下 |
| 预编译产物 | 包里含约 2.8 MB 我们自编译的 DLL/EXE | 必须在 `build()` 里从源码编译（源码齐全） |
| `-bin` 后缀 | 无 | 预编译产物必须带，非自由软件亦然 |
| makepkg 产物 | 仓库里有 `.pkg.tar.zst` | **禁止**进 AUR（本仓库已 gitignore） |
| LICENSE | 0BSD ✅ | 需要 |
| Maintainer 注释行 | 无 | 必须加在 PKGBUILD 顶部 |
| `.SRCINFO` | 无 | `makepkg --printsrcinfo > .SRCINFO` |
| 分支 | `main` | AUR **只接受 `master`** |
| 认证 | 无 | SSH key pair，公钥填进 AUR 账号 |

## 官方下载地址（已实测打通）

官网下载按钮指向 `https://adl.netease.com/d/g/uuremote/c/gw?type=pc`，该页面用 JS 跳到

```
https://api.nrd.nie.163.com/api/v1/release/dl/1?channel=gwqd
```

**这个 API 就是稳定的版本发现入口**：请求它（带 `Referer: https://uuyc.163.com/`）会返回
302，`location` 里就是当前版本的安装包地址。实测 2026-09-18：

```
location: https://a56.gdl.netease.com/UURemote_Setup_4.41.0.2311_0917074721_gwqd.exe?key1=...&key2=...&n=uuyc_4.41.0.exe
content-length: 91174552
last-modified: Thu, 17 Sep 2026 07:50:29 GMT
```

**关键点：去掉 query 参数后的裸 CDN 地址同样返回 200**（实测 content-length 一致），
所以可以直接写进 `source=()` —— 与现有 AUR 包的写法一致：

```
https://a56.gdl.netease.com/UURemote_Setup_${pkgver}_${_build}_gwqd.exe
```

（`_build` 如 `0917074721` 按版本变化，需要每次从 API 取；`channel=gwqd` 对应 URL 尾部
的 `_gwqd` 后缀。）

## 一个需要先决定的问题：版本

* 我们的适配基于 **4.40.1**（90,790,272 字节，sha256 `64f918b8…`）
* 上游当前已是 **4.41.0**（91,174,552 字节，2026-09-17 构建）

往 AUR 推之前，是否先把适配升到 4.41.0？否则推上去立刻就是过期版本。

## 我能做和不能做的

**能做**：生成 AUR 格式的 PKGBUILD（源码编译 shim、URL 源、`.SRCINFO`、LICENSE、
Maintainer 行），本地 `makepkg` 验证能构建出包，生成 SSH key pair 并把公钥给你。

**不能做**：注册 AUR 账号、决定包名（涉及"是否重复包"的政策判断）、替你承担维护责任。
公钥填进 AUR 账号后我就能推送（AUR 只收 `master` 分支）。
