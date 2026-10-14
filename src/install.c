#include "tak.h"
#include "wm.h"
#include "vfs.h"
#include "disk.h"
#include "input.h"

#define PSTART 2048ULL
#define PMAXSEC (67108864ULL)

struct inst {
    int disk, step, err, page, sel, done;
    int part_mode;
    int manual_mb;
    char msg[96];
    node *tmp;
    u64 psize;
    int pct;
    char log[8][80];
    int nlog;
};

static const char *steps[] = {
    "Checking the disk", "Formatting the partition", "Creating folders", "Copying the kernel",
    "Copying the system image", "Writing boot menu", "Adding UEFI loader", "Installing BIOS boot code", "Writing partition table"
};
#define NSTEP 9

static u8 inst_mbr[512];
static int inst_have_mbr;

static void logm(struct inst *s, const char *m) {
    if (s->nlog == 8) { memmove(s->log, s->log + 1, 7 * 80); s->nlog--; }
    snprintf(s->log[s->nlog++], 80, "%s", m);
}

static int put(struct inst *s, node *dir, const char *name, const void *d, u64 n) {
    if (vfs_save(dir, name, d, n) < 0) { snprintf(s->msg, sizeof s->msg, "Could not write %s", name); return -1; }
    return 0;
}

static node *sub(node *d, const char *n) {
    node *k = vfs_child(d, n);
    return k ? k : vfs_mk(d, n, 1);
}

static int do_step(struct inst *s) {
    struct disk *dk = &disks[s->disk];
    node *boot, *grub, *efi, *eb;
    switch (s->step) {
    case 0:
        if (dk->sectors < 409600) { snprintf(s->msg, sizeof s->msg, "Disk is too small (needs 200 MB)"); return -1; }
        if (!kimg_ptr || !initrd_ptr) { snprintf(s->msg, sizeof s->msg, "System files missing in memory"); return -1; }
        if (s->part_mode == 2 && s->manual_mb >= 200) {
            s->psize = (u64)s->manual_mb * 2048ULL;
            if (s->psize > dk->sectors - PSTART) s->psize = dk->sectors - PSTART;
        } else {
            s->psize = dk->sectors - PSTART;
        }
        if (s->psize > PMAXSEC) s->psize = PMAXSEC;
        return 0;
    case 1:
        if (fat_format(s->disk, PSTART, s->psize, "ASTROS") < 0) { snprintf(s->msg, sizeof s->msg, "Format failed"); return -1; }
        return 0;
    case 2:
        s->tmp = kzalloc(sizeof(node));
        s->tmp->dir = 1;
        strcpy(s->tmp->name, "inst");
        if (!fat_mount(s->disk, PSTART, s->psize, s->tmp)) { snprintf(s->msg, sizeof s->msg, "Could not mount the new volume"); return -1; }
        boot = sub(s->tmp, "boot");
        if (!boot || !sub(boot, "grub")) { snprintf(s->msg, sizeof s->msg, "Could not create /boot"); return -1; }
        efi = sub(s->tmp, "EFI");
        if (efi) sub(efi, "BOOT");
        return 0;
    case 3:
        return put(s, vfs_child(s->tmp, "boot"), "kernel.elf", kimg_ptr, kimg_len);
    case 4:
        return put(s, vfs_child(s->tmp, "boot"), "initrd.tar", initrd_ptr, initrd_len);
    case 5: {
        static const char cfg[] =
            "set timeout=1\n"
            "set default=0\n"
            "insmod all_video\n"
            "set gfxmode=auto\n"
            "set gfxpayload=keep\n"
            "\n"
            "menuentry \"Astros\" {\n"
            "    multiboot2 /boot/kernel.elf\n"
            "    module2 /boot/initrd.tar initrd\n"
            "    boot\n"
            "}\n";
        grub = vfs_child(vfs_child(s->tmp, "boot"), "grub");
        return put(s, grub, "grub.cfg", cfg, sizeof cfg - 1);
    }
    case 6: {
        node *src = vfs_find("/sys/grub/BOOTX64.EFI", NULL);
        if (!src) { logm(s, "No UEFI loader in this build, skipped"); return 0; }
        u64 n;
        u8 *b = vfs_slurp(src, &n);
        eb = vfs_child(vfs_child(s->tmp, "EFI"), "BOOT");
        int r = b && eb ? put(s, eb, "BOOTX64.EFI", b, n) : -1;
        kfree(b);
        return r;
    }
    case 7: {
        node *bi = vfs_find("/sys/grub/boot.img", NULL), *ci = vfs_find("/sys/grub/core.img", NULL);
        u8 mbr[512];
        memset(mbr, 0, sizeof mbr);
        if (!bi || !ci) { logm(s, "No BIOS boot code in this build, skipped"); s->msg[0] = 0; return 0; }
        u64 bn, cn;
        u8 *b = vfs_slurp(bi, &bn), *c = vfs_slurp(ci, &cn);
        if (!b || !c || bn < 512 || cn < 1024) { kfree(b); kfree(c); snprintf(s->msg, sizeof s->msg, "Bad boot images"); return -1; }
        u32 ns = (u32)((cn + 511) / 512);
        if (ns > 2000) { kfree(b); kfree(c); snprintf(s->msg, sizeof s->msg, "core.img too large"); return -1; }
        u8 *cp = kzalloc((u64)ns * 512);
        memcpy(cp, c, cn);
        *(u64 *)(cp + 500) = 2;
        *(u16 *)(cp + 508) = (u16)(ns - 1);
        *(u16 *)(cp + 510) = 0x820;
        int r = disk_write(s->disk, 1, ns, cp);
        memcpy(mbr, b, 440);
        *(u64 *)(mbr + 0x5C) = 1;
        mbr[0x64] = 0xFF;
        kfree(cp); kfree(b); kfree(c);
        if (r < 0) { snprintf(s->msg, sizeof s->msg, "Could not write core.img"); return -1; }
        memcpy(inst_mbr, mbr, 512);
        inst_have_mbr = 1;
        return 0;
    }
    case 8: {
        u8 *m = inst_mbr;
        if (!inst_have_mbr) memset(m, 0, 512);
        *(u32 *)(m + 440) = (u32)ticks * 2654435761u;
        u8 *e = m + 446;
        memset(e, 0, 64);
        e[0] = 0x80; e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;
        e[4] = 0x0C; e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
        *(u32 *)(e + 8) = (u32)PSTART;
        *(u32 *)(e + 12) = (u32)s->psize;
        m[510] = 0x55; m[511] = 0xAA;
        if (disk_write(s->disk, 0, 1, m) < 0) { snprintf(s->msg, sizeof s->msg, "Could not write the MBR"); return -1; }
        inst_have_mbr = 0;
        kfree(s->tmp);
        s->tmp = NULL;
        return 0;
    }
    }
    return 0;
}

static int run_step(struct inst *s) {
    s->msg[0] = 0;
    int r = do_step(s);
    s->pct = (s->step + 1) * 100 / NSTEP;
    s->step++;
    if (r < 0) { s->err = 1; return -1; }
    if (s->step >= NSTEP) s->done = 1;
    return 0;
}

int install_cli(int disk, void (*say)(const char *)) {
    struct inst *s = kzalloc(sizeof *s);
    s->disk = disk;
    while (!s->done) {
        char b[120];
        snprintf(b, sizeof b, "[%d/%d] %s...", s->step + 1, NSTEP, steps[s->step]);
        say(b);
        if (run_step(s) < 0) { say(s->msg); kfree(s); return -1; }
    }
    kfree(s);
    return 0;
}

static void draw(win *w) {
    struct inst *s = w->priv;
    img *c = &w->surf;
    fill(c, 0, 0, w->w, w->h, C_PANEL);
    fill(c, 0, 0, w->w, 70, 0x111827);
    text2(c, 24, 14, "Tak Installer", C_WHITE);
    text(c, 24, 46, "Astros " VER, C_LRED);
    int y = 96;
    char b[128];

    if (s->page == 0) {
        text2(c, 24, y, "Welcome", C_WHITE);
        text(c, 24, y + 44, "You are running the Astros live session. Nothing is saved yet.", C_TEXT);
        text(c, 24, y + 70, "Install Astros to a disk to keep your files and boot straight", C_DIM);
        text(c, 24, y + 88, "into the desktop next time.", C_DIM);
        ui_btn(c, 24, w->h - 56, 150, 34, "Install Astros", 0);
        ui_btn(c, 190, w->h - 56, 150, 34, "Keep trying", 0);
    } else if (s->page == 1) {
        text2(c, 24, y, "Choose a disk", C_WHITE);
        if (!ndisks) {
            text(c, 24, y + 46, "No supported disk was found.", C_LRED);
            text(c, 24, y + 68, "Astros uses IDE/ATA disks. In VirtualBox, make sure to add an", C_DIM);
            text(c, 24, y + 86, "IDE Controller in VM Storage settings (or set disk to IDE).", C_DIM);
        }
        for (int i = 0; i < ndisks; i++) {
            int yy = y + 48 + i * 46;
            fill(c, 24, yy, w->w - 48, 40, s->sel == i ? C_DRED : 0x1F2937);
            box(c, 24, yy, w->w - 48, 40, s->sel == i ? C_LRED : 0x374151);
            snprintf(b, sizeof b, "%s  -  %s", disks[i].name, disks[i].model[0] ? disks[i].model : "disk");
            text(c, 36, yy + 5, b, C_WHITE);
            snprintf(b, sizeof b, "%lu MB", disks[i].sectors / 2048);
            text(c, 36, yy + 21, b, C_DIM);
        }
        ui_btn(c, 24, w->h - 56, 100, 34, "Back", 0);
        if (ndisks) ui_btn(c, w->w - 148, w->h - 56, 124, 34, "Next", 0);
    } else if (s->page == 2) {
        text2(c, 24, y, "Partitioning method", C_WHITE);
        text(c, 24, y + 36, "Choose how to prepare the disk for Astros:", C_DIM);

        int opt_y[3] = { y + 64, y + 128, y + 192 };
        const char *titles[3] = {
            "Wipe Whole Drive (Tak as main OS)",
            "Automatic Partitioning",
            "Manual Partitioning"
        };
        const char *descs[3] = {
            "Wipes whole drive and puts Tak as main OS. No dual-booting.",
            "Sets up standard boot and main partition automatically.",
            "Customize the size of the Astros root partition manually."
        };

        for (int i = 0; i < 3; i++) {
            int sel = (s->part_mode == i);
            fill(c, 24, opt_y[i], w->w - 48, 52, sel ? C_DRED : 0x1F2937);
            box(c, 24, opt_y[i], w->w - 48, 52, sel ? C_LRED : 0x374151);
            text(c, 40, opt_y[i] + 8, titles[i], sel ? C_WHITE : C_TEXT);
            text(c, 40, opt_y[i] + 28, descs[i], C_DIM);
        }

        if (s->part_mode == 2) {
            char mb[48];
            snprintf(mb, sizeof mb, "Root Size: %d MB", s->manual_mb);
            text(c, 40, y + 256, mb, C_YELLOW);
            ui_btn(c, 230, y + 250, 80, 28, "- 100MB", 0);
            ui_btn(c, 320, y + 250, 80, 28, "+ 100MB", 0);
        }

        ui_btn(c, 24, w->h - 56, 100, 34, "Back", 0);
        ui_btn(c, w->w - 148, w->h - 56, 124, 34, "Next", 0);
    } else if (s->page == 3) {
        text2(c, 24, y, "Ready to install", C_WHITE);
        snprintf(b, sizeof b, "Everything on %s will be erased.", disks[s->sel].name);
        text(c, 24, y + 44, b, C_YELLOW);

        if (s->part_mode == 0) {
            text(c, 24, y + 70, "Mode: Wipe Whole Drive (Tak set as sole operating system)", C_TEXT);
            text(c, 24, y + 90, "Drive will be fully allocated to Astros with GRUB bootloader.", C_DIM);
        } else if (s->part_mode == 1) {
            text(c, 24, y + 70, "Mode: Automatic Partitioning (Standard system volume)", C_TEXT);
            text(c, 24, y + 90, "Standard partition scheme with FAT32 root volume.", C_DIM);
        } else {
            snprintf(b, sizeof b, "Mode: Manual Partitioning (Root volume: %d MB)", s->manual_mb);
            text(c, 24, y + 70, b, C_TEXT);
            text(c, 24, y + 90, "Custom partition size selected.", C_DIM);
        }

        ui_btn(c, 24, w->h - 56, 100, 34, "Back", 0);
        ui_btn(c, w->w - 168, w->h - 56, 144, 34, "Erase & install", 0);
    } else if (s->page == 4) {
        text2(c, 24, y, s->err ? "Installation failed" : (s->done ? "Finishing" : "Installing"), s->err ? C_LRED : C_WHITE);
        fill(c, 24, y + 50, w->w - 48, 22, 0x0B1020);
        box(c, 24, y + 50, w->w - 48, 22, C_RED);
        fill(c, 26, y + 52, (w->w - 52) * s->pct / 100, 18, s->err ? C_LRED : C_GREEN);
        if (s->err) text(c, 24, y + 86, s->msg, C_LRED);
        else if (s->step < NSTEP) { snprintf(b, sizeof b, "%s...", steps[s->step]); text(c, 24, y + 86, b, C_TEXT); }
        for (int i = 0; i < s->nlog; i++) text(c, 24, y + 112 + i * 18, s->log[i], C_DIM);
        if (s->err) ui_btn(c, 24, w->h - 56, 100, 34, "Back", 0);
    } else {
        text2(c, 24, y, "All done", C_GREEN);
        text(c, 24, y + 48, "Astros is installed. Remove the live medium, then restart", C_TEXT);
        text(c, 24, y + 66, "to boot from the disk.", C_TEXT);
        ui_btn(c, 24, w->h - 56, 150, 34, "Restart now", 0);
        ui_btn(c, 190, w->h - 56, 150, 34, "Keep trying", 0);
    }
    wm_paint(w, 0, 0, 0, 0);
}

static void next(win *w) {
    struct inst *s = w->priv;
    if (s->page == 1 && !ndisks) return;
    if (s->page == 3) {
        struct inst keep = *s;
        memset(s, 0, sizeof *s);
        s->disk = keep.sel;
        s->sel = keep.sel;
        s->part_mode = keep.part_mode;
        s->manual_mb = keep.manual_mb;
        s->page = 4;
        draw(w);
        return;
    }
    if (s->page < 5) s->page++;
    draw(w);
}

static void mouse(win *w, int x, int y, int b, int pb, int wheel) {
    (void)wheel;
    struct inst *s = w->priv;
    if (!(b & 1) || (pb & 1)) return;
    int by = w->h - 56;

    if (s->page == 0) {
        if (inside(x, y, 24, by, 150, 34)) next(w);
        else if (inside(x, y, 190, by, 150, 34)) wm_close(w);
    } else if (s->page == 1) {
        for (int i = 0; i < ndisks; i++)
            if (inside(x, y, 24, 96 + 48 + i * 46, w->w - 48, 40)) { s->sel = i; draw(w); }
        if (inside(x, y, 24, by, 100, 34)) { s->page = 0; draw(w); }
        else if (ndisks && inside(x, y, w->w - 148, by, 124, 34)) next(w);
    } else if (s->page == 2) {
        int opt_y[3] = { 96 + 64, 96 + 128, 96 + 192 };
        for (int i = 0; i < 3; i++) {
            if (inside(x, y, 24, opt_y[i], w->w - 48, 52)) {
                s->part_mode = i;
                draw(w);
            }
        }
        if (s->part_mode == 2) {
            if (inside(x, y, 230, 96 + 250, 80, 28)) {
                if (s->manual_mb > 300) s->manual_mb -= 100;
                draw(w);
            } else if (inside(x, y, 320, 96 + 250, 80, 28)) {
                if (s->manual_mb < 32000) s->manual_mb += 100;
                draw(w);
            }
        }
        if (inside(x, y, 24, by, 100, 34)) { s->page = 1; draw(w); }
        else if (inside(x, y, w->w - 148, by, 124, 34)) next(w);
    } else if (s->page == 3) {
        if (inside(x, y, 24, by, 100, 34)) { s->page = 2; draw(w); }
        else if (inside(x, y, w->w - 168, by, 144, 34)) next(w);
    } else if (s->page == 4) {
        if (s->err && inside(x, y, 24, by, 100, 34)) { s->page = 1; s->err = 0; draw(w); }
    } else {
        if (inside(x, y, 24, by, 150, 34)) sys_reboot();
        else if (inside(x, y, 190, by, 150, 34)) wm_close(w);
    }
}

static void key(win *w, int k, int mod) {
    (void)mod;
    struct inst *s = w->priv;
    if (s->page == 1 && k == K_UP && s->sel > 0) { s->sel--; draw(w); }
    else if (s->page == 1 && k == K_DOWN && s->sel < ndisks - 1) { s->sel++; draw(w); }
    else if (s->page == 2 && (k == K_UP || k == K_DOWN)) {
        s->part_mode = (s->part_mode + (k == K_UP ? 2 : 1)) % 3;
        draw(w);
    }
    else if (k == '\n' && s->page < 4) next(w);
}

static void tick(win *w) {
    struct inst *s = w->priv;
    if (s->page != 4 || s->err) return;
    if (s->done) { s->page = 5; draw(w); return; }
    char b[96];
    snprintf(b, sizeof b, "%s", steps[s->step]);
    if (run_step(s) == 0) logm(s, b);
    draw(w);
}

static void on_close(win *w) { kfree(w->priv); }

win *install_open(const char *arg) {
    (void)arg;
    win *w = wm_open("Tak Installer", 640, 480, 0);
    if (!w) return NULL;
    struct inst *s = kzalloc(sizeof *s);
    s->part_mode = 0;
    s->manual_mb = 1024;
    w->priv = s;
    w->on_mouse = mouse;
    w->on_key = key;
    w->on_tick = tick;
    w->on_close = on_close;
    w->ich = 'T';
    w->icol = 0xB91C1C;
    draw(w);
    return w;
}
