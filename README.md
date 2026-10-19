简体中文 | [English](README.en.md)

# claunch

单文件、零第三方依赖、零 C 运行库的 Windows 桌面格子启动器（x64 / Win10、11）。

一个 4889 行的 `claunch.c` 加一个 115 行的汇编桩，编译产物约 70 KB，
导入表中仅 7 个系统 DLL。专注桌面格子启动器这一件事，不引入任何额外的运行时依赖。

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

这是本项目重点优化的指标，以下均为实测数据：

| 状态 | 模块数 | total WS | 私有提交 |
|---|---|---|---|
| 托盘（静默） | 23 | 0.37 MB | 1.75 MB |
| 唤醒可见 | 31~33 | ~11 MB | 2.5 MB |

镜像 71,680 字节；`dumpbin /dependents` 输出中不含任何 CRT DLL。

> 说明：`total WS` 含共享映像页（唤醒时载入的系统 DLL 共享页），
> 任务管理器"内存"列显示的是**私有工作集**，不含它们。比较内存占用请以私有工作集为准。

达到这一量级依托以下设计约束（细节见 [claunch.c](claunch.c) 顶部注释）：

- 不用 `SHGetFileInfo` 取图标（避免引入整条 shell 图标管线）；`.lnk` 按 MS-SHLLINK 自行最小解析。
- 常规启动走 `CreateProcessW`，不用 `ShellExecuteW`（后者会引入 WinRT 栈且常驻不卸载）。
- 主窗断开 IME 关联，避免输入法被注入进程。
- OLE 初始化与图标管线延迟到首次唤醒：静默驻留阶段不承担 COM 初始化开销。
- 面板隐藏后裁剪工作集、释放离屏缓冲。
- **不链 libcmt/ucrt**：宽字符原语、格式化核、配置读写、入口点、`/GS` cookie、`mem*` 全自实现。

## 构建

需要 Visual Studio 2022（含 C++ 工具集）与 Windows SDK。在
*x64 Native Tools Command Prompt* 中执行：

```bat
ml64 /nologo /c /Fo claunch_chkstk.obj claunch.asm
cl /nologo /std:c11 /W4 /O1 /GS /GL /utf-8 /D CLAUNCH_NOCRT claunch.c ^
    /link /NODEFAULTLIB /ENTRY:wWinMainCRTStartup /SUBSYSTEM:WINDOWS ^
    /LTCG /OPT:REF /OPT:ICF /MANIFEST:EMBED /MANIFESTINPUT:claunch.manifest /OUT:claunch.exe ^
    claunch_chkstk.obj kernel32.lib user32.lib shell32.lib gdi32.lib imm32.lib advapi32.lib ole32.lib
```

CI（[.github/workflows/build.yml](.github/workflows/build.yml)）执行相同的命令，
并额外断言导入表中不含 CRT 依赖、运行 `mem*` 自检。

> `ml64` 的对象名要写成 `/Fo claunch_chkstk.obj`（**空格**，不是 `/Fo:…`）。
> 冒号形式在这里会静默不产出文件且返回 0。

## 使用

1. 双击 `claunch.exe`，静默进托盘。
2. `Ctrl+Alt+K`（或双击桌面空白）唤出面板，面板在光标处弹出。
3. 把 exe / 快捷方式 / 文件夹 / 回收站拖进格子。
4. 单击启动；`Del` 清格；托盘图标右键是菜单（显示面板 / 改快捷键 / 开机自启 / 退出）。

## 配置

`claunch.ini`（UTF-8）放在 exe 同目录，程序自己原子读写（先写 `claunch.tmp` 再改名替换，
写一半断电不会撕坏配置）。各键含义如下：

| 键 | 含义 |
|---|---|
| `Rect=x y w h` | 窗口矩形（物理像素，DPI 变化时按它还原） |
| `Pinned=0/1` | 置顶（常驻、不自动隐藏） |
| `Hotkey=<mods> <vk>` | 唤醒快捷键，mods 位：1=Alt 2=Ctrl 4=Shift 可相加；vk 是虚拟键码 |
| `Group=<n>` | 打开时显示的分组下标（0 基） |
| `Cell=<40..160>` | 格子基准尺寸（Ctrl+滚轮的记忆值） |
| `[Groups] G0…` | 分组名，编号必须从 0 连续 |
| `[Items] I0…` | 条目，五段：`名|路径|组|参数|提示`（后三段可省） |

## 许可

MIT，见 [LICENSE](LICENSE)。
