#include "tw32.h"
#include "tw_font.h"

tw_win *tw_open(const char *title, int w, int h) {
    int id = (int)sc(SYS_WNEW, w, h, (long)title, 0, 0, 0);
    if (id < 0) return NULL;
    long sz = sc(SYS_WSIZE, id, 0, 0, 0, 0, 0);
    int rw = (int)(sz >> 16), rh = (int)(sz & 0xFFFF);
    tw_win *t = malloc(sizeof *t);
    if (!t) return NULL;
    t->id = id; t->w = rw; t->h = rh;
    t->px = calloc((size_t)rw * rh, 4);
    if (!t->px) return NULL;
    return t;
}

void tw_close(tw_win *w) {
    sc(SYS_WCLOSE, w->id, 0, 0, 0, 0, 0);
    free(w->px);
    free(w);
}

void tw_title(tw_win *w, const char *t) { sc(SYS_WTITLE, w->id, (long)t, 0, 0, 0, 0); }

void tw_flip(tw_win *w) {
    sc(SYS_WBLIT, w->id, 0, 0, w->w, w->h, (long)w->px);
    sc(SYS_WFLIP, w->id, 0, 0, 0, 0, 0);
}

void tw_resize(tw_win *w, int nw, int nh) {
    u32 *np = calloc((size_t)nw * nh, 4);
    if (!np) return;
    int cw = nw < w->w ? nw : w->w, ch = nh < w->h ? nh : w->h;
    for (int y = 0; y < ch; y++) memcpy(np + (size_t)y * nw, w->px + (size_t)y * w->w, (size_t)cw * 4);
    free(w->px);
    w->px = np; w->w = nw; w->h = nh;
}

static int fetch(tw_win *w, tw_ev *e, int block) {
    int r = (int)sc(SYS_WEV, w->id, (long)e, block, 0, 0, 0);
    if (r > 0 && e->type == TW_RESIZE) tw_resize(w, e->a, e->b);
    return r;
}

int tw_poll(tw_win *w, tw_ev *e) { return fetch(w, e, 0); }
int tw_wait(tw_win *w, tw_ev *e) { return fetch(w, e, 1); }

void tw_run(tw_win *w, tw_handler h, void *user) {
    tw_ev e;
    for (;;) {
        if (tw_wait(w, &e) < 0) return;
        if (e.type == TW_CLOSE) return;
        if (h(w, &e, user)) return;
    }
}

void tw_pixel(tw_win *w, int x, int y, u32 col) {
    if (x >= 0 && y >= 0 && x < w->w && y < w->h) w->px[(size_t)y * w->w + x] = col;
}

u32 tw_get(tw_win *w, int x, int y) {
    return (x >= 0 && y >= 0 && x < w->w && y < w->h) ? w->px[(size_t)y * w->w + x] : 0;
}

void tw_fill(tw_win *w, int x, int y, int bw, int bh, u32 col) {
    int x1 = x + bw, y1 = y + bh;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > w->w) x1 = w->w;
    if (y1 > w->h) y1 = w->h;
    for (int j = y; j < y1; j++) {
        u32 *p = w->px + (size_t)j * w->w + x;
        for (int i = x; i < x1; i++) *p++ = col;
    }
}

void tw_clear(tw_win *w, u32 col) { tw_fill(w, 0, 0, w->w, w->h, col); }

void tw_rect(tw_win *w, int x, int y, int bw, int bh, u32 col) {
    tw_fill(w, x, y, bw, 1, col);
    tw_fill(w, x, y + bh - 1, bw, 1, col);
    tw_fill(w, x, y, 1, bh, col);
    tw_fill(w, x + bw - 1, y, 1, bh, col);
}

void tw_line(tw_win *w, int x0, int y0, int x1, int y1, u32 col) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        tw_pixel(w, x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void tw_disc(tw_win *w, int cx, int cy, int r, u32 col) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r) tw_pixel(w, cx + x, cy + y, col);
}

void tw_circle(tw_win *w, int cx, int cy, int r, u32 col) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        tw_pixel(w, cx + x, cy + y, col); tw_pixel(w, cx + y, cy + x, col);
        tw_pixel(w, cx - y, cy + x, col); tw_pixel(w, cx - x, cy + y, col);
        tw_pixel(w, cx - x, cy - y, col); tw_pixel(w, cx - y, cy - x, col);
        tw_pixel(w, cx + y, cy - x, col); tw_pixel(w, cx + x, cy - y, col);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

static void glyph(tw_win *w, int x, int y, int ch, u32 col, int sc2) {
    const uint8_t *g = tw_font[ch & 255];
    for (int r = 0; r < 16; r++) {
        uint8_t b = g[r];
        if (!b) continue;
        for (int c = 0; c < 8; c++)
            if (b & (0x80 >> c)) tw_fill(w, x + c * sc2, y + r * sc2, sc2, sc2, col);
    }
}

void tw_text(tw_win *w, int x, int y, const char *s, u32 col) {
    for (; *s; s++, x += 8) glyph(w, x, y, (u8)*s, col, 1);
}

void tw_text2(tw_win *w, int x, int y, const char *s, u32 col) {
    for (; *s; s++, x += 16) glyph(w, x, y, (u8)*s, col, 2);
}

int tw_textw(const char *s) { return (int)strlen(s) * 8; }

void tw_blit(tw_win *w, int x, int y, const u32 *src, int bw, int bh) {
    for (int j = 0; j < bh; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= w->h) continue;
        for (int i = 0; i < bw; i++) {
            int xx = x + i;
            if (xx >= 0 && xx < w->w) w->px[(size_t)yy * w->w + xx] = src[(size_t)j * bw + i];
        }
    }
}

u32 tw_mix(u32 a, u32 b, int t) {
    u32 r = 0;
    for (int s = 0; s < 24; s += 8) {
        int x = (a >> s) & 255, y = (b >> s) & 255;
        r |= (u32)((x * (255 - t) + y * t) / 255) << s;
    }
    return r;
}

void tw_ui_feed(tw_ui *u, const tw_ev *e) {
    if (e->type == TW_MOUSE) {
        int b = e->c & 255, pb = (e->c >> 8) & 255;
        u->mx = e->a; u->my = e->b;
        u->down = b & 1;
        if ((b & 1) && !(pb & 1)) u->click = 1;
        if (!(b & 1) && (pb & 1)) u->released = 1;
        u->wheel += e->d;
    } else if (e->type == TW_KEY && e->b) {
        u->key = e->a;
        u->mod = e->c;
    }
}

void tw_ui_end(tw_ui *u) { u->click = 0; u->released = 0; u->key = 0; u->wheel = 0; }

int tw_hit(tw_ui *u, int x, int y, int w, int h) {
    return u->mx >= x && u->my >= y && u->mx < x + w && u->my < y + h;
}

int tw_button(tw_win *w, tw_ui *u, int x, int y, int bw, int bh, const char *label) {
    int hot = tw_hit(u, x, y, bw, bh);
    u32 bg = hot ? (u->down ? 0x7F1D1D : 0x4B5563) : 0x374151;
    tw_fill(w, x, y, bw, bh, bg);
    tw_rect(w, x, y, bw, bh, hot ? 0xEF4444 : 0x6B7280);
    tw_text(w, x + (bw - tw_textw(label)) / 2, y + (bh - 16) / 2, label, TW_WHITE);
    return hot && u->click;
}

void tw_label(tw_win *w, int x, int y, const char *s, u32 col) { tw_text(w, x, y, s, col); }

int tw_checkbox(tw_win *w, tw_ui *u, int x, int y, const char *label, int *val) {
    int hot = tw_hit(u, x, y, 20 + 8 + tw_textw(label), 20);
    tw_fill(w, x, y, 20, 20, 0x0B1020);
    tw_rect(w, x, y, 20, 20, hot ? 0xEF4444 : 0x6B7280);
    if (*val) tw_fill(w, x + 5, y + 5, 10, 10, 0xEF4444);
    tw_text(w, x + 28, y + 2, label, TW_TEXT);
    if (hot && u->click) { *val = !*val; return 1; }
    return 0;
}

int tw_slider(tw_win *w, tw_ui *u, int x, int y, int sw, int *val, int min, int max) {
    tw_fill(w, x, y + 8, sw, 4, 0x4B5563);
    int pos = max > min ? (*val - min) * (sw - 12) / (max - min) : 0;
    int hot = tw_hit(u, x, y, sw, 20);
    tw_fill(w, x + pos, y + 2, 12, 16, hot ? 0xEF4444 : 0xB91C1C);
    if (hot && u->down && max > min) {
        int v = min + (u->mx - x - 6) * (max - min) / (sw - 12);
        if (v < min) v = min;
        if (v > max) v = max;
        if (v != *val) { *val = v; return 1; }
    }
    return 0;
}

void tw_progress(tw_win *w, int x, int y, int pw, int ph, int val, int max, u32 col) {
    tw_fill(w, x, y, pw, ph, 0x0B1020);
    tw_rect(w, x, y, pw, ph, 0x4B5563);
    if (max > 0 && val > 0) tw_fill(w, x + 1, y + 1, (pw - 2) * (val > max ? max : val) / max, ph - 2, col);
}

int tw_input(tw_win *w, tw_ui *u, int x, int y, int iw, char *buf, int cap, int id) {
    int hot = tw_hit(u, x, y, iw, 24);
    if (hot && u->click) u->focus = id;
    int foc = u->focus == id;
    tw_fill(w, x, y, iw, 24, 0x0B1020);
    tw_rect(w, x, y, iw, 24, foc ? 0xEF4444 : 0x4B5563);
    int n = (int)strlen(buf), maxc = (iw - 10) / 8;
    tw_text(w, x + 5, y + 4, n > maxc ? buf + n - maxc : buf, TW_TEXT);
    int changed = 0;
    if (foc && u->key) {
        if (u->key == '\b') { if (n) { buf[n - 1] = 0; changed = 1; } }
        else if (u->key >= 32 && u->key < 256 && n < cap - 1) { buf[n] = (char)u->key; buf[n + 1] = 0; changed = 1; }
    }
    return changed;
}
