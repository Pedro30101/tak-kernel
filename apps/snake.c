#include "tw32.h"

#define GW 24
#define GH 18
#define CS 24

static int sx[GW * GH], sy[GW * GH], len, dx, dy, fx, fy, score, dead;

static void food(void) {
    for (;;) {
        fx = rand() % GW; fy = rand() % GH;
        int ok = 1;
        for (int i = 0; i < len; i++) if (sx[i] == fx && sy[i] == fy) ok = 0;
        if (ok) return;
    }
}

static void reset(void) {
    len = 3; dx = 1; dy = 0; score = 0; dead = 0;
    for (int i = 0; i < len; i++) { sx[i] = 8 - i; sy[i] = 9; }
    food();
}

int main(const char *arg) {
    (void)arg;
    tw_win *w = tw_open("Snake", GW * CS, GH * CS + 32);
    if (!w) return 1;
    srand((unsigned)uptime());
    reset();
    int ndx = dx, ndy = dy;
    for (;;) {
        tw_ev e;
        int quit = 0;
        while (tw_poll(w, &e) > 0) {
            if (e.type == TW_CLOSE) quit = 1;
            if (e.type == TW_KEY && e.b) {
                if (e.a == TK_UP && dy == 0) { ndx = 0; ndy = -1; }
                else if (e.a == TK_DOWN && dy == 0) { ndx = 0; ndy = 1; }
                else if (e.a == TK_LEFT && dx == 0) { ndx = -1; ndy = 0; }
                else if (e.a == TK_RIGHT && dx == 0) { ndx = 1; ndy = 0; }
                else if (dead && (e.a == '\n' || e.a == ' ')) { reset(); ndx = dx; ndy = dy; }
            }
        }
        if (quit) break;
        if (!dead) {
            dx = ndx; dy = ndy;
            int nx = sx[0] + dx, ny = sy[0] + dy;
            if (nx < 0 || ny < 0 || nx >= GW || ny >= GH) dead = 1;
            for (int i = 0; i < len && !dead; i++) if (sx[i] == nx && sy[i] == ny) dead = 1;
            if (!dead) {
                int grow = nx == fx && ny == fy;
                if (grow) len++;
                for (int i = len - 1; i > 0; i--) { sx[i] = sx[i - 1]; sy[i] = sy[i - 1]; }
                sx[0] = nx; sy[0] = ny;
                if (grow) { score++; food(); }
            }
        }
        tw_clear(w, 0x0B1020);
        for (int y = 0; y < GH; y++)
            for (int x = 0; x < GW; x++)
                if ((x + y) & 1) tw_fill(w, x * CS, y * CS, CS, CS, 0x0F172A);
        tw_fill(w, fx * CS + 3, fy * CS + 3, CS - 6, CS - 6, TW_RED);
        for (int i = 0; i < len; i++) tw_fill(w, sx[i] * CS + 1, sy[i] * CS + 1, CS - 2, CS - 2, i ? 0x16A34A : TW_GREEN);
        char b[48];
        snprintf(b, sizeof b, "score %d%s", score, dead ? "   game over - press Enter" : "");
        tw_fill(w, 0, GH * CS, w->w, 32, 0x202938);
        tw_text(w, 8, GH * CS + 8, b, TW_TEXT);
        tw_flip(w);
        sleep_ms(dead ? 80 : 110);
    }
    tw_close(w);
    return 0;
}
