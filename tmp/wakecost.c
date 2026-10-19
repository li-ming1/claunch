// wakecost.c — 唤醒成本拆解：首次唤醒（图标全部要解析）vs 再次唤醒（图标已缓存）。
// 差值就是图标链的一次性开销。
// 用法：wakecost <exe>
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>

#define WM_APP_DRAGWAKE (WM_APP + 5)

static unsigned long long CpuMs(HANDLE p)
{
    FILETIME c, e, k, u;
    if (!GetProcessTimes(p, &c, &e, &k, &u)) return 0;
    ULARGE_INTEGER a;
    a.LowPart = u.dwLowDateTime; a.HighPart = u.dwHighDateTime;
    return a.QuadPart / 10000ULL;
}

int wmain(int argc, WCHAR **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: wakecost <exe>\n"); return 2; }
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(argv[1], NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("FAIL start err=%lu\n", GetLastError()); return 2;
    }
    Sleep(1500);
    HWND w = FindWindowW(L"ClaunchLauncherWnd", NULL);
    if (!w) { printf("FAIL: 无窗口\n"); TerminateProcess(pi.hProcess, 0); return 2; }
    RECT r; GetWindowRect(w, &r);
    SetCursorPos(r.left + 200, r.top + 200);       // 光标留在面板内，避免自动隐藏

    unsigned long long t0 = CpuMs(pi.hProcess);
    PostMessageW(w, WM_APP_DRAGWAKE, 0, 0);
    Sleep(3000);
    unsigned long long t1 = CpuMs(pi.hProcess);
    printf("首次唤醒（含图标解析）: %llums\n", t1 - t0);

    // 收起再唤醒：图标已缓存，只走淡入 + 首帧
    PostMessageW(w, WM_CLOSE, 0, 0);               // CloseToTray → 淡出隐藏
    Sleep(1500);
    SetCursorPos(r.left + 200, r.top + 200);
    unsigned long long t2 = CpuMs(pi.hProcess);
    PostMessageW(w, WM_APP_DRAGWAKE, 0, 0);
    Sleep(3000);
    unsigned long long t3 = CpuMs(pi.hProcess);
    printf("再次唤醒（图标已缓存）: %llums\n", t3 - t2);
    printf("图标链一次性开销 ≈ %lldms\n", (long long)((t1 - t0) - (t3 - t2)));

    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return 0;
}
