#include "tak.h"
#include "gfx.h"
#include "font8x16.h"

int sw, sh, fbbpp;
img scr;

static u8 *fb;
static u32 pitch;
static int rp, rs, gp, gs, bp, bs;
static int fast;
static int cx = -100, cy = -100, cshown;

#define CSW 24
#define CSH 32
#define PAD 3
#define S 8
static u32 cs[CSW * CSH];
static int pv[7][2] = { {0, 0}, {0, 132}, {29, 104}, {51, 157}, {72, 147}, {50, 96}, {90, 96} };

static int in_poly(int px, int py) {
    int c = 0;
    for (int i = 0, j = 6; i < 7; j = i++) {
        int x1 = pv[i][0], y1 = pv[i][1], x2 = pv[j][0], y2 = pv[j][1];
        if ((y1 > py) != (y2 > py)) {
            i64 xi = (i64)(x2 - x1) * (py - y1) / (y2 - y1) + x1;
            if (px < xi) c = !c;
        }
    }
    return c;
}

static i64 seg_d2(int px, int py, int x1, int y1, int x2, int y2) {
    i64 dx = x2 - x1, dy = y2 - y1, den = dx * dx + dy * dy;
    i64 num = (i64)(px - x1) * dx + (i64)(py - y1) * dy;
    if (num <= 0) return (i64)(px - x1) * (px - x1) + (i64)(py - y1) * (py - y1);
    if (num >= den) return (i64)(px - x2) * (px - x2) + (i64)(py - y2) * (py - y2);
    i64 cr = (i64)(px - x1) * dy - (i64)(py - y1) * dx;
    return cr * cr / den;
}

static int in_edge(int px, int py, int r) {
    if (in_poly(px, py)) return 1;
    for (int i = 0, j = 6; i < 7; j = i++)
        if (seg_d2(px, py, pv[i][0], pv[i][1], pv[j][0], pv[j][1]) <= (i64)r * r) return 1;
    return 0;
}

static void cursor_build(void) {
    for (int y = 0; y < CSH; y++)
        for (int x = 0; x < CSW; x++) {
            int fill_n = 0, edge_n = 0, sh_n = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int px = (x - PAD) * S + sx * 2 + 1, py = (y - PAD) * S + sy * 2 + 1;
                    fill_n += in_poly(px - 4, py - 4);
                    edge_n += in_edge(px, py, 9);
                    sh_n += in_edge(px - 8, py - 14, 12);
                }
            int fa = fill_n * 255 / 16, ea = edge_n * 255 / 16, sa = sh_n * 70 / 16;
            int a1 = ea + sa * (255 - ea) / 255;
            int a2 = fa + a1 * (255 - fa) / 255;
            u32 al = (u32)a2, col = a2 ? (u32)(255 * fa / a2) * 0x010101u : 0;
            cs[y * CSW + x] = (al << 24) | col;
        }
}

u32 mix(u32 a, u32 b, int t) {
    u32 r = 0;
    for (int s = 0; s < 24; s += 8) {
        int x = (a >> s) & 255, y = (b >> s) & 255;
        r |= (u32)((x * (255 - t) + y * t) / 255) << s;
    }
    return r;
}

u8 gfont_row(int ch, int row) { return vga_font_8x16[ch & 255][row & 15]; }

static u32 conv(u32 c) {
    u32 r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    return ((r >> (8 - rs)) << rp) | ((g >> (8 - gs)) << gp) | ((b >> (8 - bs)) << bp);
}

static void fb_put(int x, int y, u32 c) {
    if (x < 0 || y < 0 || x >= sw || y >= sh) return;
    u8 *d = fb + (u64)y * pitch + (u64)x * (fbbpp / 8);
    u32 v = conv(c);
    if (fbbpp == 32) *(u32 *)d = v;
    else if (fbbpp == 24) { d[0] = v; d[1] = v >> 8; d[2] = v >> 16; }
    else *(u16 *)d = (u16)v;
}

int gfx_init(void) {
    u8 *t = mb_tag(8);
    if (!t) return 0;
    u64 addr = *(u64 *)(t + 8);
    pitch = *(u32 *)(t + 16);
    sw = *(u32 *)(t + 20);
    sh = *(u32 *)(t + 24);
    fbbpp = t[28];
    int type = t[29];
    if (type != 1 || addr >= (1ULL << 32) || (fbbpp != 32 && fbbpp != 24 && fbbpp != 16)) return 0;
    rp = t[32]; rs = t[33]; gp = t[34]; gs = t[35]; bp = t[36]; bs = t[37];
    fb = (u8 *)addr;
    fast = fbbpp == 32 && rp == 16 && gp == 8 && bp == 0 && rs == 8 && gs == 8 && bs == 8;
    u32 *px = kalloc((u64)sw * sh * 4);
    if (!px) return 0;
    img_init(&scr, px, sw, sh);
    fill(&scr, 0, 0, sw, sh, C_BG);
    cursor_build();
    klog("gfx: %dx%d bpp=%d pitch=%d fb=%lx fast=%d\n", sw, sh, fbbpp, (int)pitch, addr, fast);
    return 1;
}

int gfx_setmode(int w, int h) {
    outw(0x1CE, 0);
    u16 id = inw(0x1CF);
    if (id < 0xB0C0 || id > 0xB0CF || (u64)w * h * 4 > (8u << 20) * 2) return 0;
    u32 *np = kalloc((u64)w * h * 4);
    if (!np) return 0;
    outw(0x1CE, 4); outw(0x1CF, 0);
    outw(0x1CE, 1); outw(0x1CF, (u16)w);
    outw(0x1CE, 2); outw(0x1CF, (u16)h);
    outw(0x1CE, 3); outw(0x1CF, 32);
    outw(0x1CE, 4); outw(0x1CF, 0x41);
    outw(0x1CE, 1);
    int rx = inw(0x1CF);
    outw(0x1CE, 2);
    int ry = inw(0x1CF);
    if (rx != w || ry != h) { kfree(np); return 0; }
    kfree(scr.p);
    img_init(&scr, np, w, h);
    fill(&scr, 0, 0, w, h, C_BG);
    sw = w; sh = h; fbbpp = 32; pitch = (u32)w * 4;
    rp = 16; gp = 8; bp = 0; rs = gs = bs = 8;
    fast = 1;
    cshown = 0;
    cx = cy = -100;
    return 1;
}

void gfx_panic(const char *msg) {
    if (!fb) return;
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) fb_put(x, y, 0x5A0000);
    int x = 24, y = 24;
    const char *t = "TAK KERNEL PANIC";
    for (const char *s = t; *s; s++, x += 16)
        for (int r = 0; r < 16; r++)
            for (int c = 0; c < 8; c++)
                if (vga_font_8x16[(u8)*s][r] & (0x80 >> c)) {
                    for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) fb_put(x + c * 2 + a, y + r * 2 + b, 0xFFFFFF);
                }
    y = 80; x = 24;
    for (const char *s = msg; *s; s++) {
        if (*s == '\n') { y += 18; x = 24; continue; }
        for (int r = 0; r < 16; r++)
            for (int c = 0; c < 8; c++)
                if (vga_font_8x16[(u8)*s][r] & (0x80 >> c)) fb_put(x + c, y + r, 0xFFFFFF);
        x += 8;
    }
}

void img_init(img *c, u32 *p, int w, int h) {
    c->p = p; c->w = w; c->h = h;
    c->cx0 = 0; c->cy0 = 0; c->cx1 = w; c->cy1 = h;
}

img *img_new(int w, int h) {
    img *c = kalloc(sizeof *c);
    if (!c) return NULL;
    u32 *p = kzalloc((u64)w * h * 4 + 16);
    if (!p) { kfree(c); return NULL; }
    img_init(c, p, w, h);
    return c;
}

void img_free(img *c) {
    if (!c) return;
    kfree(c->p);
    kfree(c);
}

void clip_reset(img *c) { c->cx0 = 0; c->cy0 = 0; c->cx1 = c->w; c->cy1 = c->h; }

void clip_set(img *c, int x, int y, int w, int h) {
    int x1 = x + w, y1 = y + h;
    c->cx0 = x < 0 ? 0 : x;
    c->cy0 = y < 0 ? 0 : y;
    c->cx1 = x1 > c->w ? c->w : x1;
    c->cy1 = y1 > c->h ? c->h : y1;
}

static inline void fill32(u32 *d, u32 v, size_t n) {
    __asm__ volatile("rep stosl" : "+D"(d), "+c"(n) : "a"(v) : "memory");
}

void fill(img *c, int x, int y, int w, int h, u32 col) {
    int x1 = x + w, y1 = y + h;
    if (x < c->cx0) x = c->cx0;
    if (y < c->cy0) y = c->cy0;
    if (x1 > c->cx1) x1 = c->cx1;
    if (y1 > c->cy1) y1 = c->cy1;
    if (x1 <= x || y1 <= y) return;
    for (int j = y; j < y1; j++) fill32(c->p + (u64)j * c->w + x, col, x1 - x);
}

void fill_a(img *c, int x, int y, int w, int h, u32 col, int a) {
    int x1 = x + w, y1 = y + h;
    if (x < c->cx0) x = c->cx0;
    if (y < c->cy0) y = c->cy0;
    if (x1 > c->cx1) x1 = c->cx1;
    if (y1 > c->cy1) y1 = c->cy1;
    for (int j = y; j < y1; j++) {
        u32 *d = c->p + (u64)j * c->w;
        for (int i = x; i < x1; i++) d[i] = mix(d[i], col, a);
    }
}

void hline(img *c, int x, int y, int w, u32 col) { fill(c, x, y, w, 1, col); }
void vline(img *c, int x, int y, int h, u32 col) { fill(c, x, y, 1, h, col); }

void box(img *c, int x, int y, int w, int h, u32 col) {
    hline(c, x, y, w, col);
    hline(c, x, y + h - 1, w, col);
    vline(c, x, y, h, col);
    vline(c, x + w - 1, y, h, col);
}

void glyph(img *c, int x, int y, int ch, u32 fg, int bg, int sc) {
    int gw = 8 * sc, gh = 16 * sc;
    if (x >= c->cx1 || y >= c->cy1 || x + gw <= c->cx0 || y + gh <= c->cy0) return;
    const u8 *g = vga_font_8x16[ch & 255];
    if (sc == 1 && x >= c->cx0 && y >= c->cy0 && x + 8 <= c->cx1 && y + 16 <= c->cy1) {
        for (int r = 0; r < 16; r++) {
            u32 *d = c->p + (u64)(y + r) * c->w + x;
            u8 bits = g[r];
            for (int b = 0; b < 8; b++) {
                if (bits & (0x80 >> b)) d[b] = fg;
                else if (bg >= 0) d[b] = (u32)bg;
            }
        }
        return;
    }
    for (int r = 0; r < 16; r++) {
        int yy = y + r * sc;
        if (yy + sc <= c->cy0 || yy >= c->cy1) continue;
        u8 bits = g[r];
        if (bg >= 0) fill(c, x, yy, gw, sc, (u32)bg);
        if (!bits) continue;
        for (int b = 0; b < 8; b++)
            if (bits & (0x80 >> b)) fill(c, x + b * sc, yy, sc, sc, fg);
    }
}

void text(img *c, int x, int y, const char *s, u32 fg) {
    for (; *s; s++, x += 8) glyph(c, x, y, (u8)*s, fg, -1, 1);
}

void textb(img *c, int x, int y, const char *s, u32 fg, u32 bg) {
    for (; *s; s++, x += 8) glyph(c, x, y, (u8)*s, fg, (int)bg, 1);
}

void text2(img *c, int x, int y, const char *s, u32 fg) {
    for (; *s; s++, x += 16) glyph(c, x, y, (u8)*s, fg, -1, 2);
}

void textn(img *c, int x, int y, const char *s, int n, u32 fg) {
    for (int i = 0; i < n && s[i]; i++, x += 8) glyph(c, x, y, (u8)s[i], fg, -1, 1);
}

static int clipblit(img *d, int *dx, int *dy, img *s, int *sx, int *sy, int *w, int *h) {
    if (*sx < 0) { *dx -= *sx; *w += *sx; *sx = 0; }
    if (*sy < 0) { *dy -= *sy; *h += *sy; *sy = 0; }
    if (*sx + *w > s->w) *w = s->w - *sx;
    if (*sy + *h > s->h) *h = s->h - *sy;
    if (*dx < d->cx0) { int k = d->cx0 - *dx; *sx += k; *w -= k; *dx = d->cx0; }
    if (*dy < d->cy0) { int k = d->cy0 - *dy; *sy += k; *h -= k; *dy = d->cy0; }
    if (*dx + *w > d->cx1) *w = d->cx1 - *dx;
    if (*dy + *h > d->cy1) *h = d->cy1 - *dy;
    return *w > 0 && *h > 0;
}

void blit(img *d, int dx, int dy, img *s, int sx, int sy, int w, int h) {
    if (!clipblit(d, &dx, &dy, s, &sx, &sy, &w, &h)) return;
    for (int j = 0; j < h; j++)
        memcpy(d->p + (u64)(dy + j) * d->w + dx, s->p + (u64)(sy + j) * s->w + sx, (u64)w * 4);
}

void blit_a(img *d, int dx, int dy, img *s, int sx, int sy, int w, int h) {
    if (!clipblit(d, &dx, &dy, s, &sx, &sy, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        u32 *dp = d->p + (u64)(dy + j) * d->w + dx;
        u32 *sp = s->p + (u64)(sy + j) * s->w + sx;
        for (int i = 0; i < w; i++) {
            u32 p = sp[i];
            u32 a = p >> 24;
            if (a == 255) dp[i] = p & 0xFFFFFF;
            else if (a) dp[i] = mix(dp[i], p & 0xFFFFFF, (int)a);
        }
    }
}

img *img_scale(img *s, int dw, int dh) {
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    img *d = img_new(dw, dh);
    if (!d) return NULL;
    if (dw <= s->w && dh <= s->h) {
        for (int y = 0; y < dh; y++) {
            int y0 = (int)((u64)y * s->h / dh), y1 = (int)((u64)(y + 1) * s->h / dh);
            if (y1 <= y0) y1 = y0 + 1;
            for (int x = 0; x < dw; x++) {
                int x0 = (int)((u64)x * s->w / dw), x1 = (int)((u64)(x + 1) * s->w / dw);
                if (x1 <= x0) x1 = x0 + 1;
                u32 a = 0, r = 0, g = 0, b = 0, n = 0;
                for (int j = y0; j < y1; j++)
                    for (int i = x0; i < x1; i++) {
                        u32 p = s->p[(u64)j * s->w + i];
                        a += p >> 24; r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255; n++;
                    }
                d->p[(u64)y * dw + x] = ((a / n) << 24) | ((r / n) << 16) | ((g / n) << 8) | (b / n);
            }
        }
    } else {
        for (int y = 0; y < dh; y++) {
            u32 fy = dh > 1 ? (u32)((u64)y * (s->h - 1) * 256 / (dh - 1)) : 0;
            int y0 = fy >> 8, y1 = y0 + 1 < s->h ? y0 + 1 : y0, wy = fy & 255;
            for (int x = 0; x < dw; x++) {
                u32 fx = dw > 1 ? (u32)((u64)x * (s->w - 1) * 256 / (dw - 1)) : 0;
                int x0 = fx >> 8, x1 = x0 + 1 < s->w ? x0 + 1 : x0, wx = fx & 255;
                u32 p00 = s->p[(u64)y0 * s->w + x0], p10 = s->p[(u64)y0 * s->w + x1];
                u32 p01 = s->p[(u64)y1 * s->w + x0], p11 = s->p[(u64)y1 * s->w + x1];
                u32 out = 0;
                for (int sh2 = 0; sh2 < 32; sh2 += 8) {
                    u32 t0 = (((p00 >> sh2) & 255) * (256 - wx) + ((p10 >> sh2) & 255) * wx) >> 8;
                    u32 t1 = (((p01 >> sh2) & 255) * (256 - wx) + ((p11 >> sh2) & 255) * wx) >> 8;
                    out |= (((t0 * (256 - wy) + t1 * wy) >> 8) & 255) << sh2;
                }
                d->p[(u64)y * dw + x] = out;
            }
        }
    }
    return d;
}

void present(int x, int y, int w, int h) {
    if (!fb) return;
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > sw) x1 = sw;
    if (y1 > sh) y1 = sh;
    if (x1 <= x || y1 <= y) return;
    for (int j = y; j < y1; j++) {
        u32 *s = scr.p + (u64)j * sw + x;
        u8 *d = fb + (u64)j * pitch + (u64)x * (fbbpp / 8);
        if (fast) {
            size_t n = x1 - x;
            __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
        } else if (fbbpp == 32) {
            for (int i = x; i < x1; i++, s++, d += 4) *(u32 *)d = conv(*s);
        } else if (fbbpp == 24) {
            for (int i = x; i < x1; i++, s++, d += 3) { u32 v = conv(*s); d[0] = v; d[1] = v >> 8; d[2] = v >> 16; }
        } else {
            for (int i = x; i < x1; i++, s++, d += 2) *(u16 *)d = (u16)conv(*s);
        }
    }
}

static void draw_cur(int x, int y) {
    x -= PAD; y -= PAD;
    for (int j = 0; j < CSH; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= sh) continue;
        for (int i = 0; i < CSW; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= sw) continue;
            u32 p = cs[j * CSW + i], a = p >> 24;
            if (!a) continue;
            u32 bg = scr.p[(u64)yy * sw + xx] & 0xFFFFFF;
            fb_put(xx, yy, a == 255 ? (p & 0xFFFFFF) : mix(bg, p & 0xFFFFFF, (int)a));
        }
    }
}

void cursor_hide(void) {
    if (cshown) { present(cx - PAD, cy - PAD, CSW, CSH); cshown = 0; }
}

void cursor_redraw(void) {
    if (!fb) return;
    draw_cur(cx, cy);
    cshown = 1;
}

void cursor_move(int x, int y) {
    if (!fb) return;
    if (cshown && x == cx && y == cy) return;
    if (cshown) present(cx - PAD, cy - PAD, CSW, CSH);
    cx = x; cy = y;
    draw_cur(cx, cy);
    cshown = 1;
}

static int isq(int v) {
    if (v <= 0) return 0;
    int r = v, x = (v + 1) / 2;
    while (x < r) { r = x; x = (r + v / r) / 2; }
    return r;
}

int rr_cov(int px, int py, int x, int y, int w, int h, int r) {
    if (px < x || py < y || px >= x + w || py >= y + h) return 0;
    if (r <= 0) return 255;
    int dx = 0, dy = 0;
    if (px < x + r) dx = x + r - 1 - px;
    else if (px >= x + w - r) dx = px - (x + w - r);
    if (py < y + r) dy = y + r - 1 - py;
    else if (py >= y + h - r) dy = py - (y + h - r);
    if (dx <= 0 || dy <= 0) return 255;
    int d2 = (dx * 2 + 1) * (dx * 2 + 1) + (dy * 2 + 1) * (dy * 2 + 1);
    int out = 4 * r * r + 4 * r + 1, in = 4 * r * r - 4 * r + 1;
    if (d2 <= in) return 255;
    if (d2 >= out) return 0;
    return 255 * (out - d2) / (out - in);
}

void fill_rr(img *c, int x, int y, int w, int h, int r, u32 col, int a) {
    int x0 = x < c->cx0 ? c->cx0 : x, y0 = y < c->cy0 ? c->cy0 : y;
    int x1 = x + w > c->cx1 ? c->cx1 : x + w, y1 = y + h > c->cy1 ? c->cy1 : y + h;
    for (int j = y0; j < y1; j++) {
        u32 *d = c->p + (u64)j * c->w;
        int edge = j < y + r || j >= y + h - r;
        for (int i = x0; i < x1; i++) {
            int cv = edge && (i < x + r || i >= x + w - r) ? rr_cov(i, j, x, y, w, h, r) : 255;
            cv = cv * a / 255;
            if (cv >= 255) d[i] = col;
            else if (cv > 0) d[i] = mix(d[i], col, cv);
        }
    }
}

void vgrad(img *c, int x, int y, int w, int h, int r, u32 top, u32 bot) {
    int x0 = x < c->cx0 ? c->cx0 : x, y0 = y < c->cy0 ? c->cy0 : y;
    int x1 = x + w > c->cx1 ? c->cx1 : x + w, y1 = y + h > c->cy1 ? c->cy1 : y + h;
    for (int j = y0; j < y1; j++) {
        u32 col = mix(top, bot, h > 1 ? (j - y) * 255 / (h - 1) : 0);
        u32 *d = c->p + (u64)j * c->w;
        int edge = j < y + r || j >= y + h - r;
        for (int i = x0; i < x1; i++) {
            int cv = edge && (i < x + r || i >= x + w - r) ? rr_cov(i, j, x, y, w, h, r) : 255;
            if (cv >= 255) d[i] = col;
            else if (cv > 0) d[i] = mix(d[i], col, cv);
        }
    }
}

static void boxblur(u32 *dst, const u32 *src, int w, int h, int rad) {
    u32 *tmp = kalloc((u64)w * h * 4);
    if (!tmp) { memcpy(dst, src, (u64)w * h * 4); return; }
    int n = rad * 2 + 1;
    for (int y = 0; y < h; y++) {
        const u32 *s = src + (u64)y * w;
        u32 *t = tmp + (u64)y * w;
        u32 r = 0, g = 0, b = 0;
        for (int k = -rad; k <= rad; k++) {
            u32 p = s[k < 0 ? 0 : (k >= w ? w - 1 : k)];
            r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255;
        }
        for (int x = 0; x < w; x++) {
            t[x] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
            int a = x + rad + 1, o = x - rad;
            u32 pa = s[a >= w ? w - 1 : a], po = s[o < 0 ? 0 : o];
            r += ((pa >> 16) & 255) - ((po >> 16) & 255);
            g += ((pa >> 8) & 255) - ((po >> 8) & 255);
            b += (pa & 255) - (po & 255);
        }
    }
    for (int x = 0; x < w; x++) {
        u32 r = 0, g = 0, b = 0;
        for (int k = -rad; k <= rad; k++) {
            u32 p = tmp[(u64)(k < 0 ? 0 : (k >= h ? h - 1 : k)) * w + x];
            r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255;
        }
        for (int y = 0; y < h; y++) {
            dst[(u64)y * w + x] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
            int a = y + rad + 1, o = y - rad;
            u32 pa = tmp[(u64)(a >= h ? h - 1 : a) * w + x], po = tmp[(u64)(o < 0 ? 0 : o) * w + x];
            r += ((pa >> 16) & 255) - ((po >> 16) & 255);
            g += ((pa >> 8) & 255) - ((po >> 8) & 255);
            b += (pa & 255) - (po & 255);
        }
    }
    kfree(tmp);
}

void glass(img *c, int rx, int ry, int rw, int rh, int sx, int sy, int sw2, int sh2, int sr, u32 tint, int ta, int bl, int gloss) {
    int x0 = rx < c->cx0 ? c->cx0 : rx, y0 = ry < c->cy0 ? c->cy0 : ry;
    int x1 = rx + rw > c->cx1 ? c->cx1 : rx + rw, y1 = ry + rh > c->cy1 ? c->cy1 : ry + rh;
    if (x1 <= x0 || y1 <= y0) return;
    int ex0 = x0 - bl < 0 ? 0 : x0 - bl, ey0 = y0 - bl < 0 ? 0 : y0 - bl;
    int ex1 = x1 + bl > c->w ? c->w : x1 + bl, ey1 = y1 + bl > c->h ? c->h : y1 + bl;
    int bw = ex1 - ex0, bh = ey1 - ey0;
    u32 *src = kalloc((u64)bw * bh * 4), *blr = kalloc((u64)bw * bh * 4);
    if (!src || !blr) { kfree(src); kfree(blr); return; }
    for (int j = 0; j < bh; j++) memcpy(src + (u64)j * bw, c->p + (u64)(ey0 + j) * c->w + ex0, (u64)bw * 4);
    boxblur(blr, src, bw, bh, bl);
    int gh = sh2 * 48 / 100;
    if (gh > 44) gh = 44;
    for (int j = y0; j < y1; j++) {
        u32 *d = c->p + (u64)j * c->w;
        const u32 *b = blr + (u64)(j - ey0) * bw;
        const u32 *o = src + (u64)(j - ey0) * bw;
        int edge = j < sy + sr || j >= sy + sh2 - sr;
        int ga = 0;
        if (gloss && j - sy < gh && j >= sy) ga = gloss * (gh - (j - sy)) / (gh ? gh : 1);
        for (int i = x0; i < x1; i++) {
            int cv = edge && (i < sx + sr || i >= sx + sw2 - sr) ? rr_cov(i, j, sx, sy, sw2, sh2, sr) : 255;
            if (i < sx || i >= sx + sw2 || j < sy || j >= sy + sh2) cv = 0;
            if (!cv) continue;
            u32 px = mix(b[i - ex0], tint, ta);
            if (ga) px = mix(px, 0xFFFFFF, ga);
            d[i] = cv >= 255 ? px : mix(o[i - ex0], px, cv);
        }
    }
    kfree(src);
    kfree(blr);
}

void glass_edge(img *c, int sx, int sy, int sw2, int sh2, int sr, int hi, int lo) {
    int bands[4][4] = { { sx, sy, sw2, 3 }, { sx, sy + sh2 - 3, sw2, 3 }, { sx, sy + 3, 3, sh2 - 6 }, { sx + sw2 - 3, sy + 3, 3, sh2 - 6 } };
    for (int k = 0; k < 4; k++) {
        int bx = bands[k][0], by = bands[k][1], bw = bands[k][2], bh = bands[k][3];
        for (int j = by; j < by + bh; j++) {
            if (j < c->cy0 || j >= c->cy1) continue;
            for (int i = bx; i < bx + bw; i++) {
                if (i < c->cx0 || i >= c->cx1) continue;
                int o = rr_cov(i, j, sx, sy, sw2, sh2, sr);
                int in = rr_cov(i, j, sx + 1, sy + 1, sw2 - 2, sh2 - 2, sr > 1 ? sr - 1 : 0);
                int ring = o - in;
                if (ring <= 0) continue;
                int t = ((i - sx) * 90 / (sw2 ? sw2 : 1)) + ((j - sy) * 165 / (sh2 ? sh2 : 1));
                int al = (j - sy < sh2 / 2 ? hi : lo) * (t < 128 ? 255 - t : t - 128 + 100) / 255;
                if (al > 255) al = 255;
                u32 *d = &c->p[(u64)j * c->w + i];
                *d = mix(*d, 0xFFFFFF, ring * al / 255);
            }
        }
    }
}

void soft_shadow(img *c, int x, int y, int w, int h, int r, int spread, int amax) {
    int x0 = x - spread, y0 = y - spread, x1 = x + w + spread, y1 = y + h + spread;
    for (int j = y0; j < y1; j++) {
        if (j < c->cy0 || j >= c->cy1) continue;
        int inband = j < y + r || j >= y + h - r;
        int left = j < y || j >= y + h;
        for (int i = x0; i < x1; i++) {
            if (i < c->cx0 || i >= c->cx1) continue;
            if (!left && i >= x + r && i < x + w - r && j >= y && j < y + h) { i = x + w - r - 1; continue; }
            if (!inband && !(i < x + r || i >= x + w - r) && j >= y && j < y + h) continue;
            int dx = i < x ? x - i : (i >= x + w ? i - (x + w - 1) : 0);
            int dy = j < y ? y - j : (j >= y + h ? j - (y + h - 1) : 0);
            int d;
            if (dx && dy) d = isq(dx * dx + dy * dy);
            else d = dx + dy;
            if (!dx && !dy) {
                if (rr_cov(i, j, x, y, w, h, r) >= 255) continue;
                d = 0;
            }
            if (d >= spread) continue;
            int t = 255 - d * 255 / spread;
            int a = amax * t / 255 * t / 255;
            if (a <= 0) continue;
            u32 *p = &c->p[(u64)j * c->w + i];
            *p = mix(*p, 0, a);
        }
    }
}
