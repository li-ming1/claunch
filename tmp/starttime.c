// starttime.c — 启动计时探针：CreateProcess(claunch.exe) → WaitForInputIdle
// （首窗就绪=CreateWindow+WM_CREATE 含托盘/钩子/热键全部完成）。
// 静默启动模式下这就是"点击到托盘可用"的墙钟时间。多轮取最小/均值。
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>

// 托盘就绪瞬间的资源快照（与 floor.c 同一测量点，可比）
static DWORD s_modSeed = 0;

static void Snapshot(HANDLE proc, int diffMode)
{
    PROCESS_MEMORY_COUNTERS mc;
    ZeroMemory(&mc, sizeof mc);
    mc.cb = sizeof mc;
    SIZE_T ws = 0;
    if (GetProcessMemoryInfo(proc, &mc, sizeof mc)) ws = mc.WorkingSetSize;
    DWORD gdi = GetGuiResources(proc, GR_GDIOBJECTS);
    DWORD usr = GetGuiResources(proc, GR_USEROBJECTS);
    HWND w = FindWindowW(L"ClaunchLauncherWnd", NULL);
    printf("  ws=%luK gdi=%lu user=%lu 面板%s\n",
           (unsigned long)(ws / 1024), gdi, usr,
           (w && IsWindowVisible(w)) ? "可见!!" : "隐藏");
    // 模块清单 diff：新加载的 DLL 逐个报（揭开工作集增长的来源）
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetProcessId(proc));
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me;
    me.dwSize = sizeof me;
    DWORD seen = 0;
    if (Module32FirstW(snap, &me)) do {
        seen++;
        if (!diffMode || (int)seen <= (int)s_modSeed) continue;   // 非 diff 模式/已知模块：不报
        printf("    +module %ls\n", me.szModule);
    } while (Module32NextW(snap, &me));
    s_modSeed = seen;
    CloseHandle(snap);
}

int wmain(int argc, WCHAR **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: starttime <claunch.exe> [rounds]\n"); return 2; }
    int rounds = argc >= 3 ? _wtoi(argv[2]) : 5;
    if (rounds < 1) rounds = 5;

    double total = 0;
    DWORD best = 0xFFFFFFFF;
    for (int i = 0; i < rounds; i++) {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof si); si.cb = sizeof si;
        ZeroMemory(&pi, sizeof pi);
        DWORD t0 = GetTickCount();
        if (!CreateProcessW(argv[1], NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            printf("FAIL: start err=%lu\n", GetLastError());
            return 2;
        }
        WaitForInputIdle(pi.hProcess, 5000);
        s_modSeed = 0;                       // 每个子进程重置基线
        Snapshot(pi.hProcess, 0);
        DWORD dt = GetTickCount() - t0;
        if (dt < best) best = dt;
        total += dt;
        printf("round %d: %lums\n", i + 1, dt);
        Sleep(600);                       // 等内核工作集管理器完成异步裁剪
        Snapshot(pi.hProcess, 1);         // diff 模式：只报这 600ms 新加载的 DLL
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Sleep(150);                            // 让托盘/句柄干净回收再跑下一轮
    }
    printf("min=%lums avg=%.1fms\n", best, total / rounds);
    return 0;
}
