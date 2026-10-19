// fmtfmt.c — wfmt/wToI/wScanU2/wScanI4 与 libc swprintf/_wtoi/swscanf 的穷举对拍。
// 结果写文件（无缓冲），避开控制台 orientation 干扰。
#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

// ---- 被测实现（与 claunch.c 逐字一致）----
static WCHAR *wNum(WCHAR *d, WCHAR *e, unsigned long long v, int base, int width, BOOL upper)
{
    WCHAR tmp[24];
    int n = 0;
    const WCHAR *dig = upper ? L"0123456789ABCDEF" : L"0123456789abcdef";
    do {
        tmp[n++] = dig[v % (unsigned)base];
        v /= (unsigned)base;
    } while (v);
    while (n < width && n < 24) tmp[n++] = L'0';
    while (n > 0 && d < e) *d++ = tmp[--n];
    return d;
}

static WCHAR *wStr(WCHAR *d, WCHAR *e, const WCHAR *s)
{
    while (d < e && *s) *d++ = *s++;
    return d;
}

static int wfmtv(WCHAR *dst, int cch, const WCHAR *fmt, va_list ap)
{
    WCHAR *d = dst, *e = dst + cch - 1;
    for (const WCHAR *p = fmt; *p; p++) {
        if (*p != L'%') { if (d < e) *d++ = *p; continue; }
        if (p[1] == L'%') { if (d < e) *d++ = L'%'; p++; continue; }
        p++;
        int width = 0;
        while (*p >= L'0' && *p <= L'9') width = width * 10 + (*p++ - L'0');
        while (*p == L'l' || *p == L'h') p++;
        switch (*p) {
        case L's': d = wStr(d, e, va_arg(ap, const WCHAR *)); break;
        case L'd': {
            long long v = va_arg(ap, int);
            if (v < 0) { if (d < e) *d++ = L'-'; v = -v; }
            d = wNum(d, e, (unsigned long long)v, 10, width, FALSE);
            break;
        }
        case L'u': d = wNum(d, e, va_arg(ap, unsigned), 10, width, FALSE); break;
        case L'X': d = wNum(d, e, va_arg(ap, unsigned long long), 16, width, TRUE); break;
        default:   if (d < e) *d++ = L'%';
                   if (d < e && *p) *d++ = *p;
                   break;
        }
    }
    *d = 0;
    return (int)(d - dst);
}

static int wfmt(WCHAR *dst, int cch, const WCHAR *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = wfmtv(dst, cch, fmt, ap);
    va_end(ap);
    return n;
}

static int wToI(const WCHAR *s)
{
    while (*s == L' ' || *s == L'\t') s++;
    BOOL neg = (*s == L'-');
    if (neg || *s == L'+') s++;
    int v = 0;
    while (*s >= L'0' && *s <= L'9') v = v * 10 + (int)(*s++ - L'0');
    return neg ? -v : v;
}

static int wScanU2(const WCHAR *s, unsigned *a, unsigned *b)
{
    unsigned *out[2] = { a, b };
    int n = 0;
    while (n < 2) {
        while (*s == L' ' || *s == L'\t') s++;
        if (*s < L'0' || *s > L'9') break;
        unsigned v = 0;
        while (*s >= L'0' && *s <= L'9') v = v * 10 + (unsigned)(*s++ - L'0');
        *out[n++] = v;
    }
    return n;
}

static int wScanI4(const WCHAR *s, int *a, int *b, int *c, int *d)
{
    int *out[4] = { a, b, c, d };
    int n = 0;
    while (n < 4) {
        while (*s == L' ' || *s == L'\t') s++;
        BOOL neg = (*s == L'-');
        if (neg || *s == L'+') s++;
        if (*s < L'0' || *s > L'9') break;
        int v = 0;
        while (*s >= L'0' && *s <= L'9') v = v * 10 + (int)(*s++ - L'0');
        *out[n++] = neg ? -v : v;
    }
    return n;
}

static FILE *g_f;
static int g_bad, g_cases;

static void CmpS(const WCHAR *fmt, const WCHAR *got, const WCHAR *want, const WCHAR *what)
{
    g_cases++;
    if (wcscmp(got, want) != 0) {
        if (g_bad < 12) fprintf(g_f, "DIFF %ls fmt=[%ls] got=[%ls] want=[%ls]\n", what, fmt, got, want);
        g_bad++;
    }
}

int wmain(void)
{
    g_f = fopen("tmp\\fmtfmt.txt", "wb");
    if (!g_f) return 2;
    WCHAR a[64], b[64];

    // ---- %d ----
    static const int ints[] = { 0, 1, 7, 10, 99, 100, -1, -7, -100, 2147483647, -2147483647, 65535 };
    for (int i = 0; i < (int)(sizeof ints / sizeof *ints); i++) {
        wfmt(a, 64, L"%d", ints[i]);
        swprintf(b, 64, L"%d", ints[i]);
        CmpS(L"%d", a, b, L"int");
    }
    // ---- %u ----
    static const unsigned us[] = { 0, 1, 42, 4294967295u, 3000000000u };
    for (int i = 0; i < (int)(sizeof us / sizeof *us); i++) {
        wfmt(a, 64, L"%u", us[i]);
        swprintf(b, 64, L"%u", us[i]);
        CmpS(L"%u", a, b, L"uint");
    }
    // ---- %02X / %02llX：0..0xFFFF 全覆盖 ----
    for (unsigned v = 0; v <= 0xFFFF; v++) {
        wfmt(a, 64, L"0x%02llX", (unsigned long long)v);
        swprintf(b, 64, L"0x%02X", v);
        CmpS(L"0x%02X", a, b, L"hex");
    }
    // ---- %s 与组合 ----
    static const WCHAR *strs[] = { L"", L"a", L"记事本", L"meliora", L"带空格 的 名字" };
    for (int i = 0; i < (int)(sizeof strs / sizeof *strs); i++) {
        wfmt(a, 64, L"%s", strs[i]);         swprintf(b, 64, L"%s", strs[i]);
        CmpS(L"%s", a, b, L"str");
        wfmt(a, 64, L"搜索: %s (%d·全部分组)", strs[i], 7);
        swprintf(b, 64, L"搜索: %s (%d·全部分组)", strs[i], 7);
        CmpS(L"搜索", a, b, L"组合");
        wfmt(a, 64, L"%s · %s", strs[i], L"常用");
        swprintf(b, 64, L"%s · %s", strs[i], L"常用");
        CmpS(L"%s · %s", a, b, L"组合");
    }
    // ---- 与配置行同形的整行 ----
    wfmt(a, 64, L"I%d=%s|%s|%d|%s|%s", 3, L"名", L"C:\\a\\b.exe", 2, L"", L"");
    swprintf(b, 64, L"I%d=%s|%s|%d|%s|%s", 3, L"名", L"C:\\a\\b.exe", 2, L"", L"");
    CmpS(L"配置行", a, b, L"整行");
    wfmt(a, 64, L"Rect=%d %d %d %d", 770, 333, 842, 631);
    swprintf(b, 64, L"Rect=%d %d %d %d", 770, 333, 842, 631);
    CmpS(L"Rect", a, b, L"整行");

    // ---- 截断：必须 NUL 收尾且不超过 cch-1 ----
    for (int cch = 1; cch <= 10; cch++) {
        ZeroMemory(a, sizeof a);
        int n = wfmt(a, cch, L"%s", L"abcdefghijklmn");
        g_cases++;
        if (n > cch - 1 || a[cch - 1] != 0 || (int)wcslen(a) != n) {
            fprintf(g_f, "FAIL 截断 cch=%d n=%d len=%d\n", cch, n, (int)wcslen(a));
            g_bad++;
        }
    }

    // ---- wToI vs _wtoi ----
    static const WCHAR *nums[] = { L"0", L"42", L"-7", L"+15", L"  99", L"123abc", L"abc", L"", L"-", L"2147483647" };
    for (int i = 0; i < (int)(sizeof nums / sizeof *nums); i++) {
        g_cases++;
        if (wToI(nums[i]) != _wtoi(nums[i])) {
            fprintf(g_f, "DIFF wToI [%ls] got=%d want=%d\n", nums[i], wToI(nums[i]), _wtoi(nums[i]));
            g_bad++;
        }
    }

    // ---- wScanU2 vs swscanf("%u %u") ----
    static const WCHAR *pairs[] = { L"3 70", L"3  70", L" 3 70", L"3", L"", L"x y", L"3 70 99" };
    for (int i = 0; i < (int)(sizeof pairs / sizeof *pairs); i++) {
        unsigned x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        int n1 = wScanU2(pairs[i], &x1, &y1);
        int n2 = swscanf(pairs[i], L"%u %u", &x2, &y2);
        if (n2 < 0) n2 = 0;      // 空串：swscanf 返回 EOF(-1)，本实现返回 0；
                                 // 调用点只判 ==2，语义等价
        g_cases++;
        if (n1 != n2 || (n1 >= 1 && x1 != x2) || (n1 >= 2 && y1 != y2)) {
            fprintf(g_f, "DIFF wScanU2 [%ls] n=%d/%d\n", pairs[i], n1, n2);
            g_bad++;
        }
    }

    // ---- wScanI4 vs swscanf("%d %d %d %d") ----
    static const WCHAR *quads[] = { L"770 333 842 631", L"-10 -20 800 600", L"1 2 3", L"", L"a b c d" };
    for (int i = 0; i < (int)(sizeof quads / sizeof *quads); i++) {
        int p1[4] = { 0 }, p2[4] = { 0 };
        int n1 = wScanI4(quads[i], &p1[0], &p1[1], &p1[2], &p1[3]);
        int n2 = swscanf(quads[i], L"%d %d %d %d", &p2[0], &p2[1], &p2[2], &p2[3]);
        if (n2 < 0) n2 = 0;      // 同上：空串 EOF vs 0
        g_cases++;
        if (n1 != n2 || memcmp(p1, p2, sizeof p1) != 0) {
            fprintf(g_f, "DIFF wScanI4 [%ls] n=%d/%d\n", quads[i], n1, n2);
            g_bad++;
        }
    }

    fprintf(g_f, "%s  %d 例  差异:%d\n", g_bad ? "FAIL" : "ok", g_cases, g_bad);
    fclose(g_f);
    return g_bad ? 1 : 0;
}
