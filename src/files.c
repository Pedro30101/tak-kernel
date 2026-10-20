#include "tak.h"
#include "auth.h"
#include "wm.h"
#include "vfs.h"
#include "input.h"

#define ROWH 26
#define TOP 40
#define SIDE 132
#define STAT 26

struct fm {
    node *cwd;
    node *list[512];
    int n, sel, top;
    node *hist[16];
    int nh, pmode, plen, lsel, hov;
    char pbuf[64], msg[80];
    u64 lt;
};

static node *clipn;

static int cmpn(node *a, node *b) {
    if (a->dir != b->dir) return b->dir - a->dir;
    return strcasecmp(a->name, b->name);
}

static void refresh(struct fm *f) {
    vfs_load(f->cwd);
    f->n = 0;
    for (node *k = f->cwd->kid; k && f->n < 512; k = k->nxt) f->list[f->n++] = k;
    for (int i = 1; i < f->n; i++)
        for (int j = i; j > 0 && cmpn(f->list[j - 1], f->list[j]) > 0; j--) {
            node *t = f->list[j]; f->list[j] = f->list[j - 1]; f->list[j - 1] = t;
        }
}

static u32 icol(node *n) {
    if (n->dir) return 0xF59E0B;
    const char *e = ext_of(n->name);
    if (!strcasecmp(e, "te")) return 0x7C3AED;
    if (!strcasecmp(e, "png") || !strcasecmp(e, "jpg") || !strcasecmp(e, "jpeg") || !strcasecmp(e, "bmp")) return 0x0E7490;
    if (!strcasecmp(e, "wav")) return 0xDB2777;
    if (!strcasecmp(e, "txt") || !strcasecmp(e, "md") || !strcasecmp(e, "c") || !strcasecmp(e, "h")) return 0x4B5563;
    return 0x374151;
}

static const struct { const char *name, *path; } places[] = {
    { "Home", NULL }, { "Apps", "/apps" }, { "Pictures", "/pictures" },
    { "Music", "/music" }, { "Disk", "/disk" }, { "Root", "/" }
};

static void draw(win *w) {
    struct fm *f = w->priv;
    img *c = &w->surf;
    int lh = w->h - TOP - STAT - (f->pmode ? 34 : 0);
    int rows = lh / ROWH;
    fill(c, 0, 0, w->w, w->h, C_PANEL);
    fill(c, 0, 0, w->w, TOP, 0x111827);
    ui_btn(c, 6, 7, 30, 26, "<", 0);
    ui_btn(c, 40, 7, 30, 26, "^", 0);
    char p[160];
    vfs_path(f->cwd, p, sizeof p);
    int bx = w->w - 6;
    bx -= 64; ui_btn(c, bx, 7, 64, 26, "Delete", 0);
    bx -= 84; ui_btn(c, bx, 7, 80, 26, "New file", 0);
    bx -= 100; ui_btn(c, bx, 7, 96, 26, "New folder", 0);
    ui_field(c, 78, 7, bx - 84, 26, p, 0);
    fill(c, 0, TOP, SIDE, w->h - TOP, 0x0F172A);
    int py = TOP + 8;
    for (int i = 0; i < 6; i++) {
        node *pn = places[i].path ? vfs_find(places[i].path, NULL) : auth_home_dir();
        if (!pn) continue;
        if (pn == f->cwd) fill(c, 0, py - 4, SIDE, 28, C_DRED);
        text(c, 14, py, places[i].name, C_TEXT);
        py += 30;
    }
    int lx = SIDE;
    int lw = w->w - SIDE - 10;
    clip_set(c, lx, TOP, w->w - lx, lh);
    for (int i = 0; i < rows + 1; i++) {
        int idx = f->top + i;
        if (idx >= f->n) break;
        node *k = f->list[idx];
        int y = TOP + i * ROWH;
        if (idx == f->sel) fill(c, lx, y, lw, ROWH, C_DRED);
        else if (idx == f->hov) fill(c, lx, y, lw, ROWH, 0x1F2937);
        if (k->dir) {
            fill(c, lx + 10, y + 7, 10, 4, 0xF59E0B);
            fill(c, lx + 10, y + 10, 22, 14, 0xF59E0B);
        } else {
            fill(c, lx + 12, y + 4, 18, 18, icol(k));
            const char *e = ext_of(k->name);
            glyph(c, lx + 16, y + 4, toupper((u8)(*e ? *e : k->name[0])), C_WHITE, -1, 1);
        }
        text(c, lx + 44, y + 5, k->name, C_TEXT);
        if (!k->dir) {
            char sz[24];
            if (k->size >= 1048576) snprintf(sz, sizeof sz, "%lu.%lu MB", k->size >> 20, (k->size * 10 >> 20) % 10);
            else if (k->size >= 1024) snprintf(sz, sizeof sz, "%lu KB", k->size >> 10);
            else snprintf(sz, sizeof sz, "%lu B", k->size);
            text(c, lx + lw - (int)strlen(sz) * 8 - 8, y + 5, sz, C_DIM);
        }
    }
    clip_reset(c);
    if (!f->n) text(c, lx + 20, TOP + 20, "This folder is empty.", C_DIM);
    ui_vscroll(c, w->w - 10, TOP, lh, f->top, f->n, rows);
    int sy = w->h - STAT;
    if (f->pmode) {
        fill(c, SIDE, sy - 34, w->w - SIDE, 34, 0x1F2937);
        text(c, SIDE + 10, sy - 26, f->pmode == 1 ? "New folder:" : f->pmode == 2 ? "New file:" : "Rename to:", C_YELLOW);
        ui_field(c, SIDE + 110, sy - 30, w->w - SIDE - 124, 26, f->pbuf, 1);
    }
    fill(c, 0, sy, w->w, STAT, 0x111827);
    char b[120];
    if (f->msg[0]) snprintf(b, sizeof b, "%s", f->msg);
    else if (f->sel >= 0 && f->sel < f->n) snprintf(b, sizeof b, "%d items  -  %s selected", f->n, f->list[f->sel]->name);
    else snprintf(b, sizeof b, "%d items", f->n);
    text(c, 10, sy + 5, b, f->msg[0] ? C_YELLOW : C_DIM);
    wm_paint(w, 0, 0, 0, 0);
}

static void go(win *w, node *n) {
    struct fm *f = w->priv;
    if (!n || !n->dir) return;
    if (f->nh < 16) f->hist[f->nh++] = f->cwd;
    else { memmove(f->hist, f->hist + 1, 15 * sizeof(node *)); f->hist[15] = f->cwd; }
    f->cwd = n;
    f->sel = -1; f->top = 0; f->msg[0] = 0;
    refresh(f);
    char p[160];
    vfs_path(n, p, sizeof p);
    wm_title(w, p);
    draw(w);
}

static void say(win *w, const char *m) {
    struct fm *f = w->priv;
    snprintf(f->msg, sizeof f->msg, "%s", m);
    draw(w);
}

static void do_open(win *w, node *k) {
    if (k->dir) go(w, k);
    else open_file(k);
}

static void do_delete(win *w) {
    struct fm *f = w->priv;
    if (f->sel < 0 || f->sel >= f->n) return;
    node *k = f->list[f->sel];
    if (vfs_rm(k) < 0) { say(w, "Cannot delete (folder not empty or read-only)"); return; }
    f->sel = -1;
    refresh(f);
    f->msg[0] = 0;
    draw(w);
}

static void commit(win *w) {
    struct fm *f = w->priv;
    int m = f->pmode;
    f->pmode = 0;
    if (!f->plen) { draw(w); return; }
    if (m == 1) { if (!vfs_mk(f->cwd, f->pbuf, 1)) { say(w, "Could not create folder"); return; } }
    else if (m == 2) { if (!vfs_mk(f->cwd, f->pbuf, 0)) { say(w, "Could not create file"); return; } }
    else if (m == 3 && f->sel >= 0 && f->sel < f->n) {
        if (vfs_rename(f->list[f->sel], f->pbuf) < 0) { say(w, "Rename failed"); return; }
    }
    f->msg[0] = 0;
    refresh(f);
    draw(w);
}

static void prompt(win *w, int mode, const char *init) {
    struct fm *f = w->priv;
    f->pmode = mode;
    snprintf(f->pbuf, sizeof f->pbuf, "%s", init);
    f->plen = (int)strlen(f->pbuf);
    draw(w);
}

static void paste(win *w) {
    struct fm *f = w->priv;
    if (!clipn || clipn->dir) { say(w, "Nothing to paste (copy a file first)"); return; }
    u64 sz;
    u8 *b = vfs_slurp(clipn, &sz);
    if (!b) { say(w, "Cannot read the copied file"); return; }
    char nm[64];
    snprintf(nm, sizeof nm, "%s", clipn->name);
    if (vfs_child(f->cwd, nm)) snprintf(nm, sizeof nm, "copy_%s", clipn->name);
    int r = vfs_save(f->cwd, nm, b, sz);
    kfree(b);
    if (r < 0) say(w, "Paste failed");
    else { refresh(f); say(w, "Pasted"); }
}

static void key(win *w, int k, int mod) {
    struct fm *f = w->priv;
    if (f->pmode) {
        if (k == 27) { f->pmode = 0; draw(w); }
        else if (k == '\n') commit(w);
        else if (k == '\b') { if (f->plen) f->pbuf[--f->plen] = 0; draw(w); }
        else if (k >= 32 && k < 256 && f->plen < 58 && k != '/') { f->pbuf[f->plen++] = (char)k; f->pbuf[f->plen] = 0; draw(w); }
        return;
    }
    int rows = (w->h - TOP - STAT) / ROWH;
    if (mod & MOD_CTRL) {
        if (k == 'c' && f->sel >= 0 && f->sel < f->n) { clipn = f->list[f->sel]; say(w, "Copied"); }
        else if (k == 'v') paste(w);
        return;
    }
    switch (k) {
    case K_UP: if (f->sel > 0) f->sel--; else if (f->sel < 0 && f->n) f->sel = 0; break;
    case K_DOWN: if (f->sel < f->n - 1) f->sel++; break;
    case K_HOME: f->sel = f->n ? 0 : -1; break;
    case K_END: f->sel = f->n - 1; break;
    case K_PGUP: f->sel -= rows; if (f->sel < 0) f->sel = f->n ? 0 : -1; break;
    case K_PGDN: f->sel += rows; if (f->sel >= f->n) f->sel = f->n - 1; break;
    case '\n': if (f->sel >= 0 && f->sel < f->n) do_open(w, f->list[f->sel]); return;
    case '\b': if (f->cwd->par) go(w, f->cwd->par); return;
    case K_DEL: do_delete(w); return;
    case K_F2: if (f->sel >= 0 && f->sel < f->n) prompt(w, 3, f->list[f->sel]->name); return;
    default: return;
    }
    if (f->sel < f->top) f->top = f->sel;
    if (f->sel >= f->top + rows) f->top = f->sel - rows + 1;
    draw(w);
}

static void mouse(win *w, int x, int y, int b, int pb, int wheel) {
    struct fm *f = w->priv;
    int rows = (w->h - TOP - STAT) / ROWH;
    if (wheel) {
        f->top -= wheel * 3;
        if (f->top > f->n - rows) f->top = f->n - rows;
        if (f->top < 0) f->top = 0;
        draw(w);
        return;
    }
    int hov = -1;
    if (x >= SIDE && y >= TOP && y < w->h - STAT) {
        int i = f->top + (y - TOP) / ROWH;
        if (i < f->n) hov = i;
    }
    int press = (b & 1) && !(pb & 1);
    if (!press) {
        if (hov != f->hov) { f->hov = hov; draw(w); }
        return;
    }
    if (y < TOP) {
        int bx = w->w - 6 - 64;
        if (x >= bx) do_delete(w);
        else if (x >= bx - 84) prompt(w, 2, "");
        else if (x >= bx - 184) prompt(w, 1, "");
        else if (inside(x, y, 6, 7, 30, 26)) { if (f->nh) { node *n = f->hist[--f->nh]; f->cwd = n; f->sel = -1; f->top = 0; refresh(f); char p[160]; vfs_path(n, p, sizeof p); wm_title(w, p); draw(w); } }
        else if (inside(x, y, 40, 7, 30, 26)) { if (f->cwd->par) go(w, f->cwd->par); }
        return;
    }
    if (x < SIDE) {
        int py = TOP + 8;
        for (int i = 0; i < 6; i++) {
            node *pn = places[i].path ? vfs_find(places[i].path, NULL) : auth_home_dir();
            if (!pn) continue;
            if (y >= py - 4 && y < py + 24) { go(w, pn); return; }
            py += 30;
        }
        return;
    }
    if (hov >= 0) {
        if (hov == f->sel && f->lsel == hov && ticks - f->lt < 40) { do_open(w, f->list[hov]); f->lt = 0; return; }
        f->sel = hov; f->lsel = hov; f->lt = ticks; f->msg[0] = 0;
        draw(w);
    } else if (f->sel != -1) { f->sel = -1; draw(w); }
}

static void resize(win *w, int nw, int nh) { (void)nw; (void)nh; draw(w); }
static void on_close(win *w) { kfree(w->priv); }

win *files_open(const char *arg) {
    win *w = wm_open("Files", 780, 520, WF_RESIZE);
    if (!w) return NULL;
    struct fm *f = kzalloc(sizeof *f);
    w->priv = f;
    w->on_key = key;
    w->on_mouse = mouse;
    w->on_resize = resize;
    w->on_close = on_close;
    w->ich = 'F';
    w->icol = 0xF59E0B;
    f->sel = -1;
    f->hov = -1;
    f->cwd = arg && *arg ? vfs_find(arg, NULL) : auth_home_dir();
    if (!f->cwd || !f->cwd->dir) f->cwd = vroot;
    refresh(f);
    char p[160];
    vfs_path(f->cwd, p, sizeof p);
    wm_title(w, p);
    draw(w);
    return w;
}
