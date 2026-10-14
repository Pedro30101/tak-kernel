#include "tak.h"
#include "wm.h"
#include "vfs.h"
#include "proc.h"
#include "term.h"

extern int live;
extern char gpu_name[];
extern u64 idle_ticks, busy_ticks;

static void cpu_brand(char *out) {
    u32 r[12];
    for (int i = 0; i < 3; i++) {
        u32 a = 0x80000002 + i, b, c, d;
        __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
        r[i * 4] = a; r[i * 4 + 1] = b; r[i * 4 + 2] = c; r[i * 4 + 3] = d;
    }
    memcpy(out, r, 48);
    out[48] = 0;
    char *p = out;
    while (*p == ' ') p++;
    if (p != out) memmove(out, p, strlen(p) + 1);
}

static void si_draw(win *w) {
    img *c = &w->surf;
    fill(c, 0, 0, w->w, w->h, C_PANEL);
    fill(c, 0, 0, w->w, 64, 0x111827);
    text2(c, 16, 14, "ASTROS", C_WHITE);
    text(c, 16 + 6 * 16 + 12, 30, "TAK Kernel " VER, C_LRED);
    char b[160], cpu[64];
    cpu_brand(cpu);
    int y = 80;
    const char *k[] = { "Mode", "CPU", "Graphics", "Display", "Memory", "Heap", "Uptime", "Processes", "Windows" };
    u64 up = uptime_ms() / 1000;
    extern int fbbpp;
    char v[9][96];
    snprintf(v[0], 96, "%s", live ? "Live session" : "Installed");
    snprintf(v[1], 96, "%s", cpu);
    snprintf(v[2], 96, "%s", gpu_name);
    snprintf(v[3], 96, "%dx%d, %d bpp (framebuffer)", sw, sh, fbbpp);
    snprintf(v[4], 96, "%lu MB usable, %lu MB free", mem_total_kb() / 1024, mem_free_kb() / 1024);
    snprintf(v[5], 96, "%lu MB free", heap_free_kb() / 1024);
    snprintf(v[6], 96, "%lu min %lu s", up / 60, up % 60);
    snprintf(v[7], 96, "see Task Manager");
    snprintf(v[8], 96, "%d open", wm_count());
    for (int i = 0; i < 9; i++) {
        text(c, 20, y, k[i], C_DIM);
        text(c, 130, y, v[i], C_TEXT);
        y += 26;
    }
    (void)b;
    wm_paint(w, 0, 0, 0, 0);
}

static void si_tick(win *w) {
    if (ticks % 100 == 0) si_draw(w);
}

static win *si_open(const char *arg) {
    (void)arg;
    win *w = wm_open("System Info", 520, 330, 0);
    if (!w) return NULL;
    w->on_tick = si_tick;
    w->ich = 'i';
    w->icol = 0x0891B2;
    si_draw(w);
    return w;
}

win *settings_open(const char *arg);
win *files_open(const char *arg);
win *edit_open(const char *arg);
win *view_open(const char *arg);
win *taskmgr_open(const char *arg);
win *install_open(const char *arg);
win *media_open(const char *arg);
win *tide_open(const char *arg);

app_t apps[] = {
    { "Tak Installer", 'T', 0xB91C1C, install_open, A_LIVE },
    { "TIDE Studio", 'D', 0x7C3AED, tide_open, A_LIVE | A_FULL },
    { "Terminal", '>', 0x15803D, term_open, A_LIVE | A_FULL },
    { "Files", 'F', 0xF59E0B, files_open, A_LIVE | A_FULL },
    { "Text Editor", 'E', 0x2563EB, edit_open, A_FULL },
    { "Image Viewer", 'P', 0x0E7490, view_open, A_FULL },
    { "Task Manager", 'M', 0xDC2626, taskmgr_open, A_FULL },
    { "Media Player", 'M', 0xDB2777, media_open, A_FULL },
    { "Settings", 'S', 0xB45309, settings_open, A_LIVE | A_FULL },
    { "System Info", 'i', 0x0891B2, si_open, A_LIVE | A_FULL },
};
int napps = sizeof apps / sizeof apps[0];

win *app_open(const char *name, const char *arg) {
    for (int i = 0; i < napps; i++)
        if (!strcasecmp(apps[i].name, name)) return apps[i].open(arg);
    return NULL;
}

void open_file(node *n) {
    char p[160];
    vfs_path(n, p, sizeof p);
    const char *e = ext_of(n->name);
    if (n->dir) { app_open("Files", p); return; }
    if (!strcasecmp(e, "te")) { proc_spawn(p, NULL, NULL); return; }
    if (!strcasecmp(e, "png") || !strcasecmp(e, "jpg") || !strcasecmp(e, "jpeg") ||
        !strcasecmp(e, "bmp") || !strcasecmp(e, "webp") || !strcasecmp(e, "svg")) {
        app_open("Image Viewer", p); return;
    }
    if (!strcasecmp(e, "wav") || !strcasecmp(e, "mp3") || !strcasecmp(e, "ogg") ||
        !strcasecmp(e, "mp4") || !strcasecmp(e, "mkv")) {
        app_open("Media Player", p); return;
    }
    if (!strcasecmp(e, "py") || !strcasecmp(e, "c")) {
        app_open("TIDE Studio", p); return;
    }
    app_open("Text Editor", p);
}

void apps_boot(void) {
    if (live) {
        app_open("Tak Installer", NULL);
        app_open("TIDE Studio", NULL);
    }
}
