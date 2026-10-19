// probe2.c — 钉死变量的自动隐藏验证：启动 kk_new，1s 后强行把光标
// 移到窗口外（用户实时动鼠标会干扰心跳判定，这一步消除该变量），
// 之后只观察：3s 宽限 + 150ms 移出延迟后窗口应自动隐藏
#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>

static void Out(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

int wmain(int argc, WCHAR **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) return 2;
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(argv[1], NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        Out("启动失败 err=%lu", GetLastError()); return 2;
    }
    Sleep(1000);
    HWND w = FindWindowW(L"ClaunchLauncherWnd", NULL);
    if (!w) { Out("FAIL: 无窗口"); return 2; }
    RECT r;
    GetWindowRect(w, &r);
    POINT c = { r.right + 60, r.bottom + 60 };   // 窗口外固定点
    SetCursorPos(c.x, c.y);
    Out("光标已钉到窗口外 (%d,%d)，窗口=%d,%d %dx%d", c.x, c.y,
        r.left, r.top, r.right - r.left, r.bottom - r.top);
    int hidden = 0;
    for (int i = 0; i < 14; i++) {          // 7s 内应隐藏（3s 宽限 + 150ms）
        Sleep(200);
        SetCursorPos(c.x, c.y);             // 持续压制：用户动鼠标也无法把光标带回窗内
        if (!IsWindowVisible(w)) { hidden = 1; Out("T+%.1fs 已自动隐藏", 1.0 + (i + 1) * 0.2); break; }
    }
    Out(hidden ? "ok   自动隐藏正常（心跳无回归）" : "FAIL: 7s 未隐藏");
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return hidden ? 0 : 1;
}
