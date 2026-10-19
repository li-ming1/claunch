// libtest.c — 新工具原语与旧实现的穷举对拍：wcpy 对 wcsncpy_s(_TRUNCATE)，
// wcat 对 wcscat_s，S_ 宏对 MulDiv。纯函数；结果写文件（无缓冲）。
//
// 注意：wcscat_s 溢出会走 CRT 的无效参数处理器直接终止进程（它没有"截断"
// 语义），所以 wcat 只在"装得下"的定义域内与它对比；截断域单独断言。
#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int g_dpi = 96;
#define S_(v) (((v) * g_dpi + 48) / 96)

static void wcpy(WCHAR *d, int cch, const WCHAR *s)
{
    int n = cch - 1;
    while (n > 0 && *s) { *d++ = *s++; n--; }
    *d = 0;
}

static void wcat(WCHAR *d, int cch, const WCHAR *s)
{
    int n = 0;
    while (n < cch - 1 && d[n]) n++;
    while (n < cch - 1 && *s) d[n++] = *s++;
    d[n] = 0;
}

static FILE *g_f;
static void OnBadParam(const wchar_t *e, const wchar_t *fn, const wchar_t *f,
                       unsigned line, uintptr_t p)
{
    (void)e; (void)fn; (void)f; (void)line; (void)p;
    fprintf(g_f, "FAIL 无效参数处理器被触发\n");
    fflush(g_f);
    ExitProcess(9);
}

int wmain(void)
{
    g_f = fopen("tmp\\libtest.txt", "wb");
    if (!g_f) return 2;
    _set_invalid_parameter_handler(OnBadParam);
    int bad = 0, nS = 0, nC = 0, nT = 0;

    // ---- S_ vs MulDiv：全部 DPI 档位 × 程序里出现的量级
    static const int dpis[] = { 96, 120, 144, 168, 192, 216, 240, 288, 336, 384, 432, 480 };
    for (int d = 0; d < (int)(sizeof dpis / sizeof *dpis); d++) {
        g_dpi = dpis[d];
        for (int v = 0; v <= 2000; v++) {
            int a = MulDiv(v, g_dpi, 96), b = S_(v);
            nS++;
            if (a != b) {
                if (bad < 10) fprintf(g_f, "DIFF S_: dpi=%d v=%d muldiv=%d macro=%d\n", g_dpi, v, a, b);
                bad++;
            }
        }
    }

    // ---- wcpy vs wcsncpy_s(_TRUNCATE)
    static const WCHAR *srcs[] = { L"", L"a", L"abc", L"abcdefghij", L"中文路径测试" };
    for (int si = 0; si < (int)(sizeof srcs / sizeof *srcs); si++)
        for (int cch = 1; cch <= 12; cch++) {
            WCHAR a[16], b[16];
            ZeroMemory(a, sizeof a); ZeroMemory(b, sizeof b);
            wcsncpy_s(a, (size_t)cch, srcs[si], _TRUNCATE);
            wcpy(b, cch, srcs[si]);
            nC++;
            if (wcscmp(a, b) != 0) {
                fprintf(g_f, "DIFF wcpy: src=[%ls] cch=%d old=[%ls] new=[%ls]\n", srcs[si], cch, a, b);
                bad++;
            }
        }

    // ---- wcat vs wcscat_s（仅"装得下"的定义域）
    for (int si = 0; si < (int)(sizeof srcs / sizeof *srcs); si++)
        for (int cch = 4; cch <= 12; cch++) {
            if ((int)wcslen(L"xy") + (int)wcslen(srcs[si]) >= cch) continue;   // 溢出域不测
            WCHAR a[16], b[16];
            wcscpy_s(a, 16, L"xy");
            wcscpy_s(b, 16, L"xy");
            wcscat_s(a, (size_t)cch, srcs[si]);
            wcat(b, cch, srcs[si]);
            nC++;
            if (wcscmp(a, b) != 0) {
                fprintf(g_f, "DIFF wcat: src=[%ls] cch=%d old=[%ls] new=[%ls]\n", srcs[si], cch, a, b);
                bad++;
            }
        }

    // ---- wcat 截断域：装不下就截断，且必带 NUL、绝不越界（旧 wcscat_s 会终止进程）
    {
        WCHAR buf[8];
        wcpy(buf, 8, L"xy");
        wcat(buf, 5, L"abcdefghij");       // 只剩 2 格
        nT++;
        if (wcscmp(buf, L"xyab") != 0) { fprintf(g_f, "FAIL wcat 截断: [%ls]\n", buf); bad++; }
        wcpy(buf, 8, L"1234567");
        wcat(buf, 8, L"9");                // 已满
        nT++;
        if (wcscmp(buf, L"1234567") != 0) { fprintf(g_f, "FAIL wcat 满缓冲: [%ls]\n", buf); bad++; }
        wcpy(buf, 1, L"abc");              // cch=1：只能放 NUL
        nT++;
        if (buf[0] != 0) { fprintf(g_f, "FAIL wcpy cch=1\n"); bad++; }
    }

    fprintf(g_f, "%s  S_:%d 例  wcpy/wcat:%d 例  截断:%d 例  差异:%d\n",
            bad ? "FAIL" : "ok", nS, nC, nT, bad);
    fclose(g_f);
    return bad ? 1 : 0;
}
