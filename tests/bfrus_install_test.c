#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
char *strncpy(char *dest, const char *src, size_t n);
size_t strlen(const char *s);
int strcmp(const char *s1, const char *s2);
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t i64;

struct bfrus;
struct node;

struct fsops {
    i64 (*read)(struct node *n, u64 off, void *buf, u64 len);
    i64 (*write)(struct node *n, u64 off, const void *buf, u64 len);
    int (*load)(struct node *dir);
    int (*create)(struct node *dir, const char *name, int isdir, struct node *out);
    int (*unlink)(struct node *n);
};

struct node {
    char name[64];
    u8 dir, loaded;
    u64 size, cap;
    u8 *data;
    struct node *par, *kid, *nxt;
    const struct fsops *ops;
    u64 priv, priv2;
    void *fs;
};

int bfrus_format(int d, u64 start, u64 size, const char *label);
struct bfrus *bfrus_mount(int d, u64 start, u64 size, struct node *at);
i64 bfrus_file_write(struct node *n, u64 off, const void *buf, u64 len);
i64 bfrus_file_read(struct node *n, u64 off, void *buf, u64 len);
int bfrus_file_create(struct node *dir, const char *name, int isdir, struct node *out);

void *kalloc(size_t size) { return malloc(size); }
void kfree(void *ptr) { free(ptr); }
void *kzalloc(size_t size) { return calloc(1, size); }

static u8 disk_img[256 * 1024 * 1024];
static int fake_disk = 0;
int disk_read(int d, u64 lba, u32 n, void *buf) {
    if (d != fake_disk) return -1;
    if (lba + n > 256 * 1024 * 1024 / 512) return -1;
    memcpy(buf, disk_img + lba * 512, n * 512);
    return 0;
}
int disk_write(int d, u64 lba, u32 n, const void *buf) {
    if (d != fake_disk) return -1;
    if (lba + n > 256 * 1024 * 1024 / 512) return -1;
    memcpy(disk_img + lba * 512, buf, n * 512);
    return 0;
}
int rtc_get(int *h, int *m, int *s, int *dd, int *mo, int *y) {
    *h = 12; *m = 0; *s = 0; *dd = 1; *mo = 1; *y = 2026;
    return 0;
}

struct node *vfs_add(struct node *dir, const char *name, int isdir) {
    struct node *n = calloc(1, sizeof(struct node));
    if (!n) return NULL;
    strncpy(n->name, name, 63);
    n->name[63] = 0;
    n->dir = isdir;
    n->par = dir;
    struct node **pp = &dir->kid;
    while (*pp) pp = &(*pp)->nxt;
    *pp = n;
    return n;
}

static struct node *vfs_child(struct node *dir, const char *name) {
    if (!dir || !dir->dir) return NULL;
    for (struct node *k = dir->kid; k; k = k->nxt)
        if (!strcmp(k->name, name)) return k;
    return NULL;
}

static struct node *vfs_mk(struct node *dir, const char *name, int isdir) {
    if (!dir || !dir->dir || !*name || strlen(name) >= 60) return NULL;
    if (vfs_child(dir, name)) return NULL;
    struct node *n = vfs_add(dir, name, isdir);
    if (!n) return NULL;
    if (dir->ops && dir->ops->create &&
        dir->ops->create(dir, name, isdir, n) < 0) {
        free(n);
        return NULL;
    }
    return n;
}

int main(void) {
    fake_disk = 0;
    u64 psize = 32 * 1024 * 1024 / 512;  /* 32 MB in sectors */
    u64 esp_sectors = 262144ULL;
    u64 pstart = 2048ULL;

    /* Simulate install.c step 1: format */
    if (bfrus_format(0, pstart + esp_sectors, psize, "ASTROS") < 0) {
        printf("FAIL: bfrus_format\n");
        return 1;
    }
    printf("PASS: bfrus_format (32MB volume)\n");

    /* Simulate install.c step 2: mount and create directories */
    struct node root = {0};
    root.dir = 1;
    root.priv = 1;
    root.fs = NULL;

    struct bfrus *fs = bfrus_mount(0, pstart + esp_sectors, psize, &root);
    if (!fs) {
        printf("FAIL: bfrus_mount\n");
        return 1;
    }
    printf("PASS: bfrus_mount\n");

    struct node *boot = vfs_mk(&root, "boot", 1);
    if (!boot) { printf("FAIL: vfs_mk boot\n"); return 1; }
    printf("PASS: created /boot\n");

    struct node *grub = vfs_mk(boot, "grub", 1);
    if (!grub) { printf("FAIL: vfs_mk grub\n"); return 1; }
    printf("PASS: created /boot/grub\n");

    /* Simulate install.c step 3: write kernel.elf (~297KB) */
    u64 kimg_len = 297960;
    u8 *kimg = calloc(1, kimg_len);
    if (!kimg) { printf("FAIL: alloc kimg\n"); return 1; }
    memset(kimg, 0x42, kimg_len);

    struct node *kf = vfs_mk(boot, "kernel.elf", 0);
    if (!kf) { printf("FAIL: vfs_mk kernel.elf\n"); return 1; }
    if (bfrus_file_write(kf, 0, kimg, kimg_len) < 0) {
        printf("FAIL: bfrus_file_write kernel.elf (%llu bytes)\n", (unsigned long long)kimg_len);
        return 1;
    }
    kf->size = kimg_len;
    printf("PASS: wrote kernel.elf (%llu bytes)\n", (unsigned long long)kimg_len);
    free(kimg);

    /* Simulate install.c step 4: write initrd.tar (~2MB) */
    u64 initrd_len = 2109440;
    u8 *initrd = calloc(1, initrd_len);
    if (!initrd) { printf("FAIL: alloc initrd\n"); return 1; }
    memset(initrd, 0x43, initrd_len);

    struct node *if_node = vfs_mk(boot, "initrd.tar", 0);
    if (!if_node) { printf("FAIL: vfs_mk initrd.tar\n"); return 1; }
    if (bfrus_file_write(if_node, 0, initrd, initrd_len) < 0) {
        printf("FAIL: bfrus_file_write initrd.tar (%llu bytes)\n", (unsigned long long)initrd_len);
        return 1;
    }
    if_node->size = initrd_len;
    printf("PASS: wrote initrd.tar (%llu bytes)\n", (unsigned long long)initrd_len);
    free(initrd);

    /* Simulate install.c step 5: write grub.cfg */
    const char *cfg = "set timeout=1\nset default=0\ninsmod all_video\nmenuentry \"Astros\" {\n    multiboot2 /boot/kernel.elf\n    module2 /boot/initrd.tar initrd\n    boot\n}\n";
    struct node *cf = vfs_mk(grub, "grub.cfg", 0);
    if (!cf) { printf("FAIL: vfs_mk grub.cfg\n"); return 1; }
    if (bfrus_file_write(cf, 0, cfg, strlen(cfg)) < 0) {
        printf("FAIL: bfrus_file_write grub.cfg\n");
        return 1;
    }
    cf->size = strlen(cfg);
    printf("PASS: wrote grub.cfg\n");

    /* Unmount and remount to verify persistence */
    free(fs);
    root.kid = NULL;
    root.loaded = 0;

    struct bfrus *fs2 = bfrus_mount(0, pstart + esp_sectors, psize, &root);
    if (!fs2) { printf("FAIL: bfrus_mount remount\n"); return 1; }
    printf("PASS: remount\n");

    /* Verify kernel.elf */
    struct node *kf2 = vfs_child(vfs_child(&root, "boot"), "kernel.elf");
    if (!kf2) { printf("FAIL: kernel.elf not found after remount\n"); return 1; }
    kf2->fs = fs2;
    u8 kbuf[4096];
    i64 r = bfrus_file_read(kf2, 0, kbuf, sizeof(kbuf));
    if (r < 0 || kbuf[0] != 0x42) {
        printf("FAIL: kernel.elf data mismatch after remount\n");
        return 1;
    }
    printf("PASS: kernel.elf data intact after remount\n");

    /* Verify initrd.tar */
    struct node *if2 = vfs_child(vfs_child(&root, "boot"), "initrd.tar");
    if (!if2) { printf("FAIL: initrd.tar not found after remount\n"); return 1; }
    if2->fs = fs2;
    u8 ibuf[4096];
    r = bfrus_file_read(if2, 0, ibuf, sizeof(ibuf));
    if (r < 0 || ibuf[0] != 0x43) {
        printf("FAIL: initrd.tar data mismatch after remount\n");
        return 1;
    }
    printf("PASS: initrd.tar data intact after remount\n");

    /* Verify grub.cfg */
    struct node *cf2 = vfs_child(vfs_child(vfs_child(&root, "boot"), "grub"), "grub.cfg");
    if (!cf2) { printf("FAIL: grub.cfg not found after remount\n"); return 1; }
    cf2->fs = fs2;
    u8 cbuf[512];
    r = bfrus_file_read(cf2, 0, cbuf, sizeof(cbuf));
    if (r < 0 || memcmp(cbuf, cfg, strlen(cfg)) != 0) {
        printf("FAIL: grub.cfg data mismatch after remount\n");
        return 1;
    }
    printf("PASS: grub.cfg data intact after remount\n");

    free(fs2);
    printf("\nAll installer simulation tests passed.\n");
    return 0;
}
