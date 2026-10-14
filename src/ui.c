#include "tak.h"
#include "wm.h"

void ui_btn(img *c, int x, int y, int w, int h, const char *s, int st) {
    u32 bg = st == 2 ? C_DRED : st == 1 ? 0x4B5563 : 0x374151;
    fill(c, x, y, w, h, bg);
    box(c, x, y, w, h, st == 2 ? C_RED : 0x6B7280);
    int tw = (int)strlen(s) * 8;
    text(c, x + (w - tw) / 2, y + (h - 16) / 2, s, C_WHITE);
}

void ui_field(img *c, int x, int y, int w, int h, const char *s, int foc) {
    fill(c, x, y, w, h, 0x0B1020);
    box(c, x, y, w, h, foc ? C_RED : 0x4B5563);
    int maxc = (w - 10) / 8;
    int n = (int)strlen(s);
    const char *p = n > maxc ? s + n - maxc : s;
    text(c, x + 5, y + (h - 16) / 2, p, C_TEXT);
}

void ui_vscroll(img *c, int x, int y, int h, int pos, int total, int vis) {
    fill(c, x, y, 10, h, 0x111827);
    if (total <= vis || total <= 0) return;
    int th = h * vis / total;
    if (th < 16) th = 16;
    int ty = y + (h - th) * pos / (total - vis);
    fill(c, x + 1, ty, 8, th, 0x6B7280);
}
