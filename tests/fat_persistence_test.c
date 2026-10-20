#define AUTH_TEST
#include "../src/fat32.c"
#include "../src/vfs.c"
#include "../src/auth.c"
#include "../src/settings.c"
#include "../src/input.h"

#include <stdio.h>
#include <stdlib.h>

#define DISK_SECTORS 72048
#define PART_START 2048
#define PART_SECTORS 70000

volatile u64 ticks;
int sw = 800, sh = 600;
static u8 *test_disk;
static int restored_taskbar, restored_keyboard, relayout_count;
static u8 large_payload[20000];

void *kalloc(size_t n) { return malloc(n); }
void *kzalloc(size_t n) { return calloc(1, n); }
void *krealloc(void *p, size_t n) { return realloc(p, n); }
void kfree(void *p) { free(p); }
void wm_tbpos(int position) { restored_taskbar = position; }
int wm_tbpos_get(void) { return restored_taskbar; }
int kb_layout_get(void) { return restored_keyboard; }
void kb_layout(int layout) { restored_keyboard = layout; }
int gfx_setmode(int width, int height) { sw = width; sh = height; return 1; }
void wm_relayout(void) { relayout_count++; }
img *img_load(node *n) { (void)n; return NULL; }
void img_free(img *image) { (void)image; }
void wm_wallpaper(img *image) { (void)image; }

int disk_read(int d, u64 lba, u32 n, void *buf) {
    if (d || lba + n > DISK_SECTORS) return -1;
    memcpy(buf, test_disk + lba * 512, (size_t)n * 512);
    return 0;
}

int disk_write(int d, u64 lba, u32 n, const void *buf) {
    if (d || lba + n > DISK_SECTORS) return -1;
    memcpy(test_disk + lba * 512, buf, (size_t)n * 512);
    return 0;
}

int rtc_get(int *h, int *m, int *s, int *d, int *mo, int *y) {
    (void)h; (void)m; (void)s; (void)d; (void)mo; (void)y;
    return 0;
}

void klog(const char *fmt, ...) { (void)fmt; }

static int mount_fresh_vfs(void) {
    vfs_init();
    node *disk = vfs_mk(vroot, "disk", 1);
    return disk && fat_mount(0, PART_START, PART_SECTORS, disk) ? 0 : -1;
}

static int save_and_check_settings(u32 taskbar) {
    struct st state = {0};
    restored_taskbar = (int)taskbar;
    restored_keyboard = LAYOUT_ABNT2;
    sw = 1920;
    sh = 1080;
    snprintf(wallpaper_path, sizeof wallpaper_path, "%s", "/pictures/Default.png");
    if (persist_settings(&state) < 0) return -1;

    node *home = auth_home_dir();
    node *file = home ? vfs_child(home, ".astros-settings") : NULL;
    struct saved_settings actual;
    return file && !file->dir && file->size == sizeof actual &&
           vfs_rd(file, 0, &actual, sizeof actual) == (i64)sizeof actual &&
           !memcmp(actual.magic, "ASTSET01", 8) && actual.version == 1 &&
           actual.taskbar == taskbar && actual.keyboard == LAYOUT_ABNT2 &&
           actual.width == 1920 && actual.height == 1080 &&
           !strcmp(actual.wallpaper, "/pictures/Default.png") ? 0 : -1;
}

static int check_saved_settings(u32 taskbar) {
    node *home = auth_home_dir();
    node *file = home ? vfs_child(home, ".astros-settings") : NULL;
    struct saved_settings actual;
    if (!file || file->dir || file->size != sizeof actual ||
        vfs_rd(file, 0, &actual, sizeof actual) != (i64)sizeof actual ||
        memcmp(actual.magic, "ASTSET01", 8) || actual.version != 1 ||
        actual.taskbar != taskbar || actual.keyboard != LAYOUT_ABNT2 ||
        actual.width != 1920 || actual.height != 1080 ||
        strcmp(actual.wallpaper, "/pictures/Default.png")) return -1;
    return 0;
}

static int restore_and_check_settings(u32 taskbar) {
    sw = 800;
    sh = 600;
    restored_taskbar = 0;
    restored_keyboard = LAYOUT_US;
    relayout_count = 0;
    settings_restore();
    return restored_taskbar == (int)taskbar &&
           restored_keyboard == LAYOUT_ABNT2 &&
           sw == 1920 && sh == 1080 && relayout_count == 1 ? 0 : -1;
}

static int save_large_file(void) {
    node *home = auth_home_dir();
    return home ? vfs_save(home, "downloaded test file.bin",
                           large_payload, sizeof large_payload) : -1;
}

static int check_large_file(void) {
    node *home = auth_home_dir();
    node *file = home ? vfs_child(home, "downloaded test file.bin") : NULL;
    u8 actual[sizeof large_payload];
    return file && !file->dir && file->size == sizeof actual &&
           vfs_rd(file, 0, actual, sizeof actual) == (i64)sizeof actual &&
           !memcmp(actual, large_payload, sizeof actual) ? 0 : -1;
}

int main(void) {
    test_disk = calloc(DISK_SECTORS, 512);
    if (!test_disk) return 1;
    for (size_t i = 0; i < sizeof large_payload; i++)
        large_payload[i] = (u8)(i * 37u + 11u);

    if (fat_format(0, PART_START, PART_SECTORS, "ASTROS") < 0 ||
        mount_fresh_vfs() < 0 ||
        auth_init() != AUTH_SETUP ||
        auth_create("astro_user", "persistent-test-password") < 0 ||
        auth_init() != AUTH_LOGIN ||
        save_and_check_settings(3) < 0 || save_large_file() < 0) {
        fputs("Initial installed-volume settings save failed\n", stderr);
        free(test_disk);
        return 1;
    }

    if (mount_fresh_vfs() < 0 ||
        auth_init() != AUTH_LOGIN ||
        auth_verify("astro_user", "persistent-test-password") < 0 ||
        check_saved_settings(3) < 0 ||
        restore_and_check_settings(3) < 0 ||
        check_large_file() < 0) {
        fputs("Settings did not survive the first simulated reboot\n", stderr);
        free(test_disk);
        return 1;
    }

    if (save_and_check_settings(1) < 0 || mount_fresh_vfs() < 0 ||
        auth_init() != AUTH_LOGIN ||
        auth_verify("astro_user", "persistent-test-password") < 0 ||
        check_saved_settings(1) < 0 ||
        restore_and_check_settings(1) < 0 ||
        check_large_file() < 0) {
        fputs("Updated settings did not survive the second simulated reboot\n", stderr);
        free(test_disk);
        return 1;
    }

    puts("Installed FAT account home and settings survive remounts.");
    free(test_disk);
    return 0;
}
