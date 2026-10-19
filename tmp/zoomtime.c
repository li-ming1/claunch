// zoomtime.c — Ctrl+滚轮缩放的成本：按住 Ctrl 连发 N 次滚轮（每次格子尺寸 +4，
// 图标尺寸随之变化）。旧实现每格丢一次全部图标 → 下一帧重解析整屏；新实现
// 手势停稳 150ms 后才丢一次。
// 用法：zoomtime <exe> [notches]
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define WM_APP_DRAGWAKE (WM_APP + 5)

static unsigned long long CpuMs(HANDLE p)
{
    FILETIME c, e, k, u;
    if (!GetProcessTimes(p, &c, &e, &k, &u)) return 0;
    ULARGE_INTEGER a;
    a.LowPart = u.dwLowDateTime; a.HighPart = u.dwHighDateTime;
    return a.QuadPart / 10000ULL;
}

static void Key(WORD vk, BOOL up)
{
    INPUT in;
    ZeroMemory(&in, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof in);
}

int wmain(int argc, WCHAR **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: zoomtime <exe> [n]\n"); return 2; }
    int n = argc > 2 ? _wtoi(argv[2]) : 12;
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(argv[1], NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("FAIL start err=%lu\n", GetLastError()); return 2;
    }
    Sleep(1200);
    HWND w = FindWindowW(L"ClaunchLauncherWnd", NULL);
    if (!w) { printf("FAIL: 无窗口\n"); TerminateProcess(pi.hProcess, 0); return 2; }
    RECT r; GetWindowRect(w, &r);
    SetCursorPos(r.left + 300, r.top + 300);
    PostMessageW(w, WM_APP_DRAGWAKE, 0, 0);
    Sleep(3000);                                   // 图标全部就位

    unsigned long long t0 = CpuMs(pi.hProcess);
    Key(VK_CONTROL, FALSE);
    Sleep(60);
    for (int i = 0; i < n; i++) {
        PostMessageW(w, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(r.left + 300, r.top + 300));
        Sleep(80);                                 // 人类连续拨动的节奏
    }
    Key(VK_CONTROL, TRUE);
    Sleep(3000);                                   // 等图标重载 + 重绘收尾
    unsigned long long t1 = CpuMs(pi.hProcess);
    printf("缩放 %d 格: %llums = %.1fms 每格\n", n, t1 - t0, (double)(t1 - t0) / n);

    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return 0;
}
