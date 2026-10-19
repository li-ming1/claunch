// mods.c — 模块表探针：拉起 claunch，静默托盘态枚举已加载模块（名字 + 映射体积），
// 再唤醒一次看唤醒要额外拖进什么。用来验证"COM/图标管线延迟到唤醒"这条红线，
// 以及 /DELAYLOAD 是否真的让 ole32 不在启动态出现。
// 用法：mods <claunch.exe> [tray 秒数]
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>

#define WM_APP_DRAGWAKE (WM_APP + 5)

static int Dump(const char *tag, HANDLE proc)
{
    HMODULE mods[256];
    DWORD need = 0;
    if (!EnumProcessModules(proc, mods, sizeof mods, &need)) {
        printf("%s: EnumProcessModules 失败 err=%lu\n", tag, GetLastError());
        return 0;
    }
    int n = (int)(need / sizeof(HMODULE));
    if (n > 256) n = 256;
    unsigned long long total = 0;
    int hasOle = 0;
    printf("--- %s: %d 个模块 ---\n", tag, n);
    for (int i = 0; i < n; i++) {
        WCHAR name[MAX_PATH] = L"?";
        GetModuleBaseNameW(proc, mods[i], name, MAX_PATH);
        MODULEINFO mi;
        if (GetModuleInformation(proc, mods[i], &mi, sizeof mi))
            total += mi.SizeOfImage;
        char ansi[MAX_PATH];
        WideCharToMultiByte(CP_ACP, 0, name, -1, ansi, MAX_PATH, NULL, NULL);
        if (_stricmp(ansi, "ole32.dll") == 0) hasOle = 1;
        printf("  %-24s\n", ansi);
    }
    printf("%s: 合计映射 %llu KB  ole32=%s\n", tag, total / 1024, hasOle ? "已加载" : "未加载");
    return hasOle;
}

int wmain(int argc, WCHAR **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: mods <exe> [tray_secs]\n"); return 2; }
    int secs = argc > 2 ? _wtoi(argv[2]) : 2;
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(argv[1], NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("FAIL start err=%lu\n", GetLastError()); return 2;
    }
    Sleep((DWORD)secs * 1000);
    int oleTray = Dump("托盘态", pi.hProcess);
    HWND w = FindWindowW(L"ClaunchLauncherWnd", NULL);
    if (w) {
        PostMessageW(w, WM_APP_DRAGWAKE, 0, 0);
        Sleep(1500);
        Dump("唤醒后", pi.hProcess);
    }
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return oleTray ? 1 : 0;      // 托盘态加载了 ole32 就返回 1
}
