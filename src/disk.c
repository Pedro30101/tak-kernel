#include "tak.h"
#include "disk.h"

struct disk disks[MAXD];
int ndisks;

static const u16 base[2] = { 0x1F0, 0x170 };
static const u16 ctl[2] = { 0x3F6, 0x376 };

static inline void insw(u16 p, void *b, int n) { __asm__ volatile("rep insw" : "+D"(b), "+c"(n) : "d"(p) : "memory"); }
static inline void outsw(u16 p, const void *b, int n) { __asm__ volatile("rep outsw" : "+S"(b), "+c"(n) : "d"(p)); }

static int wait_busy(int ch) {
    for (int i = 0; i < 400000; i++) {
        u8 s = inb(base[ch] + 7);
        if (!(s & 0x80)) return 0;
    }
    return -1;
}

static int wait_drq(int ch) {
    for (int i = 0; i < 400000; i++) {
        u8 s = inb(base[ch] + 7);
        if (s & 0x21) return -1;
        if (!(s & 0x80) && (s & 8)) return 0;
    }
    return -1;
}

static void select(int ch, int dev) {
    outb(base[ch] + 6, 0xA0 | (dev << 4));
    for (int i = 0; i < 4; i++) inb(ctl[ch]);
}

static void ident(int ch, int dev) {
    if (ndisks >= MAXD) return;
    select(ch, dev);
    outb(base[ch] + 2, 0); outb(base[ch] + 3, 0); outb(base[ch] + 4, 0); outb(base[ch] + 5, 0);
    outb(base[ch] + 7, 0xEC);
    if (inb(base[ch] + 7) == 0) return;
    if (inb(base[ch] + 7) == 0xFF) return;
    if (wait_busy(ch) < 0) return;
    if (inb(base[ch] + 4) || inb(base[ch] + 5)) return;
    if (wait_drq(ch) < 0) return;
    u16 id[256];
    insw(base[ch], id, 256);
    struct disk *d = &disks[ndisks];
    d->chan = ch; d->dev = dev; d->ok = 1;
    d->lba48 = (id[83] >> 10) & 1;
    d->sectors = d->lba48 ? ((u64)id[103] << 48 | (u64)id[102] << 32 | (u64)id[101] << 16 | id[100]) : ((u32)id[61] << 16 | id[60]);
    for (int i = 0; i < 20; i++) { d->model[i * 2] = id[27 + i] >> 8; d->model[i * 2 + 1] = id[27 + i] & 255; }
    d->model[40] = 0;
    for (int i = 39; i >= 0 && d->model[i] == ' '; i--) d->model[i] = 0;
    snprintf(d->name, sizeof d->name, "hd%c", 'a' + ndisks);
    klog("disk %s: %s, %lu sectors\n", d->name, d->model, d->sectors);
    ndisks++;
}

static int io(int di, u64 lba, u32 n, void *buf, int wr) {
    struct disk *d = &disks[di];
    int ch = d->chan;
    while (n) {
        u32 c = n > 128 ? 128 : n;
        int ext = lba + c >= (1ULL << 28) || lba >= (1ULL << 28);
        if (ext && !d->lba48) return -1;
        if (wait_busy(ch) < 0) return -1;
        if (ext) {
            outb(base[ch] + 6, 0x40 | (d->dev << 4));
            outb(base[ch] + 2, 0); outb(base[ch] + 3, lba >> 24); outb(base[ch] + 4, lba >> 32); outb(base[ch] + 5, lba >> 40);
            outb(base[ch] + 2, c); outb(base[ch] + 3, lba); outb(base[ch] + 4, lba >> 8); outb(base[ch] + 5, lba >> 16);
            outb(base[ch] + 7, wr ? 0x34 : 0x24);
        } else {
            outb(base[ch] + 6, 0xE0 | (d->dev << 4) | ((lba >> 24) & 15));
            outb(base[ch] + 2, c); outb(base[ch] + 3, lba); outb(base[ch] + 4, lba >> 8); outb(base[ch] + 5, lba >> 16);
            outb(base[ch] + 7, wr ? 0x30 : 0x20);
        }
        for (u32 s = 0; s < c; s++) {
            if (wait_drq(ch) < 0) return -1;
            if (wr) outsw(base[ch], (u8 *)buf + s * 512, 256);
            else insw(base[ch], (u8 *)buf + s * 512, 256);
        }
        if (wr) {
            outb(base[ch] + 7, d->lba48 ? 0xEA : 0xE7);
            if (wait_busy(ch) < 0) return -1;
        }
        lba += c; n -= c; buf = (u8 *)buf + c * 512;
    }
    return 0;
}

int disk_read(int d, u64 lba, u32 n, void *buf) { return d >= 0 && d < ndisks ? io(d, lba, n, buf, 0) : -1; }
int disk_write(int d, u64 lba, u32 n, const void *buf) { return d >= 0 && d < ndisks ? io(d, lba, n, (void *)buf, 1) : -1; }

int disk_parts(int d, struct part *out, int max) {
    u8 s[512];
    int n = 0;
    if (disk_read(d, 0, 1, s) < 0) return 0;
    if (s[510] != 0x55 || s[511] != 0xAA) return 0;
    for (int i = 0; i < 4 && n < max; i++) {
        u8 *e = s + 446 + i * 16;
        u32 st = *(u32 *)(e + 8), sz = *(u32 *)(e + 12);
        if (!e[4] || !sz) continue;
        out[n].disk = d; out[n].start = st; out[n].size = sz; out[n].type = e[4]; out[n].boot = e[0];
        n++;
    }
    return n;
}

struct fat *fat_main;
struct bfrus *bfrus_main;
u8 *initrd_ptr, *kimg_ptr;
u64 initrd_len, kimg_len;

void disk_init(void) {
    for (int ch = 0; ch < 2; ch++) {
        outb(ctl[ch], 2);
        for (int dev = 0; dev < 2; dev++) ident(ch, dev);
    }
    if (!ndisks) return;
    node *at = NULL;
    for (int d = 0; d < ndisks && !bfrus_main; d++) {
        struct part p[4];
        int n = disk_parts(d, p, 4);
        for (int i = 0; i < n && !bfrus_main; i++) {
            if (p[i].type != 0xDA) continue;
            if (!at) at = vfs_mk(vroot, "disk", 1);
            if (!at) break;
            bfrus_main = bfrus_mount(d, p[i].start, p[i].size, at);
        }
    }
    if (bfrus_main) return;
    at = NULL;
    for (int d = 0; d < ndisks && !fat_main; d++) {
        struct part p[4];
        int n = disk_parts(d, p, 4);
        for (int i = 0; i < n && !fat_main; i++) {
            if (p[i].type != 0x0B && p[i].type != 0x0C && p[i].type != 0xEF) continue;
            if (!at) at = vfs_mk(vroot, "disk", 1);
            if (!at) break;
            fat_main = fat_mount(d, p[i].start, p[i].size, at);
        }
    }
}
