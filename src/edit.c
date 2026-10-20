#include "tak.h"
#include "auth.h"
#include "wm.h"
#include "vfs.h"
#include "input.h"

#define GUT 48
#define BAR 34
#define STAT 24

struct ed {
    char *b;
    int len, cap, cur, top, left, want, mod;
    char path[128];
    int pmode, plen;
    char pbuf[96], msg[64];
    int blink;
};

static void ensure(struct ed *e, int n) {
    if (e->len + n + 1 <= e->cap) return;
    int c = e->cap ? e->cap * 2 : 4096;
    while (c < e->len + n + 1) c *= 2;
    e->b = krealloc(e->b, c);
    e->cap = c;
}

static int line_start(struct ed *e, int p) {
    while (p > 0 && e->b[p - 1] != '\n') p--;
    return p;
}

static int line_end(struct ed *e, int p) {
    while (p < e->len && e->b[p] != '\n') p++;
    return p;
}

static void cur_rc(struct ed *e, int *r, int *c) {
    int row = 0;
    for (int i = 0; i < e->cur; i++) if (e->b[i] == '\n') row++;
    *r = row;
    *c = e->cur - line_start(e, e->cur);
}

static void scroll_to_cursor(win *w) {
    struct ed *e = w->priv;
    int rows = (w->h - BAR - STAT) / 16;
    int cols = (w->w - GUT - 12) / 8;
    int r, c;
    cur_rc(e, &r, &c);
    if (r < e->top) e->top = r;
    if (r >= e->top + rows) e->top = r - rows + 1;
    if (c < e->left) e->left = c;
    if (c >= e->left + cols) e->left = c - cols + 1;
}

static void draw(win *w) {
    struct ed *e = w->priv;
    img *c = &w->surf;
    int rows = (w->h - BAR - STAT) / 16;
    fill(c, 0, 0, w->w, w->h, 0x0F172A);
    fill(c, 0, 0, w->w, BAR, 0x111827);
    ui_btn(c, 6, 4, 60, 26, "Save", 0);
    ui_btn(c, 72, 4, 80, 26, "Save as", 0);
    char t[160];
    snprintf(t, sizeof t, "%s%s", e->path[0] ? e->path : "(untitled)", e->mod ? " *" : "");
    text(c, 164, 9, t, C_TEXT);
    fill(c, 0, BAR, GUT, w->h - BAR - STAT, 0x111827);
    int p = 0, row = 0;
    while (row < e->top && p < e->len) { if (e->b[p] == '\n') row++; p++; }
    int cr, cc;
    cur_rc(e, &cr, &cc);
    clip_set(c, GUT, BAR, w->w - GUT, rows * 16);
    for (int i = 0; i <= rows; i++) {
        int y = BAR + i * 16;
        int le = line_end(e, p);
        if (p > e->len) break;
        char n[12];
        snprintf(n, sizeof n, "%5d", e->top + i + 1);
        if (p <= e->len) {
            clip_reset(c);
            clip_set(c, 0, BAR, GUT, rows * 16);
            text(c, 4, y, n, 0x6B7280);
            clip_reset(c);
            clip_set(c, GUT, BAR, w->w - GUT, rows * 16);
        }
        if (e->top + i == cr) fill(c, GUT, y, w->w - GUT, 16, 0x1E293B);
        for (int k = p + e->left, x = 0; k < le && x * 8 < w->w; k++, x++)
            glyph(c, GUT + 6 + x * 8, y, (u8)e->b[k], C_TEXT, -1, 1);
        if (e->top + i == cr && e->blink) fill(c, GUT + 6 + (cc - e->left) * 8, y, 2, 16, C_LRED);
        if (le >= e->len) break;
        p = le + 1;
    }
    clip_reset(c);
    int sy = w->h - STAT;
    fill(c, 0, sy, w->w, STAT, 0x111827);
    if (e->pmode) {
        fill(c, 0, sy - 34, w->w, 34, 0x1F2937);
        text(c, 10, sy - 26, "Save as path:", C_YELLOW);
        ui_field(c, 130, sy - 30, w->w - 144, 26, e->pbuf, 1);
    }
    char b[96];
    snprintf(b, sizeof b, "Ln %d, Col %d    %d bytes    %s", cr + 1, cc + 1, e->len, e->msg);
    text(c, 10, sy + 4, b, C_DIM);
    wm_paint(w, 0, 0, 0, 0);
}

static void save_to(win *w, const char *path) {
    struct ed *e = w->priv;
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s", path);
    char *sl = strrchr(tmp, '/');
    node *dir = vroot;
    const char *nm = tmp;
    if (sl) { nm = sl + 1; if (sl != tmp) { *sl = 0; dir = vfs_find(tmp, NULL); } }
    if (!dir || !dir->dir || !*nm || vfs_save(dir, nm, e->b ? e->b : "", e->len) < 0) {
        snprintf(e->msg, sizeof e->msg, "Save failed");
    } else {
        snprintf(e->path, sizeof e->path, "%s", path);
        e->mod = 0;
        snprintf(e->msg, sizeof e->msg, "Saved");
        wm_title(w, nm);
    }
}

static void do_save(win *w) {
    struct ed *e = w->priv;
    if (e->path[0]) save_to(w, e->path);
    else {
        node *home = auth_home_dir();
        if (!home) snprintf(e->msg, sizeof e->msg, "Persistent home folder unavailable");
        else {
            vfs_path(home, e->pbuf, sizeof e->pbuf);
            size_t n = strlen(e->pbuf);
            if (n + sizeof "/untitled.txt" <= sizeof e->pbuf) {
                memcpy(e->pbuf + n, "/untitled.txt", sizeof "/untitled.txt");
                e->plen = (int)strlen(e->pbuf);
                e->pmode = 1;
            } else snprintf(e->msg, sizeof e->msg, "Home path is too long");
        }
    }
    draw(w);
}

static void insert(win *w, const char *s, int n) {
    struct ed *e = w->priv;
    ensure(e, n);
    memmove(e->b + e->cur + n, e->b + e->cur, e->len - e->cur);
    memcpy(e->b + e->cur, s, n);
    e->len += n;
    e->cur += n;
    e->b[e->len] = 0;
    e->mod = 1;
    e->msg[0] = 0;
}

static void key(win *w, int k, int mod) {
    struct ed *e = w->priv;
    e->blink = 1;
    if (e->pmode) {
        if (k == 27) e->pmode = 0;
        else if (k == '\n') { e->pmode = 0; if (e->plen) save_to(w, e->pbuf); }
        else if (k == '\b') { if (e->plen) e->pbuf[--e->plen] = 0; }
        else if (k >= 32 && k < 256 && e->plen < 90) { e->pbuf[e->plen++] = (char)k; e->pbuf[e->plen] = 0; }
        draw(w);
        return;
    }
    if (mod & MOD_CTRL) {
        if (k == 's' || k == 'S') do_save(w);
        return;
    }
    int keep = 0;
    switch (k) {
    case K_LEFT: if (e->cur > 0) e->cur--; break;
    case K_RIGHT: if (e->cur < e->len) e->cur++; break;
    case K_HOME: e->cur = line_start(e, e->cur); break;
    case K_END: e->cur = line_end(e, e->cur); break;
    case K_UP: case K_DOWN: case K_PGUP: case K_PGDN: {
        int n = (k == K_PGUP || k == K_PGDN) ? (w->h - BAR - STAT) / 16 - 1 : 1;
        int ls = line_start(e, e->cur), col = e->cur - ls;
        if (e->want > col) col = e->want;
        e->want = col;
        keep = 1;
        for (int i = 0; i < n; i++) {
            if (k == K_UP || k == K_PGUP) {
                if (ls == 0) break;
                ls = line_start(e, ls - 1);
            } else {
                int le = line_end(e, ls);
                if (le >= e->len) break;
                ls = le + 1;
            }
        }
        int le = line_end(e, ls);
        e->cur = ls + col > le ? le : ls + col;
        break;
    }
    case '\b':
        if (e->cur > 0) { memmove(e->b + e->cur - 1, e->b + e->cur, e->len - e->cur); e->cur--; e->len--; e->b[e->len] = 0; e->mod = 1; }
        break;
    case K_DEL:
        if (e->cur < e->len) { memmove(e->b + e->cur, e->b + e->cur + 1, e->len - e->cur - 1); e->len--; e->b[e->len] = 0; e->mod = 1; }
        break;
    case '\n': insert(w, "\n", 1); break;
    case '\t': insert(w, "    ", 4); break;
    default:
        if (k >= 32 && k < 256) { char ch = (char)k; insert(w, &ch, 1); }
        else return;
    }
    if (!keep) e->want = 0;
    scroll_to_cursor(w);
    draw(w);
}

static void mouse(win *w, int x, int y, int b, int pb, int wheel) {
    struct ed *e = w->priv;
    if (wheel) {
        e->top -= wheel * 3;
        if (e->top < 0) e->top = 0;
        draw(w);
        return;
    }
    if (!(b & 1) || (pb & 1)) return;
    if (y < BAR) {
        if (inside(x, y, 6, 4, 60, 26)) do_save(w);
        else if (inside(x, y, 72, 4, 80, 26)) {
            e->pmode = 1;
            if (e->path[0]) snprintf(e->pbuf, sizeof e->pbuf, "%s", e->path);
            else {
                node *home = auth_home_dir();
                if (!home) { e->pmode = 0; snprintf(e->msg, sizeof e->msg, "Persistent home folder unavailable"); draw(w); return; }
                vfs_path(home, e->pbuf, sizeof e->pbuf);
                size_t n = strlen(e->pbuf);
                if (n + sizeof "/untitled.txt" <= sizeof e->pbuf) memcpy(e->pbuf + n, "/untitled.txt", sizeof "/untitled.txt");
            }
            e->plen = (int)strlen(e->pbuf);
            draw(w);
        }
        return;
    }
    if (x < GUT || y >= w->h - STAT) return;
    int row = e->top + (y - BAR) / 16, col = (x - GUT - 6) / 8 + e->left;
    if (col < 0) col = 0;
    int p = 0, r = 0;
    while (r < row && p < e->len) { if (e->b[p] == '\n') r++; p++; }
    if (r < row) p = e->len;
    int le = line_end(e, p);
    e->cur = p + col > le ? le : p + col;
    e->blink = 1;
    draw(w);
}

static void tick(win *w) {
    struct ed *e = w->priv;
    if (ticks % 50 == 0) { e->blink = !e->blink; draw(w); }
}

static void resize(win *w, int nw, int nh) { (void)nw; (void)nh; scroll_to_cursor(w); draw(w); }
static void on_close(win *w) { struct ed *e = w->priv; kfree(e->b); kfree(e); }

win *edit_open(const char *arg) {
    win *w = wm_open("Text Editor", 760, 520, WF_RESIZE);
    if (!w) return NULL;
    struct ed *e = kzalloc(sizeof *e);
    w->priv = e;
    w->on_key = key;
    w->on_mouse = mouse;
    w->on_tick = tick;
    w->on_resize = resize;
    w->on_close = on_close;
    w->ich = 'E';
    w->icol = 0x2563EB;
    e->blink = 1;
    if (arg && *arg) {
        node *n = vfs_find(arg, NULL);
        if (n && !n->dir) {
            u64 sz;
            u8 *d = vfs_slurp(n, &sz);
            if (d) { e->b = (char *)d; e->len = (int)sz; e->cap = (int)sz + 1; }
            vfs_path(n, e->path, sizeof e->path);
            wm_title(w, n->name);
        }
    }
    draw(w);
    return w;
}
