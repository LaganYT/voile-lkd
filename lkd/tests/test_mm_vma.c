/* test_mm_vma.c: the VMA table (AVL tree with gap augmentation) of the mm module.
 * Models: mm/vma.c and mm/mmap.c of Linux v6.12 (find_vma returns the first VMA whose end is above the
 * address; vm_unmapped_area finds the highest gap below high_limit for a top-down search). */
#include "ltest.h"
#include "../src/mm/mm_priv.h"

static mm_vma *mk(uint64_t s, uint64_t e)
{
    mm_vma *v = calloc(1, sizeof *v);
    v->start = s;
    v->end = e;
    return v;
}

static uint64_t first_fit(void *ctx, uint64_t lo, uint64_t hi, mm_vma *p, mm_vma *n)
{
    (void)p; (void)n;
    uint64_t len = *(uint64_t *)ctx;
    return hi - lo >= len ? hi - len : 0; /* top of the gap */
}

static uint64_t bottom_fit(void *ctx, uint64_t lo, uint64_t hi, mm_vma *p, mm_vma *n)
{
    (void)p; (void)n;
    uint64_t len = *(uint64_t *)ctx;
    return hi - lo >= len ? lo : 0;
}

LT_TEST(lookup_semantics)
{
    vtree t;
    vt_init(&t, 0x1000);
    mm_vma *a = mk(0x10000, 0x12000), *b = mk(0x20000, 0x30000), *c = mk(0x30000, 0x31000);
    vt_insert(&t, b);
    vt_insert(&t, a);
    vt_insert(&t, c);
    LT_EQ(vt_check(&t), 0);
    LT_EQ(t.n, 3);
    LT_CHECK(vt_find(&t, 0x10000) == a);
    LT_CHECK(vt_find(&t, 0x11fff) == a);
    LT_CHECK(vt_find(&t, 0x12000) == NULL);      /* end is exclusive */
    LT_CHECK(vt_first_end_after(&t, 0x12000) == b); /* find_vma: first with vm_end > addr */
    LT_CHECK(vt_first_end_after(&t, 0x0) == a);
    LT_CHECK(vt_first_end_after(&t, 0x31000) == NULL);
    LT_CHECK(vt_last_start_before(&t, 0x20000) == a);
    LT_CHECK(vt_last_start_before(&t, 0x20001) == b);
    LT_CHECK(vt_next(&t, b) == c && vt_prev(&t, b) == a && vt_prev(&t, a) == NULL);
    LT_CHECK(vt_first(&t) == a && vt_last(&t) == c);
    /* gaps: [0x1000,0x10000) [0x12000,0x20000) [0x30000,0x30000) */
    LT_EQ(t.root->maxgap, 0xf000);
    vt_set_end(&t, a, 0x11000);
    LT_EQ(b->prev_end, 0x11000);
    LT_EQ(vt_check(&t), 0);
    vt_remove(&t, b);
    LT_EQ(c->prev_end, 0x11000);
    LT_EQ(vt_check(&t), 0);
    vt_set_start(&t, c, 0x2f000);
    LT_EQ(vt_check(&t), 0);
    free(a); free(b); free(c);
}

LT_TEST(gap_search_topdown_and_bottomup)
{
    vtree t;
    vt_init(&t, 0x1000);
    mm_vma *v[4] = { mk(0x10000, 0x20000), mk(0x24000, 0x30000), mk(0x80000, 0x90000), mk(0xa0000, 0xa1000) };
    for (int i = 0; i < 4; i++) vt_insert(&t, v[i]);
    uint64_t len = 0x4000;
    /* highest gap below high = 0xa0000 is [0x90000, 0xa0000): place at its top */
    LT_EQ(vt_search(&t, len, 0x1000, 0xa0000, 0x100000, 1, first_fit, &len), 0x9c000);
    /* the tail gap above the last VMA counts first when topdown */
    LT_EQ(vt_search(&t, len, 0x1000, 0x200000, 0x100000, 1, first_fit, &len), 0xfc000);
    /* bottom-up: lowest gap of 0x4000 is [0x1000,0x10000) */
    LT_EQ(vt_search(&t, len, 0x1000, 0x200000, 0x100000, 0, bottom_fit, &len), 0x1000);
    /* a gap of exactly 0x4000 between 0x20000 and 0x24000 is found when low excludes the first one */
    LT_EQ(vt_search(&t, len, 0x18000, 0x200000, 0x100000, 0, bottom_fit, &len), 0x20000);
    len = 0x50000; /* only [0x30000,0x80000) fits */
    LT_EQ(vt_search(&t, len, 0x1000, 0x200000, 0xa1000, 1, first_fit, &len), 0x30000);
    len = 0x60000; /* nothing fits below the top */
    LT_EQ(vt_search(&t, len, 0x1000, 0x200000, 0xa1000, 1, first_fit, &len), 0);
    for (int i = 0; i < 4; i++) free(v[i]);
}

/* Random insertions and removals checked against a plain array, with the tree invariants (height,
 * balance, prev_end, maxgap) verified after every step. */
LT_TEST(random_against_reference)
{
    enum { N = 3000 };
    static uint8_t used[N];
    static mm_vma *node[N];
    memset(used, 0, sizeof used);
    vtree t;
    vt_init(&t, 0);
    uint32_t seed = 12345;
    int bad = 0, lookups_bad = 0;
    for (int step = 0; step < 20000; step++) {
        seed = seed * 1103515245u + 12345u;
        int i = (int)((seed >> 8) % N);
        if (!used[i]) {
            node[i] = mk((uint64_t)i * 0x10000 + 0x1000, (uint64_t)i * 0x10000 + 0x3000);
            vt_insert(&t, node[i]);
            used[i] = 1;
        } else {
            vt_remove(&t, node[i]);
            free(node[i]);
            used[i] = 0;
        }
        if (step % 500 == 0 && vt_check(&t)) bad++;
        uint64_t probe = (uint64_t)((seed >> 4) % N) * 0x10000 + 0x2000;
        mm_vma *f = vt_find(&t, probe);
        int k = (int)((probe - 0x1000) / 0x10000);
        if ((f != NULL) != (used[k] != 0)) lookups_bad++;
    }
    LT_EQ(bad, 0);
    LT_EQ(vt_check(&t), 0);
    LT_EQ(lookups_bad, 0);
    size_t n = 0;
    for (int i = 0; i < N; i++) n += used[i];
    LT_EQ(t.n, n);
    /* height stays logarithmic: an AVL tree of n nodes is at most 1.44 log2(n+2) high */
    LT_CHECK(t.root == NULL || t.root->h <= 18);
    for (int i = 0; i < N; i++) if (used[i]) { vt_remove(&t, node[i]); free(node[i]); }
    LT_EQ(t.n, 0);
    LT_CHECK(t.root == NULL);
}

LT_TEST(entry_is_small)
{
    /* HyperLinux kept 4144 bytes per entry; the LKD entry stays small. */
    LT_CHECK(sizeof(mm_vma) <= 128);
}

int main(void)
{
    LT_RUN(lookup_semantics);
    LT_RUN(gap_search_topdown_and_bottomup);
    LT_RUN(random_against_reference);
    LT_RUN(entry_is_small);
    return lt_finish();
}
