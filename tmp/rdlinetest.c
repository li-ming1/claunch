// rdlinetest.c — RdLine 行缓冲单元测试（逐字拷贝 kk.c 第十八轮实现）：
// 喂一条 1221 字节 UTF-8 的条目行（超旧缓冲 1200 字节上限），断言
// name/path/args/tip 四字段经 UTF-8→UTF-16 转换后完整无损
#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;

static void Check(BOOL ok, const char *what)
{
    if (ok) printf("ok   %s\n", what);
    else { printf("FAIL %s\n", what); g_fail++; }
}

// ============ 逐字拷贝自 kk.c（第十八轮 RdLine 实现） ============
typedef struct { HANDLE f; BYTE buf[4096]; int len, pos; BOOL first; } RdCtx;

static BOOL RdRefill(RdCtx *r)
{
    if (r->pos < r->len) return TRUE;
    DWORD n = 0;
    if (!ReadFile(r->f, r->buf, sizeof r->buf, &n, NULL)) { r->len = r->pos = 0; return FALSE; }
    r->pos = 0;
    r->len = (int)n;
    return n > 0;
}

static BOOL RdLine(RdCtx *r, WCHAR *out, int cch)
{
    // 行缓冲按"字节"算，而写侧 FmtLine 按 1200 个 WCHAR 封顶——一条含
    // 中文路径的行（每字 UTF-8 至多 3 字节）可到 3600 字节，缓冲小于它
    // 就会把超出的字段静默截掉（读回的路径/参数损坏）。给足最坏值
    BYTE line[4096];
    int n = 0;
    for (;;) {
        if (r->pos >= r->len && !RdRefill(r)) {
            if (n == 0) return FALSE;
            break;
        }
        BYTE c = r->buf[r->pos++];
        if (c == '\n') break;
        if (n < (int)sizeof line) line[n++] = c;
    }
    if (n > 0 && line[n - 1] == '\r') n--;
    int w = MultiByteToWideChar(CP_UTF8, 0, (char *)line, n, out, cch - 1);
    if (w < 0) w = 0;
    out[w] = 0;
    if (r->first) {
        r->first = FALSE;
        if (out[0] == 0xFEFF) {                      // 去掉 UTF-8 BOM
            int i = 0;
            while (out[i]) { out[i] = out[i + 1]; i++; }
        }
    }
    return TRUE;
}
// ============ 拷贝结束 ============

int wmain(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    // 最坏情况行（对齐 SaveConfig 的字段上限）：name 47 中文（141B）、
    // path 259 中文（777B）、args 191 中文（573B）、tip 95 中文（285B），
    // 合计约 1780 UTF-8 字节 > 旧缓冲 1200 —— 截断场景复现
    char name[256], path[1024], args[1024], tip[512];
    int i;
    for (i = 0; i < 47; i++) memcpy(name + i * 3, "\xe9\x95\xbf", 3); name[i * 3] = 0;
    strcpy(path, "D:////");
    for (i = 0; i < 250; i++) memcpy(path + 3 + i * 3, "\xe5\xbe\x84", 3);
    path[3 + i * 3] = 0;
    for (i = 0; i < 191; i++) memcpy(args + i * 3, "\xe5\x8f\x82", 3); args[i * 3] = 0;
    for (i = 0; i < 95; i++) memcpy(tip + i * 3, "\xe6\x8f\x90", 3); tip[i * 3] = 0;
    char line[4096];
    int ln = snprintf(line, sizeof line, "I0=%s|%s|1|%s||%s\r\n", name, path, args, tip);
    int lineBytes = (int)strlen(line) - 2;   // 不含 CRLF
    printf("行 UTF-8 字节数: %d\n", lineBytes);

    // 写临时文件
    FILE *f = fopen("rdline_test.ini", "wb");
    fwrite(line, 1, ln, f);
    fclose(f);

    // 读回
    RdCtx r;
    ZeroMemory(&r, sizeof r);
    r.first = TRUE;
    r.f = CreateFileW(L"rdline_test.ini", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    WCHAR out[1200];
    BOOL got = RdLine(&r, out, 1200);
    if (r.f != INVALID_HANDLE_VALUE) CloseHandle(r.f);
    DeleteFileW(L"rdline_test.ini");
    Check(got, "行读回成功");
    if (!got) return 1;

    // 字段完整性：按 | 切分，逐一比对字节内容
    WCHAR sep = L'|';
    WCHAR *fields[8] = { 0 };
    int nf = 0;
    WCHAR *p = out;
    while (nf < 8) {
        fields[nf++] = p;
        WCHAR *b = wcschr(p, sep);
        if (!b) break;
        *b = 0;
        p = b + 1;
    }
    // 分段：[0]="I0+名字" [1]=路径 [2]=组号 [3]=参数 [4]=空work [5]=提示
    Check(nf == 6, "字段数 = 6（I0 前缀并入名字段）");
    // 逐字段转回 UTF-8 与源比对：截断最敏感的是行尾的 tip 与 args
    char argsUtf8[2048] = "", tipUtf8[1024] = "", pathUtf8[2048] = "";
    WideCharToMultiByte(CP_UTF8, 0, fields[3], -1, argsUtf8, 2048, NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, fields[5], -1, tipUtf8, 1024, NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, fields[1], -1, pathUtf8, 2048, NULL, NULL);
    Check(strcmp(argsUtf8, args) == 0, "args 字段逐字节完整");
    Check(strcmp(tipUtf8, tip) == 0, "tip 字段逐字节完整");
    Check(strcmp(pathUtf8, path) == 0, "path 字段逐字节完整");

    printf(g_fail ? "== 结果: %d 项失败 ==\n" : "== 结果: 全部通过 ==\n", g_fail);
    return g_fail ? 1 : 0;
}
