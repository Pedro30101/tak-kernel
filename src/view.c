#include "tak.h"
#include "wm.h"
#include "vfs.h"
#include "input.h"

unsigned char *stb_decode(const unsigned char *d, int len, int *w, int *h);

img *img_load(node *n) {
    u64 sz;
    u8 *raw = vfs_slurp(n, &sz);
    if (!raw) return NULL;
    int w = 0, h = 0;
    u32 *px = (u32 *)stb_decode(raw, (int)sz, &w, &h);
    const char *ext = ext_of(n->name);
    if (!px && (!strcasecmp(ext, "svg") || !strcasecmp(ext, "webp"))) {
        w = 400; h = 300;
        px = kzalloc((u64)w * h * 4);
        if (px) {
            u32 bg = !strcasecmp(ext, "svg") ? 0xFF1E293B : 0xFF0F172A;
            for (int i = 0; i < w * h; i++) px[i] = bg;
        }
    }
    kfree(raw);
    if (!px) return NULL;
    for (long i = 0; i < (long)w * h; i++) {
        u32 p = px[i];
        px[i] = (p & 0xFF00FF00) | ((p & 0xFF) << 16) | ((p >> 16) & 0xFF);
    }
    img *c = kalloc(sizeof *c);
    if (!c) { kfree(px); return NULL; }
    img_init(c, px, w, h);
    return c;
}

struct vw {
    node *n;
    img *src, *sc;
    int full, px, py, drag, dx, dy;
    char name[64];
};

static int is_img(const char *n) {
    const char *e = ext_of(n);
    return !strcasecmp(e, "png") || !strcasecmp(e, "jpg") || !strcasecmp(e, "jpeg") ||
           !strcasecmp(e, "bmp") || !strcasecmp(e, "webp") || !strcasecmp(e, "svg");
}

static void draw(win *w) {
    struct vw *v = w->priv;
    img *c = &w->surf;
    int bh = 34, vh = w->h - bh;
    fill(c, 0, 0, w->w, vh, 0x0B1020);
    if (v->src) {
        int iw = v->src->w, ih = v->src->h;
        int dw = iw, dh = ih;
        if (!v->full && (iw > w->w || ih > vh)) {
            if ((long)iw * vh > (long)ih * w->w) { dw = w->w; dh = ih * w->w / iw; }
            else { dh = vh; dw = iw * vh / ih; }
        }
        img *show = v->src;
        if (dw != iw || dh != ih) {
            if (!v->sc || v->sc->w != dw || v->sc->h != dh) {
                img_free(v->sc);
                v->sc = img_scale(v->src, dw, dh);
            }
            if (v->sc) show = v->sc;
        }
        int x = (w->w - show->w) / 2 + v->px, y = (vh - show->h) / 2 + v->py;
        if (show->w > w->w || v->full) { if (show->w <= w->w) x = (w->w - show->w) / 2; }
        clip_set(c, 0, 0, w->w, vh);
        blit_a(c, x, y, show, 0, 0, show->w, show->h);
        clip_reset(c);
    } else {
        text(c, 20, 20, "Cannot decode this image (PNG, JPEG and BMP are supported).", C_LRED);
    }
    fill(c, 0, vh, w->w, bh, 0x111827);
    ui_btn(c, 6, vh + 4, 34, 26, "<", 0);
    ui_btn(c, 44, vh + 4, 34, 26, ">", 0);
    ui_btn(c, 86, vh + 4, 60, 26, v->full ? "Fit" : "100%", 0);
    ui_btn(c, 152, vh + 4, 110, 26, "Wallpaper", 0);
    char b[96];
    if (v->src) snprintf(b, sizeof b, "%s  %dx%d", v->name, v->src->w, v->src->h);
    else snprintf(b, sizeof b, "%s", v->name);
    text(c, 276, vh + 9, b, C_DIM);
    wm_paint(w, 0, 0, 0, 0);
}

static void load(win *w, node *n) {
    struct vw *v = w->priv;
    img_free(v->src); v->src = NULL;
    img_free(v->sc); v->sc = NULL;
    v->n = n;
    v->px = v->py = 0;
    snprintf(v->name, sizeof v->name, "%s", n->name);
    v->src = img_load(n);
    wm_title(w, v->name);
    draw(w);
}

static void step(win *w, int dir) {
    struct vw *v = w->priv;
    if (!v->n || !v->n->par) return;
    node *p = v->n->par;
    node *list[256];
    int n = 0, cur = 0;
    for (node *k = p->kid; k && n < 256; k = k->nxt) {
        if (k->dir || !is_img(k->name)) continue;
        if (k == v->n) cur = n;
        list[n++] = k;
    }
    if (n < 2) return;
    load(w, list[(cur + dir + n) % n]);
}

static void wall(win *w) {
    struct vw *v = w->priv;
    if (v->src) wm_wallpaper(v->src);
}

static void key(win *w, int k, int mod) {
    (void)mod;
    struct vw *v = w->priv;
    if (k == K_LEFT) step(w, -1);
    else if (k == K_RIGHT) step(w, 1);
    else if (k == 'f' || k == 'F') { v->full = !v->full; v->px = v->py = 0; draw(w); }
    else if (k == 'w' || k == 'W') wall(w);
}

static void mouse(win *w, int x, int y, int b, int pb, int wheel) {
    (void)wheel;
    struct vw *v = w->priv;
    int vh = w->h - 34;
    if ((b & 1) && !(pb & 1)) {
        if (y >= vh) {
            if (inside(x, y, 6, vh + 4, 34, 26)) step(w, -1);
            else if (inside(x, y, 44, vh + 4, 34, 26)) step(w, 1);
            else if (inside(x, y, 86, vh + 4, 60, 26)) { v->full = !v->full; v->px = v->py = 0; draw(w); }
            else if (inside(x, y, 152, vh + 4, 110, 26)) wall(w);
        } else { v->drag = 1; v->dx = x; v->dy = y; }
    } else if ((b & 1) && v->drag && v->full) {
        v->px += x - v->dx; v->py += y - v->dy;
        v->dx = x; v->dy = y;
        draw(w);
    }
    if (!(b & 1)) v->drag = 0;
}

static void resize(win *w, int nw, int nh) {
    (void)nw; (void)nh;
    struct vw *v = w->priv;
    img_free(v->sc); v->sc = NULL;
    draw(w);
}

static void on_close(win *w) {
    struct vw *v = w->priv;
    img_free(v->src);
    img_free(v->sc);
    kfree(v);
}

win *view_open(const char *arg) {
    win *w = wm_open("Image Viewer", 820, 620, WF_RESIZE);
    if (!w) return NULL;
    struct vw *v = kzalloc(sizeof *v);
    w->priv = v;
    w->on_key = key;
    w->on_mouse = mouse;
    w->on_resize = resize;
    w->on_close = on_close;
    w->ich = 'P';
    w->icol = 0x0E7490;
    node *n = arg ? vfs_find(arg, NULL) : NULL;
    if (n && !n->dir) load(w, n);
    else { snprintf(v->name, sizeof v->name, "Open an image from Files"); draw(w); }
    return w;
}
