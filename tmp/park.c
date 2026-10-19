// park.c — 把光标钉到 (20,20)。像素比对（kbtest）必须钉死光标：
// 悬停格一变，高亮环和卡片位置就变，哈希随之不同。每次 kb 截图前跑一次
#include <windows.h>
int wmain(int c, wchar_t **v){ (void)c; (void)v; SetCursorPos(20,20); return 0; }
