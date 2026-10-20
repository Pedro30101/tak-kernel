#include "tak.h"
#include "vfs.h"
#include "wm.h"
#include "net.h"
#include "netstack.h"

#define MAXW 64
#define MENUW 296
#define ITEMH 38
#define MENU_HEAD 48
#define MENU_PAD 12

int tbpos;
static int tbx, tby, tbw, tbh, dkx, dky, dkw, dkh, cpx, cpy, cpw, cph;
static int wp_custom;
static win *wins[MAXW];
static int nw, nextid = 1;
static win *session_gate;
win *focus;
static img *wp;
static int dx0, dy0, dx1, dy1;
static int bprev;
static win *cap, *dragw, *resw;
static int ddx, ddy, rsx, rsy, rsw0, rsh0;
static int menu_open, menu_hov = -1;
static int hov_id, hov_code;
static int tb_hov = -2;
static char clk[8] = "--:--", dat[8] = "--/--";
static u64 last_click;
static int last_click_id;
extern int wm_idle;

struct mi {
    char label[48];
    int act;
    char path[80];
    int appi;
    u32 col;
    char letter;
};
static struct mi menu[48];
static int nmenu;

int inside(int px, int py, int x, int y, int w, int h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}

void wm_session_lock(win *w) {
    if (!w || !(w->flags & WF_LOCKSCREEN)) return;
    session_gate = w;
    menu_open = 0;
    cap = dragw = resw = NULL;
    wm_focus(w);
    wm_dirty(0, 0, sw, sh);
}

void wm_session_unlock(win *w) {
    if (!w || session_gate != w) return;
    session_gate = NULL;
    wm_dirty(0, 0, sw, sh);
}

void wm_dirty(int x, int y, int w, int h) {
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > sw) x1 = sw;
    if (y1 > sh) y1 = sh;
    if (x1 <= x || y1 <= y) return;
    if (dx1 <= dx0 || dy1 <= dy0) { dx0 = x; dy0 = y; dx1 = x1; dy1 = y1; return; }
    if (x < dx0) dx0 = x;
    if (y < dy0) dy0 = y;
    if (x1 > dx1) dx1 = x1;
    if (y1 > dy1) dy1 = y1;
}

static void dirty_win(win *w) { wm_dirty(w->x, w->y, w->w + 2 * BD + 7, w->h + TB + BD + 7); }
static void tb_layout(void) {
    switch (tbpos) {
    case 0: tbx = 0; tby = 0; tbw = TBW; tbh = sh; dkx = TBW; dky = 0; dkw = sw - TBW; dkh = sh;
            cpx = 8; cpy = 8; cpw = 56; cph = sh - 16; break;
    case 1: tbx = 0; tby = 0; tbw = sw; tbh = TBW; dkx = 0; dky = TBW; dkw = sw; dkh = sh - TBW;
            cpx = 8; cpy = 8; cpw = sw - 16; cph = 56; break;
    case 2: tbx = sw - TBW; tby = 0; tbw = TBW; tbh = sh; dkx = 0; dky = 0; dkw = sw - TBW; dkh = sh;
            cpx = sw - 64; cpy = 8; cpw = 56; cph = sh - 16; break;
    default: tbx = 0; tby = sh - TBW; tbw = sw; tbh = TBW; dkx = 0; dky = 0; dkw = sw; dkh = sh - TBW;
            cpx = 8; cpy = sh - 64; cpw = sw - 16; cph = 56; break;
    }
}

static int horiz(void) { return tbpos == 1 || tbpos == 3; }
static void dirty_tb(void) { wm_dirty(tbx, tby, tbw, tbh); }

static void menu_rect(int *x, int *y, int *w, int *h) {
    int hh = nmenu * ITEMH + MENU_HEAD + MENU_PAD * 2;
    *w = MENUW; *h = hh;
    switch (tbpos) {
    case 0: *x = tbx + tbw + 4; *y = 10; break;
    case 1: *x = 10; *y = tby + tbh + 4; break;
    case 2: *x = tbx - MENUW - 4; *y = 10; break;
    default: *x = 10; *y = tby - hh - 4; break;
    }
    if (*y < 4) *y = 4;
}

static void dirty_menu(void) {
    int x, y, w, h;
    menu_rect(&x, &y, &w, &h);
    wm_dirty(x - 24, y - 24, w + 56, h + 56);
}

static void start_xy(int *x, int *y) { *x = cpx + 8; *y = cpy + 8; }

static void item_xy(int i, int *x, int *y) {
    if (horiz()) { *x = cpx + 64 + i * 48; *y = cpy + 8; }
    else { *x = cpx + 8; *y = cpy + 64 + i * 48; }
}

static int max_items(void) { return horiz() ? (cpw - 64 - 70) / 48 : (cph - 64 - 66) / 48; }

static void clock_xy(int *x, int *y) {
    if (horiz()) { *x = cpx + cpw - 52; *y = cpy + 10; }
    else { *x = cpx + 8; *y = cpy + cph - 44; }
}

static int tb_hit(int px, int py) {
    if (!inside(px, py, cpx, cpy, cpw, cph)) return -3;
    int x, y;
    start_xy(&x, &y);
    if (inside(px, py, x, y, 40, 40)) return -1;
    for (int i = 0; i < nw && i < max_items(); i++) {
        item_xy(i, &x, &y);
        if (inside(px, py, x, y, 40, 40)) return i;
    }
    return -2;
}

static void tip_dirty(void) {
    if (horiz()) wm_dirty(0, tbpos == 1 ? tby + tbh : tby - 44, sw, 44);
    else wm_dirty(tbpos == 0 ? tbx + tbw : tbx - 320, 0, 320, sh);
}

static void glow(img *c, int cx, int cy, int rad, u32 col, int amax) {
    int x0 = cx - rad < 0 ? 0 : cx - rad, x1 = cx + rad > c->w ? c->w : cx + rad;
    int y0 = cy - rad < 0 ? 0 : cy - rad, y1 = cy + rad > c->h ? c->h : cy + rad;
    long r2 = (long)rad * rad;
    for (int y = y0; y < y1; y++) {
        u32 *d = c->p + (u64)y * c->w;
        for (int x = x0; x < x1; x++) {
            long d2 = (long)(x - cx) * (x - cx) + (long)(y - cy) * (y - cy);
            if (d2 >= r2) continue;
            long t = 255 - d2 * 255 / r2;
            int a = (int)(amax * (t * t / 255) / 255);
            if (a > 0) d[x] = mix(d[x], col, a);
        }
    }
}

static void default_wall(void) {
    for (int y = 0; y < sh; y++) fill(wp, 0, y, sw, 1, mix(0x070B1E, 0x14305E, y * 255 / sh));
    glow(wp, sw * 22 / 100, sh * 28 / 100, sh * 60 / 100, 0x2F6BFF, 150);
    glow(wp, sw * 78 / 100, sh * 62 / 100, sh * 65 / 100, 0x8B3DFF, 120);
    glow(wp, sw * 50 / 100, sh * 95 / 100, sh * 55 / 100, 0x10B7E8, 110);
    glow(wp, sw * 90 / 100, sh * 12 / 100, sh * 35 / 100, 0xFF4F9A, 70);
    int sc = sh >= 1000 ? 10 : 6;
    const char *t = "ASTROS";
    int tw = 6 * 8 * sc, x0 = dkx + (dkw - tw) / 2, y0 = dky + dkh / 2 - 8 * sc;
    for (int i = 0; i < 6; i++)
        for (int r = 0; r < 16; r++) {
            u8 bits = gfont_row(t[i], r);
            for (int b = 0; b < 8; b++) if (bits & (0x80 >> b)) fill_a(wp, x0 + i * 8 * sc + b * sc, y0 + r * sc, sc, sc, 0xFFFFFF, 18);
        }
}

void wm_wallpaper(img *src) {
    if (!src) { wp_custom = 0; default_wall(); wm_dirty(0, 0, sw, sh); return; }
    wp_custom = 1;
    int dw = sw, dh = src->h * sw / src->w;
    if (dh < sh) { dh = sh; dw = src->w * sh / src->h; }
    img *s = img_scale(src, dw, dh);
    if (!s) return;
    blit(wp, 0, 0, s, (dw - sw) / 2, (dh - sh) / 2, sw, sh);
    img_free(s);
    wm_dirty(0, 0, sw, sh);
}

static int is_te(const char *n) { return !strcasecmp(ext_of(n), "te"); }

void wm_menu_refresh(void) {
    nmenu = 0;
    extern int live;
    for (int i = 0; i < napps && nmenu < 40; i++) {
        if (!(apps[i].flags & (live ? A_LIVE : A_FULL))) continue;
        struct mi *m = &menu[nmenu++];
        snprintf(m->label, sizeof m->label, "%s", apps[i].name);
        m->act = 1; m->appi = i; m->col = apps[i].col; m->letter = apps[i].letter;
    }
    node *d = live ? NULL : vfs_find("/apps", NULL);
    int first = 1;
    if (d) {
        vfs_load(d);
        for (node *k = d->kid; k && nmenu < 40; k = k->nxt) {
            if (k->dir || !is_te(k->name)) continue;
            if (first) { menu[nmenu++].act = 0; first = 0; }
            struct mi *m = &menu[nmenu++];
            snprintf(m->label, sizeof m->label, "%s", k->name);
            char *dot = strrchr(m->label, '.');
            if (dot) *dot = 0;
            m->act = 2;
            vfs_path(k, m->path, sizeof m->path);
            m->col = 0x6D28D9;
            m->letter = (char)toupper(m->label[0]);
        }
    }
    menu[nmenu++].act = 0;
    struct mi *m = &menu[nmenu++];
    snprintf(m->label, sizeof m->label, "Restart");
    m->act = 3; m->col = 0xB45309; m->letter = 'R';
    m = &menu[nmenu++];
    snprintf(m->label, sizeof m->label, "Shut down");
    m->act = 4; m->col = 0x991B1B; m->letter = 'S';
}

static void sorted(win **out) {
    for (int i = 0; i < nw; i++) out[i] = wins[i];
    for (int i = 1; i < nw; i++)
        for (int j = i; j > 0 && out[j - 1]->id > out[j]->id; j--) {
            win *t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
}

int wm_count(void) { return nw; }
win *wm_nth(int i) { return i >= 0 && i < nw ? wins[i] : NULL; }

win *wm_byid(int id) {
    for (int i = 0; i < nw; i++) if (wins[i]->id == id) return wins[i];
    return NULL;
}

static void refocus(void) {
    focus = NULL;
    for (int i = nw - 1; i >= 0; i--)
        if (!(wins[i]->flags & WF_MIN)) { focus = wins[i]; break; }
}

void wm_focus(win *w) {
    if (session_gate && w != session_gate) return;
    int idx = -1;
    for (int i = 0; i < nw; i++) if (wins[i] == w) idx = i;
    if (idx < 0) return;
    if (w->flags & WF_MIN) { w->flags &= ~WF_MIN; dirty_win(w); }
    for (int i = idx; i < nw - 1; i++) wins[i] = wins[i + 1];
    wins[nw - 1] = w;
    if (focus && focus != w) dirty_win(focus);
    focus = w;
    dirty_win(w);
    dirty_tb();
}

win *wm_open(const char *title, int w, int h, int flags) {
    if (nw >= MAXW) return NULL;
    if (flags & WF_LOCKSCREEN) { w = sw; h = sh; }
    win *n = kzalloc(sizeof *n);
    if (!n) return NULL;
    int maxw = dkw - 2 * BD - 8, maxh = dkh - TB - BD - 8;
    if (!(flags & WF_LOCKSCREEN)) {
        if (w > maxw) w = maxw;
        if (h > maxh) h = maxh;
    }
    u32 *p = kzalloc((u64)w * h * 4 + 16);
    if (!p) { kfree(n); return NULL; }
    img_init(&n->surf, p, w, h);
    n->w = w; n->h = h;
    n->flags = flags;
    n->id = nextid++;
    snprintf(n->title, sizeof n->title, "%s", title);
    if (flags & WF_LOCKSCREEN) {
        n->x = n->y = 0;
    } else {
        int k = nw % 8;
        n->x = dkx + 30 + k * 28 + (dkw - w) / 10;
        n->y = dky + 24 + k * 28;
        if (n->x + w + 2 * BD > dkx + dkw) n->x = dkx + dkw - w - 2 * BD;
        if (n->y + h + TB + BD > dky + dkh) n->y = dky + dkh - h - TB - BD;
        if (n->x < dkx) n->x = dkx;
        if (n->y < dky) n->y = dky;
    }
    n->ich = toupper((u8)title[0]);
    n->icol = 0x2563EB;
    wins[nw++] = n;
    wm_focus(n);
    return n;
}

void wm_close(win *w) {
    if (w == session_gate) return;
    int idx = -1;
    for (int i = 0; i < nw; i++) if (wins[i] == w) idx = i;
    if (idx < 0) return;
    if (w->on_close) w->on_close(w);
    dirty_win(w);
    for (int i = idx; i < nw - 1; i++) wins[i] = wins[i + 1];
    nw--;
    if (cap == w) cap = NULL;
    if (dragw == w) dragw = NULL;
    if (resw == w) resw = NULL;
    if (focus == w) refocus();
    if (focus) dirty_win(focus);
    dirty_tb();
    kfree(w->surf.p);
    kfree(w);
}

void wm_close_owner(int pid) {
    for (int i = nw - 1; i >= 0; i--)
        if (i < nw && wins[i]->owner == pid && (wins[i]->flags & WF_USER)) wm_close(wins[i]);
}

void wm_title(win *w, const char *t) {
    snprintf(w->title, sizeof w->title, "%s", t);
    wm_dirty(w->x, w->y, w->w + 2 * BD, TB);
    dirty_tb();
}

void wm_paint(win *w, int x, int y, int cw, int ch) {
    if (w->flags & WF_MIN) return;
    if (cw <= 0) { x = 0; y = 0; cw = w->w; ch = w->h; }
    wm_dirty(w->x + BD + x, w->y + TB + y, cw, ch);
}

void wm_resize(win *w, int nw2, int nh2) {
    if (nw2 < 160) nw2 = 160;
    if (nh2 < 100) nh2 = 100;
    if (nw2 == w->w && nh2 == w->h) return;
    u32 *p = kzalloc((u64)nw2 * nh2 * 4 + 16);
    if (!p) return;
    img old = w->surf;
    img_init(&w->surf, p, nw2, nh2);
    int cw = old.w < nw2 ? old.w : nw2, chh = old.h < nh2 ? old.h : nh2;
    for (int j = 0; j < chh; j++) memcpy(p + (u64)j * nw2, old.p + (u64)j * old.w, (u64)cw * 4);
    kfree(old.p);
    dirty_win(w);
    w->w = nw2; w->h = nh2;
    dirty_win(w);
    if (w->on_resize) w->on_resize(w, nw2, nh2);
    if (w->flags & WF_USER) {
        struct uev e = { UE_RESIZE, nw2, nh2, 0, 0 };
        wm_post(w, &e);
    }
}

static void maximize(win *w, int on) {
    if (on && !(w->flags & WF_MAX)) {
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
        dirty_win(w);
        w->x = dkx; w->y = dky;
        w->flags |= WF_MAX;
        wm_resize(w, dkw - 2 * BD, dkh - TB - BD);
        if (w->w != dkw - 2 * BD) w->flags &= ~WF_MAX;
        dirty_win(w);
    } else if (!on && (w->flags & WF_MAX)) {
        dirty_win(w);
        w->flags &= ~WF_MAX;
        w->x = w->rx; w->y = w->ry;
        wm_resize(w, w->rw, w->rh);
        dirty_win(w);
    }
}

static void minimize(win *w) {
    if (w->flags & WF_NOMIN) return;
    w->flags |= WF_MIN;
    dirty_win(w);
    if (focus == w) refocus();
    if (focus) dirty_win(focus);
    dirty_tb();
}

int wm_post(win *w, struct uev *e) {
    int n = (w->qt + 1) % 64;
    if (n == w->qh) return -1;
    w->q[w->qt] = *e;
    w->qt = n;
    if (w->owner) proc_wake(w->owner);
    return 0;
}

int wm_pop(win *w, struct uev *e) {
    if (w->qh == w->qt) return 0;
    *e = w->q[w->qh];
    w->qh = (w->qh + 1) % 64;
    return 1;
}

static void to_key(win *w, int key, int down, int mod) {
    if (w->flags & WF_USER) {
        struct uev e = { UE_KEY, key, down, mod, 0 };
        wm_post(w, &e);
    } else if (down && w->on_key) {
        w->on_key(w, key, mod);
    }
}

static void to_mouse(win *w, int px, int py, int b, int pb, int wheel) {
    int cx = px - (w->x + BD), cy = py - (w->y + TB);
    if (w->flags & WF_USER) {
        struct uev e = { UE_MOUSE, cx, cy, b | (pb << 8), wheel };
        wm_post(w, &e);
    } else if (w->on_mouse) {
        w->on_mouse(w, cx, cy, b, pb, wheel);
    }
}

static void req_close(win *w) {
    if (w->flags & WF_USER) {
        for (int i = w->qh; i != w->qt; i = (i + 1) % 64)
            if (w->q[i].type == UE_CLOSE) { proc_kill(w->owner); return; }
        struct uev e = { UE_CLOSE, 0, 0, 0, 0 };
        wm_post(w, &e);
    } else {
        wm_close(w);
    }
}

static win *win_at(int px, int py) {
    for (int i = nw - 1; i >= 0; i--) {
        win *w = wins[i];
        if (w->flags & WF_MIN) continue;
        if (inside(px, py, w->x, w->y, w->w + 2 * BD, w->h + TB + BD)) return w;
    }
    return NULL;
}

enum { R_NONE, R_CLIENT, R_TITLE, R_CLOSE, R_MAX, R_MIN, R_SIZE };

static int region(win *w, int px, int py) {
    int W = w->w + 2 * BD, H = w->h + TB + BD;
    int rx = px - w->x, ry = py - w->y;
    if (rx < 0 || ry < 0 || rx >= W || ry >= H) return R_NONE;
    if (ry < TB) {
        int x0 = W - 8 - 104;
        if (ry >= 5 && ry < 27 && rx >= x0 && rx < x0 + 104) {
            if (rx >= x0 + 60) return R_CLOSE;
            if (rx >= x0 + 30) return (w->flags & WF_NOMAX) ? R_TITLE : R_MAX;
            return (w->flags & WF_NOMIN) ? R_TITLE : R_MIN;
        }
        return R_TITLE;
    }
    if ((w->flags & WF_RESIZE) && !(w->flags & WF_MAX)) {
        if (rx >= W - BD - 8 && ry >= H - BD - 8) return R_SIZE;
        if (rx >= W - BD || ry >= H - BD) return R_SIZE;
    }
    return R_CLIENT;
}

static void draw_x(img *c, int cx, int cy, u32 col) {
    for (int i = 0; i < 10; i++) {
        fill(c, cx - 5 + i, cy - 5 + i, 2, 1, col);
        fill(c, cx - 5 + i, cy + 4 - i, 2, 1, col);
    }
}

static void draw_icon(img *c, int x, int y, int s, u32 col, int ch, int hov, int dim, int sc2) {
    u32 top = mix(col, 0xFFFFFF, dim ? 10 : 80), bot = mix(col, 0x000000, dim ? 150 : 60);
    int r = s / 4 + 2;
    vgrad(c, x, y, s, s, r, top, bot);
    fill_rr(c, x + 2, y + 2, s - 4, s / 2 - 1, r - 2, 0xFFFFFF, hov ? 78 : 50);
    glass_edge(c, x, y, s, s, r, 190, 70);
    int gw = 8 * sc2, gh = 16 * sc2;
    u32 gc = dim ? 0xC4CAD6 : 0xFFFFFF;
    glyph(c, x + (s - gw) / 2 + 1, y + (s - gh) / 2 + 1, ch, 0x000000, -1, sc2);
    glyph(c, x + (s - gw) / 2, y + (s - gh) / 2, ch, gc, -1, sc2);
}

static void title_text(img *c, int x, int y, const char *t, int n, u32 col) {
    textn(c, x + 1, y + 1, t, n, 0x0A1224);
    textn(c, x, y, t, n, col);
}

static void draw_win(win *w) {
    if (w->flags & WF_LOCKSCREEN) {
        blit(&scr, w->x, w->y, &w->surf, 0, 0, w->w, w->h);
        return;
    }
    int W = w->w + 2 * BD, H = w->h + TB + BD;
    if (w->x - 24 >= scr.cx1 || w->y - 24 >= scr.cy1 || w->x + W + 24 <= scr.cx0 || w->y + H + 28 <= scr.cy0) return;
    int foc = w == focus, R = 18;
    soft_shadow(&scr, w->x, w->y + 6, W, H, R, 24, foc ? 112 : 68);
    u32 tint = foc ? 0x5275A8 : 0x77849A;
    int ta = foc ? 82 : 68;
    glass(&scr, w->x, w->y, W, TB, w->x, w->y, W, H, R, tint, ta, 9, foc ? 82 : 54);
    glass(&scr, w->x, w->y + H - BD, W, BD, w->x, w->y, W, H, R, tint, ta, 9, 0);
    glass(&scr, w->x, w->y + TB, BD, H - TB - BD, w->x, w->y, W, H, R, tint, ta, 9, 0);
    glass(&scr, w->x + W - BD, w->y + TB, BD, H - TB - BD, w->x, w->y, W, H, R, tint, ta, 9, 0);
    int maxc = (W - 190) / 8;
    int n = (int)strlen(w->title);
    if (n > maxc) n = maxc > 0 ? maxc : 0;
    vgrad(&scr, w->x + 12, w->y + 9, 16, 16, 5, mix(w->icol, 0xFFFFFF, 90), mix(w->icol, 0, 70));
    glyph(&scr, w->x + 16, w->y + 9, w->ich, C_WHITE, -1, 1);
    title_text(&scr, w->x + 36, w->y + 9, w->title, n, foc ? C_WHITE : 0xDDE3EE);
    int x0 = w->x + W - 8 - 104, y0 = w->y + 5;
    int hv = hov_id == w->id ? hov_code : 0;
    fill_rr(&scr, x0, y0, 104, 22, 9, 0x0A1224, 70);
    if (hv == R_MIN) fill_rr(&scr, x0, y0, 30, 22, 9, 0xFFFFFF, 80);
    if (hv == R_MAX) fill_rr(&scr, x0 + 30, y0, 30, 22, 4, 0xFFFFFF, 80);
    fill_rr(&scr, x0 + 60, y0, 44, 22, 9, hv == R_CLOSE ? 0xF0445A : 0xC8283E, 255);
    fill_rr(&scr, x0 + 62, y0 + 1, 40, 9, 5, 0xFFFFFF, hv == R_CLOSE ? 100 : 66);
    glass_edge(&scr, x0, y0, 104, 22, 9, 210, 80);
    fill(&scr, x0 + 9, y0 + 14, 12, 3, C_WHITE);
    box(&scr, x0 + 39, y0 + 6, 12, 10, C_WHITE);
    hline(&scr, x0 + 39, y0 + 7, 12, C_WHITE);
    draw_x(&scr, x0 + 82, y0 + 11, C_WHITE);
    blit(&scr, w->x + BD, w->y + TB, &w->surf, 0, 0, w->w, w->h);
    fill_a(&scr, w->x + BD - 1, w->y + TB - 1, w->w + 2, 1, 0, 110);
    fill_a(&scr, w->x + BD - 1, w->y + TB + w->h, w->w + 2, 1, 0, 70);
    fill_a(&scr, w->x + BD - 1, w->y + TB, 1, w->h, 0, 90);
    fill_a(&scr, w->x + BD + w->w, w->y + TB, 1, w->h, 0, 90);
    glass_edge(&scr, w->x, w->y, W, H, R, foc ? 190 : 125, foc ? 82 : 50);
}

static void indicator(int x, int y, int on) {
    u32 col = on ? C_WHITE : 0x8FA3C8;
    int len = on ? 18 : 6;
    if (horiz()) {
        int yy = tbpos == 1 ? cpy + 2 : cpy + cph - 5;
        fill_rr(&scr, x + 20 - len / 2, yy, len, 3, 1, col, 255);
    } else {
        int xx = tbpos == 0 ? cpx + 2 : cpx + cpw - 5;
        fill_rr(&scr, xx, y + 20 - len / 2, 3, len, 1, col, 255);
    }
}

static void draw_taskbar(void) {
    int sx0 = cpx - 22, sy0 = cpy - 22;
    if (sx0 >= scr.cx1 || sx0 + cpw + 44 <= scr.cx0 || sy0 >= scr.cy1 || sy0 + cph + 44 <= scr.cy0) return;
    soft_shadow(&scr, cpx, cpy + 3, cpw, cph, 24, 20, 120);
    glass(&scr, cpx, cpy, cpw, cph, cpx, cpy, cpw, cph, 24, 0x1E3560, 108, 8, 70);
    glass_edge(&scr, cpx, cpy, cpw, cph, 24, 200, 90);
    int x, y;
    start_xy(&x, &y);
    draw_icon(&scr, x, y, 40, menu_open ? 0xE0344D : 0xC81E3A, 'A', menu_open || tb_hov == -1, 0, 2);
    if (horiz()) vline(&scr, cpx + 56, cpy + 12, cph - 24, 0x5C74A8);
    else hline(&scr, cpx + 12, cpy + 56, cpw - 24, 0x5C74A8);
    win *s[MAXW];
    sorted(s);
    for (int i = 0; i < nw && i < max_items(); i++) {
        win *w = s[i];
        item_xy(i, &x, &y);
        int mn = w->flags & WF_MIN;
        draw_icon(&scr, x, y, 40, w->icol, w->ich, tb_hov == i, mn, 2);
        indicator(x, y, w == focus);
    }
    clock_xy(&x, &y);
    text(&scr, x + 1, y + 1, clk, 0x0A1224);
    text(&scr, x, y, clk, C_WHITE);
    text(&scr, x, y + 18, dat, 0xB9C6E4);
    if (tb_hov >= -1) {
        const char *t = "Apps";
        int ix, iy;
        start_xy(&ix, &iy);
        if (tb_hov >= 0) { sorted(s); if (tb_hov < nw) t = s[tb_hov]->title; item_xy(tb_hov, &ix, &iy); }
        int tw = (int)strlen(t) * 8 + 24, tx, ty;
        if (horiz()) { tx = ix; ty = tbpos == 1 ? cpy + cph + 10 : cpy - 38; }
        else { tx = tbpos == 0 ? cpx + cpw + 10 : cpx - tw - 10; ty = iy + 7; }
        soft_shadow(&scr, tx, ty + 2, tw, 26, 13, 10, 100);
        glass(&scr, tx, ty, tw, 26, tx, ty, tw, 26, 13, 0x142038, 150, 5, 50);
        glass_edge(&scr, tx, ty, tw, 26, 13, 190, 80);
        text(&scr, tx + 13, ty + 5, t, C_WHITE);
    }
}

static void draw_menu(void) {
    int x, y, mw, h;
    menu_rect(&x, &y, &mw, &h);
    if (x - 32 >= scr.cx1 || x + MENUW + 36 <= scr.cx0 || y - 32 >= scr.cy1 || y + h + 36 <= scr.cy0) return;
    soft_shadow(&scr, x, y + 5, MENUW, h, 24, 28, 118);
    glass(&scr, x, y, MENUW, h, x, y, MENUW, h, 24, 0x263754, 154, 10, 44);
    glass_edge(&scr, x, y, MENUW, h, 24, 168, 54);

    draw_icon(&scr, x + 18, y + 13, 28, 0xC81E3A, 'A', menu_open, 0, 1);
    text(&scr, x + 58, y + 11, "Astros", C_WHITE);
    text(&scr, x + 58, y + 27, "Applications", 0xCFD8EA);
    fill_rr(&scr, x + MENUW - 42, y + 16, 24, 24, 12, 0xFFFFFF, 22);
    for (int dot = 0; dot < 3; dot++)
        fill_rr(&scr, x + MENUW - 35 + dot * 6, y + 26, 3, 3, 2, 0xE8EEFA, 210);
    fill_a(&scr, x + 16, y + MENU_HEAD, MENUW - 32, 1, 0xFFFFFF, 45);

    int yy = y + MENU_HEAD + MENU_PAD / 2;
    for (int i = 0; i < nmenu; i++) {
        struct mi *m = &menu[i];
        if (m->act == 0) {
            fill_a(&scr, x + 20, yy + ITEMH / 2, MENUW - 40, 1, 0xDCE7FB, 48);
            yy += ITEMH;
            continue;
        }
        if (i == menu_hov) {
            fill_rr(&scr, x + 10, yy + 2, MENUW - 20, ITEMH - 4, 13, 0x8DB8FF, 92);
        }
        draw_icon(&scr, x + 19, yy + 7, 24, m->col, (u8)m->letter, i == menu_hov, 0, 1);
        text(&scr, x + 57, yy + (ITEMH - 16) / 2, m->label, 0xF4F6FC);
        yy += ITEMH;
    }
}

static void compose(void) {
    if (dx1 <= dx0 || dy1 <= dy0) return;
    int x = dx0, y = dy0, w = dx1 - dx0, h = dy1 - dy0;
    clip_set(&scr, x, y, w, h);
    if (session_gate) {
        draw_win(session_gate);
    } else {
        blit(&scr, 0, 0, wp, 0, 0, sw, sh);
        for (int i = 0; i < nw; i++) if (!(wins[i]->flags & WF_MIN)) draw_win(wins[i]);
        draw_taskbar();
        if (menu_open) draw_menu();
    }
    clip_reset(&scr);
    dx0 = dx1 = dy0 = dy1 = 0;
    present(x, y, w, h);
    cursor_redraw();
}

static void menu_run(struct mi *m) {
    menu_open = 0;
    dirty_menu();
    dirty_tb();
    if (m->act == 1) {
        win *w = apps[m->appi].open(NULL);
        (void)w;
    } else if (m->act == 2) {
        proc_spawn(m->path, NULL, NULL);
    } else if (m->act == 3) {
        sys_reboot();
    } else if (m->act == 4) {
        sys_poweroff();
    }
}

static int menu_at(int px, int py) {
    int x, y, w, h;
    menu_rect(&x, &y, &w, &h);
    if (!inside(px, py, x, y, w, h)) return -2;
    int item_y = py - y - MENU_HEAD - MENU_PAD / 2;
    if (item_y < 0) return -1;
    int i = item_y / ITEMH;
    if (i < 0 || i >= nmenu) return -1;
    return menu[i].act ? i : -1;
}

static void set_menu(int on) {
    if (on) wm_menu_refresh();
    menu_open = on;
    menu_hov = -1;
    dirty_menu();
    dirty_tb();
}

static void taskbar_click(int hit) {
    if (hit == -1) { set_menu(!menu_open); return; }
    if (hit < 0) return;
    win *s[MAXW];
    sorted(s);
    if (hit >= nw) return;
    win *w = s[hit];
    if (w->flags & WF_MIN) wm_focus(w);
    else if (w == focus) minimize(w);
    else wm_focus(w);
}

static void hover_update(int px, int py) {
    int hit = tb_hit(px, py);
    int th = hit == -3 ? -2 : hit;
    if (th != tb_hov) { tb_hov = th; dirty_tb(); tip_dirty(); }
    int id = 0, code = 0;
    win *w = hit == -3 ? win_at(px, py) : NULL;
    if (w) {
        int r = region(w, px, py);
        if (r == R_CLOSE || r == R_MAX || r == R_MIN) { id = w->id; code = r; }
    }
    if (id != hov_id || code != hov_code) {
        win *o = wm_byid(hov_id);
        if (o) wm_dirty(o->x, o->y, o->w + 2 * BD, TB);
        hov_id = id; hov_code = code;
        if (w) wm_dirty(w->x, w->y, w->w + 2 * BD, TB);
    }
}

static void mouse_ev(struct ev *e) {
    int px = e->x, py = e->y, b = e->btn, pb = bprev;
    int press = b & ~pb, rel = pb & ~b;
    bprev = b;
    if (session_gate) {
        if (session_gate->on_mouse) session_gate->on_mouse(session_gate, px, py, b, pb, e->wheel);
        return;
    }
    hover_update(px, py);

    if (menu_open) {
        int mi = menu_at(px, py);
        int nh = mi >= 0 ? mi : -1;
        if (nh != menu_hov) { menu_hov = nh; dirty_menu(); }
        if (press & M_L) {
            if (mi >= 0) menu_run(&menu[mi]);
            else if (tb_hit(px, py) == -1) set_menu(0);
            else if (mi == -2) set_menu(0);
        }
        return;
    }
    if (dragw) {
        if (b & M_L) {
            dirty_win(dragw);
            dragw->x = px - ddx;
            dragw->y = py - ddy;
            if (dragw->y < dky) dragw->y = dky;
            if (dragw->y > dky + dkh - TB) dragw->y = dky + dkh - TB;
            if (dragw->x < dkx + 100 - dragw->w) dragw->x = dkx + 100 - dragw->w;
            if (dragw->x > dkx + dkw - 100) dragw->x = dkx + dkw - 100;
            dirty_win(dragw);
        } else dragw = NULL;
        return;
    }
    if (resw) {
        if (b & M_L) wm_resize(resw, rsw0 + px - rsx, rsh0 + py - rsy);
        else resw = NULL;
        return;
    }
    if (cap) {
        to_mouse(cap, px, py, b, pb, e->wheel);
        if (!b) cap = NULL;
        return;
    }
    int th = tb_hit(px, py);
    if (th != -3) {
        if (press & M_L) taskbar_click(th);
        return;
    }
    win *w = win_at(px, py);
    if (e->wheel) {
        win *t = w ? w : focus;
        if (t) to_mouse(t, px, py, b, b, e->wheel);
        return;
    }
    if (!w) return;
    if (press) {
        if (w != focus) wm_focus(w);
        int r = region(w, px, py);
        if ((press & M_L) && r == R_CLOSE) req_close(w);
        else if ((press & M_L) && r == R_MAX) maximize(w, !(w->flags & WF_MAX));
        else if ((press & M_L) && r == R_MIN) minimize(w);
        else if ((press & M_L) && r == R_TITLE) {
            if (last_click_id == w->id && ticks - last_click < 40 && !(w->flags & WF_NOMAX)) {
                maximize(w, !(w->flags & WF_MAX));
                last_click_id = 0;
            } else {
                last_click = ticks;
                last_click_id = w->id;
                if (!(w->flags & WF_MAX)) { dragw = w; ddx = px - w->x; ddy = py - w->y; }
            }
        } else if ((press & M_L) && r == R_SIZE) {
            resw = w; rsx = px; rsy = py; rsw0 = w->w; rsh0 = w->h;
        } else if (r == R_CLIENT) {
            cap = w;
            to_mouse(w, px, py, b, pb, 0);
        }
        return;
    }
    if (rel) return;
    if (region(w, px, py) == R_CLIENT) to_mouse(w, px, py, b, pb, 0);
}

static void key_ev(struct ev *e) {
    int k = e->key, d = e->down;
    if (session_gate) {
        to_key(session_gate, k, d, e->mod);
        return;
    }
    if (k == K_SUPER) { if (d) set_menu(!menu_open); return; }
    if (d && (e->mod & MOD_ALT)) {
        if (k == K_F4 && focus) { req_close(focus); return; }
        if (k == '\t' && nw > 1) {
            for (int i = 0; i < nw; i++) {
                if (!(wins[i]->flags & WF_MIN)) { wm_focus(wins[i]); break; }
            }
            return;
        }
    }
    if (menu_open) {
        if (d && k == 27) set_menu(0);
        return;
    }
    if (focus) to_key(focus, k, d, e->mod);
}

static void clock_update(void) {
    int h, m, s, d, mo, y;
    if (!rtc_get(&h, &m, &s, &d, &mo, &y)) return;
    char a[8], b[8];
    snprintf(a, sizeof a, "%02d:%02d", h, m);
    snprintf(b, sizeof b, "%02d/%02d", d, mo);
    if (strcmp(a, clk) || strcmp(b, dat)) {
        memcpy(clk, a, 6);
        memcpy(dat, b, 6);
        { int cx, cy; clock_xy(&cx, &cy); wm_dirty(cx - 4, cy - 4, 64, 44); }
    }
}

void wm_tbpos(int p) {
    if (p < 0 || p > 3 || p == tbpos) return;
    tbpos = p;
    tb_layout();
    for (int i = 0; i < nw; i++) {
        win *w = wins[i];
        int W = w->w + 2 * BD, H = w->h + TB + BD;
        if (w->flags & WF_MAX) { w->x = dkx; w->y = dky; wm_resize(w, dkw - 2 * BD, dkh - TB - BD); continue; }
        if (w->x + W > dkx + dkw) w->x = dkx + dkw - W;
        if (w->y + H > dky + dkh) w->y = dky + dkh - H;
        if (w->x < dkx) w->x = dkx;
        if (w->y < dky) w->y = dky;
    }
    if (!wp_custom) default_wall();
    menu_open = 0;
    wm_dirty(0, 0, sw, sh);
}

int wm_tbpos_get(void) { return tbpos; }

void wm_relayout(void) {
    tb_layout();
    img_free(wp);
    wp = img_new(sw, sh);
    wp_custom = 0;
    default_wall();
    mouse_clip(sw, sh);
    for (int i = 0; i < nw; i++) {
        win *w = wins[i];
        int W = w->w + 2 * BD, H = w->h + TB + BD;
        if (w->flags & WF_MAX) { w->x = dkx; w->y = dky; wm_resize(w, dkw - 2 * BD, dkh - TB - BD); continue; }
        if (W > dkw) wm_resize(w, dkw - 2 * BD, w->h);
        if (H > dkh) wm_resize(w, w->w, dkh - TB - BD);
        W = w->w + 2 * BD; H = w->h + TB + BD;
        if (w->x + W > dkx + dkw) w->x = dkx + dkw - W;
        if (w->y + H > dky + dkh) w->y = dky + dkh - H;
        if (w->x < dkx) w->x = dkx;
        if (w->y < dky) w->y = dky;
    }
    menu_open = 0;
    wm_dirty(0, 0, sw, sh);
}

void wm_init(void) {
    tb_layout();
    wp = img_new(sw, sh);
    default_wall();
    wm_menu_refresh();
    mouse_clip(sw, sh);
    wm_dirty(0, 0, sw, sh);
}

void wm_run(void) {
    u64 last = 0, lastsec = 0;
    clock_update();
    for (;;) {
        net_poll();
        net_stack_poll();
        struct ev e;
        int any = 0;
        while (ev_pop(&e)) {
            any = 1;
            if (e.type == EV_MOUSE) mouse_ev(&e);
            else key_ev(&e);
        }
        if (ticks != last) {
            last = ticks;
            for (int i = 0; i < nw; i++) {
                win *w = wins[i];
                if (!(w->flags & WF_USER) && w->on_tick) w->on_tick(w);
            }
            if (ticks - lastsec >= 100) { lastsec = ticks; clock_update(); }
        }
        compose();
        cursor_move(mx, my);
        if (!any && dx1 <= dx0) {
            if (proc_runnable()) proc_yield();
            else { wm_idle = 1; halt(); wm_idle = 0; }
        } else if (proc_runnable()) {
            proc_yield();
        }
    }
}
