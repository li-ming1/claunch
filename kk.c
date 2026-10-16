// kk.c — 桌面格子：单文件、零第三方依赖的桌面启动器
// 左侧自定义分组、密集格子（空格也画出来）、拖程序进格子直接单击启动、
// 悬停顶部居中显示名称、鼠标移出自动隐藏、定住按钮、双击桌面空白唤醒、
// Ctrl+Alt+K 快捷键、右键格子可编辑 名称/文件名/参数/工作文件夹/工具提示。
// 内存取向：不用整屏双缓冲（直接绘制 + 精确失效），配置流式读写，
// 图标按需加载，仅依赖 Win32 + shell32。
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
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")

// ---------------------------------------------------------------- 常量
#define MAX_GROUPS      16
#define MAX_ITEMS       128
#define APP_CLASS       L"KKLauncherWnd"
#define DLG_CLASS       L"KKDialogWnd"
#define APPNAME         L"桌面格子"
#define ININAME         L"kk.ini"

#define WM_APP_WAKE     (WM_APP + 1)
#define TIMER_HIDE      1
#define TIMER_FADE      2
#define HOTKEY_ID       1

#define IDM_LAUNCH      1001
#define IDM_ADMIN       1002
#define IDM_LOC         1003
#define IDM_DEL         1004
#define IDM_GRP_RENAME  1005
#define IDM_GRP_DEL     1006
#define IDM_GRP_NEW     1007
#define IDM_PROPS       1008
#define IDM_TOP         1009
#define IDM_EXIT        1010

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
typedef struct {
    WCHAR name[48];
    WCHAR path[MAX_PATH];
    WCHAR args[192];
    WCHAR work[MAX_PATH];
    WCHAR tip[96];
    int   group;
    HICON icon;
    BOOL  ownIcon;
    BOOL  iconTried;
} Item;

typedef struct { WCHAR name[48]; } Group;

static Item  g_items[MAX_ITEMS];
static int   g_nItems = 0;
static Group g_groups[MAX_GROUPS];
static int   g_nGroups = 1;

// ---------------------------------------------------------------- 状态
static HINSTANCE g_inst;
static HWND      g_hwnd;
static int       g_dpi = 96;

static HFONT  g_font, g_fontT;
static HBRUSH g_brBG, g_brSide, g_brCell, g_brCellHi, g_brLine, g_brAccent, g_brAccel, g_brPanel;
static int    g_cw, g_ch;

static int  g_titleH = 34, g_sideW = 104, g_gap = 3, g_cellW = 72, g_cellH = 58, g_grpH = 34;
static BOOL g_pinned = FALSE;
static BOOL g_topmost = TRUE;
static int  g_curGroup = 0;
static int  g_scroll = 0;       // 格子区滚动（像素）
static int  g_grpScroll = 0;    // 侧栏滚动（像素）

static int  g_hoverCell = -1, g_hoverGroup = -1, g_hoverBtn = 0; // btn: 1 钉 2 关
static WCHAR g_hoverText[80];
static BOOL g_hoverActive = FALSE;
static BOOL g_tracking = FALSE;

static DWORD g_outSince = 0;
static DWORD g_wakeAt = 0;
static int   g_alpha = 255, g_fadeDir = 0;
static BOOL  g_dlgOpen = FALSE;

static HHOOK g_hook = NULL;
static WCHAR g_ini[MAX_PATH];
static int   g_savedX, g_savedY, g_savedW, g_savedH;
static BOOL  g_applySavedRect = FALSE;

typedef BOOL (WINAPI *PFN_SetProcDpiCtx)(HANDLE);
static PFN_SetProcDpiCtx pSetDpi = NULL;

#define UNREF(x) ((void)(x))

// 前向声明
static void ScaleMetrics(void);
static void MakeFontsAndBrushes(void);
static void PaintWindow(HDC dc);
static void ClampToWorkArea(void);
static void MoveToPoint(POINT pt);
static void BeginHide(BOOL force);
static void SaveConfig(void);
static void InvalidateCell(int vi);
static void InvalidateTitle(void);
static BOOL RunDialog(HWND owner, const WCHAR *title, void *fields, int n);
static void ForceForeground(HWND h);
static void UpdateHoverText(void);

// ---------------------------------------------------------------- 工具
static int S_(int v) { return MulDiv(v, g_dpi, 96); }

// 把 '|' 换成 '_'，避免破坏配置格式
static const WCHAR *SafeField(WCHAR *dst, int cch, const WCHAR *src)
{
    wcsncpy_s(dst, (size_t)cch, src, _TRUNCATE);
    for (WCHAR *p = dst; *p; p++) if (*p == L'|') *p = L'_';
    return dst;
}

// ---------------------------------------------------------------- 布局
static int Cols(void)
{
    int c = (g_cw - g_sideW + g_gap) / (g_cellW + g_gap);
    return c < 1 ? 1 : c;
}

static int Rows(void)
{
    int r = (g_ch - g_titleH + g_gap) / (g_cellH + g_gap);
    return r < 1 ? 1 : r;
}

static RECT CellRect(int vi)
{
    int c = Cols(), col = vi % c, row = vi / c;
    RECT r;
    r.left   = g_sideW + col * (g_cellW + g_gap);
    r.top    = g_titleH + row * (g_cellH + g_gap) - g_scroll;
    r.right  = r.left + g_cellW;
    r.bottom = r.top + g_cellH;
    return r;
}

static int ViewCount(void)
{
    int n = 0;
    for (int i = 0; i < g_nItems; i++)
        if (g_curGroup == 0 || g_items[i].group == g_curGroup) n++;
    return n;
}

static int ItemAtView(int vi)
{
    int k = -1;
    for (int i = 0; i < g_nItems; i++) {
        if (g_curGroup == 0 || g_items[i].group == g_curGroup) {
            k++;
            if (k == vi) return i;
        }
    }
    return -1;
}

static int CellAt(POINT p)
{
    if (p.x < g_sideW || p.y < g_titleH) return -1;
    int n = ViewCount();
    if (n > Cols() * Rows()) n = Cols() * Rows();   // 只有可见格子可点
    for (int vi = 0; vi < n; vi++) {
        RECT r = CellRect(vi);
        if (PtInRect(&r, p)) return vi;
    }
    return -1;
}

static void ClampScroll(void)
{
    int need = ViewCount();
    int visible = Cols() * Rows();
    int maxS = need > visible ? (need - visible + Cols() - 1) / Cols() * (g_cellH + g_gap) : 0;
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
    g_scroll = 0;
    ClampScroll();
    EnsureGroupVisible(gi);
    UpdateHoverText();
    RECT sr = { 0, 0, g_sideW, g_ch };
    InvalidateRect(g_hwnd, &sr, FALSE);
    RECT gr = { g_sideW, g_titleH, g_cw, g_ch };
    InvalidateRect(g_hwnd, &gr, FALSE);
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

static RECT PinBtnRect(void)
{
    int w = S_(30), h = S_(22), t = (g_titleH - h) / 2;
    RECT r = { g_cw - 2 - w * 2 - 4, t, g_cw - 2 - w - 4, t + h };
    return r;
}

static RECT CloseBtnRect(void)
{
    int w = S_(30), h = S_(22), t = (g_titleH - h) / 2;
    RECT r = { g_cw - 2 - w, t, g_cw - 2, t + h };
    return r;
}

static int BtnAt(POINT p)
{
    RECT r;
    r = PinBtnRect();   if (PtInRect(&r, p)) return 1;
    r = CloseBtnRect(); if (PtInRect(&r, p)) return 2;
    return 0;
}

// ---------------------------------------------------------------- 图标（按需加载）
// 取图标优先走 user32 内部函数 PrivateExtractIconsW：它直接从文件资源里取，
// 不会把 shell 的图标/缩略图处理栈（shell.dll、PROPSYS、thumbcache…，
// 实测多出 10 个模块、约 2.5 MB 常驻）拉进本进程。
typedef int (WINAPI *PFN_PrivateExtractIconsW)(LPCWSTR, int, int, int,
                                               HICON *, int *, int, int);
static PFN_PrivateExtractIconsW pExtractIcons = NULL;

// 是否是“可执行映像”（图标直接来自自身资源，无需 shell 参与）
static BOOL IsImageFile(const WCHAR *path)
{
    static const WCHAR *exts[] = { L".exe", L".dll", L".ico", L".cpl", L".scr",
                                   L".ocx", L".ax",  L".mui", L".efi", L".sys" };
    const WCHAR *dot = wcsrchr(path, L'.');
    const WCHAR *bs = wcsrchr(path, L'\\');
    if (!dot || (bs && dot < bs)) return FALSE;
    for (int i = 0; i < (int)(sizeof exts / sizeof exts[0]); i++)
        if (!_wcsicmp(dot, exts[i])) return TRUE;
    return FALSE;
}

static HICON IconFor(const WCHAR *path, int want)
{
    if (pExtractIcons) {
        HICON icons[1] = { 0 };
        int ids[1] = { 0 };
        if (pExtractIcons(path, 0, want, want, icons, ids, 1, 0) > 0 && icons[0])
            return icons[0];
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
    {
        HICON big = NULL, sml = NULL;
        if (ExtractIconExW(path, 0, &big, &sml, 1) && (big || sml)) {
            if (big) return big;
            if (sml) {
                HICON scaled = (HICON)CopyImage(sml, IMAGE_ICON, want, want, LR_DEFAULTCOLOR);
                DestroyIcon(sml);
                return scaled;
            }
        }
    }
    // 文件夹、快捷方式、脚本、文档：只能用 shell（SHGetFileInfo）。
    // 但对 exe/dll 不走这条路——它会把 shell.dll、PROPSYS、thumbcache 等
    // 10 个模块和约 2.4 MB 常驻内存带进来。
    if (IsImageFile(path)) return NULL;
    SHFILEINFOW si;
    ZeroMemory(&si, sizeof si);
    DWORD a = GetFileAttributesW(path);
    UINT fl = SHGFI_ICON | SHGFI_LARGEICON;
    if (a == INVALID_FILE_ATTRIBUTES) { fl |= SHGFI_USEFILEATTRIBUTES; a = FILE_ATTRIBUTE_NORMAL; }
    else if (a & FILE_ATTRIBUTE_DIRECTORY) { fl |= SHGFI_USEFILEATTRIBUTES; }
    if (SHGetFileInfoW(path, a, &si, sizeof si, fl)) return si.hIcon;
    return NULL;
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
    HICON h = IconFor(g_items[i].path, S_(30));
    if (h) { g_items[i].icon = h; g_items[i].ownIcon = TRUE; }
    else    g_items[i].icon = LoadIconW(NULL, IDI_APPLICATION);   // 共享图标，不销毁
}

// ---------------------------------------------------------------- 条目与分组
static int AddItemRaw(const WCHAR *name, const WCHAR *path, int group,
                      const WCHAR *args, const WCHAR *work, const WCHAR *tip)
{
    if (g_nItems >= MAX_ITEMS) return -1;
    Item *it = &g_items[g_nItems];
    ZeroMemory(it, sizeof *it);
    wcsncpy_s(it->name, 48, name, _TRUNCATE);
    wcsncpy_s(it->path, MAX_PATH, path, _TRUNCATE);
    if (args) wcsncpy_s(it->args, 192, args, _TRUNCATE);
    if (work) wcsncpy_s(it->work, MAX_PATH, work, _TRUNCATE);
    if (tip)  wcsncpy_s(it->tip, 96, tip, _TRUNCATE);
    it->group = group;
    return g_nItems++;
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
    return AddItemRaw(name, path, group, NULL, NULL, NULL);
}

static void RemoveItemAt(int k)
{
    if (k < 0 || k >= g_nItems) return;
    DropIcon(k);
    for (int i = k; i < g_nItems - 1; i++) g_items[i] = g_items[i + 1];
    g_nItems--;
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
    ClampScroll();
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
        g_scroll = 0;
        ClampGrpScroll();
        SaveConfig();
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

// ---------------------------------------------------------------- 启动
static void Launch(int idx, BOOL asAdmin)
{
    if (idx < 0 || idx >= g_nItems) return;
    const Item *it = &g_items[idx];
    ShellExecuteW(g_hwnd, asAdmin ? L"runas" : L"open", it->path,
                  it->args[0] ? it->args : NULL,
                  it->work[0] ? it->work : NULL, SW_SHOWNORMAL);
}

static void OpenLocation(const WCHAR *path)
{
    WCHAR args[MAX_PATH + 16];
    swprintf(args, MAX_PATH + 16, L"/select,\"%s\"", path);
    ShellExecuteW(g_hwnd, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
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
    g_topmost = TRUE;
    g_nGroups = 0;
    wcsncpy_s(g_groups[0].name, 48, L"全部", _TRUNCATE);
    g_nGroups = 1;

    WCHAR line[1200], section[32] = L"";
    WCHAR rect[64] = L"";
    if (r.f == INVALID_HANDLE_VALUE) {                  // 无配置文件：给默认分组
        AddGroup(L"常用");
        ClampScroll();
        return;
    }
    while (RdLine(&r, line, 1200)) {
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
            else if (!wcscmp(key, L"Topmost")) g_topmost = _wtoi(val) ? TRUE : FALSE;
            else if (!wcscmp(key, L"Group"))   g_curGroup = _wtoi(val);
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
                       nf >= 4 ? f[3] : NULL, nf >= 5 ? f[4] : NULL, nf >= 6 ? f[5] : NULL);
        }
    }
    CloseHandle(r.f);
    if (g_nGroups < 2) AddGroup(L"常用");
    if (g_curGroup < 0 || g_curGroup >= g_nGroups) g_curGroup = 0;

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
    FmtLine(f, L"Topmost=%d\r\n", g_topmost ? 1 : 0);
    FmtLine(f, L"Group=%d\r\n", g_curGroup);
    FmtLine(f, L"[Groups]\r\n");
    FmtLine(f, L"Count=%d\r\n", g_nGroups);
    for (int i = 0; i < g_nGroups; i++) {
        static WCHAR gName[96];
        FmtLine(f, L"G%d=%s\r\n", i, SafeField(gName, 96, g_groups[i].name));
    }
    FmtLine(f, L"[Items]\r\n");
    FmtLine(f, L"Count=%d\r\n", g_nItems);
    // 注意：每个字段必须用独立的缓冲区——函数参数求值顺序不确定
    static WCHAR sName[96], sPath[MAX_PATH + 8], sArgs[MAX_PATH + 8],
                  sWork[MAX_PATH + 8], sTip[128];
    for (int i = 0; i < g_nItems; i++) {
        FmtLine(f, L"I%d=%s|%s|%d|%s|%s|%s\r\n", i,
                SafeField(sName, 96, g_items[i].name),
                SafeField(sPath, MAX_PATH + 8, g_items[i].path), g_items[i].group,
                SafeField(sArgs, MAX_PATH + 8, g_items[i].args),
                SafeField(sWork, MAX_PATH + 8, g_items[i].work),
                SafeField(sTip, 128, g_items[i].tip));
    }
    CloseHandle(f);
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
}

static void BeginHide(BOOL force)
{
    if (!force && g_pinned) return;
    g_fadeDir = -1;
    SetTimer(g_hwnd, TIMER_FADE, 12, NULL);
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
    g_sideW  = S_(104);
    g_gap    = S_(3);
    g_cellW  = S_(72);
    g_cellH  = S_(58);
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

static void MakeFontsAndBrushes(void)
{
    MakeFonts();
    MakeBrushes();
}

// ---------------------------------------------------------------- 悬停文字
static void UpdateHoverText(void)
{
    if (g_hoverCell >= 0) {
        int ii = ItemAtView(g_hoverCell);
        if (ii >= 0) {
            const Item *it = &g_items[ii];
            wcsncpy_s(g_hoverText, 80, it->tip[0] ? it->tip : it->name, _TRUNCATE);
            g_hoverActive = TRUE;
            return;
        }
    }
    if (g_hoverGroup >= 0) {
        wcsncpy_s(g_hoverText, 80, g_groups[g_hoverGroup].name, _TRUNCATE);
        g_hoverActive = TRUE;
        return;
    }
    switch (g_hoverBtn) {
    case 1: wcsncpy_s(g_hoverText, 80, g_pinned ? L"取消定住" : L"定住窗口", _TRUNCATE); g_hoverActive = TRUE; return;
    case 2: wcsncpy_s(g_hoverText, 80, L"退出", _TRUNCATE); g_hoverActive = TRUE; return;
    default: break;
    }
    if (g_nItems == 0) {
        wcsncpy_s(g_hoverText, 80, L"把程序拖进格子里", _TRUNCATE);
        g_hoverActive = FALSE;
        return;
    }
    wcsncpy_s(g_hoverText, 80, APPNAME, _TRUNCATE);
    g_hoverActive = FALSE;
}

static void HoverFromPoint(POINT p)
{
    g_hoverCell  = CellAt(p);
    g_hoverGroup = (g_hoverCell >= 0) ? -1 : GroupAt(p);
    g_hoverBtn   = (g_hoverCell >= 0 || g_hoverGroup >= 0) ? 0 : BtnAt(p);
    UpdateHoverText();
}

// ---------------------------------------------------------------- 绘制
static void InvalidateCell(int vi)
{
    if (vi < 0) return;
    RECT r = CellRect(vi);
    InvalidateRect(g_hwnd, &r, FALSE);
}

static void InvalidateTitle(void)
{
    RECT r = { 0, 0, g_cw, g_titleH };
    InvalidateRect(g_hwnd, &r, FALSE);
}

static void DrawPinGlyph(HDC dc, RECT r, BOOL on)
{
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2 - 2;
    int rad = S_(4);
    HPEN pen = CreatePen(PS_SOLID, 1, on ? C_ACCENT : C_DIM);
    HBRUSH br = CreateSolidBrush(on ? C_ACCENT : C_BG);
    HPEN op = (HPEN)SelectObject(dc, pen);
    HBRUSH ob = (HBRUSH)SelectObject(dc, br);
    Ellipse(dc, cx - rad, cy - rad, cx + rad, cy + rad);
    MoveToEx(dc, cx, cy + rad, NULL);
    LineTo(dc, cx, r.bottom - 3);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(pen); DeleteObject(br);
}

static void DrawCloseGlyph(HDC dc, RECT r)
{
    HPEN pen = CreatePen(PS_SOLID, S_(1), C_DIM);
    HPEN op = (HPEN)SelectObject(dc, pen);
    int m = S_(8);
    MoveToEx(dc, r.left + m, r.top + m, NULL); LineTo(dc, r.right - m, r.bottom - m);
    MoveToEx(dc, r.right - m, r.top + m, NULL); LineTo(dc, r.left + m, r.bottom - m);
    SelectObject(dc, op);
    DeleteObject(pen);
}

static void PaintSidebar(HDC dc)
{
    RECT sr = { 0, 0, g_sideW, g_ch };
    FillRect(dc, &sr, g_brSide);
    RECT ln = { g_sideW - 1, 0, g_sideW, g_ch };
    FillRect(dc, &ln, g_brLine);

    HFONT old = (HFONT)SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < g_nGroups; i++) {
        RECT r = GroupRect(i);
        if (r.bottom < g_titleH || r.top > g_ch) continue;
        BOOL sel = (i == g_curGroup), hov = (i == g_hoverGroup);
        // 只用“选中高亮”，不再叠加悬停灰底，避免两种高亮重复
        if (sel) FillRect(dc, &r, g_brCell);
        SetTextColor(dc, sel ? C_ACCEL : (hov ? C_TEXT : C_DIM));
        RECT tr = { r.left + S_(6), r.top, r.right - S_(4), r.bottom };
        DrawTextW(dc, g_groups[i].name, -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        // 选中标记放在下方（原来在左侧）
        if (sel) {
            RECT bar = { r.left, r.bottom - S_(2), r.right, r.bottom };
            FillRect(dc, &bar, g_brAccel);
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
    SelectObject(dc, old);
}

static void PaintTitle(HDC dc)
{
    HFONT old = (HFONT)SelectObject(dc, g_fontT);
    SetBkMode(dc, TRANSPARENT);
    RECT ttl = { g_sideW + S_(8), 0, g_cw - S_(90), g_titleH };
    SetTextColor(dc, g_hoverActive ? C_TEXT : C_DIM);
    DrawTextW(dc, g_hoverText, -1, &ttl,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
    DrawPinGlyph(dc, PinBtnRect(), g_pinned);
    DrawCloseGlyph(dc, CloseBtnRect());
}

static void PaintGrid(HDC dc)
{
    HFONT old = (HFONT)SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    int slots = Cols() * Rows();
    int isz = S_(30);
    int n = ViewCount();
    int limit = slots > n ? slots : n;
    for (int vi = 0; vi < limit; vi++) {
        RECT r = CellRect(vi);
        if (r.bottom <= g_titleH || r.top >= g_ch) continue;
        int ii = ItemAtView(vi);
        BOOL hov = (vi == g_hoverCell) && ii >= 0;
        FillRect(dc, &r, hov ? g_brCellHi : g_brCell);
        FrameRect(dc, &r, g_brLine);
        if (ii < 0) continue;
        EnsureIcon(ii);
        if (g_items[ii].icon) {
            int ix = (r.left + r.right - isz) / 2;
            DrawIconEx(dc, ix, r.top + S_(5), g_items[ii].icon, isz, isz, 0, NULL, DI_NORMAL);
        }
        RECT lr = { r.left + S_(2), r.bottom - S_(17), r.right - S_(2), r.bottom - S_(3) };
        SetTextColor(dc, C_TEXT);
        DrawTextW(dc, g_items[ii].name, -1, &lr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, old);
}

static void PaintWindow(HDC dc)
{
    RECT rc = { 0, 0, g_cw, g_ch };
    FillRect(dc, &rc, g_brBG);
    PaintSidebar(dc);
    PaintTitle(dc);
    PaintGrid(dc);
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
        {FILE *dbg=fopen("kk_debug.log","a"); if(dbg){fwprintf(dbg,L"DLGCREATE n=%d dlgW=%d rowH=%d labW=%d titleH=%d\r\n",c->n,c->dlgW,c->rowH,c->labW,c->titleH); fclose(dbg);}}   // TEMP-DEBUG
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
            {FILE *dbg=fopen("kk_debug.log","a"); if(dbg){fwprintf(dbg,L"  row %d st=%p ed=%p y=%d\r\n",i,(void*)st,(void*)ed,y); fclose(dbg);}}   // TEMP-DEBUG
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

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = DlgProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = DLG_CLASS;
    RegisterClassExW(&wc);

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

// ---------------------------------------------------------------- 菜单
static void ShowCellMenu(int ii)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_LAUNCH, L"打开");
    AppendMenuW(m, MF_STRING, IDM_ADMIN, L"以管理员身份运行");
    AppendMenuW(m, MF_STRING, IDM_LOC, L"打开文件位置");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_PROPS, L"格子属性(&R)...");
    AppendMenuW(m, MF_STRING, IDM_DEL, L"从格子移除");
    POINT p;
    GetCursorPos(&p);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
    if (!cmd) return;
    switch (cmd) {
    case IDM_LAUNCH: Launch(ii, FALSE); break;
    case IDM_ADMIN:  Launch(ii, TRUE); break;
    case IDM_LOC:    OpenLocation(g_items[ii].path); break;
    case IDM_DEL:    RemoveItemAt(ii); SaveConfig(); ClampScroll(); InvalidateRect(g_hwnd, NULL, FALSE); break;
    case IDM_PROPS: {
        WCHAR oldPath[MAX_PATH];
        wcsncpy_s(oldPath, MAX_PATH, g_items[ii].path, _TRUNCATE);
        DlgField f[5] = {
            { L"名称(N):",       g_items[ii].name, 48,       0 },
            { L"文件名(F):",     g_items[ii].path, MAX_PATH, 1 },
            { L"参数(P):",       g_items[ii].args, 192,      0 },
            { L"工作文件夹(D):", g_items[ii].work, MAX_PATH, 0 },
            { L"工具提示(T):",   g_items[ii].tip,  96,       0 },
        };
        if (RunDialog(g_hwnd, L"格子属性", f, 5)) {
            if (wcscmp(oldPath, g_items[ii].path)) {   // 目标变了，重取图标
                DropIcon(ii);
                EnsureIcon(ii);
            }
            SaveConfig();
            UpdateHoverText();
            InvalidateCell(ItemAtView(g_hoverCell));
            InvalidateTitle();
            InvalidateRect(g_hwnd, NULL, FALSE);
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
    POINT p;
    GetCursorPos(&p);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
    if (cmd == IDM_GRP_RENAME) {
        DlgField f[1] = { { L"名称(N):", g_groups[gi].name, 48, 0 } };
        if (RunDialog(g_hwnd, L"重命名分组", f, 1)) {
            if (!g_groups[gi].name[0]) wcsncpy_s(g_groups[gi].name, 48, L"未命名", _TRUNCATE);
            SaveConfig();
            UpdateHoverText();
            InvalidateRect(g_hwnd, NULL, FALSE);
        }
    } else if (cmd == IDM_GRP_DEL) {
        DeleteGroup(gi);
        SaveConfig();
        InvalidateRect(g_hwnd, NULL, FALSE);
    } else if (cmd == IDM_GRP_NEW) {
        NewGroup();
    }
}

static void ShowTitleMenu(void)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g_topmost ? MF_CHECKED : 0), IDM_TOP, L"总在最前面");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出程序");
    POINT p;
    GetCursorPos(&p);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
    if (cmd == IDM_TOP) {
        g_topmost = !g_topmost;
        SetWindowPos(g_hwnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SaveConfig();
    } else if (cmd == IDM_EXIT) {
        SaveConfig();
        DestroyWindow(g_hwnd);
    }
}

// ---------------------------------------------------------------- 全局双击唤醒
static DWORD s_lastClick = 0;
static POINT  s_lastPt = { 0, 0 };

static BOOL IsDesktopPoint(POINT pt)
{
    HWND h = WindowFromPoint(pt);
    if (!h) return FALSE;
    h = GetAncestor(h, GA_ROOT);
    if (!h) return FALSE;
    WCHAR cls[64];
    GetClassNameW(h, cls, 64);
    return !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW");
}

static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && wParam == WM_LBUTTONDOWN) {
        MSLLHOOKSTRUCT *p = (MSLLHOOKSTRUCT *)lParam;
        DWORD now = GetTickCount();
        int dx = p->pt.x - s_lastPt.x, dy = p->pt.y - s_lastPt.y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        BOOL dbl = s_lastClick && (now - s_lastClick) <= (DWORD)GetDoubleClickTime()
                   && dx <= GetSystemMetrics(SM_CXDLGFRAME)
                   && dy <= GetSystemMetrics(SM_CYDLGFRAME);
        if (dbl && !IsWindowVisible(g_hwnd) && IsDesktopPoint(p->pt))
            PostMessageW(g_hwnd, WM_APP_WAKE, 0, 0);
        s_lastClick = now;
        s_lastPt = p->pt;
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// ---------------------------------------------------------------- 窗口过程
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        g_hwnd = h;
        MakeFontsAndBrushes();
        SetWindowPos(h, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
        DragAcceptFiles(h, TRUE);
        g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, NULL, 0);
        RegisterHotKey(h, HOTKEY_ID, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'K');
        SetTimer(h, TIMER_HIDE, 120, NULL);
        return 0;
    }

    case WM_SIZE: {
        g_cw = LOWORD(l);
        g_ch = HIWORD(l);
        {   // TEMP-DEBUG
            FILE *dbg = fopen("kk_debug.log", "a");
            if (dbg) { fwprintf(dbg, L"SIZE %dx%d dpi=%d\r\n", g_cw, g_ch, g_dpi); fclose(dbg); }
        }
        int rad = S_(10);
        HRGN rgn = CreateRoundRectRgn(0, 0, g_cw + 1, g_ch + 1, rad, rad);
        if (rgn) SetWindowRgn(h, rgn, TRUE);
        ClampScroll();
        ClampGrpScroll();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)l;
        mm->ptMinTrackSize.x = S_(260);
        mm->ptMinTrackSize.y = S_(200);
        return 0;
    }

    case WM_DPICHANGED: {
        {   // TEMP-DEBUG
            FILE *dbg = fopen("kk_debug.log", "a");
            if (dbg) {
                RECT *rr = (RECT *)l;
                fwprintf(dbg, L"DPICHANGED wParam=%d applySaved=%d suggest=(%d,%d %dx%d) cur=%dx%d\r\n",
                         (int)LOWORD(w), g_applySavedRect, rr->left, rr->top,
                         rr->right - rr->left, rr->bottom - rr->top, g_cw, g_ch);
                fclose(dbg);
            }
        }
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
        PaintWindow(dc);          // 直接绘制：系统按更新区域裁剪，不会闪
        EndPaint(h, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        int oc = g_hoverCell, og = g_hoverGroup, ob = g_hoverBtn;
        HoverFromPoint(p);
        if (oc != g_hoverCell || og != g_hoverGroup || ob != g_hoverBtn) {
            InvalidateCell(oc);
            InvalidateCell(g_hoverCell);
            RECT sr = { 0, 0, g_sideW, g_ch };
            if (og != g_hoverGroup) InvalidateRect(h, &sr, FALSE);
            InvalidateTitle();
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
        }
        return 0;

    case WM_LBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        HoverFromPoint(p);
        if (g_hoverBtn == 2) { SaveConfig(); DestroyWindow(h); return 0; }
        if (g_hoverBtn == 1) {
            g_pinned = !g_pinned;
            UpdateHoverText();
            SaveConfig();
            InvalidateTitle();
            return 0;
        }
        if (g_hoverGroup >= 0) {
            g_curGroup = g_hoverGroup;
            g_scroll = 0;
            ClampScroll();
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        if (g_hoverCell >= 0) {
            int ii = ItemAtView(g_hoverCell);
            if (ii >= 0) Launch(ii, FALSE);
            return 0;
        }
        ReleaseCapture();
        SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, l);
        return 0;
    }

    case WM_RBUTTONUP: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        HoverFromPoint(p);
        if (g_hoverCell >= 0) {
            int ii = ItemAtView(g_hoverCell);
            if (ii >= 0) ShowCellMenu(ii);
        } else if (g_hoverGroup >= 0) {
            ShowGroupMenu(g_hoverGroup);
        } else if (g_hoverBtn || p.y < g_titleH) {
            ShowTitleMenu();
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ScreenToClient(h, &p);
        int d = GET_WHEEL_DELTA_WPARAM(w);
        if (p.x < g_sideW) {
            // 侧栏：滚轮上下切换分组（等同于点击相邻分组），到头后循环
            int gi = g_curGroup + (d > 0 ? -1 : 1);
            if (gi < 0) gi = g_nGroups - 1;
            if (gi >= g_nGroups) gi = 0;
            SelectGroup(gi);
        } else {
            int before = g_scroll;
            g_scroll += (d > 0 ? -1 : 1) * (g_cellH + g_gap);
            ClampScroll();
            if (g_scroll != before) {
                RECT gr = { g_sideW, g_titleH, g_cw, g_ch };
                InvalidateRect(h, &gr, FALSE);
            }
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (w == VK_ESCAPE) BeginHide(TRUE);
        return 0;

    case WM_DROPFILES: {
        HDROP hd = (HDROP)w;
        WCHAR path[MAX_PATH];
        if (DragQueryFileW(hd, 0, path, MAX_PATH)) {
            POINT p;
            DragQueryPoint(hd, &p);
            int vi = CellAt(p);
            int g = (g_curGroup == 0) ? DefaultGroup() : g_curGroup;
            int ii = (vi >= 0) ? ItemAtView(vi) : -1;
            if (ii >= 0) {
                DropIcon(ii);
                WCHAR name[48];
                NameFromPath(path, name, 48);
                wcsncpy_s(g_items[ii].name, 48, name, _TRUNCATE);
                wcsncpy_s(g_items[ii].path, MAX_PATH, path, _TRUNCATE);
                g_items[ii].group = g;
                g_items[ii].args[0] = 0;
                g_items[ii].work[0] = 0;
                EnsureIcon(ii);
            } else {
                AddItemByPath(path, g);
            }
            SaveConfig();
            ClampScroll();
            InvalidateRect(h, NULL, FALSE);
        }
        DragFinish(hd);
        return 0;
    }

    case WM_TIMER:
        if (w == TIMER_HIDE) {
            if (g_pinned || g_dlgOpen || !IsWindowVisible(h) || g_fadeDir) { g_outSince = 0; break; }
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
                else if (now - g_outSince > 300) BeginHide(FALSE);
            }
        } else if (w == TIMER_FADE) {
            StepFade();
        }
        return 0;

    case WM_HOTKEY:
        if (IsWindowVisible(h)) BeginHide(TRUE);
        else ShowLauncher();
        return 0;

    case WM_APP_WAKE:
        ShowLauncher();
        return 0;

    case WM_DESTROY:
        SaveConfig();
        if (g_hook) UnhookWindowsHookEx(g_hook);
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

    pSetDpi = (PFN_SetProcDpiCtx)
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (pSetDpi) pSetDpi((HANDLE)-4);   // PerMonitorV2
    pExtractIcons = (PFN_PrivateExtractIconsW)
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "PrivateExtractIconsW");

    HDC scr = GetDC(NULL);
    g_dpi = GetDeviceCaps(scr, LOGPIXELSX);
    ReleaseDC(NULL, scr);

    IniPath();
    ScaleMetrics();
    LoadConfig();

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

    g_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_LAYERED, APP_CLASS, APPNAME, WS_POPUP,
        g_savedX, g_savedY, g_savedW, g_savedH, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;

    ClampToWorkArea();
    ShowWindow(g_hwnd, SW_SHOW);
    ForceForeground(g_hwnd);
    g_alpha = 0;
    SetLayeredWindowAttributes(g_hwnd, 0, 0, LWA_ALPHA);
    g_fadeDir = 1;
    SetTimer(g_hwnd, TIMER_FADE, 12, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}