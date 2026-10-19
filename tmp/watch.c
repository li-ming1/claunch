// watch.c — 时间线监视：拉起 claunch，每 100ms 采样一次 WS/GDI/可见性，
// 打印跳变时刻与内容。定位静默启动后资源增长的发生点。
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>

int wmain(int argc, WCHAR **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: watch <claunch.exe> [秒数]\n"); return 2; }
    int secs = argc >= 3 ? _wtoi(argv[2]) : 3;

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(argv[1], NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("FAIL: start err=%lu\n", GetLastError());
        return 2;
    }
    CloseHandle(pi.hThread);
    DWORD prevGdi = 0, prevUsr = 0;
    SIZE_T prevWs = 0;
    for (int t = 0; t < secs * 10; t++) {
        PROCESS_MEMORY_COUNTERS mc;
        ZeroMemory(&mc, sizeof mc);
        mc.cb = sizeof mc;
        SIZE_T ws = 0;
        if (GetProcessMemoryInfo(pi.hProcess, &mc, sizeof mc)) ws = mc.WorkingSetSize;
        DWORD gdi = GetGuiResources(pi.hProcess, GR_GDIOBJECTS);
        DWORD usr = GetGuiResources(pi.hProcess, GR_USEROBJECTS);
        HWND w = FindWindowW(L"ClaunchLauncherWnd", NULL);
        BOOL vis = w && IsWindowVisible(w);
        if (gdi != prevGdi || usr != prevUsr || ws / 1024 != prevWs / 1024 || vis) {
            printf("t=%4dms ws=%luK gdi=%lu user=%lu vis=%d\n",
                   t * 100, (unsigned long)(ws / 1024), gdi, usr, vis ? 1 : 0);
            prevGdi = gdi; prevUsr = usr; prevWs = ws;
        }
        if (vis) { printf("(面板出现，停 1s 观察后续)\n"); Sleep(1000); break; }
        Sleep(100);
    }
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    printf("DONE\n");
    return 0;
}
