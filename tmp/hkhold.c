// hkhold.c — 热键占用器：注册指定组合并保持 N 秒（验证 claunch 的冲突路径）
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
int wmain(int argc, WCHAR **argv)
{
    unsigned mods = argc >= 2 ? (unsigned)_wtoi(argv[1]) : 6;
    unsigned vk   = argc >= 3 ? (unsigned)_wtoi(argv[2]) : 114;
    int secs      = argc >= 4 ? _wtoi(argv[3]) : 5;
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"HkHoldWnd";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(0, L"HkHoldWnd", L"hold", WS_POPUP, 0, 0, 1, 1,
                             NULL, NULL, wc.hInstance, NULL);
    if (!RegisterHotKey(w, 7, mods | MOD_NOREPEAT, vk)) {
        printf("占用失败（组合本就空闲?）\n");
        return 1;
    }
    printf("已占用 mods=%u vk=%u 保持 %ds\n", mods, vk, secs);
    fflush(stdout);
    Sleep(secs * 1000);
    return 0;
}
