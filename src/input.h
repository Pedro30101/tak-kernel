#ifndef INPUT_H
#define INPUT_H
#include "tak.h"

enum { EV_KEY = 1, EV_MOUSE = 2 };
enum { M_L = 1, M_R = 2, M_M = 4 };
enum { MOD_CTRL = 1, MOD_ALT = 2, MOD_SHIFT = 4 };
enum {
    K_UP = 0x101, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_DEL, K_INS,
    K_F1 = 0x111, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10, K_F11, K_F12,
    K_SUPER = 0x120
};

struct ev {
    u8 type, down, mod, btn;
    u16 key;
    i16 x, y, wheel;
};

enum { LAYOUT_US, LAYOUT_ABNT2 };

void input_init(void);
int ev_pop(struct ev *e);
void ev_flush(void);
void kb_layout(int l);
int kb_layout_get(void);
int caps_on(void);
extern int mx, my;
void mouse_clip(int w, int h);

#endif
