// killours.c — 只杀"我们自己"的 claunch 实例，绝不碰用户别的程序。
//
// 为什么需要它：taskkill /IM claunch.exe 与 tasklist /FI "IMAGENAME eq
// claunch.exe" 都是**大小写不敏感**的镜像名匹配——用户 concurrently 跑着的
// CLaunch.exe（D:\SmallTools\CLaunch_Ver.4.10_Chs\X64\CLaunch.exe）会被一起
// 杀掉。已经因此误杀过两次。
//
// 判定用两个都必须成立的条件：
//   1) 顶层窗口类名 == ClaunchLauncherWnd   （我们的窗口类，别的程序不会用）
//   2) 该进程的镜像全路径 == 本仓库的 claunch.exe（命令行传入，全路径比较）
// 两条都满足才 TerminateProcess。找不到就静默返回 0（"没有我们的实例"）。
//
// 用法: killours [exe 全路径]     缺省用仓库根的 ..\claunch.exe

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define OUR_CLASS L"ClaunchLauncherWnd"

#define UNREF(x) ((void)(x))
static WCHAR g_want[MAX_PATH];
static DWORD g_pid;

static BOOL SamePathI(const WCHAR *a, const WCHAR *b)
{
    for (; *a && *b; a++, b++) {
        WCHAR ca = *a, cb = *b;
        if (ca >= L'A' && ca <= L'Z') ca += 32;
        if (cb >= L'A' && cb <= L'Z') cb += 32;
        if (ca != cb) return FALSE;
    }
    return *a == 0 && *b == 0;
}

static BOOL CALLBACK EnumProc(HWND h, LPARAM lp)
{
    UNREF(lp);
    WCHAR cls[64] = L"";
    GetClassNameW(h, cls, 64);
    if (wcscmp(cls, OUR_CLASS) != 0) return TRUE;      // 不是我们的窗口类
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid || pid == g_pid) return TRUE;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return TRUE;
    WCHAR path[MAX_PATH];
    DWORD n = MAX_PATH;
    BOOL mine = QueryFullProcessImageNameW(p, 0, path, &n) &&
                SamePathI(path, g_want);
    CloseHandle(p);
    if (mine) g_pid = pid;                             // 记下最后一个匹配的
    return TRUE;
}

int wmain(int argc, WCHAR **argv)
{
    WCHAR want[MAX_PATH];
    if (argc > 1) {
        // 相对路径要先绝对化：QueryFullProcessImageName 给的是全路径
        GetFullPathNameW(argv[1], MAX_PATH, want, NULL);
    } else {
        // 缺省：本工具所在目录的上一级（仓库根）的 claunch.exe
        GetModuleFileNameW(NULL, want, MAX_PATH);
        WCHAR *slash = wcsrchr(want, L'\\');
        if (slash) *slash = 0;
        WCHAR *up = wcsrchr(want, L'\\');              // tmp\ → 仓库根
        if (up) *up = 0;
        wcscpy_s(g_want, MAX_PATH, want);
        WCHAR root[MAX_PATH];
        wcscpy_s(root, MAX_PATH, want);
        wcscat_s(root, MAX_PATH, L"\\claunch.exe");
        wcscpy_s(g_want, MAX_PATH, root);
        wcscpy_s(want, MAX_PATH, root);
    }
    wcscpy_s(g_want, MAX_PATH, want);

    // 多轮：每轮枚举+杀一个，最多 8 个实例
    for (int round = 0; round < 8; round++) {
        DWORD prev = g_pid;
        g_pid = 0;
        EnumWindows(EnumProc, 0);
        if (!g_pid || g_pid == prev) break;
        HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, g_pid);
        if (!p) break;
        wprintf(L"killours: 结束我们自己的实例 pid=%lu\n", g_pid);
        TerminateProcess(p, 0);
        WaitForSingleObject(p, 2000);
        CloseHandle(p);
    }
    return 0;
}
