// quit.c — 给面板发 WM_CLOSE，触发 CloseToTray→BeginHide→淡出→保存。
// 配置写出回归用：退出前后 cmp claunch.ini，必须逐字节一致
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
int wmain(int c, wchar_t **v){ (void)c;(void)v; HWND w=FindWindowW(L"ClaunchLauncherWnd",NULL); if(!w) return 2; PostMessageW(w, WM_CLOSE, 0, 0); return 0; }
