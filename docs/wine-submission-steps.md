# 往 WineHQ 提交：逐步操作

> 所有要粘贴的正文和要上传的附件都在 `docs/wine-attachments/` 下。
> 两个平台的账号不通用；两边都用英文正文（已写好，直接粘贴）。

---

## 第一步：Bugzilla Bug 53795（优先级最高）

地址：<https://bugs.winehq.org/show_bug.cgi?id=53795>

这个 bug 2022-10-15 提交，**四年零改动、CC 列表 0 人、状态仍是 UNCONFIRMED**，
而上游的修复 MR 已经写出来了却和它互不关联。把它俩接上，是投入产出比最高的一步。

| # | 操作 | 具体做法 |
| --- | --- | --- |
| 1 | 确认已登录 | 右上角应显示你的邮箱；没登录则 <https://bugs.winehq.org/createaccount.cgi> |
| 2 | **把自己加进 CC** | 在 *CC List* 一栏点 **Add**，填自己的邮箱并保存。当前 0 人，加进去才会收到后续通知 |
| 3 | 贴评论 | 在页面底部 *Additional Comments* 框里粘贴 `wine-attachments/bugzilla-53795-comment.txt` 的正文（**去掉开头两行 `#` 注释**），点 **Save Changes** |
| 4 | 上传附件 | *Add an attachment* → 依次上传三个文件，描述分别写：`standalone probe source`、`probe output`、`in-process call trace` |

三个附件：

```
uuyc-otmprobe.c        独立探针源码（不需要客户端、不需要 GUI）
otmprobe-output.txt    探针在本机的输出（1260 个字体实例 + 结论）
qt-call-sequence.txt   进程内实测的 QUERY/FILL 调用序列
```

`qt-call-sequence.txt` 里最有说服力的是这三行相邻记录 —— 同一进程、同一次运行：

```
QUERY cbData=0 otm=NULL             -> ret=0 FAILED, face=[Noto Color Emoji]
FILL  cbData=0 otm=00007F0EDD83FCA0 -> ret=0 FAILED, face=[Noto Color Emoji]
[fromWCharArray] BLOCKED ... rdi=0x7f0edd83fca0
```

`otm=` 与 `rdi=` 是同一个地址，这行证据直接说明：查询返回 0、Qt 没检查、
填充没写任何东西、Qt 随后就从那个 0 字节分配里读 `otmpFullName`。

---

## 第二步：GitLab MR !11388（把修复和 bug 连起来）

地址：<https://gitlab.winehq.org/wine/wine/-/merge_requests/11388>

这是 Draft、**至今无人 review**、0 条讨论。它缺的正是"这是真问题、影响不止一个应用、
机制已测清"的证据，而我们全有。

| # | 操作 | 具体做法 |
| --- | --- | --- |
| 1 | 登录 | <https://gitlab.winehq.org>，可用 GitHub 账号登录 |
| 2 | 贴评论 | 拉到 MR 页面底部讨论框，粘贴 `wine-attachments/gitlab-mr11388-comment.txt` 正文，提交 |
| 3 | 上传附件 | 评论框支持直接拖拽文件，拖入那三个文件 |

评论里已经写明了指向 Bug 53795 的链接，两边就串起来了。

---

## 第三步（可选）：PROPVARIANT 未初始化出参

这是**唯一可以新报**的缺陷 —— 我核对过上游 master，那三个方法至今仍是返回前不碰出参。
正文见 `docs/wine-bug-propvariant.md`（本身就是一份完整的报告）。

两种形式：

* **发 MR**（推荐，Wine 惯例是有测试的补丁优先）：把 `support/shshim/testshshim.c`
  的毒化断言改写成 `dlls/shell32/tests/` 里的 `ok(vt == VT_EMPTY, ...)` 形式，
  附上三行修复；
* **报 bug**：<https://bugs.winehq.org/enter_bug.cgi?product=Wine>，
  Component `shell32`，Version `11.17`。

> ⚠️ 报告里**绝不能**把它和"点进桌面崩溃"关联。那是我们早期犯过的错，
> 因果链已被实测推翻。一旦维护者发现证伪，整份报告的可信度都会受损。

---

## 第四步（可选）：D3D11 硬解无回退

`docs/wine-issue-draft.md` 这份草稿**先用不了** —— 它写的"anv 把 Vulkan Video
限制在 Gen12+"已被 `VERIFY.md` 5.2u 逐条核对 Mesa 源码后推翻（H.264 那行没有世代
判断，Gen12 门槛只在 AV1 上）。必须先改掉这个说法，另外还要先查上游那条
`[PATCH 1/5] d3d11: Implement ID3D11VideoDevice::GetVideoDecoderProfile[Count]()`
是否已经覆盖了它。

---

## 附：能否由我代为提交

两个平台都支持 API，凭据给我就能直接发：

* **Bugzilla**：在 <https://bugs.winehq.org/userprefs.cgi?tab=apikey> 生成 API key，
  然后 `POST /rest/bug/53795/comment`；
* **GitLab**：生成一个 `api` 作用域的 personal access token，
  然后 `POST /projects/:id/merge_requests/:iid/notes`。

不想给凭据就按上面手动走，三个文件十分钟内能贴完。
