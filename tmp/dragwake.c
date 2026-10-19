// dragwake.c — 拖放唤醒端到端验证：面板隐藏时按住左键把光标从区外拖进面板，
// 面板应就地弹出（SW_SHOWNA，不抢前台）。这轮把钩子里的 GetAsyncKeyState 换成了
// 本地按键镜像，这里是它唯一的行为验证点。
// 用法：dragwake <exe>
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static int g_fail;

static void Check(BOOL ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

static void Mouse(DWORD flags, int x, int y)
{
    INPUT in;
    ZeroMemory(&in, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dx = x;
    in.mi.dy = y;
    in.mi.dwFlags = flags;
    SendInput(1, &in, sizeof in);
}

// 绝对坐标移动必须走 SendInput：SetCursorPos 只挪光标，不产生输入事件，
// 全局钩子看不到——拖放唤醒自然测不出来
static void MoveTo(int x, int y)
{
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    Mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE,
          x * 65535 / (sw - 1), y * 65535 / (sh - 1));
}

int wmain(int argc, WCHAR **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: dragwake <exe>\n"); return 2; }
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(argv[1], NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("FAIL start err=%lu\n", GetLastError()); return 2;
    }
    Sleep(1500);
    HWND w = FindWindowW(L"ClaunchLauncherWnd", NULL);
    if (!w) { printf("FAIL: 无窗口\n"); TerminateProcess(pi.hProcess, 0); return 2; }
    Check(!IsWindowVisible(w), "启动后面板隐藏（静默托盘态）");

    RECT r;
    GetWindowRect(w, &r);
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;

    // 从面板外按下左键，再分几步拖进面板中心
    SetCursorPos(r.left - 120, r.top - 80);
    Sleep(200);
    Mouse(MOUSEEVENTF_LEFTDOWN, 0, 0);
    Sleep(120);
    for (int i = 1; i <= 6; i++) {
        MoveTo(r.left - 120 + (cx - (r.left - 120)) * i / 6,
               r.top - 80 + (cy - (r.top - 80)) * i / 6);
        Sleep(120);
    }
    Sleep(600);
    Check(IsWindowVisible(w), "按住左键拖入面板 → 面板就地弹出");
    Mouse(MOUSEEVENTF_LEFTUP, 0, 0);
    Sleep(300);

    // 松开后（不再有按键）把光标移到面板外：不应再有拖放唤醒，且应自动隐藏
    MoveTo(r.left - 400, r.top - 300);
    Sleep(4500);        // 唤醒有 3s 宽限 + 150ms 移出延迟：等够再断言
    Check(!IsWindowVisible(w), "松开按键后移出 → 自动隐藏");

    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    printf("%s\n", g_fail ? "== 有失败项 ==" : "== 全部通过 ==");
    return g_fail ? 1 : 0;
}
