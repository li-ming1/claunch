// hold.c — 让被测实例持续存活 N 秒（默认 5s）。沙箱里直接 & 起的进程会被
// 回收，探针拿不到窗口；用它撑住"实例在跑"这个前提。用完按 PID 收尾
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdlib.h>
int wmain(int c, wchar_t **v){ if(c<2) return 2; STARTUPINFOW si={0}; PROCESS_INFORMATION pi={0}; si.cb=sizeof si;
 if(!CreateProcessW(v[1],NULL,NULL,NULL,FALSE,0,NULL,NULL,&si,&pi)) return 3;
 Sleep(c>2?(DWORD)_wtoi(v[2]):5000); TerminateProcess(pi.hProcess,0); return 0; }
