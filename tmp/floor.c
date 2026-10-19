// floor.c — 内存/启动地板价基准：一个"什么也不干"的托盘应用
// （注册类+建窗+托盘图标+消息循环，无 OLE/钩子/条目/图标链）。
// 与 claunch 的实测值（~3.6MB / 93ms）对比即知应用侧真实开销。
// 用法：floor.exe [驻留毫秒数，默认 1500]
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define PSAPI_VERSION 2
#include <windows.h>
#include <shellapi.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>

#define WM_APP_TRAY (WM_APP + 1)
#define TRAY_ID 1

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_APP_TRAY) return 0;
    return DefWindowProcW(h, m, w, l);
}

int wmain(int argc, WCHAR **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int dwell = argc >= 2 ? _wtoi(argv[1]) : 1500;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"FloorProbeWnd";
    RegisterClassExW(&wc);

    HWND w = CreateWindowExW(WS_EX_TOOLWINDOW, L"FloorProbeWnd", L"floor",
                             WS_POPUP, 100, 100, 200, 200, NULL, NULL,
                             GetModuleHandleW(NULL), NULL);
    if (!w) { printf("FAIL: window\n"); return 2; }

    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = w;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wcscpy_s(nid.szTip, 64, L"floor");
    Shell_NotifyIconW(NIM_ADD, &nid);

    Sleep(dwell);                            // 让外壳/工作集稳定

    PROCESS_MEMORY_COUNTERS mc;
    ZeroMemory(&mc, sizeof mc);
    mc.cb = sizeof mc;
    SIZE_T ws = 0;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &mc, sizeof mc))
        ws = mc.WorkingSetSize;
    DWORD gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    DWORD usr = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    printf("floor: ws=%luK gdi=%lu user=%lu\n",
           (unsigned long)(ws / 1024), gdi, usr);

    Shell_NotifyIconW(NIM_DELETE, &nid);
    DestroyWindow(w);
    return 0;
}
