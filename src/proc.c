#include "tak.h"
#include "cpu.h"
#include "wm.h"
#include "vfs.h"
#include "proc.h"

#define UBASE 0x40000000ULL
#define UTOP 0x80000000ULL
#define USTK 0x7FFF0000ULL
#define KSTK 32768
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

enum { S_READY, S_RUN, S_BLOCK, S_SLEEP, S_DEAD };

struct task {
    int pid, state, user;
    char name[32];
    u64 rsp, kbase, ktop, cr3, wake;
    struct task *next;
    void *term;
    int inq[64], ih, it;
    u64 *fr;
    int nfr, cfr;
    u64 cpu;
    u64 pd1;
    u64 entry;
    u64 brk, hend;
    char arg[64];
};

struct te {
    char magic[4];
    u32 entry, fsize, msize, data_offset;
    char name[32];
};

extern u64 pml4[], pdpt[];

#define PTE_NX (1ULL << 63)

static void reap(void);
static struct task t0, *cur = &t0, *tlist = &t0;
static int npid = 1;
u64 idle_ticks, busy_ticks;
int wm_idle;

static int track(struct task *t, u64 pa) {
    if (t->nfr == t->cfr) {
        int cap = t->cfr ? t->cfr * 2 : 32;
        u64 *fr = krealloc(t->fr, (u64)cap * sizeof *fr);
        if (!fr) return -1;
        t->fr = fr;
        t->cfr = cap;
    }
    t->fr[t->nfr++] = pa;
    return 0;
}

static u64 tpalloc(struct task *t) {
    u64 p = palloc();
    if (p && track(t, p) < 0) {
        pfree(p);
        return 0;
    }
    return p;
}

void proc_init(void) {
    t0.pid = 0;
    t0.state = S_RUN;
    snprintf(t0.name, sizeof t0.name, "astros");
    t0.next = &t0;
    __asm__ volatile("mov %%cr3,%0" : "=r"(t0.cr3));
    t0.ktop = 0;
}

static void schedule(void) {
    u64 f = irqsave();
    struct task *n = cur->next;
    struct task *pick = NULL;
    for (int i = 0; i < 256 && n != cur; n = n->next, i++) {
        if (n->state == S_SLEEP && n->wake <= ticks) n->state = S_READY;
        if (n->state == S_READY) { pick = n; break; }
    }
    if (!pick) {
        if (cur->state == S_RUN || cur->state == S_READY) { irqrest(f); return; }
        pick = &t0;
        if (cur == &t0) { irqrest(f); return; }
    }
    struct task *old = cur;
    if (old->state == S_RUN) old->state = S_READY;
    pick->state = S_RUN;
    cur = pick;
    if (pick->ktop) tss_rsp0(pick->ktop);
    if (pick->cr3 != old->cr3) __asm__ volatile("mov %0,%%cr3" : : "r"(pick->cr3) : "memory");
    switch_ctx(&old->rsp, pick->rsp);
    irqrest(f);
}

void proc_yield(void) {
    if (cur == &t0) reap();
    schedule();
}

int proc_runnable(void) {
    for (struct task *n = tlist->next; n != tlist; n = n->next) {
        if (n->state == S_READY) return 1;
        if (n->state == S_SLEEP && n->wake <= ticks) return 1;
    }
    return 0;
}

void sched_tick(int from_user) {
    if (cur == &t0) { if (wm_idle) idle_ticks++; else busy_ticks++; }
    else { cur->cpu++; busy_ticks++; }
    for (struct task *n = tlist->next; n != tlist; n = n->next)
        if (n->state == S_SLEEP && n->wake <= ticks) n->state = S_READY;
    if (from_user) schedule();
}

void proc_wake(int pid) {
    for (struct task *n = tlist->next; n != tlist; n = n->next)
        if (n->pid == pid && n->state == S_BLOCK) n->state = S_READY;
}

static struct task *byid(int pid) {
    for (struct task *n = tlist->next; n != tlist; n = n->next)
        if (n->pid == pid && n->state != S_DEAD) return n;
    return NULL;
}

int proc_alive(int pid) { return byid(pid) != NULL; }

void proc_stdin(int pid, int ch) {
    struct task *t = byid(pid);
    if (!t) return;
    int nx = (t->it + 1) % 64;
    if (nx == t->ih) return;
    t->inq[t->it] = ch;
    t->it = nx;
    if (t->state == S_BLOCK) t->state = S_READY;
}

static void reap(void) {
    struct task *p = tlist;
    while (p->next != tlist) {
        struct task *n = p->next;
        if (n->state == S_DEAD && n != cur) {
            p->next = n->next;
            for (int i = 0; i < n->nfr; i++) pfree(n->fr[i]);
            kfree(n->fr);
            kfree((void *)n->kbase);
            kfree(n);
        } else p = n;
    }
}

static void kill_task(struct task *t) {
    t->state = S_DEAD;
    wm_close_owner(t->pid);
}

void task_done(void) {
    kill_task(cur);
    schedule();
    for (;;) halt();
}

int proc_kill(int pid) {
    struct task *t = byid(pid);
    if (!t || t == &t0) return -1;
    kill_task(t);
    if (t == cur) { schedule(); for (;;) halt(); }
    return 0;
}

void proc_fault(struct regs *r) {
    klog("user fault pid %d (%s) rip=%lx\n", cur->pid, cur->name, r->rip);
    if (cur->term) {
        char b[80];
        int n = snprintf(b, sizeof b, "\n[%s crashed: vec %d at %lx]\n", cur->name, (int)r->vec, r->rip);
        term_out(cur->term, b, n);
    }
    proc_kill(cur->pid);
}

static u64 *ptwalk(struct task *t, u64 va, int make) {
    u64 *pd = (u64 *)(t->pd1 & ~0xFFFULL);
    u64 i = (va >> 21) & 511;
    if (!(pd[i] & 1)) {
        if (!make) return NULL;
        u64 pt = tpalloc(t);
        if (!pt) return NULL;
        pd[i] = pt | 7;
    }
    u64 *pt = (u64 *)(pd[i] & ~0xFFFULL);
    return &pt[(va >> 12) & 511];
}

static u64 upa(struct task *t, u64 va) {
    if (va < UBASE || va >= UTOP) return 0;
    u64 *e = ptwalk(t, va, 0);
    if (!e || !(*e & 1)) return 0;
    return (*e & PTE_ADDR_MASK) | (va & 0xFFF);
}

static int copy_in(struct task *t, void *dst, u64 va, u64 n) {
    u8 *d = dst;
    while (n) {
        u64 pa = upa(t, va);
        if (!pa) return -1;
        u64 c = 4096 - (va & 0xFFF);
        if (c > n) c = n;
        memcpy(d, (void *)pa, c);
        d += c; va += c; n -= c;
    }
    return 0;
}

static int copy_out(struct task *t, u64 va, const void *src, u64 n) {
    const u8 *s = src;
    while (n) {
        u64 pa = upa(t, va);
        if (!pa) return -1;
        u64 c = 4096 - (va & 0xFFF);
        if (c > n) c = n;
        memcpy((void *)pa, s, c);
        s += c; va += c; n -= c;
    }
    return 0;
}

static int ustr(struct task *t, char *dst, u64 va, int max) {
    for (int i = 0; i < max - 1; i++) {
        if (copy_in(t, dst + i, va + i, 1) < 0) return -1;
        if (!dst[i]) return i;
    }
    dst[max - 1] = 0;
    return -1;
}

static int user_home_path(const char *path) {
    if (!strcasecmp(path, "/home")) return 1;
    if (strncasecmp(path, "/home/", 6) || !path[6]) return 0;
    const char *part = path + 6;
    for (const char *p = part;; p++) {
        if (*p != '/' && *p != 0) continue;
        size_t len = (size_t)(p - part);
        if (!len || len >= 60 || (len == 1 && part[0] == '.') ||
            (len == 2 && part[0] == '.' && part[1] == '.')) return 0;
        if (!*p) return 1;
        part = p + 1;
    }
}

static int user_home_file_target(const char *path, node **dir, const char **name) {
    if (!user_home_path(path) || !strcasecmp(path, "/home")) return 0;
    node *home = vfs_find("/home", NULL);
    if (!home || !home->dir) return 0;

    const char *last = strrchr(path, '/');
    if (!last || !last[1]) return 0;
    node *parent = home;
    if (last != path + 5) {
        char subdir[160];
        size_t len = (size_t)(last - (path + 6));
        if (!len || len >= sizeof subdir) return 0;
        memcpy(subdir, path + 6, len);
        subdir[len] = 0;
        parent = vfs_find(subdir, home);
    }
    if (!parent || !parent->dir) return 0;
    *dir = parent;
    *name = last;
    return 1;
}

static void user_start(void *arg) {
    struct task *t = arg;
    enter_user(t->entry, USTK - 128, USTK - 64);
}

static struct task *mk_task(const char *name, void (*fn)(void *), void *arg) {
    struct task *t = kzalloc(sizeof *t);
    if (!t) return NULL;
    t->kbase = (u64)kalloc(KSTK + 64);
    if (!t->kbase) { kfree(t); return NULL; }
    t->ktop = (t->kbase + KSTK + 15) & ~15ULL;
    u64 *f = (u64 *)(t->ktop - 72);
    f[0] = f[1] = 0;
    f[2] = (u64)arg;
    f[3] = (u64)fn;
    f[4] = f[5] = 0;
    f[6] = (u64)task_tramp;
    t->rsp = (u64)f;
    t->pid = npid++;
    snprintf(t->name, sizeof t->name, "%s", name);
    return t;
}

int proc_spawn(const char *path, const char *arg, void *term) {
    node *n = vfs_find(path, NULL);
    u64 sz;
    u8 *img = vfs_slurp(n, &sz);
    if (!img) return -1;
    struct te *h = (struct te *)img;
    if (sz < sizeof *h || memcmp(h->magic, "TE02", 4) ||
        h->fsize > sz - sizeof *h || h->msize < h->fsize ||
        h->msize > (64u << 20) || h->data_offset > h->msize ||
        (h->data_offset & 4095) || h->entry < UBASE ||
        h->entry >= UBASE + h->data_offset) {
        kfree(img);
        return -2;
    }
    char nm[32];
    snprintf(nm, sizeof nm, "%s", h->name[0] ? h->name : n->name);
    struct task *t = mk_task(nm, user_start, NULL);
    if (!t) { kfree(img); return -3; }
    *(u64 *)(t->rsp + 16) = (u64)t;
    t->user = 1;
    t->term = term;
    t->entry = h->entry;
    if (arg) snprintf(t->arg, sizeof t->arg, "%s", arg);
    u64 pm = tpalloc(t), pp = tpalloc(t), pd = tpalloc(t);
    if (!pm || !pp || !pd) goto fail;
    u64 *m4 = (u64 *)pm, *p3 = (u64 *)pp;
    for (int i = 0; i < 512; i++) m4[i] = pml4[i];
    m4[0] = pp | 7;
    for (int i = 0; i < 4; i++) p3[i] = pdpt[i];
    p3[1] = pd | 7;
    t->cr3 = pm;
    t->pd1 = pd;
    u64 pages = (h->msize + 4095) / 4096;
    for (u64 i = 0; i < pages; i++) {
        u64 pa = tpalloc(t);
        if (!pa) goto fail;
        u64 off = i * 4096;
        if (off < h->fsize) {
            u64 c = h->fsize - off;
            memcpy((void *)pa, img + sizeof *h + off, c > 4096 ? 4096 : c);
        }
        u64 *e = ptwalk(t, UBASE + off, 1);
        if (!e) goto fail;
        *e = pa | (off < h->data_offset ? 5 : (7 | PTE_NX));
    }
    t->hend = UBASE + pages * 4096;
    t->brk = t->hend;
    for (int i = 0; i < 16; i++) {
        u64 pa = tpalloc(t);
        if (!pa) goto fail;
        u64 *e = ptwalk(t, USTK - 16 * 4096 + i * 4096, 1);
        if (!e) goto fail;
        *e = pa | 7 | PTE_NX;
    }
    {
        char a[64];
        snprintf(a, sizeof a, "%s", t->arg);
        copy_out(t, USTK - 64, a, 64);
    }
    kfree(img);
    u64 f = irqsave();
    t->state = S_READY;
    t->next = tlist->next;
    tlist->next = t;
    irqrest(f);
    return t->pid;
fail:
    kfree(img);
    for (int i = 0; i < t->nfr; i++) pfree(t->fr[i]);
    kfree(t->fr);
    kfree((void *)t->kbase);
    kfree(t);
    return -3;
}

void proc_list(void (*cb)(int, const char *, int, u64, void *), void *ctx) {
    for (struct task *n = tlist; ; n = n->next) {
        if (n->state != S_DEAD) cb(n->pid, n->name, n->state, n->pid ? (u64)n->nfr * 4 : 0, ctx);
        if (n->next == tlist) break;
    }
}

u64 proc_cpu(int pid) {
    struct task *t = byid(pid);
    return t ? t->cpu : 0;
}

void proc_reap(void) { reap(); }

static win *uwin(struct task *t, int id) {
    win *w = wm_byid(id);
    return (w && w->owner == t->pid && (w->flags & WF_USER)) ? w : NULL;
}

static void block(void) {
    cur->state = S_BLOCK;
    schedule();
}

long sys_call(struct regs *r) {
    struct task *t = cur;
    u64 a = r->rdi, b = r->rsi, c = r->rdx, d = r->r10, e = r->r8, f = r->r9;
    char s[160];
    switch (r->rax) {
    case 0:
        proc_kill(t->pid);
        return 0;
    case 1: {
        if (c > 4096) c = 4096;
        char buf[4096];
        if (copy_in(t, buf, b, c) < 0) return -1;
        if (t->term) term_out(t->term, buf, (int)c);
        else for (u64 i = 0; i < c; i++) ser_putc(buf[i]);
        return (long)c;
    }
    case 2: {
        if (!t->term) return 0;
        int n = 0;
        while (n == 0) {
            while (t->ih != t->it && n < (int)c) {
                char ch = (char)t->inq[t->ih];
                t->ih = (t->ih + 1) % 64;
                if (copy_out(t, b + n, &ch, 1) < 0) return -1;
                n++;
            }
            if (n) break;
            block();
        }
        return n;
    }
    case 3:
        t->wake = ticks + a / 10 + 1;
        t->state = S_SLEEP;
        schedule();
        return 0;
    case 4:
        schedule();
        return 0;
    case 5: return t->pid;
    case 6: return (long)uptime_ms();
    case 7: {
        if (ustr(t, s, c, sizeof s) < 0) return -1;
        win *w = wm_open(s, (int)a, (int)b, WF_USER | WF_RESIZE);
        if (!w) return -1;
        w->owner = t->pid;
        w->ich = toupper((u8)s[0]);
        w->icol = 0x7C3AED;
        return w->id;
    }
    case 8: {
        win *w = uwin(t, (int)a);
        if (!w) return -1;
        fill(&w->surf, (int)b, (int)c, (int)d, (int)e, (u32)f);
        return 0;
    }
    case 9: {
        win *w = uwin(t, (int)a);
        if (!w || ustr(t, s, e, sizeof s) < 0) return -1;
        text(&w->surf, (int)b, (int)c, s, (u32)d);
        return 0;
    }
    case 10: {
        win *w = uwin(t, (int)a);
        int bx = (int)b, by = (int)c, bw = (int)d, bh = (int)e;
        if (!w || bw <= 0 || bh <= 0 || bw > 4096 || bh > 4096) return -1;
        u32 *row = kalloc((u64)bw * 4);
        if (!row) return -1;
        for (int j = 0; j < bh; j++) {
            if (copy_in(t, row, f + (u64)j * bw * 4, (u64)bw * 4) < 0) { kfree(row); return -1; }
            int yy = by + j;
            if (yy < 0 || yy >= w->h) continue;
            for (int i = 0; i < bw; i++) {
                int xx = bx + i;
                if (xx >= 0 && xx < w->w) w->surf.p[(u64)yy * w->w + xx] = row[i];
            }
        }
        kfree(row);
        return 0;
    }
    case 11: {
        win *w = uwin(t, (int)a);
        if (!w) return -1;
        wm_paint(w, 0, 0, 0, 0);
        return 0;
    }
    case 12: {
        win *w = uwin(t, (int)a);
        if (!w) return -1;
        struct uev ev;
        for (;;) {
            if (wm_pop(w, &ev)) {
                if (copy_out(t, b, &ev, sizeof ev) < 0) return -1;
                return 1;
            }
            if (!c) return 0;
            block();
            if (!uwin(t, (int)a)) return -1;
        }
    }
    case 13: {
        win *w = uwin(t, (int)a);
        if (!w) return -1;
        wm_close(w);
        return 0;
    }
    case 14: {
        win *w = uwin(t, (int)a);
        if (!w || ustr(t, s, b, sizeof s) < 0) return -1;
        wm_title(w, s);
        return 0;
    }
    case 15: {
        win *w = uwin(t, (int)a);
        if (!w) return -1;
        return ((long)w->w << 16) | w->h;
    }
    case 16: {
        node *dir;
        const char *name;
        if (ustr(t, s, a, sizeof s) < 0 ||
            !user_home_file_target(s, &dir, &name)) return -1;
        node *n = vfs_child(dir, name);
        if (!n || n->dir) return -1;
        if (d > (16 << 20)) d = 16 << 20;
        u8 *buf = kalloc(d);
        if (!buf) return -1;
        i64 rd = vfs_rd(n, 0, buf, d);
        if (rd > 0 && copy_out(t, b, buf, rd) < 0) rd = -1;
        kfree(buf);
        return rd;
    }
    case 17: {
        node *dir;
        const char *name;
        if (ustr(t, s, a, sizeof s) < 0 || c > (16 << 20) ||
            !user_home_file_target(s, &dir, &name)) return -1;
        u8 *buf = kalloc(c + 1);
        if (!buf) return -1;
        long rv = -1;
        if (copy_in(t, buf, b, c) == 0) {
            rv = vfs_save(dir, name, buf, c);
        }
        kfree(buf);
        return rv;
    }
    case 18: {
        if (ustr(t, s, a, sizeof s) < 0 || !user_home_path(s)) return -1;
        node *dn = vfs_find(s, NULL);
        if (!dn || !dn->dir) return -1;
        vfs_load(dn);
        node *k = dn->kid;
        for (u64 i = 0; k && i < b; i++) k = k->nxt;
        if (!k) return 0;
        char nm[66];
        snprintf(nm, sizeof nm, "%s", k->name);
        if (copy_out(t, c, nm, strlen(nm) + 1) < 0) return -1;
        return k->dir ? 2 : 1;
    }
    case 19: {
        if (ustr(t, s, a, sizeof s) < 0) return -1;
        char ar[64] = "";
        if (b && ustr(t, ar, b, sizeof ar) < 0) return -1;
        return proc_spawn(s, ar[0] ? ar : NULL, NULL);
    }
    case 20: {
        i64 inc = (i64)a;
        u64 old = t->brk, nb = old + (u64)inc;
        if (inc < 0 || nb > USTK - 0x20000) return -1;
        while (t->hend < nb) {
            u64 pa = tpalloc(t);
            if (!pa) return -1;
            u64 *e = ptwalk(t, t->hend, 1);
            if (!e) return -1;
            *e = pa | 7 | PTE_NX;
            t->hend += 4096;
        }
        t->brk = nb;
        return (long)old;
    }
    default:
        return -38;
    }
    (void)f;
}
