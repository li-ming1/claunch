# tmp/ — claunch 探针与回归套件

每个 `.c` 是一个独立探针：拉起 `claunch.exe`、注入输入、对照**可观测事实**
（像素哈希 / 句柄数 / 模块表 / 配置文件字节 / 计数）给结论。全都纯 Win32，
无框架无外部依赖。

## 构建

```bat
cd tmp
buildall.bat                 :: 全部
buildall.bat kbtest          :: 只构建一个（去掉 .c）
```

产物 `*.exe / *.obj / *.pdb` 被根目录 `.gitignore` 覆盖，不入库。

## 跑法

绝大多数探针第一个参数是被测 exe 路径。**单实例互斥体的坑**：实例还活着时
新实例会静默退出，探针拿到的是旧窗口 —— 跑之前先 `tasklist | findstr
claunch.exe` 数一遍（用 PID 收尾，别用 `taskkill /IM`，大小写不敏感会
误杀用户正在跑的同名程序）。

```bat
tmp\claunch.exe                          :: 先启动被测（或让探针自己拉）
tmp\park.exe                             :: 像素比对前必跑：钉死光标
tmp\kbtest.exe tmp\claunch.exe           :: 键盘导航 + 五张像素哈希
tmp\soak.exe   tmp\claunch.exe 4         :: 句柄/工作集稳定性
tmp\probe2.exe tmp\claunch.exe           :: 自动隐藏
tmp\starttime.exe tmp\claunch.exe 3      :: 启动耗时
```

## 分类与用法

### 行为回归（每轮必跑）

| 探针 | 做什么 | 通过判据 |
|---|---|---|
| `kbtest` | 唤醒 → 截图 → End/Home/PageUp → 再截图，逐张算 FNV 哈希 | 六张哈希与**当前基线**一致 |
| `dragwake` | 按住左键把光标从面板外拖入面板 | 面板就地弹出；松开移出后自动隐藏 |
| `dropchk` | OLE 拖放注册状态 | `OleDropTargetInterface=SET` 且 `WS_EX_ACCEPTFILES=set` |
| `probe2` | 钉死变量测自动隐藏 | 移到面板外后按 3s 宽限 + 150ms 隐藏 |
| `soak` | 反复唤醒/隐藏，数 GDI/user 句柄与工作集 | 句柄不爬（基线 gdi82/user14，见下） |
| `starttime` | `CreateProcess` → 托盘就绪的墙钟 | min 值可复现（当前 ~110ms，噪声 ±30ms） |
| `traytip4` | 托盘悬停提示内容（Win11 XAML 弹层） | 差分检测通过 |

> **`kbtest` 的哈希跟活配置走**：`Rect=`（窗口几何）和 `Cell=`（格子尺寸）变了，
> 六张哈希就全变——这不是回归。历史哈希 `2C716E5C/978B520C/EA46015E` 属于
> `Rect=770 333 842 631` + `Cell=56`；你手动拉伸过面板之后基线就成了另一组
> （当前是 `1163700B/685DFC3B/12576AAB`，Rect=1239 389 540 631）。
> **判断标准是"本次改动前后一致"，不是"和历史一致"**——几何被人改过就重新记基线。
> 缩放类探针会把 `Cell=` 写坏，看到哈希集体变化先查配置，别怀疑代码。

### 配置写出回归

```bat
copy claunch.ini tmp\ref.txt            :: 拍快照
tmp\hold.exe claunch.exe 8000           :: 撑住实例
tmp\quit.exe                            :: 触发 CloseToTray 收尾保存
cmp tmp\ref.txt claunch.ini             :: 必须逐字节一致
```

`tmp\claunch.ini` 是探针自用的空配置种子（0 条目），与被测无关。

### 光源：性能量化（"能用计数就别用计时"）

这台机器上 `GetProcessTimes` 类计时噪声有 3~5 倍，**跨构建对比一律用计数**。

| 探针 | 测量什么 | 结论口径 |
|---|---|---|
| `zoomcount` | Ctrl+滚轮手势里 `IconFor` 的解析次数（靠窗口标题把计数暴露出来） | 确定性差值，如 276→20 |
| `zoomtime` | 缩放 N 格的 CPU 增量 | 仅看量级 |
| `wheel2` | 整格区重绘 / 换格悬停的每次成本（需可滚动压测床） | 0.32ms/帧 |
| `wakecost` | 首次唤醒（图标全解析）vs 再次唤醒（已缓存） | 拆出图标链一次开销 ~15ms |
| `hooktime` | 全局钩子每鼠标移动事件的成本 | ~6µs/次（LL 钩子固有成本） |
| `mods` | 托盘态 / 唤醒后加载了哪些模块、映射合计 | 验证"管线延迟到唤醒" |
| `watch` | 每 100ms 采样 WS/GDI/可见性 | 启动后再无增长 |
| `floor` | "什么也不干"的托盘应用（地板价） | 纯 Win32 对照基线 |

`wheel2` / `zoomcount` 需要可滚动压测床（128 条目 + `Cell=72`）：把 exe 复制到
一个空目录，按同样的键写一份 128 行 `I%d=程序%02d\|C:\Windows\System32\notepad.exe\|0\|\|`
的 `claunch.ini`。**别拿用户自己的配置跑缩放类探针**——它会把 `Cell=` 写坏，
kbtest 的像素哈希就全变了（这坑踩过三次，每次都得从快照恢复）。

### 功能/子系统工具

| 探针 | 用途 |
|---|---|
| `hkhold` | 注册一个组合键并占用 N 秒，测 claunch 的"快捷键被占用"路径 |
| `traytip4` | 托盘提示内容 |
| `binicon` / `binicon.c` 缺失时勿用 | 图标位深 |
| `flyouts` 已删 | — |
| `rdlinetest` | `RdLine` 行缓冲边界单元测试 |
| `test_move` | `MoveItem` 索引轮转自检 |
| `verify` | 功能冒烟（纯 Win32） |

### 单元测试（对拍，无 UI）

| 探针 | 对拍内容 | 通过判据 |
|---|---|---|
| `nametest` | `NameFromPath` 新旧实现逐例 | 0 例差异 |
| `libtest` | `wcpy/wcat` 对 `wcsncpy_s/wcscat_s`；`S_` 对 `MulDiv` | 24012 例 0 差异 |
| `fmtfmt` | `wfmt/wToI/wScan*` 对 `swprintf/_wtoi/swscanf` | 65602 例 0 差异 |
| `parsefuzz` | 三个"吃外来数据"入口的对抗性测试（见下） | 容纳不变式 + 存读往返全过 |
| `memtest` | `..\claunch.asm` 的 mem* 四件套对朴素参考实现（含 memmove 重叠双向、哨兵越界） | 217 例 0 差异 |

> `memtest` 是**唯一不链 CRT 的探针**（链 CRT 就会测到 CRT 的 memcpy，永远假通过）。
> `buildall.bat` 对它特判走专用链接行，两种入口都支持（全量 / 指名 `memtest`）。

结果写 `tmp\<name>.txt`（避开控制台 orientation 干扰）。这几个是本轮格式化层 /
字符串层 / DPI 宏的安全网，改动那三处后必须重跑。

`parsefuzz` 直接 `#include "../claunch.c"`（把 `wWinMain` 改名避开入口冲突），
所以测的是**真代码而不是副本**，不拉起窗口：

- `ParseLnkBuf`：随机/畸形 .lnk 缓冲（头长越界、截断、标志全随机）——返回 TRUE
  的五个字段必须 NUL 收尾
- `RdLine`：任意字节流 × 任意 cch（含 1）——必须收尾、不许越界、哨兵字节不许动
- `LoadConfig`：缺字段/多字段/跳号索引/越界组号/非法 UTF-8/超长数字/BOM/随机字节
  ——之后条目分组不超上限、组号合法、每个字符串收尾
- **存读往返**：结构化配置 → `LoadConfig` → `SaveConfig` → 再 `LoadConfig`，
  条目数/条目名/组号/窗口尺寸必须一一还原（这条直接保护用户数据；

  真宿主窗是必须的——`SaveConfig` 的 Rect 来自 `GetWindowRect(g_hwnd)`，
  给假句柄会让它写出垃圾 Rect，往返断言就变成测脚手架自己）
- **覆盖计数器**是它的牙齿：`true 333 / RdLine 4967 / 47 条目 / 往返 101 轮带条目`
  全为 0 就说明什么都没测到（这坑踩过两次：`g_ini` 没设导致配置段空跑、
  往返写错了路径）

## 已知失效/已删（勿再找）

- `dlgtest`、`traymenu`：前者驱动不了菜单键盘（面板非前台），后者测的是
  第四十六轮删掉的 `Silent` 配置项
- `dragsim`：依赖外部已运行实例，跨桌面窗口不可见；已被自包含的 `dragwake` 取代
- `test_lnk`：`#include "../kk.c"` 早随改名失效

## 固定装置

- `中文测试子目录层级1..9/`：中文多层路径的 `.lnk`/路径解析夹具，别删
- `pintest.py`：拼音首字母映射的独立核对脚本
