#include "tak.h"
#include "wm.h"
#include "vfs.h"
#include "proc.h"
#include "term.h"
#include "pci.h"
#include "net.h"
#include "netstack.h"
#include "input.h"
#include "disk.h"

#define MAXC 240
#define NL 1000
#define CW 9
#define CHH 16
#define MAXR 128

static const u32 pal[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

struct term {
    win *w;
    u16 (*lines)[MAXC];
    int cols, rows;
    long base, maxabs;
    int cx, cy, off, pcy;
    u8 color;
    u8 dirty[MAXR];
    int alld, curon;
    char line[256];
    int len, pos;
    long iabs;
    int ix;
    char hist[32][256];
    int nh, hi;
    node *cwd;
    int job;
    u64 jstate, jn;
    int fg;
    char *rbuf;
    int rlen, rcap;
};

static struct term *T;
static int first_term = 1;

static u16 *row_of(struct term *t, long a) { return t->lines[a % NL]; }

static void ensure(struct term *t, long a) {
    while (t->maxabs < a) {
        t->maxabs++;
        u16 *l = row_of(t, t->maxabs);
        for (int i = 0; i < MAXC; i++) l[i] = 0x0720;
    }
}

static void mark(struct term *t, int r) { if (r >= 0 && r < MAXR) t->dirty[r] = 1; }

static void need_row(struct term *t) {
    if (t->cy >= t->rows) {
        t->base += t->cy - t->rows + 1;
        t->cy = t->rows - 1;
        t->alld = 1;
    }
    ensure(t, t->base + t->cy);
}

static void rawc(struct term *t, int ch) {
    if (t->rbuf) {
        if (t->rlen + 2 > t->rcap) {
            t->rcap = t->rcap ? t->rcap * 2 : 1024;
            t->rbuf = krealloc(t->rbuf, t->rcap);
            if (!t->rbuf) { t->rlen = 0; t->rcap = 0; return; }
        }
        t->rbuf[t->rlen++] = (char)ch;
        return;
    }
    if (t->off) { t->off = 0; t->alld = 1; }
    if (ch == '\n') { t->cx = 0; t->cy++; need_row(t); return; }
    if (ch == '\r') { t->cx = 0; return; }
    if (ch == '\b') {
        if (t->cx > 0) {
            t->cx--;
            need_row(t);
            row_of(t, t->base + t->cy)[t->cx] = ((u16)t->color << 8) | ' ';
            mark(t, t->cy);
        }
        return;
    }
    if (ch == '\t') {
        int n = 8 - (t->cx % 8);
        while (n--) rawc(t, ' ');
        return;
    }
    if (t->cx >= t->cols) { t->cx = 0; t->cy++; }
    need_row(t);
    row_of(t, t->base + t->cy)[t->cx] = ((u16)t->color << 8) | (u8)ch;
    mark(t, t->cy);
    t->cx++;
    if (t->cx >= t->cols) { t->cx = 0; t->cy++; need_row(t); }
}

static void tputs(const char *s) { while (*s) rawc(T, (u8)*s++); }

static void tprintf(const char *f, ...) {
    char b[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    tputs(b);
}

static void tcolor(int c) { T->color = (u8)c; }

void term_out(void *tv, const char *b, int n) {
    struct term *t = tv, *old = T;
    T = t;
    for (int i = 0; i < n; i++) rawc(t, (u8)b[i]);
    T = old;
}

static void cls(struct term *t) {
    long cur = t->base + t->cy;
    t->base = cur;
    t->cy = 0;
    t->cx = 0;
    ensure(t, cur + t->rows);
    for (int r = 0; r < t->rows; r++) {
        u16 *l = row_of(t, cur + r);
        for (int i = 0; i < MAXC; i++) l[i] = 0x0720;
    }
    t->alld = 1;
}

static void draw_cell(img *s, int x, int y, u16 cell, int inv) {
    u32 fg = pal[(cell >> 8) & 15], bg = pal[(cell >> 12) & 15];
    int ch = cell & 255;
    if (inv) { u32 k = fg; fg = bg; bg = k; if (fg == bg) fg = 0xAAAAAA; }
    glyph(s, x, y, ch, fg, (int)bg, 1);
    if (ch >= 0xC0 && ch <= 0xDF) {
        for (int r = 0; r < 16; r++) fill(s, x + 8, y + r, 1, 1, (gfont_row(ch, r) & 1) ? fg : bg);
    } else {
        fill(s, x + 8, y, 1, 16, bg);
    }
}

static void render(struct term *t) {
    img *s = &t->w->surf;
    mark(t, t->pcy);
    mark(t, t->cy);
    t->pcy = t->cy;
    int y0 = -1, y1 = -1;
    long top = t->base - t->off;
    for (int r = 0; r < t->rows; r++) {
        if (!t->alld && !t->dirty[r]) continue;
        long a = top + r;
        u16 *l = (a >= 0 && a <= t->maxabs && a > t->maxabs - NL) ? row_of(t, a) : NULL;
        for (int c = 0; c < t->cols; c++) {
            u16 cell = l ? l[c] : 0x0720;
            int inv = t->off == 0 && r == t->cy && c == t->cx && t->curon && t->job == 0 && !t->fg;
            draw_cell(s, c * CW, r * CHH, cell, inv);
        }
        t->dirty[r] = 0;
        if (y0 < 0) y0 = r;
        y1 = r;
    }
    if (t->alld) {
        fill(s, t->cols * CW, 0, s->w - t->cols * CW, s->h, 0);
        fill(s, 0, t->rows * CHH, s->w, s->h - t->rows * CHH, 0);
        wm_paint(t->w, 0, 0, s->w, s->h);
    } else if (y0 >= 0) {
        wm_paint(t->w, 0, y0 * CHH, t->cols * CW, (y1 - y0 + 1) * CHH);
    }
    t->alld = 0;
}

static void setcell(struct term *t, long a, int col, int ch) {
    ensure(t, a);
    row_of(t, a)[col] = ((u16)7 << 8) | (u8)ch;
    long r = a - (t->base - t->off);
    if (r >= 0 && r < t->rows) mark(t, (int)r);
}

static void place_cursor(struct term *t) {
    int p = t->ix + t->pos;
    long a = t->iabs + p / t->cols;
    int row = (int)(a - t->base);
    if (row >= t->rows) { t->base += row - t->rows + 1; row = t->rows - 1; t->alld = 1; }
    if (row < 0) row = 0;
    ensure(t, t->base + row);
    t->cy = row;
    t->cx = p % t->cols;
}

static void redraw_input(struct term *t) {
    int need = t->ix + t->len + 1;
    long last = t->iabs + need / t->cols;
    int row = (int)(last - t->base);
    if (row >= t->rows) { t->base += row - t->rows + 1; t->alld = 1; }
    for (int k = 0; k <= t->len; k++) {
        int p = t->ix + k;
        setcell(t, t->iabs + p / t->cols, p % t->cols, k < t->len ? (u8)t->line[k] : ' ');
    }
    place_cursor(t);
}

static void pathstr(char *out, int cap) {
    char p[160];
    vfs_path(T->cwd, p, sizeof p);
    if (!strcmp(p, "/home")) snprintf(out, cap, "~");
    else if (!strncmp(p, "/home/", 6)) snprintf(out, cap, "~%s", p + 5);
    else snprintf(out, cap, "%s", p);
}

static void new_prompt(struct term *t) {
    struct term *o = T;
    T = t;
    char p[160];
    pathstr(p, sizeof p);
    if (t->cx) rawc(t, '\n');
    tcolor(10);
    tprintf("tak@kernel:%s$ ", p);
    tcolor(7);
    t->ix = t->cx;
    t->iabs = t->base + t->cy;
    redraw_input(t);
    T = o;
}

static void term_stop_job(struct term *t) {
    struct term *o = T;
    T = t;
    if (t->job == 1) tprintf("Stopped after %lu bytes.\n", t->jn);
    t->job = 0;
    T = o;
    new_prompt(t);
}

static int tokenize(char *s, char **argv, int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (*s == '"' || *s == '\'') {
            char q = *s++;
            argv[n++] = s;
            while (*s && *s != q) s++;
        } else {
            argv[n++] = s;
            while (*s && *s != ' ' && *s != '\t') s++;
        }
        if (*s) *s++ = 0;
    }
    return n;
}

static node *res(const char *p) { return vfs_find(p, T->cwd); }

static void need_args(const char *c, const char *u) {
    tcolor(12);
    tprintf("%s: usage: %s %s\n", c, c, u);
    tcolor(7);
}

static void err(const char *a, const char *b) {
    tcolor(12);
    tprintf("%s: %s\n", a, b);
    tcolor(7);
}

typedef void (*cmdfn)(int argc, char **argv, char *raw);
struct cmd { const char *name; cmdfn fn; const char *help; };
static const struct cmd cmds[];

static void c_help(int argc, char **argv, char *raw) {
    (void)argc; (void)argv; (void)raw;
    tputs("Built-in commands:\n");
    for (int i = 0; cmds[i].name; i++) tprintf("  %-12s %s\n", cmds[i].name, cmds[i].help);
    tputs("Programs: type a .te or ELF executable in /apps or /bin, or use run <file>.\n");
    tputs("Redirect output with > file or >> file. Ctrl+C stops a program.\n");
}

static void c_clear(int argc, char **argv, char *raw) { (void)argc; (void)argv; (void)raw; cls(T); }

static void c_echo(int argc, char **argv, char *raw) {
    (void)argc; (void)argv;
    int nl = 1, esc = 0;
    char *t = raw;
    for (;;) {
        while (*t == ' ' || *t == '\t') t++;
        if (t[0] != '-' || t[1] == 0 || t[1] == ' ') break;
        char *e = t + 1;
        int ok = 1;
        while (*e && *e != ' ' && *e != '\t') {
            if (*e != 'n' && *e != 'e' && *e != 'E') { ok = 0; break; }
            e++;
        }
        if (!ok) break;
        for (char *o = t + 1; o < e; o++) {
            if (*o == 'n') nl = 0;
            else if (*o == 'e') esc = 1;
            else if (*o == 'E') esc = 0;
        }
        t = e;
    }
    for (int i = 0; t[i]; i++) {
        if (!esc || t[i] != '\\' || !t[i + 1]) { rawc(T, (u8)t[i]); continue; }
        char c = t[++i];
        switch (c) {
        case 'a': break;
        case 'b': rawc(T, '\b'); break;
        case 'c': return;
        case 'n': rawc(T, '\n'); break;
        case 'r': rawc(T, '\r'); break;
        case 't': rawc(T, '\t'); break;
        case '\\': rawc(T, '\\'); break;
        default: rawc(T, '\\'); rawc(T, (u8)c); break;
        }
    }
    if (nl) rawc(T, '\n');
}

static void c_print(int argc, char **argv, char *raw) { (void)argc; (void)argv; tputs(raw); rawc(T, '\n'); }

static void c_infobyte(int argc, char **argv, char *raw) {
    (void)argc; (void)argv; (void)raw;
    T->job = 1;
    T->jstate = 0x9E3779B97F4A7C15ULL;
    T->jn = 0;
    tputs("Streaming Data\n");
}

static void job_tick(struct term *t) {
    if (t->job != 1) return;
    struct term *o = T;
    T = t;
    for (int line = 0; line < 12; line++) {
        for (int k = 0; k < 8; k++) {
            t->jstate ^= t->jstate << 13;
            t->jstate ^= t->jstate >> 7;
            t->jstate ^= t->jstate << 17;
            u8 b = (u8)t->jstate;
            for (int bit = 7; bit >= 0; bit--) rawc(t, (b & (1 << bit)) ? '1' : '0');
            rawc(t, ' ');
            t->jn++;
        }
        rawc(t, '\n');
    }
    T = o;
}

static void c_uname(int argc, char **argv, char *raw) {
    (void)raw;
    const char *a = argc > 1 ? argv[1] : "";
    if (!strcmp(a, "-a")) tputs("TAK tak-kernel " VER " x86_64\n");
    else if (!strcmp(a, "-m")) tputs("x86_64\n");
    else if (!strcmp(a, "-r")) tputs(VER "\n");
    else tputs("TAK\n");
}

static void c_version(int argc, char **argv, char *raw) { (void)argc; (void)argv; (void)raw; tputs("TAK Kernel " VER " (x86_64)\n"); }
static void c_halt(int argc, char **argv, char *raw) { (void)argc; (void)argv; (void)raw; sys_halt(); }
static void c_reboot(int argc, char **argv, char *raw) { (void)argc; (void)argv; (void)raw; sys_reboot(); }
static void c_poweroff(int argc, char **argv, char *raw) { (void)argc; (void)argv; (void)raw; sys_poweroff(); }

static void set_layout(const char *n) {
    if (!strcasecmp(n, "US")) { kb_layout(LAYOUT_US); tputs("Keyboard layout changed to US.\n"); }
    else if (!strcasecmp(n, "BR-ABNT2") || !strcasecmp(n, "BR")) { kb_layout(LAYOUT_ABNT2); tputs("Keyboard layout changed to BR-ABNT2.\n"); }
    else tputs("Unknown layout, Choose US or BR-ABNT2.\n");
}

static void c_keyboard(int argc, char **argv, char *raw) {
    (void)raw;
    if (argc < 2) tprintf("Active keyboard layout: %s\n", kb_layout_get() == LAYOUT_ABNT2 ? "BR-ABNT2" : "US");
    else set_layout(argv[1]);
}

static int cmp_node(node *a, node *b) {
    if (a->dir != b->dir) return b->dir - a->dir;
    return strcasecmp(a->name, b->name);
}

static void c_ls(int argc, char **argv, char *raw) {
    (void)raw;
    node *d = argc > 1 ? res(argv[1]) : T->cwd;
    if (!d) { err("ls", "no such file or directory"); return; }
    if (!d->dir) { tprintf("%s  %lu\n", d->name, d->size); return; }
    vfs_load(d);
    node *a[256];
    int n = 0;
    for (node *k = d->kid; k && n < 256; k = k->nxt) a[n++] = k;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && cmp_node(a[j - 1], a[j]) > 0; j--) { node *t = a[j]; a[j] = a[j - 1]; a[j - 1] = t; }
    for (int i = 0; i < n; i++) {
        if (a[i]->dir) { tcolor(11); tprintf("  %s/\n", a[i]->name); }
        else {
            tcolor(!strcasecmp(ext_of(a[i]->name), "te") ? 10 : 7);
            tprintf("  %-28s %lu\n", a[i]->name, a[i]->size);
        }
        tcolor(7);
    }
    if (!n) tputs("  (empty)\n");
}

static void c_cd(int argc, char **argv, char *raw) {
    (void)raw;
    node *d = argc > 1 ? res(argv[1]) : vfs_find("/home", NULL);
    if (!d) { err("cd", "no such directory"); return; }
    if (!d->dir) { err("cd", "not a directory"); return; }
    T->cwd = d;
}

static void c_pwd(int argc, char **argv, char *raw) {
    (void)argc; (void)argv; (void)raw;
    char p[160];
    vfs_path(T->cwd, p, sizeof p);
    tprintf("%s\n", p);
}

static void c_cat(int argc, char **argv, char *raw) {
    (void)raw;
    if (argc < 2) { need_args("cat", "file"); return; }
    for (int i = 1; i < argc; i++) {
        node *n = res(argv[i]);
        if (!n || n->dir) { err(argv[i], "no such file"); continue; }
        u64 sz;
        u8 *b = vfs_slurp(n, &sz);
        if (!b) { err(argv[i], "read error"); continue; }
        for (u64 k = 0; k < sz; k++) {
            u8 c = b[k];
            rawc(T, (c >= 32 || c == '\n' || c == '\t' || c == '\r') ? c : '.');
        }
        if (sz && b[sz - 1] != '\n') rawc(T, '\n');
        kfree(b);
    }
}

static node *parent_of(const char *path, char *name, int cap) {
    const char *sl = strrchr(path, '/');
    if (!sl) { snprintf(name, cap, "%s", path); return T->cwd; }
    snprintf(name, cap, "%s", sl + 1);
    if (sl == path) return vroot;
    char dir[160];
    int n = (int)(sl - path);
    if (n > 159) n = 159;
    memcpy(dir, path, n);
    dir[n] = 0;
    return res(dir);
}

static void c_mkdir(int argc, char **argv, char *raw) {
    (void)raw;
    if (argc < 2) { need_args("mkdir", "name"); return; }
    char nm[64];
    node *p = parent_of(argv[1], nm, sizeof nm);
    if (!p || !p->dir || !vfs_mk(p, nm, 1)) err("mkdir", "cannot create directory");
}

static void c_touch(int argc, char **argv, char *raw) {
    (void)raw;
    if (argc < 2) { need_args("touch", "name"); return; }
    char nm[64];
    node *p = parent_of(argv[1], nm, sizeof nm);
    if (!p || !p->dir) { err("touch", "bad path"); return; }
    if (!vfs_child(p, nm) && !vfs_mk(p, nm, 0)) err("touch", "cannot create file");
}

static void c_rm(int argc, char **argv, char *raw) {
    (void)raw;
    if (argc < 2) { need_args("rm", "name"); return; }
    for (int i = 1; i < argc; i++) {
        node *n = res(argv[i]);
        if (!n) { err(argv[i], "no such file"); continue; }
        if (n == T->cwd) { err(argv[i], "is the current directory"); continue; }
        if (vfs_rm(n) < 0) err(argv[i], "cannot remove (directory not empty?)");
    }
}

static int copy_file(node *src, node *dir, const char *name) {
    u64 sz;
    u8 *b = vfs_slurp(src, &sz);
    if (!b) return -1;
    int r = vfs_save(dir, name, b, sz);
    kfree(b);
    return r;
}

static void c_cp(int argc, char **argv, char *raw) {
    (void)raw;
    if (argc < 3) { need_args("cp", "src dst"); return; }
    node *s = res(argv[1]);
    if (!s || s->dir) { err("cp", "source must be a file"); return; }
    node *d = res(argv[2]);
    int r;
    if (d && d->dir) r = copy_file(s, d, s->name);
    else {
        char nm[64];
        node *p = parent_of(argv[2], nm, sizeof nm);
        r = p && p->dir ? copy_file(s, p, nm) : -1;
    }
    if (r < 0) err("cp", "copy failed");
}

static void c_mv(int argc, char **argv, char *raw) {
    (void)raw;
    if (argc < 3) { need_args("mv", "src dst"); return; }
    node *s = res(argv[1]);
    if (!s || !s->par) { err("mv", "no such file"); return; }
    node *d = res(argv[2]);
    char nm[64];
    node *p;
    if (d && d->dir) { p = d; snprintf(nm, sizeof nm, "%s", s->name); }
    else p = parent_of(argv[2], nm, sizeof nm);
    if (!p || !p->dir) { err("mv", "bad destination"); return; }
    if (p == s->par && !vfs_child(p, nm)) {
        if (vfs_rename(s, nm) < 0) err("mv", "rename failed");
        return;
    }
    if (s->dir) { err("mv", "cannot move directories"); return; }
    if (copy_file(s, p, nm) < 0 || vfs_rm(s) < 0) err("mv", "move failed");
}

static void c_run(int argc, char **argv, char *raw) {
    (void)raw;
    if (argc < 2) { need_args("run", "program [arg]"); return; }
    node *n = res(argv[1]);
    if (!n || n->dir) { err(argv[1], "no such program"); return; }
    char p[160];
    vfs_path(n, p, sizeof p);
    int pid = proc_spawn(p, argc > 2 ? argv[2] : NULL, T);
    if (pid < 0) err(argv[1], "cannot load executable (bad format or unsupported feature)");
    else T->fg = pid;
}

static void c_open(int argc, char **argv, char *raw) {
    (void)raw;
    if (argc < 2) { need_args("open", "file"); return; }
    node *n = res(argv[1]);
    if (!n) { err(argv[1], "no such file"); return; }
    open_file(n);
}

static void c_files(int argc, char **argv, char *raw) {
    (void)raw;
    char p[160];
    node *n = argc > 1 ? res(argv[1]) : T->cwd;
    if (!n || !n->dir) { err("files", "no such directory"); return; }
    vfs_path(n, p, sizeof p);
    app_open("Files", p);
}

static void c_edit(int argc, char **argv, char *raw) {
    (void)raw;
    char p[160];
    if (argc < 2) { app_open("Text Editor", NULL); return; }
    node *n = res(argv[1]);
    if (!n) {
        char nm[64];
        node *par = parent_of(argv[1], nm, sizeof nm);
        if (!par || !par->dir) { err("edit", "bad path"); return; }
        n = vfs_mk(par, nm, 0);
    }
    if (!n || n->dir) { err("edit", "cannot open"); return; }
    vfs_path(n, p, sizeof p);
    app_open("Text Editor", p);
}

static void c_view(int argc, char **argv, char *raw) {
    (void)raw;
    char p[160];
    if (argc < 2) { need_args("view", "image"); return; }
    node *n = res(argv[1]);
    if (!n || n->dir) { err(argv[1], "no such file"); return; }
    vfs_path(n, p, sizeof p);
    app_open("Image Viewer", p);
}

extern img *img_load(node *n);

static void c_wall(int argc, char **argv, char *raw) {
    (void)raw;
    if (argc < 2) { wm_wallpaper(NULL); return; }
    node *n = res(argv[1]);
    if (!n || n->dir) { err(argv[1], "no such file"); return; }
    img *i = img_load(n);
    if (!i) { err(argv[1], "cannot decode image"); return; }
    wm_wallpaper(i);
    img_free(i);
}

static void ps_cb(int pid, const char *name, int st, u64 kb, void *ctx) {
    (void)ctx;
    static const char *sn[] = { "ready", "running", "blocked", "sleeping", "dead" };
    tprintf("  %-5d %-20s %-9s %lu KB\n", pid, name, sn[st], kb);
}

static void c_ps(int argc, char **argv, char *raw) {
    (void)argc; (void)argv; (void)raw;
    tputs("  PID   NAME                 STATE     MEM\n");
    proc_list(ps_cb, NULL);
}

static void c_kill(int argc, char **argv, char *raw) {
    (void)raw;
    if (argc < 2) { need_args("kill", "pid"); return; }
    if (proc_kill(atoi(argv[1])) < 0) err("kill", "no such process");
}

static void c_mem(int argc, char **argv, char *raw) {
    (void)argc; (void)argv; (void)raw;
    tprintf("RAM usable: %lu KB   free frames: %lu KB   heap free: %lu KB\n", mem_total_kb(), mem_free_kb(), heap_free_kb());
}

static void c_date(int argc, char **argv, char *raw) {
    (void)argc; (void)argv; (void)raw;
    int h, m, s, d, mo, y;
    if (rtc_get(&h, &m, &s, &d, &mo, &y)) tprintf("%04d-%02d-%02d %02d:%02d:%02d\n", y, mo, d, h, m, s);
    else tputs("clock not available\n");
}

static void c_uptime(int argc, char **argv, char *raw) {
    (void)argc; (void)argv; (void)raw;
    u64 s = uptime_ms() / 1000;
    tprintf("up %lu min %lu s\n", s / 60, s % 60);
}

static void c_sysinfo(int argc, char **argv, char *raw) {
    (void)argc; (void)argv; (void)raw;
    app_open("System Info", NULL);
}

static void c_start(int argc, char **argv, char *raw) {
    (void)argc; (void)argv;
    if (!*raw) { need_args("start", "app name"); return; }
    if (!app_open(raw, NULL)) err(raw, "no such app (see: apps)");
}

static void c_disks(int argc, char **argv, char *raw) {
    (void)argc; (void)argv; (void)raw;
    if (!ndisks) { tputs("No ATA disks found.\n"); return; }
    for (int i = 0; i < ndisks; i++) {
        tprintf("  %s  %lu MB  %s\n", disks[i].name, disks[i].sectors / 2048, disks[i].model);
        struct part p[4];
        int n = disk_parts(i, p, 4);
        for (int k = 0; k < n; k++) tprintf("    part %d: start %lu, %lu MB, type %02x%s\n", k + 1, p[k].start, p[k].size / 2048, p[k].type, p[k].boot ? " (boot)" : "");
    }
}

static void say_line(const char *m) { tputs(m); rawc(T, '\n'); }
int install_cli(int disk, void (*say)(const char *));

static void c_install(int argc, char **argv, char *raw) {
    (void)raw;
    if (argc < 3 || strcmp(argv[2], "yes")) { need_args("install", "<disk e.g. hda> yes   (erases the whole disk)"); return; }
    int d = -1;
    for (int i = 0; i < ndisks; i++) if (!strcmp(disks[i].name, argv[1])) d = i;
    if (d < 0) { err("install", "no such disk (see: disks)"); return; }
    if (install_cli(d, say_line) == 0) tputs("Install finished. Remove the live medium and reboot.\n");
}

static void c_taskbar(int argc, char **argv, char *raw) {
    (void)raw;
    static const char *n[] = { "left", "top", "right", "bottom" };
    for (int i = 0; argc > 1 && i < 4; i++) if (!strcasecmp(argv[1], n[i])) { wm_tbpos(i); return; }
    need_args("taskbar", "left|top|right|bottom");
}

static void c_lsapps(int argc, char **argv, char *raw) {
    (void)argc; (void)argv; (void)raw;
    for (int i = 0; i < napps; i++) tprintf("  %s\n", apps[i].name);
}

#define XHCI_MAX 255

static int map_uc(u64 a) {
    extern u64 page_directory[];
    if (a >= 0x100000000ULL) return 0;
    size_t e = a >> 21;
    if (e >= 2048) return 0;
    page_directory[e] |= 0x18;
    __asm__ volatile("invlpg (%0)" : : "r"((uintptr_t)a) : "memory");
    return 1;
}

static void xhci_probe(u8 bus, u8 dev, u8 fn) {
    u32 bl = pci_r32(bus, dev, fn, 0x10);
    if (bl & 1) { tputs("xHCI BAR is I/O space; cannot map it.\n"); return; }
    u64 mmio = bl & 0xFFFFFFF0u;
    if (((bl >> 1) & 3) == 2) mmio |= (u64)pci_r32(bus, dev, fn, 0x14) << 32;
    if (!mmio || mmio >= 0x100000000ULL) { tputs("xHCI MMIO BAR is outside TAK's current mapped range.\n"); return; }
    u16 cmd = (u16)pci_r32(bus, dev, fn, 4);
    if (!(cmd & 2)) pci_w16(bus, dev, fn, 4, cmd | 2);
    if (!map_uc(mmio)) { tputs("Could not map xHCI MMIO.\n"); return; }
    volatile u8 *r = (volatile u8 *)mmio;
    u8 caplen = r[0];
    u16 ver = *(volatile u16 *)(r + 2);
    u32 hcs1 = *(volatile u32 *)(r + 4);
    unsigned np = (hcs1 >> 24) & 0xFF;
    if (caplen < 0x20 || np == 0 || np > XHCI_MAX) { tputs("xHCI capability registers returned invalid values.\n"); return; }
    u32 st = *(volatile u32 *)(r + caplen + 4);
    tprintf("xHCI %04X at MMIO %08lX; ports: %u%s\n", ver, mmio, np, (st & 1) ? "; halted" : "; running");
    unsigned conn = 0;
    for (unsigned p = 0; p < np; p++) {
        u32 ps = *(volatile u32 *)(mmio + caplen + 0x400 + p * 0x10);
        if (!(ps & 1)) continue;
        conn++;
        tprintf("  connected USB port %u; speed code %02X\n", p + 1, (ps >> 10) & 15);
    }
    if (!conn) tputs("  no USB devices connected to xHCI ports\n");
    tputs("xHCI MMIO/port probe only; USB transfers are not implemented yet.\n");
}

static void c_usb(int argc, char **argv, char *raw) {
    (void)argc; (void)argv; (void)raw;
    unsigned count = 0;
    for (unsigned bus = 0; bus < 256; bus++)
        for (unsigned dev = 0; dev < 32; dev++) {
            if ((u16)pci_r32(bus, dev, 0, 0) == 0xFFFF) continue;
            u8 hdr = pci_r8(bus, dev, 0, 0x0E);
            unsigned nf = (hdr & 0x80) ? 8 : 1;
            for (unsigned fn = 0; fn < nf; fn++) {
                if ((u16)pci_r32(bus, dev, fn, 0) == 0xFFFF) continue;
                u32 ci = pci_r32(bus, dev, fn, 8);
                if ((u8)(ci >> 24) != 0x0C || (u8)(ci >> 16) != 3 || (u8)(ci >> 8) != 0x30) continue;
                tprintf("PCI %02X:%02X.%u xHCI controller\n", bus, dev, fn);
                count++;
                xhci_probe(bus, dev, fn);
            }
        }
    if (!count) tputs("No PCI xHCI USB host controllers found.\n");
}

static void c_netinfo(int argc, char **argv, char *raw) {
    (void)argc; (void)argv; (void)raw;
    unsigned count = 0;
    for (unsigned bus = 0; bus < 256; bus++)
        for (unsigned dev = 0; dev < 32; dev++) {
            if ((u16)pci_r32(bus, dev, 0, 0) == 0xFFFF) continue;
            u8 hdr = pci_r8(bus, dev, 0, 0x0E);
            unsigned nf = (hdr & 0x80) ? 8 : 1;
            for (unsigned fn = 0; fn < nf; fn++) {
                u32 id = pci_r32(bus, dev, fn, 0);
                if ((u16)id == 0xFFFF) continue;
                u32 ci = pci_r32(bus, dev, fn, 8);
                if ((u8)(ci >> 24) != 0x02) continue;
                u16 vendor = (u16)id;
                u16 device = (u16)(id >> 16);
                const char *name = "PCI network controller";
                if ((u8)(ci >> 16) == 0x00) {
                    if (vendor == 0x8086 && device == 0x100E)
                        name = "Intel 82540EM / e1000 (QEMU or VirtualBox)";
                    else if (vendor == 0x1AF4 && (device == 0x1000 || device == 0x1041))
                        name = "virtio-net (transitional or modern)";
                    else if (vendor == 0x1022 && device == 0x2000)
                        name = "AMD PCnet";
                    else if (vendor == 0x10EC && device == 0x8139)
                        name = "Realtek RTL8139";
                    else
                        name = "PCI Ethernet controller";
                }
                tprintf("PCI %02X:%02X.%u %s [%04X:%04X]\n",
                        bus, dev, fn, name, vendor, device);
                count++;
            }
        }
    if (!count) tputs("No PCI network controllers found.\n");
    if (net_active()) {
        const u8 *mac = net_mac();
        tprintf("Intel E1000 driver: active; link %s; MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
                net_link_up() ? "up" : "down",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        tprintf("Ethernet frames: RX %lu (dropped %lu), TX %lu\n",
                net_rx_packets(), net_rx_dropped(), net_tx_packets());
        tprintf("IPv4: %u.%u.%u.%u; mask %u.%u.%u.%u; gateway %u.%u.%u.%u; DNS %u.%u.%u.%u; DHCP %s\n",
                (u8)(net_ipv4_address() >> 24), (u8)(net_ipv4_address() >> 16),
                (u8)(net_ipv4_address() >> 8), (u8)net_ipv4_address(),
                (u8)(net_ipv4_netmask() >> 24), (u8)(net_ipv4_netmask() >> 16),
                (u8)(net_ipv4_netmask() >> 8), (u8)net_ipv4_netmask(),
                (u8)(net_ipv4_gateway() >> 24), (u8)(net_ipv4_gateway() >> 16),
                (u8)(net_ipv4_gateway() >> 8), (u8)net_ipv4_gateway(),
                (u8)(net_ipv4_dns() >> 24), (u8)(net_ipv4_dns() >> 16),
                (u8)(net_ipv4_dns() >> 8), (u8)net_ipv4_dns(),
                net_dhcp_ready() ? "configured" : "pending");
        tputs("ARP, IPv4, ICMP, DHCP, DNS, and a bounded HTTP-over-TCP client are supported; TLS and Linux networking are not implemented.\n");
    } else {
        tputs("No supported E1000 driver active; IP and higher network protocols are not implemented.\n");
    }
}

static void c_dns(int argc, char **argv, char *raw) {
    (void)raw;
    if (argc < 2) { need_args("dns", "hostname"); return; }
    u32 address;
    if (!net_dhcp_ready()) {
        tputs("DNS lookup needs an active network and DHCP lease.\n");
        return;
    }
    if (!net_dns_resolve(argv[1], &address, 800)) {
        tprintf("DNS lookup failed or timed out: %s\n", argv[1]);
        return;
    }
    tprintf("%s -> %u.%u.%u.%u\n", argv[1],
            (u8)(address >> 24), (u8)(address >> 16),
            (u8)(address >> 8), (u8)address);
}

static void c_httpget(int argc, char **argv, char *raw) {
    (void)raw;
    if (argc < 3) { need_args("http-get", "hostname path"); return; }
    char response[4096];
    long length = net_http_get(argv[1], argv[2], response,
                               sizeof(response), 1000);
    if (length == -1) {
        tputs("HTTP request failed or timed out. This command only supports unencrypted HTTP on port 80.\n");
        return;
    }
    if (length == -2) {
        tputs("HTTP response exceeded the bounded receive buffer; response truncated.\n");
        tputs(response);
        return;
    }
    tputs(response);
    if (length && response[length - 1] != '\n') rawc(T, '\n');
}

static const struct cmd cmds[] = {
    { "help", c_help, "shows this list" },
    { "clear", c_clear, "clears the screen" },
    { "echo", c_echo, "write text (-n, -e, -E)" },
    { "print", c_print, "prints text" },
    { "ls", c_ls, "list a directory" },
    { "cd", c_cd, "change directory" },
    { "pwd", c_pwd, "print working directory" },
    { "cat", c_cat, "show a file" },
    { "mkdir", c_mkdir, "make a directory" },
    { "touch", c_touch, "create an empty file" },
    { "rm", c_rm, "remove a file or empty directory" },
    { "cp", c_cp, "copy a file" },
    { "mv", c_mv, "move or rename a file" },
    { "run", c_run, "run a .te or ELF executable" },
    { "open", c_open, "open a file with its app" },
    { "files", c_files, "open the file explorer" },
    { "edit", c_edit, "open the text editor" },
    { "view", c_view, "open an image" },
    { "wall", c_wall, "set wallpaper (no arg = default)" },
    { "apps", c_lsapps, "list built-in apps" },
    { "taskbar", c_taskbar, "move the taskbar: left|top|right|bottom" },
    { "disks", c_disks, "list disks and partitions" },
    { "install", c_install, "install Astros: install <disk> yes" },
    { "start", c_start, "open an app by name" },
    { "ps", c_ps, "list running programs" },
    { "kill", c_kill, "kill a program by pid" },
    { "mem", c_mem, "memory usage" },
    { "date", c_date, "show the clock" },
    { "uptime", c_uptime, "time since boot" },
    { "sysinfo", c_sysinfo, "open system info" },
    { "info-byte", c_infobyte, "stream binary data until Escape" },
    { "usb-info", c_usb, "inspect xHCI MMIO and ports" },
    { "net-info", c_netinfo, "show network adapter and E1000 frame I/O status" },
    { "dns", c_dns, "resolve a hostname using the DHCP DNS server" },
    { "http-get", c_httpget, "fetch an unencrypted HTTP URL: hostname path" },
    { "uname", c_uname, "[-a] kernel and system information" },
    { "version", c_version, "shows kernel version" },
    { "ver", c_version, "same as version" },
    { "keyboard", c_keyboard, "show or set layout (US, BR-ABNT2)" },
    { "keyboard-US", NULL, "change layout" },
    { "keyboard-BR-ABNT2", NULL, "change layout" },
    { "halt", c_halt, "stop the CPU" },
    { "reboot", c_reboot, "restarts the computer" },
    { "poweroff", c_poweroff, "turn the machine off" },
    { NULL, NULL, NULL }
};

static node *find_prog(const char *name) {
    node *n = NULL;
    if (strchr(name, '/')) n = res(name);
    else {
        n = vfs_child(T->cwd, name);
        if (!n || n->dir) {
            char p[96];
            snprintf(p, sizeof p, "/apps/%s", name);
            n = vfs_find(p, NULL);
            if (!n) { snprintf(p, sizeof p, "/apps/%s.te", name); n = vfs_find(p, NULL); }
            if (!n) { snprintf(p, sizeof p, "/apps/%s.elf", name); n = vfs_find(p, NULL); }
            if (!n) { snprintf(p, sizeof p, "/bin/%s", name); n = vfs_find(p, NULL); }
            if (!n) { snprintf(p, sizeof p, "/bin/%s.elf", name); n = vfs_find(p, NULL); }
        }
    }
    if (n && !n->dir) {
        u8 magic[4];
        int elf = vfs_rd(n, 0, magic, sizeof magic) == sizeof magic &&
                  magic[0] == 0x7F && magic[1] == 'E' &&
                  magic[2] == 'L' && magic[3] == 'F';
        if (!strcasecmp(ext_of(n->name), "te") || elf) return n;
    }
    return NULL;
}

static void exec(struct term *t, char *line) {
    T = t;
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return;
    char *redir = NULL;
    int app = 0;
    char q = 0;
    for (char *s = p; *s; s++) {
        if (q) { if (*s == q) q = 0; continue; }
        if (*s == '"' || *s == '\'') { q = *s; continue; }
        if (*s == '>') {
            redir = s;
            *s++ = 0;
            if (*s == '>') { app = 1; *s++ = 0; }
            while (*s == ' ') s++;
            redir = s;
            break;
        }
    }
    char *name = p, *args = p;
    while (*args && *args != ' ' && *args != '\t') args++;
    if (*args) { *args++ = 0; while (*args == ' ' || *args == '\t') args++; }
    char rawbuf[256];
    snprintf(rawbuf, sizeof rawbuf, "%s", args);
    int len = (int)strlen(rawbuf);
    while (len > 0 && (rawbuf[len - 1] == ' ' || rawbuf[len - 1] == '\t')) rawbuf[--len] = 0;
    char tok[256];
    snprintf(tok, sizeof tok, "%s", rawbuf);
    char *argv[16];
    argv[0] = name;
    int argc = 1 + tokenize(tok, argv + 1, 14);
    if (redir && *redir) { t->rbuf = kalloc(1024); t->rcap = 1024; t->rlen = 0; if (!t->rbuf) t->rcap = 0; }
    if (!strncasecmp(name, "keyboard-", 9)) { set_layout(name + 9); goto done; }
    for (int i = 0; cmds[i].name; i++) {
        if (cmds[i].fn && !strcmp(cmds[i].name, name)) { cmds[i].fn(argc, argv, rawbuf); goto done; }
    }
    node *prog = find_prog(name);
    if (prog) {
        char path[160];
        vfs_path(prog, path, sizeof path);
        int pid = proc_spawn(path, argc > 1 ? argv[1] : NULL, t);
        if (pid < 0) err(name, "cannot load executable (bad format or unsupported feature)");
        else t->fg = pid;
        goto done;
    }
    tcolor(12);
    tprintf("%s: command not found\n", name);
    tcolor(7);
done:
    if (redir && *redir) {
        char *buf = t->rbuf;
        int n = t->rlen;
        t->rbuf = NULL;
        t->rlen = t->rcap = 0;
        char nm[64];
        node *par = parent_of(redir, nm, sizeof nm);
        node *f = par && par->dir ? vfs_child(par, nm) : NULL;
        if (par && par->dir && !f) f = vfs_mk(par, nm, 0);
        if (!f || f->dir) err("redirect", "cannot write file");
        else {
            if (!app) f->size = 0;
            vfs_wr(f, app ? f->size : 0, buf ? buf : "", n);
        }
        kfree(buf);
    }
}

static void ins(struct term *t, const char *s) {
    for (; *s && t->len < 254; s++) {
        memmove(t->line + t->pos + 1, t->line + t->pos, t->len - t->pos + 1);
        t->line[t->pos++] = *s;
        t->len++;
    }
    t->line[t->len] = 0;
}

static void complete(struct term *t) {
    T = t;
    int s = t->pos;
    while (s > 0 && t->line[s - 1] != ' ') s--;
    char word[128];
    int wl = t->pos - s;
    if (wl > 127) wl = 127;
    memcpy(word, t->line + s, wl);
    word[wl] = 0;
    char dir[128] = "", base[64];
    char *sl = strrchr(word, '/');
    if (sl) { int n = (int)(sl - word) + 1; memcpy(dir, word, n); dir[n] = 0; snprintf(base, sizeof base, "%s", sl + 1); }
    else snprintf(base, sizeof base, "%s", word);
    int bl = (int)strlen(base);
    char match[32][64];
    int isd[32], nm = 0;
    if (s == 0 && !sl) {
        for (int i = 0; cmds[i].name && nm < 32; i++)
            if (!strncasecmp(cmds[i].name, base, bl)) { snprintf(match[nm], 64, "%s", cmds[i].name); isd[nm++] = 0; }
        node *ad = vfs_find("/apps", NULL);
        if (ad) for (node *k = ad->kid; k && nm < 32; k = k->nxt) {
            if (k->dir || strcasecmp(ext_of(k->name), "te")) continue;
            char b[64];
            snprintf(b, sizeof b, "%s", k->name);
            char *dot = strrchr(b, '.');
            if (dot) *dot = 0;
            if (!strncasecmp(b, base, bl)) { snprintf(match[nm], 64, "%s", b); isd[nm++] = 0; }
        }
    } else {
        node *d = dir[0] ? res(dir) : t->cwd;
        if (!d || !d->dir) return;
        vfs_load(d);
        for (node *k = d->kid; k && nm < 32; k = k->nxt)
            if (!strncasecmp(k->name, base, bl)) { snprintf(match[nm], 64, "%s", k->name); isd[nm++] = k->dir; }
    }
    if (!nm) return;
    int common = (int)strlen(match[0]);
    for (int i = 1; i < nm; i++) {
        int k = 0;
        while (k < common && tolower(match[0][k]) == tolower(match[i][k])) k++;
        common = k;
    }
    if (common > bl) {
        char add[64];
        memcpy(add, match[0] + bl, common - bl);
        add[common - bl] = 0;
        ins(t, add);
        if (nm == 1) ins(t, isd[0] ? "/" : " ");
    } else if (nm > 1) {
        int p = t->pos;
        t->pos = t->len;
        place_cursor(t);
        rawc(t, '\n');
        for (int i = 0; i < nm; i++) { tputs(match[i]); tputs(isd[i] ? "/  " : "  "); }
        t->pos = p;
        new_prompt(t);
        return;
    }
    redraw_input(t);
}

static void hist_set(struct term *t, int i) {
    t->hi = i;
    snprintf(t->line, sizeof t->line, "%s", i < t->nh ? t->hist[i] : "");
    t->len = t->pos = (int)strlen(t->line);
    redraw_input(t);
}

static void tkey(win *w, int key, int mod) {
    struct term *t = w->priv;
    if (t->fg) {
        if ((mod & MOD_CTRL) && (key == 'c' || key == 'C')) { proc_kill(t->fg); return; }
        if (key < 256 && key) proc_stdin(t->fg, key);
        else if (key == K_UP) proc_stdin(t->fg, 0x11);
        return;
    }
    if (t->job) {
        if (key == 27 || ((mod & MOD_CTRL) && (key == 'c' || key == 'C'))) term_stop_job(t);
        return;
    }
    if (key == K_PGUP || key == K_PGDN) {
        int d = t->rows - 2;
        t->off += key == K_PGUP ? d : -d;
        long lim = t->base - (t->maxabs > NL ? t->maxabs - NL + 1 : 0);
        if (t->off > lim) t->off = (int)lim;
        if (t->off < 0) t->off = 0;
        t->alld = 1;
        render(t);
        return;
    }
    if (t->off) { t->off = 0; t->alld = 1; }
    if ((mod & MOD_CTRL) && (key == 'l' || key == 'L')) { cls(t); t->pos = t->len = 0; new_prompt(t); render(t); return; }
    if ((mod & MOD_CTRL) && (key == 'c' || key == 'C')) {
        t->pos = t->len;
        place_cursor(t);
        T = t; tputs("^C\n");
        t->len = t->pos = 0; t->line[0] = 0;
        new_prompt(t);
        render(t);
        return;
    }
    if ((mod & MOD_CTRL) && (key == 'u' || key == 'U')) { t->len = t->pos = 0; t->line[0] = 0; redraw_input(t); render(t); return; }
    switch (key) {
    case '\n': {
        t->pos = t->len;
        place_cursor(t);
        T = t;
        rawc(t, '\n');
        char cmd[256];
        snprintf(cmd, sizeof cmd, "%s", t->line);
        if (t->len && (t->nh == 0 || strcmp(t->hist[t->nh - 1], t->line))) {
            if (t->nh == 32) { memmove(t->hist[0], t->hist[1], 31 * 256); t->nh--; }
            snprintf(t->hist[t->nh++], 256, "%s", t->line);
        }
        t->hi = t->nh;
        t->len = t->pos = 0;
        t->line[0] = 0;
        exec(t, cmd);
        if (!t->job && !t->fg) new_prompt(t);
        break;
    }
    case '\b':
        if (t->pos > 0) {
            memmove(t->line + t->pos - 1, t->line + t->pos, t->len - t->pos + 1);
            t->pos--; t->len--;
            redraw_input(t);
        }
        break;
    case K_DEL:
        if (t->pos < t->len) {
            memmove(t->line + t->pos, t->line + t->pos + 1, t->len - t->pos);
            t->len--;
            redraw_input(t);
        }
        break;
    case K_LEFT: if (t->pos > 0) { t->pos--; place_cursor(t); } break;
    case K_RIGHT: if (t->pos < t->len) { t->pos++; place_cursor(t); } break;
    case K_HOME: t->pos = 0; place_cursor(t); break;
    case K_END: t->pos = t->len; place_cursor(t); break;
    case K_UP: if (t->hi > 0) hist_set(t, t->hi - 1); break;
    case K_DOWN: if (t->hi < t->nh) hist_set(t, t->hi + 1); break;
    case '\t': complete(t); break;
    default:
        if (key >= ' ' && key < 256) {
            char s[2] = { (char)key, 0 };
            ins(t, s);
            redraw_input(t);
        }
        break;
    }
    t->curon = 1;
    render(t);
}

static void tmouse(win *w, int x, int y, int b, int pb, int wheel) {
    (void)x; (void)y; (void)b; (void)pb;
    struct term *t = w->priv;
    if (!wheel) return;
    t->off += wheel * 3;
    long lim = t->base - (t->maxabs > NL ? t->maxabs - NL + 1 : 0);
    if (t->off > lim) t->off = (int)lim;
    if (t->off < 0) t->off = 0;
    t->alld = 1;
    render(t);
}

static void ttick(win *w) {
    struct term *t = w->priv;
    if (t->job) {
        job_tick(t);
    }
    if (t->fg && !proc_alive(t->fg)) {
        t->fg = 0;
        new_prompt(t);
    }
    if (ticks % 50 == 0) { t->curon = !t->curon; mark(t, t->cy); }
    render(t);
}

static void tresize(win *w, int nw, int nh) {
    struct term *t = w->priv;
    t->cols = nw / CW;
    t->rows = nh / CHH;
    if (t->cols > MAXC) t->cols = MAXC;
    if (t->rows > MAXR) t->rows = MAXR;
    if (t->cols < 20) t->cols = 20;
    if (t->rows < 4) t->rows = 4;
    if (t->cy >= t->rows) { t->base += t->cy - t->rows + 1; t->cy = t->rows - 1; }
    if (t->cx >= t->cols) t->cx = t->cols - 1;
    t->alld = 1;
    if (!t->job && !t->fg) place_cursor(t);
    render(t);
}

static void tclose(win *w) {
    struct term *t = w->priv;
    if (t->fg) proc_kill(t->fg);
    kfree(t->lines);
    kfree(t->rbuf);
    kfree(t);
}

static const char *logo[] = {
    "                            ***********************",
    "                            **     ***        *****",
    "                            **     *****     *** **",
    "                            **    **   *******   **",
    "                            **   **    ** ***    **",
    "                            **  ***   **   ***** **",
    "                            **  **  ***    **  ****",
    "                            ** **  ***      **   **",
    "                            **** ***        **   **",
    "                            *******         **   **",
    "                            ***********************",
};

win *term_open(const char *arg) {
    win *w = wm_open("Terminal", 80 * CW, 25 * CHH, WF_RESIZE);
    if (!w) return NULL;
    struct term *t = kzalloc(sizeof *t);
    t->lines = kzalloc((u64)NL * MAXC * 2);
    if (!t->lines) { kfree(t); wm_close(w); return NULL; }
    t->w = w;
    t->cols = 80; t->rows = 25;
    t->maxabs = -1;
    t->color = 7;
    t->cwd = vfs_find("/home", NULL);
    if (!t->cwd) t->cwd = vroot;
    t->curon = 1;
    t->alld = 1;
    w->priv = t;
    w->on_key = tkey;
    w->on_mouse = tmouse;
    w->on_tick = ttick;
    w->on_resize = tresize;
    w->on_close = tclose;
    w->ich = '>';
    w->icol = 0x15803D;
    T = t;
    ensure(t, 0);
    if (first_term && w->w >= 80 * CW) {
        tcolor(12);
        for (int i = 0; i < 11; i++) { tputs(logo[i]); rawc(t, '\n'); }
        rawc(t, '\n');
    }
    first_term = 0;
    tcolor(3);
    tputs("TAK Kernel " VER "\n");
    tcolor(7);
    tputs("type 'help' to see available commands\n\n");
    if (arg && *arg) {
        node *n = vfs_find(arg, NULL);
        if (n && n->dir) t->cwd = n;
    }
    new_prompt(t);
    render(t);
    return w;
}

void term_run(win *w, const char *cmd) {
    struct term *t = w->priv;
    char b[256];
    snprintf(b, sizeof b, "%s", cmd);
    exec(t, b);
}
