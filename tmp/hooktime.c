// hooktime.c — 全局鼠标钩子的每事件成本：注入 N 个裸移动（不按键），
// 比较两个 exe 在钩子回调里花的用户态 CPU。这轮把 IsWindowVisible +
// GetAsyncKeyState 两次内核查询换成了本地按键镜像。
// 用法：hooktime <exe> [n]
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

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
    if (argc < 2) { printf("usage: hooktime <exe> [n]\n"); return 2; }
    int n = argc > 2 ? _wtoi(argv[2]) : 4000;
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(argv[1], NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("FAIL start err=%lu\n", GetLastError()); return 2;
    }
    Sleep(1500);
    HWND w = FindWindowW(L"ClaunchLauncherWnd", NULL);
    if (!w) { printf("FAIL: 无窗口\n"); TerminateProcess(pi.hProcess, 0); return 2; }
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);

    unsigned long long t0 = CpuMs(pi.hProcess);
    for (int i = 0; i < n; i++) {
        INPUT in;
        ZeroMemory(&in, sizeof in);
        in.type = INPUT_MOUSE;
        in.mi.dx = (i * 37 % sw) * 65535 / (sw - 1);
        in.mi.dy = (i * 53 % sh) * 65535 / (sh - 1);
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        SendInput(1, &in, sizeof in);
    }
    Sleep(1500);                       // 等钩子线程把队列吃完
    unsigned long long t1 = CpuMs(pi.hProcess);
    printf("%llums / %d 次移动 = %.3fms 每千次\n", t1 - t0, n, (double)(t1 - t0) * 1000.0 / n);

    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return 0;
}
