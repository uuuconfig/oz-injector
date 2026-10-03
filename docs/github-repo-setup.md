# GitHub 仓库页面设置

仓库元数据（Description、topics）不属于 git 的数据模型，无法通过 commit 设置。
需要你在网页端粘贴以下内容。

仓库地址：https://github.com/uuuconfig/oz-injector

---

## Description

粘到仓库页右侧栏的 **Description** 输入框：

```
Standalone x64 DLL injector with manual-map and LoadLibrary modes. Win32 + GDI UI, no Qt, no vcpkg, no runtime dependencies; ~250 KB single-file exe that builds in seconds. Engine is UI-independent and reusable, with a one-shot --inject mode for scripting.
```

上面那行是单行版本（Description 框不支持换行）。下面这版分行更易读，
如果你的界面允许用 `<br>` 就用这个：

```
Standalone x64 DLL injector with manual-map and LoadLibrary modes.<br><br>Win32 + GDI UI, no Qt, no vcpkg, no runtime dependencies; ~250 KB single-file exe that builds in seconds. Engine is UI-independent and reusable, with a one-shot --inject mode for scripting.
```

> Description 保持英文。GitHub 的仓库页是面向全球的，英文的传播范围更广，
> 也不容易在搜索时被中文关键词漏掉。

---

## Topics

粘到 **Edit topics**（右侧栏 "Topics" 那一项）：

```
injection
dll-injection
manual-mapping
windows
win32
win32-api
reverse-engineering
debugging
c-plus-plus
injection-technique
```

---

## About 这一栏留空

勾选或留空都行。勾上 "Include a link to the website" 的话需要一个网址，
本项目没有独立站点，所以留空更合适。

---

## 中文 README 已经处理好

仓库的 `README.md` 已经是中文了，GitHub 会自动识别并在页面顶部渲染，
同时保留一份英文版（`README.en.md`）方便国际访问者阅读，在页面右侧栏
**Languages** 里可以切换。

---

## 可选：挂一个 Release

把 `build/Release/oz_injector.exe` 挂到 Release 上，这样别人不用 clone
整个仓库就能直接下载二进制。在网页端进入 **Releases → Draft a new release**，
新建一个 tag（如 `v1.0.0`），把 exe 拖进去即可。

需要的话也可以给我一个有 `repo` 权限的 PAT，我用 API 上传。
