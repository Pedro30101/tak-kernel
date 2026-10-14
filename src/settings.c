#include "tak.h"
#include "wm.h"
#include "vfs.h"
#include "input.h"

extern char gpu_name[];
extern int gpu_kind;
extern int live;
extern int fbbpp;
img *img_load(node *n);
int gfx_setmode(int w, int h);

struct hot { int x, y, w, h, id; };
struct st {
    int tab, hov;
    struct hot hot[48];
    int nhot;
    char msg[96];
};

static const char *tabs[] = { "Taskbar", "Wallpaper", "Display", "Keyboard", "About" };
static const int modes[][2] = { {1920, 1080}, {1600, 900}, {1366, 768}, {1280, 720}, {1024, 768}, {800, 600} };

static void hb(struct st *s, img *c, int x, int y, int w, int h, const char *t, int id, int on) {
    ui_btn(c, x, y, w, h, t, s->hov == id ? 1 : 0);
    if (on) box(c, x, y, w, h, C_LRED), box(c, x + 1, y + 1, w - 2, h - 2, C_LRED);
    if (s->nhot < 48) s->hot[s->nhot++] = (struct hot){ x, y, w, h, id };
}

static int is_img(const char *n) {
    const char *e = ext_of(n);
    return !strcasecmp(e, "png") || !strcasecmp(e, "jpg") || !strcasecmp(e, "jpeg") || !strcasecmp(e, "bmp");
}

static void draw(win *w) {
    struct st *s = w->priv;
    img *c = &w->surf;
    fill(c, 0, 0, w->w, w->h, C_PANEL);
    fill(c, 0, 0, 150, w->h, 0x111827);
    s->nhot = 0;
    for (int i = 0; i < 5; i++) {
        int y = 12 + i * 38;
        if (s->tab == i) fill(c, 0, y, 150, 34, C_DRED);
        else if (s->hov == 1 + i) fill(c, 0, y, 150, 34, 0x1F2937);
        text(c, 16, y + 9, tabs[i], s->tab == i ? C_WHITE : C_TEXT);
        s->hot[s->nhot++] = (struct hot){ 0, y, 150, 34, 1 + i };
    }
    int x = 170, y = 16;
    if (s->tab == 0) {
        text2(c, x, y, "Taskbar", C_WHITE);
        text(c, x, y + 44, "Choose where the taskbar sits on screen.", C_DIM);
        int p = wm_tbpos_get();
        hb(s, c, x, y + 80, 110, 36, "Left", 100, p == 0);
        hb(s, c, x + 124, y + 80, 110, 36, "Top", 101, p == 1);
        hb(s, c, x, y + 128, 110, 36, "Right", 102, p == 2);
        hb(s, c, x + 124, y + 128, 110, 36, "Bottom", 103, p == 3);
    } else if (s->tab == 1) {
        text2(c, x, y, "Wallpaper", C_WHITE);
        hb(s, c, x, y + 50, 200, 30, "Astros default", 110, 0);
        node *d = vfs_find("/pictures", NULL);
        int row = 0;
        if (d) {
            vfs_load(d);
            for (node *k = d->kid; k && row < 8; k = k->nxt) {
                if (k->dir || !is_img(k->name)) continue;
                hb(s, c, x, y + 90 + row * 36, 300, 30, k->name, 111 + row, 0);
                row++;
            }
        }
        if (!row) text(c, x, y + 96, "Put pictures in /pictures to list them here.", C_DIM);
    } else if (s->tab == 2) {
        text2(c, x, y, "Display", C_WHITE);
        char b[96];
        snprintf(b, sizeof b, "Current: %dx%d, %d bpp", sw, sh, fbbpp);
        text(c, x, y + 44, b, C_TEXT);
        snprintf(b, sizeof b, "Graphics: %s", gpu_name);
        text(c, x, y + 66, b, C_DIM);
        if (gpu_kind == 4 || gpu_kind == 7) {
            text(c, x, y + 96, "Pick a resolution (virtual display adapter):", C_TEXT);
            for (int i = 0; i < 6; i++) {
                char m[24];
                snprintf(m, sizeof m, "%dx%d", modes[i][0], modes[i][1]);
                hb(s, c, x + (i % 2) * 150, y + 124 + (i / 2) * 42, 140, 34, m, 120 + i, sw == modes[i][0] && sh == modes[i][1]);
            }
        } else {
            text(c, x, y + 96, "This adapter has no native mode switching in Astros.", C_TEXT);
            text(c, x, y + 118, "The resolution is chosen by the bootloader (GRUB)", C_DIM);
            text(c, x, y + 136, "and uses the framebuffer it hands to the
                 kernel.", C_DIM);
        }
    } else if (s->tab == 3) {
        text2(c, x, y, "Keyboard", C_WHITE);
        int l = kb_layout_get();
        hb(s, c, x, y + 60, 160, 36, "US", 130, l == LAYOUT_US);
        hb(s, c, x + 176, y + 60, 160, 36, "BR-ABNT2", 131, l == LAYOUT_ABNT2);
        text(c, x, y + 116, "ABNT2 supports dead keys for accents (a, e, i, o, u, c-cedilla).", C_DIM);
    } else {
        text2(c, x, y, "About", C_WHITE);
        char b[96];
        text(c, x, y + 50, "Astros desktop environment", C_TEXT);
        text(c, x, y + 72, "TAK Kernel " VER " (x86_64)", C_TEXT);
        snprintf(b, sizeof b, "Session: %s", live ? "live (nothing is saved)" : "installed");
        text(c, x, y + 94, b, C_DIM);
        snprintf(b, sizeof b, "Memory: %lu MB usable", mem_total_kb() / 1024);
        text(c, x, y + 116, b, C_DIM);
        snprintf(b, sizeof b, "Graphics: %s", gpu_name);
        text(c, x, y + 138, b, C_DIM);
    }
    if (s->msg[0]) text(c, 170, w->h - 28, s->msg, C_YELLOW);
    wm_paint(w, 0, 0, 0, 0);
}

static void act(win *w, int id) {
    struct st *s = w->priv;
    s->msg[0] = 0;
    if (id >= 1 && id <= 5) s->tab = id - 1;
    else if (id >= 100 && id <= 103) wm_tbpos(id - 100);
    else if (id == 110) wm_wallpaper(NULL);
    else if (id >= 111 && id < 120) {
        node *d = vfs_find("/pictures", NULL);
        int row = 0;
        for (node *k = d ? d->kid : NULL; k; k = k->nxt) {
            if (k->dir || !is_img(k->name)) continue;
            if (row++ == id - 111) {
                img *i = img_load(k);
                if (i) { wm_wallpaper(i); img_free(i); }
                else snprintf(s->msg, sizeof s->msg, "Could not decode %s", k->name);
                break;
            }
        }
    } else if (id >= 120 && id < 126) {
        int i = id - 120;
        if (gfx_setmode(modes[i][0], modes[i][1])) wm_relayout();
        else snprintf(s->msg, sizeof s->msg, "Mode not supported by this adapter");
    } else if (id == 130) kb_layout(LAYOUT_US);
    else if (id == 131) kb_layout(LAYOUT_ABNT2);
}

static void mouse(win *w, int x, int y, int b, int pb, int wheel) {
    (void)wheel;
    struct st *s = w->priv;
    int h = 0;
    for (int i = 0; i < s->nhot; i++)
        if (inside(x, y, s->hot[i].x, s->hot[i].y, s->hot[i].w, s->hot[i].h)) h = s->hot[i].id;
    int press = (b & 1) && !(pb & 1);
    if (press && h) act(w, h);
    if (press || h != s->hov) { s->hov = h; draw(w); }
}

static void on_close(win *w) { kfree(w->priv); }

win *settings_open(const char *arg) {
    win *w = wm_open("Settings", 640, 420, 0);
    if (!w) return NULL;
    struct st *s = kzalloc(sizeof *s);
    w->priv = s;
    w->on_mouse = mouse;
    w->on_close = on_close;
    w->ich = 'S';
    w->icol = 0xB45309;
    if (arg && !strcmp(arg, "taskbar")) s->tab = 0;
    draw(w);
    return w;
}
