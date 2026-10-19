// traytip4.c — claunch.exe 托盘悬停提示验证（差分检测，兼容 Win11 XAML 弹层）：
// 启动 kk → 悬停其托盘图标 → 快照差分找新弹出的可见窗口。
// 用法: traytip4.exe <kk 路径>
#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>

#define UNREF(x) ((void)(x))
#define MAXW 512

static void Out(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

typedef struct { DWORD cbSize; HWND hWnd; UINT uID; GUID guidItem; } NID;
static HRESULT (WINAPI *pGetRect)(const NID *, RECT *);

static BOOL TrayRect(HWND hwnd, RECT *rc)
{
    if (!pGetRect) {
        HMODULE sh = LoadLibraryW(L"shell32.dll");
        if (!sh) return FALSE;
        pGetRect = (HRESULT (WINAPI *)(const NID *, RECT *))(void *)
            GetProcAddress(sh, "Shell_NotifyIconGetRect");
    }
    if (!pGetRect) return FALSE;
    NID nid;
    ZeroMemory(&nid, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = hwnd;
    nid.uID = 1;
    return pGetRect(&nid, rc) == S_OK;
}

// 位置判据检测：用户鼠标可能悬在托盘区让弹层常驻（差分快照会失效），
// 改为直接找“位于托盘图标上方的 XAML 弹层”。图标中心用全局传进去
static int g_iconCx, g_iconCy;
static HWND g_tipFound;

static BOOL CALLBACK TipProc(HWND h, LPARAM lp)
{
    UNREF(lp);
    WCHAR cls[128] = L"";
    GetClassNameW(h, cls, 128);
    if (wcscmp(cls, L"Xaml_WindowedPopupClass") != 0) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    RECT wr;
    GetWindowRect(h, &wr);
    int mx = (wr.left + wr.right) / 2, my = (wr.top + wr.bottom) / 2;
    if (abs(mx - g_iconCx) < 300 && abs(my - g_iconCy) < 300) {
        g_tipFound = h;
        Out("  [悬停弹层] hwnd=%p rect=%d,%d %dx%d", (void*)h,
            wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top);
    }
    return TRUE;
}

static HWND FindTip(BOOL verbose)
{
    g_tipFound = NULL;
    EnumWindows(TipProc, 0);
    if (g_tipFound && verbose) {
        RECT wr;
        GetWindowRect(g_tipFound, &wr);
        Out("  [悬停弹层] hwnd=%p rect=%d,%d %dx%d", (void*)g_tipFound,
            wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top);
    }
    return g_tipFound;
}

int wmain(int argc, WCHAR **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { Out("用法: traytip4.exe <kk 路径>"); return 2; }
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(argv[1], NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        Out("FAIL: 启动失败 err=%lu", GetLastError()); return 2;
    }
    Sleep(1800);

    HWND w = FindWindowW(L"ClaunchLauncherWnd", NULL);
    Out("主窗=%p", (void*)w);
    RECT rc;
    if (!w || !TrayRect(w, &rc)) { Out("FAIL: 托盘图标未注册"); goto done; }
    int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2;
    Out("托盘图标 rect=%d,%d %dx%d 中心=(%d,%d)",
        rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, cx, cy);

    g_iconCx = cx; g_iconCy = cy;

    SetCursorPos(cx, cy);
    mouse_event(MOUSEEVENTF_MOVE, 2, 0, 0, 0);
    mouse_event(MOUSEEVENTF_MOVE, (DWORD)-2, 0, 0, 0);

    HWND tip = NULL;
    for (int i = 0; i < 80 && !tip; i++) {   // 8s：用户实时动鼠标时延迟更大
        Sleep(100);
        tip = FindTip(TRUE);
    }
    Out(tip ? "ok   悬停托盘图标有提示弹出" : "FAIL: 悬停无提示弹出");

    // 闭环验证：移开光标提示应消失，再悬停应重现（工具提示生命周期特征）
    if (tip) {
        SetCursorPos(cx + 500, cy - 300);   // 移开
        int gone = 0;
        for (int i = 0; i < 20 && !gone; i++) {
            Sleep(100);
            if (!IsWindow(tip) || !FindTip(FALSE)) gone = 1;
        }
        Out(gone ? "ok   移开光标后提示消失" : "FAIL: 移开光标后提示仍在");
        Sleep(1500);                      // XAML 弹层有重显示冷却：刚移开就回去不弹
        SetCursorPos(cx, cy);               // 再悬停
        mouse_event(MOUSEEVENTF_MOVE, 2, 0, 0, 0);
        mouse_event(MOUSEEVENTF_MOVE, (DWORD)-2, 0, 0, 0);
        HWND tip2 = NULL;
        for (int i = 0; i < 60 && !tip2; i++) {   // 6s
            Sleep(100);
            tip2 = FindTip(TRUE);
        }
        Out(tip2 ? "ok   再次悬停提示重现" : "FAIL: 再次悬停无提示");
    }

done:
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
}
