// zoomcount.c — 缩放时图标解析次数的确定性对比：插桩版把 IconFor 的调用计数
// 写进窗口标题（自绘标题栏，肉眼看不见），这里读出来。
// 用法：zoomcount <exe> [notches]
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define WM_APP_DRAGWAKE (WM_APP + 5)

static int ReadCount(HWND w)
{
    WCHAR t[64] = L"";
    GetWindowTextW(w, t, 64);
    const WCHAR *p = wcschr(t, L'=');
    return p ? _wtoi(p + 1) : -1;
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
    if (argc < 2) { printf("usage: zoomcount <exe> [n]\n"); return 2; }
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
    Sleep(3000);
    { WCHAR t0[64]=L""; GetWindowTextW(w,t0,64);
      printf("唤醒后 可见=%d 标题=[%ls]\n", (int)IsWindowVisible(w), t0); }
    int before = ReadCount(w);

    Key(VK_CONTROL, FALSE);
    Sleep(60);
    for (int i = 0; i < n; i++) {
        PostMessageW(w, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(r.left + 300, r.top + 300));
        Sleep(80);
    }
    Key(VK_CONTROL, TRUE);
    Sleep(2500);                                  // 等手势停稳后的重载 + 重绘
    int after = ReadCount(w);

    WCHAR raw[64] = L"";
    GetWindowTextW(w, raw, 64);
    printf("标题=[%ls] 缩放 %d 格：图标解析 %d → %d，本次手势新增 %d 次\n",
           raw, n, before, after, after - before);
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return 0;
}
