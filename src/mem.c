#include "tak.h"

extern char _end[];
u32 *mbi;

#define LIM (1ULL << 30)
#define NPG (LIM >> 12)

static u8 used[NPG / 8];
static u64 nfree, ntotal;
static u64 scan = 256;

static void mark(u64 pg, int u) {
    if (pg >= NPG) return;
    int was = (used[pg >> 3] >> (pg & 7)) & 1;
    if (u && !was) { used[pg >> 3] |= 1 << (pg & 7); nfree--; }
    if (!u && was) { used[pg >> 3] &= ~(1 << (pg & 7)); nfree++; }
}

static int isused(u64 pg) { return (used[pg >> 3] >> (pg & 7)) & 1; }

void *mb_tag(u32 type) {
    u8 *p = (u8 *)mbi + 8;
    u8 *end = (u8 *)mbi + mbi[0];
    while (p < end) {
        u32 t = *(u32 *)p, s = *(u32 *)(p + 4);
        if (t == 0) break;
        if (t == type) return p;
        p += (s + 7) & ~7u;
    }
    return NULL;
}

int mb_mod(int i, u64 *start, u64 *end, const char **name) {
    u8 *p = (u8 *)mbi + 8;
    u8 *e = (u8 *)mbi + mbi[0];
    int n = 0;
    while (p < e) {
        u32 t = *(u32 *)p, s = *(u32 *)(p + 4);
        if (t == 0) break;
        if (t == 3) {
            if (n == i) {
                *start = *(u32 *)(p + 8);
                *end = *(u32 *)(p + 12);
                *name = (const char *)(p + 16);
                return 1;
            }
            n++;
        }
        p += (s + 7) & ~7u;
    }
    return 0;
}

static void reserve(u64 a, u64 b) {
    for (u64 pg = a >> 12; pg < ((b + 4095) >> 12); pg++) mark(pg, 1);
}

u64 palloc(void) {
    for (u64 i = 0; i < NPG; i++) {
        u64 pg = (scan + i) % NPG;
        if (!isused(pg)) {
            mark(pg, 1);
            scan = pg + 1;
            memset((void *)(pg << 12), 0, 4096);
            return pg << 12;
        }
    }
    return 0;
}

u64 palloc_n(u32 n) {
    u64 run = 0;
    for (u64 pg = 256; pg < NPG; pg++) {
        if (isused(pg)) { run = 0; continue; }
        if (++run == n) {
            u64 s = pg + 1 - n;
            for (u64 k = s; k <= pg; k++) mark(k, 1);
            return s << 12;
        }
    }
    return 0;
}

void pfree(u64 pa) { mark(pa >> 12, 0); }
u64 mem_free_kb(void) { return nfree * 4; }
u64 mem_total_kb(void) { return ntotal * 4; }

struct blk {
    size_t sz;
    size_t free;
    struct blk *prev, *next;
};
#define HDR 32

static struct blk *head;
static u64 hsize;

static void heap_init(void) {
    u64 best = 0, bs = 0, run = 0, rs = 0;
    for (u64 pg = 256; pg < NPG; pg++) {
        if (isused(pg)) { run = 0; continue; }
        if (run == 0) rs = pg;
        if (++run > best) { best = run; bs = rs; }
    }
    u64 want = nfree * 3 / 4;
    if (want > best) want = best;
    if (want > (700ULL << 20) >> 12) want = (700ULL << 20) >> 12;
    u64 base = palloc_n((u32)want);
    (void)bs;
    if (!base) panic("no heap");
    head = (struct blk *)base;
    head->sz = (want << 12) - HDR;
    head->free = 1;
    head->prev = head->next = NULL;
    hsize = want << 12;
}

void mem_init(void) {
    memset(used, 0xFF, sizeof used);
    nfree = 0;
    ntotal = 0;
    u32 *t = mb_tag(6);
    if (!t) panic("no memory map");
    u32 es = t[2];
    for (u8 *e = (u8 *)(t + 4); e < (u8 *)t + t[1]; e += es) {
        u64 a = *(u64 *)e, l = *(u64 *)(e + 8);
        u32 ty = *(u32 *)(e + 16);
        if (ty != 1) continue;
        u64 s = (a + 4095) & ~4095ULL, f = (a + l) & ~4095ULL;
        if (s < (1 << 20)) s = 1 << 20;
        if (f > LIM) f = LIM;
        for (u64 p = s; p < f; p += 4096) { mark(p >> 12, 0); ntotal++; }
    }
    reserve(0x100000, (u64)_end);
    reserve((u64)mbi, (u64)mbi + mbi[0]);
    u64 ms, me;
    const char *nm;
    for (int i = 0; mb_mod(i, &ms, &me, &nm); i++) reserve(ms, me);
    heap_init();
}

void *kalloc(size_t n) {
    if (!n) n = 1;
    n = (n + 15) & ~15ULL;
    for (struct blk *b = head; b; b = b->next) {
        if (!b->free || b->sz < n) continue;
        if (b->sz >= n + HDR + 32) {
            struct blk *r = (struct blk *)((u8 *)b + HDR + n);
            r->sz = b->sz - n - HDR;
            r->free = 1;
            r->prev = b;
            r->next = b->next;
            if (b->next) b->next->prev = r;
            b->next = r;
            b->sz = n;
        }
        b->free = 0;
        return (u8 *)b + HDR;
    }
    return NULL;
}

void *kzalloc(size_t n) {
    void *p = kalloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void kfree(void *p) {
    if (!p) return;
    struct blk *b = (struct blk *)((u8 *)p - HDR);
    b->free = 1;
    if (b->next && b->next->free) {
        b->sz += HDR + b->next->sz;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
    if (b->prev && b->prev->free) {
        b->prev->sz += HDR + b->sz;
        b->prev->next = b->next;
        if (b->next) b->next->prev = b->prev;
    }
}

void *krealloc(void *p, size_t n) {
    if (!p) return kalloc(n);
    struct blk *b = (struct blk *)((u8 *)p - HDR);
    if (b->sz >= n) return p;
    void *q = kalloc(n);
    if (!q) return NULL;
    memcpy(q, p, b->sz);
    kfree(p);
    return q;
}

u64 heap_free_kb(void) {
    u64 f = 0;
    for (struct blk *b = head; b; b = b->next) if (b->free) f += b->sz;
    return f >> 10;
}
