# GitHub 仓库页面设置

仓库元数据（Description、topics）不通过 git 传输，git 无法设置。
需要你在网页端粘贴以下内容。

仓库地址：https://github.com/uuuconfig/oz-injector

---

## Description

粘到仓库页右侧栏的 **Description** 输入框：

```
Standalone x64 DLL injector with manual-map and LoadLibrary modes. Win32 + GDI UI, no Qt, no vcpkg, no runtime dependencies; ~250 KB single-file exe that builds in seconds. Engine is UI-independent and reusable, with a one-shot --inject mode for scripting.
```

上面那行是单行版本（Description 不支持换行）。下面这版分行更易读，
如果你的界面允许用 `<br>` 就用这个：

```
Standalone x64 DLL injector with manual-map and LoadLibrary modes.<br><br>Win32 + GDI UI, no Qt, no vcpkg, no runtime dependencies; ~250 KB single-file exe that builds in seconds. Engine is UI-independent and reusable, with a one-shot --inject mode for scripting.
```

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

## About 这栏不用填

勾选或留空都行。勾上 "Include a link to the website" 的话需要一个网址，
本项目没有独立站点，所以留空更合适。

---

## 做完之后

仓库页会显示 Description、topics 和 README 渲染效果。README 里的
`docs_ui_small.png` 会作为主图显示在页面顶部。

可选：把 `build/Release/oz_injector.exe` 挂到 Release 上（gh release create
或网页端 upload），这样别人不用克隆整个仓库就能直接下载二进制。
