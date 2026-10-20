#include "tak.h"
#include "wm.h"

void ui_btn(img *c, int x, int y, int w, int h, const char *s, int st) {
    u32 top = st == 2 ? 0xE34B56 : st == 1 ? 0x526782 : 0x3B5273;
    u32 bot = st == 2 ? 0xB91C1C : st == 1 ? 0x354A68 : 0x273B59;
    int radius = h / 2;
    if (radius > 12) radius = 12;
    vgrad(c, x, y, w, h, radius, top, bot);
    fill_rr(c, x + 1, y + 1, w - 2, h / 2, radius - 1, 0xFFFFFF, st == 1 ? 24 : 34);
    glass_edge(c, x, y, w, h, radius, 140, 45);
    int tw = (int)strlen(s) * 8;
    text(c, x + (w - tw) / 2, y + (h - 16) / 2, s, C_WHITE);
}

void ui_field(img *c, int x, int y, int w, int h, const char *s, int foc) {
    int radius = h / 3;
    if (radius > 12) radius = 12;
    fill_rr(c, x, y, w, h, radius, foc ? 0x18253A : 0x111A29, 255);
    fill_rr(c, x + 1, y + 1, w - 2, h / 2, radius - 1, 0xFFFFFF, 12);
    glass_edge(c, x, y, w, h, radius, foc ? 150 : 80, foc ? 70 : 35);
    if (foc)
        fill_rr(c, x + 8, y + h - 3, w - 16, 2, 1, C_LRED, 230);
    int maxc = (w - 10) / 8;
    int n = (int)strlen(s);
    const char *p = n > maxc ? s + n - maxc : s;
    text(c, x + 5, y + (h - 16) / 2, p, C_TEXT);
}

void ui_vscroll(img *c, int x, int y, int h, int pos, int total, int vis) {
    fill_rr(c, x + 3, y, 4, h, 2, 0x111827, 90);
    if (total <= vis || total <= 0) return;
    int th = h * vis / total;
    if (th < 22) th = 22;
    int ty = y + (h - th) * pos / (total - vis);
    fill_rr(c, x + 1, ty, 8, th, 4, 0x93A4BE, 210);
}
