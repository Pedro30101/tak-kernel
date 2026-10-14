#ifndef TW32_H
#define TW32_H
#include "te.h"

/*
 * tw32 - window library for TAK .te apps.
 * Draw into the window canvas, call tw_flip(), read events with tw_poll()/tw_wait().
 */

enum { TW_KEY = 1, TW_MOUSE = 2, TW_CLOSE = 3, TW_RESIZE = 4 };
enum { TW_BTN_L = 1, TW_BTN_R = 2, TW_BTN_M = 4 };
enum { TW_MOD_CTRL = 1, TW_MOD_ALT = 2, TW_MOD_SHIFT = 4 };
enum {
    TK_UP = 0x101, TK_DOWN, TK_LEFT, TK_RIGHT, TK_HOME, TK_END, TK_PGUP, TK_PGDN, TK_DEL, TK_INS,
    TK_F1 = 0x111, TK_F2, TK_F3, TK_F4, TK_F5, TK_F6, TK_F7, TK_F8, TK_F9, TK_F10, TK_F11, TK_F12
};

#define TW_RGB(r, g, b) ((u32)(((r) << 16) | ((g) << 8) | (b)))
#define TW_WHITE 0xFFFFFF
#define TW_BLACK 0x000000
#define TW_RED 0xEF4444
#define TW_GREEN 0x4ADE80
#define TW_BLUE 0x3B82F6
#define TW_YELLOW 0xFACC15
#define TW_PANEL 0x202938
#define TW_DIM 0x9CA3AF
#define TW_TEXT 0xE5E7EB

typedef struct {
    int id, w, h;
    u32 *px;
} tw_win;

typedef struct {
    int type;
    int a, b, c, d;
} tw_ev;

typedef struct {
    int mx, my, down, click, released, wheel;
    int key, mod;
    int focus;
} tw_ui;

tw_win *tw_open(const char *title, int w, int h);
void tw_close(tw_win *w);
void tw_title(tw_win *w, const char *t);
void tw_flip(tw_win *w);
int tw_poll(tw_win *w, tw_ev *e);
int tw_wait(tw_win *w, tw_ev *e);
void tw_resize(tw_win *w, int nw, int nh);

typedef int (*tw_handler)(tw_win *w, tw_ev *e, void *user);
void tw_run(tw_win *w, tw_handler h, void *user);

void tw_clear(tw_win *w, u32 col);
void tw_pixel(tw_win *w, int x, int y, u32 col);
u32 tw_get(tw_win *w, int x, int y);
void tw_fill(tw_win *w, int x, int y, int bw, int bh, u32 col);
void tw_rect(tw_win *w, int x, int y, int bw, int bh, u32 col);
void tw_line(tw_win *w, int x0, int y0, int x1, int y1, u32 col);
void tw_disc(tw_win *w, int cx, int cy, int r, u32 col);
void tw_circle(tw_win *w, int cx, int cy, int r, u32 col);
void tw_text(tw_win *w, int x, int y, const char *s, u32 col);
void tw_text2(tw_win *w, int x, int y, const char *s, u32 col);
int tw_textw(const char *s);
void tw_blit(tw_win *w, int x, int y, const u32 *src, int sw, int sh);
u32 tw_mix(u32 a, u32 b, int t);

void tw_ui_feed(tw_ui *u, const tw_ev *e);
void tw_ui_end(tw_ui *u);
int tw_hit(tw_ui *u, int x, int y, int w, int h);
int tw_button(tw_win *w, tw_ui *u, int x, int y, int bw, int bh, const char *label);
void tw_label(tw_win *w, int x, int y, const char *s, u32 col);
int tw_checkbox(tw_win *w, tw_ui *u, int x, int y, const char *label, int *val);
int tw_slider(tw_win *w, tw_ui *u, int x, int y, int sw, int *val, int min, int max);
void tw_progress(tw_win *w, int x, int y, int pw, int ph, int val, int max, u32 col);
int tw_input(tw_win *w, tw_ui *u, int x, int y, int iw, char *buf, int cap, int id);

#endif
