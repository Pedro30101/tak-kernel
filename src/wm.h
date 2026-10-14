#ifndef WM_H
#define WM_H
#include "tak.h"
#include "gfx.h"
#include "input.h"

struct node;

#define TBW 72
#define TB 34
#define BD 7

enum { WF_RESIZE = 1, WF_USER = 2, WF_MAX = 4, WF_MIN = 8, WF_NOMIN = 16, WF_NOMAX = 32, WF_LOCKSCREEN = 64 };
enum { UE_KEY = 1, UE_MOUSE = 2, UE_CLOSE = 3, UE_RESIZE = 4 };

struct uev {
    i32 type, a, b, c, d;
};

typedef struct win win;
struct win {
    int x, y, w, h;
    int rx, ry, rw, rh;
    img surf;
    char title[48];
    int flags;
    int id, owner;
    int ich;
    u32 icol;
    void *priv;
    void (*on_key)(win *w, int key, int mod);
    void (*on_mouse)(win *w, int x, int y, int btn, int prev, int wheel);
    void (*on_resize)(win *w, int nw, int nh);
    void (*on_close)(win *w);
    void (*on_tick)(win *w);
    struct uev q[64];
    int qh, qt;
};

typedef struct {
    const char *name;
    char letter;
    u32 col;
    win *(*open)(const char *arg);
    int flags;
} app_t;

enum { A_LIVE = 1, A_FULL = 2 };

extern app_t apps[];
extern int napps;
extern win *focus;

win *wm_open(const char *title, int w, int h, int flags);
void wm_close(win *w);
void wm_paint(win *w, int x, int y, int w2, int h2);
void wm_title(win *w, const char *t);
void wm_focus(win *w);
void wm_resize(win *w, int nw, int nh);
void wm_dirty(int x, int y, int w, int h);
void wm_wallpaper(img *src);
void wm_run(void);
void wm_init(void);
int wm_post(win *w, struct uev *e);
int wm_pop(win *w, struct uev *e);
win *wm_byid(int id);
int wm_count(void);
void wm_close_owner(int pid);
void proc_wake(int pid);
int proc_kill(int pid);
int proc_spawn(const char *path, const char *arg, void *term);
win *wm_nth(int i);
void wm_menu_refresh(void);
void wm_session_lock(win *w);
void wm_session_unlock(win *w);
void wm_tbpos(int p);
void wm_relayout(void);
int wm_tbpos_get(void);

win *app_open(const char *name, const char *arg);
void open_file(struct node *n);

void ui_btn(img *c, int x, int y, int w, int h, const char *s, int st);
void ui_field(img *c, int x, int y, int w, int h, const char *s, int focus);
void ui_vscroll(img *c, int x, int y, int h, int pos, int total, int vis);
int inside(int px, int py, int x, int y, int w, int h);

int rtc_get(int *h, int *m, int *s, int *d, int *mo, int *y);
void sys_reboot(void);
void sys_poweroff(void);
void sys_halt(void);

void proc_yield(void);
int proc_runnable(void);

#endif
