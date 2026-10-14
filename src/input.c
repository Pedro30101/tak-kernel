#include "tak.h"
#include "input.h"

#define EVN 256

static struct ev q[EVN];
static volatile int qh, qt;

int mx, my;
static int mw = 1024, mh = 768;
static int mbtn;
static int layout;
static int shift, ctrl, alt, caps, caps_held, ext, dead;
static int has_wheel;
static u8 mpk[4], mpi;

enum { D_ACUTE = 1, D_GRAVE, D_TILDE, D_CIRC, D_DIAE };

void mouse_clip(int w, int h) { mw = w; mh = h; mx = w / 2; my = h / 2; }
void kb_layout(int l) { layout = l; dead = 0; }
int kb_layout_get(void) { return layout; }
int caps_on(void) { return caps; }

static void push(struct ev *e) {
    int n = (qt + 1) % EVN;
    if (e->type == EV_MOUSE && qh != qt) {
        struct ev *l = &q[(qt + EVN - 1) % EVN];
        if (l->type == EV_MOUSE && l->btn == e->btn && !e->wheel && !l->wheel) {
            l->x = e->x;
            l->y = e->y;
            return;
        }
    }
    if (n == qh) return;
    q[qt] = *e;
    qt = n;
}

int ev_pop(struct ev *e) {
    u64 f = irqsave();
    int r = 0;
    if (qh != qt) {
        *e = q[qh];
        qh = (qh + 1) % EVN;
        r = 1;
    }
    irqrest(f);
    return r;
}

void ev_flush(void) {
    u64 f = irqsave();
    qh = qt;
    irqrest(f);
}

static int mod(void) { return (ctrl ? MOD_CTRL : 0) | (alt ? MOD_ALT : 0) | (shift ? MOD_SHIFT : 0); }

static void key_out(int k, int down) {
    struct ev e = { EV_KEY, (u8)down, (u8)mod(), 0, (u16)k, 0, 0, 0 };
    push(&e);
}

static u8 shifted(u8 sc, u8 c) {
    switch (sc) {
    case 0x02: return '!'; case 0x03: return '@'; case 0x04: return '#'; case 0x05: return '$';
    case 0x06: return '%'; case 0x07: return '^'; case 0x08: return '&'; case 0x09: return '*';
    case 0x0A: return '('; case 0x0B: return ')'; case 0x0C: return '_'; case 0x0D: return '+';
    case 0x1A: return '{'; case 0x1B: return '}'; case 0x2B: return '|'; case 0x27: return ':';
    case 0x28: return '"'; case 0x33: return '<'; case 0x34: return '>'; case 0x35: return '?';
    case 0x29: return '~';
    default: return c;
    }
}

static u8 xlate(u8 sc, int sh) {
    static const u8 us[128] = {
        [0x01] = 27,
        [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5', [0x07] = '6',
        [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
        [0x0E] = '\b', [0x0F] = '\t',
        [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't', [0x15] = 'y',
        [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
        [0x1C] = '\n', [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
        [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';', [0x28] = '\'',
        [0x29] = '`', [0x2B] = '\\', [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
        [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
        [0x39] = ' '
    };
    if (layout == LAYOUT_ABNT2) {
        switch (sc) {
        case 0x07: return sh ? D_DIAE : '6';
        case 0x1A: return sh ? D_GRAVE : D_ACUTE;
        case 0x1B: return sh ? '{' : '[';
        case 0x27: return sh ? 128 : 135;
        case 0x28: return sh ? D_CIRC : D_TILDE;
        case 0x29: return sh ? '"' : '\'';
        case 0x2B: return sh ? '}' : ']';
        case 0x35: return sh ? ':' : ';';
        case 0x56: return sh ? '|' : '\\';
        case 0x73: return sh ? '?' : '/';
        default: break;
        }
    }
    u8 c = us[sc];
    if (sh && c) c = shifted(sc, c);
    return c;
}

static u8 compose(u8 d, u8 c) {
    switch (d) {
    case D_ACUTE:
        switch (c) {
        case 'a': return 160; case 'e': return 130; case 'i': return 161; case 'o': return 162; case 'u': return 163;
        case 'A': return 181; case 'E': return 144; case 'I': return 214; case 'O': return 224; case 'U': return 233;
        } break;
    case D_GRAVE:
        switch (c) {
        case 'a': return 133; case 'e': return 138; case 'i': return 141; case 'o': return 149; case 'u': return 151;
        case 'A': return 183; case 'E': return 212; case 'I': return 222; case 'O': return 227; case 'U': return 235;
        } break;
    case D_TILDE:
        switch (c) {
        case 'a': return 198; case 'o': return 228; case 'n': return 164;
        case 'A': return 199; case 'O': return 229; case 'N': return 165;
        } break;
    case D_CIRC:
        switch (c) {
        case 'a': return 131; case 'e': return 136; case 'i': return 140; case 'o': return 147; case 'u': return 150;
        case 'A': return 182; case 'E': return 210; case 'I': return 215; case 'O': return 226; case 'U': return 234;
        } break;
    case D_DIAE:
        switch (c) {
        case 'a': return 132; case 'e': return 137; case 'i': return 139; case 'o': return 148; case 'u': return 129;
        case 'A': return 142; case 'E': return 211; case 'I': return 216; case 'O': return 153; case 'U': return 154;
        } break;
    }
    return 0;
}

static u8 dead_char(u8 d) {
    switch (d) {
    case D_ACUTE: return '\'';
    case D_GRAVE: return '`';
    case D_TILDE: return '~';
    case D_CIRC: return '^';
    default: return '"';
    }
}

static void ext_key(u8 sc, int down) {
    int k = 0;
    switch (sc) {
    case 0x48: k = K_UP; break;
    case 0x50: k = K_DOWN; break;
    case 0x4B: k = K_LEFT; break;
    case 0x4D: k = K_RIGHT; break;
    case 0x47: k = K_HOME; break;
    case 0x4F: k = K_END; break;
    case 0x49: k = K_PGUP; break;
    case 0x51: k = K_PGDN; break;
    case 0x53: k = K_DEL; break;
    case 0x52: k = K_INS; break;
    case 0x1D: ctrl = down; return;
    case 0x38: alt = down; return;
    case 0x5B: case 0x5C: k = K_SUPER; break;
    case 0x1C: k = '\n'; break;
    case 0x35: k = '/'; break;
    }
    if (k) key_out(k, down);
}

static void kbd_byte(u8 sc) {
    if (sc == 0xE0) { ext = 1; return; }
    int down = !(sc & 0x80);
    sc &= 0x7F;
    if (ext) { ext = 0; ext_key(sc, down); return; }
    if (sc == 0x2A || sc == 0x36) { shift = down; return; }
    if (sc == 0x1D) { ctrl = down; return; }
    if (sc == 0x38) { alt = down; return; }
    if (sc == 0x3A) {
        if (down && !caps_held) caps = !caps;
        caps_held = down;
        return;
    }
    if (sc >= 0x3B && sc <= 0x44) { key_out(K_F1 + (sc - 0x3B), down); return; }
    if (sc == 0x57) { key_out(K_F11, down); return; }
    if (sc == 0x58) { key_out(K_F12, down); return; }
    u8 c = xlate(sc, shift);
    if (!c) return;
    if (!down) { if (c > 5) key_out(c, 0); return; }
    if (layout == LAYOUT_ABNT2 && c >= D_ACUTE && c <= D_DIAE) { dead = c; return; }
    if (((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) && caps != shift)
        c = (c >= 'a' && c <= 'z') ? c - 32 : c + 32;
    if (dead) {
        u8 r = compose(dead, c);
        if (r) c = r;
        else {
            key_out(dead_char(dead), 1);
            if (c == ' ') { dead = 0; return; }
        }
        dead = 0;
    }
    key_out(c, 1);
}

static void mouse_byte(u8 b) {
    int need = has_wheel ? 4 : 3;
    if (mpi == 0 && !(b & 8)) return;
    mpk[mpi++] = b;
    if (mpi < need) return;
    mpi = 0;
    if (mpk[0] & 0xC0) return;
    int dx = (int)(i8)mpk[1], dy = -(int)(i8)mpk[2];
    int wh = has_wheel ? -(int)(i8)((mpk[3] & 8) ? (mpk[3] | 0xF0) : (mpk[3] & 7)) : 0;
    mx += dx; my += dy;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx >= mw) mx = mw - 1;
    if (my >= mh) my = mh - 1;
    mbtn = mpk[0] & 7;
    struct ev e = { EV_MOUSE, 0, (u8)mod(), (u8)mbtn, 0, (i16)mx, (i16)my, (i16)wh };
    push(&e);
}

static void drain(void) {
    for (int n = 0; n < 64; n++) {
        u8 st = inb(0x64);
        if (!(st & 1)) break;
        u8 d = inb(0x60);
        if (st & 0x20) mouse_byte(d);
        else kbd_byte(d);
    }
}

static int wait_in(void) {
    for (int i = 0; i < 100000; i++) { if (!(inb(0x64) & 2)) return 1; }
    return 0;
}

static int wait_out(void) {
    for (int i = 0; i < 100000; i++) { if (inb(0x64) & 1) return 1; }
    return 0;
}

static int mcmd(u8 c) {
    if (!wait_in()) return -1;
    outb(0x64, 0xD4);
    if (!wait_in()) return -1;
    outb(0x60, c);
    for (int t = 0; t < 4; t++) {
        if (!wait_out()) return -1;
        u8 r = inb(0x60);
        if (r == 0xFA) return 0;
        if (r == 0xFE) return -1;
    }
    return -1;
}

void input_init(void) {
    wait_in(); outb(0x64, 0xAD);
    wait_in(); outb(0x64, 0xA7);
    while (inb(0x64) & 1) inb(0x60);
    wait_in(); outb(0x64, 0x20);
    wait_out();
    u8 cfg = inb(0x60);
    cfg |= 3;
    cfg &= ~0x30;
    wait_in(); outb(0x64, 0x60);
    wait_in(); outb(0x60, cfg);
    wait_in(); outb(0x64, 0xAE);
    wait_in(); outb(0x64, 0xA8);
    mcmd(0xF6);
    mcmd(0xF3); mcmd(200);
    mcmd(0xF3); mcmd(100);
    mcmd(0xF3); mcmd(80);
    if (mcmd(0xF2) == 0 && wait_out() && inb(0x60) == 3) has_wheel = 1;
    mcmd(0xF3); mcmd(100);
    mcmd(0xF4);
    while (inb(0x64) & 1) inb(0x60);
    irq_set(1, drain);
    irq_set(12, drain);
    klog("input: wheel=%d\n", has_wheel);
}
