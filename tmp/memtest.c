// memtest.c — claunch.asm 里 mem* 四件套的单元测试（含 memmove 重叠双向）。
// 测的是真实现：与 claunch.asm 汇编产物同源链接，不链任何 CRT。
//
// 构建（在仓库根目录）：
//   ml64 /nologo /c /Fo tmp\memtest_asm.obj claunch.asm
//   cl /nologo /std:c11 /O1 /GS- /utf-8 /NODEFAULTLIB /ENTRY:mem_main ^
//      /Fe:tmp\memtest.exe tmp\memtest.c tmp\memtest_asm.obj ^
//      /link /SUBSYSTEM:CONSOLE kernel32.lib
//
// 跑法：tmp\memtest.exe   —— 结果写 tmp\memtest.txt（避开控制台干扰）
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stddef.h>

void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

#define MAXBUF 2048
static BYTE s_a[MAXBUF], s_b[MAXBUF], s_ref[MAXBUF];

static int s_cases, s_fails;
static WCHAR s_msg[256];
static int s_msgLen;

static void Say(const WCHAR *s)
{
    int n = 0;
    while (s[n] && s_msgLen < 250) { s_msg[s_msgLen++] = s[n++]; }
}

static void SayNum(unsigned long long v)
{
    WCHAR t[24];
    int n = 0;
    if (!v) t[n++] = L'0';
    while (v) { t[n++] = (WCHAR)(L'0' + (v % 10)); v /= 10; }
    while (n && s_msgLen < 250) s_msg[s_msgLen++] = t[--n];
}

// 结果一律追加写：先 Fail 的明细，最后 Report 的汇总，明细不会被汇总覆盖
static void AppText(const WCHAR *s, int n)
{
    HANDLE h = CreateFileW(L"tmp\\memtest.txt", FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(h, s, (DWORD)(n * sizeof(WCHAR)), &w, NULL);
        CloseHandle(h);
    }
}

static void Fail(const WCHAR *what)
{
    s_fails++;
    s_msgLen = 0;
    Say(L"FAIL "); Say(what); Say(L"\r\n");
    AppText(s_msg, s_msgLen);
}

// 参考实现：重叠安全的朴素字节循环，用来对拍 memmove
static void RefMove(BYTE *d, const BYTE *s, size_t n)
{
    if (d < s) for (size_t i = 0; i < n; i++) d[i] = s[i];
    else       for (size_t i = n; i-- > 0; ) d[i] = s[i];
}

static const size_t SIZES[] = { 0, 1, 2, 3, 4, 7, 8, 15, 16, 17, 31, 32, 63, 64, 100, 255, 1000, 2000 };
static const int    DELTAS[] = { -64, -9, -3, -1, 0, 1, 3, 9, 64 };

// 经易失函数指针调用：挡住编译器把调用点替换成内联 intrinsic，保证测到真实现
static void *(* volatile p_cpy)(void *, const void *, size_t) = memcpy;
static void *(* volatile p_mov)(void *, const void *, size_t) = memmove;
static void *(* volatile p_set)(void *, int, size_t) = memset;
static int   (* volatile p_cmp)(const void *, const void *, size_t) = memcmp;

static void TestMemset(void)
{
    for (size_t k = 0; k < sizeof SIZES / sizeof SIZES[0]; k++) {
        size_t n = SIZES[k];
        if (n + 32 > MAXBUF) continue;
        for (int i = 0; i < MAXBUF; i++) s_a[i] = 0x5A;
        // 前后各留 16 字节哨兵，中间填 0xC3
        void *got = p_set(s_a + 16, 0xC3, n);
        s_cases++;
        if (got != s_a + 16) { Fail(L"memset return != dst"); return; }
        for (int i = 0; i < 16; i++)
            if (s_a[i] != 0x5A) { Fail(L"memset clobber before dst"); return; }
        for (size_t i = 0; i < n; i++)
            if (s_a[16 + i] != 0xC3) { Fail(L"memset body"); return; }
        for (size_t i = 16 + n; i < 16 + n + 16; i++)
            if (s_a[i] != 0x5A) { Fail(L"memset clobber after dst"); return; }
    }
    // 高位字节必须被忽略：memset(p, 0x1234, n) 只应写 0x34
    for (int i = 0; i < MAXBUF; i++) s_a[i] = 0;
    p_set(s_a, 0x1234, 32);
    if (s_a[0] != 0x34 || s_a[31] != 0x34) Fail(L"memset ignores high byte of c");
}

static void TestMemcpy(void)
{
    for (int i = 0; i < MAXBUF; i++) s_b[i] = (BYTE)(i * 31 + 7);
    for (size_t k = 0; k < sizeof SIZES / sizeof SIZES[0]; k++) {
        size_t n = SIZES[k];
        if (n + 16 > MAXBUF) continue;
        for (int i = 0; i < MAXBUF; i++) s_a[i] = 0xEE;
        void *got = p_cpy(s_a, s_b, n);
        s_cases++;
        if (got != s_a) { Fail(L"memcpy return != dst"); return; }
        for (size_t i = 0; i < n; i++)
            if (s_a[i] != s_b[i]) { Fail(L"memcpy body"); return; }
        for (size_t i = n; i < n + 16; i++)
            if (s_a[i] != 0xEE) { Fail(L"memcpy wrote past n"); return; }
    }
}

static void TestMemcmp(void)
{
    for (int i = 0; i < MAXBUF; i++) s_a[i] = s_b[i] = (BYTE)(i * 17 + 3);
    for (size_t k = 0; k < sizeof SIZES / sizeof SIZES[0]; k++) {
        size_t n = SIZES[k];
        if (n + 1 > MAXBUF) continue;
        s_cases++;
        if (p_cmp(s_a, s_b, n) != 0) { Fail(L"memcmp equal buffers != 0"); return; }
        s_b[n] = (BYTE)(s_a[n] + 1);            // 首个差异在第 n 字节
        int r = p_cmp(s_a, s_b, n + 1);
        s_cases++;
        if (r >= 0) { Fail(L"memcmp a<b must be negative"); return; }
        r = p_cmp(s_b, s_a, n + 1);
        if (r <= 0) { Fail(L"memcmp a>b must be positive"); return; }
        s_b[n] = s_a[n];                        // 复原
    }
    // 差异不在长度内：应当判等（不越界读）
    s_b[100] = 0xFF;
    s_cases++;
    if (p_cmp(s_a, s_b, 100) != 0) { Fail(L"memcmp must not read past n"); return; }
    s_b[100] = s_a[100];
}

static void TestMemmove(void)
{
    for (int i = 0; i < MAXBUF; i++) s_a[i] = (BYTE)(i * 13 + 5);
    for (size_t k = 0; k < sizeof SIZES / sizeof SIZES[0]; k++) {
        for (size_t d = 0; d < sizeof DELTAS / sizeof DELTAS[0]; d++) {
            size_t n = SIZES[k];
            ptrdiff_t delta = DELTAS[d];
            // 源区固定在 [768,768+n)，落点平移 delta，两端都留在缓冲内
            BYTE *src = s_a + 768;
            ptrdiff_t dstOff = 768 + delta;
            if (n > 600 || dstOff < 0 || dstOff + (ptrdiff_t)n > MAXBUF) continue;
            BYTE *dst = s_a + dstOff;

            for (int i = 0; i < MAXBUF; i++) s_ref[i] = s_a[i];
            BYTE *rsrc = s_ref + 768, *rdst = s_ref + dstOff;
            RefMove(rdst, rsrc, n);

            void *got = p_mov(dst, src, n);
            s_cases++;
            if (got != dst) { Fail(L"memmove return != dst"); return; }
            // 整缓冲逐字节对拍：越界写、方向错、重叠算错都会在哨兵区露出来
            for (int i = 0; i < MAXBUF; i++)
                if (s_a[i] != s_ref[i]) {
                    s_fails++;
                    s_msgLen = 0;
                    Say(L"FAIL memmove n="); SayNum(n);
                    Say(L" delta="); SayNum((unsigned long long)(delta + 1000));
                    Say(L" at="); SayNum((unsigned long long)i);
                    Say(L"\r\n");
                    AppText(s_msg, s_msgLen);
                    return;
                }
        }
    }
}

static void Report(void)
{
    s_msgLen = 0;
    Say(s_fails ? L"FAIL cases=" : L"PASS cases=");
    SayNum((unsigned long long)s_cases);
    Say(L" fails="); SayNum((unsigned long long)s_fails);
    Say(L"\r\n");
    AppText(s_msg, s_msgLen);
}

void __stdcall mem_main(void)
{
    // 先清空结果文件：本次的失败明细 + 末尾汇总就是全部内容
    HANDLE h = CreateFileW(L"tmp\\memtest.txt", GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    TestMemset();
    if (!s_fails) TestMemcpy();
    if (!s_fails) TestMemcmp();
    if (!s_fails) TestMemmove();
    Report();
    ExitProcess(s_fails ? 1 : 0);
}
