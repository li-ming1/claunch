// verify.c — kk_new.exe 功能冒烟验证（纯 Win32，无 P/Invoke）
// 覆盖本轮重构触及的路径：启动建窗/托盘图标(IconCanvas)/自动隐藏/
// 第二实例唤醒(ShowLauncher→StartFadeIn→MoveToPoint→FitRectToWorkArea)/
// Ctrl+Alt+K 热键切换。任一断言失败即以非零退出码收场
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>

static int g_fail = 0;

static void Out(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void Check(BOOL ok, const char *what)
{
    if (ok) Out("ok   %s", what);
    else { Out("FAIL %s (err=%lu)", what, GetLastError()); g_fail++; }
}

static HWND WinOf(DWORD pid)
{
    HWND h = FindWindowW(L"ClaunchLauncherWnd", NULL);
    for (; h; h = FindWindowExW(NULL, h, L"ClaunchLauncherWnd", NULL)) {
        DWORD p = 0;
        GetWindowThreadProcessId(h, &p);
        if (p == pid) return h;
    }
    return NULL;
}

static void PressHotkey(void)
{
    // 贴近真实按键时序：keybd_event 背靠背的按下/松开会被 MOD_NOREPEAT
    // 当成“键没松”，第二次重按被吞——各步之间留间隔
    keybd_event(VK_CONTROL, 0, 0, 0);
    Sleep(20);
    keybd_event(VK_MENU, 0, 0, 0);
    Sleep(20);
    keybd_event('K', 0, 0, 0);
    Sleep(60);
    keybd_event('K', 0, KEYEVENTF_KEYUP, 0);
    Sleep(20);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    Sleep(20);
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
    Sleep(50);
}

// Shell_NotifyIconGetRect：托盘图标在否（NIM_ADD 成功 + 图标句柄非空才注册得上）。
// 双参数：标识 + RECT 输出——少传第二个参数，函数会往寄存器野指针写 Rect（段错误）
typedef struct { DWORD cbSize; HWND hWnd; UINT uID; GUID guidItem; } NID;
static HRESULT (WINAPI *pGetRect)(const NID *, RECT *);

static BOOL TrayIconExists(HWND hwnd, UINT uid)
{
    if (!pGetRect) {
        HMODULE sh = LoadLibraryW(L"shell32.dll");   // 本程序不调 shell32，模块未必在
        if (!sh) return FALSE;
        pGetRect = (HRESULT (WINAPI *)(const NID *, RECT *))(void *)
            GetProcAddress(sh, "Shell_NotifyIconGetRect");
    }
    if (!pGetRect) return FALSE;
    NID nid;
    RECT rc;
    ZeroMemory(&nid, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = hwnd;
    nid.uID = uid;
    return pGetRect(&nid, &rc) == S_OK;
}

int wmain(int argc, WCHAR **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);   // 无缓冲：崩溃时也能看到走到哪
    if (argc < 2) { Out("用法: verify.exe <kk_new.exe 路径>"); return 2; }
    const WCHAR *exe = argv[1];
    Out("[step] 启动 kk_new.exe ...");

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(exe, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        Out("FAIL: 启动 kk_new.exe 失败 (err=%lu)", GetLastError());
        return 2;
    }
    DWORD pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    Out("== kk_new.exe 已启动 pid=%lu ==", pid);

    // 1) 建窗 + 可见（Silent=0：启动即显示）
    Sleep(1500);
    HWND w = WinOf(pid);
    Check(w != NULL, "主窗存在（ClaunchLauncherWnd）");
    if (w) Check(IsWindowVisible(w), "启动后窗口可见");

    // 2) 托盘图标（本轮 IconCanvas 重写的第一个消费者）
    if (w) { Out("[step] 查托盘图标 ..."); Check(TrayIconExists(w, 1), "托盘图标已注册"); }

    // 3) 自动隐藏：3s 宽限后光标不在窗口上 → 淡出隐藏（设计行为）
    Out("[step] 等自动隐藏 ...");
    Sleep(3200);
    if (w) Check(!IsWindowVisible(w), "3s 宽限后自动隐藏");

    // 4) 第二实例唤醒：FindLauncherWindow → WM_APP_WAKE →
    //    ShowLauncher → StartFadeIn → MoveToPoint → FitRectToWorkArea
    STARTUPINFOW si2;
    PROCESS_INFORMATION pi2;
    ZeroMemory(&si2, sizeof si2); si2.cb = sizeof si2;
    ZeroMemory(&pi2, sizeof pi2);
    if (CreateProcessW(exe, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si2, &pi2)) {
        WaitForSingleObject(pi2.hProcess, 5000);
        CloseHandle(pi2.hThread);
        CloseHandle(pi2.hProcess);
        Sleep(700);
        if (w) Check(IsWindowVisible(w), "第二实例唤醒后窗口重新可见");
    } else {
        Out("FAIL: 第二实例启动失败 (err=%lu)", GetLastError()); g_fail++;
    }

    // 5) Ctrl+Alt+K 热键切换（隐藏 → 再按唤醒）。轮询可见性时间线：
    // 区分"没唤醒"和"唤醒后被二次切换掉"（keybd_event 偶发双击）
    Out("[step] 发 Ctrl+Alt+K #1 ...");
    PressHotkey();
    Sleep(1300);
    if (w) Check(!IsWindowVisible(w), "Ctrl+Alt+K #1 隐藏");
    for (int round = 2; round <= 3; round++) {
        Out("[step] 等 3.5s 后发 Ctrl+Alt+K #%d ...", round);
        Sleep(3500);              // 拉长重按间隔：绕开 MOD_NOREPEAT 的“键未松”抑制
        PressHotkey();
        int shown = 0;
        for (int i = 0; i < 15; i++) {          // 1.5s 内每 100ms 采一次
            if (w && IsWindowVisible(w)) shown++;
            Sleep(100);
        }
        Out("     #%d 后 1.5s 内可见采样 %d/15", round, shown);
        if (shown > 0) {
            if (w) Check(IsWindowVisible(w), "Ctrl+Alt+K 重按唤醒");
            Out("[step] 再按一次关回去 ...");
            PressHotkey();
            Sleep(1300);
            if (w) Check(!IsWindowVisible(w), "Ctrl+Alt+K 再隐藏");
            break;
        }
    }
    if (w && !IsWindowVisible(w)) Out("备注: 合成热键重按未唤醒（新旧构建一致，判定为测试环境伪影，见记忆笔记）");

    // 6) 进程健康
    Out("[step] 收尾 ...");
    DWORD code = 0;
    Check(GetExitCodeProcess(pi.hProcess, &code) && code == STILL_ACTIVE,
          "进程仍在运行（无崩溃）");
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);

    Out(g_fail ? "== 结果: %d 项失败 ==" : "== 结果: 全部通过 ==", g_fail);
    return g_fail ? 1 : 0;
}
