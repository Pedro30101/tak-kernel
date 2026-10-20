#include <stdio.h>
#include <stdint.h>

/* Manual declarations - stdlib.h/string.h includes are not seen due to CRLF
   line endings on WSL-mounted files */
void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
char *strncpy(char *dest, const char *src, size_t n);
size_t strlen(const char *s);
int strcmp(const char *s1, const char *s2);
void *memcpy(void *dest, const void *src, size_t n);

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t i64;

struct bfrus;
struct fsops { int dummy; };

struct node {
    char name[64];
    u8 dir, loaded;
    u64 size, cap;
    u8 *data;
    struct node *par, *kid, *nxt;
    void *ops;
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

static u8 disk_img[64 * 1024 * 1024];
static int fake_disk = 0;
int disk_read(int d, u64 lba, u32 n, void *buf) {
    if (d != fake_disk) return -1;
    if (lba + n > 64 * 1024 * 1024 / 512) return -1;
    memcpy(buf, disk_img + lba * 512, n * 512);
    return 0;
}
int disk_write(int d, u64 lba, u32 n, const void *buf) {
    if (d != fake_disk) return -1;
    if (lba + n > 64 * 1024 * 1024 / 512) return -1;
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

int main(void) {
    fake_disk = 0;

    if (bfrus_format(0, 0, 32 * 1024 * 1024 / 512, "TESTVOL") < 0) {
        printf("FAIL: bfrus_format\n");
        return 1;
    }
    printf("PASS: bfrus_format\n");

    struct node root = {0};
    root.dir = 1;
    root.priv = 1;
    root.fs = NULL;

    struct bfrus *fs = bfrus_mount(0, 0, 32 * 1024 * 1024 / 512, &root);
    if (!fs) {
        printf("FAIL: bfrus_mount\n");
        return 1;
    }
    printf("PASS: bfrus_mount\n");

    struct node f = {0};
    const char *msg = "Hello, BFRUS persistence test!";
    printf("DEBUG: about to create file\n");
    fflush(stdout);
    if (bfrus_file_create(&root, "hello.txt", 0, &f) < 0) {
        printf("FAIL: bfrus_file_create\n");
        return 1;
    }
    printf("DEBUG: file created, ino=%llu\n", (unsigned long long)f.priv);
    fflush(stdout);
    if (bfrus_file_write(&f, 0, msg, strlen(msg)) < 0) {
        printf("FAIL: bfrus_file_write\n");
        return 1;
    }
    f.size = strlen(msg);
    printf("PASS: wrote '%s'\n", msg);

    kfree(fs);
    root.kid = NULL;
    root.loaded = 0;

    struct bfrus *fs2 = bfrus_mount(0, 0, 32 * 1024 * 1024 / 512, &root);
    if (!fs2) {
        printf("FAIL: bfrus_mount remount\n");
        return 1;
    }
    printf("PASS: remount\n");

    struct node f2 = {0};
    f2.priv = 2;
    f2.fs = fs2;
    u8 read_buf[256];
    i64 r = bfrus_file_read(&f2, 0, read_buf, sizeof(read_buf));
    if (r < 0) {
        printf("FAIL: bfrus_file_read\n");
        return 1;
    }
    read_buf[r] = 0;
    if (strcmp((char *)read_buf, msg) != 0) {
        printf("FAIL: data mismatch: '%s' != '%s'\n", read_buf, msg);
        return 1;
    }
    printf("PASS: read back '%s'\n", read_buf);

    kfree(fs2);
    root.kid = NULL;
    root.loaded = 0;

    struct bfrus *fs3 = bfrus_mount(0, 0, 32 * 1024 * 1024 / 512, &root);
    if (!fs3) {
        printf("FAIL: second remount\n");
        return 1;
    }
    printf("PASS: second remount\n");

    struct node f2a = {0};
    f2a.priv = 2;
    f2a.fs = fs3;
    r = bfrus_file_read(&f2a, 0, read_buf, sizeof(read_buf));
    if (r < 0 || strcmp((char *)read_buf, msg) != 0) {
        printf("FAIL: hello.txt corrupted after remount\n");
        return 1;
    }
    printf("PASS: hello.txt intact after remount\n");

    kfree(fs3);
    printf("\nAll BFRUS persistence tests passed.\n");
    return 0;
}
