#ifndef GFX_H
#define GFX_H
#include "tak.h"

typedef struct {
    u32 *p;
    int w, h;
    int cx0, cy0, cx1, cy1;
} img;

#define C_BG 0x111827
#define C_PANEL 0x202938
#define C_PANEL2 0x2B3548
#define C_RED 0xB91C1C
#define C_DRED 0x7F1D1D
#define C_LRED 0xEF4444
#define C_TEXT 0xE5E7EB
#define C_DIM 0x9CA3AF
#define C_WHITE 0xFFFFFF
#define C_BLACK 0x000000
#define C_GREEN 0x4ADE80
#define C_CYAN 0x22D3EE
#define C_YELLOW 0xFACC15
#define C_BLUE 0x3B82F6

extern int sw, sh, fbbpp;
extern img scr;

int gfx_init(void);
int gfx_setmode(int w, int h);
void img_init(img *c, u32 *p, int w, int h);
img *img_new(int w, int h);
void img_free(img *c);
void clip_set(img *c, int x, int y, int w, int h);
void clip_reset(img *c);
void fill(img *c, int x, int y, int w, int h, u32 col);
void fill_a(img *c, int x, int y, int w, int h, u32 col, int a);
void box(img *c, int x, int y, int w, int h, u32 col);
void hline(img *c, int x, int y, int w, u32 col);
void vline(img *c, int x, int y, int h, u32 col);
void glyph(img *c, int x, int y, int ch, u32 fg, int bg, int sc);
void text(img *c, int x, int y, const char *s, u32 fg);
void textb(img *c, int x, int y, const char *s, u32 fg, u32 bg);
void text2(img *c, int x, int y, const char *s, u32 fg);
void textn(img *c, int x, int y, const char *s, int n, u32 fg);
void blit(img *d, int dx, int dy, img *s, int sx, int sy, int w, int h);
void blit_a(img *d, int dx, int dy, img *s, int sx, int sy, int w, int h);
img *img_scale(img *s, int dw, int dh);
void present(int x, int y, int w, int h);
void cursor_move(int x, int y);
void cursor_hide(void);
void cursor_redraw(void);
u32 mix(u32 a, u32 b, int t);
int rr_cov(int px, int py, int x, int y, int w, int h, int r);
void fill_rr(img *c, int x, int y, int w, int h, int r, u32 col, int a);
void vgrad(img *c, int x, int y, int w, int h, int r, u32 top, u32 bot);
void glass(img *c, int rx, int ry, int rw, int rh, int sx, int sy, int sw, int sh, int sr, u32 tint, int ta, int bl, int gloss);
void glass_edge(img *c, int sx, int sy, int sw, int sh, int sr, int hi, int lo);
void soft_shadow(img *c, int x, int y, int w, int h, int r, int spread, int amax);
u8 gfont_row(int ch, int row);

#endif
