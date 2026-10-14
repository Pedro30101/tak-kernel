#include "tak.h"
#include "wm.h"
#include "vfs.h"
#include "proc.h"
#include "cpu.h"
#include "input.h"
#include "gfx.h"
#include "disk.h"
#include "login.h"

int live;
void apps_boot(void);
void gpu_init(void);
void disk_init(void);
int audio_init(void);

static int has_live(const char *s) {
    for (; *s; s++) if (!strncmp(s, "live", 4)) return 1;
    return 0;
}

static void vga_msg(const char *m) {
    volatile u16 *v = (volatile u16 *)0xB8000;
    for (int i = 0; i < 80 * 25; i++) v[i] = 0x0720;
    for (int i = 0; m[i]; i++) v[i] = 0x0C00 | (u8)m[i];
}

void kmain(u64 magic, u64 info) {
    ser_init();
    klog("\nAstros / TAK Kernel " VER " booting\n");
    mbi = (u32 *)info;
    if (magic != 0x36D76289) { vga_msg("Astros needs a Multiboot2 bootloader (GRUB)."); for (;;) __asm__ volatile("hlt"); }
    mem_init();
    klog("mem: %lu KB usable, %lu KB free, heap free %lu KB\n", mem_total_kb(), mem_free_kb(), heap_free_kb());
    cpu_init();
    vfs_init();
    u8 *cl = mb_tag(1);
    if (cl && has_live((char *)cl + 8)) live = 1;
    u64 ms, me;
    const char *nm;
    for (int i = 0; mb_mod(i, &ms, &me, &nm); i++) {
        klog("module %d: %s %lx-%lx\n", i, nm, ms, me);
        if (!strncmp(nm, "initrd", 6)) { initrd_ptr = (u8 *)ms; initrd_len = me - ms; vfs_untar(initrd_ptr, initrd_len); }
        if (!strncmp(nm, "kimg", 4)) { kimg_ptr = (u8 *)ms; kimg_len = me - ms; }
    }
    if (!gfx_init()) {
        vga_msg("Astros needs a graphical framebuffer from GRUB. Boot again and do not force text mode.");
        for (;;) __asm__ volatile("hlt");
    }
    proc_init();
    input_init();
    gpu_init();
    audio_init();
    disk_init();
    wm_init();
    sti();
    if (live) apps_boot();
    else if (login_start() < 0) panic("could not start the login screen");
    wm_run();
}
