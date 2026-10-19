// kbtest.c — 键盘导航功能验证：唤醒面板 → 截图 → 发 End/Home/PageDown →
// 再截图，像素差分验证滚动与跳转真的发生（坏键=画面纹丝不动）。
// 用法：kbtest.exe <claunch.exe>
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define WM_APP_DRAGWAKE (WM_APP + 5)

static unsigned long long HashWnd(HWND w)
{
    HDC dc = GetWindowDC(w);
    if (!dc) return 0;
    RECT r;
    GetWindowRect(w, &r);
    int wdt = r.right - r.left, hgt = r.bottom - r.top;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, wdt, hgt);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(w, mem, 0);
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = wdt;
    bi.bmiHeader.biHeight = -hgt;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    BYTE *px = (BYTE *)malloc((size_t)wdt * hgt * 4);
    GetDIBits(mem, bmp, 0, hgt, px, &bi, DIB_RGB_COLORS);
    // FNV-1a 64 位：只对像素内容敏感
    unsigned long long h = 1469598103934665603ULL;
    for (int i = 0; i < wdt * hgt * 4; i += 7) {   // 每 7 字节采样：快且足够分辨
        h ^= px[i];
        h *= 1099511628211ULL;
    }
    free(px);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(w, dc);
    return h;
}

// 连拍取帧：两张相同才算画面静止（图标填充完成）。上限 20 次×100ms
static unsigned long long Settle(HWND w)
{
    unsigned long long a = HashWnd(w);
    for (int i = 0; i < 20; i++) {
        Sleep(100);
        unsigned long long b = HashWnd(w);
        if (b == a) return b;
        a = b;
    }
    return a;
}

int wmain(int argc, WCHAR **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: kbtest <claunch.exe>\n"); return 2; }
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(argv[1], NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("FAIL start err=%lu\n", GetLastError()); return 2;
    }
    Sleep(1000);
    HWND w = FindWindowW(L"ClaunchLauncherWnd", NULL);
    if (!w) { printf("FAIL: no window\n"); TerminateProcess(pi.hProcess, 0); return 2; }
    PostMessageW(w, WM_APP_DRAGWAKE, 0, 0);
    Sleep(900);                            // 等淡入+图标加载

    // 画面静止再取帧：图标是首绘时同步填充的，滚动还会揭示新格子、触发新
    // 一批加载，固定 sleep 会撞上填充时序——曾把"PageUp != Home"误判成导航
    // 回归。做法是连拍两张、相同才算稳，最多等 2s
    unsigned long long h0 = Settle(w);
    PostMessageW(w, WM_KEYDOWN, VK_END, 0);
    unsigned long long hEnd = Settle(w);
    PostMessageW(w, WM_KEYDOWN, VK_HOME, 0);
    unsigned long long hHome = Settle(w);
    PostMessageW(w, WM_KEYDOWN, VK_HOME, 0);      // 再按一次 Home：应幂等无变化
    unsigned long long hHome2 = Settle(w);
    PostMessageW(w, WM_KEYDOWN, VK_PRIOR, 0);     // 顶部再上翻：应无变化
    unsigned long long hPgUp = Settle(w);

    // 注意：初始态无悬停环，Home 后第一格出现焦点环——画面本就该和初始
    // 不同。断言的是行为语义：End 有位移、Home 离开底部、再 Home 幂等、
    // 顶部 PageUp 无副作用
    printf("初始=%016llX End=%016llX Home=%016llX Home2=%016llX 顶页PageUp=%016llX\n",
           h0, hEnd, hHome, hHome2, hPgUp);
    int ok = (hEnd != h0) && (hHome != hEnd) && (hHome2 == hHome) && (hPgUp == hHome2);
    printf("%s\n", ok ? "ok   End 有位移/Home 回顶部且幂等/顶页 PageUp 无副作用"
                      : "FAIL: 键盘导航与预期不符");
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return ok ? 0 : 1;
}
