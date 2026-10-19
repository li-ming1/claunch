// soak.c — 句柄/工作集稳定性探针：拉起 kk → 循环（WM_APP_DRAGWAKE 唤醒
// [SW_SHOWNA 不抢前台] → WM_CLOSE 隐藏到托盘）→ 每轮记录 GDI/USER 句柄与
// 工作集。每轮唤醒都触发 RefreshVirtualIcons（图标换缓存路径）——正是
// 泄漏嫌疑最大的新增路径。热身首轮后序列平稳=无泄漏；线性增长=泄漏。
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>

#define WM_APP_DRAGWAKE (WM_APP + 5)

int wmain(int argc, WCHAR **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: soak <claunch.exe> [rounds]\n"); return 2; }
    int rounds = argc >= 3 ? _wtoi(argv[2]) : 24;
    if (rounds < 4) rounds = 24;

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(argv[1], NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("FAIL: start err=%lu\n", GetLastError());
        return 2;
    }
    CloseHandle(pi.hThread);
    Sleep(1600);                            // 首启建窗+托盘+首屏图标

    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pi.dwProcessId);
    if (!proc) { printf("FAIL: open process\n"); TerminateProcess(pi.hProcess, 0); return 2; }

    HWND w = NULL;
    for (int r = 0; r < rounds; r++) {
        w = FindWindowW(L"ClaunchLauncherWnd", NULL);
        if (w) PostMessageW(w, WM_APP_DRAGWAKE, 0, 0);   // 唤醒（隐藏时才生效）
        Sleep(750);
        DWORD gdi = GetGuiResources(proc, GR_GDIOBJECTS);
        DWORD usr = GetGuiResources(proc, GR_USEROBJECTS);
        PROCESS_MEMORY_COUNTERS mc;
        ZeroMemory(&mc, sizeof mc);
        mc.cb = sizeof mc;
        SIZE_T ws = 0;
        if (GetProcessMemoryInfo(proc, &mc, sizeof mc)) ws = mc.WorkingSetSize;
        printf("r%02d gdi=%lu user=%lu ws=%luK\n", r + 1, gdi, usr, (unsigned long)(ws / 1024));
        if (w) PostMessageW(w, WM_CLOSE, 0, 0);          // 隐藏到托盘
        Sleep(550);
    }
    // 稳定性判定：去掉首轮热身后，首尾窗口均值漂移超 15% 判疑似泄漏
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(proc);
    CloseHandle(pi.hProcess);
    printf("DONE\n");
    return 0;
}
