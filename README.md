# oz injector

> [English](README.en.md) | 简体中文

一个独立的 x64 DLL 注入器。从 OpenZen 的 `native/loader` 中剥离并泛化而来：任意 DLL、任意进程、两种注入模式、暗色界面。

![oz injector 界面](docs_ui_small.png)

OpenZen 原版把一个硬编码的 payload（它自己的 `OpenZen.dll`）塞在 RCDATA 里，
只针对 `javaw.exe`。这个版本把 DLL 路径和目标进程都交给用户指定，
所以可以作为通用工具使用。

## 这是什么

一个**开发 / 调试工具**。它做的事情和 Windows 工具链里任何进程注入工具一样：
把一个 DLL 加载进你指定的、正在运行的 x64 进程里——要么走系统 loader，
要么自己映射镜像。

用途就是注入通常的用途：附加调试器、在活进程里跑插桩或诊断代码、
用 hook 观察行为。DLL 由你提供，这个工具只负责加载。

如果你不拥有目标进程、没有修改它的权限、或未获授权，那这个工具不适合你。
目标进程的所有者通常能在模块列表、句柄监视器或 EDR 里看到这次加载；
手动映射只是让它不出现在其中某一个视图里，**并不是从所有视图里消失**。

## 编译

```
build_msvc.bat            # Release，产物在 build\Release\oz_injector.exe
build_msvc.bat Debug
```

或者用 CMake（前提是生成器能找到编译器）：

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

之所以额外提供 `build_msvc.bat`：本工作区的沙箱拦截了 `reg.exe`，而
`vcvars64.bat` 依赖它——所以 CMake 的编译器探测在这里会失败。
这个批处理脚本自己设置 `INCLUDE`/`LIB`/`PATH`。它还会编译并运行一个 UI
自检，自检不过就让构建失败。

依赖：MSVC（VS 2022，实测 14.44）和 Windows SDK。**不需要 Qt、不需要
vcpkg、不需要任何第三方库**——界面是纯 Win32 + GDI，所以构建只要几秒，
而不是静态编译 Qt 需要的约 2 小时。

产物：`build\Release\oz_injector.exe`，约 250 KB，单文件，无运行时依赖。

## 使用

**图形界面**——直接运行：

1. 从列表里选一个进程（每秒自动刷新；显示窗口标题，便于区分同名的多个实例）
2. 选择模式：`Manual map` 或 `LoadLibrary`
3. 点 **Browse** 选一个 DLL
4. 点 **Inject**。日志区会显示每一步和最终结果

**一次性模式**——供脚本使用：

```
oz_injector.exe --inject <pid> <dll.dll> [--manual|--loadlibrary]
```

执行过程输出到 stderr；退出码 0 表示成功，1 表示失败。
`oz_injector.exe /?` 打印用法。

## 两种模式

| | Manual map | LoadLibrary |
|---|---|---|
| 出现在模块列表里？ | 否 | 是 |
| 原理 | 在目标内存里重定位 PE、修复导入、通过蹦床调用 `DllMain` | 远程 `LoadLibraryW` |
| DLL 必须可重定位 | 是 | 否 |
| 调试难度 | 较高 | 较低 |

手动映射让 DLL 对所有检查目标模块列表的东西都不可见。LoadLibrary 是经典做法——
更简单，而且对没有按可重定位要求构建的 DLL 也能用。

两种模式都要求目标进程的完整性级别不高于注入器。目标以管理员身份运行时，
注入器也要以管理员身份运行。

**手动映射的镜像无法卸载。** 对于一个 loader 从未注册的镜像，不存在
`FreeLibrary` 路径，所以它会在目标进程的整个生命周期内驻留。注入器会记录
本次会话中已注入的 pid，并拒绝对同一进程重复注入。

## 目录结构

```
src/oz_injector_core.h    引擎 API（不依赖 UI，不依赖 Qt）
src/oz_injector_core.cpp  引擎：枚举、校验、手动映射、LoadLibrary
src/oz_injector_ui.h/.cpp 窗口：进程列表、路径框、模式选择、日志
src/main.cpp              入口：GUI，以及 --inject 一次性路径
src/selftest.cpp          进程内 UI 自检，由构建脚本调用
src/render.cpp            在进程内部把 UI 导出为 PNG
res/                      图标、manifest（comctl32 v6、per-monitor DPI）、rc
test/                     端到端注入测试
```

引擎完全不知道窗口和消息的存在，所以可以被控制台工具或别的 UI 复用。

## 测试

**界面**——`build_msvc.bat` 在链接完成后会运行 `selftest.exe`。它创建真实的
窗口，断言每个子控件都创建成功、进程列表有数据、日志区有内容、点 Refresh
后会追加一行干净可读的文本、每个按钮都有标题，并且**滚动位置在定时刷新和
手动 Refresh 之后都保持不变**。共 18 项检查，全部通过。

按钮标题必须在 `render_ui.bat` 里做**像素级**校验，源码层的断言抓不到它：
`BS_OWNERDRAW` 按钮的标题一直都在，出问题的是 `WM_DRAWITEM` 画错了指针。
所以渲染器会统计按钮矩形内与背景填充色差异明显的像素占比——正确渲染会有
8%~18% 的文字像素，坏版本是 0%。

**注入**——

```
cd test
build_test.bat
python e2e_test.py
```

它启动一个探针宿主进程，用两种模式分别注入一个探针 DLL，每种模式断言两件事：
`DllMain` 确实执行了（DLL 会把自己的镜像基址写进标记文件），
以及该模块出现在（或不出现在）目标进程的模块列表里。

最近一次运行：**2/2 通过。**

| 模式 | DllMain 基址 | 在模块列表里 | 预期 |
|---|---|---|---|
| `--manual` | `0x24921AA0000` | 否 | 不应出现 |
| `--loadlibrary` | `0x7FFBE93C0000` | 是 | 应出现 |

这两个记录下来的基址也说明了为什么引擎不信任 `GetExitCodeThread` 返回的
64 位 `HMODULE`：注入器观测到的是 `0xAC0B0000`，而 DLL 自己看到的是
`0x7FFBAC0B0000`。

## 已知限制

- 仅支持 x64。32 位的目标或 DLL 会被拒绝，报 `WrongArchitecture`
- 手动映射需要 `IMAGE_DIRECTORY_ENTRY_BASERELOC`；用 `/DYNAMICBASE:NO` 构建的
  DLL 会被拒绝，报 `RelocationUnsupported`
- 依赖 loader 已注册模块的 TLS 回调和静态初始化，在手动映射下行为可能不同
- 导入解析会遍历目标进程的模块列表，缺失的项再回退到远程 `LoadLibraryW`
- **日志区是普通 `EDIT` 控件，不是 RichEdit 也不是自绘列表框。** 前面两种方案
  在这台机器上都是错的：
  - RichEdit 会忽略 `EM_SETBKGNDCOLOR` 和 `WM_CTLCOLOREDIT` 对背景色的处理，
    所以日志区一直是白的，无法做成暗色
  - `LBS_OWNERDRAWFIXED` 列表框不支持 `LB_SETITEMDATA`（导致
    `DRAWITEMSTRUCT.itemData` 未定义），而且 `LB_ADDSTRING` **永远只接受
    ANSI 字符串**。在中日韩代码页下（本机 ACP = 936）所有非 ASCII 字符都会
    变成乱码——`──` 分隔符渲染成了垃圾。`EM_REPLACESEL` 接受宽字符串，
    所以换成 `EDIT` 就绕开了全部这些问题
- `render_ui.bat` 是在拥有窗口的进程内部截图。外部的窗口枚举器看不到本沙箱的
  GUI 会话，所以像素必须由持有窗口的那个进程自己去读

## 发布 / 推送

`publish.bat` 会在需要时创建 GitHub 仓库，然后推送 `main`：

```
publish.bat            # 交互式，提示输入 token
publish.bat <token>   # 非交互式
```

它需要 PATH 上有 `git`，以及一个具备写权限的 token（classic PAT 勾 `repo`
scope，或 fine-grained PAT 勾 Contents: read+write）。token 通过 remote URL
传递，不写进 `.git/config`，因此不会落盘。

注意：仓库本身需要由人来创建。编写本文时所用的 GitHub connector 是一个
缺少 Administration 权限的 GitHub App，`POST /user/repos` 会返回
`403 Resource not accessible by integration`——`publish.bat` 两种情况都处理了
（token 有权限时就建仓，然后无论如何都推送）。
