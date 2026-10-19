// dropchk.c — OLE 拖放注册验证：启动 claunch，先唤醒（WM_APP_DRAGWAKE：
// OLE 拖放目标已改为延迟到首次唤醒注册——静默托盘态不付 COM 的钱），
// 再查主窗的 "OleDropTargetInterface" 属性（RegisterDragDrop 的登记方式，
// 拖放源也靠它识别 OLE 目标），随后收掉进程
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>

#define WM_APP_DRAGWAKE (WM_APP + 5)

int wmain(int argc, WCHAR **argv)
{
    if (argc < 2) { printf("usage: dropchk <exe>\n"); return 2; }
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(argv[1], NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("FAIL start err=%lu\n", GetLastError()); return 2;
    }
    Sleep(1000);
    HWND w = FindWindowW(L"ClaunchLauncherWnd", NULL);
    if (w) PostMessageW(w, WM_APP_DRAGWAKE, 0, 0);   // 唤醒：触发 InitOleDrop 注册
    Sleep(500);                                      // 等 OLE 初始化 + 首绘完成
    w = FindWindowW(L"ClaunchLauncherWnd", NULL);
    int ok = 0, accept = 0;
    if (w) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == pi.dwProcessId) {
            LONG_PTR prop = (LONG_PTR)GetPropW(w, L"OleDropTargetInterface");
            ok = prop != 0;
            accept = (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_ACCEPTFILES) != 0;
            printf("window pid=%lu OleDropTargetInterface=%s WS_EX_ACCEPTFILES=%s\n",
                   pid, ok ? "SET" : "MISSING", accept ? "set" : "clear");
        } else {
            printf("FAIL: window belongs to pid=%lu (stale instance?)\n", pid);
        }
    } else {
        printf("FAIL: no window\n");
    }
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (!ok) return 1;
    printf("ok   OLE drop target registered\n");
    return 0;
}
