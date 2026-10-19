// wheel2.c — 绘制热路径实测（可滚动压测床：128 条目 + Cell=72）：
//   A) 滚轮：每次整格区重绘（内容整体位移一行）
//   B) 鼠标横扫：每次 1~2 格重绘 + 标题 + 卡片
// 用进程用户态 CPU 增量算每次操作的成本。绝对值随机器变，看相对量。
// 用法：wheel2 <exe> [次数]
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define WM_APP_DRAGWAKE (WM_APP + 5)

static unsigned long long HashWnd(HWND w)
{
    HDC dc = GetWindowDC(w);
    if (!dc) return 0;
    RECT r; GetWindowRect(w, &r);
    int wd = r.right - r.left, ht = r.bottom - r.top;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, wd, ht);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(w, mem, 0);
    BITMAPINFO bi; ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = wd; bi.bmiHeader.biHeight = -ht;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    BYTE *px = (BYTE *)malloc((size_t)wd * ht * 4);
    GetDIBits(mem, bmp, 0, ht, px, &bi, DIB_RGB_COLORS);
    unsigned long long h = 1469598103934665603ULL;
    for (int i = 0; i < wd * ht * 4; i += 7) { h ^= px[i]; h *= 1099511628211ULL; }
    free(px);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(w, dc);
    return h;
}

static unsigned long long CpuMs(HANDLE p)
{
    FILETIME c, e, k, u;
    if (!GetProcessTimes(p, &c, &e, &k, &u)) return 0;
    ULARGE_INTEGER a;
    a.LowPart = u.dwLowDateTime; a.HighPart = u.dwHighDateTime;
    return a.QuadPart / 10000ULL;
}

// 探针自己也要 DPI-aware：否则 GetWindowRect 拿到的是虚拟化坐标，
// 注入点与光标钉位全差一个缩放系数
static void MakeDpiAware(void)
{
    BOOL (WINAPI *pCtx)(HANDLE) = (BOOL (WINAPI *)(HANDLE))(void *)
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (pCtx && pCtx((HANDLE)-4)) return;
    SetProcessDPIAware();
}

int wmain(int argc, WCHAR **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    MakeDpiAware();
    if (argc < 2) { printf("usage: wheel2 <exe> [n]\n"); return 2; }
    int n = argc > 2 ? _wtoi(argv[2]) : 200;
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(argv[1], NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("FAIL start err=%lu\n", GetLastError()); return 2;
    }
    Sleep(1200);
    HWND w = FindWindowW(L"ClaunchLauncherWnd", NULL);
    if (!w) { printf("FAIL: no window\n"); TerminateProcess(pi.hProcess, 0); return 2; }
    PostMessageW(w, WM_APP_DRAGWAKE, 0, 0);
    Sleep(1500);
    RECT r;
    GetWindowRect(w, &r);
    SetCursorPos(r.left + 300, r.top + 300);      // 光标钉在面板内：不触发自动隐藏
    Sleep(1500);                                  // 淡入 + 图标就位
    GetWindowRect(w, &r);
    int sx = r.left + 300, sy = r.top + 300;      // 落在格子区（侧栏 84、标题 34 之内）
    printf("窗口 %ld,%ld %ldx%ld  注入点 %d,%d\n", r.left, r.top,
           r.right - r.left, r.bottom - r.top, sx, sy);

    DWORD ec = 0; GetExitCodeProcess(pi.hProcess, &ec);
    DWORD wpid = 0; GetWindowThreadProcessId(w, &wpid);
    printf("可见=%d 前台=%d 子进程活着=%d 窗口pid=%lu 子进程pid=%lu\n",
           (int)IsWindowVisible(w), (int)(GetForegroundWindow() == w),
           ec == STILL_ACTIVE, wpid, pi.dwProcessId);
    // A) 滚轮：一次下滚 = 整格区重绘
    unsigned long long h0 = HashWnd(w);
    unsigned long long t0 = CpuMs(pi.hProcess);
    for (int i = 0; i < n; i++)
        PostMessageW(w, WM_MOUSEWHEEL, MAKEWPARAM(0, (WORD)(-WHEEL_DELTA)), MAKELPARAM(sx, sy));
    Sleep(3000);
    unsigned long long t1 = CpuMs(pi.hProcess);
    printf("滚轮前后画面: %016llX -> %016llX\n", h0, HashWnd(w));
    printf("滚轮: %llums / %d 次 = %.2fms 每次整格区重绘 (可见=%d)\n",
           t1 - t0, n, (double)(t1 - t0) / n, (int)IsWindowVisible(w));

    // B) 鼠标横扫同一行：每次悬停换格（部分重绘 + 卡片重锚）
    t0 = CpuMs(pi.hProcess);
    for (int i = 0; i < n; i++) {
        int cx = r.left + 100 + (i % 8) * 80;
        PostMessageW(w, WM_MOUSEMOVE, 0, MAKELPARAM(cx - r.left, 60 + 40));
    }
    Sleep(3000);
    t1 = CpuMs(pi.hProcess);
    printf("悬停: %llums / %d 次 = %.2fms 每次换格\n", t1 - t0, n, (double)(t1 - t0) / n);

    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return 0;
}
