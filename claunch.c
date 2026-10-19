// claunch.c — 桌面格子：单文件、零第三方依赖的桌面启动器
// 分组完全用户自治（默认"常用"，可删可改名、行可拖拽排序决定滚轮切换顺序，
// 无内置"全部"聚合视图）；正方形密集格子（槽位模型：拖放即与落点互换、
// 清空不挪其他格、可插入/删除格子、窗口铺满重排、边缘可拉伸）。
// 拖程序进格子单击启动（标题栏闪现启动结果）、启动时自动清理失效项目、
// 悬停顶部居中显示名称、移出自动隐藏、置顶二合一按钮、双击桌面空白唤醒、
// 快捷键唤醒（默认 Ctrl+Alt+K，托盘菜单可改，冲突保留原键）。
// 静默启动是唯一行为：直接进托盘，失效清理延到首次唤醒。
// 桌面/资源管理器的文件与回收站等 shell 虚拟项均可拖入（OLE 拖放）：
// 落格子=落位、落分组行=入该组、边缘悬停自动滚动；
// 虚拟项图标状态化（回收站空/满），唤醒时重取与桌面同步。
// 键盘：方向键移动悬停、回车/数字键启动、Del 清格、Home/End/PageUp/PageDown 翻页、
// Ctrl+滚轮缩放（40~160 记忆）；键入即跨分组搜索（前缀>词首>子串>拼音首字母>
// 路径排序，jsb→记事本），回车启动最优，Esc 清空。Ctrl+C/V 复制/粘贴悬停格。
// 托盘菜单：显示面板 / 设置唤醒快捷键 / 开机自启（写 HKCU Run）/ 退出。
// 内存红线（实测）：配置流式读写、图标按需加载、SHGetFileInfo 只作兜底、
// .lnk 解析 + CreateProcess 直启替代 ShellExecute、主窗断开 IME、
// COM/图标管线延迟到唤醒——破防会拖进 15~30MB 外部模块链。
// 绘制走整窗离屏缓冲：直接绘制在快速滚动时露"画一半"中间帧，频闪。
// 运行时零依赖：不链 libcmt/ucrt（build.bat 里 /NODEFAULTLIB + /D CLAUNCH_NOCRT）。
// 宽字符原语、格式化核、配置读写全是自实现；入口、/GS cookie 与两个编译器桩
// （chkstk.asm）也自己来。量化收益见 build.bat 注释
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS
#define COBJMACROS              // C 侧 COM 便捷宏（IDataObject_GetData 等）
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#include <windows.h>
#include <shellapi.h>
#include <ole2.h>        // OLE 拖放：IDropTarget/RegisterDragDrop（ole32，唯一新增依赖）
#include <shlobj.h>
#include <commdlg.h>
#include <imm.h>
#include <stdarg.h>      // 只有 varargs 还来自 CRT：格式化本身走自带的 wfmt
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "advapi32.lib")   // 开机自启的注册表操作；advapi32 本就经 shell32 间接映射，无新增 DLL
#pragma comment(lib, "ole32.lib")      // OLE 拖放（回收站等虚拟项拖入）


// ---------------------------------------------------------------- 宽字符原语
// 自己实现的几个：wcslen/wcscmp/wcschr/wcsrchr/wcsstr 在 ucrt 里都拖着整条
// 宽字符/locale 支持，而这个程序只需要"取长度/比较/找字符"，全是几行循环。
// 命名加下划线后缀以免与 CRT 同名（将来若链回 CRT 也不会撞）
__declspec(noinline) size_t wlen_(const WCHAR *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

__declspec(noinline) WCHAR *wchr_(const WCHAR *s, WCHAR c)
{
    for (; *s; s++) if (*s == c) return (WCHAR *)s;
    return NULL;
}

__declspec(noinline) WCHAR *wrchr_(const WCHAR *s, WCHAR c)
{
    WCHAR *last = NULL;
    for (; *s; s++) if (*s == c) last = (WCHAR *)s;
    return last;
}

__declspec(noinline) int wcmp_(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)*a - (int)*b;
}

__declspec(noinline) WCHAR *wstr_(const WCHAR *h, const WCHAR *nd)
{
    if (!*nd) return (WCHAR *)h;
    for (; *h; h++) {
        const WCHAR *a = h, *b = nd;
        while (*b && *a == *b) { a++; b++; }
        if (!*b) return (WCHAR *)h;
    }
    return NULL;
}

// ---------------------------------------------------------------- 常量
#define MAX_GROUPS      16
#define MAX_ITEMS       128
#define APP_CLASS       L"ClaunchLauncherWnd"
#define DLG_CLASS       L"ClaunchDialogWnd"
#define APPNAME         L"claunch"
#define ININAME         L"claunch.ini"

#define WM_APP_WAKE     (WM_APP + 1)
#define WM_APP_WHEEL    (WM_APP + 3)   // 低级钩子转发的滚轮（面板置顶且未聚焦时也可用）
#define WM_APP_WAKE_CAND (WM_APP + 4)  // 钩子转发的“疑似桌面双击”候选，UI 线程确认后再唤醒
#define WM_APP_DRAGWAKE  (WM_APP + 5)  // 钩子转发的“拖动进入面板静置区”：面板就地弹回（不抢前台）
#define TIMER_HIDE      1
#define TIMER_FADE      2
#define TIMER_DRAGSCROLL 3   // 拖放悬停网格边缘的自动滚动
#define TIMER_ICONS     4    // 缩放手势停稳后重解析图标
#define HOTKEY_ID       1
#define CAP_CLASS       L"ClaunchCapWnd"     // 热键捕获对话框类

// 时间与容差（毫秒 / 像素）：唤醒行为的一组参数，集中在这里调
#define WAKE_GRACE_MS   3000   // 唤醒后光标不在窗口上的宽限：误唤醒不会立刻隐去
#define HIDE_DELAY_MS   150    // 光标移出多久开始淡出
#define FLASH_MS        1200   // 标题栏反馈消息的停留时间
#define CURSOR_SLOP     2      // 光标在窗口判定上的外扩：贴边时不“卡”在边缘

// 标题栏按钮编码（同 g_hoverBtn / g_pressBtn）：1=钉住 2=关闭
#define BTN_PIN         1
#define BTN_CLOSE       2

// 格子缩放与图标尺寸的边界（Ctrl+滚轮的配置记忆值 40~160）
#define CELL_BASE_MIN   40
#define CELL_BASE_MAX   160
#define ICON_SIZE_MIN   12
#define ICON_SIZE_MAX   64

#define IDM_LAUNCH      1001
#define IDM_ADMIN       1002
#define IDM_LOC         1003
#define IDM_INS_SLOT    1004
#define IDM_DEL_SLOT    1005
#define IDM_CLR_SLOT    1006
#define IDM_PROPS       1007
#define IDM_GRP_RENAME  1008
#define IDM_GRP_DEL     1009
#define IDM_GRP_NEW     1011
#define IDM_EXIT        1010

// 托盘
#define TRAY_ID             1
#define WM_TRAY             (WM_APP + 2)   // 托盘回调消息
#define IDM_TRAY_SHOW       1101
#define IDM_TRAY_PIN        1102
#define IDM_TRAY_AUTO       1103
#define IDM_TRAY_HOTKEY     1105

#define DLG_OK          1
#define DLG_CANCEL      2
#define DLG_BROWSE      100      // 浏览按钮：100 + 字段序号
#define ID_EDIT_BASE    1000     // 输入框：1000 + 字段序号
// 注意：输入框与浏览按钮的 ID 段必须分开。输入框会发 EN_* 通知，
// 若与浏览按钮 ID 相同，打开对话框的瞬间就会被当成“点击浏览”而弹出文件选择框。

// 配色（暗色，格子可见边框）
#define C_BG        RGB(0x2B, 0x2D, 0x31)
#define C_SIDE      RGB(0x23, 0x25, 0x28)
#define C_CELL      RGB(0x38, 0x3B, 0x40)
#define C_CELL_HI   RGB(0x47, 0x4B, 0x53)
#define C_LINE      RGB(0x4D, 0x51, 0x59)
#define C_ACCENT    RGB(0x3D, 0x7B, 0xE7)
#define C_ACCEL     RGB(0x35, 0xC4, 0x6A)   // 选中分组的高亮色（与参考实现一致：绿）
#define C_TEXT      RGB(0xEC, 0xED, 0xEF)
#define C_DIM       RGB(0x9C, 0x9F, 0xA5)
#define C_PANEL     RGB(0x33, 0x35, 0x3A)
#define C_CLOSE     RGB(0xC4, 0x2B, 0x2B)   // 关闭键悬停底：Windows 标题栏惯例暗红
#define C_CLOSE_PR  RGB(0x9C, 0x22, 0x22)   // 关闭键按下底：深一档
#define C_BTN_PR    RGB(0x41, 0x44, 0x4B)   // 钉住键按下底：比悬停底深一档

// ---------------------------------------------------------------- 数据
// 字段按访问热度排列：绘制只碰 name/icon/group/ownIcon/iconTried（同一缓存行），
// 启动才碰 path，属性对话框/悬停才碰 args/tip——每帧少跨缓存行。
typedef struct {
    WCHAR name[48];        // 热：每格绘制
    HICON icon;            // 热：每格绘制
    int   group;           // 热：视图过滤
    BOOL  ownIcon;         // 热：图标生命周期
    BOOL  iconTried;       // 热：按需加载哨兵
    WCHAR path[MAX_PATH];  // 温：启动 / 失效清理
    WCHAR args[192];       // 冷：仅属性对话框
    WCHAR tip[96];         // 冷：仅悬停文字
} Item;

typedef struct { WCHAR name[48]; } Group;

// 剪贴板条目：只含可序列化的文本字段。组号不复制——粘到哪格就入哪组；
// icon/ownIcon/iconTried 是运行期字段，不进剪贴板
typedef struct {
    WCHAR name[48];
    WCHAR path[MAX_PATH];
    WCHAR args[192];
    WCHAR tip[96];
} ClipItem;

static Item  g_items[MAX_ITEMS];
static int   g_nItems = 0;
static Group g_groups[MAX_GROUPS];
static int  g_nGroups = 0;

// 当前视图：当前分组条目的全局下标（按序）。条目增删移/换组/切组才失效重建，
// 之后 ViewCount/ItemAtView 都是 O(1) 读表——绘制、悬停、滚轮每帧都走这条路。
static int  g_viewIdx[MAX_ITEMS];
static int  g_viewN = 0;
static BOOL g_viewValid = FALSE;

// ---------------------------------------------------------------- 状态
static HINSTANCE g_inst;
static HWND      g_hwnd;
static int       g_dpi = 96;

static HFONT  g_font, g_fontT;
static HBRUSH g_brBG, g_brSide, g_brCell, g_brCellHi, g_brLine, g_brAccent, g_brAccel, g_brPanel;
static HPEN   g_penLine;              // 格子/卡片边框（1px C_LINE）
static HPEN   g_penAccent;            // 格子悬停/拖动焦点环（1px C_ACCENT）
// 标题栏按钮的图形对象（缓存，见 MakeGdiObjects）：钉住三态填色 +
// 字形提亮 + 两枚按下底。字形全部走填充（多边形/椭圆），不用 pen
static HBRUSH g_brPinOff, g_brPinOn, g_brGlyph, g_brBtnPress, g_brClose, g_brClosePr;
static int    g_cw, g_ch;
static HDC    g_memDC;          // 离屏缓冲：WM_SIZE 重建，WM_PAINT 画进去再整体上屏
static HBITMAP g_memBmp;
static BOOL   g_memFresh;       // 缓冲重建后第一帧必须整窗重画

static int  g_titleH = 34, g_sideW = 104, g_gap = 3, g_cell = 72, g_grpH = 34;   // 格子正方形
// 侧栏分行几何：ScaleMetrics 随 DPI 一次算好，五个函数共用（原来裸 6/3/12
// 散落五处且不随 DPI 缩放）
static int  g_grpPadX, g_grpPadTop, g_grpRowGap, g_grpPadY, g_grpSelBar;
static int  g_cols = 1;    // 列数：只随窗口宽度变（WM_SIZE→UpdateCell），命中/绘制路径缓存读
static int  g_slots = 1;   // 可视格数=列×行：同出处缓存，命中/绘制/滚动钳制直接读
static int  g_cellBase = 56;   // 格子目标宽基准：Ctrl+滚轮调节，配置持久化（UpdateCell 据此算列数）
static BOOL g_pinned = FALSE;    // 置顶：常驻不自动隐藏 + HWND_TOPMOST（原“定住”与“置顶”合并）
static BOOL g_needPrune;         // 静默启动把失效清理延到首次唤醒：托盘就绪不该等磁盘
static BOOL g_firstRun;          // 无配置文件：首跑给一条引导气球（可发现性）
// 唤醒快捷键（用户可配置，默认 Ctrl+Alt+K；配置行 Hotkey=<mods> <vk>）。
// mods 位：1=Shift 2=Ctrl 4=Alt。Win 组合不提供——那是系统的地盘
static UINT g_hotMods = MOD_CONTROL | MOD_ALT;
static UINT g_hotVk = 'K';
static BOOL g_autostart = FALSE; // 开机自启：以 HKCU Run 键为准，启动时读入（不进配置）
static UINT g_clipFmt = 0;       // 私有剪贴板格式原子（Ctrl+C/V；注册失败则只用纯文本）
static BOOL g_cfgDirty = FALSE;  // 配置有待落盘：缩放等连续操作防抖，避免每格滚轮都写文件
static int  g_curGroup = 0;
static WCHAR g_filter[24];   // 名称搜索过滤词（瞬态，不持久化）：非空时视图只保留名称含它的条目
static int  g_scroll = 0;       // 格子区滚动（像素）
static int  g_grpScroll = 0;    // 侧栏滚动（像素）
static int  g_scrollMemo[MAX_GROUPS];   // 每个分组各自的格子滚动位：切组时保存/恢复

static int  g_hoverCell = -1, g_hoverGroup = -1, g_hoverBtn = 0; // btn: 1 钉 2 关
static WCHAR g_hoverText[80];
static BOOL  g_cardOn = FALSE;  // 悬停小卡片可见（悬停格子/分组时浮在其上方）
static RECT  g_cardRect;        // 卡片区域：悬停目标变化时重算，绘制只读
static WCHAR g_cardText[80];    // 卡片文字：程序名/组名
static WCHAR g_flashText[80];   // 标题栏启动反馈（"已启动/启动失败: 名"）
static ULONGLONG g_flashAt = 0;  // 反馈起始时刻；1.2s 后自然过期（0=无）。64 位时刻：无 49 天回绕

static int  g_pressCell = -1;   // 按下的格子：原地抬起=启动，拖过阈值=排序
static BOOL g_dragging;         // 已越过拖动阈值
static int  g_dropCell = -1;    // 拖动悬停的目标格（视图序号）
static BOOL g_dropIn;           // 目标在格子区内（区外释放=取消）
static BOOL g_extDrag;          // 外部 OLE 拖放悬停中：落点高亮与内部排序同源同款，
                                // 与内部拖动互斥（OLE 拖动时光标捕获在拖放源）
static BOOL g_oleOk;            // 当前 OLE 拖放的数据有可收格式（DragEnter 定，整场有效）
static DWORD g_oleAllow;        // 拖放源允许的效果集合（DragEnter 入参 *ef，整场有效）
static int  g_oleGrp = -1;      // 外部拖放悬停的侧栏分组行（落组目标；-1=不在分组上）
static int  g_oleAutoDir;       // 拖放自动滚动方向（-1 上 +1 下，0 停）
static POINT g_oleAutoPt;       // 自动滚动期间复算落点的最后光标（屏幕坐标）
static BOOL g_dropOn;           // IDropTarget 已注册主窗（销毁时 Revoke）
static BOOL g_oleInit;          // OleInitialize 已成功（销毁时配对 Uninitialize）
static UINT g_cfShellIdList;    // CFSTR_SHELLIDLIST 格式原子（0=注册失败：虚拟项不可收）
static int  g_pressGroup = -1;  // 按下的分组行（侧栏重排用）
static BOOL g_dragGroup;        // 分组重排中（已越过拖动阈值）
static int  g_dropGroup = -1;   // 分组重排的落点行
static POINT g_pressPt;
static BOOL g_hoverActive = FALSE;
static BOOL g_tracking = FALSE;

static ULONGLONG g_outSince = 0;
static ULONGLONG g_wakeAt = 0;
static int   g_alpha = 255, g_fadeDir = 0;
static BOOL  g_dlgOpen = FALSE;
static BOOL  g_menuOpen = FALSE;   // TrackPopupMenu 弹着（模态循环里 WM_TIMER 仍会派发：
                                   // 心跳里的 shell 调用此刻一律回避，防卡 UI）

static HHOOK g_hook = NULL;
static HANDLE g_hookH;              // 钩子专用线程：回调不再与绘制抢 UI 线程
static DWORD  g_hookTid;
static WCHAR g_ini[MAX_PATH];
static int   g_savedX, g_savedY, g_savedW, g_savedH;
static BOOL  g_applySavedRect = FALSE;

// 托盘
static HICON g_trayIcon = NULL;
static int   g_traySz = 0;      // 图标的绘制尺寸：DPI 变化时比对，变了才重建
static UINT  g_msgTaskbarCreated = 0;
static NOTIFYICONDATAW g_nid;

typedef BOOL (WINAPI *PFN_SetProcDpiCtx)(HANDLE);
static PFN_SetProcDpiCtx pSetDpi = NULL;

#define UNREF(x) ((void)(x))

// 对话框字段描述（格子属性/重命名/新建分组共用，RunDialog 消费）。
// 定义在这里是因为新建分组（NewGroup）在条目/分组节就要用它
typedef struct {
    const WCHAR *label;
    WCHAR *buf;
    int    cch;
    int    mode;      // 0 文本 / 1 带“浏览”按钮
} DlgField;

// 前向声明：只保留使用点早于定义的函数
static void ClampToWorkArea(void);
static void MoveToPoint(POINT pt);
static void BeginHide(BOOL force);
static void ReleaseMemBuffer(void);   // 双缓冲释放：隐藏即放，唤醒首绘重建
static void InitOleDrop(HWND h);   // 延迟到首次唤醒（见定义处注释：静默态不付 COM 的钱）
static void HotkeyName(WCHAR *out, int cch);   // 唤醒快捷键的可读名（定义在 OnCreate 区）
static void ModsName(WCHAR *out, int cch, UINT mods);   // 仅修饰键部分（捕获框预览用）
static BOOL ApplyHotkey(HWND h);                  // 注册/更换唤醒快捷键（原子替换语义）
static void SaveConfig(void);
static void InvalidateTitle(void);
static void InvalidateSidebar(void);
static void InvalidateCell(int vi);
static int  PopupAt(HMENU m);
static int  Rows(void);
static int  DefaultGroup(void);
static int  TargetGroup(void);
static void UpdateBtnRects(void);
static BOOL  UpdateHoverText(void);
static void RefreshCard(void);
static void UpdateTrayTip(void);
static BOOL RunDialog(HWND owner, const WCHAR *title, void *fields, int n);

// 条目/分组数据变化后的统一收尾：悬停文字重算、卡片重锚、整窗重绘、落盘。
// 六处数据变更路径（粘贴×2、槽位操作、分组重排、格子/分组改名）共用同一套收尾
static void SyncAndSave(void)
{
    UpdateHoverText();
    RefreshCard();
    InvalidateRect(g_hwnd, NULL, FALSE);
    SaveConfig();
}

// 关闭按钮 / WM_CLOSE：有托盘图标时只隐藏到托盘，托盘图标不可用才真正退出
static void CloseToTray(void)
{
    SaveConfig();
    if (g_trayIcon) BeginHide(TRUE);
    else DestroyWindow(g_hwnd);
}


static void ExitApp(void)
{
    SaveConfig();
    DestroyWindow(g_hwnd);
}

static HANDLE g_once;              // 单实例互斥体：进程生命周期持有，OnDestroy 收


// ---------------------------------------------------------------- 工具
// DPI 缩放：96 为基准、四舍五入（与 MulDiv 同语义，正数域逐值等价）。
// 原实现走 MulDiv——一次 kernel32 调用约 30 周期，而 PaintGrid 每格要问 5 次；
// 换成整数算式后 /96 被编译器折成乘移，调用点直接进寄存器
#define S_(v) (((v) * g_dpi + 48) / 96)

// 定长宽字符拷贝：截断且必带 NUL。替代 wcsncpy_s——后者每次调用都走 CRT 的
// 参数校验分支，热路径（悬停文字、每格字段）上纯属白付
static void wcpy(WCHAR *d, int cch, const WCHAR *s)
{
    int n = cch - 1;
    while (n > 0 && *s) { *d++ = *s++; n--; }
    *d = 0;
}

// 定长拼接：剩余空间内追加，永不越界
static void wcat(WCHAR *d, int cch, const WCHAR *s)
{
    int n = 0;
    while (n < cch - 1 && d[n]) n++;
    while (n < cch - 1 && *s) d[n++] = *s++;
    d[n] = 0;
}

// 大小写不敏感的整串比较，只折叠 ASCII。替代 _wcsicmp——后者走 locale 表
//（nlsdata/lcidtoname 一整条链，几十 KB），而这里的用途只有扩展名比对
static BOOL SameTextI(const WCHAR *a, const WCHAR *b)
{
    for (;; a++, b++) {
        WCHAR ca = *a, cb = *b;
        if (ca >= L'A' && ca <= L'Z') ca += 32;
        if (cb >= L'A' && cb <= L'Z') cb += 32;
        if (ca != cb) return FALSE;
        if (!ca) return TRUE;
    }
}

// ---------------------------------------------------------------- 格式化
// 全文件只需要 %s/%d/%u/%llX 四种形状，且都落在配置读写与提示文案上。自带一个
// 几十行的核替掉 libc 的 printf/scanf 整条链（cfout/cvt/input/atox + 宽字符转换
// 与 locale 数据），实测镜像 241152 → 174592 字节（−65KB，−27%），顺带让
// "写用户数据"这条路径不再经过不可控的格式解析。
// 契约：dst 恒 NUL 收尾、空间不足即截断、返回实际写入长度。
// 宽度前缀按补零处理（本文件只用 %02X 这种写法），l/h 修饰符读入即忽略。

static WCHAR *wNum(WCHAR *d, WCHAR *e, unsigned long long v, int base, int width, BOOL upper)
{
    WCHAR tmp[24];
    int n = 0;
    const WCHAR *dig = upper ? L"0123456789ABCDEF" : L"0123456789abcdef";
    do {
        tmp[n++] = dig[v % (unsigned)base];
        v /= (unsigned)base;
    } while (v);
    while (n < width && n < 24) tmp[n++] = L'0';
    while (n > 0 && d < e) *d++ = tmp[--n];
    return d;
}

static WCHAR *wStr(WCHAR *d, WCHAR *e, const WCHAR *s)
{
    while (d < e && *s) *d++ = *s++;
    return d;
}

static int wfmtv(WCHAR *dst, int cch, const WCHAR *fmt, va_list ap)
{
    WCHAR *d = dst, *e = dst + cch - 1;
    for (const WCHAR *p = fmt; *p; p++) {
        if (*p != L'%') { if (d < e) *d++ = *p; continue; }
        if (p[1] == L'%') { if (d < e) *d++ = L'%'; p++; continue; }
        p++;
        int width = 0;
        while (*p >= L'0' && *p <= L'9') width = width * 10 + (*p++ - L'0');
        while (*p == L'l' || *p == L'h') p++;
        switch (*p) {
        case L's': d = wStr(d, e, va_arg(ap, const WCHAR *)); break;
        case L'd': {
            long long v = va_arg(ap, int);
            if (v < 0) { if (d < e) *d++ = L'-'; v = -v; }
            d = wNum(d, e, (unsigned long long)v, 10, width, FALSE);
            break;
        }
        case L'u': d = wNum(d, e, va_arg(ap, unsigned), 10, width, FALSE); break;
        case L'X': d = wNum(d, e, va_arg(ap, unsigned long long), 16, width, TRUE); break;
        default:   if (d < e) *d++ = L'%';      // 不认识的转换按字面量吐回
                   if (d < e && *p) *d++ = *p;
                   break;
        }
    }
    *d = 0;
    return (int)(d - dst);
}

static int wfmt(WCHAR *dst, int cch, const WCHAR *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = wfmtv(dst, cch, fmt, ap);
    va_end(ap);
    return n;
}

// 十进制整数解析（等价 _wtoi：跳过前导空白与正负号，遇非数字即停）
static int wToI(const WCHAR *s)
{
    while (*s == L' ' || *s == L'\t') s++;
    BOOL neg = (*s == L'-');
    if (neg || *s == L'+') s++;
    int v = 0;
    while (*s >= L'0' && *s <= L'9') v = v * 10 + (int)(*s++ - L'0');
    return neg ? -v : v;
}

// "3 70" → 两个无符号数（配置里的 Hotkey 行）。返回解析到的个数
static int wScanU2(const WCHAR *s, unsigned *a, unsigned *b)
{
    unsigned *out[2] = { a, b };
    int n = 0;
    while (n < 2) {
        while (*s == L' ' || *s == L'\t') s++;
        if (*s < L'0' || *s > L'9') break;
        unsigned v = 0;
        while (*s >= L'0' && *s <= L'9') v = v * 10 + (unsigned)(*s++ - L'0');
        *out[n++] = v;
    }
    return n;
}

// "770 333 842 631" → 四个整数（配置里的 Rect 行）。返回解析到的个数
static int wScanI4(const WCHAR *s, int *a, int *b, int *c, int *d)
{
    int *out[4] = { a, b, c, d };
    int n = 0;
    while (n < 4) {
        while (*s == L' ' || *s == L'\t') s++;
        BOOL neg = (*s == L'-');
        if (neg || *s == L'+') s++;
        if (*s < L'0' || *s > L'9') break;
        int v = 0;
        while (*s >= L'0' && *s <= L'9') v = v * 10 + (int)(*s++ - L'0');
        *out[n++] = neg ? -v : v;
    }
    return n;
}

// 格子缩放基准的合法区间（配置与 Ctrl+滚轮共用同一处钳制）
static int ClampCellBase(int v)
{
    return v < CELL_BASE_MIN ? CELL_BASE_MIN : v > CELL_BASE_MAX ? CELL_BASE_MAX : v;
}

// 图标尺寸随缩放等级走：加载（EnsureIcon）与绘制（PaintGrid）共用同一公式，
// 保证拿到的图标就是绘制尺寸，不重采样；只跟 g_cellBase 走而不跟 g_cell 走，
// 窗口缩放时图标尺寸不变，不会引发重新加载
static int IconSize(void)
{
    int v = S_(g_cellBase) * 3 / 7;
    return v < ICON_SIZE_MIN ? ICON_SIZE_MIN : v > ICON_SIZE_MAX ? ICON_SIZE_MAX : v;
}

// 把 '|' 换成 '_'，避免破坏配置格式
static const WCHAR *SafeField(WCHAR *dst, int cch, const WCHAR *src)
{
    wcpy(dst, cch, src);
    for (WCHAR *p = dst; *p; p++) if (*p == L'|') *p = L'_';
    return dst;
}

// 大小写不敏感子串查找（ASCII 折叠大小写，CJK 原样比较。不引 shlwapi 的
// StrStrIW——少一个 DLL 依赖，且名称≤47 字符，O(n*m) 可忽略）。返回命中
// 位置，-1 未中
static int FindTextI(const WCHAR *hay, const WCHAR *needle)
{
    if (!needle[0]) return 0;
    for (const WCHAR *p = hay; *p; p++) {
        const WCHAR *a = p, *b = needle;
        while (*b) {
            WCHAR ca = *a, cb = *b;
            if (ca >= L'A' && ca <= L'Z') ca += 32;
            if (cb >= L'A' && cb <= L'Z') cb += 32;
            if (ca != cb) break;
            a++; b++;
        }
        if (!*b) return (int)(p - hay);
    }
    return -1;
}

static BOOL IsSepW(WCHAR c)
{
    return c == L' ' || c == L'\t' || c == L'(' || c == L')' || c == L'-' ||
           c == L'_' || c == L'.' || c == L'[' || c == L']' || c == L'（' ||
           c == L'）' || c == L'【' || c == L'】' || c == L'·' || c == L'—' ||
           c == L',' || c == L'，';
}

// ---------------------------------------------------------------- 拼音首字母（中文搜索）
// 常用字拼音首字母表（人工维护，~1800 常用字；生僻字不收录=不参与匹配，
// 多音字取最常用读法）。启动建一次 O(1) 映射，搜索零开销、不连输入法
static const WCHAR *PINYIN_GROUPS[26] = {
    L"啊阿哀挨矮蔼爱碍安氨俺岸按案暗昂",                                        // a
    L"八巴扒吧疤拔把靶坝爸罢白百柏摆败拜斑般班颁搬板版办半伴扮拌瓣帮绑榜膀傍包胞雹饱宝保堡报抱暴杯卑悲碑北贝备背倍被辈奔本笨崩绷逼鼻比彼笔鄙币必毕闭辟避臂边编鞭扁便变遍辨辩标彪表别宾滨兵冰丙柄饼并病拨波玻剥播伯驳勃博搏薄补捕不布步部", // b
    L"擦猜才材财裁采彩踩菜参餐残蚕惨灿苍舱藏操草册侧测策层蹭叉差插茶查察岔拆柴缠产铲颤长场常偿厂敞畅唱抄超朝潮吵炒车扯撤尘沉陈趁称撑成呈城诚承乘程惩吃池迟持匙尺齿耻翅赤充冲虫崇重抽仇愁绸稠筹丑臭出初厨除础储处触川穿传船串疮窗床闯创吹炊垂锤春纯蠢戳词慈磁此次刺从丛粗醋促窜催脆翠村存寸错彻", // c
    L"搭达答打大呆歹代带待怠袋戴贷单担丹胆旦但诞弹淡蛋当挡党荡档刀导岛倒蹈到悼盗道稻得德的灯登等凳低堤滴敌笛底抵地帝弟递第颠典点电店垫殿雕吊钓调爹跌叠蝶丁叮盯钉顶订定丢东冬懂动冻栋洞都斗抖陡豆逗督毒读独堵赌杜肚度渡端短段断缎堆队对吨蹲盾顿多夺朵躲舵堕", // d
    L"鹅蛾额恩儿而耳尔饵二",                                                     // e
    L"发罚伐乏阀法帆番翻凡烦繁反返犯泛饭范贩访纺放飞非菲肥废沸费分纷芬坟粉份奋愤粪丰风封疯峰锋蜂逢缝凤奉佛否夫肤孵伏扶服浮符幅福辐抚辅腐父付妇负附复副赋傅富腹覆", // f
    L"该改盖概干甘杆肝赶敢感冈刚钢纲缸岗港杠高糕搞稿告哥胳鸽割歌阁格隔个各给根跟更工弓公功攻供宫恭共贡勾沟钩构购够估姑孤菇古谷股骨鼓固故顾瓜刮挂乖拐怪关观官冠馆管贯惯灌罐光广归龟规硅鬼贵滚棍锅国果过", // g
    L"哈孩海害含寒喊汉汗旱撼憾号毫豪好浩喝禾合何和河核盒贺黑恨横衡恒轰哄烘红洪虹猴吼后厚候乎呼忽狐胡壶湖糊虎互护花华哗滑化划画话怀淮坏欢环缓换唤患荒慌皇黄煌晃谎灰恢挥辉回毁悔汇会绘贿惠慧昏婚浑魂混豁活火伙或货祸很", // h
    L"",                                                                         // i
    L"击机肌鸡积基绩激及吉级即极急疾集籍几己脊计记既济继寂寄加夹佳家嘉甲假价驾架嫁歼坚尖间肩艰兼监检减剪简见件建剑健舰渐践鉴键箭江姜将浆僵疆讲奖匠降酱交郊娇骄胶椒焦角饺脚搅缴叫轿较教阶皆揭接街节杰洁结捷截竭界借届金津紧锦仅尽进近劲晶精经井警净竞竟敬境静镜纠究揪九久酒旧救就舅居局举句巨拒具俱剧惧距聚卷倦决绝觉掘崛军均君俊解辑", // j
    L"开凯慨刊堪砍看康慷扛抗炕考烤靠科棵颗壳咳可渴克刻客课肯垦恳坑空孔恐惧控口叩扣苦库裤酷夸垮挎跨块快宽款狂框况矿亏葵愧溃昆捆困扩阔",           // k
    L"拉啦蜡腊辣来莱赖蓝篮拦栏兰懒烂滥郎狼廊朗浪捞劳牢老涝雷磊蕾累泪类冷厘离梨犁璃理里礼李力历厉立利励例隶粒帘连怜莲联廉镰脸恋练炼良凉梁粮两亮谅辆量撩聊辽疗料列劣烈猎裂邻林临淋琳鳞凛吝灵玲凌铃零龄领岭令溜刘流留浏览柳六龙笼聋楼搂漏露芦炉鲁陆录鹿路驴旅履律虑率绿卵乱掠略抡轮伦论罗萝螺裸落洛骆了", // l
    L"妈抹麻马码骂吗买迈麦卖脉蛮慢漫蔓忙芒盲莽猫毛矛冒贸帽貌么没玫枚眉梅媒煤霉每美门闷们萌蒙盟猛梦弥谜迷米秘蜜密眠绵棉免勉面苗描秒妙庙灭民敏名明鸣命谋模膜摩磨蘑魔末莫墨默亩母墓幕慕木目牧", // m
    L"拿哪内那纳奶耐南男难囊挠脑闹呢嫩能尼泥你拟匿腻年念娘酿鸟尿捏您宁凝牛扭纽农浓弄奴努怒女暖挪诺哦", // n
    L"哦欧鸥",                                                                   // o
    L"怕趴拍排牌派攀盘判叛乓乒胖抛炮袍跑泡陪培赔佩配喷盆朋棚蓬鹏捧碰批披劈皮疲匹屁偏篇骗飘漂瓶品聘平评苹凭屏泼婆破迫魄仆扑铺葡朴普谱",           // p
    L"七妻栖期欺漆齐骑棋旗企乞岂启起气弃汽迄掐恰洽千迁牵铅谦签前钱潜浅遣欠枪呛腔强抢敲桥侨巧切窃亲侵秦勤青轻倾清蜻情晴顷请庆穷丘秋求球区曲驱屈趋取娶去趣圈全权泉拳犬劝缺确却雀群擎器", // q
    L"然燃染攘让饶扰热人仁忍刃认任扔仍日绒荣容融柔肉如儒乳辱入软锐瑞闰润若弱",   // r
    L"撒萨塞赛三伞散桑丧扫嫂色森杀沙纱傻厦筛晒山删闪陕扇善伤商赏上尚捎烧少绍哨蛇舍设社射申伸身深神沈审婶肾甚渗生升声绳圣胜盛剩尸失师诗湿十石时识实拾食蚀史使驶始士氏世市示事侍势视试饰室是适逝释收手守首寿兽受瘦书梳舒疏输蔬熟属鼠数树竖帅双谁水税睡顺瞬说硕丝司私思死肆四寺似饲松耸送诵搜艘苏俗诉肃素速宿塑酸算虽随岁穗孙损笋缩所索锁术", // s
    L"他她它塔踏胎台抬太态泰贪摊滩坛谈痰坦毯叹探炭汤塘堂唐糖躺烫趟掏涛桃逃淘陶讨套特疼腾梯踢提题体剃替天添田甜填挑条跳贴铁帖厅听亭庭停挺通同桐铜童统桶痛头投透秃突图途徒涂土吐兔团推腿退吞屯托拖脱驼妥椭", // t
    L"",                                                                         // u
    L"",                                                                         // v
    L"挖娃瓦袜外弯湾完玩顽挽晚碗万王网往忘旺望危威微为围违唯维伟伪尾纬委卫未位味谓喂温文纹闻稳问翁涡窝我卧握乌污屋无吴五午武伍舞务物误悟",       // w
    L"西吸希析息牺牲悉惜稀溪锡袭席洗系细虾瞎峡狭霞下夏嫌显险现献县线限相香箱乡湘详祥翔享响想向巷象像项削消宵萧硝销小晓校效些歇协邪斜胁写泄卸屑蟹谢心芯信兴星腥刑行形型醒杏姓幸性凶兄胸雄熊休修羞朽秀袖绣锈虚须许序叙绪续宣悬旋选穴学雪血寻巡询循训讯迅新携", // x
    L"压呀押鸦牙芽哑雅亚咽烟淹延严言岩沿盐颜衍掩眼宴艳焰验央秧扬羊阳杨洋养氧痒样妖腰邀摇遥咬药要爷也冶野页叶夜液一衣医依仪宜移遗疑乙已以蚁倚椅义亿忆艺议亦异役译易疫益谊意毅翼因阴音吟银引隐印应英樱鹰迎赢影映拥佣庸永泳勇涌用优忧悠尤由犹油游友有又右幼诱于余鱼娱渔愉逾喻御遇郁育狱浴预域誉愈邮与运云远乐", // y
    L"杂灾栽宰载再在咱攒暂赞脏葬遭糟凿枣早藻皂灶责择泽贼怎增扎渣闸眨诈摘宅窄债寨沾粘盏斩展崭占战站张章涨掌丈帐账障招着找召赵照罩遮折哲者这浙针侦珍真诊枕阵振镇蒸争征挣睁整正证政症之支枝知织脂汁直值职植止只旨址纸指志制治质秩智置中忠终钟肿众种仲周洲州粥轴肘皱骤朱珠株诸竹烛逐主煮嘱住助注贮驻柱祝筑铸抓爪专砖转赚庄桩装壮状撞追准捉拙卓桌浊姿资滋子紫自字宗综棕踪总纵走奏揍租足族阻组祖钻醉最罪尊遵昨左佐作坐座做", // z
};

static unsigned char g_pinMap[0x9FA5 - 0x4E00 + 1];   // 0=无读音，否则 'a'..'z'

static void PinMapInit(void)
{
    for (int l = 0; l < 26; l++)
        for (const WCHAR *p = PINYIN_GROUPS[l]; *p; p++) {
            unsigned c = *p;
            if (c >= 0x4E00 && c <= 0x9FA5 && !g_pinMap[c - 0x4E00])
                g_pinMap[c - 0x4E00] = (unsigned char)('a' + l);
        }
}

static unsigned char PinInitial(WCHAR c)
{
    static BOOL ready = FALSE;
    if (!ready) { PinMapInit(); ready = TRUE; }
    return (c >= 0x4E00 && c <= 0x9FA5) ? g_pinMap[c - 0x4E00] : 0;
}

// 名字的拼音首字母序列：ASCII 原样小写、CJK 查首字母表、其余（符号）跳过。
// “记事本”→jsb、“HP Support Assistant”→hpsupportassistant
static int NameInitials(const WCHAR *name, WCHAR *out, int cch)
{
    int n = 0;
    for (const WCHAR *p = name; *p && n < cch - 1; p++) {
        WCHAR c = *p;
        if (c >= L'A' && c <= L'Z') c += 32;
        if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9')) {
            out[n++] = c;
        } else {
            unsigned char ini = PinInitial(c);
            if (ini) out[n++] = ini;
        }
    }
    out[n] = 0;
    return n;
}

// 搜索打分：前缀 4 > 词首 3 > 名内子串 2 > 拼音首字母 3/2 > 路径子串 1 > 0。
// 词首 = 词分隔符（空格/括号/点/横线/中点…）之后——"HP Support Assistant"
// 输 hp、sa 均为词首；CJK 无词边界，按子串计分
static int MatchScore(const WCHAR *name, const WCHAR *path, const WCHAR *q)
{
    int pos = FindTextI(name, q);
    if (pos >= 0) {
        if (pos == 0) return 4;
        if (IsSepW(name[pos - 1])) return 3;
        return 2;
    }
    // 拼音首字母命中（jsb → 记事本），计分并入词首/子串档
    WCHAR ini[64];
    if (NameInitials(name, ini, 64)) {
        int pp = FindTextI(ini, q);
        if (pp == 0) return 3;
        if (pp > 0) return 2;
    }
    return (path[0] && FindTextI(path, q) >= 0) ? 1 : 0;
}

// ---------------------------------------------------------------- 布局
// 列数按目标格宽最近取整、格子尺寸由列数反推（横向铺满保持正方形）；
// g_cols/g_slots 只在 WM_SIZE/DPI 变化时算，命中/绘制/滚动直读缓存
static void UpdateCell(void)
{
    int w = g_cw - g_sideW;
    int step = S_(g_cellBase) + g_gap;
    int c = (w + g_gap + step / 2) / step;
    g_cols = c < 1 ? 1 : c;
    g_cell = (g_cw - g_sideW - (g_cols - 1) * g_gap) / g_cols;
    if (g_cell < 8) g_cell = 8;
    g_slots = g_cols * Rows();   // 每帧命中/绘制/滚动钳制直接读缓存
    UpdateBtnRects();            // 标题栏按钮随窗口宽度重定位
}

static int Rows(void)
{
    int r = (g_ch - g_titleH + g_gap) / (g_cell + g_gap);
    return r < 1 ? 1 : r;
}

static RECT CellRect(int vi)
{
    int c = g_cols, col = vi % c, row = vi / c;
    RECT r;
    r.left   = g_sideW + col * (g_cell + g_gap);
    r.top    = g_titleH + row * (g_cell + g_gap) - g_scroll;
    r.right  = r.left + g_cell;
    r.bottom = r.top + g_cell;
    return r;
}

// 视图缓存失效：条目增/删/移/换组、切组、删组后调用
static void ViewDirty(void)
{
    g_viewValid = FALSE;
}

static void ViewRebuild(void)
{
    g_viewN = 0;
    if (g_filter[0]) {
        // 搜索模式：跨全部分组的极速匹配，按相关度排序（前缀>词首>子串>路径，
        // 同分保持配置顺序）。≤128 条插入排序，键入速率下零感知
        struct { int idx, score; } hits[MAX_ITEMS];
        int n = 0;
        for (int i = 0; i < g_nItems; i++) {
            int s = MatchScore(g_items[i].name, g_items[i].path, g_filter);
            if (s > 0) { hits[n].idx = i; hits[n].score = s; n++; }
        }
        for (int i = 1; i < n; i++) {
            int idx = hits[i].idx, sc = hits[i].score;
            int j = i - 1;
            while (j >= 0 && hits[j].score < sc) { hits[j + 1] = hits[j]; j--; }
            hits[j + 1].idx = idx; hits[j + 1].score = sc;
        }
        for (int i = 0; i < n; i++) g_viewIdx[g_viewN++] = hits[i].idx;
    } else {
        for (int i = 0; i < g_nItems; i++) {
            if (g_items[i].group != g_curGroup) continue;
            g_viewIdx[g_viewN++] = i;
        }
    }
    g_viewValid = TRUE;
}

static int ViewCount(void)
{
    if (!g_viewValid) ViewRebuild();
    return g_viewN;
}

static int ItemAtView(int vi)
{
    if (!g_viewValid) ViewRebuild();
    return (vi >= 0 && vi < g_viewN) ? g_viewIdx[vi] : -1;
}

// 空格：有格位（占视图序号、参与重排）但没有程序路径的条目。
// 配置里记为 I#=||组|||，空路径即空格。空格让"拖到第 N 格就停第 N 格"成为可能。
static BOOL IsBlank(int i)
{
    return i >= 0 && i < g_nItems && !g_items[i].path[0];
}

// 真实条目数（不含空格）
static int RealCount(void)
{
    int n = 0;
    for (int i = 0; i < g_nItems; i++)
        if (g_items[i].path[0]) n++;
    return n;
}

// 原始格位：不筛已占项，空格也返回序号（拖动落点/末尾补格用）；超出可视格返回 -1。
// 命中测试核心——CellAt 与拖动落点判定都基于它，不各写一遍除法
static int SlotAt(POINT p)
{
    if (p.x < g_sideW || p.y < g_titleH) return -1;
    int step = g_cell + g_gap;
    int col = (p.x - g_sideW) / step;
    // 列数取整后右边缘可能剩一条窄带：落进去的 col 会等于 g_cols，算出的
    // vi 就窜到下一行行首——拖放落点会指错格。拦在这里
    if (col >= g_cols) return -1;
    int vi = ((p.y - g_titleH + g_scroll) / step) * g_cols + col;
    return vi < g_slots ? vi : -1;
}

// 格子命中：列/行一次除到位，槽位与整格上界同源算清——原来经
// SlotAt + CellRect 要四次除法，而这是鼠标移动的最高频路径。
// 格子间隙里的点击不算命中（保持原语义）
static int CellAt(POINT p)
{
    if (p.x < g_sideW || p.y < g_titleH) return -1;
    int step = g_cell + g_gap;
    int col = (p.x - g_sideW) / step;
    if (col >= g_cols) return -1;
    int row = (p.y - g_titleH + g_scroll) / step;
    int x = g_sideW + col * step, y = g_titleH + row * step - g_scroll;
    // 已保证 p.x >= x、p.y >= y（除法取整的构造），只需查右/下边界
    if (p.x >= x + g_cell || p.y >= y + g_cell) return -1;
    int vi = row * g_cols + col;
    return vi < g_slots && vi < ViewCount() ? vi : -1;
}

static void ClampScroll(void)
{
    int need = ViewCount();
    int visible = g_slots;
    int maxS = need > visible ? (need - visible + g_cols - 1) / g_cols * (g_cell + g_gap) : 0;
    if (g_scroll > maxS) g_scroll = maxS;
    if (g_scroll < 0) g_scroll = 0;
}

// 把视图格 vi 滚进可视区（最小滚动量）。返回滚动量是否真的变了——
// 调用方据此决定要不要整窗重绘（Home/End/翻页键走这条）
static BOOL ScrollCellIntoView(int vi)
{
    if (vi < 0 || vi >= ViewCount()) return FALSE;
    int old = g_scroll;
    int top = (vi / g_cols) * (g_cell + g_gap);
    int view = g_ch - g_titleH;
    if (top < g_scroll) g_scroll = top;
    else if (top + g_cell > g_scroll + view) g_scroll = top + g_cell - view;
    ClampScroll();
    return g_scroll != old;
}

// 侧栏分组列表的虚拟内容高度（行高 + 上下留白）：滚动钳制与滚动条
// 几何必须共用同一算式，否则 thumb 与钳制对不上
static int SidebarContentH(void)
{
    return g_nGroups * g_grpH + g_grpPadY;
}

static void ClampGrpScroll(void)
{
    int content = SidebarContentH();
    int view = g_ch - g_titleH;
    int maxS = content > view ? content - view : 0;
    if (g_grpScroll > maxS) g_grpScroll = maxS;
    if (g_grpScroll < 0) g_grpScroll = 0;
}

// 分组多到装不下时，把指定分组滚进可视区（最小滚动量）
static void EnsureGroupVisible(int gi)
{
    int view = g_ch - g_titleH;
    int top = g_grpPadTop + gi * g_grpH;
    int bottom = top + g_grpH;
    if (top - g_grpScroll < 0) g_grpScroll = top;
    else if (bottom - g_grpScroll > view) g_grpScroll = bottom - view;
    ClampGrpScroll();
}

// 切换到指定分组（点击和滚轮共用）
static void SelectGroup(int gi)
{
    if (gi < 0 || gi >= g_nGroups || gi == g_curGroup) return;
    // 滚动位备忘的写入边界：g_curGroup 理论上恒在 [0, MAX_GROUPS)，
    // 这里再收一道——数组写越界没有第二次机会
    if (g_curGroup >= 0 && g_curGroup < MAX_GROUPS)
        g_scrollMemo[g_curGroup] = g_scroll;    // 记住离开时的滚动位
    g_curGroup = gi;
    ViewDirty();
    g_scroll = g_scrollMemo[gi];            // 恢复目标组上次的滚动位
    ClampScroll();                          // 内容可能变了：钳回范围
    EnsureGroupVisible(gi);
    UpdateHoverText();
    InvalidateRect(g_hwnd, NULL, FALSE);   // 标题/侧栏/格子全区都变（原三个分区矩形的并集就是全窗）
    RefreshCard();          // 视图换了：同一悬停格对应的条目可能变了，卡片重锚
    UpdateWindow(g_hwnd);   // 滚轮连发时 WM_PAINT 会被消息饿死，立即同步重绘
    UpdateTrayTip();        // 托盘提示带当前分组名：切组后悬停信息同步
}

static RECT GroupRect(int i)
{
    int top = g_titleH + g_grpPadTop + i * g_grpH - g_grpScroll;
    RECT r = { g_grpPadX, top, g_sideW - g_grpPadX, top + g_grpH - g_grpRowGap };
    return r;
}

static int GroupAt(POINT p)
{
    if (p.x >= g_sideW || p.y < g_titleH) return -1;
    int i = (p.y - (g_titleH + g_grpPadTop) + g_grpScroll) / g_grpH;
    if (i >= 0 && i < g_nGroups) {
        RECT r = GroupRect(i);
        if (p.y >= r.top && p.y < r.bottom) return i;
    }
    return -1;
}

// 标题栏右侧按钮：1=钉住 2=关闭（编码同 g_hoverBtn/g_pressBtn）。
// 几何缓存成矩形表，命中/绘制只读——热路径上不做乘除
static RECT g_btnRect[3];     // 下标即按钮编码（0 号位不用）
static int  g_pressBtn = 0;   // 正按下的按钮：纯视觉反馈，动作仍即时生效
static int  g_btnZoneW;       // 按钮集群总宽（两钮+间距+右边距）：标题文字右边界据此避让

static void UpdateBtnRects(void)
{
    int w = S_(28), h = S_(20);
    int y = (g_titleH - h) / 2;
    int x = g_cw - S_(6) - w;                 // 关闭：贴右
    SetRect(&g_btnRect[BTN_CLOSE], x, y, x + w, y + h);
    x -= w + S_(4);                           // 钉住：左边一位
    SetRect(&g_btnRect[BTN_PIN], x, y, x + w, y + h);
    g_btnZoneW = g_cw - x;                    // 即 2w + 间距 + 右边距，少两处 S_
}

static int BtnAt(POINT p)
{
    if (PtInRect(&g_btnRect[BTN_PIN], p)) return BTN_PIN;
    if (PtInRect(&g_btnRect[BTN_CLOSE], p)) return BTN_CLOSE;
    return 0;
}

// ---------------------------------------------------------------- 图标（按需加载）
// 轻量链优先：PrivateExtractIconsW → ExtractIconExW/资源映射 → 注册表关联链，
// 全部失败才 SHGetFileInfo 兜底——它会拖来 ~15MB 常驻 shell 图标管线，
// 代价只由轻量链解不出的文件类型（如打包应用关联）承担
typedef int (WINAPI *PFN_PrivateExtractIconsW)(LPCWSTR, int, int, int,
                                               HICON *, int *, int, int);
static PFN_PrivateExtractIconsW pExtractIcons = NULL;

// 从映像文件（exe/dll/ico…）取指定大小/索引的图标；失败返回 NULL
static HICON IconFromImage(const WCHAR *path, int want, int idx)
{
    HICON icons[1] = { 0 };
    int ids[1] = { 0 };
    if (pExtractIcons) {
        if (pExtractIcons(path, idx, want, want, icons, ids, 1, 0) > 0 && icons[0])
            return icons[0];
        // 部分程序图标只在特定尺寸档有资源：换标准档位再试（DrawIconEx 绘制时会缩放到位）
        static const int fb[3] = { 32, 48, 16 };
        for (int k = 0; k < 3; k++) {
            icons[0] = NULL;
            if (pExtractIcons(path, idx, fb[k], fb[k], icons, ids, 1, 0) > 0 && icons[0])
                return icons[0];
        }
    }
    // 把目标当数据文件映射，遍历它的 RT_GROUP_ICON（存根程序图标 ID 不一定是 1）
    HMODULE hm = LoadLibraryExW(path, NULL,
                                LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (hm) {
        HICON out = NULL;
        for (DWORD id = 1; id <= 32 && !out; id++) {
            HRSRC hg = FindResourceW(hm, MAKEINTRESOURCEW(id), RT_GROUP_ICON);
            if (!hg) continue;
            HGLOBAL gl = LoadResource(hm, hg);
            BYTE *dir = gl ? (BYTE *)LockResource(gl) : NULL;
            if (!dir) continue;
            WORD pic = (WORD)LookupIconIdFromDirectoryEx(dir, TRUE, want, want, 0);
            if (!pic) continue;
            if (!FindResourceW(hm, MAKEINTRESOURCEW(pic), RT_ICON)) continue;
            out = (HICON)LoadImageW(hm, MAKEINTRESOURCEW(pic), IMAGE_ICON,
                                    want, want, LR_DEFAULTCOLOR);
        }
        FreeLibrary(hm);
        if (out) return out;
    }
    // 再试轻量的 ExtractIconEx（不触发缩略图管道）
    // 注意：变量名不能叫 small —— Windows 头里有 #define small char
    HICON big = NULL, sml = NULL;
    if (ExtractIconExW(path, idx, &big, &sml, 1) && (big || sml)) {
        // 两个尺寸可能同时提取成功：大的直接用，小的要么缩放成大尺寸
        // 要么销毁——只 return big 会把 sml 泄漏掉（每次加载一枚）
        HICON out;
        if (big) {
            out = big;
        } else {
            out = (HICON)CopyImage(sml, IMAGE_ICON, want, want, LR_DEFAULTCOLOR);
        }
        if (sml) DestroyIcon(sml);
        return out;
    }
    return NULL;
}

// ---------------------------------------------------------------- .lnk 解析（MS-SHLLINK 最小实现）
// 图标和启动都不过 shell。字段缺失不算失败（target/iconLoc 为空即无），越界才拒绝。
typedef struct {
    WCHAR target[MAX_PATH];
    WCHAR iconLoc[MAX_PATH];
    int   iconIdx;
    WCHAR args[192];
    WCHAR work[MAX_PATH];
} LnkInfo;

static BOOL ParseLnkBuf(const BYTE *b, DWORD n, LnkInfo *li)
{
    if (n < 0x4C) return FALSE;
    const BYTE *end = b + n;
    DWORD hdr   = *(const DWORD *)(b + 0x00);
    DWORD flags = *(const DWORD *)(b + 0x14);
    DWORD hicon = *(const DWORD *)(b + 0x38);               // 头部 IconIndex
    if (hdr < 0x4C || b + hdr > end) return FALSE;
    ZeroMemory(li, sizeof *li);
    const BYTE *p = b + hdr;

    if (flags & 0x0001) {                                   // HasLinkTargetIDList
        if (p + 2 > end) return FALSE;
        p += 2 + *(const WORD *)p;
    }
    const BYTE *linfo = NULL;
    if (flags & 0x0002) {                                   // HasLinkInfo
        if (p + 4 > end) return FALSE;
        DWORD lisz = *(const DWORD *)p;
        if (lisz < 0x1C || p + lisz > end) return FALSE;
        linfo = p;
        p += lisz;
    }
    // StringData：固定顺序 NAME/REL_PATH/WORK_DIR/ARGS/ICON_LOC，各自带 u16 长度前缀
    BOOL uni = (flags & 0x0080) != 0;
    struct { DWORD bit; WCHAR *dst; int cch; } ss[5] = {
        { 0x0004, NULL,          0   },                     // NAME_STRING：跳过
        { 0x0008, NULL,          0   },                     // RELATIVE_PATH：跳过
        { 0x0010, li->work,    MAX_PATH },
        { 0x0020, li->args,    192 },
        { 0x0040, li->iconLoc, MAX_PATH },
    };
    for (int i = 0; i < 5; i++) {
        if (!(flags & ss[i].bit)) continue;
        if (p + 2 > end) return FALSE;
        DWORD cch = *(const WORD *)p;
        p += 2;
        DWORD nb = uni ? cch * 2u : cch;
        if (nb && p + nb > end) return FALSE;
        if (ss[i].dst && cch) {
            // cch 有两种实现：规范含尾 NUL、WScript/PowerShell 不含。
            // 尾字符为 0 就当含 NUL 处理，两种写法通吃（字段总长都是 cch*2）。
            if (uni) {
                DWORD cnt = cch;
                if (*(const WCHAR *)(p + (cnt - 1) * 2) == 0) cnt--;
                int m = (int)cnt;
                if (m > ss[i].cch - 1) m = ss[i].cch - 1;
                memcpy(ss[i].dst, p, (size_t)m * 2);
                ss[i].dst[m] = 0;
            } else {
                int m = (int)cch;
                if (p[m - 1] == 0) m--;
                if (m > ss[i].cch - 1) m = ss[i].cch - 1;
                MultiByteToWideChar(CP_ACP, 0, (const char *)p, m, ss[i].dst, ss[i].cch);
                ss[i].dst[ss[i].cch - 1] = 0;
            }
        }
        p += nb;
    }
    // LinkInfo → 本地目标路径（Unicode 优先，退 ANSI）
    if (linfo) {
        DWORD lisz = *(const DWORD *)linfo;
        DWORD lhs  = *(const DWORD *)(linfo + 4);
        DWORD lfl  = *(const DWORD *)(linfo + 8);
        if (lfl & 0x01) {                                   // VolumeIDAndLocalBasePath
            DWORD offu = (lhs >= 0x24) ? *(const DWORD *)(linfo + 0x1C)
                                       : 0;                 // LocalBasePathOffsetUnicode
            if (offu && offu + 2 <= lisz) {
                const WCHAR *s  = (const WCHAR *)(linfo + offu);
                const WCHAR *se = (const WCHAR *)(linfo + lisz);
                int m = 0;
                while (s + m < se && s[m] && m < MAX_PATH - 1) { li->target[m] = s[m]; m++; }
                li->target[m] = 0;
            } else {
                DWORD off = *(const DWORD *)(linfo + 0x10); // LocalBasePathOffset（ANSI）
                if (off && off < lisz) {
                    const char *s  = (const char *)(linfo + off);
                    const char *se = (const char *)(linfo + lisz);
                    int m = 0;
                    while (s + m < se && s[m] && m < MAX_PATH - 1) m++;
                    MultiByteToWideChar(CP_ACP, 0, s, m, li->target, MAX_PATH - 1);
                    li->target[MAX_PATH - 1] = 0;
                }
            }
        }
        // CommonPathSuffix：本地路径的后半段（路径被拆开存时 LocalBasePath 只是
        // 前缀，完整目标 = LocalBasePath + CommonPathSuffix——漏掉它目标被截断）
        DWORD sfxoff = *(const DWORD *)(linfo + 0x18);
        if (lhs >= 0x28) {
            DWORD soffu = *(const DWORD *)(linfo + 0x20);
            if (soffu && soffu + 2 <= lisz) {
                const WCHAR *s = (const WCHAR *)(linfo + soffu);
                const WCHAR *se = (const WCHAR *)(linfo + lisz);
                int m = (int)wlen_(li->target);
                while (s < se && *s && m < MAX_PATH - 1) li->target[m++] = *s++;
                li->target[m] = 0;
            }
        } else if (sfxoff && sfxoff < lisz) {
            const char *s  = (const char *)(linfo + sfxoff);
            const char *se = (const char *)(linfo + lisz);
            int r = 0;
            while (s + r < se && s[r]) r++;
            int m = (int)wlen_(li->target);
            if (r) {
                MultiByteToWideChar(CP_ACP, 0, s, r, li->target + m, MAX_PATH - m);
                li->target[MAX_PATH - 1] = 0;
            }
        }
    }
    // ICON_LOCATION 的 ",N" 后缀 → 图标索引；没有则用头部 IconIndex
    WCHAR *c = wrchr_(li->iconLoc, L',');
    if (c && c[1] >= L'0' && c[1] <= L'9') {
        li->iconIdx = wToI(c + 1);
        *c = 0;
    } else if (!li->iconIdx && (hicon & 0x7FFFFFFF)) {
        li->iconIdx = (int)(hicon & 0x7FFFFFFF);
    }
    return TRUE;
}

static BOOL ParseLnk(const WCHAR *path, LnkInfo *li)
{
    BYTE buf[8192];   // ponytail: 超过 8KB 的 .lnk 视为异常，走 ShellExecute 兜底
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    DWORD n = 0;
    BOOL ok = ReadFile(f, buf, sizeof buf, &n, NULL) && n > 0x4C;
    CloseHandle(f);
    return ok && ParseLnkBuf(buf, n, li);
}

// 图标画布：32bpp 自上而下 DIBSection 当颜色位图。GDI 绘制不动 alpha
// （实测还会把触碰处 alpha 清 0），所以 alpha 必须画完后在 CanvasEnd 写；
// 掩码位图只是占位——32bpp 颜色位图下 CreateIconIndirect 忽略掩码内容
typedef struct { HDC dc; HBITMAP col, msk; DWORD *bits; } IconCanvas;

static BOOL CanvasBegin(IconCanvas *cv, int sz)
{
    HDC scr = GetDC(NULL);
    if (!scr) return FALSE;
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = sz;
    bi.bmiHeader.biHeight = -sz;              // 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    cv->col = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, (void **)&cv->bits, NULL, 0);
    cv->msk = CreateBitmap(sz, sz, 1, 1, NULL); // 占位：须与颜色位图同尺寸
    cv->dc = (cv->col && cv->msk) ? CreateCompatibleDC(scr) : NULL;
    ReleaseDC(NULL, scr);
    if (!cv->col || !cv->msk || !cv->dc || !cv->bits) {
        if (cv->col) DeleteObject(cv->col);
        if (cv->msk) DeleteObject(cv->msk);
        if (cv->dc) DeleteDC(cv->dc);
        return FALSE;
    }
    SelectObject(cv->dc, cv->col);
    return TRUE;
}

// 收尾出图标：alpha 整体置 255（不透明），keep 非空时区外挖空（托盘图标的圆角）。
// 两个手绘图标只画内容，新建/写 alpha/建图标/弃画布的样板都收在这里
static HICON CanvasEnd(IconCanvas *cv, int sz, HRGN keep)
{
    DWORD n = (DWORD)sz * (DWORD)sz;
    for (DWORD i = 0; i < n; i++) cv->bits[i] |= 0xFF000000;
    if (keep) {
        for (int y = 0; y < sz; y++)
            for (int x = 0; x < sz; x++)
                if (!PtInRegion(keep, x, y))
                    cv->bits[y * sz + x] &= 0x00FFFFFF;   // 区外透明：只清 alpha
    }
    ICONINFO ii;
    ZeroMemory(&ii, sizeof ii);
    ii.fIcon = TRUE;
    ii.hbmColor = cv->col;
    ii.hbmMask = cv->msk;                     // 占位掩码：颜色位图带 alpha，以它为准
    HICON ico = CreateIconIndirect(&ii);      // 图标自拷贝位图，画布随后可弃
    if (cv->dc) DeleteDC(cv->dc);
    if (cv->col) DeleteObject(cv->col);
    if (cv->msk) DeleteObject(cv->msk);
    return ico;
}

// 文件夹：画一个最简琥珀色文件夹。整幅不透明，无需挖孔
static HICON MakeFolderIcon(int sz)
{
    IconCanvas cv;
    if (!CanvasBegin(&cv, sz)) return NULL;
    HBRUSH fb = CreateSolidBrush(RGB(0xE2, 0xB2, 0x3E));
    RECT tab  = { sz / 8, sz / 5, sz / 8 + sz / 3, sz / 5 + sz / 6 };
    RECT body = { sz / 8, sz / 5 + sz / 6 - 1, sz - sz / 8, sz - sz / 5 };
    FillRect(cv.dc, &tab, fb);
    FillRect(cv.dc, &body, fb);
    DeleteObject(fb);
    return CanvasEnd(&cv, sz, NULL);
}

// 路径存在性检查的安全边界：断连的映射盘（盘符在、网络不通）会让
// GetFileAttributesW 阻塞数秒，而启动修剪与图标加载都在 UI 线程上——
// 只对本地盘做存在性检查；UNC/映射盘/未知类型/URL 一律返回 FALSE
// （跳过），由调用方决定降级策略（修剪=保留条目、图标=走注册表关联链）
static BOOL PathCheckable(const WCHAR *path)
{
    if (path[0] == L'\\' && path[1] == L'\\') return FALSE;   // UNC
    if (wstr_(path, L"://")) return FALSE;                   // URL（http://、steam://…）
    if (path[1] != L':') return TRUE;                        // 相对路径：无盘符
    WCHAR root[4] = { path[0], L':', L'\\', 0 };
    UINT t = GetDriveTypeW(root);             // 查映射表，不产生文件系统 I/O
    return t == DRIVE_FIXED || t == DRIVE_REMOVABLE || t == DRIVE_CDROM;
}

// 扩展名判断：.lnk 检测等处共用（大小写不敏感）
static BOOL HasExt(const WCHAR *path, const WCHAR *ext)
{
    const WCHAR *dot = wrchr_(path, L'.');
    return dot && SameTextI(dot, ext);
}

// 读注册表 DefaultIcon 值并提取图标。值形如 "路径[,索引]"（负索引 = 资源 ID），
// 引号包路径也认；REG_EXPAND_SZ 的环境变量由 RegGetValueW 自动展开。
// src：%1/%0 占位符的替换源——exefile 的 DefaultIcon 就是 "%1"（用文件自己当图标）
static HICON IconFromDefaultKey(const WCHAR *subkey, const WCHAR *src, int want)
{
    WCHAR val[600];
    DWORD cb = sizeof val;
    if (RegGetValueW(HKEY_CLASSES_ROOT, subkey, NULL, RRF_RT_REG_SZ,
                     NULL, val, &cb) != ERROR_SUCCESS || !val[0])
        return NULL;
    int idx = 0;
    WCHAR *icon = val;
    if (*icon == L'"') {                       // "带引号的路径",索引
        WCHAR *e = wchr_(icon + 1, L'"');
        if (!e) return NULL;
        *e = 0;
        if (e[1] == L',') idx = wToI(e + 2);
        icon = icon + 1;
    } else {
        WCHAR *c = wrchr_(icon, L',');        // 从右往左找逗号（路径可能含逗号）
        if (c && c[1]) { *c = 0; idx = wToI(c + 1); }
    }
    if (!icon[0]) return NULL;
    if (!wcmp_(icon, L"%1") || !wcmp_(icon, L"%0")) icon = (WCHAR *)src;
    return IconFromImage(icon, want, idx);
}

// 文档类型关联图标：扩展名 → ProgID → DefaultIcon。手工走注册表，
// 不碰 SHGetFileInfo 的 shell 图标管线（内存红线）。顺序与资源管理器
// 一致：先用户选择（FileExts\UserChoice）再退机器默认——反了会显示
// 用户已换掉的旧关联图标
static HICON AssocIcon(const WCHAR *path, int want)
{
    const WCHAR *ext = wrchr_(path, L'.');
    if (!ext || wlen_(ext) < 2 || wlen_(ext) > 30) return NULL;
    WCHAR key[300];
    WCHAR uc[300];
    if (wfmt(uc, 300,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\%s\\UserChoice",
                 ext) < 300) {
        WCHAR up[128];
        DWORD cb = sizeof up;
        if (RegGetValueW(HKEY_CURRENT_USER, uc, L"ProgID", RRF_RT_REG_SZ,
                         NULL, up, &cb) == ERROR_SUCCESS && up[0]) {
            if (wfmt(key, 300, L"%s\\DefaultIcon", up) < 300) {
                HICON h = IconFromDefaultKey(key, path, want);
                if (h) return h;
            }
        }
    }
    WCHAR progid[128];
    DWORD cb = sizeof progid;
    if (RegGetValueW(HKEY_CLASSES_ROOT, ext, NULL, RRF_RT_REG_SZ,
                     NULL, progid, &cb) == ERROR_SUCCESS && progid[0]) {
        if (wfmt(key, 300, L"%s\\DefaultIcon", progid) < 300)
            return IconFromDefaultKey(key, path, want);
    }
    return NULL;
}

// 文件夹图标：Directory\DefaultIcon（系统真实文件夹），失败退手绘琥珀文件夹
static HICON FolderIcon(const WCHAR *path, int want)
{
    HICON h = IconFromDefaultKey(L"Directory\\DefaultIcon", path, want);
    return h ? h : MakeFolderIcon(want);
}

// 兜底：SHGetFileInfo 取资源管理器同款图标。会拉起 shell 图标管线（文件头内存
// 红线所禁），故只在前面的轻量链路全部失败时才走到——代价仅由确实解不出的
// 文件类型承担（如 Win11 新版记事本的打包应用关联、本机无关联的 .doc）。
// 返回的句柄归我们所有（ownIcon=TRUE），DropIcon 时销毁
static HICON ShellIcon(const WCHAR *path)
{
    SHFILEINFOW fi = { 0 };
    if (SHGetFileInfoW(path, 0, &fi, sizeof fi, SHGFI_ICON | SHGFI_LARGEICON) && fi.hIcon)
        return fi.hIcon;                        // 32px：绘制时 DrawIconEx 缩到格宽
    return NULL;
}

// shell 命名空间项（回收站/此电脑/控制面板…）的图标：路径是 "::{CLSID}"
// 解析名，SHGetFileInfo 直接吃字符串不认（实测 FAIL），经 SHParseDisplayName
// 拿 PIDL 再走 SHGFI_PIDL 就能取到资源管理器同款（实测真图标）。纯注册表/
// 内存操作，无文件系统 I/O，UI 线程安全
static HICON NamespaceIcon(const WCHAR *path)
{
    PIDLIST_ABSOLUTE pidl;
    if (FAILED(SHParseDisplayName(path, NULL, &pidl, 0, NULL))) return NULL;
    SHFILEINFOW fi = { 0 };
    DWORD_PTR ok = SHGetFileInfoW((LPCWSTR)pidl, 0, &fi, sizeof fi,
                                  SHGFI_ICON | SHGFI_PIDL | SHGFI_LARGEICON);
    CoTaskMemFree(pidl);
    return ok ? fi.hIcon : NULL;               // 32px：绘制时 DrawIconEx 缩到格宽
}

// shell 命名空间解析名（回收站/此电脑等 = "::{CLSID}"）统一判定：
// 图标状态化（回收站空/满）、无文件位置可去、提权打开无意义
static BOOL IsVirtualPath(const WCHAR *path)
{
    return path[0] == L':' && path[1] == L':';
}

// shell 虚拟项图标随状态变化（回收站空/满，与桌面同款状态感知）。
// 缓存一次就冻结，故唤醒时重取：先取新再换旧，绘制无空窗。
// 刻意不做心跳刷新——SHGetFileInfo 在 shell 忙时同步阻塞 UI 线程，
// 定时触发等于埋雷；唤醒是用户主动动作，时机安全
static void RefreshVirtualIcons(void)
{
    if (g_menuOpen || g_dlgOpen) return;   // 菜单/对话框模态期间回避 shell 调用：
                                           // SHGetFileInfo 在 shell 忙时同步阻塞，会卡 UI
    for (int i = 0; i < g_nItems; i++) {
        if (!IsVirtualPath(g_items[i].path)) continue;
        HICON h = NamespaceIcon(g_items[i].path);
        if (!h) continue;
        if (g_items[i].ownIcon && g_items[i].icon) DestroyIcon(g_items[i].icon);
        g_items[i].icon = h;
        g_items[i].ownIcon = TRUE;
        for (int v = 0; v < ViewCount(); v++)       // 在当前视图里才需局部重绘
            if (ItemAtView(v) == i) { InvalidateCell(v); break; }
    }
}

static HICON IconFor(const WCHAR *path, int want)
{
    HICON h = NULL;
    // shell 命名空间解析名（"::{CLSID}"）：不进文件系统检查/扩展名关联链，
    // 直接问 shell 要图标（状态感知，见 RefreshVirtualIcons）
    if (IsVirtualPath(path))
        return NamespaceIcon(path);
    // 网络路径/URL 整体早退：不做任何路径 I/O（断连映射盘会阻塞数秒且
    // 阻塞在 UI 线程），图标只按扩展名走注册表关联链；解析不出时返回
    // NULL，由 EnsureIcon 落到通用图标
    if (!PathCheckable(path))
        return AssocIcon(path, want);
    BOOL isDir = FALSE;
    {
        DWORD a = GetFileAttributesW(path);
        isDir = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    }
    if (isDir) {
        h = FolderIcon(path, want);
    } else if (HasExt(path, L".lnk")) {
        LnkInfo lnk;
        if (ParseLnk(path, &lnk)) {
            if (lnk.iconLoc[0]) {
                if (PathCheckable(lnk.iconLoc))
                    h = IconFromImage(lnk.iconLoc, want, lnk.iconIdx);
                else
                    h = AssocIcon(lnk.iconLoc, want);   // 图标位置在网络盘：注册表兜底
            }
            if (!h && lnk.target[0]) {
                if (!PathCheckable(lnk.target)) {
                    h = AssocIcon(lnk.target, want);    // 目标在网络盘：注册表兜底
                } else {
                    DWORD ta = GetFileAttributesW(lnk.target);
                    if (ta != INVALID_FILE_ATTRIBUTES && (ta & FILE_ATTRIBUTE_DIRECTORY))
                        h = FolderIcon(lnk.target, want);   // 指向文件夹的快捷方式
                    else {
                        h = IconFromImage(lnk.target, want, 0);
                        if (!h) h = ShellIcon(lnk.target);  // 目标：资源管理器同款
                    }
                }
            }
        }
    } else {
        h = IconFromImage(path, want, 0);
    }
    if (!h) h = AssocIcon(path, want);          // 文档类型：注册表关联图标兜底
    if (!h) h = ShellIcon(path);                // 最终兜底：资源管理器同款
    return h;
}

static void DropIcon(int i)
{
    if (g_items[i].ownIcon && g_items[i].icon) DestroyIcon(g_items[i].icon);
    g_items[i].icon = NULL;
    g_items[i].ownIcon = FALSE;
    g_items[i].iconTried = FALSE;
}

static void EnsureIcon(int i)
{
    if (g_items[i].iconTried) return;
    g_items[i].iconTried = TRUE;
    if (!g_items[i].path[0]) return;
    HICON h = IconFor(g_items[i].path, IconSize());
    if (h) { g_items[i].icon = h; g_items[i].ownIcon = TRUE; }
    else    g_items[i].icon = LoadIconW(NULL, IDI_APPLICATION);   // 共享图标，不销毁
}

// ---------------------------------------------------------------- 条目与分组
// 条目文本字段的唯一写入口：新建/换目标/粘贴三条路径共用，截断规则只此一处
static void ItemSetText(Item *it, const WCHAR *name, const WCHAR *path,
                        const WCHAR *args, const WCHAR *tip)
{
    wcpy(it->name, 48, name);
    wcpy(it->path, MAX_PATH, path);
    wcpy(it->args, 192, args ? args : L"");
    wcpy(it->tip, 96, tip ? tip : L"");
}

static int AddItemRaw(const WCHAR *name, const WCHAR *path, int group,
                      const WCHAR *args, const WCHAR *tip)
{
    if (g_nItems >= MAX_ITEMS) return -1;
    Item *it = &g_items[g_nItems];
    ZeroMemory(it, sizeof *it);
    ItemSetText(it, name, path, args, tip);
    it->group = group;
    g_nItems++;
    ViewDirty();
    return g_nItems - 1;
}

static void NameFromPath(const WCHAR *path, WCHAR *out, int cch)
{
    const WCHAR *p = wrchr_(path, L'\\');
    p = p ? p + 1 : path;
    const WCHAR *dot = wrchr_(p, L'.');
    // 只剥"真扩展名"：点必须落在首字符之后。".workbuddy"/".gitignore" 这类
    // 点开头的名字（隐藏目录/文件的命名习惯）从首字符剥会剩一个 "."，
    // 连同 "archive.tar.gz" 只剥最后一截的规则一样，都是取"最后一个真扩展名"。
    // 一遍扫到"结尾或扩展名点"即停，不再 wcslen + 再拷一遍
    int cut = (dot && dot > p) ? (int)(dot - p) : 0x7FFFFFFF;
    int n = 0;
    while (n < cch - 1 && n < cut && p[n]) n++;
    memcpy(out, p, (size_t)n * sizeof(WCHAR));
    out[n] = 0;
}

static int AddItemByPath(const WCHAR *path, int group)
{
    WCHAR name[48];
    NameFromPath(path, name, 48);
    return AddItemRaw(name, path, group, NULL, NULL);
}

// 带显示名的条目：虚拟 shell 项（回收站等）的路径是 "::{CLSID}" 解析名，
// NameFromPath 取不出可读名，由拖放方经 SHGetNameFromIDList 显式给名
static int AddNamedItem(const WCHAR *path, const WCHAR *name, int group)
{
    if (name && name[0]) return AddItemRaw(name, path, group, NULL, NULL);
    return AddItemByPath(path, group);
}

// 把格子 ii 改指向 path 的新程序：取名、换目标、清参数/提示、丢旧图标。
// 图标不在本函数加载——一律由 PaintGrid 对可见格按需解析：批量拖 50 个
// 文档到屏幕外时，就地加载要同步付 50 次图标链解析（每次 ~0.4-1ms）
static void SetItemTarget(int ii, const WCHAR *path, int group)
{
    WCHAR name[48];
    NameFromPath(path, name, 48);
    DropIcon(ii);
    ItemSetText(&g_items[ii], name, path, L"", L"");   // 旧参数/提示描述的是旧目标
    g_items[ii].group = group;
}

static void RemoveItemAt(int k)
{
    if (k < 0 || k >= g_nItems) return;
    DropIcon(k);
    if (k + 1 < g_nItems)
        memmove(&g_items[k], &g_items[k + 1], (size_t)(g_nItems - k - 1) * sizeof(Item));
    g_nItems--;
    ViewDirty();
}

// 侧栏分组重排：把第 from 组移到 to 位，中间顺移。
// 条目的 group 编号与被移动组跨越的区间一起平移，当前组跟着走——
// 否则组序变了、条目还挂在旧编号上
static void MoveGroup(int from, int to)
{
    if (from < 0 || to < 0 || from >= g_nGroups || to >= g_nGroups || from == to) return;
    Group tmp = g_groups[from];
    if (from < to)
        memmove(&g_groups[from], &g_groups[from + 1], (size_t)(to - from) * sizeof(Group));
    else
        memmove(&g_groups[to + 1], &g_groups[to], (size_t)(from - to) * sizeof(Group));
    g_groups[to] = tmp;
    for (int i = 0; i < g_nItems; i++) {
        int g = g_items[i].group;
        if (g == from) g_items[i].group = to;
        else if (from < to && g > from && g <= to) g_items[i].group = g - 1;
        else if (from > to && g >= to && g < from) g_items[i].group = g + 1;
    }
    if (g_curGroup == from) g_curGroup = to;
    else if (from < to && g_curGroup > from && g_curGroup <= to) g_curGroup--;
    else if (from > to && g_curGroup >= to && g_curGroup < from) g_curGroup++;
    ViewDirty();
    SyncAndSave();
}

// 把当前视图补齐到至少覆盖 slot 格：不足就在末尾补空格。
// 这样拖到"末尾之外的空格"也能精确落位，前面的格子保持不动。
static void EnsureSlot(int slot)
{
    if (slot < 0) return;
    int g = TargetGroup();
    // 视图计数只读一次：AddItemRaw 每补一格就把视图标脏，循环里再问
    // ViewCount 会触发整表重建——补 n 格是 O(n²)，批量拖放逐文件调用
    // 还要再乘一层。脏标记是懒的：循环里本地计数，收尾一次重建即可
    int have = ViewCount();
    while (have <= slot && g_nItems < MAX_ITEMS) {
        AddItemRaw(L"", L"", g, NULL, NULL);
        have++;
    }
}

// ---------------------------------------------------------------- 槽位操作（插入/删除/清空）
// 槽位模型：格子是固定槽位，条目住在槽位里。清空不挪其他格子；插入/删除才挪。

// 三种槽位操作共用的收尾：视图重算 + 滚动钳制 + 数据变更统一收尾
static void SlotOpRefresh(void)
{
    ViewDirty();
    ClampScroll();
    SyncAndSave();
}

// 在视图位 vi 前插入一个空格（其后条目整体后移）；vi 超出视图则追加到末尾
static void InsertSlotAt(int vi)
{
    if (g_nItems >= MAX_ITEMS) return;
    int at = ItemAtView(vi);
    int pos = (at >= 0) ? at : g_nItems;
    int g = TargetGroup();
    memmove(&g_items[pos + 1], &g_items[pos], (size_t)(g_nItems - pos) * sizeof(Item));
    ZeroMemory(&g_items[pos], sizeof g_items[pos]);
    g_items[pos].group = g;
    g_nItems++;
    SlotOpRefresh();
}

// 清空视图位 vi 的条目成空格：槽位保留，其他格子原位不动
static void ClearSlotAt(int vi)
{
    int at = ItemAtView(vi);
    if (at < 0) return;
    int g = g_items[at].group;            // 空格留在原来的组里
    DropIcon(at);
    ZeroMemory(&g_items[at], sizeof g_items[at]);
    g_items[at].group = g;
    SlotOpRefresh();
}

// 删除视图位 vi 的整个槽位（条目一并删除，其后条目前移）
static void DeleteSlotAt(int vi)
{
    int at = ItemAtView(vi);
    if (at < 0) return;
    RemoveItemAt(at);
    SlotOpRefresh();
}

// 启动时清理失效项目：路径已不存在（含 .lnk 被删）直接移除并写回配置。
// URL（含 ://）不走 GetFileAttributes；UNC/映射盘/未知盘的连通性不可信
// （PathCheckable 统一判定），跳过存在性检查——断连的映射盘宁可留着条目，
// 也不能让启动修剪卡在网络上。空格（空路径）不是失效项：那是用户保留的空位
static BOOL PruneInvalidItems(void)
{
    BOOL removed = FALSE;
    for (int i = 0; i < g_nItems; ) {
        const WCHAR *p = g_items[i].path;
        if (!p[0] || !PathCheckable(p)) { i++; continue; }
        if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) {
            RemoveItemAt(i);
            removed = TRUE;
        } else {
            i++;
        }
    }
    if (removed) ClampScroll();
    return removed;
}

static int AddGroup(const WCHAR *name)
{
    if (g_nGroups >= MAX_GROUPS) return -1;
    wcpy(g_groups[g_nGroups].name, 48, name);
    return g_nGroups++;
}

static void DeleteGroup(int i)
{
    if (i < 0 || i >= g_nGroups) return;    // 分组完全用户自治：首组也可删（连同其条目）
    for (int k = 0; k < g_nItems; ) {
        if (g_items[k].group == i) RemoveItemAt(k);
        else k++;
    }
    if (i + 1 < g_nGroups)
        memmove(&g_groups[i], &g_groups[i + 1], (size_t)(g_nGroups - i - 1) * sizeof(Group));
    g_nGroups--;
    for (int k = 0; k < g_nItems; k++)
        if (g_items[k].group > i) g_items[k].group--;
    // 当前组跟着编号平移走：否则删掉前面的组后，视图会莫名跳到别的组
    if (g_curGroup > i) g_curGroup--;
    if (g_curGroup >= g_nGroups) g_curGroup = g_nGroups - 1;
    if (g_curGroup < 0) g_curGroup = 0;
    ViewDirty();
    ClampScroll();
    RefreshCard();       // 视图/分组列表变了：悬停目标可能已失效，卡片重算
}

// 默认落组：第一分组；一个分组都没有时兜底建“常用”（删光后拖入仍可落）
static int DefaultGroup(void)
{
    if (g_nGroups <= 0) AddGroup(L"常用");
    return 0;
}

// 新增条目的落组：当前分组（当前组失效——如删空——则回落首组）
static int TargetGroup(void)
{
    if (g_curGroup >= g_nGroups) return DefaultGroup();
    return g_curGroup;
}

// 新建分组：先弹命名框（预填默认名），取消=不建；清空=回落默认名。
// 建出来再"重命名分组"要多点一次右键，创建这一步直接把名字定了
static void NewGroup(void)
{
    if (g_nGroups >= MAX_GROUPS) return;       // 预检：别弹了框再静默失败
    WCHAR name[48];
    wfmt(name, 48, L"新分组%d", g_nGroups + 1);
    DlgField f[1] = { { L"名称(N):", name, 48, 0 } };
    if (!RunDialog(g_hwnd, L"新建分组", f, 1)) return;
    if (!name[0]) wfmt(name, 48, L"新分组%d", g_nGroups + 1);
    int ni = AddGroup(name);
    if (ni < 0) return;
    SelectGroup(ni);      // 切过去：滚动位/侧栏可见性/悬停/卡片全套收尾
                        // （新建组 appended 在末尾，EnsureGroupVisible 保证选中行看得见）
    SaveConfig();
}

// ---------------------------------------------------------------- 启动
// 常规启动一律 CreateProcess：ShellExecuteW 会把 WinRT 启动栈
// （Windows.System.Launcher → Windows.Storage 等约 15MB）拉进进程且常驻不卸。
// ShellExecuteW 只留给 runas 提权和 CreateProcess 不认识的目标兜底。
static BOOL CreateRun(const WCHAR *exe, const WCHAR *args, const WCHAR *work)
{
    WCHAR cmd[MAX_PATH + 208];
    wfmt(cmd, MAX_PATH + 208, L"\"%s\"%s%s", exe, args[0] ? L" " : L"", args);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL,
                             work[0] ? work : NULL, &si, &pi);
    if (ok) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return ok;
}

static BOOL Launch(int idx, BOOL asAdmin)
{
    if (idx < 0 || idx >= g_nItems) return FALSE;
    const Item *it = &g_items[idx];
    if (!it->path[0]) return FALSE;              // 空格：没有可启动的目标
    if (!asAdmin) {
        if (HasExt(it->path, L".lnk")) {
            LnkInfo lnk;
            if (ParseLnk(it->path, &lnk) && lnk.target[0]) {
                // .lnk 的工作目录常含环境变量（如 Git CMD 的 %HOMEDRIVE%%HOMEPATH%），
                // CreateProcess 不展开 lpCurrentDirectory：不先展开直启必败，
                // 白白退到 shell 兜底路径
                WCHAR wexp[MAX_PATH];
                const WCHAR *work = lnk.work;
                if (work[0] && ExpandEnvironmentStringsW(work, wexp, MAX_PATH) && wexp[0])
                    work = wexp;
                if (CreateRun(lnk.target, it->args[0] ? it->args : lnk.args, work))
                    return TRUE;
            }
        } else if (CreateRun(it->path, it->args, L"")) {
            return TRUE;
        }
        // CreateProcess 不认识的目标（文件夹/文档/协议）：退回 shell
    }
    // ShellExecuteW 返回值 ≤32 即失败（SE_ERR_*）；调用方据此给启动反馈
    return (INT_PTR)ShellExecuteW(g_hwnd, asAdmin ? L"runas" : L"open", it->path,
                                  it->args[0] ? it->args : NULL,
                                  NULL, SW_SHOWNORMAL) > 32;
}

static void OpenLocation(const WCHAR *path)
{
    WCHAR args[MAX_PATH + 16];
    wfmt(args, MAX_PATH + 16, L"/select,\"%s\"", path);
    CreateRun(L"explorer.exe", args, L"");
}

// ---------------------------------------------------------------- 配置：流式读写（低内存）
static void IniPath(void)
{
    GetModuleFileNameW(NULL, g_ini, MAX_PATH);
    WCHAR *p = wrchr_(g_ini, L'\\');
    if (p) p[1] = 0;
    wcat(g_ini, MAX_PATH, ININAME);
}

typedef struct { HANDLE f; BYTE buf[4096]; int len, pos; BOOL first; } RdCtx;

static BOOL RdRefill(RdCtx *r)
{
    if (r->pos < r->len) return TRUE;
    DWORD n = 0;
    if (!ReadFile(r->f, r->buf, sizeof r->buf, &n, NULL)) { r->len = r->pos = 0; return FALSE; }
    r->pos = 0;
    r->len = (int)n;
    return n > 0;
}

static BOOL RdLine(RdCtx *r, WCHAR *out, int cch)
{
    // 行缓冲按"字节"算，而写侧 FmtLine 按 1200 个 WCHAR 封顶——一条含
    // 中文路径的行（每字 UTF-8 至多 3 字节）可到 3600 字节，缓冲小于它
    // 就会把超出的字段静默截掉（读回的路径/参数损坏）。给足最坏值
    static BYTE line[4096];      // 静态化：帧压在 4KB 以下，编译器不发 __chkstk
    int n = 0;
    for (;;) {
        if (r->pos >= r->len && !RdRefill(r)) {
            if (n == 0) return FALSE;
            break;
        }
        BYTE c = r->buf[r->pos++];
        if (c == '\n') break;
        if (n < (int)sizeof line) line[n++] = c;
    }
    if (n > 0 && line[n - 1] == '\r') n--;
    int w = MultiByteToWideChar(CP_UTF8, 0, (char *)line, n, out, cch - 1);
    out[w] = 0;
    if (r->first) {
        r->first = FALSE;
        if (out[0] == 0xFEFF)                        // 去掉 UTF-8 BOM
            memmove(out, out + 1, (size_t)w * sizeof(WCHAR));
    }
    return TRUE;
}

// 配置写出走 8KB 暂存：行满即冲、收尾一次冲完（逐行 WriteFile 要 130 次系统调用）
static HANDLE g_outFile;
static BYTE   g_outBuf[8192];
static int    g_outLen;

static void OutFlush(void)
{
    if (!g_outLen) return;
    DWORD w = 0;
    WriteFile(g_outFile, g_outBuf, (DWORD)g_outLen, &w, NULL);
    g_outLen = 0;
}

static void OutLine(const WCHAR *text)
{
    // 单行最坏 1200 WCHAR → 3600 UTF-8 字节；余量不足先冲，别丢行
    if (g_outLen + 3600 > (int)sizeof g_outBuf) OutFlush();
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, (char *)g_outBuf + g_outLen,
                                (int)sizeof g_outBuf - g_outLen, NULL, NULL);
    if (n > 0) g_outLen += n - 1;      // 不写结尾 NUL
}

static void FmtLine(const WCHAR *fmt, ...)
{
    WCHAR line[1200];
    va_list ap;
    va_start(ap, fmt);
    wfmtv(line, 1200, fmt, ap);
    va_end(ap);
    OutLine(line);
}

static void SetDefaultRect(void)
{
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g_savedW = S_(680);
    g_savedH = S_(520);
    g_savedX = (wa.left + wa.right - g_savedW) / 2;
    g_savedY = (wa.top + wa.bottom - g_savedH) / 2;
}

static void LoadConfig(void)
{
    RdCtx r;
    ZeroMemory(&r, sizeof r);
    r.first = TRUE;
    r.f = CreateFileW(g_ini, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    g_firstRun = (r.f == INVALID_HANDLE_VALUE);   // 无配置=首跑：托盘就绪后给引导气球

    g_pinned = FALSE;
    g_nGroups = 0;               // 分组完全用户自治：无内置“全部”，空配置兜底建“常用”

    WCHAR line[1200], section[32] = L"";
    WCHAR rect[64] = L"";
    // 无配置文件时循环条件直接为假：保持默认分组。这里不能提前 return——
    // 会跳过 SetDefaultRect，首启创建出 0x0 的不可见窗口
    while (r.f != INVALID_HANDLE_VALUE && RdLine(&r, line, 1200)) {
        if (line[0] == L'[') {
            int n = 0;
            for (int i = 1; line[i] && line[i] != L']' && n < 31; i++) section[n++] = line[i];
            section[n] = 0;
            continue;
        }
        if (!line[0] || line[0] == L'#' || line[0] == L';') continue;
        WCHAR *eq = wchr_(line, L'=');
        if (!eq) continue;
        *eq = 0;
        WCHAR *key = line, *val = eq + 1;

        if (!wcmp_(section, L"Main")) {
            if (!wcmp_(key, L"Pinned"))  g_pinned = wToI(val) ? TRUE : FALSE;
            else if (!wcmp_(key, L"Hotkey")) {           // Hotkey=<mods> <vk>
                unsigned mm = 0, vv = 0;
                if (wScanU2(val, &mm, &vv) == 2 &&
                    (mm & (MOD_CONTROL | MOD_ALT)) &&                 // 至少 Ctrl/Alt 之一
                    !(mm & ~(MOD_CONTROL | MOD_ALT | MOD_SHIFT)) &&    // 不许混入别的位
                    vv > 0 && vv <= 0xFF &&                            // 合法虚拟键
                    vv != VK_CONTROL && vv != VK_MENU && vv != VK_SHIFT) {
                    g_hotMods = mm;
                    g_hotVk = vv;
                }
            }
            else if (!wcmp_(key, L"Group"))   g_curGroup = wToI(val);
            else if (!wcmp_(key, L"Cell"))    g_cellBase = ClampCellBase(wToI(val));
            else if (!wcmp_(key, L"Rect"))    wcpy(rect, 64, val);
        } else if (!wcmp_(section, L"Groups")) {
            if (key[0] == L'G' && wToI(key + 1) == g_nGroups) AddGroup(val);
        } else if (!wcmp_(section, L"Items")) {
            if (key[0] != L'I') continue;
            if (wToI(key + 1) != g_nItems) continue;
            WCHAR *f[5] = { 0, 0, 0, 0, 0 };
            int nf = 0;
            WCHAR *p = val;
            while (nf < 5) {
                f[nf++] = p;
                WCHAR *b = wchr_(p, L'|');
                if (!b) break;
                *b = 0;
                p = b + 1;
            }
            if (nf < 2) continue;
            int grp = (nf >= 3 && f[2][0]) ? wToI(f[2]) : 0;
            if (grp < 0 || grp >= g_nGroups) grp = DefaultGroup();
            AddItemRaw(f[0], f[1], grp,
                       nf >= 4 ? f[3] : NULL, nf >= 5 ? f[4] : NULL);   // 名|路径|组|参数|提示
        }
    }
    if (r.f != INVALID_HANDLE_VALUE) CloseHandle(r.f);
    if (g_nGroups < 1) AddGroup(L"常用");   // 空配置/全删光：兜底建首组
    if (g_curGroup < 0 || g_curGroup >= g_nGroups) g_curGroup = 0;
    ViewDirty();

    int x = 0, y = 0, cx = 0, cy = 0;
    if (rect[0] && wScanI4(rect, &x, &y, &cx, &cy) == 4 && cx > 200 && cy > 160) {
        g_savedX = x; g_savedY = y; g_savedW = cx; g_savedH = cy;
        g_applySavedRect = TRUE;      // 首次 WM_DPICHANGED 时按物理像素还原
    } else {
        SetDefaultRect();
    }
    ClampScroll();
    ClampGrpScroll();
}

static void SaveConfig(void)
{
    if (!g_hwnd) return;
    // 原子写：先写等长换名的 claunch.tmp，成功后 MoveFileEx 原子替换 claunch.ini——
    // 写入中途崩溃/断电不会把配置撕成半截（直接覆盖写会）。
    // 换名取等长替换（claunch.ini→claunch.tmp 同 11 字符），无路径拼接溢出风险
    WCHAR tmp[MAX_PATH];
    wcpy(tmp, MAX_PATH, g_ini);
    SIZE_T len = wlen_(tmp);
    if (len < 11 || wcmp_(tmp + len - 11, L"claunch.ini")) return;   // ini 名不符预期：防呆不写
    wcpy(tmp + len - 11, 12, L"claunch.tmp");
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    g_outFile = f;
    g_outLen = 0;
    static const BYTE bom[3] = { 0xEF, 0xBB, 0xBF };
    DWORD w = 0;
    WriteFile(f, bom, 3, &w, NULL);

    RECT r;
    GetWindowRect(g_hwnd, &r);
    FmtLine(L"# claunch 桌面格子配置 (UTF-8)\r\n");
    FmtLine(L"# Hotkey=<mods> <vk>：唤醒快捷键（mods: 1=Alt 2=Ctrl 4=Shift 可相加；vk=虚拟键码）\r\n");
    FmtLine(L"[Main]\r\n");
    FmtLine(L"Rect=%d %d %d %d\r\n", r.left, r.top,
            r.right - r.left, r.bottom - r.top);
    FmtLine(L"Pinned=%d\r\n", g_pinned ? 1 : 0);
    FmtLine(L"Hotkey=%u %u\r\n", g_hotMods, g_hotVk);
    FmtLine(L"Group=%d\r\n", g_curGroup);
    FmtLine(L"Cell=%d\r\n", g_cellBase);
    FmtLine(L"[Groups]\r\n");
    FmtLine(L"Count=%d\r\n", g_nGroups);
    for (int i = 0; i < g_nGroups; i++) {
        WCHAR nm[96];
        FmtLine(L"G%d=%s\r\n", i, SafeField(nm, 96, g_groups[i].name));
    }
    FmtLine(L"[Items]\r\n");
    FmtLine(L"Count=%d\r\n", g_nItems);
    // 条目行五字段：名|路径|组|参数|提示。四个字段必须各有各的缓冲——参数求值
    // 顺序不确定，共用一块就会互相覆盖；用栈上的局部量而非函数静态，省 BSS
    for (int i = 0; i < g_nItems; i++) {
        WCHAR nm[96], pa[MAX_PATH + 8], ag[MAX_PATH + 8], tp[128];
        FmtLine(L"I%d=%s|%s|%d|%s|%s\r\n", i,
                SafeField(nm, 96, g_items[i].name),
                SafeField(pa, MAX_PATH + 8, g_items[i].path), g_items[i].group,
                SafeField(ag, MAX_PATH + 8, g_items[i].args),
                SafeField(tp, 128, g_items[i].tip));
    }
    OutFlush();
    BOOL bad = !FlushFileBuffers(f);       // 落到设备：替换前的最后保障
    CloseHandle(f);
    // 替换失败（极少：旧 ini 被外部短暂独占）→ 删临时件、旧配置原封不动、
    // 保留 dirty 让下个心跳重试
    if (bad || !MoveFileExW(tmp, g_ini, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp);
        return;
    }
    g_cfgDirty = FALSE;   // 磁盘已与内存一致
}

// 应用格子缩放（Ctrl+滚轮）：重算列数、按新尺寸重载图标、钳制滚动、整窗重绘。
// 落盘防抖：连续缩放手势只写一次配置（由 TIMER_HIDE 心跳代劳）
static void ApplyCellBase(int v)
{
    v = ClampCellBase(v);
    if (v == g_cellBase) return;
    g_cellBase = v;
    UpdateCell();
    // 图标尺寸变了要按新尺寸重解析，但 Ctrl+滚轮是连续手势：每格都丢等于每格
    // 重解析一屏图标（~15ms/次，连拨十几格就是几百毫秒的卡顿）。改成手势停稳
    // 150ms 后一次性丢——期间的旧图标由 DrawIconEx 缩到新尺寸，只差 1~2px
    SetTimer(g_hwnd, TIMER_ICONS, 150, NULL);   // 连续拨动会不断重置，只在停稳后触发
    ClampScroll();
    InvalidateRect(g_hwnd, NULL, FALSE);
    g_cfgDirty = TRUE;        // 缩放是连续操作：落盘交给心跳防抖，别每格滚轮都写文件
}

// ---------------------------------------------------------------- 剪贴板（Ctrl+C/V 复制格子）
// 双格式：私有格式存全字段（本程序自识别），另附一份 CF_UNICODETEXT 路径，
// 复制出来的内容也能粘到别的程序里。粘贴目标：悬停格（空格也填），
// 没有悬停格就追加到当前组
static void ItemToClip(const Item *it, ClipItem *ci)
{
    wcpy(ci->name, 48, it->name);
    wcpy(ci->path, MAX_PATH, it->path);
    wcpy(ci->args, 192, it->args);
    wcpy(ci->tip, 96, it->tip);
}

static BOOL ClipSet(const ClipItem *ci)
{
    if (!OpenClipboard(g_hwnd)) return FALSE;   // 剪贴板被别的程序占着：本次复制失败
    EmptyClipboard();
    if (g_clipFmt) {
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(ClipItem));
        if (h) {
            ClipItem *dst = (ClipItem *)GlobalLock(h);
            if (dst) {
                *dst = *ci;
                GlobalUnlock(h);
                // SetClipboardData 成功后系统接管句柄，失败才由我们释放
                if (!SetClipboardData(g_clipFmt, h)) GlobalFree(h);
            } else {
                GlobalFree(h);
            }
        }
    }
    size_t cb = (wlen_(ci->path) + 1) * sizeof(WCHAR);
    HGLOBAL ht = GlobalAlloc(GMEM_MOVEABLE, cb);
    if (ht) {
        WCHAR *dst = (WCHAR *)GlobalLock(ht);
        if (dst) {
            wcpy(dst, (int)(cb / sizeof(WCHAR)), ci->path);
            GlobalUnlock(ht);
            if (!SetClipboardData(CF_UNICODETEXT, ht)) GlobalFree(ht);
        } else {
            GlobalFree(ht);
        }
    }
    CloseClipboard();
    return TRUE;
}

// 从剪贴板取条目：优先私有格式（全字段），退化到纯文本（只当路径，名字由路径推导），
// 最后退化到 CF_HDROP（资源管理器里"复制"文件给的是文件句柄表而非文本——
// 不认它，复制文件后 Ctrl+V 会莫名其妙地失败）。多文件复制只取第一个
static BOOL ClipGet(ClipItem *ci)
{
    if (!OpenClipboard(g_hwnd)) return FALSE;
    BOOL ok = FALSE;
    if (g_clipFmt) {
        HANDLE h = GetClipboardData(g_clipFmt);
        if (h && GlobalSize(h) == sizeof(ClipItem)) {   // 尺寸不符当不存在（防伪造格式名）
            ClipItem *src = (ClipItem *)GlobalLock(h);
            if (src) { *ci = *src; GlobalUnlock(h); ok = TRUE; }
        }
    }
    if (!ok) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            WCHAR *s = (WCHAR *)GlobalLock(h);
            if (s && s[0]) {
                ZeroMemory(ci, sizeof *ci);
                wcpy(ci->path, MAX_PATH, s);
                NameFromPath(ci->path, ci->name, 48);
                ok = TRUE;
            }
            if (s) GlobalUnlock(h);
        }
    }
    if (!ok) {
        HANDLE h = GetClipboardData(CF_HDROP);           // 剪贴板里的文件列表
        if (h) {
            WCHAR p[MAX_PATH];
            if (DragQueryFileW((HDROP)h, 0, p, MAX_PATH) > 0) {   // 取第一个文件
                ZeroMemory(ci, sizeof *ci);
                wcpy(ci->path, MAX_PATH, p);
                NameFromPath(ci->path, ci->name, 48);
                ok = TRUE;
            }
        }
    }
    CloseClipboard();
    return ok;
}

// 粘进指定格：替换全部文本字段、丢掉旧图标；组号不变（粘到哪格就入哪组）。
// 同 SetItemTarget：不就地加载，图标由随后那次绘制按需加载
static void PasteItemAt(int ii, const ClipItem *ci)
{
    ItemSetText(&g_items[ii], ci->name, ci->path, ci->args, ci->tip);   // 组号不变：粘到哪格入哪组
    DropIcon(ii);
    ViewDirty();                     // 名字可能变了：过滤视图要重建
    SyncAndSave();
}

// 追加为新条目到当前组（分组被删光时兜底建“常用”）
static void PasteItemNew(const ClipItem *ci)
{
    int g = TargetGroup();
    if (AddItemRaw(ci->name, ci->path, g, ci->args, ci->tip) >= 0) {
        ClampScroll();
        SyncAndSave();
    }
}

// ---------------------------------------------------------------- 显示/隐藏
static void ForceForeground(HWND h)
{
    HWND fg = GetForegroundWindow();
    // 前台线程挂死时 AttachThreadInput 会无限等（输入队列同步要等对方处理）——
    // 托盘右键由此卡死整条 UI 线程。挂死窗口绕过附加，直上 SetForegroundWindow
    if (fg && fg != h && !IsHungAppWindow(fg)) {
        DWORD fgTid = GetWindowThreadProcessId(fg, NULL);
        DWORD me = GetCurrentThreadId();
        if (fgTid != me) {
            AttachThreadInput(fgTid, me, TRUE);
            SetForegroundWindow(h);
            AttachThreadInput(fgTid, me, FALSE);
            return;
        }
    }
    SetForegroundWindow(h);
}

// 淡入就位（唤醒与首启共用）：透明度从 0 起、淡入定时器启动、移出宽限清零
static void StartFadeIn(void)
{
    g_outSince = 0;
    g_wakeAt = GetTickCount64();   // 给足 3s 宽限，否则光标不在窗口上时约 0.3s 就自动隐藏
    RefreshVirtualIcons();       // 唤醒即对时：回收站空/满等状态化图标对齐当前真实状态
    g_alpha = 0;
    SetLayeredWindowAttributes(g_hwnd, 0, 0, LWA_ALPHA);
    g_fadeDir = 1;
    SetTimer(g_hwnd, TIMER_FADE, 12, NULL);
}

// 静默启动延后的失效清理：任一唤醒路径（面板/拖放唤醒）首次执行一次。
// 非静默启动不走这里——启动时就地清完（见 wWinMain）。
// 外部拖放悬停中跳过：清理会挪动视图序号，光标下的落点会跳
static void WakePrune(void)
{
    if (!g_needPrune || g_extDrag) return;
    g_needPrune = FALSE;
    if (PruneInvalidItems()) SaveConfig();
}

static void ShowLauncher(void)
{
    WakePrune();
    InitOleDrop(g_hwnd);    // 唤醒即注册拖放目标（延迟初始化，见 InitOleDrop）
    POINT pt;
    GetCursorPos(&pt);
    RECT wr;
    GetWindowRect(g_hwnd, &wr);
    if (!PtInRect(&wr, pt)) MoveToPoint(pt);
    else ClampToWorkArea();

    if (!IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd, SW_SHOW);
    ForceForeground(g_hwnd);
    StartFadeIn();
    SetTimer(g_hwnd, TIMER_HIDE, 120, NULL);   // 隐藏期间定时器是停的，这里才重启
}

static void BeginHide(BOOL force)
{
    if (!force && g_pinned) return;
    g_pressBtn = 0;         // 隐藏时按压态一并作废，重新唤回不留残影
    g_fadeDir = -1;
    SetTimer(g_hwnd, TIMER_FADE, 12, NULL);
}

// 把工作集裁到最小：任务管理器内存回到低位，触碰时按需软调入（CLaunch 同款策略）
static void TrimWS(void)
{
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

static void StepFade(void)
{
    if (g_fadeDir > 0) {
        g_alpha += 45;
        if (g_alpha >= 255) { g_alpha = 255; g_fadeDir = 0; KillTimer(g_hwnd, TIMER_FADE); }
        SetLayeredWindowAttributes(g_hwnd, 0, (BYTE)g_alpha, LWA_ALPHA);
    } else if (g_fadeDir < 0) {
        g_alpha -= 45;
        if (g_alpha <= 0) {
            g_alpha = 0; g_fadeDir = 0;
            KillTimer(g_hwnd, TIMER_FADE);
            ShowWindow(g_hwnd, SW_HIDE);
            KillTimer(g_hwnd, TIMER_HIDE);     // 已隐藏：停掉移出检测心跳，进托盘零轮询
            ReleaseMemBuffer();                // 双缓冲 ~2MB 随隐藏释放，唤醒首绘重建
            g_flashAt = 0;                    // 心跳停了：反馈不可能再过期重绘，直接收掉
            if (g_cfgDirty) { g_cfgDirty = FALSE; SaveConfig(); }   // 心跳将停：待落盘的立即落
            TrimWS();
        } else {
            SetLayeredWindowAttributes(g_hwnd, 0, (BYTE)g_alpha, LWA_ALPHA);
        }
    }
}

// 把窗口矩形收进工作区：先缩到装得下，再四边推进去（两个定位路径共用）
static void FitRectToWorkArea(RECT *wr, const RECT *wa)
{
    int w = wr->right - wr->left, h = wr->bottom - wr->top;
    if (w > wa->right - wa->left) w = wa->right - wa->left;
    if (h > wa->bottom - wa->top) h = wa->bottom - wa->top;
    if (wr->left < wa->left) wr->left = wa->left;
    if (wr->top < wa->top) wr->top = wa->top;
    if (wr->left + w > wa->right) wr->left = wa->right - w;
    if (wr->top + h > wa->bottom) wr->top = wa->bottom - h;
    wr->right = wr->left + w;
    wr->bottom = wr->top + h;
}

static void ClampToWorkArea(void)
{
    RECT wr;
    GetWindowRect(g_hwnd, &wr);
    // 收进"窗口中心所在显示器"的工作区（多显示器：副屏上的面板不该被
    // 钳回主屏——SPI_GETWORKAREA 只报主屏）。中心点不在任何显示器上
    // （窗口被拖到屏外）时 MONITOR_DEFAULTTONEAREST 取最近的那个
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof mi);
    mi.cbSize = sizeof mi;
    POINT c = { (wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2 };
    if (!GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi)) return;
    FitRectToWorkArea(&wr, &mi.rcWork);
    SetWindowPos(g_hwnd, NULL, wr.left, wr.top,
                 wr.right - wr.left, wr.bottom - wr.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void MoveToPoint(POINT pt)
{
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
    RECT wr;
    GetWindowRect(g_hwnd, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    wr.left = pt.x - w / 2;          // 以光标为中心，再收进该显示器工作区
    wr.top  = pt.y - h / 2;
    wr.right = wr.left + w;
    wr.bottom = wr.top + h;
    FitRectToWorkArea(&wr, &mi.rcWork);
    SetWindowPos(g_hwnd, NULL, wr.left, wr.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------- 资源
static void ScaleMetrics(void)
{
    g_titleH = S_(34);
    g_sideW  = S_(84);
    g_gap    = S_(3);
    g_grpH   = S_(34);
    g_grpPadX   = S_(6);      // 分行左右留白
    g_grpPadTop = S_(6);      // 首行距标题栏
    g_grpRowGap = S_(3);      // 行高里的间距（行视觉高度 = 行高 - 间距）
    g_grpPadY   = S_(12);     // 列表内容高度的上下留白
    g_grpSelBar = S_(2);      // 选中分组下方指示条粗细
    UpdateBtnRects();      // 标题栏按钮垂直居中于新标题栏高度
}

static void MakeFonts(void)
{
    LOGFONTW lf;
    if (g_font)  { DeleteObject(g_font);  g_font  = NULL; }
    if (g_fontT) { DeleteObject(g_fontT); g_fontT = NULL; }
    ZeroMemory(&lf, sizeof lf);
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcpy(lf.lfFaceName, LF_FACESIZE, L"Segoe UI");
    lf.lfHeight = -MulDiv(9, g_dpi, 72);
    lf.lfWeight = FW_NORMAL;
    g_font = CreateFontIndirectW(&lf);
    lf.lfHeight = -MulDiv(11, g_dpi, 72);
    lf.lfWeight = FW_SEMIBOLD;
    g_fontT = CreateFontIndirectW(&lf);
}

static void MakeBrushes(void)
{
    if (g_brBG) return;
    g_brBG     = CreateSolidBrush(C_BG);
    g_brSide   = CreateSolidBrush(C_SIDE);
    g_brCell   = CreateSolidBrush(C_CELL);
    g_brCellHi = CreateSolidBrush(C_CELL_HI);
    g_brLine   = CreateSolidBrush(C_LINE);
    g_brAccent = CreateSolidBrush(C_ACCENT);
    g_brAccel  = CreateSolidBrush(C_ACCEL);
    g_brPanel  = CreateSolidBrush(C_PANEL);
}

// 标题栏按钮与格子边框的 GDI 对象：颜色固定，DPI 变化时整套重建
// （画一帧建一套再销毁是纯 churn）
static void MakeGdiObjects(void)
{
    if (g_brPinOff)   { DeleteObject(g_brPinOff);   g_brPinOff = NULL; }
    if (g_brPinOn)    { DeleteObject(g_brPinOn);    g_brPinOn = NULL; }
    if (g_brGlyph)    { DeleteObject(g_brGlyph);    g_brGlyph = NULL; }
    if (g_brBtnPress) { DeleteObject(g_brBtnPress); g_brBtnPress = NULL; }
    if (g_brClose)    { DeleteObject(g_brClose);    g_brClose = NULL; }
    if (g_brClosePr)  { DeleteObject(g_brClosePr);  g_brClosePr = NULL; }
    if (g_penLine)    { DeleteObject(g_penLine);    g_penLine = NULL; }
    if (g_penAccent)  { DeleteObject(g_penAccent);  g_penAccent = NULL; }
    g_penLine    = CreatePen(PS_SOLID, 1, C_LINE);
    g_penAccent  = CreatePen(PS_SOLID, 1, C_ACCENT);   // 格子悬停/拖动焦点环
    g_brPinOff   = CreateSolidBrush(C_DIM);       // 钉住·未置顶
    g_brPinOn    = CreateSolidBrush(C_ACCENT);    // 钉住·已置顶
    g_brGlyph    = CreateSolidBrush(C_TEXT);      // 字形·悬停提亮
    g_brBtnPress = CreateSolidBrush(C_BTN_PR);    // 钉住·按下底
    g_brClose    = CreateSolidBrush(C_CLOSE);     // 关闭·悬停底
    g_brClosePr  = CreateSolidBrush(C_CLOSE_PR);  // 关闭·按下底
}

static void MakeFontsAndBrushes(void)
{
    MakeFonts();
    MakeBrushes();
    MakeGdiObjects();
}

// ---------------------------------------------------------------- 悬停文字
// 设置悬停文字（顺带置激活态），返回是否有变化——没变就不重绘标题栏。
// 比较放在写入前，调用方不必拷旧文字来对比（超长按显示的前 79 字符比）
static BOOL SetHover(const WCHAR *s, BOOL active)
{
    BOOL chg = active != g_hoverActive || wcmp_(s, g_hoverText) != 0;
    wcpy(g_hoverText, 80, s);
    g_hoverActive = active;
    return chg;
}

// 刷新悬停文字，返回是否变化（没变就不重绘标题栏）
static BOOL UpdateHoverText(void)
{
    if (g_hoverCell >= 0) {
        int ii = ItemAtView(g_hoverCell);
        if (ii >= 0) {
            if (IsBlank(ii)) return SetHover(L"空位：把程序拖进来", TRUE);
            const Item *it = &g_items[ii];
            if (g_filter[0]) {
                // 搜索中卡片：名字 + 所属分组——跨组搜索时用户需要知道条目住哪组
                WCHAR txt[96];
                wfmt(txt, 96, L"%s · %s", it->name, g_groups[it->group].name);
                return SetHover(txt, TRUE);
            }
            return SetHover(it->tip[0] ? it->tip : it->name, TRUE);
        }
    }
    if (g_hoverGroup >= 0) return SetHover(g_groups[g_hoverGroup].name, TRUE);
    switch (g_hoverBtn) {
    case 1: return SetHover(g_pinned ? L"取消置顶" : L"置顶窗口", TRUE);
    case 2: return SetHover(L"退出", TRUE);
    }
    if (RealCount() == 0) return SetHover(L"把程序拖进格子里", FALSE);
    return SetHover(g_groups[g_curGroup].name, FALSE);   // 闲时显示当前分组
}

// 返回悬停是否变化。鼠标在同一个格子上滑动是最高频事件：CellAt 一次算清
// 列/行/槽位（两除），与旧悬停比较后直接早退——不再单设"整格矩形包含"快路径
static BOOL HoverFromPoint(POINT p)
{
    int nc = CellAt(p);
    int ng = (nc >= 0) ? -1 : GroupAt(p);
    // 按钮只在标题栏里：光标在标题栏以下时连 BtnAt 都不必问
    // （格子间隙里移动也会走到这，省两次 PtInRect）
    int nb = (nc >= 0 || ng >= 0 || p.y >= g_titleH) ? 0 : BtnAt(p);
    if (nc == g_hoverCell && ng == g_hoverGroup && nb == g_hoverBtn)
        return FALSE;
    g_hoverCell = nc; g_hoverGroup = ng; g_hoverBtn = nb;
    return UpdateHoverText();
}

// ---------------------------------------------------------------- 悬停小卡片
// 悬停目标旁浮出卡片（类工具提示）。锚定悬停目标而非光标：锚光标每帧
// 重算+重绘；锚目标只在悬停变化时算一次，绘制阶段只读缓存、零分配
static void UpdateCard(void)
{
    const WCHAR *txt = NULL;
    RECT anc = { 0, 0, 0, 0 };                 // 锚点：悬停目标矩形
    if (g_hoverCell >= 0) {
        int ii = ItemAtView(g_hoverCell);
        if (ii >= 0 && !IsBlank(ii)) { txt = g_items[ii].name; anc = CellRect(g_hoverCell); }
    } else if (g_hoverGroup >= 0 && g_hoverGroup < g_nGroups) {   // 删组后下标可能已失效
        txt = g_groups[g_hoverGroup].name;
        anc = GroupRect(g_hoverGroup);
    }
    if (!txt || !txt[0]) { g_cardOn = FALSE; return; }   // 空格/空白/按钮不出卡片

    HDC dc = GetDC(g_hwnd);
    if (!dc) { g_cardOn = FALSE; return; }
    HFONT old = (HFONT)SelectObject(dc, g_font);
    RECT tr = { 0, 0, 0, 0 };
    DrawTextW(dc, txt, -1, &tr, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(g_hwnd, dc);

    int w = tr.right - tr.left + S_(16);
    int h = tr.bottom - tr.top + S_(8);
    if (w > g_cw - S_(8)) w = g_cw - S_(8);     // 超长名：收窄 + DT_END_ELLIPSIS
    // 上/下/左/右四个候选位置，完全放得下的里面取剩余空间最大的；
    // 一个都放不下（窗口被拉到极小）时也取空间最大的方向，交给钳制兜底
    int gap  = S_(4);
    int minX = S_(4), minY = g_titleH;              // 边界：不遮标题栏、不出窗口
    int maxX = g_cw - S_(4), maxY = g_ch - S_(4);
    int cx = (anc.left + anc.right) / 2, cy = (anc.top + anc.bottom) / 2;
    int X[4] = { cx - w / 2,        cx - w / 2,        anc.right + gap, anc.left - gap - w };
    int Y[4] = { anc.top - gap - h, anc.bottom + gap,  cy - h / 2,      cy - h / 2 };
    int nd[4] = { h, h, w, w };                     // 上下占高 h，左右占宽 w
    int sp[4] = { anc.top - minY, maxY - anc.bottom, maxX - anc.right, anc.left - minX };
    int pick = 0;
    for (int i = 1; i < 4; i++) {
        BOOL ifit = sp[i] >= nd[i], pfit = sp[pick] >= nd[pick];
        if (ifit && !pfit) pick = i;                         // 这个放得下、已选中的放不下
        else if (ifit == pfit && sp[i] > sp[pick]) pick = i;  // 都放得下/都放不下：空间大的优先
    }
    int x = X[pick], y = Y[pick];
    if (x < minX) x = minX;
    if (x + w > maxX) x = maxX - w;
    if (y < minY) y = minY;
    if (y + h > maxY) y = maxY - h;
    wcpy(g_cardText, 80, txt);
    g_cardRect.left = x; g_cardRect.top = y;
    g_cardRect.right = x + w; g_cardRect.bottom = y + h;
    g_cardOn = TRUE;
}

// 悬停目标/视图/名称/滚动变化后刷新：擦除旧卡片区域，重算并弄脏新区域
static void RefreshCard(void)
{
    if (g_cardOn) { InvalidateRect(g_hwnd, &g_cardRect, FALSE); g_cardOn = FALSE; }
    UpdateCard();
    if (g_cardOn) InvalidateRect(g_hwnd, &g_cardRect, FALSE);
}

// 悬停目标变化后的统一刷新（鼠标移动/键盘导航共用）：重绘旧/新格、侧栏、标题、卡片。
// ob（按钮悬停）不参与绘制——按钮没有悬停高亮，故不收
static void RefreshHoverSpots(int oc, int og, BOOL txtChg)
{
    InvalidateCell(oc);
    InvalidateCell(g_hoverCell);
    if (og != g_hoverGroup) InvalidateSidebar();   // 悬停行换了才擦侧栏
    if (txtChg) InvalidateTitle();   // 文字没变的悬停切换不碰标题栏
    RefreshCard();                   // 小卡片跟着悬停目标（内部含擦除旧区域）
}

// 搜索过滤词变化后：重建视图、钳制滚动、刷新悬停与整窗重绘。
// 键入是人类速率，全窗重绘可接受；悬停格在新视图里可能已换人，卡片/文字都要重算
static void ApplyFilter(void)
{
    ViewDirty();
    ClampScroll();
    UpdateHoverText();
    InvalidateRect(g_hwnd, NULL, FALSE);
    RefreshCard();
}

static void ClearFilter(void)
{
    if (!g_filter[0]) return;
    g_filter[0] = 0;
    ApplyFilter();
}

// ---------------------------------------------------------------- 绘制
// 字体沿用 PaintWindow 已选好的 g_font，这里只设文字色
static void DrawCard(HDC dc)
{
    RECT r = g_cardRect;
    int rad = S_(8);
    HPEN op = (HPEN)SelectObject(dc, g_penLine);
    HBRUSH ob = (HBRUSH)SelectObject(dc, g_brPanel);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);   // 圆角面板：填充+边框一枚调用
    SelectObject(dc, op); SelectObject(dc, ob);
    SetTextColor(dc, C_TEXT);
    DrawTextW(dc, g_cardText, -1, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

// 内缩 inset 的 1px 焦点框：格子拖动源/落点与分组重排落点共用同一强调色
static void FrameInset(HDC dc, RECT r, int inset, HBRUSH br)
{
    InflateRect(&r, -inset, -inset);
    FrameRect(dc, &r, br);
}

static void InvalidateCell(int vi)
{
    if (vi < 0) return;
    RECT r = CellRect(vi);
    InvalidateRect(g_hwnd, &r, FALSE);
}

// 重绘 [a, b] 闭区间格子：多文件拖放的落点区间重绘用。
// 区间在网格里连续——按行列算一个外接矩形一次失效，别逐格发 N 个更新区
static void InvalidateCells(int a, int b)
{
    if (a > b) { int t = a; a = b; b = t; }
    if (b < 0 || a >= g_slots) return;
    if (a < 0) a = 0;
    int c = g_cols;
    int ra = a / c, rb = b / c;
    int lo = (ra == rb) ? a % c : 0;      // 同行取两端，跨行铺满宽
    int hi = (ra == rb) ? b % c : c - 1;
    RECT r;
    r.left   = g_sideW + lo * (g_cell + g_gap);
    r.right  = g_sideW + hi * (g_cell + g_gap) + g_cell;
    r.top    = g_titleH + ra * (g_cell + g_gap) - g_scroll;
    r.bottom = g_titleH + rb * (g_cell + g_gap) + g_cell - g_scroll;
    InvalidateRect(g_hwnd, &r, FALSE);
}

// 拖动阈值判断（格子排序与分组重排共用）：位移超过系统拖动矩形即算拖动。
// 系统拖动矩形读一次缓存——拖动中每帧都问，而它不随 DPI 变
static BOOL PastDragThreshold(POINT cur, POINT org)
{
    static int cx = -1, cy = -1;
    if (cx < 0) {
        cx = GetSystemMetrics(SM_CXDRAG);
        cy = GetSystemMetrics(SM_CYDRAG);
    }
    int dx = cur.x - org.x, dy = cur.y - org.y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx > cx || dy > cy;
}

// 重绘单个分组行（侧栏重排的落点高亮擦除用）
static void InvalidateGroup(int gi)
{
    if (gi < 0) return;
    RECT r = GroupRect(gi);
    InvalidateRect(g_hwnd, &r, FALSE);
}

// 重绘 [a, b] 分组行区间：侧栏重排每次移动只发一个更新区。
// 行按 y 序排：上取 a 行顶、下取 b 行底即外接矩形（与 InvalidateCells 同构）
static void InvalidateGroups(int a, int b)
{
    if (a > b) { int t = a; a = b; b = t; }
    if (b < 0 || a >= g_nGroups) return;
    if (a < 0) a = 0;
    RECT r = GroupRect(a);
    r.bottom = GroupRect(b).bottom;
    InvalidateRect(g_hwnd, &r, FALSE);
}

static void InvalidateTitle(void)
{
    RECT r = { 0, 0, g_cw, g_titleH };
    InvalidateRect(g_hwnd, &r, FALSE);
}

// 侧栏整段失效（悬停行/选中行变化）：InvalidateTitle/InvalidateCell/InvalidateGroup 同族
static void InvalidateSidebar(void)
{
    RECT r = { 0, 0, g_sideW, g_ch };
    InvalidateRect(g_hwnd, &r, FALSE);
}

// 标题栏闪现一条消息 FLASH_MS（时间戳驱动，不占新定时器；过期重绘由 TIMER_HIDE 顺带做）
static void FlashMsg(const WCHAR *text)
{
    wcpy(g_flashText, 80, text);
    g_flashAt = GetTickCount64();
    InvalidateTitle();
}

// 闪现一条格式化消息：启动结果、剪贴板反馈等“文字里带变量”的场景共用
static void FlashFmt(const WCHAR *fmt, ...)
{
    WCHAR t[128];
    va_list ap;
    va_start(ap, fmt);
    wfmtv(t, 128, fmt, ap);
    va_end(ap);
    FlashMsg(t);
}

static void FlashLaunch(BOOL ok, const WCHAR *name)
{
    FlashFmt(ok ? L"已启动: %s" : L"启动失败: %s", name);
}

static BOOL FlashActive(void)
{
    return g_flashAt && GetTickCount64() - g_flashAt < FLASH_MS;
}

// 启动视图位 vi 的格子并在标题栏闪现结果（单击抬起/回车/数字键 1-9 共用）
static void LaunchView(int vi)
{
    int ii = ItemAtView(vi);
    if (ii >= 0 && !IsBlank(ii))
        FlashLaunch(Launch(ii, FALSE), g_items[ii].name);
}

// 置顶开关：标题栏图钉点击与托盘菜单共用。置顶 = 常驻不自动隐藏 + HWND_TOPMOST
static void TogglePin(void)
{
    g_pinned = !g_pinned;
    SetWindowPos(g_hwnd, g_pinned ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SaveConfig();
    UpdateHoverText();
    InvalidateTitle();
    UpdateTrayTip();      // 托盘提示里的“置顶”标记跟着变
}

// ---------------------------------------------------------------- 标题栏按钮图形
// 视觉语言：全 GDI 原生图形，无位图资源，随 S_() 缩放。悬停/按下底色是
// 胶囊形（关闭键悬停转标题栏惯例暗红）；× 用两段 45° 旋转矩形——GDI 线帽
// 是方的，斜线不用多边形会阶梯；钉住是地图钉剪影，色即状态（暗=未置顶）

// 45° 粗描边：以 (cx,cy) 为中心、方向 (sx,sy)（各 ∈ {-1,1}）、长 len 粗 th 的
// 一根斜“胶囊”。th 与 len 都是 DPI 缩放后传入的。
// 1/√2 用 Q8 定点（181/256 = 0.70703，误差 1e-4，最长 40px 的线也只差 0.004px）：
// 整条绘制路径不碰浮点——窗口过程里的 double 会把浮点支持和整套数学表拖进镜像
static void Stroke45(HDC dc, int cx, int cy, int sx, int sy, int len, int th)
{
    const int dl = (181 * len + 256) >> 9;   // 沿线半长分量（四舍五入，同 Rnd）
    const int dt = (181 * th + 256) >> 9;    // 垂线半粗分量（th ≥ 2，不会退化成 0）
    const int ax = sx * dl, ay = sy * dl;    // 沿线向量
    const int bx = -sy * dt, by = sx * dt;   // 垂线向量
    POINT q[4] = {
        { cx + ax + bx, cy + ay + by },
        { cx + ax - bx, cy + ay - by },
        { cx - ax - bx, cy - ay - by },
        { cx - ax + bx, cy - ay + by },
    };
    Polygon(dc, q, 4);
}

// 关闭 ×：常态暗、悬停亮（底色由 PaintTitleButtons 给）
static void DrawCloseGlyph(HDC dc, RECT r, HBRUSH br)
{
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    HBRUSH ob = (HBRUSH)SelectObject(dc, br);
    Stroke45(dc, cx, cy, -1, -1, S_(10), S_(2));
    Stroke45(dc, cx, cy, +1, -1, S_(10), S_(2));
    SelectObject(dc, ob);
}

// 钉住：地图钉剪影。钉尾三角形的底边埋进圆头里，两段填充视觉上成一整枚
static void DrawPinGlyph(HDC dc, RECT r, HBRUSH br)
{
    int cx = (r.left + r.right) / 2;
    int cy = (r.top + r.bottom) / 2 - S_(1);
    int rad = S_(4);
    HBRUSH ob = (HBRUSH)SelectObject(dc, br);
    Ellipse(dc, cx - rad, cy - rad, cx + rad, cy + rad);
    POINT tail[3] = {
        { cx - rad + S_(1), cy + rad - S_(2) },
        { cx + rad - S_(1), cy + rad - S_(2) },
        { cx, cy + rad + S_(5) },
    };
    Polygon(dc, tail, 3);
    SelectObject(dc, ob);
}

// 按钮一览：悬停/按下胶囊底 + 字形。状态判定集中在同一个循环里，
// PaintTitle 只发一次调用；两枚钮同构，加第三枚只是往枚举里添一行
static void PaintTitleButtons(HDC dc)
{
    for (int b = BTN_PIN; b <= BTN_CLOSE; b++) {
        RECT r = g_btnRect[b];
        BOOL hov = (g_hoverBtn == b), prs = (g_pressBtn == b);
        if (hov || prs) {
            HBRUSH bg = prs ? (b == BTN_CLOSE ? g_brClosePr : g_brBtnPress)
                            : (b == BTN_CLOSE ? g_brClose : g_brCellHi);
            RECT pill = r;
            InflateRect(&pill, -S_(1), -S_(1));
            int rad = (pill.bottom - pill.top) / 2;   // 半高半径 = 全胶囊
            HPEN op = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
            HBRUSH ob = (HBRUSH)SelectObject(dc, bg);
            RoundRect(dc, pill.left, pill.top, pill.right, pill.bottom, rad, rad);
            SelectObject(dc, op); SelectObject(dc, ob);
        }
        HBRUSH glyph;
        if (b == BTN_CLOSE)
            glyph = hov ? g_brGlyph : g_brPinOff;              // × ：暗 / 亮
        else
            glyph = hov ? g_brGlyph : (g_pinned ? g_brPinOn : g_brPinOff);
        if (b == BTN_CLOSE) DrawCloseGlyph(dc, r, glyph);
        else                DrawPinGlyph(dc, r, glyph);
    }
}

static void PaintSidebar(HDC dc, const RECT *clip)
{
    RECT sr = { 0, 0, g_sideW, g_ch };
    FillRect(dc, &sr, g_brSide);
    RECT ln = { g_sideW - 1, 0, g_sideW, g_ch };
    FillRect(dc, &ln, g_brLine);

    RECT hit;
    const int pad = S_(4);
    for (int i = 0; i < g_nGroups; i++) {
        RECT r = GroupRect(i);
        if (r.bottom < g_titleH || r.top > g_ch) continue;
        if (clip && !IntersectRect(&hit, &r, clip)) continue;   // 行不在脏区：整行跳过
        BOOL sel = (i == g_curGroup), hov = (i == g_hoverGroup);
        // 选中行常亮底 + 下方指示条；未选中行悬停时给同一层浅底，让
        // “这一行有反应”可感知（两者互斥取同一刷子，不会叠加）
        if (sel || hov) FillRect(dc, &r, g_brCell);
        SetTextColor(dc, sel ? C_ACCEL : (hov ? C_TEXT : C_DIM));
        RECT tr = { r.left + pad, r.top, r.right - pad, r.bottom };
        DrawTextW(dc, g_groups[i].name, -1, &tr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        // 选中标记放在下方（原来在左侧）
        if (sel) {
            RECT bar = { r.left, r.bottom - g_grpSelBar, r.right, r.bottom };
            FillRect(dc, &bar, g_brAccel);
        }
        // 分组重排的落点高亮（源行即当前选中行，已有底色，不重复画）
        if (g_dragGroup && i == g_dropGroup && i != g_pressGroup)
            FrameInset(dc, r, S_(2), g_brAccent);
        // 外部拖放悬停的落组高亮：与重排落点同一视觉语言（accent 内框）
        if (g_extDrag && i == g_oleGrp)
            FrameInset(dc, r, S_(2), g_brAccent);
    }

    // 分组超出可视高度时，右侧画一个细滚动条，让“可以滚”看得见
    int content = SidebarContentH();
    int view = g_ch - g_titleH;
    if (content > view) {
        int trackTop = g_titleH + S_(4);
        int trackH = view - S_(8);
        int thumbH = trackH * view / content;
        if (thumbH < S_(20)) thumbH = S_(20);
        if (thumbH > trackH) thumbH = trackH;
        int maxS = content - view;
        int thumbY = trackTop + (maxS > 0 ? (trackH - thumbH) * g_grpScroll / maxS : 0);
        RECT th = { g_sideW - S_(5), thumbY, g_sideW - S_(3), thumbY + thumbH };
        FillRect(dc, &th, g_brLine);
    }
}

static void PaintTitle(HDC dc)
{
    HFONT old = (HFONT)SelectObject(dc, g_fontT);
    RECT ttl = { g_sideW + S_(8), 0, g_cw - g_btnZoneW - S_(8), g_titleH };
    if (FlashActive()) {
        // 启动反馈优先：刚点过启动，先让用户看到结果（悬停/搜索都让位）
        SetTextColor(dc, C_TEXT);
        DrawTextW(dc, g_flashText, -1, &ttl,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    } else if (g_filter[0]) {
        // 搜索中：标题栏让位给过滤词（左对齐 + 命中数），悬停详情看小卡片
        WCHAR txt[64];
        wfmt(txt, 64, L"搜索: %s (%d·全部分组)", g_filter, ViewCount());
        SetTextColor(dc, C_TEXT);
        DrawTextW(dc, txt, -1, &ttl,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    } else {
        SetTextColor(dc, g_hoverActive ? C_TEXT : C_DIM);
        DrawTextW(dc, g_hoverText, -1, &ttl,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, old);
    PaintTitleButtons(dc);    // 钉住/关闭：悬停底、按下底、字形
    // 标题栏底部 1px 分隔线：与侧栏右边线连贯，把头部与内容区从视觉上分开
    RECT ln = { 0, g_titleH - 1, g_cw, g_titleH };
    FillRect(dc, &ln, g_brLine);
}

static void PaintGrid(HDC dc, const RECT *clip)
{
    const int step   = g_cell + g_gap;
    const int isz    = IconSize();
    const int n      = ViewCount();               // 一次定表：循环里直读 g_viewIdx
    const int limit  = n > g_slots ? n : g_slots;
    const int rad0   = g_cell / 2 - 1;
    const int rad    = rad0 > S_(6) ? S_(6) : rad0;
    // 每格要用的缩放量全部提到循环外：原来每格 5 次 S_、两次除法
    const int lblIn  = S_(2), lblTop = S_(17), lblBot = S_(3);
    const int iconX  = (g_cell - isz) / 2;        // 图标在格内的相对位置
    const int iconY  = (g_cell - S_(20) - isz) / 2;
    // 边框 pen / 底色 brush 循环外只选一次（普通态）；一枚 RoundRect 同时
    // 完成填充+边框+圆角。悬停/拖动格换 accent 边——焦点环贴着圆角走
    // （替代旧的内缩 FrameRect），悬停重绘反而少一次 GDI 调用
    HPEN   op = (HPEN)SelectObject(dc, g_penLine);
    HBRUSH ob = (HBRUSH)SelectObject(dc, g_brCell);
    SetTextColor(dc, C_TEXT);            // 所有格子的标签同色：循环外设一次
    RECT hit;
    for (int row = 0; row * g_cols < limit; row++) {
        int y = g_titleH + row * step - g_scroll;
        if (y >= g_ch) break;                       // 整行在可视区下方
        if (y + g_cell <= g_titleH) continue;       // 整行已滚出上方
        int x = g_sideW;
        int vi = row * g_cols;
        for (int col = 0; col < g_cols; col++, x += step, vi++) {
            RECT r = { x, y, x + g_cell, y + g_cell };
            // 脏区剔除：悬停/拖动时只脏 1~2 格，与更新区不相交的整格连 GDI 调用都不发
            if (clip && !IntersectRect(&hit, &r, clip)) continue;
            int ii = vi < n ? g_viewIdx[vi] : -1;   // 视图表直读：省掉脏检查与边界重验
            BOOL hov = (vi == g_hoverCell) && ii >= 0;
            // 拖动标示两路共用：内部排序（g_dragging）与外部 OLE 拖放（g_extDrag，
            // 无按下格概念）互斥，任一激活时落点格画常规底 + accent 边
            BOOL drag = (g_dragging && (vi == g_pressCell ||
                                        (g_dropIn && vi == g_dropCell && vi != g_pressCell)))
                     || (g_extDrag && g_dropIn && vi == g_dropCell);
            if (hov || drag) {          // 焦点态：accent 边 + 提亮底（悬停）/常规底（拖动标示）
                SelectObject(dc, g_penAccent);
                SelectObject(dc, hov ? g_brCellHi : g_brCell);
            }
            RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
            if (hov || drag) {          // 恢复普通态对象，后续格子不必重选
                SelectObject(dc, g_penLine);
                SelectObject(dc, g_brCell);
            }
            if (ii < 0 || !g_items[ii].path[0]) continue;   // 空格：只画底框，无图标/名字
            if (!g_items[ii].iconTried) EnsureIcon(ii);     // 已解析的格子省一次调用
            if (g_items[ii].icon)
                DrawIconEx(dc, x + iconX, y + iconY, g_items[ii].icon, isz, isz,
                           0, NULL, DI_NORMAL);
            RECT lr = { x + lblIn, y + g_cell - lblTop,
                        x + g_cell - lblIn, y + g_cell - lblBot };
            DrawTextW(dc, g_items[ii].name, -1, &lr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }
    SelectObject(dc, ob); SelectObject(dc, op);
}

// clip = 双缓冲路径的更新区（就是 memDC 当前的裁剪区，WM_PAINT 直接传入，
// 不必每次绘制再 GetClipBox 问一遍）；NULL = 直绘兜底，全画
static void PaintWindow(HDC dc, const RECT *clip)
{
    RECT rc = { 0, 0, g_cw, g_ch };
    FillRect(dc, &rc, g_brBG);           // DC 带裁剪时本条只碰脏区
    // 三个绘制段共用同一正文字体/透明背景：选一次就够，各段不再重复 SelectObject
    HFONT old = (HFONT)SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);

    RECT hit;
    RECT sr = { 0, 0, g_sideW, g_ch };
    RECT tr = { 0, 0, g_cw, g_titleH };
    RECT gr = { g_sideW, g_titleH, g_cw, g_ch };
    if (!clip || IntersectRect(&hit, &sr, clip)) PaintSidebar(dc, clip);
    if (!clip || IntersectRect(&hit, &tr, clip)) PaintTitle(dc);
    if (!clip || IntersectRect(&hit, &gr, clip)) PaintGrid(dc, clip);
    // 小卡片最后画：压在侧栏/标题/格子之上；脏区不碰卡片时整段跳过
    if (g_cardOn && (!clip || IntersectRect(&hit, &g_cardRect, clip))) DrawCard(dc);
    SelectObject(dc, old);
}

// comdlg32 手写延迟加载："浏览"按钮才用到。/DELAYLOAD 要拖 delayimp 与 load
// config 依赖，自己 LoadLibrary 更干净——导入表上也彻底没有 comdlg32 这一项
typedef BOOL (WINAPI *PFN_GetOpenFileNameW)(LPOPENFILENAMEW);

static PFN_GetOpenFileNameW GetOpenFileProc(void)
{
    static PFN_GetOpenFileNameW p;
    if (!p) {
        HMODULE h = LoadLibraryW(L"comdlg32.dll");
        if (h) p = (PFN_GetOpenFileNameW)GetProcAddress(h, "GetOpenFileNameW");
    }
    return p;
}

// ---------------------------------------------------------------- 对话框（格子属性 / 重命名）
typedef struct {
    HWND     hwnd;
    WCHAR    title[64];
    DlgField f[6];
    int      n;
    HWND     ed[6];
    BOOL     ok;
    int      dlgW, rowH, labW, titleH, browseW;
} DlgCtx;

static DlgCtx g_dlg;

static void BrowseForFile(HWND edit)
{
    WCHAR file[MAX_PATH] = L"";
    GetWindowTextW(edit, file, MAX_PATH);
    OPENFILENAMEW of;
    ZeroMemory(&of, sizeof of);
    of.lStructSize = sizeof of;
    of.hwndOwner = g_dlg.hwnd;
    of.lpstrFilter = L"程序\0*.exe;*.lnk;*.bat;*.cmd\0所有文件\0*.*\0\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    // OFN_NOCHANGEDIR：浏览对话框默认会把进程当前目录改成用户看过的目录，
    // 而无工作目录的条目直启时 lpCurrentDirectory=NULL（继承我们的 CWD）——
    // 不拦这一下，浏览过一次之后所有启动的 CWD 都漂了
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    PFN_GetOpenFileNameW fn = GetOpenFileProc();
    if (fn && fn(&of)) SetWindowTextW(edit, file);
}

// ---------------------------------------------------------------- 对话框：消息处理
// 与主窗同一风格：每条消息一个小函数，DlgProc 只做分发

static void OnDlgCreate(HWND h)
{
    DlgCtx *c = &g_dlg;
    int y = c->titleH + S_(10);
    for (int i = 0; i < c->n; i++) {
        HWND st = CreateWindowExW(0, L"STATIC", c->f[i].label,
                                  WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                  S_(12), y + S_(4), c->labW - S_(8), S_(18),
                                  h, (HMENU)(INT_PTR)(2000 + i), g_inst, NULL);
        int ex = S_(16) + c->labW;
        int ew = c->dlgW - ex - S_(14) - (c->f[i].mode ? c->browseW + S_(6) : 0);
        HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", c->f[i].buf,
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | ES_AUTOHSCROLL,
                                  ex, y, ew, S_(23), h,
                                  (HMENU)(INT_PTR)(ID_EDIT_BASE + i), g_inst, NULL);
        if (st) SendMessageW(st, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(ed, WM_SETFONT, (WPARAM)g_font, TRUE);
        c->ed[i] = ed;
        if (c->f[i].mode) {
            HWND b = CreateWindowExW(0, L"BUTTON", L"浏览(&B)...",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                     c->dlgW - S_(14) - c->browseW, y, c->browseW, S_(23),
                                     h, (HMENU)(INT_PTR)(DLG_BROWSE + i), g_inst, NULL);
            SendMessageW(b, WM_SETFONT, (WPARAM)g_font, TRUE);
        }
        y += c->rowH;
    }
    int bw = S_(84), bh = S_(27);
    int by = c->titleH + S_(10) + c->n * c->rowH + S_(12);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"确定",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              c->dlgW - S_(14) - bw * 2 - S_(8), by, bw, bh,
                              h, (HMENU)(INT_PTR)DLG_OK, g_inst, NULL);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"取消",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  c->dlgW - S_(14) - bw, by, bw, bh,
                                  h, (HMENU)(INT_PTR)DLG_CANCEL, g_inst, NULL);
    SendMessageW(ok, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(cancel, WM_SETFONT, (WPARAM)g_font, TRUE);
    SetFocus(c->n > 0 ? c->ed[0] : ok);
    if (c->n > 0) SendMessageW(c->ed[0], EM_SETSEL, 0, -1);
}

static void OnDlgCommand(HWND h, WPARAM w)
{
    int id = LOWORD(w), code = HIWORD(w);
    if (id == DLG_OK || id == DLG_CANCEL) {
        if (id == DLG_OK) {
            for (int i = 0; i < g_dlg.n; i++)
                GetWindowTextW(g_dlg.ed[i], g_dlg.f[i].buf, g_dlg.f[i].cch);
            g_dlg.ok = TRUE;
        }
        DestroyWindow(h);
        return;
    }
    // 只在按钮真正被点击时打开浏览框（否则输入框的 EN_* 通知会误触发）
    if (code == BN_CLICKED && id >= DLG_BROWSE && id < DLG_BROWSE + 6) {
        int i = id - DLG_BROWSE;
        if (g_dlg.f[i].mode == 1) BrowseForFile(g_dlg.ed[i]);
    }
}

static void OnDlgPaint(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    FillRect(dc, &rc, g_brPanel);
    RECT sep = { 0, g_dlg.titleH, rc.right, g_dlg.titleH + 1 };
    FillRect(dc, &sep, g_brLine);
    HFONT old = (HFONT)SelectObject(dc, g_fontT);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, C_TEXT);
    RECT tr = { S_(14), 0, rc.right - S_(14), g_dlg.titleH };
    WCHAR ttl[64] = L"";
    GetWindowTextW(h, ttl, 64);
    DrawTextW(dc, ttl, -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
    EndPaint(h, &ps);
}

// 标题栏整条都是拖动命中区；客户区其余部分转交默认过程（控件命中）
static LRESULT OnDlgNcHitTest(HWND h, LPARAM l)
{
    POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
    ScreenToClient(h, &p);
    if (p.y < g_dlg.titleH) return HTCAPTION;
    return 0;
}

// ---------------------------------------------------------------- 对话框
static LRESULT CALLBACK DlgProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:            OnDlgCreate(h);                     return 0;
    case WM_COMMAND:           OnDlgCommand(h, w);                 return 0;
    case WM_CLOSE:             DestroyWindow(h);                   return 0;
    case WM_DESTROY:           PostQuitMessage(0);                 return 0;
    case WM_ERASEBKGND:      { RECT rc; GetClientRect(h, &rc);
                               FillRect((HDC)w, &rc, g_brPanel);  return 1; }
    case WM_PAINT:             OnDlgPaint(h);                      return 0;
    case WM_CTLCOLORSTATIC:    SetBkMode((HDC)w, TRANSPARENT);
                               SetTextColor((HDC)w, C_DIM);
                                                     return (LRESULT)g_brPanel;
    case WM_CTLCOLOREDIT:      SetTextColor((HDC)w, C_TEXT);
                               SetBkColor((HDC)w, C_CELL);
                                                     return (LRESULT)g_brCell;
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:       SetTextColor((HDC)w, C_TEXT);
                               SetBkColor((HDC)w, C_PANEL);
                                                     return (LRESULT)g_brPanel;
    case WM_NCHITTEST:       { LRESULT ht = OnDlgNcHitTest(h, l);
                               if (ht) return ht; }              break;
    default:                                                            break;
    }
    return DefWindowProcW(h, m, w, l);
}
// 模态小窗的公共骨架：居中于宿主 → 建窗 → 置前 → 禁用宿主 → 自己的消息循环 →
// 恢复宿主。属性框与热键捕获框只差窗口类、尺寸、是否走 IsDialogMessage
//（对话框要 Tab/方向键导航，捕获框自己吃 WM_KEYDOWN）。建窗失败返回 NULL。
// 模态期置 g_dlgOpen：心跳冻结自动隐藏、shell 调用回避，面板不会在窗下溜走
static HWND RunModal(HWND owner, const WCHAR *cls, const WCHAR *title,
                     int w, int h, BOOL tabNav)
{
    RECT or_;
    GetWindowRect(owner, &or_);
    HWND dlg = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_CONTROLPARENT,
                               cls, title, WS_POPUP | WS_CLIPCHILDREN,
                               or_.left + ((or_.right - or_.left) - w) / 2,
                               or_.top + ((or_.bottom - or_.top) - h) / 2,
                               w, h, owner, NULL, g_inst, NULL);
    if (!dlg) return NULL;

    g_dlgOpen = TRUE;
    ShowWindow(dlg, SW_SHOW);
    ForceForeground(dlg);
    EnableWindow(owner, FALSE);          // 等价于 DisableWindow（SDK 未声明后者）
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!tabNav || !IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    g_dlgOpen = FALSE;
    return dlg;
}

// 属性框/重命名框共用：模态小窗（IsDialogMessage 处理 Tab/方向键），
// 关闭后由调用方读 g_dlg.ok 决定是否应用。字段值直接写回调用方的缓冲
static BOOL RunDialog(HWND owner, const WCHAR *title, void *fields, int n)
{
    DlgField *f = (DlgField *)fields;
    ZeroMemory(&g_dlg, sizeof g_dlg);
    for (int i = 0; i < n && i < 6; i++) g_dlg.f[i] = f[i];
    g_dlg.n = n;
    wcpy(g_dlg.title, 64, title);
    g_dlg.dlgW   = S_(540);
    g_dlg.titleH = S_(34);
    g_dlg.rowH   = S_(32);
    g_dlg.labW   = S_(100);
    g_dlg.browseW = S_(86);

    int dlgH = g_dlg.titleH + S_(10) + n * g_dlg.rowH + S_(12) + S_(27) + S_(14);
    // 对话框窗口类已在 wWinMain 注册一次，这里不再重复注册
    g_dlg.hwnd = RunModal(owner, DLG_CLASS, title, g_dlg.dlgW, dlgH, TRUE);
    if (!g_dlg.hwnd) return FALSE;
    BOOL ok = g_dlg.ok;
    g_dlg.hwnd = NULL;
    return ok;
}

// ---------------------------------------------------------------- 开机自启
// HKCU 的 Run 键是唯一事实源：启动时读一次供菜单勾选，切换时直接写/删，
// 不镜像进 claunch.ini——避免两处状态互相不一致。HKCU 无需管理员权限
#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

// 读出自启项登记的路径（无该项返回 FALSE）。容量按写入上限（引号+全路径+NUL）给足
static BOOL QueryAutostart(WCHAR *pathOut, int cch)
{
    if (!pathOut || cch <= 0) return FALSE;      // 防御：NULL 目标在 RegQueryValueExW 之后必崩
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return FALSE;
    DWORD cb = (DWORD)cch * sizeof(WCHAR);
    LONG r = RegQueryValueExW(k, APPNAME, NULL, NULL, (BYTE *)pathOut, &cb);
    RegCloseKey(k);
    return r == ERROR_SUCCESS && pathOut[0];
}

// 返回是否切换成功（失败时菜单勾选保持不变）
static BOOL SetAutostart(BOOL on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_WRITE, &k) != ERROR_SUCCESS)
        return FALSE;
    LONG r;
    if (on) {
        WCHAR path[MAX_PATH], cmd[MAX_PATH + 4];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        wfmt(cmd, MAX_PATH + 4, L"\"%s\"", path);   // 路径可能含空格：加引号
        r = RegSetValueExW(k, APPNAME, 0, REG_SZ, (const BYTE *)cmd,
                           (DWORD)(wlen_(cmd) + 1) * sizeof(WCHAR));
    } else {
        r = RegDeleteValueW(k, APPNAME);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;   // 本来就没有：算成功
    }
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static void ToggleAutostart(void)
{
    if (SetAutostart(!g_autostart)) {
        g_autostart = !g_autostart;
        UpdateTrayTip();      // 托盘提示里的“自启”标记跟着变
        FlashFmt(L"开机自启动已%s", g_autostart ? L"开启" : L"关闭");
    }
}

// 开机自启初始化：读状态供菜单勾选；登记的路径与当前 exe 不一致
// （程序被移动/重装到别处）就地重写，免得登录时静默失效
static void InitAutostart(void)
{
    WCHAR runPath[MAX_PATH + 8] = L"";
    g_autostart = QueryAutostart(runPath, MAX_PATH + 8);
    if (!g_autostart) return;
    WCHAR *q = runPath;                       // 写入时加了引号，比对前剥掉
    if (*q == L'"') { q++; WCHAR *e = wchr_(q, L'"'); if (e) *e = 0; }
    WCHAR self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    if (wcmp_(q, self) != 0) SetAutostart(TRUE);
}

// ---------------------------------------------------------------- 托盘
// 运行时画一个 2x2 格子图标：不必引入 .ico 资源，保持单文件

// 组装托盘提示：当前分组 + 激活的开关（全关时退化）。开关状态一目了然，
// 不用每次都右键开菜单看勾选；分组名让悬停一眼知道面板停在哪组。
// 静默启动已是唯一行为（配置可手工改），不再是“开关”所以不进提示
static void BuildTrayTip(WCHAR *out, int cch)
{
    const WCHAR *grp = (g_curGroup >= 0 && g_curGroup < g_nGroups)
                     ? g_groups[g_curGroup].name : L"";
    // 顺序拼装，分隔符随项走：原实现先攒 flags 再四分支 swprintf，
    // 六条格式串里有一半是重复的
    wcpy(out, cch, L"桌面格子");
    if (grp[0]) { wcat(out, cch, L"["); wcat(out, cch, grp); wcat(out, cch, L"]"); }
    if (g_pinned)    wcat(out, cch, L" — 置顶");
    if (g_autostart) wcat(out, cch, L" — 自启");
    wcat(out, cch, L" — 单击显示，右键菜单");
}

// 开关变化后刷新托盘提示（无托盘图标时是空操作；只改 tip，不动图标/回调）
static void UpdateTrayTip(void)
{
    if (!g_trayIcon) return;
    BuildTrayTip(g_nid.szTip, 64);
    // NIF_SHOWTIP 不能省：NOTIFYICON_VERSION_4 行为下不带它，shell 存下
    // szTip 却不弹悬停提示——鼠标浮动到托盘图标上什么都没有
    g_nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static HICON MakeTrayIcon(int sz)
{
    IconCanvas cv;
    if (!CanvasBegin(&cv, sz)) return NULL;

    RECT all = { 0, 0, sz, sz };
    HBRUSH bg = CreateSolidBrush(C_BG);
    FillRect(cv.dc, &all, bg);
    DeleteObject(bg);

    // 2x2 浅色格子
    int pad = sz / 6, gap = sz / 10;
    int cw = (sz - pad * 2 - gap) / 2;
    HBRUSH fg = CreateSolidBrush(RGB(0xEC, 0xED, 0xEF));
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            RECT r = { pad + j * (cw + gap), pad + i * (cw + gap),
                       pad + j * (cw + gap) + cw, pad + i * (cw + gap) + cw };
            FillRect(cv.dc, &r, fg);
        }
    DeleteObject(fg);

    // 圆角外挖空：keep 区之外的像素 alpha 清 0（RGB 保留，只动 alpha 字节）
    HRGN rgn = CreateRoundRectRgn(0, 0, sz + 1, sz + 1, sz / 3, sz / 3);
    HICON ico = CanvasEnd(&cv, sz, rgn);
    if (rgn) DeleteObject(rgn);
    return ico;
}

// 一次性气球（热键占用等异常）：不告知的话用户只会觉得"按了没反应"。
// V4 下 uTimeout 由系统接管，仍给值兜底
static void TrayBalloon(const WCHAR *text)
{
    if (!g_trayIcon) return;
    NOTIFYICONDATAW nid = g_nid;               // 结构拷贝：cbSize/hWnd/uID 等照旧
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;               // 信息级：不发声不闪烁
    wcpy(nid.szInfo, 256, text);
    nid.uTimeout = 8000;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void TrayAdd(void)
{
    if (!g_trayIcon) {
        g_traySz = GetSystemMetrics(SM_CYSMICON);
        g_trayIcon = MakeTrayIcon(g_traySz);
    }
    if (!g_trayIcon) return;

    ZeroMemory(&g_nid, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_hwnd;
    g_nid.uID = TRAY_ID;
    // NIF_SHOWTIP 与 NIF_TIP 必须同时给：V4 行为下只给 NIF_TIP 的话，
    // szTip 存得进去但悬停时不弹提示（实测复现：鼠标浮动托盘什么都没有）
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_trayIcon;
    BuildTrayTip(g_nid.szTip, 64);
    // NIM_ADD 失败=图标已存在（Explorer 没重启但 TaskbarCreated 误报、
    // 或重复添加）：静默失败会让托盘里什么都没有，回落 NIM_MODIFY 刷内容
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid))
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

static void TrayRemove(void)
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (g_trayIcon) { DestroyIcon(g_trayIcon); g_trayIcon = NULL; }
}

// ---------------------------------------------------------------- 热键捕获对话框
// 模态小窗：按下“Ctrl/Alt 之一 + 主键”即录用，Esc（无修饰）取消。
// 修饰键先按后松都常见——主键到达那一刻读实时修饰状态即当前组合。
// Win 组合天然设不出（只读 Ctrl/Alt/Shift 三个键的实时状态）
static UINT g_capMods, g_capVk;
static BOOL  g_capOk;

// 实时修饰键状态（捕获框预览用）
static UINT LiveMods(void)
{
    UINT mods = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)    mods |= MOD_ALT;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   mods |= MOD_SHIFT;
    return mods;
}

static BOOL IsModVk(UINT vk)
{
    return vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT ||
           vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL;
}

static LRESULT CALLBACK CapProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_KEYDOWN: {
        UINT mods = LiveMods();
        UINT vk = (UINT)w;
        if (vk == VK_ESCAPE && !mods) { g_capOk = FALSE; DestroyWindow(h); return 0; }
        if (IsModVk(vk)) { InvalidateRect(h, NULL, FALSE); return 0; }   // 修饰键：刷新预览
        if (!(mods & (MOD_CONTROL | MOD_ALT))) return 0;   // 裸字母/功能键不收
        g_capMods = mods; g_capVk = vk; g_capOk = TRUE;
        DestroyWindow(h);
        return 0;
    }
    case WM_KEYUP:
        if (IsModVk((UINT)w)) InvalidateRect(h, NULL, FALSE);   // 松修饰键：预览回退
        return 0;
    case WM_CLOSE:   DestroyWindow(h);   return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_ERASEBKGND: { RECT rc; GetClientRect(h, &rc);
                          FillRect((HDC)w, &rc, g_brPanel); return 1; }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        SetBkMode(dc, TRANSPARENT);
        HFONT old = (HFONT)SelectObject(dc, g_font);
        SetTextColor(dc, C_DIM);
        RECT tr0 = { S_(20), S_(14), rc.right - S_(20), S_(38) };
        WCHAR cur[80];
        HotkeyName(cur, 80);
        WCHAR line0[96];
        wfmt(line0, 96, L"当前唤醒快捷键：%s", cur);
        DrawTextW(dc, line0, -1, &tr0,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        // 预览行：实时修饰键 + 省略号（主键未按）——按着修饰键就能看到
        // 将要录用的组合，不用盲按
        UINT mods = LiveMods();
        WCHAR prev[48], pv[24];
        ModsName(pv, 24, mods);
        if (pv[0]) wfmt(prev, 48, L"%s…", pv);
        else       wcpy(prev, 48, L"（等待按键）");
        SetTextColor(dc, C_TEXT);
        RECT tr1 = { S_(20), S_(44), rc.right - S_(20), S_(76) };
        DrawTextW(dc, prev, -1, &tr1,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SetTextColor(dc, C_DIM);
        RECT tr2 = { S_(20), S_(88), rc.right - S_(20), S_(112) };
        DrawTextW(dc, L"需要包含 Ctrl 或 Alt；按 Esc 取消", -1, &tr2,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

// 捕获新组合；成功写回 *mods/*vk（模态骨架与 RunDialog 同一套，只是不走
// IsDialogMessage——按键由 CapProc 自己判定）
static BOOL RunHotkeyCapture(HWND owner, UINT *mods, UINT *vk)
{
    g_capOk = FALSE;
    if (!RunModal(owner, CAP_CLASS, L"设置唤醒快捷键", S_(380), S_(132), FALSE))
        return FALSE;
    if (g_capOk) { *mods = g_capMods; *vk = g_capVk; }
    return g_capOk;
}

static void TrayShowMenu(void)
{
    // 开关项点击后菜单就地重弹（打钩即时更新）：标准弹出菜单选中即收，
    // 用关闭-重弹循环模拟"点一下钩原地翻"——光标没动，观感是当场变了
    for (;;) {
        // 自启以 HKCU Run 键为唯一事实源：每轮弹菜单前重读，别处动过也当即反映
        WCHAR runPath[MAX_PATH + 8];
        g_autostart = QueryAutostart(runPath, MAX_PATH + 8);

        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, IDM_TRAY_SHOW, L"显示主面板");
        AppendMenuW(m, MF_STRING, IDM_TRAY_HOTKEY, L"设置唤醒快捷键(&H)...");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING | (g_pinned ? MF_CHECKED : 0), IDM_TRAY_PIN, L"置顶窗口");
        // 自启是“重启后生效”的持久偏好（而非即时开关）：勾了当下没有可见
        // 效果，文本里注明生效时机，免得看着像打钩没反应
        AppendMenuW(m, MF_STRING | (g_autostart ? MF_CHECKED : 0), IDM_TRAY_AUTO,
                    L"开机自启动（重启生效）");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出程序");
        int cmd = PopupAt(m);

        if (cmd == IDM_TRAY_PIN)    { TogglePin();       continue; }
        if (cmd == IDM_TRAY_AUTO)   { ToggleAutostart(); continue; }
        if (cmd == IDM_TRAY_HOTKEY) {
            UINT nm = g_hotMods, nv = g_hotVk;
            if (RunHotkeyCapture(g_hwnd, &nm, &nv)) {
                g_hotMods = nm; g_hotVk = nv;
                if (ApplyHotkey(g_hwnd)) {          // 注册成功=新组合生效（旧注册被原子替换）
                    SaveConfig();
                    WCHAR t[40], msg[96];
                    HotkeyName(t, 40);
                    wfmt(msg, 96, L"唤醒快捷键已设置为 %s", t);
                    TrayBalloon(msg);
                } else {                            // 冲突：RegisterHotKey 失败且旧注册原样
                    TrayBalloon(L"该组合已被其他程序占用，已保留原快捷键");  // 保留，无需回退赋值
                }
            }
            break;                       // 设置后菜单收场（设置是一次性动作，不是开关）
        }
        if (cmd == IDM_TRAY_SHOW)   ShowLauncher();
        else if (cmd == IDM_EXIT)   ExitApp();
        break;    // 非开关项（显示面板/退出/Esc/点空白）：菜单收场
    }
}

// ---------------------------------------------------------------- 菜单
// 四个菜单弹出前的动作完全一样，收拢一处。用 ForceForeground 而非裸
// SetForegroundWindow——输入队列没挂反时菜单会秒关，像没反应
static int PopupAt(HMENU m)
{
    POINT p;
    GetCursorPos(&p);
    ForceForeground(g_hwnd);
    g_menuOpen = TRUE;                 // 模态循环里 WM_TIMER 照派：心跳 shell 调用回避
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g_hwnd, NULL);
    g_menuOpen = FALSE;
    DestroyMenu(m);
    return cmd;
}

// 格子右键菜单：有程序的格子给启动/属性等；空格也给槽位菜单（插入/删除）。
// 菜单按视图位 vi 打开——插入/删除是槽位级操作，作用于可见格位
static void ShowCellMenu(int vi)
{
    int ii = ItemAtView(vi);
    BOOL hasItem = ii >= 0 && ii < g_nItems && !IsBlank(ii);   // 上界写死：SAL 也认的显式边界
    // shell 虚拟项（回收站/此电脑）：没有文件位置可去，提权打开也无意义：
    // 这两个菜单项对它不诚实，直接不出现
    BOOL virt = hasItem && IsVirtualPath(g_items[ii].path);
    HMENU m = CreatePopupMenu();
    if (hasItem) {
        AppendMenuW(m, MF_STRING, IDM_LAUNCH, L"打开");
        if (!virt) {
            AppendMenuW(m, MF_STRING, IDM_ADMIN, L"以管理员身份运行");
            AppendMenuW(m, MF_STRING, IDM_LOC, L"打开文件位置");
        }
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(m, MF_STRING, IDM_INS_SLOT, L"在此插入格子");
    AppendMenuW(m, MF_STRING, IDM_DEL_SLOT, L"删除格子");
    if (hasItem) AppendMenuW(m, MF_STRING, IDM_CLR_SLOT, L"清空格子");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    if (hasItem) AppendMenuW(m, MF_STRING, IDM_PROPS, L"格子属性(&R)...");
    int cmd = PopupAt(m);
    if (!cmd) return;
    // 指针重解析：悬停可能已失效（删组/删格后旧下标），进 switch 前把
    // 条目指针定死，后续读 path / 对话框原地编辑都走它——NULL 分支
    // 即是"没有条目"的菜单项。属性对话框要原地改字段：指针非 const
    Item *it = (ii >= 0 && ii < g_nItems) ? &g_items[ii] : NULL;
    switch (cmd) {
    case IDM_LAUNCH: Launch(ii, FALSE); break;      // Launch 自带下标边界守卫
    case IDM_ADMIN:  Launch(ii, TRUE); break;
    case IDM_LOC:    if (it) OpenLocation(it->path); break;
    case IDM_INS_SLOT: InsertSlotAt(vi); break;
    case IDM_DEL_SLOT: DeleteSlotAt(vi); break;
    case IDM_CLR_SLOT: ClearSlotAt(vi); break;
    case IDM_PROPS: if (it) {
        WCHAR oldPath[MAX_PATH];
        wcpy(oldPath, MAX_PATH, it->path);
        DlgField f[4] = {
            { L"名称(N):",     it->name, 48,       0 },
            { L"文件名(F):",   it->path, MAX_PATH, 1 },
            { L"参数(P):",     it->args, 192,      0 },
            { L"工具提示(T):", it->tip,  96,       0 },
        };
        if (RunDialog(g_hwnd, L"格子属性", f, 4)) {
            if (wcmp_(oldPath, it->path))   // 目标变了：丢掉旧图标，绘制时按新目标重载
                DropIcon(ii);
            SyncAndSave();
        }
        break;
    }
    default: break;
    }
}

static void ShowGroupMenu(int gi)
{
    HMENU m = CreatePopupMenu();
    if (gi >= 0 && gi < g_nGroups) {    // 分组完全用户自治：任意行都可改名/删除（首组同理）
        AppendMenuW(m, MF_STRING, IDM_GRP_RENAME, L"重命名分组");
        AppendMenuW(m, MF_STRING, IDM_GRP_DEL, L"删除该分组");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(m, MF_STRING, IDM_GRP_NEW, L"新建分组");
    int cmd = PopupAt(m);
    if (cmd == IDM_GRP_RENAME && gi >= 0 && gi < g_nGroups) {
        DlgField f[1] = { { L"名称(N):", g_groups[gi].name, 48, 0 } };
        if (RunDialog(g_hwnd, L"重命名分组", f, 1)) {
            if (!g_groups[gi].name[0]) wcpy(g_groups[gi].name, 48, L"未命名");
            SyncAndSave();
        }
    } else if (cmd == IDM_GRP_DEL) {
        DeleteGroup(gi);
        SyncAndSave();       // 悬停行可能随删组失效：统一收尾重算（原手滚缺 UpdateHoverText）
    } else if (cmd == IDM_GRP_NEW) {
        NewGroup();
    }
}

// 标题栏右键菜单：置顶已并入图钉开关（按钮/托盘菜单），这里只剩退出
static void ShowTitleMenu(void)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出程序");
    if (PopupAt(m) == IDM_EXIT) ExitApp();
}

// ---------------------------------------------------------------- 全局双击唤醒
static ULONGLONG s_lastClick = 0;
static POINT  s_lastPt = { 0, 0 };
static BOOL   s_dragInRect;     // 拖动光标是否在面板静置区内（只报“从区外进入”的沿口）
static BOOL   s_lbtnDown;       // 左键按住的本地镜像：钩子自己看得到按下/抬起，
                                // 移动事件里就不必再问 GetAsyncKeyState
static DWORD s_dblClick;      // 双击间隔/双击容差：钩子线程启动时读一次——
static int    s_dblX, s_dblY; // 全局输入路径上每次点击都问，不值当

// 桌面 ListView 的命中测试消息（不引 commctrl.h，按消息编号直接定义）
#ifndef LVM_FIRST
#define LVM_FIRST 0x1000
#endif
#define LVM_SUBITEMHITTEST (LVM_FIRST + 57)

// 从“光标下的窗口”沿父链找桌面 ListView：图标由 SysListView32 自绘，
// WindowFromPoint 落在图标上和落在空白上返回同一个窗口，只能靠它命中测试。
static HWND DesktopListViewFrom(HWND h)
{
    for (HWND w = h; w; w = GetParent(w)) {
        WCHAR cls[64];
        GetClassNameW(w, cls, 64);
        if (!wcmp_(cls, L"SysListView32")) return w;
        if (!wcmp_(cls, L"SHELLDLL_DefView"))
            return FindWindowExW(w, NULL, L"SysListView32", NULL);
    }
    return NULL;
}

// 跨进程命中测试：LVHITTESTINFO 必须放在目标进程的内存里，
// 裸 SendMessage 传指针跨进程是未定义行为。任何一步失败都按“命中图标”处理——
// 宁可漏唤醒，也绝不能在双击图标启动程序时误弹面板。
static BOOL PointHitsDesktopIcon(HWND lv, POINT ptScr)
{
    struct { POINT pt; UINT flags; int iItem; int iSubItem; } hi;
    ZeroMemory(&hi, sizeof hi);
    hi.pt = ptScr;
    ScreenToClient(lv, &hi.pt);       // ListView 客户坐标
    hi.flags = 0; hi.iItem = -1; hi.iSubItem = -1;

    DWORD pid = 0;
    GetWindowThreadProcessId(lv, &pid);
    if (!pid) return TRUE;
    HANDLE hp = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ |
                            PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hp) return TRUE;
    BOOL hit = TRUE;
    void *rm = VirtualAllocEx(hp, NULL, sizeof hi, MEM_COMMIT, PAGE_READWRITE);
    if (rm && WriteProcessMemory(hp, rm, &hi, sizeof hi, NULL)) {
        DWORD_PTR res = 0;
        if (SendMessageTimeoutW(lv, LVM_SUBITEMHITTEST, 0, (LPARAM)rm,
                                SMTO_ABORTIFHUNG, 120, &res) &&
            ReadProcessMemory(hp, rm, &hi, sizeof hi, NULL))
            hit = (hi.iItem != -1);   // 未命中任何图标 = 桌面空白
        // 超时/读写失败：hit 保持 TRUE（保守：当成图标，不唤醒）
    }
    if (rm) VirtualFreeEx(hp, rm, 0, MEM_RELEASE);
    CloseHandle(hp);
    return hit;
}

static BOOL IsDesktopPoint(POINT pt)
{
    HWND h = WindowFromPoint(pt);
    if (!h) return FALSE;
    HWND root = GetAncestor(h, GA_ROOT);
    if (!root) return FALSE;
    WCHAR cls[64];
    GetClassNameW(root, cls, 64);
    if (wcmp_(cls, L"Progman") && wcmp_(cls, L"WorkerW")) return FALSE;

    // 桌面图标和桌面空白的根窗口都是 Progman/WorkerW，过不了上面的检查只是说明
    // “在桌面上”。双击桌面图标是要启动程序，不能顺带唤醒面板——再拿落点在桌面
    // ListView 上做一次图标命中测试，命中图标就不是空白。
    HWND lv = DesktopListViewFrom(h);
    if (lv) return !PointHitsDesktopIcon(lv, pt);
    return TRUE;    // 拿不到 ListView（深度定制桌面）：维持旧行为
}

static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    // 跨线程读 UI 态（g_hwnd/g_dlgOpen/IsWindowVisible）：按构造无害——
    // g_hwnd 在本线程创建前就已赋值（WM_CREATE 先于 CreateThread），
    // g_dlgOpen 是单字节标志（x86 对齐读写不可撕裂），IsWindowVisible 是
    // 窗口管理器的内核态读取。最坏情况是慢一拍（晚一个事件才看到状态
    // 变化），不产生错误动作。s_* 全是本线程私有，不共享
    if (nCode == HC_ACTION) {
        if (wParam == WM_LBUTTONDOWN) {
            MSLLHOOKSTRUCT *p = (MSLLHOOKSTRUCT *)lParam;
            s_lbtnDown = TRUE;
            ULONGLONG now = GetTickCount64();
            int dx = p->pt.x - s_lastPt.x, dy = p->pt.y - s_lastPt.y;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            BOOL dbl = s_lastClick && (now - s_lastClick) <= s_dblClick
                       && dx <= s_dblX && dy <= s_dblY;
            // 钩子回调跑在全局输入路径上：这里只做时序判断。
            // WindowFromPoint 是跨线程窗口调用，被查窗口卡死会拖住整个系统的鼠标输入，
            // 所以“点的是不是桌面”放到 UI 线程再确认（WM_APP_WAKE_CAND）。
            if (dbl && !IsWindowVisible(g_hwnd) && !g_dlgOpen)
                PostMessageW(g_hwnd, WM_APP_WAKE_CAND, 0,
                             MAKELPARAM((WORD)p->pt.x, (WORD)p->pt.y));
            s_lastClick = now;
            s_lastPt = p->pt;
        } else if (wParam == WM_LBUTTONUP) {
            s_lbtnDown = FALSE;
        } else if (wParam == WM_MOUSEMOVE) {
            // 拖放唤醒：按住左键拖动、光标从区外进入面板静置区（面板隐藏时）——
            // 就地弹回面板（WM_APP_DRAGWAKE → SW_SHOWNA 不抢拖放源的前台），
            // 让落点重新可投递。只在进入沿口发一次；普通移动/悬停零打扰。
            // 没按左键的移动是系统里最频繁的事件：先看本地镜像直接走掉，
            // 原来的 IsWindowVisible + GetAsyncKeyState 两次内核查询全省了
            if (!s_lbtnDown) {
                s_dragInRect = FALSE;
            } else if (!g_dlgOpen && !IsWindowVisible(g_hwnd)) {
                // GetWindowRect 是窗口管理器查询不发消息，UI 线程卡死也不拖累钩子
                MSLLHOOKSTRUCT *p = (MSLLHOOKSTRUCT *)lParam;
                RECT r;
                GetWindowRect(g_hwnd, &r);
                BOOL inside = PtInRect(&r, p->pt) != 0;
                if (inside && !s_dragInRect)
                    PostMessageW(g_hwnd, WM_APP_DRAGWAKE, 0, 0);
                s_dragInRect = inside;
            } else {
                s_dragInRect = FALSE;
            }
        } else if (wParam == WM_MOUSEWHEEL) {
            // 接管条件收到最窄，缺一不可：面板可见、置顶（这一像素确实属于
            // 面板）、对话框没开、面板无焦点。全局钩子只要有丝毫越界就会偷走
            // 其他窗口的滚动——绝不能碰的红线。不置顶时光标底下归谁谁滚
            MSLLHOOKSTRUCT *p = (MSLLHOOKSTRUCT *)lParam;
            if (IsWindowVisible(g_hwnd) && g_pinned && !g_dlgOpen &&
                GetForegroundWindow() != g_hwnd) {
                RECT r;
                GetWindowRect(g_hwnd, &r);
                if (PtInRect(&r, p->pt)) {
                    POINT c = p->pt;
                    ScreenToClient(g_hwnd, &c);
                    int d = (short)HIWORD(p->mouseData);
                    PostMessageW(g_hwnd, WM_APP_WHEEL, (WPARAM)(short)d, MAKELPARAM(c.x, c.y));
                    return 1;               // 确认这一格归面板，才吞
                }
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// LL 钩子装在专用线程上：UI 线程忙绘制/菜单时回调照常即时触发，
// 否则快速滚动时回调超时被系统跳过，滚轮就丢了。
static DWORD WINAPI HookThread(LPVOID arg)
{
    UNREF(arg);
    s_dblClick = GetDoubleClickTime();
    s_dblX = GetSystemMetrics(SM_CXDOUBLECLK);   // 双击容差就是这两个指标；
    s_dblY = GetSystemMetrics(SM_CYDOUBLECLK);   // 原用 SM_C*DLGFRAME（边框宽）是张冠李戴
    // 左键状态的初值：程序可能在用户已经按住左键时启动，此后钩子只看得见
    // 后续的按下/抬起，这里补一次真实状态，拖放唤醒才不会漏掉这一场拖动
    s_lbtnDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, NULL, 0);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = NULL; }
    return 0;
}

// ---------------------------------------------------------------- 窗口过程：消息处理
// 每条消息一个小函数，WndProc 只做分发；顺序与 switch 分发表一致

// ---------------------------------------------------------------- 唤醒快捷键（可配置）
static void ModsName(WCHAR *out, int cch, UINT mods)
{
    out[0] = 0;
    if (mods & MOD_CONTROL) wcat(out, cch, L"Ctrl+");
    if (mods & MOD_ALT)     wcat(out, cch, L"Alt+");
    if (mods & MOD_SHIFT)   wcat(out, cch, L"Shift+");
}

// 特殊主键的可读名：查表比 15 个 case 短，也免了每次调用的分支链
static const struct { UINT vk; const WCHAR *name; } VK_NAMES[] = {
    { VK_SPACE,  L"空格" },   { VK_RETURN, L"回车" },
    { VK_TAB,    L"Tab" },    { VK_INSERT, L"Insert" },
    { VK_DELETE, L"Delete" }, { VK_HOME,   L"Home" },
    { VK_END,    L"End" },    { VK_PRIOR,  L"PageUp" },
    { VK_NEXT,   L"PageDown" },
    { VK_LEFT,   L"←" },      { VK_RIGHT,  L"→" },
    { VK_UP,     L"↑" },      { VK_DOWN,   L"↓" },
};

static void HotkeyName(WCHAR *out, int cch)
{
    ModsName(out, cch, g_hotMods);
    int n = (int)wlen_(out);
    UINT vk = g_hotVk;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        if (n + 1 < cch) { out[n] = (WCHAR)vk; out[n + 1] = 0; }
    } else if (vk >= VK_F1 && vk <= VK_F24) {
        wfmt(out + n, cch - n, L"F%u", vk - VK_F1 + 1);
    } else {
        UINT i = 0;
        while (i < _countof(VK_NAMES) && VK_NAMES[i].vk != vk) i++;
        if (i < _countof(VK_NAMES)) wcat(out, cch, VK_NAMES[i].name);
        else                        wfmt(out + n, cch - n, L"0x%02llX", (unsigned long long)vk);
    }
}

// 注册/更换唤醒快捷键。同一 HOTKEY_ID 重复注册即原子替换——冲突时返回
// FALSE 且旧注册原样保留（“先撤后注”的写法会在冲突时把旧组合弄丢）
static BOOL ApplyHotkey(HWND h)
{
    return RegisterHotKey(h, HOTKEY_ID, g_hotMods | MOD_NOREPEAT, g_hotVk);
}

static void OnCreate(HWND h)
{
    g_hwnd = h;
    MakeFontsAndBrushes();
    SetWindowPos(h, g_pinned ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
    DragAcceptFiles(h, TRUE);
    g_hookH = CreateThread(NULL, 0, HookThread, NULL, 0, &g_hookTid);
    if (!ApplyHotkey(h)) {
        WCHAR t[40], msg[128];
        HotkeyName(t, 40);
        wfmt(msg, 128, L"唤醒快捷键 %s 被其他程序占用：请从托盘菜单更换，或改用托盘/桌面双击唤醒", t);
        TrayBalloon(msg);
    }
    SetTimer(h, TIMER_HIDE, 120, NULL);
    // 托盘图标
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    TrayAdd();
    if (g_firstRun) {
        WCHAR t[40], msg[160];
        HotkeyName(t, 40);
        wfmt(msg, 160, L"claunch 已驻留托盘：单击图标显示面板，%s 随时唤醒，双击桌面空白也可以", t);
        TrayBalloon(msg);
    }
}

// 离屏双缓冲：隐藏即释放（~2MB commit，挂托盘时纯占窝），
// 首绘/尺寸变化时惰性重建（OnSize 必重建保尺寸配对，OnPaint 兜底）
static void ReleaseMemBuffer(void)
{
    if (g_memDC) { DeleteDC(g_memDC); g_memDC = NULL; }
    if (g_memBmp) { DeleteObject(g_memBmp); g_memBmp = NULL; }
}

static void EnsureMemBuffer(HWND h)
{
    if (g_memDC) return;
    HDC scr = GetDC(h);
    HDC dc = CreateCompatibleDC(scr);
    HBITMAP bmp = CreateCompatibleBitmap(scr, g_cw, g_ch);
    ReleaseDC(h, scr);
    if (dc && bmp) {
        SelectObject(dc, bmp);
        g_memDC = dc;
        g_memBmp = bmp;
        g_memFresh = TRUE;
    } else {                          // 建不起：退回直接绘制路径（OnPaint 有兜底）
        if (dc) DeleteDC(dc);
        if (bmp) DeleteObject(bmp);
    }
}

static void OnSize(HWND h, LPARAM l)
{
    g_cw = LOWORD(l);
    g_ch = HIWORD(l);
    UpdateCell();       // 横向铺满的正方格宽随窗口变化
    // 离屏缓冲按新尺寸重建：先放旧的再惰性建新的（失败则 OnPaint 直绘兜底）
    ReleaseMemBuffer();
    EnsureMemBuffer(h);
    int rad = S_(10);
    HRGN rgn = CreateRoundRectRgn(0, 0, g_cw + 1, g_ch + 1, rad, rad);
    if (rgn) SetWindowRgn(h, rgn, TRUE);
    ClampScroll();
    ClampGrpScroll();
    RefreshCard();      // 尺寸变了：卡片按新布局重新锚定/钳制
    InvalidateRect(h, NULL, FALSE);
}

// WS_POPUP 无边框：手动给边缘 6px 命中区，窗口才能拉伸。
// 不在边缘返回 0（HTNOWHERE），调用方转交 DefWindowProc
static LRESULT OnNcHitTest(HWND h, LPARAM l)
{
    POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
    ScreenToClient(h, &p);
    int mg = S_(6);
    BOOL lft = p.x < mg, rgt = p.x >= g_cw - mg, top = p.y < mg, bot = p.y >= g_ch - mg;
    if (top && lft) return HTTOPLEFT;
    if (top && rgt) return HTTOPRIGHT;
    if (bot && lft) return HTBOTTOMLEFT;
    if (bot && rgt) return HTBOTTOMRIGHT;
    if (lft) return HTLEFT;
    if (rgt) return HTRIGHT;
    if (top) return HTTOP;
    if (bot) return HTBOTTOM;
    return 0;
}

static void OnDpiChanged(HWND h, WPARAM w, LPARAM l)
{
    g_dpi = LOWORD(w);
    ScaleMetrics();
    MakeFontsAndBrushes();
    // 图标尺寸 = S_(格基)×3/7 随 DPI 变：缓存图标不丢就会被拉伸到新尺寸（发虚），
    // 与 ApplyCellBase 同一处理——丢掉，随后那次绘制按新 DPI 重载
    for (int i = 0; i < g_nItems; i++) DropIcon(i);
    // 托盘图标同理：shell 按任务栏所在显示器的 DPI 要图标，尺寸变了不重建
    // 就被拉伸发虚（多显示器跨屏场景）
    if (g_trayIcon) {
        int tsz = GetSystemMetrics(SM_CYSMICON);
        if (tsz != g_traySz) {
            DestroyIcon(g_trayIcon);
            g_trayIcon = MakeTrayIcon(tsz);
            if (g_trayIcon) {
                g_traySz = tsz;
                g_nid.hIcon = g_trayIcon;
                g_nid.uFlags = NIF_ICON;
                Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            }
        }
    }
    RECT r;
    if (g_applySavedRect) {
        // 首次 DPI 切换：配置里记的是物理像素，必须以它为准，
        // 否则采纳系统建议矩形会让窗口每次启动都缩小（1/1.25）。
        g_applySavedRect = FALSE;
        r.left = g_savedX;
        r.top = g_savedY;
        r.right = g_savedX + g_savedW;
        r.bottom = g_savedY + g_savedH;
    } else {
        r = *(RECT *)l;
    }
    SetWindowPos(h, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

static void OnPaint(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    // 隐藏窗没有可画的内容：启动期实测 Windows 会给不可见的层面板发
    // WM_PAINT——整段跳过（图标/shell 管线/离屏缓冲全不碰），一次省
    // ~16MB 工作集 + ~50 GDI。唤醒后窗口可见，照常全量绘制
    if (IsWindowVisible(h)) {
        EnsureMemBuffer(h);      // 隐藏期间缓冲已释放：唤醒后首绘前重建
        if (!g_memDC) {
            PaintWindow(dc, NULL);       // 缓冲不可用：直绘兜底，无裁剪全画
        } else {
            // 只重画更新区域：整窗重画会拖垮消息队列（悬停文字/右键跟着卡）。
            // 其余缓冲内容保持上一帧，与屏幕一致
            RECT rc = ps.rcPaint;
            if (g_memFresh) { rc.left = 0; rc.top = 0; rc.right = g_cw; rc.bottom = g_ch; }
            // 裁剪走 IntersectClipRect：memDC 每次画完都 SelectClipRgn(NULL)
            // 复位成整幅位图（WM_SIZE 重建时也是全新 DC），起始裁剪即全图，
            // 四次矩形求交等价于替换裁剪区，但省掉每次绘制一次 GDI 区域对象
            // 的创建+销毁——WM_PAINT 是悬停/拖动/滚动每条热路径的终点
            IntersectClipRect(g_memDC, rc.left, rc.top, rc.right, rc.bottom);
            PaintWindow(g_memDC, &rc);     // 裁剪区即 rc：直接传，省一次 GetClipBox
            SelectClipRgn(g_memDC, NULL);
            BitBlt(dc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                   g_memDC, rc.left, rc.top, SRCCOPY);
            g_memFresh = FALSE;
        }
    }
    EndPaint(h, &ps);
}

// 格子/分组的按下-拖动-抬起状态机：两套拖动各三拍（按下记起点、
// 过阈值进拖动、抬起落定），外加焦点环跟随。悬停检测兜底收尾
static void OnMouseMove(HWND h, LPARAM l)
{
    POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
    if (g_pressCell >= 0) {                 // 拖动排序进行中
        if (!g_dragging) {
            // 拖动排序按视图序号操作：过滤中的视图是子集，序号含义不同，
            // 先清过滤回全量再拖，落点高亮才和用户看到的一致
            if (PastDragThreshold(p, g_pressPt)) {
                ClearFilter();
                g_dragging = TRUE;
                g_dropCell = g_pressCell;
                g_dropIn = TRUE;
                InvalidateCell(g_pressCell);
            }
        }
        if (g_dragging) {
            // 落点即槽位序号：CellAt 的间隙拒绝会被紧随的 SlotAt 兜底抵消
            // （原两段式等价于一次 SlotAt），这里一次算清
            int nc = SlotAt(p);
            BOOL nin = p.x >= g_sideW && p.y >= g_titleH;
            if (nc != g_dropCell || nin != g_dropIn) {
                // 新旧落点都有效时合成一个外接矩形：相邻格拖动每次移动
                // 少发一个更新区（-1 表示窗外/网格外，各自单发即可）
                if (g_dropCell >= 0 && nc >= 0) InvalidateCells(g_dropCell, nc);
                else {
                    InvalidateCell(g_dropCell);
                    InvalidateCell(nc);
                }
                g_dropCell = nc;
                g_dropIn = nin;
            }
        }
    }
    if (g_pressGroup >= 0) {                 // 侧栏分组重排进行中
        if (!g_dragGroup && PastDragThreshold(p, g_pressPt)) {
            g_dragGroup = TRUE;
            g_dropGroup = g_pressGroup;
            InvalidateGroup(g_pressGroup);
        }
        if (g_dragGroup) {
            int ng = GroupAt(p);
            // 行间隙/首行上方/末行下方 GroupAt=-1：保持上一个目标行（粘性）——
            // 向上拖出首行上方是自然的“移到最顶”手势，不能把目标冲掉
            if (ng >= 0 && ng != g_dropGroup) {
                InvalidateGroups(g_dropGroup, ng);   // 行区间一个外接矩形
                g_dropGroup = ng;
            }
        }
    }
    int oc = g_hoverCell, og = g_hoverGroup, ob = g_hoverBtn;
    BOOL txtChg = HoverFromPoint(p);
    if (oc != g_hoverCell || og != g_hoverGroup || ob != g_hoverBtn) {
        g_flashAt = 0;                    // 悬停目标变了：新意图 supersede 启动反馈
        RefreshHoverSpots(oc, og, txtChg);
    }
    if (!g_tracking) {
        TRACKMOUSEEVENT tme;
        ZeroMemory(&tme, sizeof tme);
        tme.cbSize = sizeof tme;
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = h;
        g_tracking = TrackMouseEvent(&tme);
    }
}

static void OnMouseLeave(void)
{
    g_tracking = FALSE;
    g_pressBtn = 0;     // 光标离开：按压态作废（按下中移出窗口很常见）
    int oc = g_hoverCell;
    g_hoverCell = g_hoverGroup = -1;
    g_hoverBtn = 0;
    UpdateHoverText();
    InvalidateCell(oc);
    InvalidateSidebar();
    InvalidateTitle();
    RefreshCard();                   // 鼠标离开：收起小卡片
}

static void OnLButtonDown(HWND h, LPARAM l)
{
    POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
    HoverFromPoint(p);
    RefreshCard();      // 悬停可能瞬变（如唤醒后首次点击），卡片同步
    // 按下即入按压态（胶囊底深一档）——动作仍即时生效，这只是触感
    if (g_hoverBtn) { g_pressBtn = g_hoverBtn; InvalidateTitle(); }
    if (g_hoverBtn == BTN_CLOSE) { CloseToTray(); return; }
    if (g_hoverBtn == BTN_PIN) { TogglePin(); return; }
    if (g_hoverGroup >= 0) {
        if (g_hoverGroup == g_curGroup) {
            // 点当前组 = 回到顶部（既有行为）
            if (g_scroll) {
                g_scroll = 0;
                ClampScroll();
                InvalidateRect(h, NULL, FALSE);
                RefreshCard();
            }
        } else {
            SelectGroup(g_hoverGroup);   // 切组：滚动位记忆/恢复与滚轮切组同一路径
        }
        // 任意分组行按下即记住起点（重排）。SelectGroup 不动 g_hoverGroup，无需重验
        g_pressGroup = g_hoverGroup;
        g_pressPt = p;
        g_dragGroup = FALSE;
        g_dropGroup = -1;
        SetCapture(h);
        return;
    }
    if (g_hoverCell >= 0) {
        int ii = ItemAtView(g_hoverCell);
        if (ii >= 0 && !IsBlank(ii)) {
            // 按下先不启动：原地抬起才是启动，拖过阈值是排序（见抬起落定）
            g_pressCell = g_hoverCell;
            g_pressPt = p;
            g_dragging = FALSE;
            g_dropCell = -1;
            g_dropIn = FALSE;
            SetCapture(h);
            return;
        }
    }
    ReleaseCapture();
    POINT sp = p;                           // NC 消息要屏幕坐标，否则拖动瞬移
    ClientToScreen(h, &sp);
    SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(sp.x, sp.y));
}

// 返回是否消费（TRUE=有按下态在管）；都不在管时转交 DefWindowProc
static BOOL OnLButtonUp(void)
{
    if (g_pressBtn) {                 // 按钮抬起：按压态收头
        g_pressBtn = 0;
        InvalidateTitle();
    }
    if (g_pressGroup >= 0) {               // 侧栏分组重排落定（未拖动时按下已完成选中，无需处理）
        int from = g_pressGroup, to = g_dropGroup;
        BOOL wasDrag = g_dragGroup;
        g_pressGroup = -1; g_dragGroup = FALSE; g_dropGroup = -1;
        ReleaseCapture();
        if (wasDrag && to >= 0 && to != from) MoveGroup(from, to);
        return TRUE;
    }
    if (g_pressCell < 0) return FALSE;
    int from = g_pressCell, to = g_dropCell;
    BOOL inGrid = g_dropIn, wasDrag = g_dragging;
    g_pressCell = -1; g_dragging = FALSE; g_dropCell = -1; g_dropIn = FALSE;
    ReleaseCapture();
    if (!wasDrag) {
        LaunchView(from);
    } else if (inGrid) {
        int n = ViewCount();
        if (to < 0) to = n - 1;            // 落点解析不出格位（拖到网格下方空白）：排到末尾
        if (to >= n) {                     // 落点在末尾之外的空格：补齐空格再落
            EnsureSlot(to);
            n = ViewCount();
            if (to >= n) to = n - 1;
        }
        if (to != from && to >= 0) {
            int fi = ItemAtView(from), ti = ItemAtView(to);
            if (fi >= 0 && ti >= 0) {
                // 槽位模型：落点（空格或程序）与源互换——拖谁跟谁换，其余格子原位不动
                Item tmp = g_items[fi];
                g_items[fi] = g_items[ti];
                g_items[ti] = tmp;
                SaveConfig();
            }
            ClampScroll();
            ViewDirty();
        }
        InvalidateCell(from);
        InvalidateCell(to);
        RefreshCard();       // 互换后同一悬停格对应的条目变了
    } else {
        InvalidateCell(from);            // 拖出格子区：取消，抹掉源/落点高亮
        if (to >= 0 && to != from) InvalidateCell(to);
    }
    return TRUE;
}

static void OnCaptureLost(void)
{
    if (g_pressCell >= 0) {
        int a = g_pressCell, b = g_dropCell;
        g_pressCell = -1; g_dragging = FALSE; g_dropCell = -1; g_dropIn = FALSE;
        InvalidateCell(a);                    // 互换模型下高亮只有源/落点两格，逐格擦
        if (b >= 0 && b != a) InvalidateCell(b);
    }
    if (g_pressGroup >= 0) {
        int a = g_pressGroup, b = g_dropGroup;
        g_pressGroup = -1; g_dragGroup = FALSE; g_dropGroup = -1;
        InvalidateGroup(a);                   // 先清状态再失效：重绘时已无高亮
        if (b >= 0 && b != a) InvalidateGroup(b);
    }
}

static void OnRButtonUp(LPARAM l)
{
    POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
    HoverFromPoint(p);
    RefreshCard();
    if (g_hoverCell >= 0) ShowCellMenu(g_hoverCell);   // 空格也给槽位菜单（插入/删除）
    else if (g_hoverGroup >= 0) {
        ShowGroupMenu(g_hoverGroup);
    } else if (p.x < g_sideW && p.y >= g_titleH) {
        ShowGroupMenu(-1);            // 侧边栏空白处（列表下方）：只给“新建分组”
    } else if (g_hoverBtn || p.y < g_titleH) {
        ShowTitleMenu();
    }
}

// 滚轮：Ctrl+滚轮缩放格子（光标在左右区域都响应）；侧栏滚轮切分组
//（等同于点击相邻分组，到头循环）；格子区滚动一格并让卡片跟随
static void OnWheel(HWND h, UINT m, WPARAM w, LPARAM l)
{
    POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
    if (m == WM_MOUSEWHEEL) ScreenToClient(h, &p);
    int d = (m == WM_APP_WHEEL) ? (int)(short)w : (int)GET_WHEEL_DELTA_WPARAM(w);
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
        ApplyCellBase(g_cellBase + (d > 0 ? 4 : -4));
        return;
    }
    if (p.x < g_sideW) {
        int gi = g_curGroup + (d > 0 ? -1 : 1);
        if (gi < 0) gi = g_nGroups - 1;
        if (gi >= g_nGroups) gi = 0;
        SelectGroup(gi);
    } else {
        int before = g_scroll;
        g_scroll += (d > 0 ? -1 : 1) * (g_cell + g_gap);
        ClampScroll();
        if (g_scroll != before) {
            RECT gr = { g_sideW, g_titleH, g_cw, g_ch };
            InvalidateRect(h, &gr, FALSE);
            RefreshCard();       // 格子滚动了：卡片跟着锚点走
        }
    }
}

static void OnKeyDown(WPARAM w)
{
    if (w == VK_ESCAPE) {                       // 搜索中：第一次 Esc 清过滤词，再按才隐藏
        if (g_filter[0]) ClearFilter();
        else BeginHide(TRUE);
        return;
    }
    if (g_dlgOpen || g_pressCell >= 0) return;   // 对话框/拖动排序进行中：不抢键
    if ((w == 'C' || w == 'V') && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
        // Ctrl+C/V：复制悬停格 / 粘贴到悬停格（空格也填），无悬停格则追加
        ClipItem ci;
        if (w == 'C') {
            int ii = ItemAtView(g_hoverCell);
            if (ii >= 0 && !IsBlank(ii)) {
                ItemToClip(&g_items[ii], &ci);
                if (ClipSet(&ci))              // 剪贴板被占用则不反馈（用户重试即可）
                    FlashFmt(L"已复制: %s", g_items[ii].name);
            }
            return;
        }
        if (!ClipGet(&ci)) { FlashMsg(L"剪贴板没有可粘贴的内容"); return; }
        int ii = ItemAtView(g_hoverCell);
        if (ii >= 0) PasteItemAt(ii, &ci);
        else PasteItemNew(&ci);
        FlashFmt(L"已粘贴: %s", ci.name);
        return;
    }
    if (w >= '1' && w <= '9' && !g_filter[0] &&
        !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) {   // 数字键：启动当前组第 N 格（Ctrl 修饰留给复制，过滤中归搜索）
        LaunchView((int)(w - '1'));
        return;
    }
    if (w == VK_RETURN) {                           // 回车：启动悬停格；搜索中悬停失效=最优匹配
        int v = g_hoverCell;
        if (g_filter[0] && (v < 0 || v >= ViewCount())) v = 0;
        LaunchView(v);
        return;
    }
    if (w == VK_DELETE && !g_filter[0] && g_hoverCell >= 0) {   // Del：清空悬停格（槽位保留，右键菜单同款）
        int ii = ItemAtView(g_hoverCell);
        if (ii >= 0 && !IsBlank(ii)) {
            ClearSlotAt(g_hoverCell);
            FlashMsg(L"已清空");
        }
        return;
    }
    int n = ViewCount();                            // 方向键：移动悬停高亮（与鼠标悬停同一套状态/重绘）
    if (n <= 0) return;
    int c = g_hoverCell;
    if (c < 0 || c >= n) c = 0;                     // 悬停失效时从第一格起
    // Home/End/PageUp/PageDown：跳到首末格或翻整页（顺带把目标行滚进
    // 可视区——键盘导航没有"光标带着视图走"就等于跳丢）
    if (w == VK_HOME || w == VK_END || w == VK_PRIOR || w == VK_NEXT) {
        int page = g_slots / g_cols * g_cols;       // 整页格数（按行取整）
        if (page < g_cols) page = g_cols;
        int nc = c;
        if (w == VK_HOME)             nc = 0;
        else if (w == VK_END)         nc = n - 1;
        else if (w == VK_PRIOR)       nc = c - page;
        else                          nc = c + page;
        if (nc < 0) nc = 0;
        if (nc >= n) nc = n - 1;
        int og = g_hoverGroup;
        BOOL moved = ScrollCellIntoView(nc);
        g_hoverCell = nc; g_hoverGroup = -1; g_hoverBtn = 0;
        UpdateHoverText();
        RefreshHoverSpots(c, og, TRUE);
        if (moved) InvalidateRect(g_hwnd, NULL, FALSE);   // 滚动量变了才整窗重绘
        return;
    }
    int nc = c;
    switch (w) {
    case VK_LEFT:  nc = c - 1;      break;
    case VK_RIGHT: nc = c + 1;      break;
    case VK_UP:    nc = c - g_cols; break;
    case VK_DOWN:  nc = c + g_cols; break;
    default: return;
    }
    if (nc < 0 || nc >= n) return;
    int og = g_hoverGroup;
    g_hoverCell = nc; g_hoverGroup = -1; g_hoverBtn = 0;
    UpdateHoverText();
    RefreshHoverSpots(c, og, TRUE);
}

// 可打印字符 → 搜索过滤（退格删字符，Esc 清空）。主窗 IME 已断开，
// 中文命名只能用 ASCII 搜——换输入法链的 30MB 不值
static void OnChar(WPARAM w)
{
    if (g_dlgOpen || g_pressCell >= 0) return;
    if (w == 0x08) {                              // 退格：删一个字符
        if (!g_filter[0]) return;
        int n = (int)wlen_(g_filter);
        g_filter[n - 1] = 0;
        ApplyFilter();
        return;
    }
    if (w < 32) return;                         // 其余控制字符不管
    if (w >= '1' && w <= '9' && !g_filter[0]) return;   // 空过滤时数字键归 WM_KEYDOWN 快速启动
    int n = (int)wlen_(g_filter);
    if (n >= 20) return;                        // 过滤词上限 20 字符
    g_filter[n++] = (WCHAR)w;
    g_filter[n] = 0;
    g_flashAt = 0;                                // 开始打字：搜索 supersede 启动反馈
    ApplyFilter();
}

// ---------------------------------------------------------------- 拖放（WM_DROPFILES + OLE 两路）
// 文件走 CF_HDROP（老 WM_DROPFILES 协议与 OLE 拿到的是同一种 HDROP 句柄）；
// 回收站/此电脑等虚拟项只有 CFSTR_SHELLIDLIST（PIDL），必须 OLE。两路共用
// 同一套落位核心（DropBegin/DropApply/DropFinish），落点语义完全一致
typedef struct { int slot, g, lo, hi; } DropCtx;

// 拖放前置：清过滤（落点按全量视图的格位算，EnsureSlot 补的空格不过滤）、
// 算落点槽位、整批补齐空格、定目标组。nf 为条目数（调用方已按 MAX_ITEMS 封顶）
static void DropBegin(DropCtx *c, POINT p, int nf)
{
    c->lo = c->hi = -1;                // 受影响的格子区间（局部重绘用）
    ClearFilter();
    // 拖到侧栏分组行=直接追加进该组（含首组）
    int gi = GroupAt(p);
    if (gi >= 0) {
        c->slot = -1;
        c->g = gi;
        return;
    }
    // 必须用 SlotAt：空格也是合法落点。CellAt 只匹配已有条目，
    // 拖到空格返回 -1 会被当成“落到网格外”直接追加到列表末尾——
    // 光标明明停在空格上，格子却没有任何变化。
    c->slot = SlotAt(p);
    c->g = TargetGroup();
    // 整批落点需要的空格一次补齐：逐文件调 EnsureSlot 会让视图重建
    // nf 遍（每补一格标脏一次）；一次补到位后 ItemAtView 走缓存
    if (c->slot >= 0) EnsureSlot(c->slot + nf - 1);
}

// 单个拖入目标的落位：网格外=追加末尾；落点已占用→换上新目标；
// 空格→填入该空位（落哪格停哪格）。多文件自落点依次排开
static void DropApply(DropCtx *c, int k, const WCHAR *path, const WCHAR *name)
{
    if (c->slot < 0) { AddNamedItem(path, name, c->g); return; }
    int s = c->slot + k;
    if (c->lo < 0) c->lo = c->hi = s;      // 记下受影响的格子范围
    else { if (s < c->lo) c->lo = s; if (s > c->hi) c->hi = s; }
    int ii = ItemAtView(s);
    if (ii >= 0) {
        SetItemTarget(ii, path, c->g);
        if (name && name[0]) wcpy(g_items[ii].name, 48, name);
    } else {
        AddNamedItem(path, name, c->g);    // 条目数爆满兜底
    }
}

// 拖放收尾：视图/配置/滚动/卡片统一同步，落点区间局部重绘
static void DropFinish(HWND h, const DropCtx *c)
{
    ViewDirty();                           // 替换可能改了条目分组
    // 落点分组必须对用户可见：拖到别的分组行上落下时，视图跟过去——
    // 否则用户看到的就是“拖了没反应”。落到当前组时 SelectGroup 内部早退
    if (c->g > 0 && c->g != g_curGroup) SelectGroup(c->g);
    SaveConfig();
    ClampScroll();
    RefreshCard();                         // 视图/名称可能变了，卡片同步
    if (c->lo < 0) {
        InvalidateRect(h, NULL, FALSE);    // 拖到网格外：追加到末尾，整窗重绘
    } else {
        InvalidateCells(c->lo, c->hi);     // 只重绘落点区间
    }
}

// CF_HDROP 落放核心。OLE 拿到的 HDROP 不经 DragFinish——释放归
// ReleaseStgMedium；WM_DROPFILES 路径由 OnDropFiles 收尾。
// nf 由调用方给（两路调用前都已问过一次，不再重复问）
static void HandleDroppedFiles(HWND h, HDROP hd, POINT p, UINT nf)
{
    if (!nf) return;
    if (nf > (UINT)MAX_ITEMS) nf = MAX_ITEMS;
    DropCtx c;
    DropBegin(&c, p, (int)nf);
    for (UINT k = 0; k < nf; k++) {
        WCHAR path[MAX_PATH];
        if (!DragQueryFileW(hd, k, path, MAX_PATH)) continue;
        DropApply(&c, (int)k, path, NULL);
    }
    DropFinish(h, &c);
}

static void OnDropFiles(HWND h, WPARAM w)
{
    HDROP hd = (HDROP)w;
    UINT nf = DragQueryFileW(hd, 0xFFFFFFFF, NULL, 0);
    if (nf > 0) {
        POINT p;
        // DragQueryPoint 给的是本窗客户坐标；拿不到（少数拖放路径）就退回光标位置
        if (!DragQueryPoint(hd, &p)) {
            GetCursorPos(&p);
            ScreenToClient(h, &p);
        }
        HandleDroppedFiles(h, hd, p, nf);
    }
    DragFinish(hd);
}

// ---- OLE 拖放：shell 项 PIDL → 落位 ----------------------------------------
// CIDA 里 aoffset[0] 是父文件夹 PIDL、其后是相对条目。全 PIDL = 两者拼接
//（桌面拖放时父 PIDL 为空，拼出来就是条目自身）。资源管理器同款逻辑
static UINT PidlLen(LPCITEMIDLIST p)       // 含终结符的总字节数
{
    UINT n = 0;
    if (p) while (p->mkid.cb) {
        n += p->mkid.cb;
        p = (LPCITEMIDLIST)((const BYTE *)p + p->mkid.cb);
    }
    return n + (UINT)sizeof(p->mkid.cb);
}

// a（可空）去终结符 + b（含终结符）→ 新分配的全 PIDL（CoTaskMemAlloc，调用方 CoTaskMemFree）
static LPITEMIDLIST PidlJoin(LPCITEMIDLIST a, LPCITEMIDLIST b)
{
    UINT la = a ? PidlLen(a) - (UINT)sizeof(a->mkid.cb) : 0;
    UINT lb = PidlLen(b);
    LPITEMIDLIST p = (LPITEMIDLIST)CoTaskMemAlloc(la + lb);
    if (!p) return NULL;
    if (la) memcpy(p, a, la);
    memcpy((BYTE *)p + la, b, lb);
    return p;
}

// CFSTR_SHELLIDLIST 落放：文件系统项取路径；取不到的是虚拟项（回收站…），
// 存桌面绝对解析名（"::{CLSID}"，ShellExecuteW 认它可直接启动），显示名
// 另取（"回收站"）——解析名推不出可读名。返回落位的条目数
static int HandleShellIdList(HWND h, IDataObject *d, POINT p)
{
    FORMATETC ft = { (CLIPFORMAT)g_cfShellIdList, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM med;
    if (FAILED(IDataObject_GetData(d, &ft, &med))) return 0;
    int applied = 0;
    CIDA *cid = (CIDA *)GlobalLock(med.hGlobal);
    SIZE_T gsz = cid ? GlobalSize(med.hGlobal) : 0;
    // 尺寸防御：cidl 不越上限，每个 PIDL 偏移至少要容得下 2 字节终结符
    if (cid && gsz >= sizeof(CIDA) && cid->cidl <= MAX_ITEMS &&
        cid->aoffset[0] + 2 <= gsz) {
        DropCtx c;
        DropBegin(&c, p, (int)cid->cidl);
        for (UINT k = 1; k <= cid->cidl; k++) {
            if (cid->aoffset[k] + 2 > gsz) break;                    // 越界防御
            LPITEMIDLIST full = PidlJoin(
                (LPCITEMIDLIST)((const BYTE *)cid + cid->aoffset[0]),
                (LPCITEMIDLIST)((const BYTE *)cid + cid->aoffset[k]));
            if (!full) continue;
            WCHAR path[MAX_PATH];
            if (SHGetPathFromIDListW(full, path) && path[0]) {
                DropApply(&c, (int)k - 1, path, NULL);
                applied++;
            } else {
                PWSTR parse = NULL, disp = NULL;
                if (SUCCEEDED(SHGetNameFromIDList(full, SIGDN_DESKTOPABSOLUTEPARSING, &parse))) {
                    WCHAR name[48] = L"";
                    if (SUCCEEDED(SHGetNameFromIDList(full, SIGDN_NORMALDISPLAY, &disp))) {
                        wcpy(name, 48, disp);
                        CoTaskMemFree(disp);
                    }
                    DropApply(&c, (int)k - 1, parse, name);   // 空名回落 NameFromPath
                    applied++;
                    CoTaskMemFree(parse);
                }
            }
            CoTaskMemFree(full);
        }
        DropFinish(h, &c);
        GlobalUnlock(med.hGlobal);
    }
    // 尺寸/锁定防御分支：不合法数据静默丢弃（applied 保持 0），无收尾副作用
    ReleaseStgMedium(&med);
    return applied;
}

// 外部拖放悬停：落点格高亮与内部排序拖动同源（g_dropCell/g_dropIn 同款
// 更新与失效）；光标在侧栏则高亮分组行（落组目标）。光标捕获在拖放源，
// 本窗只收 DragOver 的屏幕坐标
static void OleUpdateOver(POINTL ptl)
{
    POINT p = { ptl.x, ptl.y };
    ScreenToClient(g_hwnd, &p);
    // 侧栏分组行 = 落组目标（与 DropBegin 的 GroupAt 判定同一把尺）
    int ng = (p.x < g_sideW && p.y >= g_titleH) ? GroupAt(p) : -1;
    if (ng < 0) ng = -1;
    if (ng != g_oleGrp) {
        InvalidateGroup(g_oleGrp);     // vi<0 内部早退
        InvalidateGroup(ng);
        g_oleGrp = ng;
    }
    int nc = SlotAt(p);
    BOOL nin = p.x >= g_sideW && p.y >= g_titleH;
    if (nc != g_dropCell || nin != g_dropIn) {
        if (g_dropCell >= 0 && nc >= 0) InvalidateCells(g_dropCell, nc);
        else {
            InvalidateCell(g_dropCell);
            InvalidateCell(nc);
        }
        g_dropCell = nc;
        g_dropIn = nin;
    }
    // 网格上/下边缘 24px：自动滚动。拖放循环只在光标移动时来拍——
    // 光标停在边缘也要持续滚，靠 TIMER_DRAGSCROLL 自驱；滚动后按最后
    // 光标位置复算落点，高亮才跟得上内容位移
    int dir = 0;
    if (p.x >= g_sideW && p.y >= g_titleH && p.y < g_ch) {
        if (p.y < g_titleH + S_(24)) dir = -1;
        else if (p.y >= g_ch - S_(24)) dir = 1;
    }
    g_oleAutoPt.x = ptl.x;
    g_oleAutoPt.y = ptl.y;
    if (dir != g_oleAutoDir) {
        g_oleAutoDir = dir;
        if (dir) SetTimer(g_hwnd, TIMER_DRAGSCROLL, 30, NULL);
        else KillTimer(g_hwnd, TIMER_DRAGSCROLL);
    }
}

static void OleClearOver(void)
{
    int oc = g_dropCell, og = g_oleGrp;
    g_dropCell = -1;
    g_dropIn = FALSE;
    g_oleGrp = -1;
    g_oleAutoDir = 0;                  // 拖放结束：自动滚动一并停
    KillTimer(g_hwnd, TIMER_DRAGSCROLL);
    InvalidateCell(oc);                // vi<0 内部早退，无需判
    InvalidateGroup(og);
}

// 源允许集与目标偏好的交集：回收站这类虚拟项的拖放源可能只允许
// MOVE|LINK（"复制回收站"无意义，shell 会把 COPY 从允许集剔除）——
// 返回不在允许集里的效果会被源端掩成 NONE，光标全程禁止
static DWORD PickEffect(DWORD allowed)
{
    if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    if (allowed & DROPEFFECT_MOVE) return DROPEFFECT_MOVE;
    if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
    return DROPEFFECT_NONE;
}

// 数据对象里有哪种可收格式：文件（CF_HDROP）或 shell 项（SHELLIDLIST）。
// QueryGetData 全落空时再用 GetData 探一次——个别数据对象对查询报不出
// 实际有的格式（懒渲染）。命中即收，medium 立刻还给系统
static BOOL OleAccepts(IDataObject *d)
{
    FORMATETC ft = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    HRESULT q1 = IDataObject_QueryGetData(d, &ft);
    BOOL ok = q1 == S_OK;
    STGMEDIUM med;
    BOOL g1 = FALSE, g2 = FALSE;
    if (!ok && SUCCEEDED(IDataObject_GetData(d, &ft, &med))) {
        g1 = TRUE;                              // 查不到但取得到：懒渲染
        ReleaseStgMedium(&med);
    }
    HRESULT q2 = 0;
    if (g_cfShellIdList) {
        ft.cfFormat = (CLIPFORMAT)g_cfShellIdList;
        q2 = IDataObject_QueryGetData(d, &ft);
        ok = ok || q2 == S_OK;
        if (q2 != S_OK && !g1 && SUCCEEDED(IDataObject_GetData(d, &ft, &med))) {
            g2 = TRUE;
            ReleaseStgMedium(&med);
        }
    }
    return ok || g1 || g2;
}

// IDropTarget：进程级单例（静态对象，引用计数空转，随进程存活）。
// 只在 Drop/DragEnter 里区分格式，DragOver 只做高亮跟随。
// QI 里要返回自己，对象先占位声明，lpVtbl 由下方 vtbl 定义后、注册前填好
static IDropTarget g_drop;

static HRESULT __stdcall DtQueryInterface(IDropTarget *self, REFIID riid, void **out)
{
    UNREF(self);
    // IID_IUnknown / IID_IDropTarget 是 COM ABI 的固定值，自己定义即可（不引
    // uuid.lib）；逐字节比 16 字节与 IsEqualIID 完全等价
    static const IID kIID_IUnknown    = { 0x00000000, 0x0000, 0x0000,
                                          { 0xC0,0,0,0,0,0,0,0x46 } };
    static const IID kIID_IDropTarget = { 0x00000122, 0x0000, 0x0000,
                                          { 0xC0,0,0,0,0,0,0,0x46 } };
    if (out && (memcmp(riid, &kIID_IUnknown, 16) == 0 || memcmp(riid, &kIID_IDropTarget, 16) == 0)) {
        *out = (void *)&g_drop;
        return S_OK;
    }
    if (out) *out = NULL;
    return E_NOINTERFACE;
}

static ULONG __stdcall DtAddRef(IDropTarget *self)  { UNREF(self); return 2; }
static ULONG __stdcall DtRelease(IDropTarget *self) { UNREF(self); return 2; }

// 拖放悬停期间面板必须稳住：拖放光标进入时若正淡出（移出检测在拖放开始
// 前就动了手），立即就地回满——SW_SHOWNA 不抢拖放源的焦点（前台一抢拖放
// 就断）。完全隐藏的情况走钩子的 WM_APP_DRAGWAKE 唤醒，这里是兜底
static void DragSteadyShow(void)
{
    if (!IsWindowVisible(g_hwnd)) {
        ShowWindow(g_hwnd, SW_SHOWNA);
        StartFadeIn();                             // 从 0 起淡入并重置 3s 宽限
        SetTimer(g_hwnd, TIMER_HIDE, 120, NULL);   // StartFadeIn 不重启心跳：这里补上
    } else if (g_fadeDir < 0) {                    // 淡出中：就地回满，不再眨眼
        g_fadeDir = 0;
        KillTimer(g_hwnd, TIMER_FADE);
        g_alpha = 255;
        SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);
    }
}

static HRESULT __stdcall DtDragEnter(IDropTarget *self, IDataObject *d, DWORD keys,
                                     POINTL pt, DWORD *ef)
{
    UNREF(self); UNREF(keys);
    g_extDrag = TRUE;                  // 先冻结心跳隐藏，再稳住可见性
    DragSteadyShow();
    g_oleOk = OleAccepts(d);
    g_oleAllow = *ef;                  // 源允许集：返回值必须落在它的子集里
    *ef = g_oleOk ? PickEffect(g_oleAllow) : DROPEFFECT_NONE;
    if (g_oleOk) OleUpdateOver(pt);
    return S_OK;
}

static HRESULT __stdcall DtDragOver(IDropTarget *self, DWORD keys, POINTL pt, DWORD *ef)
{
    UNREF(self); UNREF(keys);
    *ef = g_oleOk ? PickEffect(g_oleAllow) : DROPEFFECT_NONE;
    if (g_oleOk) OleUpdateOver(pt);
    return S_OK;
}

static HRESULT __stdcall DtDragLeave(IDropTarget *self)
{
    UNREF(self);
    g_extDrag = FALSE;
    OleClearOver();
    return S_OK;
}

static HRESULT __stdcall DtDrop(IDropTarget *self, IDataObject *d, DWORD keys,
                                POINTL pt, DWORD *ef)
{
    UNREF(self); UNREF(keys);
    *ef = DROPEFFECT_NONE;
    g_extDrag = FALSE;
    OleClearOver();
    POINT p = { pt.x, pt.y };
    ScreenToClient(g_hwnd, &p);
    FORMATETC ft = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM med;
    if (SUCCEEDED(IDataObject_GetData(d, &ft, &med))) {
        UINT nf = DragQueryFileW((HDROP)med.hGlobal, 0xFFFFFFFF, NULL, 0);
        if (nf > 0) {
            HandleDroppedFiles(g_hwnd, (HDROP)med.hGlobal, p, nf);
            ReleaseStgMedium(&med);
            *ef = PickEffect(g_oleAllow);
            return S_OK;
        }
        // 关键：虚拟项（回收站等）的数据对象可能挂一个"零文件"的合法
        // HDROP——不能当成功收场，必须落到 SHELLIDLIST 路线拿 PIDL
        ReleaseStgMedium(&med);
    }
    if (g_cfShellIdList) {
        HandleShellIdList(g_hwnd, d, p);
        *ef = PickEffect(g_oleAllow);
    }
    return S_OK;
}

static IDropTargetVtbl g_dtVtbl = {
    DtQueryInterface, DtAddRef, DtRelease,
    DtDragEnter, DtDragOver, DtDragLeave, DtDrop,
};

// 注册 OLE 拖放：回收站等虚拟项只有 PIDL 格式，WM_DROPFILES 收不到。
// OleInitialize 失败（COM 不可用）就留在 DragAcceptFiles 老路，功能不倒退
static void InitOleDrop(HWND h)
{
    if (g_oleInit) return;            // 幂等：延迟初始化下每次唤醒都会走到这
    if (FAILED(OleInitialize(NULL))) return;
    g_oleInit = TRUE;
    g_drop.lpVtbl = &g_dtVtbl;
    g_dropOn = SUCCEEDED(RegisterDragDrop(h, &g_drop));
    g_cfShellIdList = RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
}

// TIMER_HIDE 心跳（120ms）：反馈过期收起、配置防抖落盘、自动隐藏判定。
// TIMER_FADE：淡入淡出步进
static void OnTimer(HWND h, WPARAM w)
{
    if (w == TIMER_HIDE) {
        if (g_flashAt && !FlashActive()) { g_flashAt = 0; InvalidateTitle(); }   // 反馈过期：收起
        if (g_cfgDirty) { g_cfgDirty = FALSE; SaveConfig(); }   // 待落盘的配置（缩放防抖）
        if (g_dlgOpen) {          // 对话框开着：定期裁剪，输入法/WinRT 的堆不驻留工作集。
                                  // 120ms 一次是空转——裁完立刻又被触碰，一秒一次足够
            static int tick;
            if (++tick >= 8) { tick = 0; TrimWS(); }
            g_outSince = 0;
            return;
        }
        if (g_menuOpen) {         // 弹出菜单开着（模态循环照派 WM_TIMER）：面板
            g_outSince = 0;       // 不在眼底时也别隐藏——用户还在读菜单
            return;
        }
        if (g_extDrag) {          // 外部拖放悬停中：绝不自动隐藏（进入时已就地稳住）
            g_outSince = 0;
            return;
        }
        if (g_pinned || !IsWindowVisible(h) || g_fadeDir) { g_outSince = 0; return; }
        if (GetTickCount64() - g_wakeAt < WAKE_GRACE_MS) { g_outSince = 0; return; }
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) { g_outSince = 0; return; }
        POINT p;
        GetCursorPos(&p);
        RECT r;
        GetWindowRect(h, &r);
        InflateRect(&r, CURSOR_SLOP, CURSOR_SLOP);
        if (PtInRect(&r, p)) {
            g_outSince = 0;
            if (g_fadeDir < 0) g_fadeDir = 1;
        } else {
            ULONGLONG now = GetTickCount64();
            if (!g_outSince) g_outSince = now;
            else if (now - g_outSince > HIDE_DELAY_MS) BeginHide(FALSE);   // 移出即开始消失
        }
    } else if (w == TIMER_FADE) {
        StepFade();
    } else if (w == TIMER_DRAGSCROLL) {
        // 拖放自动滚动：光标停在网格边缘也持续滚；滚不动（到顶/到底）即停表。
        // 滚动后按最后光标位置复算落点高亮，跟上内容位移
        if (!g_extDrag || !g_oleAutoDir) {
            KillTimer(h, TIMER_DRAGSCROLL);
            g_oleAutoDir = 0;
            return;
        }
        int old = g_scroll;
        g_scroll += g_oleAutoDir * S_(28);
        ClampScroll();
        if (g_scroll == old) {         // 到顶/到底：内容不再位移，停表省空转
            g_oleAutoDir = 0;
            KillTimer(h, TIMER_DRAGSCROLL);
            return;
        }
        RECT gr = { g_sideW, g_titleH, g_cw, g_ch };
        InvalidateRect(h, &gr, FALSE);
        OleUpdateOver((POINTL){ g_oleAutoPt.x, g_oleAutoPt.y });
    } else if (w == TIMER_ICONS) {
        // 缩放手势停稳：图标尺寸已定，丢掉旧图标，下次绘制按新尺寸重解析
        KillTimer(h, TIMER_ICONS);
        for (int i = 0; i < g_nItems; i++) DropIcon(i);
        InvalidateRect(h, NULL, FALSE);
    }
}

static void OnDestroy(HWND h)
{
    SaveConfig();
    TrayRemove();
    if (g_dropOn) { RevokeDragDrop(h); g_dropOn = FALSE; }
    if (g_oleInit) { g_oleInit = FALSE; OleUninitialize(); }   // Revoke 必须在 Uninitialize 之前
    if (g_hookH) {
        PostThreadMessageW(g_hookTid, WM_QUIT, 0, 0);
        WaitForSingleObject(g_hookH, 1000);
        CloseHandle(g_hookH);
        g_hookH = NULL;
    }
    UnregisterHotKey(h, HOTKEY_ID);
    for (int i = 0; i < g_nItems; i++) DropIcon(i);
    // GDI 对象收尾：数组一遍过（原来 13 条逐项 if+DeleteObject，缩进还乱）
    HGDIOBJ objs[] = {
        g_font, g_fontT,
        g_brBG, g_brSide, g_brCell, g_brCellHi, g_brLine,
        g_brAccent, g_brAccel, g_brPanel,
        g_brPinOff, g_brPinOn, g_brGlyph, g_brBtnPress, g_brClose, g_brClosePr,
        g_penLine, g_penAccent,
    };
    for (size_t i = 0; i < _countof(objs); i++)
        if (objs[i]) DeleteObject(objs[i]);
    ReleaseMemBuffer();           // 双缓冲：与 OnSize/OnPaint 的创建路径配对
    if (g_trayIcon) { DestroyIcon(g_trayIcon); g_trayIcon = NULL; }
    if (g_once) { CloseHandle(g_once); g_once = NULL; }   // 释放单实例互斥体
    PostQuitMessage(0);
}

// ---------------------------------------------------------------- 窗口过程
// 分发器：一屏读完，逻辑都在上面的处理函数里
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    // 资源管理器重启会广播 TaskbarCreated：托盘图标已丢失，重建
    if (g_msgTaskbarCreated && (UINT)m == g_msgTaskbarCreated) {
        TrayAdd();
        return 0;
    }
    switch (m) {
    // 关机/注销：DefWindowProc 对 WM_ENDSESSION 不落盘——待写配置会随进程
    // 退出丢掉。先写后放行（QUERY 返回 TRUE=允许会话结束）
    case WM_ENDSESSION:      SaveConfig();
    case WM_QUERYENDSESSION:
                               return TRUE;
    case WM_CREATE:            OnCreate(h);                              return 0;
    case WM_SIZE:              OnSize(h, l);                             return 0;
    case WM_GETMINMAXINFO: {   MINMAXINFO *mm = (MINMAXINFO *)l;
                               mm->ptMinTrackSize.x = S_(260);
                               mm->ptMinTrackSize.y = S_(200);          return 0; }
    case WM_NCHITTEST:       { LRESULT ht = OnNcHitTest(h, l);
                               if (ht) return ht; }                    break;
    case WM_DPICHANGED:        OnDpiChanged(h, w, l);                    return 0;
    case WM_PAINT:             OnPaint(h);                               return 0;
    case WM_ERASEBKGND:                                                  return 1;
    case WM_MOUSEMOVE:         OnMouseMove(h, l);                        return 0;
    case WM_MOUSELEAVE:        OnMouseLeave();                           return 0;
    case WM_LBUTTONDOWN:       OnLButtonDown(h, l);                      return 0;
    case WM_LBUTTONUP:         if (OnLButtonUp())                        return 0;
                               break;
    case WM_CAPTURECHANGED:    OnCaptureLost();                          return 0;
    case WM_RBUTTONUP:         OnRButtonUp(l);                           return 0;
    case WM_APP_WHEEL:
    case WM_MOUSEWHEEL:        OnWheel(h, m, w, l);                      return 0;
    case WM_KEYDOWN:           OnKeyDown(w);                             return 0;
    case WM_CHAR:              OnChar(w);                                return 0;
    case WM_IME_SETCONTEXT:                                              return 0;
    case WM_CLOSE:             CloseToTray();                            return 0;
    case WM_COMMAND:           if (LOWORD(w) == IDM_EXIT) { ExitApp(); return 0; }
                               break;
    case WM_DROPFILES:         OnDropFiles(h, w);                        return 0;
    case WM_TIMER:             OnTimer(h, w);                            return 0;
    case WM_HOTKEY:            if (IsWindowVisible(h)) BeginHide(TRUE);
                               else ShowLauncher();                      return 0;
    // 托盘图标：左键单击显示面板，右键弹菜单
    case WM_TRAY:            { UINT ev = LOWORD(l);   // NOTIFYICON_VERSION_4：事件在 lParam 低字
                               if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK) ShowLauncher();
                               else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) TrayShowMenu();
                                                                        return 0; }
    case WM_APP_WAKE:          ShowLauncher();                           return 0;
    // 桌面双击唤醒候选：钩子只报时序，这里在 UI 线程确认落点确实是桌面再唤醒
    case WM_APP_WAKE_CAND:   { POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
                               if (!IsWindowVisible(h) && IsDesktopPoint(p)) ShowLauncher();
                                                                        return 0; }
    // 拖放唤醒：面板隐藏时拖动光标进入其静置区——就地显示但不抢前台
    //（SW_SHOWNA：前台一抢，拖放源的拖动就断了），位置不动（落点就在原地）
    case WM_APP_DRAGWAKE:      if (!IsWindowVisible(h) && !g_dlgOpen) {
                                   InitOleDrop(h);      // 先注册拖放目标再显示：
                                   WakePrune();        // 可被命中时目标必已就位
                                   ShowWindow(h, SW_SHOWNA);
                                   StartFadeIn();
                                   SetTimer(h, TIMER_HIDE, 120, NULL);
                               }                                     return 0;
    case WM_DESTROY:           OnDestroy(h);                             return 0;
    default:                                                             break;
    }
    return DefWindowProcW(h, m, w, l);
}
// ---------------------------------------------------------------- 崩溃取证
// buildclaunch.bat 的 /MAP 产出 claunch.map（RVA→符号）就是为这里服务的：未处理异常
// 时把指令指针的 RVA 写进 exe 同目录的 claunch_crash.txt，拿 RVA 对 map 一查
// 即得崩溃点函数——单文件工具没有调试器，这是唯一的取证据手段。
// 处理器里只用最朴素的 WinAPI（防二次崩溃），写完交还系统按默认路径终止
// 崩溃处理器里只用栈上的手写拼装：不调 CRT 的格式化（那是整条 printf 链，
// 也是二次崩溃的来源），也不分配
static int PutA(char *d, const char *s)
{
    int n = 0;
    while (s[n]) { d[n] = s[n]; n++; }
    return n;
}

static int HexA(char *d, unsigned long long v, int width)
{
    static const char dig[] = "0123456789ABCDEF";
    char tmp[16];
    int n = 0;
    do { tmp[n++] = dig[v & 15]; v >>= 4; } while (v);
    int w = n > width ? n : width;
    for (int i = 0; i < w; i++) d[i] = (i < w - n) ? '0' : tmp[w - 1 - i];
    return w;
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS *ep)
{
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    WCHAR *slash = wrchr_(path, L'\\');
    if (slash) slash[1] = 0;
    wcat(path, MAX_PATH, L"claunch_crash.txt");
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD_PTR rva = (DWORD_PTR)ep->ContextRecord->Rip -
                        (DWORD_PTR)GetModuleHandleW(NULL);
        char buf[96];
        int n = 0;
        n += PutA(buf + n, "code=0x");
        n += HexA(buf + n, (unsigned long long)ep->ExceptionRecord->ExceptionCode, 8);
        n += PutA(buf + n, " rva=0x");
        n += HexA(buf + n, (unsigned long long)rva, 0);
        n += PutA(buf + n, "\r\n");
        DWORD w = 0;
        WriteFile(f, buf, (DWORD)n, &w, NULL);
        CloseHandle(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---------------------------------------------------------------- 入口
static HWND g_searchHit = NULL;

static BOOL CALLBACK FindLauncherCb(HWND h, LPARAM lp)
{
    WCHAR cls[64];
    UNREF(lp);
    GetClassNameW(h, cls, 64);
    if (!wcmp_(cls, APP_CLASS)) { g_searchHit = h; return FALSE; }
    return TRUE;
}

static HWND FindLauncherWindow(void)
{
    g_searchHit = NULL;
    EnumWindows(FindLauncherCb, 0);
    return g_searchHit;
}

// 两个窗口类（主窗/对话框）注册参数完全相同，收拢一处
static ATOM RegClass(const WCHAR *name, WNDPROC proc)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = name;
    return RegisterClassExW(&wc);
}

static int AppMain(HINSTANCE inst)
{
    g_inst = inst;

    // 单实例权威判定：命名互斥体（窗口类检查有 race——同时启动互查不到对方
    // 的窗会双开）。内核保证原子、进程被杀自动释放；Local\ 每会话一个
    g_once = CreateMutexW(NULL, TRUE, L"claunch_single_instance");
    if (g_once && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND exist = FindLauncherWindow();
        if (exist) PostMessageW(exist, WM_APP_WAKE, 0, 0);   // 已有实例：转唤醒
        CloseHandle(g_once);
        g_once = NULL;
        return 0;
    }

    HWND exist = FindLauncherWindow();
    if (exist) {
        PostMessageW(exist, WM_APP_WAKE, 0, 0);
        return 0;
    }

    // 两个导出都取自 user32；取不到模块（不可能发生）则指针留空——
    // 下游全有 NULL 路径（DPI 走默认、图标链跳过 PrivateExtractIconsW）
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        pSetDpi = (PFN_SetProcDpiCtx)
            GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (pSetDpi) pSetDpi((HANDLE)-4);   // PerMonitorV2
        pExtractIcons = (PFN_PrivateExtractIconsW)
            GetProcAddress(u32, "PrivateExtractIconsW");
    }

    HDC scr = GetDC(NULL);
    g_dpi = GetDeviceCaps(scr, LOGPIXELSX);
    ReleaseDC(NULL, scr);

    SetUnhandledExceptionFilter(CrashFilter);   // 崩溃取证：先于一切业务初始化装上

    IniPath();
    ScaleMetrics();
    LoadConfig();
    InitAutostart();   // 开机自启状态以注册表为准（托盘菜单勾选用）；路径过期则自愈重写
    g_clipFmt = RegisterClipboardFormatW(L"ClaunchItem");   // 私有剪贴板格式；失败则 Ctrl+V 只认纯文本

    RegClass(APP_CLASS, WndProc);
    RegClass(DLG_CLASS, DlgProc);   // 对话框类只注册一次（原在 RunDialog 里反复注册）
    RegClass(CAP_CLASS, CapProc);

    g_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_LAYERED, APP_CLASS, APPNAME, WS_POPUP,
        g_savedX, g_savedY, g_savedW, g_savedH, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;

    // 主窗没有输入框：断开 IME——否则窗口一拿焦点，整条输入法链
    // （~30MB）就被拉进进程。属性对话框保持默认 IME，中文输入不受影响
    ImmAssociateContext(g_hwnd, NULL);

    // OLE 拖放刻意不在此初始化（见 InitOleDrop 注释）：静默驻托盘时注册
    // 拖放目标毫无意义，而 OleInitialize 一次性烫入 ~16MB COM 基础设施

    ClampToWorkArea();
    // 静默启动是唯一行为：直接进托盘，停掉移出检测心跳（不可见窗口用不到，
    // 唤醒时 ShowLauncher 重启）；失效清理延到首次唤醒（托盘就绪不等磁盘）
    g_needPrune = TRUE;
    ShowWindow(g_hwnd, SW_HIDE);
    KillTimer(g_hwnd, TIMER_HIDE);
    ReleaseMemBuffer();                // 双缓冲 ~2MB 从未被画过：直接放
    TrimWS();                          // 托盘就绪即回落低位工作集（软调入按需，无感）

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}


// ---------------------------------------------------------------- 入口
// CLAUNCH_NOCRT：不链 CRT（libcmt/ucrt）的入口自举。同样内容的开关在 build.bat
// 里由 /D CLAUNCH_NOCRT + /NODEFAULTLIB + /ENTRY:wWinMainCRTStartup 打开。
// 少掉的正是 CRT 的启动开销（空 GUI 程序的地板就有 102KB）
#ifndef CLAUNCH_NOCRT
// 链回 CRT 时的入口壳子：CRT 启动后调 wWinMain，我们只做 AppMain 的转发
int WINAPI wWinMain(HINSTANCE a, HINSTANCE b, LPWSTR c, int d)
{
    UNREF(b); UNREF(c); UNREF(d);
    return AppMain(a);
}
#endif

#ifdef CLAUNCH_NOCRT

// /GS 安全 cookie：uCRT 的存储与初始化都不在，自己来。熵用时间戳与栈地址
// 抖动——比 uCRT 的多源熵弱，但比没有强得多
uintptr_t __security_cookie = 0x00002B992DDFA232ULL;

void __cdecl __security_init_cookie(void)   // 签名与 vcruntime.h 一致
{
    uintptr_t c = (uintptr_t)GetTickCount64();
    c ^= (uintptr_t)&c;
    c ^= c >> 32;
    if (c < 0x1000) c |= 0x1000;             // 太小等于没有
    __security_cookie = c;
}

__declspec(noinline) void __security_check_cookie(uintptr_t cookie)
{
    if (cookie != __security_cookie) __fastfail(FAST_FAIL_STACK_COOKIE_CHECK_FAILURE);
}

__declspec(noreturn) void __report_rangecheckfailure(void)
{
    __fastfail(FAST_FAIL_RANGE_CHECK_FAILURE);
}

// __chkstk 与 __GSHandlerCheck 是编译器/汇编侧的桩，见 chkstk.asm

// 入口：没有 CRT，自己取模块句柄；跑完直接 ExitProcess。
// 注意 __security_init_cookie 必须在任何带 cookie 的函数之前跑
void __stdcall wWinMainCRTStartup(void)
{
    __security_init_cookie();
    g_inst = GetModuleHandleW(NULL);
    ExitProcess((UINT)AppMain(g_inst));
}

#endif // CLAUNCH_NOCRT
