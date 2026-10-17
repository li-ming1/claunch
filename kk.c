// kk.c — 桌面格子：单文件、零第三方依赖的桌面启动器
// 左侧自定义分组（行可拖拽排序，决定滚轮切换顺序）、正方形密集格子（槽位模型：
// 拖放即与落点互换、清空不挪其他格、可插入/删除格子、窗口铺满重排、边缘可拉伸）、
// 拖程序进格子单击启动（标题栏闪现启动结果）、启动时自动清理失效项目、悬停顶部居中显示名称、
// 鼠标移出自动隐藏、置顶按钮（常驻+置顶二合一）、双击桌面空白唤醒、Ctrl+Alt+K 快捷键、
// 右键格子可编辑 名称/文件名/参数/工具提示。
// 键盘：方向键移动悬停高亮、回车或数字键 1-9 启动、Ctrl+滚轮缩放格子（40~160，配置记忆）、
// 直接键入即按名称搜索过滤（Esc 清空；拖动排序/拖放会清空过滤回全量视图）。
// Ctrl+C/V：复制悬停格 / 粘贴到悬停格或追加（剪贴板同时带一份可外粘的路径文本）。
// 托盘菜单可开/关：开机自启（写 HKCU Run 键）、静默启动（直接进托盘）。
// 内存取向：配置流式读写，图标按需加载，仅依赖 Win32 + shell32。
// 绘制走整窗离屏缓冲（~2.4MB 提交，闲置被裁剪不驻留）：直接绘制会在快速滚动时
// 露出"画一半"的中间帧，产生频闪；离屏画完一次 BitBlt 上屏就没有这个问题。
// 常驻内存红线（实测）：SHGetFileInfo 只作图标轻量链路失败后的兜底、不走
// ShellExecute 常规启动（各有 .lnk 解析 + CreateProcess 直启替代）、主窗断开
// IME——破防会把 15~30MB 的外部模块链（shell 管线 / WinRT / 输入法）拖进进程。
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <imm.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "advapi32.lib")   // 开机自启的注册表操作；advapi32 本就经 shell32 间接映射，无新增 DLL

// ---------------------------------------------------------------- 常量
#define MAX_GROUPS      16
#define MAX_ITEMS       128
#define APP_CLASS       L"KKLauncherWnd"
#define DLG_CLASS       L"KKDialogWnd"
#define APPNAME         L"桌面格子"
#define ININAME         L"kk.ini"

#define WM_APP_WAKE     (WM_APP + 1)
#define WM_APP_WHEEL    (WM_APP + 3)   // 低级钩子转发的滚轮（面板置顶且未聚焦时也可用）
#define WM_APP_WAKE_CAND (WM_APP + 4)  // 钩子转发的“疑似桌面双击”候选，UI 线程确认后再唤醒
#define TIMER_HIDE      1
#define TIMER_FADE      2
#define HOTKEY_ID       1

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
#define IDM_TRAY_SILENT     1104

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
static int   g_nGroups = 1;

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
static HPEN   g_penDim, g_penAccent;      // 标题栏图形（缓存，见 MakeGdiObjects）
static HBRUSH g_brPinOn, g_brPinOff;
static HPEN   g_penLine;                  // 格子边框（同出处）
static int    g_cw, g_ch;
static HDC    g_memDC;          // 离屏缓冲：WM_SIZE 重建，WM_PAINT 画进去再整体上屏
static HBITMAP g_memBmp;
static BOOL   g_memFresh;       // 缓冲重建后第一帧必须整窗重画

static int  g_titleH = 34, g_sideW = 104, g_gap = 3, g_cell = 72, g_grpH = 34;   // 格子正方形
static int  g_cols = 1;    // 列数：只随窗口宽度变（WM_SIZE→UpdateCell），命中/绘制路径缓存读
static int  g_slots = 1;   // 可视格数=列×行：同出处缓存，命中/绘制/滚动钳制直接读
static int  g_cellBase = 56;   // 格子目标宽基准：Ctrl+滚轮调节，配置持久化（UpdateCell 据此算列数）
static BOOL g_pinned = FALSE;    // 置顶：常驻不自动隐藏 + HWND_TOPMOST（原“定住”与“置顶”合并）
static BOOL g_silent = FALSE;    // 静默启动：启动直接进托盘不淡入（配置持久化）
static BOOL g_autostart = FALSE; // 开机自启：以 HKCU Run 键为准，启动时读入（不进配置）
static UINT g_clipFmt = 0;       // 私有剪贴板格式原子（Ctrl+C/V；注册失败则只用纯文本）
static BOOL g_cfgDirty = FALSE;  // 配置有待落盘：缩放等连续操作防抖，避免每格滚轮都写文件
static int  g_curGroup = 0;
static WCHAR g_filter[24];   // 名称搜索过滤词（瞬态，不持久化）：非空时视图只保留名称含它的条目
static int  g_scroll = 0;       // 格子区滚动（像素）
static int  g_grpScroll = 0;    // 侧栏滚动（像素）

static int  g_hoverCell = -1, g_hoverGroup = -1, g_hoverBtn = 0; // btn: 1 钉 2 关
static WCHAR g_hoverText[80];
static BOOL  g_cardOn = FALSE;  // 悬停小卡片可见（悬停格子/分组时浮在其上方）
static RECT  g_cardRect;        // 卡片区域：悬停目标变化时重算，绘制只读
static WCHAR g_cardText[80];    // 卡片文字：程序名/组名
static WCHAR g_flashText[80];   // 标题栏启动反馈（"已启动/启动失败: 名"）
static DWORD g_flashAt = 0;     // 反馈起始时刻；1.2s 后自然过期（0=无）

static int  g_pressCell = -1;   // 按下的格子：原地抬起=启动，拖过阈值=排序
static BOOL g_dragging;         // 已越过拖动阈值
static int  g_dropCell = -1;    // 拖动悬停的目标格（视图序号）
static BOOL g_dropIn;           // 目标在格子区内（区外释放=取消）
static int  g_pressGroup = -1;  // 按下的分组行（侧栏重排用；组 0“全部”不参与）
static BOOL g_dragGroup;        // 分组重排中（已越过拖动阈值）
static int  g_dropGroup = -1;   // 分组重排的落点行
static POINT g_pressPt;
static BOOL g_hoverActive = FALSE;
static BOOL g_tracking = FALSE;

static DWORD g_outSince = 0;
static DWORD g_wakeAt = 0;
static int   g_alpha = 255, g_fadeDir = 0;
static BOOL  g_dlgOpen = FALSE;

static HHOOK g_hook = NULL;
static HANDLE g_hookH;              // 钩子专用线程：回调不再与绘制抢 UI 线程
static DWORD  g_hookTid;
static WCHAR g_ini[MAX_PATH];
static int   g_savedX, g_savedY, g_savedW, g_savedH;
static BOOL  g_applySavedRect = FALSE;

// 托盘
static HICON g_trayIcon = NULL;
static UINT  g_msgTaskbarCreated = 0;
static NOTIFYICONDATAW g_nid;

typedef BOOL (WINAPI *PFN_SetProcDpiCtx)(HANDLE);
static PFN_SetProcDpiCtx pSetDpi = NULL;

#define UNREF(x) ((void)(x))

// 前向声明：只保留使用点早于定义的函数
static void ClampToWorkArea(void);
static void MoveToPoint(POINT pt);
static void BeginHide(BOOL force);
static void SaveConfig(void);
static void InvalidateTitle(void);
static void InvalidateCell(int vi);
static int  PopupAt(HMENU m);
static int  Rows(void);
static int  DefaultGroup(void);
static BOOL  UpdateHoverText(void);
static void RefreshCard(void);
static void UpdateTrayTip(void);

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

// ---------------------------------------------------------------- 工具
static int S_(int v) { return MulDiv(v, g_dpi, 96); }

// 格子缩放基准的合法区间（配置与 Ctrl+滚轮共用同一处钳制）
static int ClampCellBase(int v) { return v < 40 ? 40 : v > 160 ? 160 : v; }

// 图标尺寸随缩放等级走：加载（EnsureIcon）与绘制（PaintGrid）共用同一公式，
// 保证拿到的图标就是绘制尺寸，不重采样；只跟 g_cellBase 走而不跟 g_cell 走，
// 窗口缩放时图标尺寸不变，不会引发重新加载
static int IconSize(void)
{
    int v = S_(g_cellBase) * 3 / 7;
    return v < 12 ? 12 : v > 64 ? 64 : v;
}

// 把 '|' 换成 '_'，避免破坏配置格式
static const WCHAR *SafeField(WCHAR *dst, int cch, const WCHAR *src)
{
    wcsncpy_s(dst, (size_t)cch, src, _TRUNCATE);
    for (WCHAR *p = dst; *p; p++) if (*p == L'|') *p = L'_';
    return dst;
}

// 大小写不敏感的子串查找（名称搜索过滤用；ASCII 折叠大小写，CJK 原样比较。
// 不引 shlwapi 的 StrStrIW——少一个 DLL 依赖，且名称≤47 字符，O(n*m) 可忽略）
static BOOL ContainsText(const WCHAR *hay, const WCHAR *needle)
{
    if (!needle[0]) return TRUE;
    for (const WCHAR *p = hay; *p; p++) {
        const WCHAR *a = p, *b = needle;
        while (*b) {
            WCHAR ca = *a, cb = *b;
            if (ca >= L'A' && ca <= L'Z') ca += 32;
            if (cb >= L'A' && cb <= L'Z') cb += 32;
            if (ca != cb) break;
            a++; b++;
        }
        if (!*b) return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------- 布局
// 列数按目标格宽最近取整；格子尺寸由列数反推，横向正好铺满（保持正方形）。
// g_cols/g_slots 只在 UpdateCell 里算（WM_SIZE/DPI 变化时刷新），
// 命中/绘制/滚动钳制路径直接读缓存，不经过包装函数
static void UpdateCell(void)
{
    int w = g_cw - g_sideW;
    int step = S_(g_cellBase) + g_gap;
    int c = (w + g_gap + step / 2) / step;
    g_cols = c < 1 ? 1 : c;
    g_cell = (g_cw - g_sideW - (g_cols - 1) * g_gap) / g_cols;
    if (g_cell < 8) g_cell = 8;
    g_slots = g_cols * Rows();   // 每帧命中/绘制/滚动钳制直接读缓存
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
    for (int i = 0; i < g_nItems; i++) {
        if (g_curGroup != 0 && g_items[i].group != g_curGroup) continue;
        if (!ContainsText(g_items[i].name, g_filter)) continue;   // 搜索过滤：与分组条件链在同一遍扫描里
        g_viewIdx[g_viewN++] = i;
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

static int CellAt(POINT p)
{
    if (p.x < g_sideW || p.y < g_titleH) return -1;
    int c = g_cols;
    int col = (p.x - g_sideW) / (g_cell + g_gap);
    int row = (p.y - g_titleH + g_scroll) / (g_cell + g_gap);
    if (col < 0 || row < 0 || col >= c) return -1;
    int vi = row * c + col;
    if (vi >= g_slots || vi >= ViewCount()) return -1;
    RECT r = CellRect(vi);
    return PtInRect(&r, p) ? vi : -1;    // 格子间隙里的点击不算命中（保持原语义）
}

// 原始格位：不筛已占项，空格也返回序号（拖动落点高亮用）；超出可视格返回 -1
static int SlotAt(POINT p)
{
    if (p.x < g_sideW || p.y < g_titleH) return -1;
    int c = g_cols;
    int col = (p.x - g_sideW) / (g_cell + g_gap);
    int row = (p.y - g_titleH + g_scroll) / (g_cell + g_gap);
    int vi = row * c + col;
    return vi < g_slots ? vi : -1;
}

static void ClampScroll(void)
{
    int need = ViewCount();
    int visible = g_slots;
    int maxS = need > visible ? (need - visible + g_cols - 1) / g_cols * (g_cell + g_gap) : 0;
    if (g_scroll > maxS) g_scroll = maxS;
    if (g_scroll < 0) g_scroll = 0;
}

static void ClampGrpScroll(void)
{
    int content = g_nGroups * g_grpH + 12;
    int view = g_ch - g_titleH;
    int maxS = content > view ? content - view : 0;
    if (g_grpScroll > maxS) g_grpScroll = maxS;
    if (g_grpScroll < 0) g_grpScroll = 0;
}

// 分组多到装不下时，把指定分组滚进可视区（最小滚动量）
static void EnsureGroupVisible(int gi)
{
    int view = g_ch - g_titleH;
    int top = 6 + gi * g_grpH;
    int bottom = top + g_grpH;
    if (top - g_grpScroll < 0) g_grpScroll = top;
    else if (bottom - g_grpScroll > view) g_grpScroll = bottom - view;
    ClampGrpScroll();
}

// 切换到指定分组（点击和滚轮共用）
static void SelectGroup(int gi)
{
    if (gi < 0 || gi >= g_nGroups || gi == g_curGroup) return;
    g_curGroup = gi;
    ViewDirty();
    g_scroll = 0;
    ClampScroll();
    EnsureGroupVisible(gi);
    UpdateHoverText();
    InvalidateRect(g_hwnd, NULL, FALSE);   // 标题/侧栏/格子全区都变（原三个分区矩形的并集就是全窗）
    RefreshCard();          // 视图换了：同一悬停格对应的条目可能变了，卡片重锚
    UpdateWindow(g_hwnd);   // 滚轮连发时 WM_PAINT 会被消息饿死，立即同步重绘
}

static RECT GroupRect(int i)
{
    int top = g_titleH + 6 + i * g_grpH - g_grpScroll;
    RECT r = { 6, top, g_sideW - 6, top + g_grpH - 3 };
    return r;
}

static int GroupAt(POINT p)
{
    if (p.x >= g_sideW || p.y < g_titleH) return -1;
    int i = (p.y - (g_titleH + 6) + g_grpScroll) / g_grpH;
    if (i >= 0 && i < g_nGroups) {
        RECT r = GroupRect(i);
        if (p.y >= r.top && p.y < r.bottom) return i;
    }
    return -1;
}

// 标题栏右侧按钮：1=钉住 2=关闭（编码同 g_hoverBtn）
static RECT BtnRect(int b)
{
    int w = S_(30), h = S_(22), t = (g_titleH - h) / 2;
    int x = g_cw - 2 - w - (b == 1 ? w + 4 : 0);
    RECT r = { x, t, x + w, t + h };
    return r;
}

static int BtnAt(POINT p)
{
    RECT r;
    r = BtnRect(1); if (PtInRect(&r, p)) return 1;
    r = BtnRect(2); if (PtInRect(&r, p)) return 2;
    return 0;
}

// ---------------------------------------------------------------- 图标（按需加载）
// 首选轻量链：PrivateExtractIconsW 直接读文件资源 → 失败再 ExtractIconExW /
// 资源映射 → 注册表关联链（扩展名→ProgID→DefaultIcon）→ 全部失败才
// SHGetFileInfo（资源管理器同款；会拖来 shell 图标管线 shell.dll、PROPSYS、
// WindowsCodecs、Windows.Storage…约 15MB 映像且常驻，故只做最后兜底，
// 代价仅由轻量链确实解不出的文件类型承担，如打包应用关联）。
typedef int (WINAPI *PFN_PrivateExtractIconsW)(LPCWSTR, int, int, int,
                                               HICON *, int *, int, int);
static PFN_PrivateExtractIconsW pExtractIcons = NULL;

// 从映像文件（exe/dll/ico…）取指定大小/索引的图标；失败返回 NULL
static HICON IconFromImage(const WCHAR *path, int want, int idx)
{
    HICON icons[1] = { 0 };
    int ids[1] = { 0 };
    if (pExtractIcons && pExtractIcons(path, idx, want, want, icons, ids, 1, 0) > 0 && icons[0])
        return icons[0];
    // 部分程序图标只在特定尺寸档有资源：换标准档位再试（DrawIconEx 绘制时会缩放到位）
    if (pExtractIcons) {
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
            HRSRC hi = FindResourceW(hm, MAKEINTRESOURCEW(pic), RT_ICON);
            HGLOBAL gi = hi ? LoadResource(hm, hi) : NULL;
            HICON *p = gi ? (HICON *)LockResource(gi) : NULL;
            if (p && *p)
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
        if (big) return big;
        HICON scaled = (HICON)CopyImage(sml, IMAGE_ICON, want, want, LR_DEFAULTCOLOR);
        DestroyIcon(sml);
        return scaled;
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
                int m = (int)wcslen(li->target);
                while (s < se && *s && m < MAX_PATH - 1) li->target[m++] = *s++;
                li->target[m] = 0;
            }
        } else if (sfxoff && sfxoff < lisz) {
            const char *s  = (const char *)(linfo + sfxoff);
            const char *se = (const char *)(linfo + lisz);
            int r = 0;
            while (s + r < se && s[r]) r++;
            int m = (int)wcslen(li->target);
            if (r) {
                MultiByteToWideChar(CP_ACP, 0, s, r, li->target + m, MAX_PATH - m);
                li->target[MAX_PATH - 1] = 0;
            }
        }
    }
    // ICON_LOCATION 的 ",N" 后缀 → 图标索引；没有则用头部 IconIndex
    WCHAR *c = wcsrchr(li->iconLoc, L',');
    if (c && c[1] >= L'0' && c[1] <= L'9') {
        li->iconIdx = _wtoi(c + 1);
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

// 文件夹：画一个最简琥珀色文件夹。颜色位图 alpha 全 0 时 CreateIconIndirect
// 回退按掩码渲染，掩码全 0 即全不透明，掩码位图不用画。
static HICON MakeFolderIcon(int sz)
{
    HDC scr = GetDC(NULL);
    HDC dc = CreateCompatibleDC(scr);
    if (!dc) { ReleaseDC(NULL, scr); return NULL; }
    HBITMAP col = CreateBitmap(sz, sz, 1, 32, NULL);
    HBITMAP msk = CreateBitmap(sz, sz, 1, 1, NULL);
    HICON ico = NULL;
    if (col && msk) {
        HGDIOBJ oc = SelectObject(dc, col);
        HGDIOBJ om = SelectObject(dc, msk);
        HBRUSH fb  = CreateSolidBrush(RGB(0xE2, 0xB2, 0x3E));
        RECT tab   = { sz / 8, sz / 5, sz / 8 + sz / 3, sz / 5 + sz / 6 };
        RECT body  = { sz / 8, sz / 5 + sz / 6 - 1, sz - sz / 8, sz - sz / 5 };
        FillRect(dc, &tab, fb);
        FillRect(dc, &body, fb);
        DeleteObject(fb);
        ICONINFO ii;
        ZeroMemory(&ii, sizeof ii);
        ii.fIcon = TRUE;
        ii.hbmColor = col;
        ii.hbmMask = msk;
        ico = CreateIconIndirect(&ii);
        SelectObject(dc, oc);
        SelectObject(dc, om);
    }
    if (col) DeleteObject(col);
    if (msk) DeleteObject(msk);
    DeleteDC(dc);
    ReleaseDC(NULL, scr);
    return ico;
}

// 扩展名判断：.lnk 检测等处共用（大小写不敏感）
static BOOL HasExt(const WCHAR *path, const WCHAR *ext)
{
    const WCHAR *dot = wcsrchr(path, L'.');
    return dot && !_wcsicmp(dot, ext);
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
        WCHAR *e = wcschr(icon + 1, L'"');
        if (!e) return NULL;
        *e = 0;
        if (e[1] == L',') idx = _wtoi(e + 2);
        icon = icon + 1;
    } else {
        WCHAR *c = wcsrchr(icon, L',');        // 从右往左找逗号（路径可能含逗号）
        if (c && c[1]) { *c = 0; idx = _wtoi(c + 1); }
    }
    if (!icon[0]) return NULL;
    if (!wcscmp(icon, L"%1") || !wcscmp(icon, L"%0")) icon = (WCHAR *)src;
    return IconFromImage(icon, want, idx);
}

// 文档类型关联图标：扩展名 → ProgID → DefaultIcon（.docx→Word、.txt→txtfile…）。
// 手工走注册表，不碰 SHGetFileInfo 的 shell 图标管线（文件头内存红线）。
// 机器默认 ProgID 没有 DefaultIcon 时回落到用户选择的 ProgID（FileExts\UserChoice）
static HICON AssocIcon(const WCHAR *path, int want)
{
    const WCHAR *ext = wcsrchr(path, L'.');
    if (!ext || wcslen(ext) < 2 || wcslen(ext) > 30) return NULL;
    WCHAR progid[128];
    DWORD cb = sizeof progid;
    if (RegGetValueW(HKEY_CLASSES_ROOT, ext, NULL, RRF_RT_REG_SZ,
                     NULL, progid, &cb) != ERROR_SUCCESS || !progid[0])
        return NULL;
    WCHAR key[300];
    if (swprintf(key, 300, L"%s\\DefaultIcon", progid) >= 300) return NULL;
    HICON h = IconFromDefaultKey(key, path, want);
    if (h) return h;
    WCHAR uc[300];
    if (swprintf(uc, 300,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\%s\\UserChoice",
                 ext) >= 300) return NULL;
    WCHAR up[128];
    cb = sizeof up;
    if (RegGetValueW(HKEY_CURRENT_USER, uc, L"ProgID", RRF_RT_REG_SZ,
                     NULL, up, &cb) != ERROR_SUCCESS || !up[0])
        return NULL;
    if (swprintf(key, 300, L"%s\\DefaultIcon", up) >= 300) return NULL;
    return IconFromDefaultKey(key, path, want);
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

static HICON IconFor(const WCHAR *path, int want)
{
    HICON h = NULL;
    DWORD a = GetFileAttributesW(path);
    BOOL isDir = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    if (isDir) {
        h = FolderIcon(path, want);
    } else if (HasExt(path, L".lnk")) {
        LnkInfo lnk;
        if (ParseLnk(path, &lnk)) {
            if (lnk.iconLoc[0]) h = IconFromImage(lnk.iconLoc, want, lnk.iconIdx);
            if (!h && lnk.target[0]) {
                DWORD ta = GetFileAttributesW(lnk.target);
                if (ta != INVALID_FILE_ATTRIBUTES && (ta & FILE_ATTRIBUTE_DIRECTORY))
                    h = FolderIcon(lnk.target, want);   // 指向文件夹的快捷方式
                else {
                    h = IconFromImage(lnk.target, want, 0);
                    if (!h) h = ShellIcon(lnk.target);  // 目标：资源管理器同款
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
static int AddItemRaw(const WCHAR *name, const WCHAR *path, int group,
                      const WCHAR *args, const WCHAR *tip)
{
    if (g_nItems >= MAX_ITEMS) return -1;
    Item *it = &g_items[g_nItems];
    ZeroMemory(it, sizeof *it);
    wcsncpy_s(it->name, 48, name, _TRUNCATE);
    wcsncpy_s(it->path, MAX_PATH, path, _TRUNCATE);
    if (args) wcsncpy_s(it->args, 192, args, _TRUNCATE);
    if (tip)  wcsncpy_s(it->tip, 96, tip, _TRUNCATE);
    it->group = group;
    g_nItems++;
    ViewDirty();
    return g_nItems - 1;
}

static void NameFromPath(const WCHAR *path, WCHAR *out, int cch)
{
    const WCHAR *p = wcsrchr(path, L'\\');
    p = p ? p + 1 : path;
    const WCHAR *dot = wcsrchr(p, L'.');
    int n = dot ? (int)(dot - p) : (int)wcslen(p);
    if (n >= cch) n = cch - 1;
    if (n < 1) n = 1;
    wcsncpy_s(out, (size_t)cch, p, (size_t)n);
}

static int AddItemByPath(const WCHAR *path, int group)
{
    WCHAR name[48];
    NameFromPath(path, name, 48);
    return AddItemRaw(name, path, group, NULL, NULL);
}

// 把格子 ii 改成指向 path 的新程序：取名、换目标、清参数、重取图标。
// 拖放替换与未来的“改目标”路径共用（ShowCellMenu 属性框保持自己的字段级逻辑）
static void SetItemTarget(int ii, const WCHAR *path, int group)
{
    DropIcon(ii);
    WCHAR name[48];
    NameFromPath(path, name, 48);
    wcsncpy_s(g_items[ii].name, 48, name, _TRUNCATE);
    wcsncpy_s(g_items[ii].path, MAX_PATH, path, _TRUNCATE);
    g_items[ii].group = group;
    g_items[ii].args[0] = 0;
    EnsureIcon(ii);
}

static void RemoveItemAt(int k)
{
    if (k < 0 || k >= g_nItems) return;
    DropIcon(k);
    for (int i = k; i < g_nItems - 1; i++) g_items[i] = g_items[i + 1];
    g_nItems--;
    ViewDirty();
}

// 侧栏分组重排：把第 from 组移到 to 位，中间顺移。
// 组 0（“全部”）是伪分组，不参与。条目的 group 编号与被移动组跨越的区间
// 一起平移，当前组跟着走——否则组序变了、条目还挂在旧编号上
static void MoveGroup(int from, int to)
{
    if (from <= 0 || to <= 0 || from >= g_nGroups || to >= g_nGroups || from == to) return;
    Group tmp = g_groups[from];
    if (from < to)
        for (int k = from; k < to; k++) g_groups[k] = g_groups[k + 1];
    else
        for (int k = from; k > to; k--) g_groups[k] = g_groups[k - 1];
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
    int g = (g_curGroup == 0) ? DefaultGroup() : g_curGroup;
    while (ViewCount() <= slot && g_nItems < MAX_ITEMS)
        AddItemRaw(L"", L"", g, NULL, NULL);
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
    int g = (g_curGroup == 0) ? DefaultGroup() : g_curGroup;
    for (int i = g_nItems; i > pos; i--) g_items[i] = g_items[i - 1];
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
// URL（含 ://）不走 GetFileAttributes；UNC 断连会让启动卡到网络超时，也跳过。
// 空格（空路径）不是失效项：那是用户保留的空位，不能清。
static BOOL PruneInvalidItems(void)
{
    BOOL removed = FALSE;
    for (int i = 0; i < g_nItems; ) {
        const WCHAR *p = g_items[i].path;
        if (!p[0]) { i++; continue; }
        if (!wcsstr(p, L"://") && !(p[0] == L'\\' && p[1] == L'\\') &&
            GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) {
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
    wcsncpy_s(g_groups[g_nGroups].name, 48, name, _TRUNCATE);
    return g_nGroups++;
}

static void DeleteGroup(int i)
{
    if (i <= 0 || i >= g_nGroups) return;
    for (int k = 0; k < g_nItems; ) {
        if (g_items[k].group == i) RemoveItemAt(k);
        else k++;
    }
    for (int k = i; k < g_nGroups - 1; k++) g_groups[k] = g_groups[k + 1];
    g_nGroups--;
    for (int k = 0; k < g_nItems; k++)
        if (g_items[k].group > i) g_items[k].group--;
    if (g_curGroup >= g_nGroups) g_curGroup = 0;
    ViewDirty();
    ClampScroll();
    RefreshCard();       // 视图/分组列表变了：悬停目标可能已失效，卡片重算
}

static int DefaultGroup(void)
{
    if (g_nGroups > 1) return 1;
    return AddGroup(L"常用");
}

static void NewGroup(void)
{
    WCHAR name[48];
    swprintf(name, 48, L"新分组%d", g_nGroups);
    int ni = AddGroup(name);
    if (ni >= 0) {
        g_curGroup = ni;
        ViewDirty();
        RefreshCard();       // 切到空分组：悬停格可能已无对应条目，卡片收起
        g_scroll = 0;
        ClampGrpScroll();
        SaveConfig();
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

// ---------------------------------------------------------------- 启动
// 常规启动一律 CreateProcess：ShellExecuteW 会把 WinRT 启动栈
// （Windows.System.Launcher → Windows.Storage 等约 15MB）拉进进程且常驻不卸。
// ShellExecuteW 只留给 runas 提权和 CreateProcess 不认识的目标兜底。
static BOOL CreateRun(const WCHAR *exe, const WCHAR *args, const WCHAR *work)
{
    WCHAR cmd[MAX_PATH + 208];
    swprintf(cmd, MAX_PATH + 208, L"\"%s\"%s%s", exe, args[0] ? L" " : L"", args);
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
    swprintf(args, MAX_PATH + 16, L"/select,\"%s\"", path);
    CreateRun(L"explorer.exe", args, L"");
}

// ---------------------------------------------------------------- 配置：流式读写（低内存）
static void IniPath(void)
{
    GetModuleFileNameW(NULL, g_ini, MAX_PATH);
    WCHAR *p = wcsrchr(g_ini, L'\\');
    if (p) p[1] = 0;
    wcscat_s(g_ini, MAX_PATH, ININAME);
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
    BYTE line[1200];
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
    if (w < 0) w = 0;
    out[w] = 0;
    if (r->first) {
        r->first = FALSE;
        if (out[0] == 0xFEFF) {                      // 去掉 UTF-8 BOM
            int i = 0;
            while (out[i]) { out[i] = out[i + 1]; i++; }
        }
    }
    return TRUE;
}

static BYTE g_outBuf[4096];

static void WriteUtf8Line(HANDLE f, const WCHAR *text)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, (char *)g_outBuf,
                                (int)sizeof g_outBuf, NULL, NULL);
    if (n <= 0) return;
    DWORD w = 0;
    WriteFile(f, g_outBuf, (DWORD)(n - 1), &w, NULL);   // 不写结尾 NUL
}

static void FmtLine(HANDLE f, const WCHAR *fmt, ...)
{
    WCHAR line[1200];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(line, 1200, fmt, ap);
    va_end(ap);
    WriteUtf8Line(f, line);
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

    g_pinned = FALSE;
    wcsncpy_s(g_groups[0].name, 48, L"全部", _TRUNCATE);
    g_nGroups = 1;

    WCHAR line[1200], section[32] = L"";
    WCHAR rect[64] = L"";
    // 无配置文件（或打不开）时循环条件直接为假：保持默认分组，走下面的默认矩形。
    // 原来这里提前 return，跳过了 SetDefaultRect——首启会创建出 0x0 的不可见窗口
    while (r.f != INVALID_HANDLE_VALUE && RdLine(&r, line, 1200)) {
        if (line[0] == L'[') {
            int n = 0;
            for (int i = 1; line[i] && line[i] != L']' && n < 31; i++) section[n++] = line[i];
            section[n] = 0;
            continue;
        }
        if (!line[0] || line[0] == L'#' || line[0] == L';') continue;
        WCHAR *eq = wcschr(line, L'=');
        if (!eq) continue;
        *eq = 0;
        WCHAR *key = line, *val = eq + 1;

        if (!wcscmp(section, L"Main")) {
            if (!wcscmp(key, L"Pinned"))  g_pinned = _wtoi(val) ? TRUE : FALSE;
            else if (!wcscmp(key, L"Topmost")) { if (_wtoi(val)) g_pinned = TRUE; }   // 旧字段迁移：原置顶偏好并入置顶开关
            else if (!wcscmp(key, L"Silent"))  g_silent = _wtoi(val) ? TRUE : FALSE;
            else if (!wcscmp(key, L"Group"))   g_curGroup = _wtoi(val);
            else if (!wcscmp(key, L"Cell"))    g_cellBase = ClampCellBase(_wtoi(val));
            else if (!wcscmp(key, L"Rect"))    wcsncpy_s(rect, 64, val, _TRUNCATE);
        } else if (!wcscmp(section, L"Groups")) {
            if (key[0] == L'G' && _wtoi(key + 1) == g_nGroups) AddGroup(val);
        } else if (!wcscmp(section, L"Items")) {
            if (key[0] != L'I') continue;
            if (_wtoi(key + 1) != g_nItems) continue;
            WCHAR *f[6] = { 0, 0, 0, 0, 0, 0 };
            int nf = 0;
            WCHAR *p = val;
            while (nf < 6) {
                f[nf++] = p;
                WCHAR *b = wcschr(p, L'|');
                if (!b) break;
                *b = 0;
                p = b + 1;
            }
            if (nf < 2) continue;
            int grp = (nf >= 3 && f[2][0]) ? _wtoi(f[2]) : 0;
            if (grp < 0 || grp >= g_nGroups) grp = 0;
            if (grp == 0) grp = DefaultGroup();
            AddItemRaw(f[0], f[1], grp,
                       nf >= 4 ? f[3] : NULL, nf >= 6 ? f[5] : NULL);   // f[4] 是旧格式的 work 槽：跳过
        }
    }
    if (r.f != INVALID_HANDLE_VALUE) CloseHandle(r.f);
    if (g_nGroups < 2) AddGroup(L"常用");
    if (g_curGroup < 0 || g_curGroup >= g_nGroups) g_curGroup = 0;
    ViewDirty();

    int x = 0, y = 0, cx = 0, cy = 0;
    if (rect[0] && swscanf(rect, L"%d %d %d %d", &x, &y, &cx, &cy) == 4 && cx > 200 && cy > 160) {
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
    HANDLE f = CreateFileW(g_ini, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    static const BYTE bom[3] = { 0xEF, 0xBB, 0xBF };
    DWORD w = 0;
    WriteFile(f, bom, 3, &w, NULL);

    RECT r;
    GetWindowRect(g_hwnd, &r);
    FmtLine(f, L"# kk 桌面格子配置 (UTF-8)\r\n");
    FmtLine(f, L"[Main]\r\n");
    FmtLine(f, L"Rect=%d %d %d %d\r\n", r.left, r.top,
            r.right - r.left, r.bottom - r.top);
    FmtLine(f, L"Pinned=%d\r\n", g_pinned ? 1 : 0);
    FmtLine(f, L"Silent=%d\r\n", g_silent ? 1 : 0);
    FmtLine(f, L"Group=%d\r\n", g_curGroup);
    FmtLine(f, L"Cell=%d\r\n", g_cellBase);
    FmtLine(f, L"[Groups]\r\n");
    FmtLine(f, L"Count=%d\r\n", g_nGroups);
    for (int i = 0; i < g_nGroups; i++) {
        static WCHAR gName[96];
        FmtLine(f, L"G%d=%s\r\n", i, SafeField(gName, 96, g_groups[i].name));
    }
    FmtLine(f, L"[Items]\r\n");
    FmtLine(f, L"Count=%d\r\n", g_nItems);
    // 注意：每个字段必须用独立的缓冲区——函数参数求值顺序不确定。
    // 条目行保留 6 个位置槽（work 槽固定为空），旧配置按位置读取不受影响
    static WCHAR sName[96], sPath[MAX_PATH + 8], sArgs[MAX_PATH + 8], sTip[128];
    for (int i = 0; i < g_nItems; i++) {
        FmtLine(f, L"I%d=%s|%s|%d|%s||%s\r\n", i,
                SafeField(sName, 96, g_items[i].name),
                SafeField(sPath, MAX_PATH + 8, g_items[i].path), g_items[i].group,
                SafeField(sArgs, MAX_PATH + 8, g_items[i].args),
                SafeField(sTip, 128, g_items[i].tip));
    }
    CloseHandle(f);
    g_cfgDirty = FALSE;   // 磁盘已与内存一致（早退的失败路径不清：下个心跳重试）
}

// 应用格子缩放（Ctrl+滚轮）：重算列数、按新尺寸重载图标、钳制滚动、整窗重绘。
// 落盘防抖：连续缩放手势只写一次配置（由 TIMER_HIDE 心跳代劳）
static void ApplyCellBase(int v)
{
    v = ClampCellBase(v);
    if (v == g_cellBase) return;
    g_cellBase = v;
    UpdateCell();
    for (int i = 0; i < g_nItems; i++) DropIcon(i);   // 图标尺寸变了：丢掉，下次绘制按新尺寸重载
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
    wcsncpy_s(ci->name, 48, it->name, _TRUNCATE);
    wcsncpy_s(ci->path, MAX_PATH, it->path, _TRUNCATE);
    wcsncpy_s(ci->args, 192, it->args, _TRUNCATE);
    wcsncpy_s(ci->tip, 96, it->tip, _TRUNCATE);
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
    size_t cb = (wcslen(ci->path) + 1) * sizeof(WCHAR);
    HGLOBAL ht = GlobalAlloc(GMEM_MOVEABLE, cb);
    if (ht) {
        WCHAR *dst = (WCHAR *)GlobalLock(ht);
        if (dst) {
            wcscpy_s(dst, cb / sizeof(WCHAR), ci->path);
            GlobalUnlock(ht);
            if (!SetClipboardData(CF_UNICODETEXT, ht)) GlobalFree(ht);
        } else {
            GlobalFree(ht);
        }
    }
    CloseClipboard();
    return TRUE;
}

// 从剪贴板取条目：优先私有格式（全字段），退化到纯文本（只当路径，名字由路径推导）
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
                wcsncpy_s(ci->path, MAX_PATH, s, _TRUNCATE);
                NameFromPath(ci->path, ci->name, 48);
                ok = TRUE;
            }
            if (s) GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return ok;
}

// 粘进指定格：替换全部文本字段并重取图标；组号不变（粘到哪格就入哪组）
static void PasteItemAt(int ii, const ClipItem *ci)
{
    wcsncpy_s(g_items[ii].name, 48, ci->name, _TRUNCATE);
    wcsncpy_s(g_items[ii].path, MAX_PATH, ci->path, _TRUNCATE);
    wcsncpy_s(g_items[ii].args, 192, ci->args, _TRUNCATE);
    wcsncpy_s(g_items[ii].tip, 96, ci->tip, _TRUNCATE);
    DropIcon(ii);
    EnsureIcon(ii);
    ViewDirty();                     // 名字可能变了：过滤视图要重建
    SyncAndSave();
}

// 追加为新条目到当前组（“全部”组时落到默认组）
static void PasteItemNew(const ClipItem *ci)
{
    int g = (g_curGroup == 0) ? DefaultGroup() : g_curGroup;
    if (AddItemRaw(ci->name, ci->path, g, ci->args, ci->tip) >= 0) {
        ClampScroll();
        SyncAndSave();
    }
}

// ---------------------------------------------------------------- 显示/隐藏
static void ForceForeground(HWND h)
{
    DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
    DWORD me = GetCurrentThreadId();
    if (fg != me && fg) AttachThreadInput(fg, me, TRUE);
    SetForegroundWindow(h);
    if (fg != me && fg) AttachThreadInput(fg, me, FALSE);
}

static void ShowLauncher(void)
{
    POINT pt;
    GetCursorPos(&pt);
    RECT wr;
    GetWindowRect(g_hwnd, &wr);
    if (!PtInRect(&wr, pt)) MoveToPoint(pt);
    else ClampToWorkArea();

    if (!IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd, SW_SHOW);
    ForceForeground(g_hwnd);
    g_outSince = 0;
    g_wakeAt = GetTickCount();
    g_alpha = 0;
    SetLayeredWindowAttributes(g_hwnd, 0, (BYTE)g_alpha, LWA_ALPHA);
    g_fadeDir = 1;
    SetTimer(g_hwnd, TIMER_FADE, 12, NULL);
    SetTimer(g_hwnd, TIMER_HIDE, 120, NULL);   // 隐藏期间定时器是停的，这里才重启
}

static void BeginHide(BOOL force)
{
    if (!force && g_pinned) return;
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
            g_flashAt = 0;                    // 心跳停了：反馈不可能再过期重绘，直接收掉
            if (g_cfgDirty) { g_cfgDirty = FALSE; SaveConfig(); }   // 心跳将停：待落盘的立即落
            TrimWS();
        } else {
            SetLayeredWindowAttributes(g_hwnd, 0, (BYTE)g_alpha, LWA_ALPHA);
        }
    }
}

static void ClampToWorkArea(void)
{
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    RECT wr;
    GetWindowRect(g_hwnd, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    if (wr.left < wa.left) wr.left = wa.left;
    if (wr.top < wa.top) wr.top = wa.top;
    if (w > wa.right - wa.left) w = wa.right - wa.left;
    if (h > wa.bottom - wa.top) h = wa.bottom - wa.top;
    if (wr.left + w > wa.right) wr.left = wa.right - w;
    if (wr.top + h > wa.bottom) wr.top = wa.bottom - h;
    SetWindowPos(g_hwnd, NULL, wr.left, wr.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
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
    int x = pt.x - w / 2, y = pt.y - h / 2;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
    if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
    SetWindowPos(g_hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------- 资源
static void ScaleMetrics(void)
{
    g_titleH = S_(34);
    g_sideW  = S_(84);
    g_gap    = S_(3);
    g_grpH   = S_(34);
}

static void MakeFonts(void)
{
    LOGFONTW lf;
    if (g_font)  { DeleteObject(g_font);  g_font  = NULL; }
    if (g_fontT) { DeleteObject(g_fontT); g_fontT = NULL; }
    ZeroMemory(&lf, sizeof lf);
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Segoe UI");
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

// 标题栏图形与格子边框的 GDI 对象：颜色固定、线宽随 DPI，DPI 变化时整套重建。
// 原来每画一次就 CreatePen/CreateSolidBrush 再逐个销毁，纯 churn
static void MakeGdiObjects(void)
{
    if (g_penDim)    { DeleteObject(g_penDim);    g_penDim = NULL; }
    if (g_penAccent) { DeleteObject(g_penAccent); g_penAccent = NULL; }
    if (g_brPinOn)   { DeleteObject(g_brPinOn);   g_brPinOn = NULL; }
    if (g_brPinOff)  { DeleteObject(g_brPinOff);  g_brPinOff = NULL; }
    if (g_penLine)   { DeleteObject(g_penLine);   g_penLine = NULL; }
    g_penDim    = CreatePen(PS_SOLID, S_(1), C_DIM);
    g_penAccent = CreatePen(PS_SOLID, 1, C_ACCENT);
    g_penLine   = CreatePen(PS_SOLID, 1, C_LINE);
    g_brPinOn   = CreateSolidBrush(C_ACCENT);
    g_brPinOff  = CreateSolidBrush(C_BG);
}

static void MakeFontsAndBrushes(void)
{
    MakeFonts();
    MakeBrushes();
    MakeGdiObjects();
}

// ---------------------------------------------------------------- 悬停文字
// 设置悬停文字（顺带置激活态），返回文字/激活态是否变化。
// 原来 6 处各写一遍 wcsncpy_s，收拢到这里；比较放在写入前完成，
// 调用方不必再拷一份旧文字来对比（80+ 字符的超长文字按显示得出的
// 前 79 字符比较，最差情况是多重绘一次标题栏，像素不变）
static BOOL SetHover(const WCHAR *s, BOOL active)
{
    BOOL chg = active != g_hoverActive || wcscmp(s, g_hoverText) != 0;
    wcsncpy_s(g_hoverText, 80, s, _TRUNCATE);
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

// 返回悬停文字/激活态是否变化（没变就不重绘标题栏）。
// 悬停目标没变时直接返回：鼠标在同一个格子上滑动是最高频的事件，
// 此时连文字比较都不做
static BOOL HoverFromPoint(POINT p)
{
    int nc = CellAt(p);
    int ng = (nc >= 0) ? -1 : GroupAt(p);
    int nb = (nc >= 0 || ng >= 0) ? 0 : BtnAt(p);
    if (nc == g_hoverCell && ng == g_hoverGroup && nb == g_hoverBtn)
        return FALSE;
    g_hoverCell = nc; g_hoverGroup = ng; g_hoverBtn = nb;
    return UpdateHoverText();
}

// ---------------------------------------------------------------- 悬停小卡片
// 悬停格子/分组时在目标周围自动挑合适的位置浮出卡片，显示程序名/组名（类工具提示）。
// 位置锚定悬停目标而非光标：光标每帧都在动，锚光标意味着每帧重算+重绘；
// 锚目标则只在悬停目标变化时算一次（GetDC + DT_CALCRECT 量宽），
// 绘制阶段只读缓存的状态，零分配、零新 GDI 对象（复用面板刷/线刷/字体）
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
    wcsncpy_s(g_cardText, 80, txt, _TRUNCATE);
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
    if (og != g_hoverGroup) {
        RECT sr = { 0, 0, g_sideW, g_ch };
        InvalidateRect(g_hwnd, &sr, FALSE);
    }
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

// 字体沿用 PaintWindow 已选好的 g_font，这里只设文字色
static void DrawCard(HDC dc)
{
    RECT r = g_cardRect;
    FillRect(dc, &r, g_brPanel);
    FrameRect(dc, &r, g_brLine);
    SetTextColor(dc, C_TEXT);
    DrawTextW(dc, g_cardText, -1, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

// ---------------------------------------------------------------- 绘制
static void InvalidateCell(int vi)
{
    if (vi < 0) return;
    RECT r = CellRect(vi);
    InvalidateRect(g_hwnd, &r, FALSE);
}

// 重绘 [a, b] 闭区间格子（顺序无关）：多文件拖放的落点区间重绘用
static void InvalidateCells(int a, int b)
{
    if (a > b) { int t = a; a = b; b = t; }
    for (int i = a; i <= b; i++) InvalidateCell(i);
}

// 拖动阈值判断（格子排序与分组重排共用）：位移超过系统拖动矩形即算拖动
static BOOL PastDragThreshold(POINT cur, POINT org)
{
    int dx = cur.x - org.x, dy = cur.y - org.y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx > GetSystemMetrics(SM_CXDRAG) || dy > GetSystemMetrics(SM_CYDRAG);
}

// 重绘单个分组行（侧栏重排的落点高亮擦除用）
static void InvalidateGroup(int gi)
{
    if (gi < 0) return;
    RECT r = GroupRect(gi);
    InvalidateRect(g_hwnd, &r, FALSE);
}

static void InvalidateTitle(void)
{
    RECT r = { 0, 0, g_cw, g_titleH };
    InvalidateRect(g_hwnd, &r, FALSE);
}

#define FLASH_MS 1200

// 标题栏闪现一条消息 1.2s（时间戳驱动，不占新定时器；过期重绘由 TIMER_HIDE 顺带做）
static void FlashMsg(const WCHAR *text)
{
    wcsncpy_s(g_flashText, 80, text, _TRUNCATE);
    g_flashAt = GetTickCount();
    InvalidateTitle();
}

static void FlashLaunch(BOOL ok, const WCHAR *name)
{
    WCHAR t[80];
    swprintf(t, 80, ok ? L"已启动: %s" : L"启动失败: %s", name);
    FlashMsg(t);
}

static BOOL FlashActive(void)
{
    return g_flashAt && GetTickCount() - g_flashAt < FLASH_MS;
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

// 静默启动开关：只影响下次启动（当次不隐藏，正在用的窗口不动）
static void ToggleSilent(void)
{
    g_silent = !g_silent;
    SaveConfig();
    UpdateTrayTip();      // 托盘提示里的“静默”标记跟着变
}

static void DrawPinGlyph(HDC dc, RECT r, BOOL on)
{
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2 - 2;
    int rad = S_(4);
    HPEN op = (HPEN)SelectObject(dc, on ? g_penAccent : g_penDim);
    HBRUSH ob = (HBRUSH)SelectObject(dc, on ? g_brPinOn : g_brPinOff);
    Ellipse(dc, cx - rad, cy - rad, cx + rad, cy + rad);
    MoveToEx(dc, cx, cy + rad, NULL);
    LineTo(dc, cx, r.bottom - 3);
    SelectObject(dc, op); SelectObject(dc, ob);
}

static void DrawCloseGlyph(HDC dc, RECT r)
{
    HPEN op = (HPEN)SelectObject(dc, g_penDim);
    int m = S_(8);
    MoveToEx(dc, r.left + m, r.top + m, NULL); LineTo(dc, r.right - m, r.bottom - m);
    MoveToEx(dc, r.right - m, r.top + m, NULL); LineTo(dc, r.left + m, r.bottom - m);
    SelectObject(dc, op);
}

static void PaintSidebar(HDC dc, const RECT *clip)
{
    RECT sr = { 0, 0, g_sideW, g_ch };
    FillRect(dc, &sr, g_brSide);
    RECT ln = { g_sideW - 1, 0, g_sideW, g_ch };
    FillRect(dc, &ln, g_brLine);

    RECT hit;
    for (int i = 0; i < g_nGroups; i++) {
        RECT r = GroupRect(i);
        if (r.bottom < g_titleH || r.top > g_ch) continue;
        if (clip && !IntersectRect(&hit, &r, clip)) continue;   // 行不在脏区：整行跳过
        BOOL sel = (i == g_curGroup), hov = (i == g_hoverGroup);
        // 只用“选中高亮”，不再叠加悬停灰底，避免两种高亮重复
        if (sel) FillRect(dc, &r, g_brCell);
        SetTextColor(dc, sel ? C_ACCEL : (hov ? C_TEXT : C_DIM));
        RECT tr = { r.left + S_(4), r.top, r.right - S_(4), r.bottom };
        DrawTextW(dc, g_groups[i].name, -1, &tr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        // 选中标记放在下方（原来在左侧）
        if (sel) {
            RECT bar = { r.left, r.bottom - S_(2), r.right, r.bottom };
            FillRect(dc, &bar, g_brAccel);
        }
        // 分组重排的落点高亮（源行即当前选中行，已有底色，不重复画）
        if (g_dragGroup && i == g_dropGroup && i != g_pressGroup) {
            RECT r2 = r;
            InflateRect(&r2, -S_(2), -S_(2));
            FrameRect(dc, &r2, g_brAccent);
        }
    }

    // 分组超出可视高度时，右侧画一个细滚动条，让“可以滚”看得见
    int content = g_nGroups * g_grpH + 12;
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
    RECT ttl = { g_sideW + S_(8), 0, g_cw - S_(90), g_titleH };
    if (FlashActive()) {
        // 启动反馈优先：刚点过启动，先让用户看到结果（悬停/搜索都让位）
        SetTextColor(dc, C_TEXT);
        DrawTextW(dc, g_flashText, -1, &ttl,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    } else if (g_filter[0]) {
        // 搜索中：标题栏让位给过滤词（左对齐 + 命中数），悬停详情看小卡片
        WCHAR txt[64];
        swprintf(txt, 64, L"搜索: %s (%d)", g_filter, ViewCount());
        SetTextColor(dc, C_TEXT);
        DrawTextW(dc, txt, -1, &ttl,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    } else {
        SetTextColor(dc, g_hoverActive ? C_TEXT : C_DIM);
        DrawTextW(dc, g_hoverText, -1, &ttl,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, old);
    // 按钮悬停底色：悬停文字已给语义，底色给"这是个按钮"的按键感（无反馈像死区）
    if (g_hoverBtn == 1) { RECT r = BtnRect(1); FillRect(dc, &r, g_brCellHi); }
    if (g_hoverBtn == 2) { RECT r = BtnRect(2); FillRect(dc, &r, g_brCellHi); }
    DrawPinGlyph(dc, BtnRect(1), g_pinned);
    DrawCloseGlyph(dc, BtnRect(2));
    // 标题栏底部 1px 分隔线：与侧栏右边线连贯，把头部与内容区从视觉上分开
    RECT ln = { 0, g_titleH - 1, g_cw, g_titleH };
    FillRect(dc, &ln, g_brLine);
}

static void PaintGrid(HDC dc, const RECT *clip)
{
    int step = g_cell + g_gap;
    int isz = IconSize();
    int n = ViewCount();
    int limit = g_slots;
    if (n > limit) limit = n;
    // 脏区剔除：悬停/拖动时只脏 1~2 格，与更新区不相交的整格连 GDI 调用都不发
    // 按行递增算坐标：省掉 CellRect 里每格两次除法（% 和 /）
    RECT hit;
    // 边框 pen / 底色 brush 循环外只选一次；一个 Rectangle 同时完成填充+1px
    // 边框（原来 FillRect+FrameRect 两次调用），悬停格才换一次底色画刷
    HPEN   op = (HPEN)SelectObject(dc, g_penLine);
    HBRUSH ob = (HBRUSH)SelectObject(dc, g_brCell);
    SetTextColor(dc, C_TEXT);            // 所有格子的标签同色：循环外设一次
    for (int row = 0; row * g_cols < limit; row++) {
        int y = g_titleH + row * step - g_scroll;
        if (y >= g_ch) break;                       // 整行在可视区下方
        if (y + g_cell <= g_titleH) continue;       // 整行已滚出上方
        for (int col = 0; col < g_cols; col++) {
            int vi = row * g_cols + col;
            RECT r = { g_sideW + col * step, y,
                       g_sideW + col * step + g_cell, y + g_cell };
            if (clip && !IntersectRect(&hit, &r, clip)) continue;
            int ii = ItemAtView(vi);
            BOOL hov = (vi == g_hoverCell) && ii >= 0;
            if (hov) SelectObject(dc, g_brCellHi);
            Rectangle(dc, r.left, r.top, r.right, r.bottom);
            if (hov) SelectObject(dc, g_brCell);
            if (hov) {
                // 悬停格强调色内边框：底色变浅之外再给一圈 1px 焦点环，扫视时一眼锁定
                RECT hr = r;
                InflateRect(&hr, -1, -1);
                FrameRect(dc, &hr, g_brAccent);
            }
            if (g_dragging && (vi == g_pressCell ||
                               (g_dropIn && vi == g_dropCell && vi != g_pressCell))) {
                RECT r2 = r;                        // 拖动源/落点高亮
                InflateRect(&r2, -S_(2), -S_(2));
                FrameRect(dc, &r2, g_brAccent);
            }
            if (ii < 0 || IsBlank(ii)) continue;    // 空格：只画底框（悬停高亮已画），无图标/名字
            EnsureIcon(ii);
            if (g_items[ii].icon) {
                int ix = (r.left + r.right - isz) / 2;
                int iy = r.top + (g_cell - S_(20) - isz) / 2;   // 标签上方垂直居中
                DrawIconEx(dc, ix, iy, g_items[ii].icon, isz, isz, 0, NULL, DI_NORMAL);
            }
            RECT lr = { r.left + S_(2), r.bottom - S_(17), r.right - S_(2), r.bottom - S_(3) };
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

// ---------------------------------------------------------------- 对话框（格子属性 / 重命名）
typedef struct {
    const WCHAR *label;
    WCHAR *buf;
    int    cch;
    int    mode;      // 0 文本 / 1 带“浏览”按钮
} DlgField;

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
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&of)) SetWindowTextW(edit, file);
}

static LRESULT CALLBACK DlgProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
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
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        if (id == DLG_OK || id == DLG_CANCEL) {
            if (id == DLG_OK) {
                for (int i = 0; i < g_dlg.n; i++)
                    GetWindowTextW(g_dlg.ed[i], g_dlg.f[i].buf, g_dlg.f[i].cch);
                g_dlg.ok = TRUE;
            }
            DestroyWindow(h);
            return 0;
        }
        // 只在按钮真正被点击时打开浏览框（否则输入框的 EN_* 通知会误触发）
        if (code == BN_CLICKED && id >= DLG_BROWSE && id < DLG_BROWSE + 6) {
            int i = id - DLG_BROWSE;
            if (g_dlg.f[i].mode == 1) BrowseForFile(g_dlg.ed[i]);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)w, &rc, g_brPanel);
        return 1;
    }
    case WM_PAINT: {
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
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        SetBkMode((HDC)w, TRANSPARENT);
        SetTextColor((HDC)w, C_DIM);
        return (LRESULT)g_brPanel;
    }
    case WM_CTLCOLOREDIT: {
        SetTextColor((HDC)w, C_TEXT);
        SetBkColor((HDC)w, C_CELL);
        return (LRESULT)g_brCell;
    }
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG: {
        SetTextColor((HDC)w, C_TEXT);
        SetBkColor((HDC)w, C_PANEL);
        return (LRESULT)g_brPanel;
    }
    case WM_NCHITTEST: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ScreenToClient(h, &p);
        if (p.y < g_dlg.titleH) return HTCAPTION;
        break;
    }
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

static BOOL RunDialog(HWND owner, const WCHAR *title, void *fields, int n)
{
    DlgField *f = (DlgField *)fields;
    ZeroMemory(&g_dlg, sizeof g_dlg);
    for (int i = 0; i < n && i < 6; i++) g_dlg.f[i] = f[i];
    g_dlg.n = n;
    wcsncpy_s(g_dlg.title, 64, title, _TRUNCATE);
    g_dlg.dlgW   = S_(540);
    g_dlg.titleH = S_(34);
    g_dlg.rowH   = S_(32);
    g_dlg.labW   = S_(100);
    g_dlg.browseW = S_(86);

    int dlgH = g_dlg.titleH + S_(10) + n * g_dlg.rowH + S_(12) + S_(27) + S_(14);

    // 对话框窗口类已在 wWinMain 注册一次，这里不再重复注册

    RECT or_;
    GetWindowRect(owner, &or_);
    int x = or_.left + ((or_.right - or_.left) - g_dlg.dlgW) / 2;
    int y = or_.top + ((or_.bottom - or_.top) - dlgH) / 2;

    g_dlg.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_CONTROLPARENT,
                                 DLG_CLASS, title,
                                 WS_POPUP | WS_CLIPCHILDREN, x, y, g_dlg.dlgW, dlgH,
                                 owner, NULL, g_inst, NULL);
    if (!g_dlg.hwnd) return FALSE;

    g_dlgOpen = TRUE;
    ShowWindow(g_dlg.hwnd, SW_SHOW);
    ForceForeground(g_dlg.hwnd);
    EnableWindow(owner, FALSE);          // 等价于 DisableWindow（SDK 未声明后者）

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(g_dlg.hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    g_dlgOpen = FALSE;
    BOOL ok = g_dlg.ok;
    g_dlg.hwnd = NULL;
    return ok;
}

// ---------------------------------------------------------------- 开机自启
// HKCU 的 Run 键是唯一事实源：启动时读一次供菜单勾选，切换时直接写/删，
// 不镜像进 kk.ini——避免两处状态互相不一致。HKCU 无需管理员权限
#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

// 读出自启项登记的路径（无该项返回 FALSE）。容量按写入上限（引号+全路径+NUL）给足
static BOOL QueryAutostart(WCHAR *pathOut, int cch)
{
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
        swprintf(cmd, MAX_PATH + 4, L"\"%s\"", path);   // 路径可能含空格：加引号
        r = RegSetValueExW(k, APPNAME, 0, REG_SZ, (const BYTE *)cmd,
                           (DWORD)(wcslen(cmd) + 1) * sizeof(WCHAR));
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
    if (*q == L'"') { q++; WCHAR *e = wcschr(q, L'"'); if (e) *e = 0; }
    WCHAR self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    if (wcscmp(q, self) != 0) SetAutostart(TRUE);
}

// ---------------------------------------------------------------- 托盘
// 运行时画一个 2x2 格子图标：不必引入 .ico 资源，保持单文件

// 组装托盘提示：应用名 + 当前激活的开关（全关时退化为纯操作提示）。
// 开关状态一目了然，不用每次都右键开菜单看勾选
static void BuildTrayTip(WCHAR *out, size_t cch)
{
    WCHAR flags[40] = L"";
    if (g_pinned)    wcscat_s(flags, 40, L"置顶·");
    if (g_autostart) wcscat_s(flags, 40, L"自启·");
    if (g_silent)    wcscat_s(flags, 40, L"静默·");
    if (flags[0]) {
        flags[wcslen(flags) - 1] = 0;                     // 去掉末尾分隔符
        swprintf(out, cch, L"桌面格子 — %s — 单击显示，右键菜单", flags);
    } else {
        swprintf(out, cch, L"桌面格子 — 单击显示，右键菜单");
    }
}

// 开关变化后刷新托盘提示（无托盘图标时是空操作；只改 tip，不动图标/回调）
static void UpdateTrayTip(void)
{
    if (!g_trayIcon) return;
    BuildTrayTip(g_nid.szTip, 64);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static HICON MakeTrayIcon(int sz)
{
    HDC scr = GetDC(NULL);
    HDC dc = CreateCompatibleDC(scr);
    if (!dc) { ReleaseDC(NULL, scr); return NULL; }
    HBITMAP col = CreateBitmap(sz, sz, 1, 32, NULL);   // 颜色位图
    HBITMAP msk = CreateBitmap(sz, sz, 1, 1, NULL);    // 掩码：1=透明
    if (!col || !msk) {
        if (col) DeleteObject(col);
        if (msk) DeleteObject(msk);
        DeleteDC(dc); ReleaseDC(NULL, scr);
        return NULL;
    }
    HGDIOBJ oc = SelectObject(dc, col);
    HGDIOBJ om = SelectObject(dc, msk);

    RECT all = { 0, 0, sz, sz };
    HBRUSH bg = CreateSolidBrush(C_BG);
    FillRect(dc, &all, bg);
    DeleteObject(bg);

    // 2x2 浅色格子
    int pad = sz / 6, gap = sz / 10;
    int cw = (sz - pad * 2 - gap) / 2;
    int ch = (sz - pad * 2 - gap) / 2;
    HBRUSH fg = CreateSolidBrush(RGB(0xEC, 0xED, 0xEF));
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            RECT r = { pad + j * (cw + gap), pad + i * (ch + gap),
                       pad + j * (cw + gap) + cw, pad + i * (ch + gap) + ch };
            FillRect(dc, &r, fg);
        }
    DeleteObject(fg);

    // 掩码：圆角外透明 -> 用圆角矩形区域填充黑色（不透明）
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &all, white);
    HRGN rgn = CreateRoundRectRgn(0, 0, sz + 1, sz + 1, sz / 3, sz / 3);
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    FillRgn(dc, rgn, black);
    DeleteObject(white);
    DeleteObject(black);
    if (rgn) DeleteObject(rgn);

    HICON ico = NULL;
    ICONINFO ii;
    ZeroMemory(&ii, sizeof ii);
    ii.fIcon = TRUE;
    ii.hbmColor = col;
    ii.hbmMask = msk;
    ico = CreateIconIndirect(&ii);

    SelectObject(dc, oc);
    SelectObject(dc, om);
    DeleteDC(dc);
    ReleaseDC(NULL, scr);
    DeleteObject(col);
    DeleteObject(msk);
    return ico;
}

static void TrayAdd(void)
{
    if (!g_trayIcon) g_trayIcon = MakeTrayIcon(GetSystemMetrics(SM_CYSMICON));
    if (!g_trayIcon) return;

    ZeroMemory(&g_nid, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_hwnd;
    g_nid.uID = TRAY_ID;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_trayIcon;
    BuildTrayTip(g_nid.szTip, 64);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
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

static void TrayShowMenu(void)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_TRAY_SHOW, L"显示主面板");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_pinned ? MF_CHECKED : 0), IDM_TRAY_PIN, L"置顶窗口");
    AppendMenuW(m, MF_STRING | (g_autostart ? MF_CHECKED : 0), IDM_TRAY_AUTO,
                L"开机自启动");
    AppendMenuW(m, MF_STRING | (g_silent ? MF_CHECKED : 0), IDM_TRAY_SILENT,
                L"静默启动（进托盘）");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出程序");
    int cmd = PopupAt(m);

    if (cmd == IDM_TRAY_SHOW) ShowLauncher();
    else if (cmd == IDM_TRAY_PIN) {
        TogglePin();
    } else if (cmd == IDM_TRAY_AUTO) {
        ToggleAutostart();
    } else if (cmd == IDM_TRAY_SILENT) {
        ToggleSilent();
    } else if (cmd == IDM_EXIT) {
        ExitApp();
    }
}

// ---------------------------------------------------------------- 菜单
// 四个菜单（格子/分组/标题/托盘）弹出前的动作完全一样，收拢到这一处：
// 光标处弹出、置前台、拿命令、销毁。用 ForceForeground 而非裸
// SetForegroundWindow——输入队列没挂反时菜单会秒关，看起来像没反应
static int PopupAt(HMENU m)
{
    POINT p;
    GetCursorPos(&p);
    ForceForeground(g_hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
    return cmd;
}

// 格子右键菜单：有程序的格子给启动/属性等；空格也给槽位菜单（插入/删除）。
// 菜单按视图位 vi 打开——插入/删除是槽位级操作，作用于可见格位
static void ShowCellMenu(int vi)
{
    int ii = ItemAtView(vi);
    BOOL hasItem = ii >= 0 && !IsBlank(ii);
    HMENU m = CreatePopupMenu();
    if (hasItem) {
        AppendMenuW(m, MF_STRING, IDM_LAUNCH, L"打开");
        AppendMenuW(m, MF_STRING, IDM_ADMIN, L"以管理员身份运行");
        AppendMenuW(m, MF_STRING, IDM_LOC, L"打开文件位置");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(m, MF_STRING, IDM_INS_SLOT, L"在此插入格子");
    AppendMenuW(m, MF_STRING, IDM_DEL_SLOT, L"删除格子");
    if (hasItem) AppendMenuW(m, MF_STRING, IDM_CLR_SLOT, L"清空格子");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    if (hasItem) AppendMenuW(m, MF_STRING, IDM_PROPS, L"格子属性(&R)...");
    int cmd = PopupAt(m);
    if (!cmd) return;
    switch (cmd) {
    case IDM_LAUNCH: Launch(ii, FALSE); break;
    case IDM_ADMIN:  Launch(ii, TRUE); break;
    case IDM_LOC:    OpenLocation(g_items[ii].path); break;
    case IDM_INS_SLOT: InsertSlotAt(vi); break;
    case IDM_DEL_SLOT: DeleteSlotAt(vi); break;
    case IDM_CLR_SLOT: ClearSlotAt(vi); break;
    case IDM_PROPS: {
        WCHAR oldPath[MAX_PATH];
        wcsncpy_s(oldPath, MAX_PATH, g_items[ii].path, _TRUNCATE);
        DlgField f[4] = {
            { L"名称(N):",     g_items[ii].name, 48,       0 },
            { L"文件名(F):",   g_items[ii].path, MAX_PATH, 1 },
            { L"参数(P):",     g_items[ii].args, 192,      0 },
            { L"工具提示(T):", g_items[ii].tip,  96,       0 },
        };
        if (RunDialog(g_hwnd, L"格子属性", f, 4)) {
            if (wcscmp(oldPath, g_items[ii].path)) {   // 目标变了，重取图标
                DropIcon(ii);
                EnsureIcon(ii);
            }
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
    if (gi > 0) {
        AppendMenuW(m, MF_STRING, IDM_GRP_RENAME, L"重命名分组");
        AppendMenuW(m, MF_STRING, IDM_GRP_DEL, L"删除该分组");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(m, MF_STRING, IDM_GRP_NEW, L"新建分组");
    int cmd = PopupAt(m);
    if (cmd == IDM_GRP_RENAME) {
        DlgField f[1] = { { L"名称(N):", g_groups[gi].name, 48, 0 } };
        if (RunDialog(g_hwnd, L"重命名分组", f, 1)) {
            if (!g_groups[gi].name[0]) wcsncpy_s(g_groups[gi].name, 48, L"未命名", _TRUNCATE);
            SyncAndSave();
        }
    } else if (cmd == IDM_GRP_DEL) {
        DeleteGroup(gi);
        SaveConfig();
        InvalidateRect(g_hwnd, NULL, FALSE);
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
static DWORD s_lastClick = 0;
static POINT  s_lastPt = { 0, 0 };

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
        if (!wcscmp(cls, L"SysListView32")) return w;
        if (!wcscmp(cls, L"SHELLDLL_DefView"))
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
    if (wcscmp(cls, L"Progman") && wcscmp(cls, L"WorkerW")) return FALSE;

    // 桌面图标和桌面空白的根窗口都是 Progman/WorkerW，过不了上面的检查只是说明
    // “在桌面上”。双击桌面图标是要启动程序，不能顺带唤醒面板——再拿落点在桌面
    // ListView 上做一次图标命中测试，命中图标就不是空白。
    HWND lv = DesktopListViewFrom(h);
    if (lv) return !PointHitsDesktopIcon(lv, pt);
    return TRUE;    // 拿不到 ListView（深度定制桌面）：维持旧行为
}

static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION) {
        if (wParam == WM_LBUTTONDOWN) {
            MSLLHOOKSTRUCT *p = (MSLLHOOKSTRUCT *)lParam;
            DWORD now = GetTickCount();
            int dx = p->pt.x - s_lastPt.x, dy = p->pt.y - s_lastPt.y;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            BOOL dbl = s_lastClick && (now - s_lastClick) <= (DWORD)GetDoubleClickTime()
                       && dx <= GetSystemMetrics(SM_CXDLGFRAME)
                       && dy <= GetSystemMetrics(SM_CYDLGFRAME);
            // 钩子回调跑在全局输入路径上：这里只做时序判断。
            // WindowFromPoint 是跨线程窗口调用，被查窗口卡死会拖住整个系统的鼠标输入，
            // 所以“点的是不是桌面”放到 UI 线程再确认（WM_APP_WAKE_CAND）。
            if (dbl && !IsWindowVisible(g_hwnd) && !g_dlgOpen)
                PostMessageW(g_hwnd, WM_APP_WAKE_CAND, 0,
                             MAKELPARAM((WORD)p->pt.x, (WORD)p->pt.y));
            s_lastClick = now;
            s_lastPt = p->pt;
        } else if (wParam == WM_MOUSEWHEEL) {
            // 接管条件收到最窄，缺一不可：
            //   面板可见（否则没有转发的意义）、面板置顶（这一像素确实属于面板，
            //   没有被别的窗口盖住）、自家对话框没开、面板当前无焦点。
            // 四条全中才把滚轮转给面板并吞掉 —— 全局钩子只要有丝毫越界，
            // 用户在其他窗口的正常滚动就会被偷走，这是绝不能碰的红线。
            // 不置顶时光标底下是谁的窗口就归谁滚，绝不插手。
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
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, NULL, 0);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = NULL; }
    return 0;
}

// ---------------------------------------------------------------- 窗口过程
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    // 资源管理器（explorer.exe）重启后会广播 TaskbarCreated，此时托盘图标已丢失，需要重建
    if (g_msgTaskbarCreated && (UINT)m == g_msgTaskbarCreated) {
        TrayAdd();
        return 0;
    }
    switch (m) {
    case WM_CREATE: {
        g_hwnd = h;
        MakeFontsAndBrushes();
        SetWindowPos(h, g_pinned ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
        DragAcceptFiles(h, TRUE);
        g_hookH = CreateThread(NULL, 0, HookThread, NULL, 0, &g_hookTid);
        RegisterHotKey(h, HOTKEY_ID, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'K');
        SetTimer(h, TIMER_HIDE, 120, NULL);
        // 托盘图标
        g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        TrayAdd();
        return 0;
    }

    case WM_SIZE: {
        g_cw = LOWORD(l);
        g_ch = HIWORD(l);
        UpdateCell();       // 横向铺满的正方格宽随窗口变化
        // 重建离屏缓冲（失败则退回直接绘制）
        HDC scr = GetDC(h);
        HDC dc = CreateCompatibleDC(scr);
        HBITMAP bmp = CreateCompatibleBitmap(scr, g_cw, g_ch);
        ReleaseDC(h, scr);
        if (dc && bmp) {
            SelectObject(dc, bmp);
            if (g_memDC) { DeleteDC(g_memDC); DeleteObject(g_memBmp); }
            g_memDC = dc;
            g_memBmp = bmp;
            g_memFresh = TRUE;
        } else {
            if (dc) DeleteDC(dc);
            if (bmp) DeleteObject(bmp);
        }
        int rad = S_(10);
        HRGN rgn = CreateRoundRectRgn(0, 0, g_cw + 1, g_ch + 1, rad, rad);
        if (rgn) SetWindowRgn(h, rgn, TRUE);
        ClampScroll();
        ClampGrpScroll();
        RefreshCard();      // 尺寸变了：卡片按新布局重新锚定/钳制
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)l;
        mm->ptMinTrackSize.x = S_(260);
        mm->ptMinTrackSize.y = S_(200);
        return 0;
    }

    // WS_POPUP 无边框：手动给边缘 6px 命中区，窗口才能拉伸
    case WM_NCHITTEST: {
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
        break;
    }

    case WM_DPICHANGED: {
        g_dpi = LOWORD(w);
        ScaleMetrics();
        MakeFontsAndBrushes();
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
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (g_memDC) {
            // 只重画更新区域：整窗重画会拖垮消息队列（悬停文字/右键跟着卡）。
            // 其余缓冲内容保持上一帧，与屏幕一致。
            RECT rc;
            if (g_memFresh) { rc.left = 0; rc.top = 0; rc.right = g_cw; rc.bottom = g_ch; }
            else            rc = ps.rcPaint;
            // 设裁剪走 IntersectClipRect：memDC 每次画完都 SelectClipRgn(NULL)
            // 复位成整幅位图（WM_SIZE 重建时也是全新 DC），起始裁剪即全图，
            // 四次矩形求交等价于替换裁剪区，但省掉每次绘制一次 GDI 区域对象
            // 的创建+销毁——WM_PAINT 是悬停/拖动/滚动每条热路径的终点
            IntersectClipRect(g_memDC, rc.left, rc.top, rc.right, rc.bottom);
            PaintWindow(g_memDC, &rc);     // 裁剪区即 rc：直接传，省一次 GetClipBox
            SelectClipRgn(g_memDC, NULL);
            BitBlt(dc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                   g_memDC, rc.left, rc.top, SRCCOPY);
            g_memFresh = FALSE;
        } else {
            PaintWindow(dc, NULL);       // 缓冲不可用：直绘兜底，无裁剪全画
        }
        EndPaint(h, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE: {
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
                int nc = CellAt(p);
                if (nc < 0) nc = SlotAt(p);     // 空格也可作落点（抬起排到末尾）
                BOOL nin = p.x >= g_sideW && p.y >= g_titleH;
                if (nc != g_dropCell || nin != g_dropIn) {
                    InvalidateCell(g_dropCell);
                    InvalidateCell(nc);
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
                if (ng == 0) ng = 1;             // 组 0（全部）不参与：靠顶即落到第 1 位
                if (ng != g_dropGroup) {
                    InvalidateGroup(g_dropGroup);
                    InvalidateGroup(ng);
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
        return 0;
    }

    case WM_MOUSELEAVE:
        g_tracking = FALSE;
        {
            int oc = g_hoverCell;
            g_hoverCell = g_hoverGroup = -1;
            g_hoverBtn = 0;
            UpdateHoverText();
            InvalidateCell(oc);
            RECT sr = { 0, 0, g_sideW, g_ch };
            InvalidateRect(h, &sr, FALSE);
            InvalidateTitle();
            RefreshCard();                   // 鼠标离开：收起小卡片
        }
        return 0;

    case WM_LBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        HoverFromPoint(p);
        RefreshCard();      // 悬停可能瞬变（如唤醒后首次点击），卡片同步
        if (g_hoverBtn == 2) { CloseToTray(); return 0; }
        if (g_hoverBtn == 1) { TogglePin(); return 0; }
        if (g_hoverGroup >= 0) {
            g_curGroup = g_hoverGroup;
            ViewDirty();
            g_scroll = 0;
            ClampScroll();
            UpdateHoverText();
            InvalidateRect(h, NULL, FALSE);
            RefreshCard();       // 视图换了：同一悬停格对应的条目可能变了
            if (g_hoverGroup > 0) {          // 组 0（全部）不参与重排：按下即记住起点
                g_pressGroup = g_hoverGroup;
                g_pressPt = p;
                g_dragGroup = FALSE;
                g_dropGroup = -1;
                SetCapture(h);
            }
            return 0;
        }
        if (g_hoverCell >= 0) {
            int ii = ItemAtView(g_hoverCell);
            if (ii >= 0 && !IsBlank(ii)) {
                // 按下先不启动：原地抬起才是启动，拖过阈值是排序（见 WM_LBUTTONUP）
                g_pressCell = g_hoverCell;
                g_pressPt = p;
                g_dragging = FALSE;
                g_dropCell = -1;
                g_dropIn = FALSE;
                SetCapture(h);
                return 0;
            }
        }
        ReleaseCapture();
        POINT sp = p;                           // NC 消息要屏幕坐标，否则拖动瞬移
        ClientToScreen(h, &sp);
        SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(sp.x, sp.y));
        return 0;
    }

    case WM_LBUTTONUP: {
        if (g_pressGroup >= 0) {               // 侧栏分组重排落定（未拖动时按下已完成选中，无需处理）
            int from = g_pressGroup, to = g_dropGroup;
            BOOL wasDrag = g_dragGroup;
            g_pressGroup = -1; g_dragGroup = FALSE; g_dropGroup = -1;
            ReleaseCapture();
            if (wasDrag && to > 0 && to != from) MoveGroup(from, to);
            return 0;
        }
        if (g_pressCell < 0) break;
        int from = g_pressCell, to = g_dropCell;
        BOOL inGrid = g_dropIn, wasDrag = g_dragging;
        g_pressCell = -1; g_dragging = FALSE; g_dropCell = -1; g_dropIn = FALSE;
        ReleaseCapture();
        if (!wasDrag) {
            int ii = ItemAtView(from);
            if (ii >= 0 && !IsBlank(ii))
                FlashLaunch(Launch(ii, FALSE), g_items[ii].name);
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
        return 0;
    }

    case WM_CAPTURECHANGED:                     // 捕获被抢：作废未完成的按下/拖动
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
        return 0;

    case WM_RBUTTONUP: {
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
        return 0;
    }

    // 滚轮：WM_APP_WHEEL 来自低级钩子（lParam 已是客户坐标），无焦点也可用
    case WM_APP_WHEEL:
    case WM_MOUSEWHEEL: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        if (m == WM_MOUSEWHEEL) ScreenToClient(h, &p);
        int d = (m == WM_APP_WHEEL) ? (int)(short)w : (int)GET_WHEEL_DELTA_WPARAM(w);
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {   // Ctrl+滚轮：缩放格子（光标在左右区域都响应）
            ApplyCellBase(g_cellBase + (d > 0 ? 4 : -4));
            return 0;
        }
        if (p.x < g_sideW) {
            // 侧栏：滚轮上下切换分组（等同于点击相邻分组），到头后循环
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
        return 0;
    }

    case WM_KEYDOWN: {
        if (w == VK_ESCAPE) {                       // 搜索中：第一次 Esc 清过滤词，再按才隐藏
            if (g_filter[0]) ClearFilter();
            else BeginHide(TRUE);
            return 0;
        }
        if (g_dlgOpen || g_pressCell >= 0) return 0;   // 对话框/拖动排序进行中：不抢键
        if ((w == 'C' || w == 'V') && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
            // Ctrl+C/V：复制悬停格 / 粘贴到悬停格（空格也填），无悬停格则追加
            ClipItem ci;
            if (w == 'C') {
                int ii = ItemAtView(g_hoverCell);
                if (ii >= 0 && !IsBlank(ii)) {
                    ItemToClip(&g_items[ii], &ci);
                    if (ClipSet(&ci)) {              // 剪贴板被占用则不反馈（用户重试即可）
                        WCHAR t[80];
                        swprintf(t, 80, L"已复制: %s", g_items[ii].name);
                        FlashMsg(t);
                    }
                }
                return 0;
            }
            if (!ClipGet(&ci)) { FlashMsg(L"剪贴板没有可粘贴的内容"); return 0; }
            int ii = ItemAtView(g_hoverCell);
            if (ii >= 0) PasteItemAt(ii, &ci);
            else PasteItemNew(&ci);
            WCHAR t[80];
            swprintf(t, 80, L"已粘贴: %s", ci.name);
            FlashMsg(t);
            return 0;
        }
        if (w >= '1' && w <= '9' && !g_filter[0] &&
            !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) {   // 数字键：启动当前组第 N 格（Ctrl 修饰留给复制，过滤中归搜索）
            int ii = ItemAtView((int)(w - '1'));
            if (ii >= 0 && !IsBlank(ii))
                FlashLaunch(Launch(ii, FALSE), g_items[ii].name);
            return 0;
        }
        if (w == VK_RETURN) {                           // 回车：启动悬停格
            int ii = ItemAtView(g_hoverCell);
            if (ii >= 0 && !IsBlank(ii))
                FlashLaunch(Launch(ii, FALSE), g_items[ii].name);
            return 0;
        }
        int n = ViewCount();                            // 方向键：移动悬停高亮（与鼠标悬停同一套状态/重绘）
        if (n <= 0) return 0;
        int c = g_hoverCell;
        if (c < 0 || c >= n) c = 0;                     // 悬停失效时从第一格起
        int nc = c;
        switch (w) {
        case VK_LEFT:  nc = c - 1;      break;
        case VK_RIGHT: nc = c + 1;      break;
        case VK_UP:    nc = c - g_cols; break;
        case VK_DOWN:  nc = c + g_cols; break;
        default: return 0;
        }
        if (nc < 0 || nc >= n) return 0;
        int og = g_hoverGroup;
        g_hoverCell = nc; g_hoverGroup = -1; g_hoverBtn = 0;
        UpdateHoverText();
        RefreshHoverSpots(c, og, TRUE);
        return 0;
    }

    // 可打印字符 → 搜索过滤（退格删字符，Esc 清空）。主窗 IME 已断开，
    // 中文命名只能用 ASCII 搜——换输入法链的 30MB 不值
    case WM_CHAR: {
        if (g_dlgOpen || g_pressCell >= 0) return 0;
        if (w == 0x08) {                              // 退格：删一个字符
            if (!g_filter[0]) return 0;
            int n = (int)wcslen(g_filter);
            g_filter[n - 1] = 0;
            ApplyFilter();
            return 0;
        }
        if (w < 32) return 0;                         // 其余控制字符不管
        if (w >= '1' && w <= '9' && !g_filter[0]) return 0;   // 空过滤时数字键归 WM_KEYDOWN 快速启动
        int n = (int)wcslen(g_filter);
        if (n >= 20) return 0;                        // 过滤词上限 20 字符
        g_filter[n++] = (WCHAR)w;
        g_filter[n] = 0;
        g_flashAt = 0;                                // 开始打字：搜索 supersede 启动反馈
        ApplyFilter();
        return 0;
    }

    // 主窗不进 TSF：默认处理会激活输入上下文，把 MSCTF/textinputframework/
    // Windows.Storage（~12MB）拖进进程。属性对话框不受影响（独立窗口）。
    case WM_IME_SETCONTEXT:
        return 0;

    // 标题栏 ✕、Alt+F4、任务栏关闭：都只是隐藏到托盘，托盘菜单才能真正退出
    case WM_CLOSE:
        CloseToTray();
        return 0;

    case WM_COMMAND:
        if (LOWORD(w) == IDM_EXIT) { ExitApp(); return 0; }
        break;

    case WM_DROPFILES: {
        HDROP hd = (HDROP)w;
        UINT nf = DragQueryFileW(hd, 0xFFFFFFFF, NULL, 0);
        if (nf > 0) {
            ClearFilter();      // 落点按全量视图的格位算（EnsureSlot 补的空格不过滤）：先清过滤
            POINT p;
            // DragQueryPoint 给的是本窗客户坐标；拿不到（少数拖放路径）就退回光标位置
            if (!DragQueryPoint(hd, &p)) {
                GetCursorPos(&p);
                ScreenToClient(h, &p);
            }
            // 必须用 SlotAt：空格也是合法落点。CellAt 只匹配已有条目，
            // 拖到空格返回 -1 会被当成“落到网格外”直接追加到列表末尾——
            // 光标明明停在空格上，格子却没有任何变化。
            int slot = SlotAt(p);
            int g = (g_curGroup == 0) ? DefaultGroup() : g_curGroup;
            int lo = -1, hi = -1;              // 受影响的格子区间（局部重绘用）
            for (UINT k = 0; k < nf && k < (UINT)MAX_ITEMS; k++) {
                WCHAR path[MAX_PATH];
                if (!DragQueryFileW(hd, k, path, MAX_PATH)) continue;
                if (slot < 0) { AddItemByPath(path, g); continue; }   // 网格外：追加末尾
                int s = slot + (int)k;          // 多文件自落点依次排开
                if (lo < 0) lo = hi = s;        // 记下受影响的格子范围
                else { if (s < lo) lo = s; if (s > hi) hi = s; }
                EnsureSlot(s);                  // 落点在末尾之外：先补空格，前面的格子不动
                int ii = ItemAtView(s);
                if (ii >= 0) {
                    // 落点已占用→换上新文件；落点是空格→填入该空位（落哪格停哪格）
                    SetItemTarget(ii, path, g);
                } else {
                    AddItemByPath(path, g);    // 条目数爆满兜底
                }
            }
            ViewDirty();                       // 替换可能改了条目分组
            SaveConfig();
            ClampScroll();
            RefreshCard();                     // 视图/名称可能变了，卡片同步
            if (lo < 0) {
                InvalidateRect(h, NULL, FALSE);          // 拖到网格外：追加到末尾，整窗重绘
            } else {
                InvalidateCells(lo, hi);                 // 只重绘落点区间
            }
        }
        DragFinish(hd);
        return 0;
    }

    case WM_TIMER:
        if (w == TIMER_HIDE) {
            if (g_flashAt && !FlashActive()) { g_flashAt = 0; InvalidateTitle(); }   // 反馈过期：收起
            if (g_cfgDirty) { g_cfgDirty = FALSE; SaveConfig(); }   // 待落盘的配置（缩放防抖）
            if (g_dlgOpen) {          // 对话框开着：定期裁剪，输入法/WinRT 的堆不驻留工作集
                TrimWS();
                g_outSince = 0;
                break;
            }
            if (g_pinned || !IsWindowVisible(h) || g_fadeDir) { g_outSince = 0; break; }
            if (GetTickCount() - g_wakeAt < 3000) { g_outSince = 0; break; }
            if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) { g_outSince = 0; break; }
            POINT p;
            GetCursorPos(&p);
            RECT r;
            GetWindowRect(h, &r);
            InflateRect(&r, 2, 2);
            if (PtInRect(&r, p)) {
                g_outSince = 0;
                if (g_fadeDir < 0) g_fadeDir = 1;
            } else {
                DWORD now = GetTickCount();
                if (!g_outSince) g_outSince = now;
                else if (now - g_outSince > 150) BeginHide(FALSE);   // 移出 150ms 即开始消失
            }
        } else if (w == TIMER_FADE) {
            StepFade();
        }
        return 0;

    case WM_HOTKEY:
        if (IsWindowVisible(h)) BeginHide(TRUE);
        else ShowLauncher();
        return 0;

    // 托盘图标：左键单击显示面板，右键弹菜单
    case WM_TRAY: {
        UINT ev = LOWORD(l);           // NOTIFYICON_VERSION_4：事件在 lParam 低字
        if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK) ShowLauncher();
        else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) TrayShowMenu();
        return 0;
    }

    case WM_APP_WAKE:
        ShowLauncher();
        return 0;

    // 桌面双击唤醒候选：钩子只报时序，这里在 UI 线程确认落点确实是桌面再唤醒
    case WM_APP_WAKE_CAND: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        if (!IsWindowVisible(h) && IsDesktopPoint(p)) ShowLauncher();
        return 0;
    }

    case WM_DESTROY:
        SaveConfig();
        TrayRemove();
        if (g_hookH) {
            PostThreadMessageW(g_hookTid, WM_QUIT, 0, 0);
            WaitForSingleObject(g_hookH, 1000);
            CloseHandle(g_hookH);
            g_hookH = NULL;
        }
        UnregisterHotKey(h, HOTKEY_ID);
        for (int i = 0; i < g_nItems; i++) DropIcon(i);
        if (g_font) DeleteObject(g_font);
        if (g_fontT) DeleteObject(g_fontT);
        if (g_brBG) DeleteObject(g_brBG);
        if (g_brSide) DeleteObject(g_brSide);
        if (g_brCell) DeleteObject(g_brCell);
        if (g_brCellHi) DeleteObject(g_brCellHi);
        if (g_brLine) DeleteObject(g_brLine);
        if (g_brAccent) DeleteObject(g_brAccent);
        if (g_brAccel) DeleteObject(g_brAccel);
        if (g_brPanel) DeleteObject(g_brPanel);
    if (g_penDim) DeleteObject(g_penDim);
    if (g_penAccent) DeleteObject(g_penAccent);
    if (g_brPinOn) DeleteObject(g_brPinOn);
    if (g_brPinOff) DeleteObject(g_brPinOff);
    if (g_penLine) DeleteObject(g_penLine);
        if (g_memDC) DeleteDC(g_memDC);
        if (g_memBmp) DeleteObject(g_memBmp);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- 入口
static HWND g_searchHit = NULL;

static BOOL CALLBACK FindLauncherCb(HWND h, LPARAM lp)
{
    WCHAR cls[64];
    UNREF(lp);
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, APP_CLASS)) { g_searchHit = h; return FALSE; }
    return TRUE;
}

static HWND FindLauncherWindow(void)
{
    g_searchHit = NULL;
    EnumWindows(FindLauncherCb, 0);
    return g_searchHit;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    UNREF(prev); UNREF(cmd); UNREF(show);
    g_inst = inst;

    HWND exist = FindLauncherWindow();
    if (exist) {
        PostMessageW(exist, WM_APP_WAKE, 0, 0);
        return 0;
    }

    HMODULE u32 = GetModuleHandleW(L"user32.dll");   // 两个导出都取自 user32，取一次句柄
    pSetDpi = (PFN_SetProcDpiCtx)
        GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    if (pSetDpi) pSetDpi((HANDLE)-4);   // PerMonitorV2
    pExtractIcons = (PFN_PrivateExtractIconsW)
        GetProcAddress(u32, "PrivateExtractIconsW");

    HDC scr = GetDC(NULL);
    g_dpi = GetDeviceCaps(scr, LOGPIXELSX);
    ReleaseDC(NULL, scr);

    IniPath();
    ScaleMetrics();
    LoadConfig();
    InitAutostart();   // 开机自启状态以注册表为准（托盘菜单勾选用）；路径过期则自愈重写
    g_clipFmt = RegisterClipboardFormatW(L"KKLauncherItem");   // 私有剪贴板格式；失败则 Ctrl+V 只认纯文本

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = APP_CLASS;
    RegisterClassExW(&wc);

    // 对话框类只注册一次：原在 RunDialog 里每打开一次属性框就注册一遍，纯重复劳动
    WNDCLASSEXW dwc;
    ZeroMemory(&dwc, sizeof dwc);
    dwc.cbSize = sizeof dwc;
    dwc.lpfnWndProc = DlgProc;
    dwc.hInstance = g_inst;
    dwc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    dwc.hbrBackground = NULL;
    dwc.lpszClassName = DLG_CLASS;
    RegisterClassExW(&dwc);

    g_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_LAYERED, APP_CLASS, APPNAME, WS_POPUP,
        g_savedX, g_savedY, g_savedW, g_savedH, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;

    // 主窗没有输入框：断开 IME。否则窗口一拿焦点，整条输入法链
    // （SogouPY.ime/SogouTSF/MSCTF/textinputframework/d2d1/DWrite ~30MB）就被拉进进程。
    // 属性对话框是独立窗口，保持默认 IME，中文输入不受影响。
    ImmAssociateContext(g_hwnd, NULL);

    ClampToWorkArea();
    if (PruneInvalidItems()) SaveConfig();   // 失效项目启动即清，并写回配置
    if (g_silent) {
        // 静默启动：直接进托盘（WM_CREATE 已加托盘图标）。停掉移出检测心跳——
        // 不可见窗口本来就用不到，与淡出隐藏路径保持同一个"零轮询"不变量；
        // 唤醒时 ShowLauncher 会重启它
        ShowWindow(g_hwnd, SW_HIDE);
        KillTimer(g_hwnd, TIMER_HIDE);
    } else {
        ShowWindow(g_hwnd, SW_SHOW);
        ForceForeground(g_hwnd);
        g_outSince = 0;
        g_wakeAt = GetTickCount();   // 启动等同一次唤醒：给足 3s 宽限，否则光标不在窗口上时约 0.3s 就自动隐藏
        g_alpha = 0;
        SetLayeredWindowAttributes(g_hwnd, 0, 0, LWA_ALPHA);
        g_fadeDir = 1;
        SetTimer(g_hwnd, TIMER_FADE, 12, NULL);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}