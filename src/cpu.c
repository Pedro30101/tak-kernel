#include "tak.h"
#include "cpu.h"

struct tss {
    u32 r0;
    u64 rsp0, rsp1, rsp2;
    u64 r1;
    u64 ist[7];
    u64 r2;
    u16 r3, iomap;
} __attribute__((packed));

struct gate {
    u16 lo, sel;
    u8 ist, attr;
    u16 mid;
    u32 hi, z;
} __attribute__((packed));

extern u64 isr_table[];
extern u64 isr_table_n;

static u64 gdt[7] __attribute__((aligned(16)));
static struct tss tss;
static struct gate idt[256] __attribute__((aligned(16)));
static void (*irqs[16])(void);
volatile u64 ticks;

void tss_rsp0(u64 sp) { tss.rsp0 = sp; }
u64 uptime_ms(void) { return ticks * 10; }
void irq_set(int n, void (*fn)(void)) { irqs[n] = fn; }

static void setgate(int i, u64 f, u8 attr) {
    idt[i].lo = f & 0xFFFF;
    idt[i].sel = 0x08;
    idt[i].ist = 0;
    idt[i].attr = attr;
    idt[i].mid = (f >> 16) & 0xFFFF;
    idt[i].hi = f >> 32;
    idt[i].z = 0;
}

static void pic_init(void) {
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 4); outb(0xA1, 2);
    outb(0x21, 1); outb(0xA1, 1);
    outb(0x21, 0xF8);
    outb(0xA1, 0xEF);
}

static void pit_init(void) {
    u32 d = 11932;
    outb(0x43, 0x36);
    outb(0x40, d & 0xFF);
    outb(0x40, d >> 8);
}

void cpu_init(void) {
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL;
    gdt[2] = 0x00CF92000000FFFFULL;
    gdt[3] = 0x00CFF2000000FFFFULL;
    gdt[4] = 0x00AFFA000000FFFFULL;
    u64 b = (u64)&tss, l = sizeof tss - 1;
    gdt[5] = (l & 0xFFFF) | ((b & 0xFFFFFF) << 16) | (0x89ULL << 40) | (((l >> 16) & 0xF) << 48) | (((b >> 24) & 0xFF) << 56);
    gdt[6] = b >> 32;
    tss.iomap = sizeof tss;
    struct { u16 l; u64 b; } __attribute__((packed)) gp = { sizeof gdt - 1, (u64)gdt };
    __asm__ volatile("lgdt %0" : : "m"(gp));
    __asm__ volatile(
        "pushq $0x08\n lea 1f(%%rip),%%rax\n pushq %%rax\n lretq\n 1:\n"
        "mov $0x10,%%ax\n mov %%ax,%%ds\n mov %%ax,%%es\n mov %%ax,%%ss\n" : : : "rax", "memory");
    __asm__ volatile("ltr %%ax" : : "a"(0x28));

    for (u64 i = 0; i < isr_table_n; i++) {
        int v = i < 48 ? (int)i : 0x80;
        setgate(v, isr_table[i], v == 0x80 ? 0xEF : 0x8E);
    }
    struct { u16 l; u64 b; } __attribute__((packed)) ip = { sizeof idt - 1, (u64)idt };
    __asm__ volatile("lidt %0" : : "m"(ip));
    pic_init();
    pit_init();
}

static const char *exc_name[] = {
    "divide error", "debug", "nmi", "breakpoint", "overflow", "bound", "invalid opcode", "no fpu",
    "double fault", "", "bad tss", "no segment", "stack fault", "general protection", "page fault", "",
    "fpu error", "alignment", "machine check", "simd"
};

void panic(const char *f, ...) {
    char b[256];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    cli();
    klog("\nPANIC: %s\n", b);
    gfx_panic(b);
    for (;;) __asm__ volatile("hlt");
}

static void exception(struct regs *r) {
    u64 cr2;
    __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
    const char *n = r->vec < 20 ? exc_name[r->vec] : "exception";
    klog("EXC %d %s err=%lx rip=%lx cr2=%lx cs=%lx rsp=%lx\n", (int)r->vec, n, r->err, r->rip, cr2, r->cs, r->rsp);
    if ((r->cs & 3) == 3) {
        proc_fault(r);
        return;
    }
    u64 *sp = (u64 *)r->rsp;
    for (int i = 0; i < 40; i++) {
        u64 v = sp[i];
        if (v >= 0x100000 && v < 0x200000) klog("  stack[%d] = %lx\n", i, v);
    }
    panic("%s (vec %d) err=%lx\nrip=%lx cr2=%lx rsp=%lx\nrax=%lx rbx=%lx rcx=%lx rdx=%lx", n, (int)r->vec, r->err, r->rip, cr2, r->rsp, r->rax, r->rbx, r->rcx, r->rdx);
}

void isr_handler(struct regs *r) {
    u64 v = r->vec;
    if (v < 32) { exception(r); return; }
    if (v == 0x80) { r->rax = (u64)sys_call(r); return; }
    int irq = (int)v - 32;
    if (irq == 7 || irq == 15) {
        outb(irq == 15 ? 0xA0 : 0x20, 0x0B);
        if (!(inb(irq == 15 ? 0xA0 : 0x20) & 0x80)) {
            if (irq == 15) outb(0x20, 0x20);
            return;
        }
    }
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
    if (irq == 0) {
        ticks++;
        sched_tick((r->cs & 3) == 3);
    } else if (irqs[irq]) {
        irqs[irq]();
    }
}
