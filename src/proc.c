#include "tak.h"
#include "auth.h"
#include "cpu.h"
#include "wm.h"
#include "vfs.h"
#include "proc.h"

#define UBASE 0x40000000ULL
#define UTOP 0x80000000ULL
#define USTK 0x7FFF0000ULL
#define LINUX_STACK_PAGES 256
#define KSTK 32768
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

enum { S_READY, S_RUN, S_BLOCK, S_SLEEP, S_DEAD };

struct task {
    int pid, state, user, elf_image, linux_abi;
    char name[32];
    u64 rsp, kbase, ktop, cr3, wake;
    struct task *next;
    void *term;
    int inq[64], ih, it;
    u64 *fr;
    int nfr, cfr;
    u64 cpu;
    u64 pd1;
    u64 entry, user_rsp, user_arg;
    u64 brk, brk_min, hend, mmap_cursor, fs_base, gs_base, clear_child_tid;
    u8 fxstate[512] __attribute__((aligned(16)));
    char arg[64];
};

struct te {
    char magic[4];
    u32 entry, fsize, msize, data_offset;
    char name[32];
};

struct elf64_ehdr {
    u8 ident[16];
    u16 type, machine;
    u32 version;
    u64 entry, phoff, shoff;
    u32 flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf64_phdr {
    u32 type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
};

struct elf64_dyn {
    i64 tag;
    u64 value;
};

struct elf64_rela {
    u64 offset, info;
    i64 addend;
};

struct elf64_sym {
    u32 name;
    u8 info, other;
    u16 shndx;
    u64 value, size;
};

struct elf_object {
    const u8 *img;
    u64 size;
    const struct elf64_ehdr *eh;
    const struct elf64_phdr *ph;
    u64 bias, min_page, max_page, align;
    u64 dynamic_off, dynamic_size;
    u64 strtab, strsz, symtab, syment, symcount;
    u64 rela, relasz, relaent, jmprel, pltrelsz, pltrel;
    char name[64];
    int has_dynamic;
};

#define ELF_PT_LOAD 1
#define ELF_PT_DYNAMIC 2
#define ELF_PT_INTERP 3
#define ELF_PT_PHDR 6
#define ELF_PF_X 1
#define ELF_PF_W 2
#define ELF_ET_EXEC 2
#define ELF_ET_DYN 3
#define ELF_EM_X86_64 62
#define ELF_DT_NULL 0
#define ELF_DT_NEEDED 1
#define ELF_DT_HASH 4
#define ELF_DT_STRTAB 5
#define ELF_DT_SYMTAB 6
#define ELF_DT_RELA 7
#define ELF_DT_RELASZ 8
#define ELF_DT_RELAENT 9
#define ELF_DT_STRSZ 10
#define ELF_DT_SYMENT 11
#define ELF_DT_REL 17
#define ELF_DT_RELSZ 18
#define ELF_DT_RELENT 19
#define ELF_DT_PLTRELSZ 2
#define ELF_DT_JMPREL 23
#define ELF_DT_PLTREL 20
#define ELF_DT_GNU_HASH 0x6FFFFEF5
#define ELF_DT_TEXTREL 22
#define ELF_STB_LOCAL 0
#define ELF_STB_GLOBAL 1
#define ELF_STB_WEAK 2
#define ELF_STV_DEFAULT 0
#define ELF_STV_PROTECTED 3
#define ELF_R_X86_64_RELATIVE 8
#define ELF_R_X86_64_64 1
#define ELF_R_X86_64_GLOB_DAT 6
#define ELF_R_X86_64_JUMP_SLOT 7
#define ELF_MAX_OBJECTS 8

extern u64 pml4[], pdpt[];

#define PTE_NX (1ULL << 63)
#define PTE_SW_PROTNONE (1ULL << 9)

static void reap(void);
static int linux_random(u8 *out, u64 length);
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
    cpu_fxstate_init(t0.fxstate);
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
    cpu_set_user_tls(pick->fs_base, pick->gs_base);
    cpu_fxstate_save(old->fxstate);
    cpu_fxstate_restore(pick->fxstate);
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

static void untrack_frame(struct task *t, u64 pa) {
    for (int i = t->nfr - 1; i >= 0; i--) {
        if (t->fr[i] != pa) continue;
        t->fr[i] = t->fr[--t->nfr];
        pfree(pa);
        return;
    }
}

static int linux_range(u64 addr, u64 len, u64 *end) {
    if (!len || addr < UBASE || addr >= UTOP || len > UTOP - addr)
        return 0;
    *end = addr + len;
    return 1;
}

static int linux_map_pages(struct task *t, u64 addr, u64 len, u64 prot) {
    u64 end;
    if (!linux_range(addr, len, &end) || (addr & 4095) || (len & 4095))
        return -1;
    u64 va;
    for (va = addr; va < end; va += 4096) {
        u64 *pte = ptwalk(t, va, 0);
        if (pte && ((*pte & 1) || (*pte & PTE_SW_PROTNONE))) return -1;
    }
    for (va = addr; va < end; va += 4096) {
        u64 pa = tpalloc(t);
        u64 *pte = ptwalk(t, va, 1);
        if (!pa || !pte) {
            if (pa) untrack_frame(t, pa);
            for (u64 undo = addr; undo < va; undo += 4096) {
                u64 *old = ptwalk(t, undo, 0);
                if (old && ((*old & 1) || (*old & PTE_SW_PROTNONE))) {
                    u64 oldpa = *old & PTE_ADDR_MASK;
                    *old = 0;
                    untrack_frame(t, oldpa);
                }
            }
            return -1;
        }
        memset((void *)pa, 0, 4096);
        *pte = prot ? pa | 5 | ((prot & 2) ? 2 : 0) |
                      ((prot & 4) ? 0 : PTE_NX) :
                      pa | PTE_SW_PROTNONE;
    }
    return 0;
}

static int linux_unmap_pages(struct task *t, u64 addr, u64 len) {
    u64 end;
    if (!linux_range(addr, len, &end) || (addr & 4095) || (len & 4095))
        return -1;
    for (u64 va = addr; va < end; va += 4096) {
        u64 *pte = ptwalk(t, va, 0);
        if (pte && ((*pte & 1) || (*pte & PTE_SW_PROTNONE))) {
            u64 pa = *pte & PTE_ADDR_MASK;
            *pte = 0;
            untrack_frame(t, pa);
            __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
        }
    }
    return 0;
}

static int linux_pages_free(struct task *t, u64 addr, u64 len) {
    u64 end;
    if (!linux_range(addr, len, &end)) return 0;
    for (u64 va = addr; va < end; va += 4096) {
        u64 *pte = ptwalk(t, va, 0);
        if (pte && ((*pte & 1) || (*pte & PTE_SW_PROTNONE))) return 0;
    }
    return 1;
}

static int file_range(u64 off, u64 len, u64 size) {
    return off <= size && len <= size - off;
}

static int elf_file_offset(const struct elf64_phdr *ph, u16 n, u64 va,
                           u64 len, u64 *off) {
    for (u16 i = 0; i < n; i++) {
        if (ph[i].type != ELF_PT_LOAD || va < ph[i].vaddr) continue;
        u64 delta = va - ph[i].vaddr;
        if (delta <= ph[i].filesz && len <= ph[i].filesz - delta) {
            *off = ph[i].offset + delta;
            return 1;
        }
    }
    return 0;
}

static int elf_vaddr_file_offset(const struct elf_object *obj, u64 va,
                                 u64 len, u64 *off) {
    return va <= UTOP && len <= UTOP - va &&
           elf_file_offset(obj->ph, obj->eh->phnum, va, len, off) &&
           file_range(*off, len, obj->size);
}

static int elf_read_vaddr(const struct elf_object *obj, u64 va,
                          void *dst, u64 len) {
    u64 off;
    if (!elf_vaddr_file_offset(obj, va, len, &off)) return -1;
    memcpy(dst, obj->img + off, len);
    return 0;
}

static int elf_obj_init(struct elf_object *obj, const u8 *img, u64 size) {
    if (size < sizeof(struct elf64_ehdr) || size > (64u << 20)) return -1;
    memset(obj, 0, sizeof *obj);
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)img;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L' || eh->ident[3] != 'F' ||
        eh->ident[4] != 2 || eh->ident[5] != 1 || eh->ident[6] != 1 ||
        (eh->type != ELF_ET_EXEC && eh->type != ELF_ET_DYN) ||
        eh->machine != ELF_EM_X86_64 || eh->version != 1 ||
        eh->ehsize != sizeof *eh || eh->phentsize != sizeof(struct elf64_phdr) ||
        !eh->phnum || eh->phnum > 64 ||
        !file_range(eh->phoff, (u64)eh->phnum * eh->phentsize, size)) return -1;

    obj->img = img;
    obj->size = size;
    obj->eh = eh;
    obj->ph = (const struct elf64_phdr *)(img + eh->phoff);
    obj->min_page = UTOP;
    obj->align = 4096;
    int loads = 0;
    for (u16 i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr *p = &obj->ph[i];
        if (p->type != ELF_PT_LOAD || !p->memsz) continue;
        if (p->filesz > p->memsz || !file_range(p->offset, p->filesz, size) ||
            p->vaddr > UTOP || p->memsz > UTOP - p->vaddr ||
            ((p->flags & (ELF_PF_W | ELF_PF_X)) == (ELF_PF_W | ELF_PF_X)) ||
            (p->align > 1 && ((p->align & (p->align - 1)) || p->align > (2u << 20))) ||
            (p->align > 1 && (p->vaddr - p->offset) % p->align)) return -1;
        u64 start = p->vaddr & ~0xFFFULL;
        u64 end = (p->vaddr + p->memsz + 0xFFF) & ~0xFFFULL;
        if (end < start) return -1;
        if (p->align > obj->align) obj->align = p->align;
        for (u16 j = 0; j < i; j++) {
            const struct elf64_phdr *q = &obj->ph[j];
            if (q->type != ELF_PT_LOAD || !q->memsz) continue;
            u64 qstart = q->vaddr & ~0xFFFULL;
            u64 qend = (q->vaddr + q->memsz + 0xFFF) & ~0xFFFULL;
            if (start < qend && qstart < end &&
                ((p->flags ^ q->flags) & (ELF_PF_W | ELF_PF_X))) return -1;
        }
        if (start < obj->min_page) obj->min_page = start;
        if (end > obj->max_page) obj->max_page = end;
        loads++;
    }
    if (!loads || obj->max_page <= obj->min_page ||
        obj->max_page - obj->min_page > (64u << 20)) return -1;

    const struct elf64_phdr *dynamic = NULL;
    for (u16 i = 0; i < eh->phnum; i++) {
        if (obj->ph[i].type == ELF_PT_DYNAMIC) {
            if (dynamic) return -1;
            dynamic = &obj->ph[i];
        }
    }
    if (!dynamic) return 0;
    if (dynamic->filesz > dynamic->memsz ||
        dynamic->filesz % sizeof(struct elf64_dyn) ||
        !file_range(dynamic->offset, dynamic->filesz, size)) return -1;
    obj->has_dynamic = 1;
    obj->dynamic_off = dynamic->offset;
    obj->dynamic_size = dynamic->filesz;
    obj->relaent = sizeof(struct elf64_rela);
    obj->syment = sizeof(struct elf64_sym);
    u64 hash = 0, gnu_hash = 0;
    const struct elf64_dyn *d = (const struct elf64_dyn *)(img + dynamic->offset);
    u64 ndyn = dynamic->filesz / sizeof *d;
    int ended = 0;
    for (u64 i = 0; i < ndyn; i++) {
        switch (d[i].tag) {
        case ELF_DT_NULL: ended = 1; i = ndyn; break;
        case ELF_DT_NEEDED: break;
        case ELF_DT_HASH: hash = d[i].value; break;
        case ELF_DT_GNU_HASH: gnu_hash = d[i].value; break;
        case ELF_DT_STRTAB: obj->strtab = d[i].value; break;
        case ELF_DT_SYMTAB: obj->symtab = d[i].value; break;
        case ELF_DT_STRSZ: obj->strsz = d[i].value; break;
        case ELF_DT_RELA: obj->rela = d[i].value; break;
        case ELF_DT_RELASZ: obj->relasz = d[i].value; break;
        case ELF_DT_RELAENT: obj->relaent = d[i].value; break;
        case ELF_DT_SYMENT: obj->syment = d[i].value; break;
        case ELF_DT_JMPREL: obj->jmprel = d[i].value; break;
        case ELF_DT_PLTRELSZ: obj->pltrelsz = d[i].value; break;
        case ELF_DT_PLTREL: obj->pltrel = d[i].value; break;
        case ELF_DT_REL:
        case ELF_DT_RELSZ:
        case ELF_DT_RELENT:
        case ELF_DT_TEXTREL:
            return -1;
        default: break;
        }
    }
    if (!ended || (obj->rela && obj->relaent != sizeof(struct elf64_rela)) ||
        (obj->relasz && !obj->rela) ||
        (obj->pltrelsz && (!obj->jmprel || obj->pltrel != ELF_DT_RELA)) ||
        obj->relasz % sizeof(struct elf64_rela) ||
        obj->pltrelsz % sizeof(struct elf64_rela)) return -1;
    if (obj->strsz) {
        u64 stroff;
        if (obj->strsz > (16u << 20) ||
            !elf_vaddr_file_offset(obj, obj->strtab, obj->strsz, &stroff))
            return -1;
    }

    if (obj->symtab) {
        if (!obj->strtab || !obj->strsz || obj->syment != sizeof(struct elf64_sym))
            return -1;
        if (hash) {
            u32 hv[2];
            if (elf_read_vaddr(obj, hash, hv, sizeof hv) < 0) return -1;
            obj->symcount = hv[1];
        } else if (gnu_hash) {
            u32 gh[4];
            if (elf_read_vaddr(obj, gnu_hash, gh, sizeof gh) < 0 ||
                !gh[0] || gh[0] > 65536 || gh[1] > 65536 ||
                !gh[2] || gh[2] > 65536 ||
                gnu_hash > UTOP - 16 - (u64)gh[2] * sizeof(u64) -
                           (u64)gh[0] * sizeof(u32)) return -1;
            u64 buckets_va = gnu_hash + 16 + (u64)gh[2] * sizeof(u64);
            u64 chains_va = buckets_va + (u64)gh[0] * sizeof(u32);
            for (u32 b = 0; b < gh[0]; b++) {
                u32 idx;
                if (elf_read_vaddr(obj, buckets_va + (u64)b * 4, &idx, 4) < 0)
                    return -1;
                if (idx < gh[1]) continue;
                for (u32 steps = 0; steps < 65536; steps++, idx++) {
                    u32 chain;
                    u64 chain_va = chains_va + (u64)(idx - gh[1]) * 4;
                    if (elf_read_vaddr(obj, chain_va, &chain, 4) < 0) return -1;
                    if ((u64)idx + 1 > obj->symcount) obj->symcount = (u64)idx + 1;
                    if (chain & 1) break;
                    if (steps == 65535) return -1;
                }
            }
            if (obj->symcount < gh[1]) obj->symcount = gh[1];
        }
        u64 rela_va[2] = { obj->rela, obj->jmprel };
        u64 rela_size[2] = { obj->relasz, obj->pltrelsz };
        for (int table = 0; table < 2; table++) {
            if (!rela_size[table]) continue;
            u64 rela_off;
            if (!elf_vaddr_file_offset(obj, rela_va[table], rela_size[table],
                                       &rela_off)) return -1;
            const struct elf64_rela *rel =
                (const struct elf64_rela *)(obj->img + rela_off);
            for (u64 i = 0; i < rela_size[table] / sizeof *rel; i++) {
                u64 required = (u32)(rel[i].info >> 32) + 1ULL;
                if (required > obj->symcount) obj->symcount = required;
            }
        }
        if (!obj->symcount || obj->symcount > 65536) return -1;
        if (obj->symtab > UTOP ||
            obj->symcount > (UTOP - obj->symtab) /
                            sizeof(struct elf64_sym)) return -1;
        u64 symoff;
        if (!elf_vaddr_file_offset(obj, obj->symtab,
                                   obj->symcount * sizeof(struct elf64_sym),
                                   &symoff)) return -1;
    }
    return 0;
}

static int elf_needed_next(const struct elf_object *obj, u64 *cursor,
                           char *name, u64 cap) {
    if (!obj->has_dynamic) return 0;
    const struct elf64_dyn *d = (const struct elf64_dyn *)(obj->img + obj->dynamic_off);
    u64 count = obj->dynamic_size / sizeof *d;
    while (*cursor < count) {
        const struct elf64_dyn *ent = &d[(*cursor)++];
        if (ent->tag == ELF_DT_NULL) return 0;
        if (ent->tag != ELF_DT_NEEDED) continue;
        if (!obj->strtab || ent->value >= obj->strsz) return -1;
        u64 off;
        if (!elf_vaddr_file_offset(obj, obj->strtab + ent->value, 1, &off))
            return -1;
        u64 i = 0;
        while (i + 1 < cap && ent->value + i < obj->strsz &&
               obj->img[off + i]) {
            name[i] = (char)obj->img[off + i];
            i++;
        }
        if (!i || i + 1 >= cap || ent->value + i >= obj->strsz ||
            obj->img[off + i] || strchr(name, '/') ||
            (name[0] == '.' && (name[1] == 0 ||
                                (name[1] == '.' && name[2] == 0)))) return -1;
        name[i] = 0;
        return 1;
    }
    return -1;
}

static int elf_map_object(struct task *t, struct elf_object *obj) {
    for (u16 i = 0; i < obj->eh->phnum; i++) {
        const struct elf64_phdr *p = &obj->ph[i];
        if (p->type != ELF_PT_LOAD || !p->memsz) continue;
        u64 start = (p->vaddr + obj->bias) & ~0xFFFULL;
        u64 end = (p->vaddr + obj->bias + p->memsz + 0xFFF) & ~0xFFFULL;
        for (u64 va = start; va < end; va += 4096) {
            u64 *pte = ptwalk(t, va, 1);
            if (!pte) return -1;
            if (!(*pte & 1)) {
                u64 pa = tpalloc(t);
                if (!pa) return -1;
                memset((void *)pa, 0, 4096);
                *pte = pa | 7 | PTE_NX;
            }
        }
    }
    for (u16 i = 0; i < obj->eh->phnum; i++) {
        const struct elf64_phdr *p = &obj->ph[i];
        if (p->type != ELF_PT_LOAD || !p->memsz) continue;
        u64 dest = p->vaddr + obj->bias;
        if (p->filesz && copy_out(t, dest, obj->img + p->offset, p->filesz) < 0)
            return -1;
        if (p->memsz > p->filesz) {
            u64 zero = dest + p->filesz, remain = p->memsz - p->filesz;
            while (remain) {
                u64 pa = upa(t, zero);
                if (!pa) return -1;
                u64 chunk = 4096 - (zero & 0xFFF);
                if (chunk > remain) chunk = remain;
                memset((void *)pa, 0, chunk);
                zero += chunk;
                remain -= chunk;
            }
        }
        u64 start = dest & ~0xFFFULL;
        u64 end = (dest + p->memsz + 0xFFF) & ~0xFFFULL;
        for (u64 va = start; va < end; va += 4096) {
            u64 *pte = ptwalk(t, va, 0);
            if (!pte || !(*pte & 1)) return -1;
            if (p->flags & ELF_PF_W) *pte |= 2;
            else *pte &= ~2ULL;
            if (p->flags & ELF_PF_X) *pte &= ~PTE_NX;
            else *pte |= PTE_NX;
        }
    }
    return 0;
}

static int elf_add_signed(u64 base, i64 addend, u64 *result) {
    if (addend < 0) {
        u64 delta = (u64)(-(addend + 1)) + 1;
        if (delta > base) return -1;
        *result = base - delta;
    } else {
        if ((u64)addend > ~0ULL - base) return -1;
        *result = base + (u64)addend;
    }
    return 0;
}

static int elf_names_equal(const struct elf_object *a, u32 a_name,
                           const struct elf_object *b, u32 b_name) {
    if (a_name >= a->strsz || b_name >= b->strsz) return -1;
    u64 aoff, boff;
    if (!elf_vaddr_file_offset(a, a->strtab + a_name, 1, &aoff) ||
        !elf_vaddr_file_offset(b, b->strtab + b_name, 1, &boff)) return -1;
    for (u64 i = 0; a_name + i < a->strsz && b_name + i < b->strsz; i++) {
        char ac = (char)a->img[aoff + i], bc = (char)b->img[boff + i];
        if (ac != bc) return 0;
        if (!ac) return 1;
    }
    return -1;
}

static int elf_resolve_symbol(struct elf_object *objects, u64 nobj,
                              struct elf_object *requester, u32 index,
                              u64 *value) {
    if (!index) { *value = 0; return 0; }
    if (index >= requester->symcount) return -1;
    u64 sym_off;
    if (!elf_vaddr_file_offset(requester, requester->symtab +
                               (u64)index * sizeof(struct elf64_sym),
                               sizeof(struct elf64_sym), &sym_off)) return -1;
    struct elf64_sym ref;
    memcpy(&ref, requester->img + sym_off, sizeof ref);
    if (ref.name >= requester->strsz) return -1;
    u8 binding = ref.info >> 4;
    if (ref.shndx && binding == ELF_STB_LOCAL) {
        if (ref.shndx == 0xFFF1) *value = ref.value;
        else if (ref.value <= UTOP - requester->bias) *value = requester->bias + ref.value;
        else return -1;
        return 0;
    }

    int weak_found = 0;
    u64 weak_value = 0;
    for (u64 o = 0; o < nobj; o++) {
        struct elf_object *obj = &objects[o];
        for (u64 i = 1; i < obj->symcount; i++) {
            u64 off;
            if (!elf_vaddr_file_offset(obj, obj->symtab +
                                       i * sizeof(struct elf64_sym),
                                       sizeof(struct elf64_sym), &off)) return -1;
            struct elf64_sym sym;
            memcpy(&sym, obj->img + off, sizeof sym);
            if (!sym.shndx || sym.name >= obj->strsz) continue;
            u8 bind = sym.info >> 4;
            u8 visibility = sym.other & 3;
            if ((bind != ELF_STB_GLOBAL && bind != ELF_STB_WEAK) ||
                (visibility != ELF_STV_DEFAULT && visibility != ELF_STV_PROTECTED))
                continue;
            int match = elf_names_equal(requester, ref.name, obj, sym.name);
            if (match < 0) return -1;
            if (!match) continue;
            u64 address;
            if (sym.shndx == 0xFFF1) address = sym.value;
            else if (sym.value <= UTOP - obj->bias) address = obj->bias + sym.value;
            else return -1;
            if (bind == ELF_STB_GLOBAL) { *value = address; return 0; }
            weak_found = 1;
            weak_value = address;
        }
    }
    if (weak_found) { *value = weak_value; return 0; }
    if (binding == ELF_STB_WEAK) { *value = 0; return 0; }
    return -1;
}

static int elf_apply_rela(struct task *t, struct elf_object *objects,
                          u64 nobj, struct elf_object *obj,
                          u64 rela, u64 relasz) {
    if (!relasz) return 0;
    if (!rela || relasz % sizeof(struct elf64_rela)) return -1;
    u64 off;
    if (!elf_vaddr_file_offset(obj, rela, relasz, &off)) return -1;
    const struct elf64_rela *r = (const struct elf64_rela *)(obj->img + off);
    for (u64 i = 0; i < relasz / sizeof *r; i++) {
        u32 type = (u32)r[i].info;
        u32 symidx = (u32)(r[i].info >> 32);
        if (obj->bias > UTOP - sizeof(u64) ||
            r[i].offset > UTOP - obj->bias - sizeof(u64)) return -1;
        u64 target = obj->bias + r[i].offset;
        if (target < UBASE || target > UTOP - sizeof(u64)) return -1;
        u64 *pte = ptwalk(t, target, 0);
        u64 pa = upa(t, target);
        if (!pte || !(*pte & 2) || !pa) return -1;
        u64 base = 0, result;
        if (type == ELF_R_X86_64_RELATIVE) {
            if (symidx || elf_add_signed(obj->bias, r[i].addend, &result) < 0)
                return -1;
        } else if (type == ELF_R_X86_64_64 ||
                   type == ELF_R_X86_64_GLOB_DAT ||
                   type == ELF_R_X86_64_JUMP_SLOT) {
            if (elf_resolve_symbol(objects, nobj, obj, symidx, &base) < 0 ||
                elf_add_signed(base, r[i].addend, &result) < 0) return -1;
        } else return -1;
        if (result && (result < UBASE || result >= UTOP)) return -1;
        *(u64 *)pa = result;
    }
    return 0;
}

static int elf_relocate_object(struct task *t, struct elf_object *objects,
                               u64 nobj, struct elf_object *obj) {
    if (!obj->has_dynamic) return 0;
    if (elf_apply_rela(t, objects, nobj, obj, obj->rela, obj->relasz) < 0)
        return -1;
    if (obj->pltrelsz &&
        elf_apply_rela(t, objects, nobj, obj, obj->jmprel, obj->pltrelsz) < 0)
        return -1;
    return 0;
}

static int elf_load(struct task *t, const u8 *img, u64 size, u64 *phdr_out) {
    struct elf_object objects[ELF_MAX_OBJECTS];
    memset(objects, 0, sizeof objects);
    u64 nobj = 1, loaded_size = size;
    int result = -1;
    if (elf_obj_init(&objects[0], img, size) < 0) return -1;

    struct elf_object *main = &objects[0];
    for (u16 i = 0; i < main->eh->phnum; i++)
        if (main->ph[i].type == ELF_PT_INTERP) return -1;
    if (main->eh->type == ELF_ET_DYN) {
        if (main->min_page > UBASE) goto done;
        main->bias = UBASE - main->min_page;
    }
    if (main->min_page + main->bias < UBASE ||
        main->max_page + main->bias > USTK - 0x20000) goto done;
    u64 cursor = main->max_page + main->bias;

    for (u64 oi = 0; oi < nobj; oi++) {
        u64 next = 0;
        char needed[64];
        int found;
        while ((found = elf_needed_next(&objects[oi], &next, needed,
                                         sizeof needed)) > 0) {
            int duplicate = 0;
            for (u64 j = 1; j < nobj; j++)
                if (!strcmp(objects[j].name, needed)) duplicate = 1;
            if (duplicate) continue;
            if (nobj == ELF_MAX_OBJECTS) goto done;
            char path[96];
            snprintf(path, sizeof path, "/lib/%s", needed);
            node *lib = vfs_find(path, NULL);
            if (!lib || lib->dir || lib->size > (16u << 20) ||
                loaded_size > (32u << 20) - lib->size) goto done;
            u64 lib_size;
            u8 *lib_img = vfs_slurp(lib, &lib_size);
            if (!lib_img) goto done;
            if (lib_size > (16u << 20) ||
                loaded_size > (32u << 20) - lib_size) {
                kfree(lib_img);
                goto done;
            }
            struct elf_object *obj = &objects[nobj];
            if (elf_obj_init(obj, lib_img, lib_size) < 0 ||
                obj->eh->type != ELF_ET_DYN) {
                kfree(lib_img);
                goto done;
            }
            snprintf(obj->name, sizeof obj->name, "%s", needed);
            if (cursor > UTOP - 0x10000 - obj->align) {
                kfree(lib_img);
                goto done;
            }
            u64 base = (cursor + 0x10000 + obj->align - 1) & ~(obj->align - 1);
            if (base < obj->min_page) {
                kfree(lib_img);
                goto done;
            }
            obj->bias = base - obj->min_page;
            if (obj->max_page + obj->bias > USTK - 0x20000) {
                kfree(lib_img);
                goto done;
            }
            cursor = obj->max_page + obj->bias;
            loaded_size += lib_size;
            nobj++;
        }
        if (found < 0) goto done;
    }

    for (u64 i = 0; i < nobj; i++)
        if (elf_map_object(t, &objects[i]) < 0) goto done;
    for (u64 i = 0; i < nobj; i++)
        if (elf_relocate_object(t, objects, nobj, &objects[i]) < 0) goto done;

    if (main->eh->entry > UTOP - main->bias) goto done;
    u64 entry = main->eh->entry + main->bias;
    int executable = 0;
    for (u16 i = 0; i < main->eh->phnum; i++) {
        const struct elf64_phdr *p = &main->ph[i];
        if (p->type == ELF_PT_LOAD && (p->flags & ELF_PF_X) &&
            entry >= p->vaddr + main->bias &&
            entry - (p->vaddr + main->bias) < p->memsz) executable = 1;
    }
    if (!executable || entry < UBASE || entry >= UTOP) goto done;
    u64 phdr = 0;
    for (u16 i = 0; i < main->eh->phnum; i++) {
        const struct elf64_phdr *p = &main->ph[i];
        if (p->type == ELF_PT_PHDR) {
            phdr = p->vaddr + main->bias;
        } else if (p->type == ELF_PT_LOAD && main->eh->phoff >= p->offset) {
            u64 delta = main->eh->phoff - p->offset;
            u64 phdr_size = (u64)main->eh->phnum * main->eh->phentsize;
            if (delta <= p->filesz && phdr_size <= p->filesz - delta)
                phdr = p->vaddr + delta + main->bias;
        }
    }
    if (!phdr || phdr >= UTOP || !upa(t, phdr)) goto done;
    t->entry = entry;
    t->hend = cursor;
    t->brk = cursor;
    *phdr_out = phdr;
    result = 0;
done:
    for (u64 i = 1; i < nobj; i++) kfree((void *)objects[i].img);
    return result;
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
    node *home = auth_home_dir();
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
    enter_user(t->entry, t->user_rsp, t->user_arg);
}

static int elf_initial_stack(struct task *t, u64 phdr, u16 phnum) {
    enum { AT_NULL, AT_PHDR = 3, AT_PHENT, AT_PHNUM, AT_PAGESZ,
           AT_BASE, AT_ENTRY = 9, AT_HWCAP = 16, AT_CLKTCK,
           AT_RANDOM = 25, AT_HWCAP2, AT_EXECFN = 31 };
    char name[64];
    snprintf(name, sizeof name, "%s", t->name);
    u64 top = USTK - 64;
    u64 name_addr = top - strlen(name) - 1;
    if (copy_out(t, name_addr, name, strlen(name) + 1) < 0) return -1;
    u8 random[16];
    u64 random_addr = 0;
    if (linux_random(random, sizeof random) == 0) {
        if (name_addr < sizeof random) return -1;
        random_addr = name_addr - sizeof random;
        if (copy_out(t, random_addr, random, sizeof random) < 0) return -1;
    }
    u64 payload_low = random_addr ? random_addr : name_addr;
    u64 arg_addr = 0;
    int argc = 1;
    if (t->arg[0]) {
        u64 len = strlen(t->arg) + 1;
        if (payload_low < len) return -1;
        arg_addr = payload_low - len;
        if (copy_out(t, arg_addr, t->arg, len) < 0) return -1;
        payload_low = arg_addr;
        argc = 2;
    }
    u64 words[32];
    int n = 0;
    words[n++] = argc;
    words[n++] = name_addr;
    if (argc == 2) words[n++] = arg_addr;
    words[n++] = 0;
    words[n++] = 0;
    words[n++] = AT_PHDR; words[n++] = phdr;
    words[n++] = AT_PHENT; words[n++] = sizeof(struct elf64_phdr);
    words[n++] = AT_PHNUM; words[n++] = phnum;
    words[n++] = AT_PAGESZ; words[n++] = 4096;
    words[n++] = AT_BASE; words[n++] = 0;
    words[n++] = AT_ENTRY; words[n++] = t->entry;
    words[n++] = AT_HWCAP;
    words[n++] = (1ULL << 0) | (1ULL << 4) | (1ULL << 8) |
                 (1ULL << 15) | (1ULL << 24) | (1ULL << 25) |
                 (1ULL << 26);
    words[n++] = AT_CLKTCK; words[n++] = 100;
    words[n++] = AT_RANDOM; words[n++] = random_addr;
    words[n++] = AT_HWCAP2; words[n++] = 0;
    words[n++] = AT_EXECFN; words[n++] = name_addr;
    words[n++] = AT_NULL; words[n++] = 0;
    if (payload_low < (u64)n * sizeof(u64)) return -1;
    u64 rsp = (payload_low - (u64)n * sizeof(u64)) & ~15ULL;
    if (copy_out(t, rsp, words, (u64)n * sizeof(u64)) < 0) return -1;
    t->user_rsp = rsp;
    t->user_arg = 0;
    return 0;
}

static struct task *mk_task(const char *name, void (*fn)(void *), void *arg) {
    struct task *t = kzalloc(sizeof *t);
    if (!t) return NULL;
    cpu_fxstate_init(t->fxstate);
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
    int is_elf = sz >= 4 && img[0] == 0x7F && img[1] == 'E' &&
                 img[2] == 'L' && img[3] == 'F';
    struct te *h = (struct te *)img;
    if (!is_elf && (sz < sizeof *h || memcmp(h->magic, "TE02", 4) ||
        h->fsize > sz - sizeof *h || h->msize < h->fsize ||
        h->msize > (64u << 20) || h->data_offset > h->msize ||
        (h->data_offset & 4095) || h->entry < UBASE ||
        h->entry >= UBASE + h->data_offset)) {
        kfree(img);
        return -2;
    }
    char nm[32];
    snprintf(nm, sizeof nm, "%s", !is_elf && h->name[0] ? h->name : n->name);
    struct task *t = mk_task(nm, user_start, NULL);
    if (!t) { kfree(img); return -3; }
    *(u64 *)(t->rsp + 16) = (u64)t;
    t->user = 1;
    t->term = term;
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
    u64 stack_pages = is_elf ? LINUX_STACK_PAGES : 16;
    for (u64 i = 0; i < stack_pages; i++) {
        u64 pa = tpalloc(t);
        if (!pa) goto fail;
        u64 *e = ptwalk(t, USTK - stack_pages * 4096 + i * 4096, 1);
        if (!e) goto fail;
        *e = pa | 7 | PTE_NX;
    }
    if (is_elf) {
        u64 phdr;
        struct elf64_ehdr *eh = (struct elf64_ehdr *)img;
        if (elf_load(t, img, sz, &phdr) < 0) goto fail;
        t->elf_image = 1;
        t->linux_abi = 1;
        t->brk_min = t->brk;
        t->mmap_cursor = (t->hend + 0x10000 + 4095) & ~4095ULL;
        if (elf_initial_stack(t, phdr, eh->phnum) < 0) goto fail;
    } else {
        t->entry = h->entry;
        t->user_rsp = USTK - 128;
        t->user_arg = USTK - 64;
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
    }
    if (!is_elf) {
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

struct linux_timespec {
    i64 sec, nsec;
};

struct linux_timeval {
    i64 sec, usec;
};

struct linux_rlimit {
    u64 current, maximum;
};

struct linux_uname {
    char field[6][65];
};

static long linux_brk(struct task *t, u64 requested) {
    if (!requested) return (long)t->brk;
    if (requested < t->brk_min || requested >= USTK - 0x20000)
        return (long)t->brk;
    if (requested < t->brk) {
        t->brk = requested;
        return (long)t->brk;
    }
    u64 end = (requested + 4095) & ~4095ULL;
    for (u64 va = t->hend; va < end; va += 4096) {
        u64 *pte = ptwalk(t, va, 0);
        if (pte && ((*pte & 1) || (*pte & PTE_SW_PROTNONE)))
            return (long)t->brk;
    }
    while (t->hend < end) {
        u64 pa = tpalloc(t);
        u64 *pte = ptwalk(t, t->hend, 1);
        if (!pa || !pte) {
            if (pa) untrack_frame(t, pa);
            return (long)t->brk;
        }
        memset((void *)pa, 0, 4096);
        *pte = pa | 7 | PTE_NX;
        t->hend += 4096;
    }
    t->brk = requested;
    return (long)t->brk;
}

static long linux_mmap(struct task *t, u64 hint, u64 length, u64 prot,
                       u64 flags, i64 fd, u64 offset) {
    const u64 map_private = 0x02, map_fixed_noreplace = 0x100000;
    const u64 map_anon = 0x20, map_stack = 0x20000;
    const u64 map_noreserve = 0x4000, map_populate = 0x8000;
    const u64 supported = map_private | map_fixed_noreplace | map_anon |
                          map_stack | map_noreserve | map_populate;
    if (!length || length > (64u << 20) || prot > 7 ||
        (flags & ~supported) || (flags & (map_private | map_anon)) !=
        (map_private | map_anon) || fd != -1 || offset)
        return -22;
    if (length > UTOP - 4095) return -12;
    u64 rounded = (length + 4095) & ~4095ULL;
    u64 addr = 0, end;
    if (hint) {
        hint &= ~4095ULL;
        if (linux_range(hint, rounded, &end) &&
            linux_pages_free(t, hint, rounded))
            addr = hint;
        else if (flags & map_fixed_noreplace)
            return -17;
    } else if (flags & map_fixed_noreplace) {
        return -22;
    }
    if (!addr) {
        u64 candidate = (t->mmap_cursor + 4095) & ~4095ULL;
        while (linux_range(candidate, rounded, &end) &&
               !linux_pages_free(t, candidate, rounded))
            candidate += 4096;
        if (!linux_range(candidate, rounded, &end)) return -12;
        addr = candidate;
    }
    if (linux_map_pages(t, addr, rounded, prot) < 0) return -12;
    if (addr + rounded > t->mmap_cursor)
        t->mmap_cursor = addr + rounded + 4096;
    return (long)addr;
}

static long linux_mprotect(struct task *t, u64 addr, u64 length, u64 prot) {
    u64 end;
    if (!length || (addr & 4095) || prot > 7 ||
        length > UTOP - 4095)
        return -22;
    length = (length + 4095) & ~4095ULL;
    if (!linux_range(addr, length, &end)) return -12;
    for (u64 va = addr; va < end; va += 4096) {
        u64 *pte = ptwalk(t, va, 0);
        if (!pte || (!(*pte & 1) && !(*pte & PTE_SW_PROTNONE)))
            return -12;
    }
    for (u64 va = addr; va < end; va += 4096) {
        u64 *pte = ptwalk(t, va, 0);
        u64 pa = *pte & PTE_ADDR_MASK;
        *pte = prot ? pa | 5 | ((prot & 2) ? 2 : 0) |
                      ((prot & 4) ? 0 : PTE_NX) :
                      pa | PTE_SW_PROTNONE;
        __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    }
    return 0;
}

static int linux_random(u8 *out, u64 length) {
    u32 a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    (void)b; (void)d;
    if (!(c & (1u << 30))) return -1;
    while (length) {
        u64 value;
        u8 ready;
        int ok = 0;
        for (int i = 0; i < 10; i++) {
            __asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ready));
            if (ready) { ok = 1; break; }
        }
        if (!ok) return -1;
        u64 count = length < sizeof value ? length : sizeof value;
        memcpy(out, &value, count);
        out += count;
        length -= count;
    }
    return 0;
}

static long linux_read(struct task *t, u64 fd, u64 dst, u64 count) {
    if (fd != 0) return -9;
    if (count > 4096) count = 4096;
    if (!count) return 0;
    if (!t->term) return 0;
    u64 done = 0;
    while (!done) {
        while (t->ih != t->it && done < count) {
            char ch = (char)t->inq[t->ih];
            if (copy_out(t, dst + done, &ch, 1) < 0)
                return done ? (long)done : -14;
            t->ih = (t->ih + 1) % 64;
            done++;
        }
        if (done) break;
        block();
    }
    return (long)done;
}

static long linux_write(struct task *t, u64 fd, u64 src, u64 count) {
    if (fd != 1 && fd != 2) return -9;
    if (count > (1u << 20)) count = 1u << 20;
    char buf[256];
    u64 done = 0;
    while (done < count) {
        u64 n = count - done;
        if (n > sizeof buf) n = sizeof buf;
        if (copy_in(t, buf, src + done, n) < 0)
            return done ? (long)done : -14;
        if (t->term) term_out(t->term, buf, (int)n);
        else for (u64 i = 0; i < n; i++) ser_putc(buf[i]);
        done += n;
    }
    return (long)done;
}

static long linux_exit(struct task *t) {
    if (t->clear_child_tid) {
        u32 zero = 0;
        copy_out(t, t->clear_child_tid, &zero, sizeof zero);
    }
    proc_kill(t->pid);
    return 0;
}

long linux_sys_call(struct regs *r) {
    struct task *t = cur;
    if (!t->linux_abi) return -38;
    u64 a = r->rdi, b = r->rsi, c = r->rdx;
    u64 d = r->r10, e = r->r8, f = r->r9;
    switch (r->rax) {
    case 0: return linux_read(t, a, b, c);
    case 1: return linux_write(t, a, b, c);
    case 9: return linux_mmap(t, a, b, c, d, (i64)e, f);
    case 10: return linux_mprotect(t, a, b, c);
    case 11: {
        if (!b || (a & 4095) || b > UTOP - 4095) return -22;
        u64 len = (b + 4095) & ~4095ULL;
        return linux_unmap_pages(t, a, len) < 0 ? -22 : 0;
    }
    case 12: return linux_brk(t, a);
    case 24:
        schedule();
        return 0;
    case 28: {
        if (c != 4 && c != 8) return -22;
        if (!b || (a & 4095) || b > UTOP - 4095) return -22;
        u64 len = (b + 4095) & ~4095ULL, end;
        if (!linux_range(a, len, &end)) return -12;
        for (u64 va = a; va < end; va += 4096) {
            u64 pa = upa(t, va);
            if (!pa) return -12;
            memset((void *)(pa & ~4095ULL), 0, 4096);
        }
        return 0;
    }
    case 35: {
        struct linux_timespec req;
        if (copy_in(t, &req, a, sizeof req) < 0) return -14;
        if (req.sec < 0 || req.nsec < 0 || req.nsec >= 1000000000)
            return -22;
        u64 sub_ticks = ((u64)req.nsec + 9999999) / 10000000;
        if (sub_ticks > ~0ULL - ticks ||
            (u64)req.sec > (~0ULL - ticks - sub_ticks) / 100)
            return -22;
        u64 delay = (u64)req.sec * 100 + sub_ticks;
        t->wake = ticks + (delay ? delay : 1);
        t->state = S_SLEEP;
        schedule();
        if (b && copy_out(t, b, &(struct linux_timespec){ 0, 0 },
                          sizeof(struct linux_timespec)) < 0)
            return -14;
        return 0;
    }
    case 39:
    case 186: return t->pid;
    case 60:
    case 231: return linux_exit(t);
    case 63: {
        struct linux_uname u;
        memset(&u, 0, sizeof u);
        snprintf(u.field[0], sizeof u.field[0], "Linux");
        snprintf(u.field[1], sizeof u.field[1], "tak");
        snprintf(u.field[2], sizeof u.field[2], "0.9.6b (0.9.6 Beta)");
        snprintf(u.field[3], sizeof u.field[3], "#1 TAK");
        snprintf(u.field[4], sizeof u.field[4], "x86_64");
        snprintf(u.field[5], sizeof u.field[5], "(none)");
        return copy_out(t, a, &u, sizeof u) < 0 ? -14 : 0;
    }
    case 96:
        return -38;
    case 158: {
        u64 value;
        if (a == 0x1002 || a == 0x1001) {
            if (b && (b < UBASE || b >= UTOP)) return -22;
            if (a == 0x1002) t->fs_base = b;
            else t->gs_base = b;
            cpu_set_user_tls(t->fs_base, t->gs_base);
            return 0;
        }
        if (a == 0x1003 || a == 0x1004) {
            value = a == 0x1003 ? t->fs_base : t->gs_base;
            return copy_out(t, b, &value, sizeof value) < 0 ? -14 : 0;
        }
        return -22;
    }
    case 201: return -38;
    case 218:
        if (a && !upa(t, a)) return -14;
        t->clear_child_tid = a;
        return t->pid;
    case 228:
    case 229: {
        struct linux_timespec ts;
        if (r->rax == 229) {
            if (a > 7) return -22;
            ts.sec = 0;
            ts.nsec = 10000000;
        } else {
            if (a != 1 && a != 4) return -38;
            ts.sec = (i64)(ticks / 100);
            ts.nsec = (i64)(ticks % 100) * 10000000;
        }
        return copy_out(t, b, &ts, sizeof ts) < 0 ? -14 : 0;
    }
    case 273:
        if (b != 24) return -22;
        if (a && !upa(t, a)) return -14;
        return 0;
    case 302: {
        struct linux_rlimit lim;
        if (a && a != (u64)t->pid) return -3;
        if (c) return -1;
        switch (b) {
        case 0: lim.current = lim.maximum = ~0ULL; break;
        case 1: lim.current = lim.maximum = ~0ULL; break;
        case 2: lim.current = lim.maximum = USTK - UBASE; break;
        case 3: lim.current = lim.maximum = LINUX_STACK_PAGES * 4096; break;
        case 4: lim.current = lim.maximum = 0; break;
        case 5: lim.current = lim.maximum = ~0ULL; break;
        case 6: lim.current = lim.maximum = 1; break;
        case 7: lim.current = lim.maximum = 3; break;
        default: return -22;
        }
        return copy_out(t, d, &lim, sizeof lim) < 0 ? -14 : 0;
    }
    case 318:
        if (c & ~3ULL) return -22;
        if (b > (1u << 20)) b = 1u << 20;
        {
        u64 requested = b, done = 0;
        while (b) {
            u8 bytes[64];
            u64 n = b < sizeof bytes ? b : sizeof bytes;
            if (linux_random(bytes, n) < 0)
                return done ? (long)done : -11;
            if (copy_out(t, a, bytes, n) < 0)
                return done ? (long)done : -14;
            a += n;
            b -= n;
            done += n;
        }
        return (long)requested;
        }
    case 334: return -38;
    default: return -38;
    }
}
