// parsefuzz.c — 三个"吃外来数据"的入口的对抗性测试：ParseLnkBuf / RdLine /
// LoadConfig。这三个都是纯函数（或只碰全局态），可以直接 #include 主源码来测，
// 不需要拉起窗口。
//
// 断言的是**容纳不变式**而不是某个输入的具体输出：
//   * 任何输入都不许崩、不许越界写（哨兵字节必须原样留在缓冲区外）
//   * 返回 TRUE 的 LnkInfo 必须 NUL 收尾且字段不超各自容量
//   * LoadConfig 之后：条目/分组不超上限、组号在范围内、字符串必须收尾
//   * RdLine 对任意字节流与任意 cch（含 1）都产出以 NUL 收尾的行
//
// 用法: parsefuzz [轮数]   （默认 20000 轮随机输入 + 一批定向边界用例）

#define wWinMain claunch_app_wWinMain      // 别和本探针的入口打架
#include "../claunch.c"
#undef wWinMain

#include <stdio.h>
#include <stdlib.h>

static unsigned long long g_rng = 0x9E3779B97F4A7C15ULL;
static unsigned rnd(void)                  // xorshift64*：可复现
{
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return (unsigned)((g_rng * 0x2545F4914F6CDD1DULL) >> 32);
}

static int g_fail;
static int g_lnkOk, g_rdTrue, g_maxItems, g_maxGroups, g_rdCch1, g_rtRounds, g_rtWithItems;

// 确认测试有牙齿：统计各路径真实走到的次数，全为 0 就说明什么都没测到
static void PrintCoverage(void)
{
    printf("覆盖: ParseLnkBuf TRUE %d / RdLine TRUE %d / 配置最多 %d 条目 %d 分组"
           " / cch=1 情形 %d / 存读往返 %d 轮（%d 轮带条目）\n",
           g_lnkOk, g_rdTrue, g_maxItems, g_maxGroups, g_rdCch1, g_rtRounds, g_rtWithItems);
}

static void Chk(BOOL ok, const char *what)
{
    if (!ok) { printf("FAIL %s\n", what); g_fail++; }
}

// ---------------------------------------------------------------- .lnk 缓冲
// 布局：0x00 头长、0x14 标志、0x38 图标索引，之后是 IDList/FileLoc/… 各段。
// 生成器刻意往"偏移越界/长度为 0/计数巨大"上靠
static DWORD RandLnk(BYTE *b, DWORD cap)
{
    DWORD n = 4 + rnd() % (cap - 4);
    for (DWORD i = 0; i < n; i++) b[i] = (BYTE)rnd();
    if (n >= 0x4C) {
        *(DWORD *)(b + 0x00) = 0x4C;                     // 头长必须合法才进得去
        *(DWORD *)(b + 0x14) = rnd();                    // 全随机标志位
        *(DWORD *)(b + 0x38) = rnd() % 1000;             // 图标索引
    }
    return n;
}

static void FuzzLnk(int rounds)
{
    for (int i = 0; i < rounds; i++) {
        BYTE buf[2048];
        DWORD n = RandLnk(buf, sizeof buf);
        if (n < 0x4C) { n = 0x4C; *(DWORD *)(buf + 0x00) = 0x4C; }
        LnkInfo li;
        ZeroMemory(&li, sizeof li);
        BOOL ok = ParseLnkBuf(buf, n, &li);
        if (ok) {
            g_lnkOk++;
            // 每个字段必须 NUL 收尾（写满时最后一个字符就得是 NUL）
            Chk(li.target[MAX_PATH - 1] == 0, "lnk target 未收尾");
            Chk(li.iconLoc[MAX_PATH - 1] == 0, "lnk iconLoc 未收尾");
            Chk(li.args[191] == 0, "lnk args 未收尾");
            Chk(li.work[MAX_PATH - 1] == 0, "lnk work 未收尾");
        }
    }
    // 定向边界：头长恰好在界内外、各种截断长度
    static const DWORD lens[] = { 0, 1, 0x4B, 0x4C, 0x4D, 0x100, 0x7FF, 0x800 };
    for (int k = 0; k < (int)(sizeof lens / sizeof *lens); k++) {
        BYTE b[2048];
        ZeroMemory(b, sizeof b);
        DWORD n = lens[k];
        if (n > sizeof b) n = sizeof b;
        *(DWORD *)(b + 0x00) = 0x4C;
        LnkInfo li;
        ZeroMemory(&li, sizeof li);
        ParseLnkBuf(b, n, &li);                 // 只要求不崩
    }
    BYTE b[2048];
    ZeroMemory(b, sizeof b);
    *(DWORD *)(b + 0x00) = 0xFFFFFFFF;          // 头长越界
    { LnkInfo li; ZeroMemory(&li, sizeof li); Chk(!ParseLnkBuf(b, 2048, &li), "头长越界应拒绝"); }
}

// ---------------------------------------------------------------- RdLine
// 对任意字节流与任意 cch（含 1）都必须产出以 NUL 收尾的行；缓冲外哨兵不许动
static void FuzzRdLine(int rounds)
{
    for (int i = 0; i < rounds; i++) {
        BYTE raw[128];
        DWORD n = rnd() % sizeof raw;
        for (DWORD k = 0; k < n; k++) {
            BYTE c = (BYTE)rnd();
            if (c == '\n') c = 'x';             // 换行单独成一轮，见下
            raw[k] = c;
        }
        // 随机塞入换行/回车，测行边界
        for (int t = 0; t < 3; t++)
            if (n) raw[rnd() % n] = (rnd() & 1) ? '\n' : '\r';

        RdCtx r;
        ZeroMemory(&r, sizeof r);
        r.f = CreateFileW(L"tmp\\parsefuzz.bin", GENERIC_WRITE, 0, NULL,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (r.f == INVALID_HANDLE_VALUE) { printf("FAIL 临时文件建不了\n"); g_fail++; return; }
        DWORD wrote = 0;
        WriteFile(r.f, raw, n, &wrote, NULL);
        CloseHandle(r.f);
        r.f = CreateFileW(L"tmp\\parsefuzz.bin", GENERIC_READ, 0, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        r.first = TRUE;

        int cch = 1 + (int)(rnd() % 64);
        WCHAR out[80], guard[80];
        ZeroMemory(out, sizeof out);
        wcscpy_s(guard, 80, L"GUARD");
        WCHAR *dst = out;                        // out[0..cch-1] 是被写区
        BOOL got = RdLine(&r, dst, cch);
        if (got) {
            g_rdTrue++;
            if (cch == 1) g_rdCch1++;
            Chk(dst[cch - 1] == 0, "RdLine 未在 cch-1 收尾");
            Chk(wlen_(dst) < (size_t)cch, "RdLine 写超出 cch");
        }
        // 反复读到 EOF 必须都安全
        while (RdLine(&r, dst, cch)) { }
        CloseHandle(r.f);
        Chk(!wcscmp(guard, L"GUARD"), "RdLine 踩到缓冲区外");
    }
    DeleteFileW(L"tmp\\parsefuzz.bin");
}

// ---------------------------------------------------------------- LoadConfig
// 敌意配置：字段缺三少四、索引跳号、组号越界、编码破坏、行超长、段名乱写。
// 断言容纳不变式：条目/分组不超上限、组号合法、每个字符串都收尾
static void WriteIniTo(const WCHAR *path, const char *buf, int n);

static void WriteIni2(const char *buf, int n)   // 按长度写：随机字节里可能有 NUL
{
    WriteIniTo(L"tmp\\parsefuzz.ini", buf, n);
}

static void WriteIniTo(const WCHAR *path, const char *buf, int n)
{
    FILE *f = _wfopen(path, L"wb");
    if (f) { fwrite(buf, 1, (size_t)n, f); fclose(f); }
}

static void WriteIni(const char *text)
{
    FILE *f = fopen("tmp\\parsefuzz.ini", "wb");
    if (!f) { printf("FAIL ini 写不出\n"); g_fail++; return; }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static void CheckInvariants(const char *what)
{
    if (g_nItems > g_maxItems) g_maxItems = g_nItems;
    if (g_nGroups > g_maxGroups) g_maxGroups = g_nGroups;
    Chk(g_nItems >= 0 && g_nItems <= MAX_ITEMS, what);
    Chk(g_nGroups >= 0 && g_nGroups <= MAX_GROUPS, what);
    for (int i = 0; i < g_nItems; i++) {
        Chk(g_items[i].name[47] == 0, what);
        Chk(g_items[i].path[MAX_PATH - 1] == 0, what);
        Chk(g_items[i].args[191] == 0, what);
        Chk(g_items[i].tip[95] == 0, what);
        Chk(g_items[i].group >= 0 && g_items[i].group < g_nGroups, what);
    }
    for (int i = 0; i < g_nGroups; i++)
        Chk(g_groups[i].name[47] == 0, what);
}

static const WCHAR *kIni = L"tmp\\parsefuzz.ini";

static void LoadOne(const char *text)
{
    WriteIni(text);
    wcpy(g_ini, MAX_PATH, kIni);      // LoadConfig 只认 g_ini：不设就永远读到空路径
    g_nItems = 0; g_nGroups = 0; g_curGroup = 0;
    LoadConfig();
    CheckInvariants("LoadConfig 容纳不变式");
}

static void FuzzConfig(void)
{
    static const char *cases[] = {
        "[Main]\r\nRect=0 0 0 0\r\nCell=0\r\nGroup=99\r\n",
        "[Main]\r\nRect=abc\r\nCell=-99999\r\nHotkey=0\r\n",
        "[Groups]\r\nCount=999\r\nG0=a\r\nG5=b\r\n",
        "[Items]\r\nCount=999\r\nI0=x|C:\\a|0||\r\nI7=y|C:\\b|99||\r\n",
        "[Items]\r\nI0=x|C:\\a\r\n",                         // 少字段
        "[Items]\r\nI0=x|C:\\a|0|||extra|\r\n",              // 多字段
        "[Items]\r\nI0=",                                     // 空行尾
        "[Items]\r\nI0=x|C:\\a|0||\r\n" "I0=x|C:\\b|0||\r\n", // 索引重复
        "\xEF\xBB\xBF[Main]\r\nCell=56\r\n",                 // BOM
        "[Main]\r\nCell=56\r\n",                             // 无回车
        "[Items]\r\nI0=" "\xD6\xD0\xD0\xC4" "|C:\\a|0||\r\n", // 非法 UTF-8
        "[Main]\r\nCell=1" "2" "3" "4" "5" "6" "7" "8" "9" "0" "1" "2\r\n", // 超长数字
        "",
        "[Nonsense]\r\nFoo=bar\r\n",
    };
    for (int i = 0; i < (int)(sizeof cases / sizeof *cases); i++)
        LoadOne(cases[i]);

    // 随机字节流当配置：只要求不崩、不变式不被破坏
    for (int i = 0; i < 400; i++) {
        char buf[512];
        int n = rnd() % sizeof buf;
        for (int k = 0; k < n; k++) buf[k] = (char)(rnd() & 0xFF);
        WriteIni2(buf, n);
        wcpy(g_ini, MAX_PATH, kIni);
        g_nItems = 0; g_nGroups = 0; g_curGroup = 0;
        LoadConfig();
        CheckInvariants("LoadConfig 随机字节不变式");
    }
    DeleteFileW(L"tmp\\parsefuzz.ini");
}

// 结构化配置生成：按图索骥地打每个不变式对应的代码点。
// 顺序的 I<n>=（索引必须等于当前条目数才会被收）+ 越界组号 + 超长字段（打
// ItemSetText 的截断）+ 字段缺三少四 + 乱序 G 行 + 分段名乱写
static void GenConfig(char *buf, size_t cap, size_t *len)
{
    size_t n = 0;
    int idx = 0, grps = 0;
    n += (size_t)sprintf(buf + n, "[Main]\r\n");
    n += (size_t)sprintf(buf + n, "Rect=%d %d %d %d\r\n", (int)(rnd()%3000)-500,
                         (int)(rnd()%2000)-500, 100 + (int)(rnd()%900), 400 + (int)(rnd()%400));
    n += (size_t)sprintf(buf + n, "Cell=%d\r\n", (int)(rnd()%400) - 50);
    n += (size_t)sprintf(buf + n, "Group=%d\r\n", (int)(rnd()%6) - 1);
    n += (size_t)sprintf(buf + n, "[Groups]\r\n");
    for (int i = 0; i < 6 && n + 600 < cap; i++) {
        grps++;
        n += (size_t)sprintf(buf + n, "G%d=", i);
        for (int k = 0, m = (int)(rnd()%120); k < m && n + 8 < cap; k++)
            buf[n++] = "aB1"[rnd()%3];              // 有超长也有短的组名
        n += (size_t)sprintf(buf + n, "\r\n");
    }
    n += (size_t)sprintf(buf + n, "[Items]\r\n");
    for (int i = 0; i < 60 && n + 1400 < cap; i++) {
        int form = (int)(rnd()%5);
        if (form == 0) n += (size_t)sprintf(buf + n, "I%d=", idx++);   // 顺序：会被收
        else if (form == 1) n += (size_t)sprintf(buf + n, "I%d=", (int)(rnd()%50));  // 跳号：被跳过
        else if (form == 2) { n += (size_t)sprintf(buf + n, "X%d=x|p|0|\r\n", idx); continue; }  // 非 I：跳过
        else n += (size_t)sprintf(buf + n, "I%d=", idx++);
        for (int k = 0, m = (int)(rnd()%300); k < m && n + 8 < cap; k++)
            buf[n++] = "n/p"[rnd()%2];              // 超长名与路径
        n += (size_t)sprintf(buf + n, "|C:////x|");
        int grp = (int)(rnd()%8) - 1;               // 含越界与负值
        n += (size_t)sprintf(buf + n, "%d|", grp);
        if (rnd()&1) { for (int k = 0, m = (int)(rnd()%400); k < m && n + 8 < cap; k++) buf[n++] = 'a'; }
        n += (size_t)sprintf(buf + n, "|");
        if (rnd()&1) { for (int k = 0, m = (int)(rnd()%400); k < m && n + 8 < cap; k++) buf[n++] = 't'; }
        if (rnd()%4 == 0) n += (size_t)sprintf(buf + n, "|EXTRA");   // 多字段
        n += (size_t)sprintf(buf + n, "\r\n");
        if (rnd()%8 == 0) {   // 偶发切段：多数行必须留在 [Items] 里，否则测不到条目路径
            n += (size_t)sprintf(buf + n, "[Junk%d]\r\n", (int)(rnd()%3));
            n += (size_t)sprintf(buf + n, "[Items]\r\n");     // 再切回来，继续打条目
        }
    }
    buf[n] = 0;
    *len = n;
}

static void FuzzConfigStructured(int rounds)
{
    char *buf = (char *)malloc(64 * 1024);
    for (int i = 0; i < rounds; i++) {
        size_t n = 0;
        GenConfig(buf, 64 * 1024, &n);
        WriteIni2(buf, (int)n);
        wcpy(g_ini, MAX_PATH, kIni);
        g_nItems = 0; g_nGroups = 0; g_curGroup = 0;
        LoadConfig();
        CheckInvariants("LoadConfig 结构化不变式");
    }
    free(buf);
}

// ---------------------------------------------------------------- 存-读往返
// SaveConfig 的防呆要求 ini 名以 claunch.ini 结尾，且它只画不建目录：先自建。
// g_hwnd 用假句柄（只为骗过 !g_hwnd 的早退），GetWindowRect 失败也没关系——
// Rect 由 g_savedX/Y/W/H 决定，读取端同样走这几个字段
#define RT_DIR L"tmp\\pf"
#define RT_INI RT_DIR L"\\claunch.ini"

static LRESULT CALLBACK RtWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcW(h, m, w, l);
}

// 建一个真宿主窗：SaveConfig 的 Rect 来自 GetWindowRect(g_hwnd)，假句柄会让
// 它失败并写出垃圾 Rect，往返断言就成了测脚手架自己。真窗才能让 Rect 这一项
// 名副实义地过一遍"写→读"
static HWND RtMakeHost(void)
{
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = RtWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"ParseFuzzHost";
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(0, L"ParseFuzzHost", L"", WS_POPUP, 0, 0, 100, 100,
                             NULL, NULL, wc.hInstance, NULL);
    SetWindowPos(h, NULL, 123, 456, 789, 1011, SWP_NOZORDER | SWP_NOACTIVATE);
    return h;
}

static void RtSetup(void)
{
    CreateDirectoryW(RT_DIR, NULL);
    wcpy(g_ini, MAX_PATH, RT_INI);
    g_hwnd = RtMakeHost();
}

// 快照条目与分组，供往返前后比对
static void RtSnap(WCHAR *names, int *groups, int *n)
{
    *n = g_nItems;
    for (int i = 0; i < g_nItems; i++) {
        wcpy(names + (size_t)i * 64, 64, g_items[i].name);
        groups[i] = g_items[i].group;
    }
}

static void FuzzRoundTrip(int rounds)
{
    RtSetup();
    char *buf = (char *)malloc(64 * 1024);
    WCHAR *names = (WCHAR *)malloc((size_t)MAX_ITEMS * 64 * sizeof(WCHAR));
    int *groups = (int *)malloc((size_t)MAX_ITEMS * sizeof(int));
    int n = 0;
    for (int i = 0; i < rounds; i++) {
        size_t len = 0;
        GenConfig(buf, 64 * 1024, &len);
        WriteIniTo(RT_INI, buf, (int)len);   // 必须写到 SaveConfig 要读的那个路径
        wcpy(g_ini, MAX_PATH, RT_INI);
        g_nItems = 0; g_nGroups = 0; g_curGroup = 0;
        LoadConfig();
        CheckInvariants("往返-载入不变式");
        RtSnap(names, groups, &n);
        g_rtRounds++;
        if (n > 0) g_rtWithItems++;
        // Rect 走保存端：先固定，SaveConfig 用它写，读回来应该一模一样
        g_savedX = 123; g_savedY = 456; g_savedW = 789; g_savedH = 1011;

        SaveConfig();                       // 写盘（含防呆的 tmp+MoveFileExW 原子替换）

        g_nItems = 0; g_nGroups = 0; g_curGroup = 0;
        LoadConfig();                       // 再读回来
        CheckInvariants("往返-重载不变式");
        Chk(g_nItems == n, "往返后条目数变了");
        for (int k = 0; k < n && k < g_nItems; k++) {
            Chk(!wcmp_(g_items[k].name, names + (size_t)k * 64), "往返后条目名变了");
            Chk(g_items[k].group == groups[k], "往返后条目组号变了");
        }
        Chk(g_savedW == 789 && g_savedH == 1011, "往返后窗口尺寸变了");
    }
    free(buf); free(names); free(groups);
    if (g_hwnd) DestroyWindow(g_hwnd);
    DeleteFileW(RT_INI);
    RemoveDirectoryW(RT_DIR);
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 20000;
    printf("parsefuzz: %d 轮\n", rounds);
    FuzzLnk(rounds);
    FuzzRdLine(rounds / 4 + 1);
    FuzzConfig();
    FuzzConfigStructured(rounds / 100 + 1);
    FuzzRoundTrip(rounds / 200 + 1);
    PrintCoverage();
    printf("%s\n", g_fail ? "== 有失败项 ==" : "== 全部通过 ==");
    return g_fail ? 1 : 0;
}
