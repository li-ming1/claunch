# claunch

单文件、零第三方依赖、零 C 运行库的 Windows 桌面格子启动器（x64 / Win10、11）。

一个 4889 行的 `claunch.c` + 一个 115 行的汇编桩，编出来 70KB的 exe，
导入表里只有 7 个系统 DLL。桌面格子这一件事它全做完了，多一行运行时依赖都不加。

## 特性

- **密集格子**：正方形槽位模型。拖放即与落点互换、清空不挪其他格、可插入/删除格子、
  窗口铺满重排、边缘可拉伸。
- **分组自治**：默认"常用"，可增删改名；拖行排序决定滚轮切换顺序；每个分组各自记忆滚动位置。
- **拖进来就能用**：exe / .lnk / 文件夹，以及"回收站""此电脑"等 shell 虚拟项（OLE 拖放）。
  虚拟项图标状态化（回收站空/满，唤醒时与桌面对齐）。单击启动，标题栏闪现结果；
  启动时自动清理失效项。
- **四条唤醒路径**：托盘图标、全局快捷键（默认 Ctrl+Alt+K，托盘菜单可改，被占用时保留原键）、
  双击桌面空白、按住左键把光标拖进面板。
- **键盘全通**：方向键移动悬停、回车/数字键启动、Del 清格、Home/End/PageUp/PageDown 翻页、
  Ctrl+滚轮缩放（40~160，记忆）、键入即跨分组搜索（前缀 > 词首 > 子串 > 拼音首字母，`jsb` → 记事本）、
  Ctrl+C/V 复制粘贴悬停格。
- **静默启动**：直接进托盘，失效清理延到首次唤醒；开机自启写 HKCU Run。

## 内存与体积

这是本项目真正在优化的东西，全部实测：

| 状态 | 模块数 | total WS | 私有提交 |
|---|---|---|---|
| 托盘（静默） | 23 | 0.37 MB | 1.75 MB |
| 唤醒可见 | 31~33 | ~11 MB | 2.5 MB |

镜像 71,680 字节；`dumpbin /dependents` 里没有任何 CRT dll。
对照同场景的 CLaunch 4.10：62 个模块、24.8 MB 私有提交。

> 口径提醒：`total WS` 含共享映像页（唤醒时拉进来的是一串系统 DLL 的共享页），
> 任务管理器"内存"列看的是**私有工作集**，不含它们。比内存请认准私有口径。

做到这个量级靠的是几条红线（细节都写在 [claunch.c](claunch.c) 顶部注释里）：

- 不用 `SHGetFileInfo` 取图标（会拉整条 shell 图标管线）；`.lnk` 按 MS-SHLLINK 自己最小解析。
- 常规启动走 `CreateProcessW`，不用 `ShellExecuteW`（会拉 WinRT 栈且常驻不卸）。
- 主窗断开 IME 关联，避免输入法被注入进程。
- OLE 初始化与图标管线延迟到首次唤醒：挂托盘时不付 COM 的钱。
- 面板隐藏后裁剪工作集、释放离屏缓冲。
- **不链 libcmt/ucrt**：宽字符原语、格式化核、配置读写、入口点、`/GS` cookie、`mem*` 全自实现。

## 构建

需要 Visual Studio 2022（C++ 工具集）+ Windows SDK。仓库**不带构建脚本**
（本地脚本写死了本机 VS 路径，不入库），在 *x64 Native Tools Command Prompt* 里跑两行：

```bat
ml64 /nologo /c /Fo claunch_chkstk.obj claunch.asm
cl /nologo /std:c11 /W4 /O1 /GS /GL /utf-8 /D CLAUNCH_NOCRT claunch.c ^
    /link /NODEFAULTLIB /ENTRY:wWinMainCRTStartup /SUBSYSTEM:WINDOWS ^
    /LTCG /OPT:REF /OPT:ICF /MANIFEST:EMBED /MANIFESTINPUT:claunch.manifest /OUT:claunch.exe ^
    claunch_chkstk.obj kernel32.lib user32.lib shell32.lib gdi32.lib imm32.lib advapi32.lib ole32.lib
```

CI 跑的就是这两行，并额外断言导入表里没有 CRT、跑一遍 `mem*` 自检：见
[.github/workflows/build.yml](.github/workflows/build.yml)。

> `ml64` 的对象名要写成 `/Fo claunch_chkstk.obj`（**空格**，不是 `/Fo:…`）。
> 冒号形式在这里会静默不产出文件且返回 0。

## 使用

1. 双击 `claunch.exe`，静默进托盘。
2. `Ctrl+Alt+K`（或双击桌面空白）唤出面板，面板在光标处弹出。
3. 把 exe / 快捷方式 / 文件夹 / 回收站拖进格子。
4. 单击启动；`Del` 清格；托盘图标右键是菜单（显示面板 / 改快捷键 / 开机自启 / 退出）。

## 配置

`claunch.ini`（UTF-8）放在 exe 同目录，程序自己原子读写（先写 `claunch.tmp` 再改名替换，
写一半断电不会撕坏配置）。手写参考 [claunch.ini.example](claunch.ini.example)。

| 键 | 含义 |
|---|---|
| `Rect=x y w h` | 窗口矩形（物理像素，DPI 变化时按它还原） |
| `Pinned=0/1` | 置顶（常驻、不自动隐藏） |
| `Hotkey=<mods> <vk>` | 唤醒快捷键，mods 位：1=Alt 2=Ctrl 4=Shift 可相加；vk 是虚拟键码 |
| `Group=<n>` | 打开时显示的分组下标（0 基） |
| `Cell=<40..160>` | 格子基准尺寸（Ctrl+滚轮的记忆值） |
| `[Groups] G0…` | 分组名，编号必须从 0 连续 |
| `[Items] I0…` | 条目，五段：`名|路径|组|参数|提示`（后三段可省） |

## 测试

`tmp/` 是一套纯 Win32 探针（无框架、无外部依赖），每个探针拉起被测 exe、注入输入，
对照可观测事实（像素哈希 / 句柄数 / 模块表 / 配置文件字节 / 计数）给结论：

- **行为回归**：`kbtest`（像素哈希）、`dragwake`、`dropchk`、`probe2`、`soak`、`starttime`
- **量化光源**：`zoomcount`、`wheel2`、`wakecost`、`hooktime`、`mods`、`watch`、`floor`
- **单元测试**：`nametest`、`libtest`、`fmtfmt`、`parsefuzz`、`memtest`

`memtest` 是 `mem*` 四件套的对拍自检（217 例，含 `memmove` 重叠双向），
也是唯一不链 CRT 的探针；CI 里跑它，其余探针要本机手动跑。
用法与判据见 [tmp/README.md](tmp/README.md)。

## 许可

MIT，见 [LICENSE](LICENSE)。
