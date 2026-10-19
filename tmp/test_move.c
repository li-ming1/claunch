// test_move.c — kk.c MoveItem 索引轮转自检（算法与 kk.c 保持一致）
// cl /nologo /W4 /O1 /MT /utf-8 tmp\test_move.c /Fe:tmp\test_move.exe && tmp\test_move.exe
#include <assert.h>
#include <stdio.h>
#include <wchar.h>

#define MAX_ITEMS 128
typedef struct { int group; int id; } Item;
static Item g_items[MAX_ITEMS];
static int  g_nItems = 0;
static int  g_curGroup = 0;

static int ViewCount(void)
{
    int n = 0;
    for (int i = 0; i < g_nItems; i++)
        if (g_curGroup == 0 || g_items[i].group == g_curGroup) n++;
    return n;
}

static int ItemAtView(int vi)
{
    int k = -1;
    for (int i = 0; i < g_nItems; i++) {
        if (g_curGroup == 0 || g_items[i].group == g_curGroup) {
            k++;
            if (k == vi) return i;
        }
    }
    return -1;
}

static void MoveItem(int vfrom, int vto)
{
    int n = ViewCount();
    if (vfrom < 0 || vto < 0 || vfrom >= n || vto >= n || vfrom == vto) return;
    int pos[MAX_ITEMS];
    for (int k = 0; k < n; k++) pos[k] = ItemAtView(k);
    Item tmp = g_items[pos[vfrom]];
    if (vfrom < vto)
        for (int k = vfrom; k < vto; k++) g_items[pos[k]] = g_items[pos[k + 1]];
    else
        for (int k = vfrom; k > vto; k--) g_items[pos[k]] = g_items[pos[k - 1]];
    g_items[pos[vto]] = tmp;
}

static void Reset(const int *groups, int n)
{
    g_nItems = n;
    for (int i = 0; i < n; i++) { g_items[i].group = groups[i]; g_items[i].id = i + 1; }
}

static int IdAtView(int vi) { return g_items[ItemAtView(vi)].id; }

int main(void)
{
    // 1) 全部视图：向前移 0 -> 3，其余顺移
    const int g1[] = { 1, 2, 1, 1, 2, 1 };
    Reset(g1, 6); g_curGroup = 0;
    MoveItem(0, 3);
    int want1[] = { 2, 3, 4, 1, 5, 6 };
    for (int i = 0; i < 6; i++) assert(IdAtView(i) == want1[i]);

    // 2) 全部视图：向后移 3 -> 0
    MoveItem(3, 0);
    int want2[] = { 1, 2, 3, 4, 5, 6 };
    for (int i = 0; i < 6; i++) assert(IdAtView(i) == want2[i]);

    // 3) 分组视图（group=2 只在全局位 1、4）：视图内轮转，局外项不动
    Reset(g1, 6); g_curGroup = 2;                     // 视图 = id 2,5
    MoveItem(0, 1);
    assert(ViewCount() == 2 && IdAtView(0) == 5 && IdAtView(1) == 2);
    assert(g_curGroup == 2);

    // 4) 分组视图带夹杂：group=1 在全局位 0,2,3,5（视图 = id 1,3,4,6），移 0 -> 2
    Reset(g1, 6); g_curGroup = 1;
    MoveItem(0, 2);
    assert(IdAtView(0) == 3 && IdAtView(1) == 4 && IdAtView(2) == 1 && IdAtView(3) == 6);
    // 局外 group=2 项仍在全局位 1、4，内容未被换掉
    assert(g_items[1].id == 2 && g_items[1].group == 2);
    assert(g_items[4].id == 5 && g_items[4].group == 2);

    // 5) 移到末尾（拖到空格）与原地/越界 no-op
    Reset(g1, 6); g_curGroup = 0;
    MoveItem(0, 5);
    assert(IdAtView(5) == 1 && IdAtView(0) == 2);
    MoveItem(2, 2);                                    // 原地：无变化
    assert(IdAtView(2) == 4);
    MoveItem(-1, 0); MoveItem(0, 99); MoveItem(99, 0); // 越界：无变化
    int want5[] = { 2, 3, 4, 5, 6, 1 };
    for (int i = 0; i < 6; i++) assert(IdAtView(i) == want5[i]);

    wprintf(L"test_move: all asserts passed\n");
    return 0;
}
