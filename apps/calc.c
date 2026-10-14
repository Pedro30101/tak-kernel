#include "tw32.h"

static const char *keys[] = {
    "C", "<", "%", "/",
    "7", "8", "9", "*",
    "4", "5", "6", "-",
    "1", "2", "3", "+",
    "0", "00", "=", "="
};

static long acc, cur;
static int op, fresh = 1, err;
static char shown[32] = "0";

static void show(long v) { snprintf(shown, sizeof shown, "%ld", v); }

static void apply(void) {
    if (!op) { acc = cur; return; }
    if (op == '+') acc += cur;
    else if (op == '-') acc -= cur;
    else if (op == '*') acc *= cur;
    else if (op == '/') { if (cur) acc /= cur; else err = 1; }
    else if (op == '%') { if (cur) acc %= cur; else err = 1; }
}

static void press(const char *k) {
    if (err && k[0] != 'C') return;
    if (k[0] >= '0' && k[0] <= '9') {
        for (const char *p = k; *p; p++) {
            if (fresh) { cur = 0; fresh = 0; }
            if (cur < 100000000000L) cur = cur * 10 + (*p - '0');
        }
        show(cur);
    } else if (k[0] == 'C') { acc = cur = 0; op = 0; fresh = 1; err = 0; show(0); }
    else if (k[0] == '<') { cur /= 10; show(cur); }
    else if (k[0] == '=') { apply(); op = 0; cur = acc; fresh = 1; if (err) snprintf(shown, sizeof shown, "error"); else show(acc); }
    else { apply(); op = k[0]; cur = acc; fresh = 1; if (err) snprintf(shown, sizeof shown, "error"); else show(acc); }
}

int main(const char *arg) {
    (void)arg;
    tw_win *w = tw_open("Calculator", 280, 400);
    if (!w) return 1;
    tw_ui ui = {0};
    for (;;) {
        tw_ev e;
        if (tw_wait(w, &e) < 0) break;
        int quit = 0;
        do {
            if (e.type == TW_CLOSE) quit = 1;
            tw_ui_feed(&ui, &e);
            if (e.type == TW_KEY && e.b) {
                int k = e.a;
                char s[2] = { (char)k, 0 };
                if ((k >= '0' && k <= '9') || k == '+' || k == '-' || k == '*' || k == '/' || k == '%') press(s);
                else if (k == '\n' || k == '=') press("=");
                else if (k == '\b') press("<");
                else if (k == 'c' || k == 'C' || k == 27) press("C");
            }
        } while (tw_poll(w, &e) > 0);
        if (quit) break;
        tw_clear(w, 0x202938);
        tw_fill(w, 10, 10, 260, 56, 0x0B1020);
        tw_rect(w, 10, 10, 260, 56, 0xB91C1C);
        tw_text2(w, 262 - tw_textw(shown) * 2, 24, shown, TW_WHITE);
        for (int i = 0; i < 20; i++) {
            if (i == 19) continue;
            int cx = 10 + (i % 4) * 66, cy = 80 + (i / 4) * 62;
            int wide = i == 18 ? 2 : 1;
            if (i == 18) { if (tw_button(w, &ui, cx, cy, 126, 56, "=")) press("="); continue; }
            if (tw_button(w, &ui, cx, cy, 60 * wide, 56, keys[i])) press(keys[i]);
        }
        tw_ui_end(&ui);
        tw_flip(w);
    }
    tw_close(w);
    return 0;
}
