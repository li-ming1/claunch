// nametest.c — NameFromPath 新旧实现逐例对拍（纯函数，不碰 UI）
#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

// ---- 旧实现（HEAD 版）----
static void OldName(const WCHAR *path, WCHAR *out, int cch)
{
    const WCHAR *p = wcsrchr(path, L'\\');
    p = p ? p + 1 : path;
    const WCHAR *dot = wcsrchr(p, L'.');
    int n = (dot && dot > p) ? (int)(dot - p) : (int)wcslen(p);
    if (n >= cch) n = cch - 1;
    if (n < 1) n = 1;
    wcsncpy_s(out, (size_t)cch, p, (size_t)n);
}

// ---- 新实现 ----
static void NewName(const WCHAR *path, WCHAR *out, int cch)
{
    const WCHAR *p = wcsrchr(path, L'\\');
    p = p ? p + 1 : path;
    const WCHAR *dot = wcsrchr(p, L'.');
    int cut = (dot && dot > p) ? (int)(dot - p) : 0x7FFFFFFF;
    int n = 0;
    while (n < cch - 1 && n < cut && p[n]) n++;
    memcpy(out, p, (size_t)n * sizeof(WCHAR));
    out[n] = 0;
}

int wmain(void)
{
    static const WCHAR *cases[] = {
        L"C:\\a\\b.txt", L"C:\\a\\.gitignore", L"C:\\a\\archive.tar.gz",
        L"C:\\a\\noext", L"C:\\a\\", L"b.txt", L"C:\\a\\a.b.c",
        L"C:\\a\\.x", L"", L"\\", L"C:\\a\\中文名字.docx",
        L"C:\\a\\aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.exe",
        L"C:\\a\\a.", L"C:\\a\\.", L"C:\\a\\..", L"\\\\srv\\share\\f.lnk",
    };
    int bad = 0;
    for (int i = 0; i < (int)(sizeof cases / sizeof *cases); i++) {
        WCHAR a[48], b[48];
        OldName(cases[i], a, 48);
        NewName(cases[i], b, 48);
        int same = wcscmp(a, b) == 0;
        if (!same) bad++;
        wprintf(L"%-4s [%s] old=[%s] new=[%s]\n", same ? L"ok" : L"DIFF",
                cases[i], a, b);
    }
    printf("%s  (%d case(s) differ)\n", bad ? "FAIL" : "ok", bad);
    return bad ? 1 : 0;
}
