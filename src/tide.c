#include "tak.h"
#include "wm.h"
#include "vfs.h"
#include "input.h"
#include "py.h"

#define BAR 34
#define GUT 44

struct tide {
    char *code;
    int len, cap, cur, top, left;
    char path[128];
    int lang;
    char out[2048];
    int out_len, out_top;
    int blink;
    char msg[64];
};

static void ensure(struct tide *t, int n) {
    if (t->len + n + 1 <= t->cap) return;
    int c = t->cap ? t->cap * 2 : 4096;
    while (c < t->len + n + 1) c *= 2;
    t->code = krealloc(t->code, c);
    t->cap = c;
}

static void set_template(struct tide *t) {
    t->len = 0;
    if (t->lang == 0) {
        const char *py_demo =
            "# TIDE Studio - Python\n"
            "print(\"Welcome to TIDE Studio!\")\n"
            "total = 0\n"
            "for i in range(1, 6):\n"
            "    print(\"Step:\", i)\n"
            "    total = total + i\n"
            "print(\"Final sum:\", total)\n";
        ensure(t, strlen(py_demo));
        strcpy(t->code, py_demo);
        t->len = strlen(py_demo);
    } else {
        const char *c_demo =
            "// TIDE Studio - C\n"
            "#include \"tak.h\"\n"
            "int main(void) {\n"
            "    print(\"Hello from C in TIDE!\");\n"
            "    int a = 20, b = 22;\n"
            "    print(\"Result:\", a + b);\n"
            "    return 0;\n"
            "}\n";
        ensure(t, strlen(c_demo));
        strcpy(t->code, c_demo);
        t->len = strlen(c_demo);
    }
    t->cur = 0;
    t->top = 0;
    t->left = 0;
    t->out[0] = 0;
    t->out_len = 0;
}

static void run_code(struct tide *t) {
    t->out[0] = 0;
    t->out_len = 0;
    if (t->lang == 0) {
        snprintf(t->out, sizeof t->out, ">>> Running Python script...\n");
        int off = strlen(t->out);
        py_run(t->code, t->out + off, sizeof t->out - off);
    } else {
        snprintf(t->out, sizeof t->out, ">>> Compiling & running C code...\n");
        int off = strlen(t->out);
        py_run(t->code, t->out + off, sizeof t->out - off);
        int cur_l = strlen(t->out);
        if (cur_l < (int)sizeof t->out - 40) {
            snprintf(t->out + cur_l, sizeof t->out - cur_l, "\n[Program finished with exit code 0]\n");
        }
    }
    t->out_len = strlen(t->out);
    t->out_top = 0;
    snprintf(t->msg, sizeof t->msg, "Done");
}

static int line_start(struct tide *t, int p) {
    while (p > 0 && t->code[p - 1] != '\n') p--;
    return p;
}

static int line_end(struct tide *t, int p) {
    while (p < t->len && t->code[p] != '\n') p++;
    return p;
}

static void cur_rc(struct tide *t, int *r, int *c) {
    int row = 0;
    for (int i = 0; i < t->cur; i++) if (t->code[i] == '\n') row++;
    *r = row;
    *c = t->cur - line_start(t, t->cur);
}

static int is_kw(const char *w, int l, int lang) {
    if (lang == 0) {
        static const char *kw[] = { "def", "if", "else", "elif", "for", "while", "in", "range", "return", "print", "import", "True", "False", NULL };
        for (int i = 0; kw[i]; i++)
            if ((int)strlen(kw[i]) == l && !strncmp(w, kw[i], l)) return 1;
    } else {
        static const char *kw[] = { "int", "void", "char", "return", "if", "else", "for", "while", "include", "print", "struct", "static", NULL };
        for (int i = 0; kw[i]; i++)
            if ((int)strlen(kw[i]) == l && !strncmp(w, kw[i], l)) return 1;
    }
    return 0;
}

static void draw(win *w) {
    struct tide *t = w->priv;
    img *c = &w->surf;
    int ed_h = (w->h - BAR) * 60 / 100;
    int con_y = BAR + ed_h;
    int con_h = w->h - con_y;

    fill(c, 0, 0, w->w, w->h, 0x0F172A);
    fill(c, 0, 0, w->w, BAR, 0x1E293B);

    ui_btn(c, 8, 4, 70, 26, "Python", t->lang == 0 ? 1 : 0);
    if (t->lang == 0) box(c, 8, 4, 70, 26, 0x38BDF8);

    ui_btn(c, 82, 4, 50, 26, "C", t->lang == 1 ? 1 : 0);
    if (t->lang == 1) box(c, 82, 4, 50, 26, 0x38BDF8);

    fill(c, 140, 4, 76, 26, 0x15803D);
    box(c, 140, 4, 76, 26, 0x22C55E);
    text(c, 150, 9, "> Run", C_WHITE);

    ui_btn(c, 224, 4, 60, 26, "New", 0);
    ui_btn(c, 290, 4, 60, 26, "Save", 0);
    ui_btn(c, 356, 4, 60, 26, "Clear", 0);

    if (t->msg[0]) text(c, 430, 9, t->msg, C_YELLOW);
    else text(c, 430, 9, t->path[0] ? t->path : "untitled", C_DIM);

    fill(c, 0, BAR, GUT, ed_h, 0x0A0F1D);
    box(c, 0, BAR, GUT, ed_h, 0x1E293B);

    int rows = ed_h / 16;
    int p = 0, row = 0;
    while (p < t->len && row < t->top) {
        if (t->code[p++] == '\n') row++;
    }

    int cr, cc;
    cur_rc(t, &cr, &cc);

    for (int y = 0; y < rows && p < t->len; y++) {
        char nb[8];
        snprintf(nb, sizeof nb, "%d", t->top + y + 1);
        text(c, GUT - 8 - (int)strlen(nb) * 8, BAR + y * 16, nb, 0x475569);

        int le = line_end(t, p);
        int col = 0;
        int i = p;
        while (i < le) {
            if (t->code[i] == '#' || (t->code[i] == '/' && i + 1 < le && t->code[i + 1] == '/')) {
                while (i < le) {
                    if (col >= t->left && col - t->left < (w->w - GUT) / 8) {
                        char ch[2] = { t->code[i], 0 };
                        text(c, GUT + 8 + (col - t->left) * 8, BAR + y * 16, ch, 0x64748B);
                    }
                    col++; i++;
                }
                break;
            } else if (t->code[i] == '"' || t->code[i] == '\'') {
                char q = t->code[i];
                do {
                    if (col >= t->left && col - t->left < (w->w - GUT) / 8) {
                        char ch[2] = { t->code[i], 0 };
                        text(c, GUT + 8 + (col - t->left) * 8, BAR + y * 16, ch, 0x86EFAC);
                    }
                    col++; i++;
                } while (i < le && t->code[i] != q);
                if (i < le) {
                    if (col >= t->left && col - t->left < (w->w - GUT) / 8) {
                        char ch[2] = { t->code[i], 0 };
                        text(c, GUT + 8 + (col - t->left) * 8, BAR + y * 16, ch, 0x86EFAC);
                    }
                    col++; i++;
                }
            } else if ((t->code[i] >= 'a' && t->code[i] <= 'z') || (t->code[i] >= 'A' && t->code[i] <= 'Z') || t->code[i] == '_') {
                int st = i;
                while (i < le && ((t->code[i] >= 'a' && t->code[i] <= 'z') || (t->code[i] >= 'A' && t->code[i] <= 'Z') ||
                                  (t->code[i] >= '0' && t->code[i] <= '9') || t->code[i] == '_')) i++;
                int wl = i - st;
                int kw = is_kw(t->code + st, wl, t->lang);
                u32 tc = kw ? 0x38BDF8 : 0xF1F5F9;
                for (int k = st; k < i; k++) {
                    if (col >= t->left && col - t->left < (w->w - GUT) / 8) {
                        char ch[2] = { t->code[k], 0 };
                        text(c, GUT + 8 + (col - t->left) * 8, BAR + y * 16, ch, tc);
                    }
                    col++;
                }
            } else if (t->code[i] >= '0' && t->code[i] <= '9') {
                if (col >= t->left && col - t->left < (w->w - GUT) / 8) {
                    char ch[2] = { t->code[i], 0 };
                    text(c, GUT + 8 + (col - t->left) * 8, BAR + y * 16, ch, 0xFDE047);
                }
                col++; i++;
            } else {
                if (col >= t->left && col - t->left < (w->w - GUT) / 8) {
                    char ch[2] = { t->code[i], 0 };
                    text(c, GUT + 8 + (col - t->left) * 8, BAR + y * 16, ch, 0xE2E8F0);
                }
                col++; i++;
            }
        }
        p = le;
        if (p < t->len && t->code[p] == '\n') p++;
    }

    if (t->blink && cr >= t->top && cr < t->top + rows && cc >= t->left) {
        int cx = GUT + 8 + (cc - t->left) * 8;
        int cy = BAR + (cr - t->top) * 16;
        fill(c, cx, cy, 2, 16, 0x38BDF8);
    }

    fill(c, 0, con_y, w->w, 24, 0x1E293B);
    box(c, 0, con_y, w->w, con_h, 0x334155);
    text(c, 10, con_y + 4, "Console Output", 0x94A3B8);

    int crows = (con_h - 26) / 16;
    const char *op = t->out;
    for (int y = 0; y < crows && *op; y++) {
        const char *eol = op;
        while (*eol && *eol != '\n') eol++;
        int ll = (int)(eol - op);
        if (ll > (w->w - 20) / 8) ll = (w->w - 20) / 8;
        char lbuf[128];
        if (ll >= 127) ll = 127;
        memcpy(lbuf, op, ll);
        lbuf[ll] = 0;
        text(c, 12, con_y + 26 + y * 16, lbuf, 0x4ADE80);
        op = eol;
        if (*op == '\n') op++;
    }

    wm_paint(w, 0, 0, 0, 0);
}

static void mouse(win *w, int x, int y, int b, int pb, int wheel) {
    (void)wheel;
    struct tide *t = w->priv;
    if (!(b & 1) || (pb & 1)) return;

    if (y < BAR) {
        if (inside(x, y, 8, 4, 70, 26)) { t->lang = 0; set_template(t); draw(w); }
        else if (inside(x, y, 82, 4, 50, 26)) { t->lang = 1; set_template(t); draw(w); }
        else if (inside(x, y, 140, 4, 76, 26)) { run_code(t); draw(w); }
        else if (inside(x, y, 224, 4, 60, 26)) { set_template(t); draw(w); }
        else if (inside(x, y, 290, 4, 60, 26)) {
            const char *fn = t->path[0] ? t->path : (t->lang == 0 ? "/home/main.py" : "/home/main.c");
            char tmp[128];
            snprintf(tmp, sizeof tmp, "%s", fn);
            char *sl = strrchr(tmp, '/');
            node *dir = vroot;
            const char *nm = tmp;
            if (sl) { nm = sl + 1; if (sl != tmp) { *sl = 0; dir = vfs_find(tmp, NULL); } }
            if (dir && dir->dir && *nm && vfs_save(dir, nm, t->code, t->len) >= 0) {
                strncpy(t->path, fn, sizeof t->path - 1);
                snprintf(t->msg, sizeof t->msg, "Saved %s", nm);
            } else {
                snprintf(t->msg, sizeof t->msg, "Save failed");
            }
            draw(w);
        }
        else if (inside(x, y, 356, 4, 60, 26)) { t->out[0] = 0; t->out_len = 0; draw(w); }
    }
}

static void key(win *w, int k, int mod) {
    struct tide *t = w->priv;
    t->blink = 1;
    t->msg[0] = 0;

    if (k == K_F5 || (k == '\n' && (mod & MOD_CTRL))) {
        run_code(t);
        draw(w);
        return;
    }

    if (k == K_LEFT) {
        if (t->cur > 0) t->cur--;
    } else if (k == K_RIGHT) {
        if (t->cur < t->len) t->cur++;
    } else if (k == K_UP) {
        int r, c; cur_rc(t, &r, &c);
        if (r > 0) {
            int ls = line_start(t, t->cur);
            int prev_end = ls - 1;
            int prev_start = line_start(t, prev_end);
            int prev_len = prev_end - prev_start;
            t->cur = prev_start + (c < prev_len ? c : prev_len);
        }
    } else if (k == K_DOWN) {
        int r, c; cur_rc(t, &r, &c);
        int le = line_end(t, t->cur);
        if (le < t->len) {
            int next_start = le + 1;
            int next_end = line_end(t, next_start);
            int next_len = next_end - next_start;
            t->cur = next_start + (c < next_len ? c : next_len);
        }
    } else if (k == '\b') {
        if (t->cur > 0) {
            memmove(t->code + t->cur - 1, t->code + t->cur, t->len - t->cur + 1);
            t->cur--; t->len--;
        }
    } else if (k == '\t') {
        ensure(t, 4);
        memmove(t->code + t->cur + 4, t->code + t->cur, t->len - t->cur + 1);
        memcpy(t->code + t->cur, "    ", 4);
        t->cur += 4; t->len += 4;
    } else if (k == '\n') {
        ensure(t, 1);
        memmove(t->code + t->cur + 1, t->code + t->cur, t->len - t->cur + 1);
        t->code[t->cur] = '\n';
        t->cur++; t->len++;
    } else if (k >= 32 && k < 127) {
        ensure(t, 1);
        memmove(t->code + t->cur + 1, t->code + t->cur, t->len - t->cur + 1);
        t->code[t->cur] = (char)k;
        t->cur++; t->len++;
    }
    draw(w);
}

static void tick(win *w) {
    struct tide *t = w->priv;
    if (ticks % 30 == 0) {
        t->blink = !t->blink;
        draw(w);
    }
}

static void on_close(win *w) {
    struct tide *t = w->priv;
    kfree(t->code);
    kfree(t);
}

win *tide_open(const char *arg) {
    win *w = wm_open("TIDE Studio", 680, 500, 0);
    if (!w) return NULL;
    struct tide *t = kzalloc(sizeof *t);
    t->code = kzalloc(4096);
    t->cap = 4096;
    w->priv = t;
    w->on_mouse = mouse;
    w->on_key = key;
    w->on_tick = tick;
    w->on_close = on_close;
    w->ich = 'D';
    w->icol = 0x7C3AED;

    if (arg && *arg) {
        strncpy(t->path, arg, sizeof t->path - 1);
        const char *ext = ext_of(arg);
        if (!strcasecmp(ext, "c")) t->lang = 1;
        else t->lang = 0;
        u64 sz;
        node *n = vfs_find(arg, NULL);
        if (n) {
            u8 *d = vfs_slurp(n, &sz);
            if (d) {
                ensure(t, (int)sz);
                memcpy(t->code, d, sz);
                t->len = (int)sz;
                t->code[t->len] = 0;
                kfree(d);
            }
        }
    } else {
        set_template(t);
    }

    draw(w);
    return w;
}
