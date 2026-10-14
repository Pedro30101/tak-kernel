#include "tak.h"
#include "wm.h"
#include "proc.h"
#include "input.h"

extern u64 idle_ticks, busy_ticks;
u64 proc_cpu(int pid);

#define NR 64

struct row {
    int pid, wid;
    char name[40];
    int state;
    u64 kb;
    int cpu;
    u64 last;
};

struct tm {
    int tab, sel;
    struct row r[NR];
    int n;
    u64 lastb, lasti, lastt;
    int hist[60];
    int hov;
};

static const char *sn[] = { "Ready", "Running", "Waiting", "Sleeping", "Dead" };

static void cb(int pid, const char *name, int st, u64 kb, void *ctx) {
    struct tm *t = ctx;
    if (t->n >= NR) return;
    struct row *r = &t->r[t->n++];
    memset(r, 0, sizeof *r);
    r->pid = pid;
    snprintf(r->name, sizeof r->name, "%s", pid ? name : "tak astros (shell & kernel)");
    r->state = st;
    r->kb = kb;
}

static void collect(struct tm *t, u64 dt) {
    struct row old[NR];
    int on = t->n;
    memcpy(old, t->r, sizeof old);
    t->n = 0;
    proc_list(cb, t);
    for (int i = 0; i < t->n; i++) {
        if (!t->r[i].pid) continue;
        u64 c = proc_cpu(t->r[i].pid);
        u64 prev = 0;
        for (int j = 0; j < on; j++) if (old[j].pid == t->r[i].pid) prev = old[j].last;
        t->r[i].last = c;
        t->r[i].cpu = dt ? (int)((c - prev) * 100 / dt) : 0;
    }
    for (int i = 0; i < wm_count() && t->n < NR; i++) {
        win *w = wm_nth(i);
        if (w->flags & WF_USER) continue;
        struct row *r = &t->r[t->n++];
        memset(r, 0, sizeof *r);
        r->pid = -1;
        r->wid = w->id;
        snprintf(r->name, sizeof r->name, "%s", w->title);
        r->state = 1;
    }
}

static void draw(win *w) {
    struct tm *t = w->priv;
    img *c = &w->surf;
    fill(c, 0, 0, w->w, w->h, C_PANEL);
    fill(c, 0, 0, w->w, 38, 0x111827);
    ui_btn(c, 8, 6, 110, 26, "Processes", t->tab == 0 ? 2 : 0);
    ui_btn(c, 124, 6, 110, 26, "Performance", t->tab == 1 ? 2 : 0);
    if (t->tab == 0) {
        fill(c, 0, 38, w->w, 24, 0x1F2937);
        text(c, 12, 42, "Name", C_DIM);
        text(c, w->w - 330, 42, "PID", C_DIM);
        text(c, w->w - 270, 42, "State", C_DIM);
        text(c, w->w - 180, 42, "CPU", C_DIM);
        text(c, w->w - 120, 42, "Memory", C_DIM);
        int rows = (w->h - 62 - 40) / 24;
        for (int i = 0; i < t->n && i < rows; i++) {
            struct row *r = &t->r[i];
            int y = 62 + i * 24;
            if (i == t->sel) fill(c, 0, y, w->w, 24, C_DRED);
            char b[40];
            text(c, 12, y + 4, r->name, C_TEXT);
            if (r->pid >= 0) { snprintf(b, sizeof b, "%d", r->pid); text(c, w->w - 330, y + 4, b, C_DIM); }
            else text(c, w->w - 330, y + 4, "app", C_DIM);
            text(c, w->w - 270, y + 4, sn[r->state], C_DIM);
            snprintf(b, sizeof b, "%d%%", r->cpu);
            text(c, w->w - 180, y + 4, b, C_DIM);
            if (r->kb) { snprintf(b, sizeof b, "%lu KB", r->kb); text(c, w->w - 120, y + 4, b, C_DIM); }
        }
        fill(c, 0, w->h - 40, w->w, 40, 0x111827);
        ui_btn(c, w->w - 120, w->h - 34, 108, 28, "End task", 0);
        char b[64];
        snprintf(b, sizeof b, "%d tasks", t->n);
        text(c, 12, w->h - 28, b, C_DIM);
    } else {
        int cpu = t->hist[59];
        char b[96];
        snprintf(b, sizeof b, "CPU  %d%%", cpu);
        text2(c, 20, 52, b, C_WHITE);
        int gx = 20, gy = 96, gw = w->w - 40, gh = 110;
        fill(c, gx, gy, gw, gh, 0x0B1020);
        box(c, gx, gy, gw, gh, C_RED);
        for (int i = 0; i < 60; i++) {
            int bw = gw / 60;
            int h = t->hist[i] * (gh - 2) / 100;
            fill(c, gx + 1 + i * bw, gy + gh - 1 - h, bw - 1, h, C_LRED);
        }
        u64 tot = mem_total_kb(), fr = mem_free_kb(), hf = heap_free_kb();
        int y = 232;
        snprintf(b, sizeof b, "Frames used: %lu / %lu MB", (tot - fr) / 1024, tot / 1024);
        text(c, 20, y, b, C_TEXT);
        fill(c, 20, y + 22, gw, 16, 0x0B1020);
        fill(c, 20, y + 22, (int)((u64)gw * (tot - fr) / (tot ? tot : 1)), 16, C_BLUE);
        snprintf(b, sizeof b, "Kernel heap free: %lu MB", hf / 1024);
        text(c, 20, y + 54, b, C_TEXT);
        u64 up = uptime_ms() / 1000;
        snprintf(b, sizeof b, "Uptime: %lu min %lu s     Windows: %d", up / 60, up % 60, wm_count());
        text(c, 20, y + 84, b, C_DIM);
    }
    wm_paint(w, 0, 0, 0, 0);
}

static void tick(win *w) {
    struct tm *t = w->priv;
    if (ticks % 50 != 0) return;
    u64 b = busy_ticks, i = idle_ticks, tt = ticks;
    u64 db = b - t->lastb, di = i - t->lasti, dt = tt - t->lastt;
    int cpu = (db + di) ? (int)(db * 100 / (db + di)) : 0;
    if (t->lastt) {
        memmove(t->hist, t->hist + 1, 59 * sizeof(int));
        t->hist[59] = cpu;
    }
    t->lastb = b; t->lasti = i; t->lastt = tt;
    collect(t, dt);
    if (t->sel >= t->n) t->sel = t->n - 1;
    draw(w);
}

static void end_task(win *w) {
    struct tm *t = w->priv;
    if (t->sel < 0 || t->sel >= t->n) return;
    struct row *r = &t->r[t->sel];
    if (r->pid > 0) proc_kill(r->pid);
    else if (r->pid < 0) {
        win *x = wm_byid(r->wid);
        if (x && x != w) wm_close(x);
    }
    collect(t, 0);
    if (t->sel >= t->n) t->sel = t->n - 1;
    draw(w);
}

static void mouse(win *w, int x, int y, int b, int pb, int wheel) {
    (void)wheel;
    struct tm *t = w->priv;
    if (!(b & 1) || (pb & 1)) return;
    if (y < 38) {
        if (inside(x, y, 8, 6, 110, 26)) t->tab = 0;
        else if (inside(x, y, 124, 6, 110, 26)) t->tab = 1;
        draw(w);
        return;
    }
    if (t->tab != 0) return;
    if (y >= w->h - 40) { if (inside(x, y, w->w - 120, w->h - 34, 108, 28)) end_task(w); return; }
    int i = (y - 62) / 24;
    if (y >= 62 && i < t->n) { t->sel = i; draw(w); }
}

static void key(win *w, int k, int mod) {
    (void)mod;
    struct tm *t = w->priv;
    if (k == K_DEL) end_task(w);
    else if (k == K_UP && t->sel > 0) { t->sel--; draw(w); }
    else if (k == K_DOWN && t->sel < t->n - 1) { t->sel++; draw(w); }
}

static void on_close(win *w) { kfree(w->priv); }

win *taskmgr_open(const char *arg) {
    (void)arg;
    win *w = wm_open("Task Manager", 640, 460, WF_RESIZE);
    if (!w) return NULL;
    struct tm *t = kzalloc(sizeof *t);
    w->priv = t;
    w->on_mouse = mouse;
    w->on_key = key;
    w->on_tick = tick;
    w->on_close = on_close;
    w->ich = 'M';
    w->icol = 0xDC2626;
    t->sel = -1;
    collect(t, 0);
    draw(w);
    return w;
}
