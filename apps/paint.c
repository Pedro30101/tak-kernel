#include "tw32.h"

static const u32 cols[] = {
    0x000000, 0xFFFFFF, 0xEF4444, 0xF97316, 0xFACC15, 0x4ADE80,
    0x14B8A6, 0x3B82F6, 0x8B5CF6, 0xEC4899, 0x92400E, 0x6B7280
};

static void save_bmp(tw_win *w, int top) {
    int bw = w->w, bh = w->h - top;
    int row = (bw * 3 + 3) & ~3;
    int size = 54 + row * bh;
    u8 *b = calloc(size, 1);
    if (!b) return;
    b[0] = 'B'; b[1] = 'M';
    *(u32 *)(b + 2) = size;
    *(u32 *)(b + 10) = 54;
    *(u32 *)(b + 14) = 40;
    *(i32 *)(b + 18) = bw;
    *(i32 *)(b + 22) = bh;
    *(u16 *)(b + 26) = 1;
    *(u16 *)(b + 28) = 24;
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
            u32 p = tw_get(w, x, top + bh - 1 - y);
            u8 *d = b + 54 + y * row + x * 3;
            d[0] = p; d[1] = p >> 8; d[2] = p >> 16;
        }
    file_write("/home/paint.bmp", b, size);
    free(b);
}

static void draw_ui(tw_win *w, tw_ui *ui, int *color, int *size, char *msg, int top) {
    tw_fill(w, 0, 0, w->w, top, 0x202938);
    for (int i = 0; i < 12; i++) {
        int x = 8 + i * 30;
        tw_fill(w, x, 8, 26, 26, cols[i]);
        tw_rect(w, x - 1, 7, 28, 28, i == *color ? 0xEF4444 : 0x4B5563);
        if (ui->click && tw_hit(ui, x, 8, 26, 26)) *color = i;
    }
    tw_label(w, 380, 4, "brush", TW_DIM);
    tw_slider(w, ui, 380, 22, 90, size, 1, 20);
    if (tw_button(w, ui, 484, 10, 60, 26, "Clear"))
        tw_fill(w, 0, top, w->w, w->h - top, TW_WHITE);
    if (tw_button(w, ui, 550, 10, 80, 26, "Save")) {
        save_bmp(w, top);
        snprintf(msg, 48, "saved /home/paint.bmp");
    }
    if (msg[0]) tw_text(w, 8, top - 16, msg, TW_GREEN);
}

int main(const char *arg) {
    (void)arg;
    const int top = 48;
    tw_win *w = tw_open("Paint", 640, 480);
    if (!w) return 1;
    tw_ui ui = {0};
    int color = 0, size = 4, lx = -1, ly = -1;
    char msg[48] = "";
    tw_clear(w, TW_WHITE);
    draw_ui(w, &ui, &color, &size, msg, top);
    tw_flip(w);
    for (;;) {
        tw_ev e;
        if (tw_wait(w, &e) < 0) break;
        int quit = 0;
        do {
            if (e.type == TW_CLOSE) quit = 1;
            tw_ui_feed(&ui, &e);
            if (e.type == TW_MOUSE) {
                int down = e.c & 1;
                if (down && e.b >= top) {
                    if (lx >= 0) {
                        int dx = e.a - lx, dy = e.b - ly, n = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
                        for (int i = 0; i <= n; i++) {
                            int x = n ? lx + dx * i / n : e.a, y = n ? ly + dy * i / n : e.b;
                            tw_disc(w, x, y, size, cols[color]);
                        }
                    } else tw_disc(w, e.a, e.b, size, cols[color]);
                    lx = e.a; ly = e.b;
                } else { lx = -1; }
            }
        } while (tw_poll(w, &e) > 0);
        if (quit) break;
        draw_ui(w, &ui, &color, &size, msg, top);
        tw_ui_end(&ui);
        tw_flip(w);
    }
    tw_close(w);
    return 0;
}
