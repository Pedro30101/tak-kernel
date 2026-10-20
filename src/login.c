#include "tak.h"
#include "auth.h"
#include "gfx.h"
#include "input.h"
#include "wm.h"

#define FIELD_NAME 0
#define FIELD_PASSWORD 1
#define FIELD_CONFIRM 2
#define LOGIN_COOLDOWN (30ULL * 100)

void settings_restore(void);

struct field_rect { int x, y, w, h; };
struct login_state {
    int mode, focus, failures;
    u64 blocked_until;
    char username[32], password[10000], confirm[10000], message[96];
    struct field_rect fields[3], submit, power;
};

static win *login_window;

enum { LOGIN_SETUP, LOGIN_PASSWORD, LOGIN_STORAGE_ERROR };

static void field(img *c, struct login_state *s, int id, int x, int y, int width,
                  const char *label, const char *value, int secret, int active) {
    struct field_rect *r = &s->fields[id];
    r->x = x; r->y = y; r->w = width; r->h = 54;
    text(c, x, y, label, active ? C_WHITE : C_DIM);
    fill_rr(c, x, y + 18, width, 36, 8, 0x0D1421, 255);
    box(c, x, y + 18, width, 36, active ? C_LRED : 0x3B465A);
    if (secret) {
        size_t n = strlen(value);
        int visible = (width - 24) / 8;
        if (visible > 0 && n > (size_t)visible) n = (size_t)visible;
        for (size_t i = 0; i < n; i++) glyph(c, x + 12 + (int)i * 8, y + 28, '*', C_TEXT, -1, 1);
    } else {
        text(c, x + 12, y + 28, value[0] ? value : "Type a username", value[0] ? C_TEXT : C_DIM);
    }
}

static void draw(win *w) {
    struct login_state *s = w->priv;
    img *c = &w->surf;
    int cw = w->w < 560 ? w->w - 32 : 520;
    int ch = s->mode == LOGIN_SETUP ? 500 : 420;
    if (cw < 300) cw = w->w;
    if (ch > w->h - 24) ch = w->h - 24;
    int x = (w->w - cw) / 2, y = (w->h - ch) / 2;
    fill(c, 0, 0, w->w, w->h, 0x101827);
    vgrad(c, 0, 0, w->w, w->h, 0, 0x1C2B43, 0x0C111B);
    fill_rr(c, w->w / 10, w->h / 8, w->w / 3, w->h / 2, 72, 0xB91C1C, 35);
    fill_rr(c, w->w * 3 / 5, w->h / 3, w->w / 3, w->h / 2, 88, 0x2563EB, 24);
    soft_shadow(c, x, y + 8, cw, ch, 20, 24, 150);
    fill_rr(c, x, y, cw, ch, 20, 0x192334, 245);
    glass_edge(c, x, y, cw, ch, 20, 180, 70);

    fill_rr(c, x + 32, y + 30, 42, 42, 12, C_RED, 255);
    glyph(c, x + 44, y + 42, 'A', C_WHITE, -1, 1);

    if (s->mode == LOGIN_STORAGE_ERROR) {
        text2(c, x + 32, y + 96, "Account storage unavailable", C_WHITE);
        text(c, x + 32, y + 140, s->message, C_LRED);
        text(c, x + 32, y + 174, "The installed volume must be readable before login.", C_DIM);
        s->submit = (struct field_rect){ x + 32, y + ch - 96, cw - 64, 44 };
        ui_btn(c, s->submit.x, s->submit.y, s->submit.w, s->submit.h, "Retry", 0);
        s->power = (struct field_rect){ x + 32, y + ch - 48, 100, 28 };
        text(c, s->power.x, s->power.y + 6, "Power off", C_DIM);
    } else if (s->mode == LOGIN_SETUP) {
        text2(c, x + 32, y + 90, "Welcome to Tak Astros", C_WHITE);
        text(c, x + 32, y + 118, "Create your local account to finish setting up this PC.", C_DIM);
        int fw = cw - 64;
        field(c, s, FIELD_NAME, x + 32, y + 152, fw, "Username", s->username, 0, s->focus == FIELD_NAME);
        field(c, s, FIELD_PASSWORD, x + 32, y + 228, fw, "Password", s->password, 1, s->focus == FIELD_PASSWORD);
        field(c, s, FIELD_CONFIRM, x + 32, y + 304, fw, "Confirm password", s->confirm, 1, s->focus == FIELD_CONFIRM);
        s->submit = (struct field_rect){ x + 32, y + ch - 104, fw, 42 };
        ui_btn(c, s->submit.x, s->submit.y, s->submit.w, s->submit.h, "Create account", 0);
        text(c, x + 32, y + ch - 48, "Password: 3-9999 characters. Input is hidden.", C_DIM);
        s->power = (struct field_rect){ 0, 0, 0, 0 };
    } else {
        text2(c, x + 32, y + 96, "Welcome back", C_WHITE);
        text(c, x + 32, y + 128, s->username, C_LRED);
        int fw = cw - 64;
        field(c, s, FIELD_PASSWORD, x + 32, y + 174, fw, "Password", s->password, 1, 1);
        s->submit = (struct field_rect){ x + 32, y + 260, fw, 42 };
        ui_btn(c, s->submit.x, s->submit.y, s->submit.w, s->submit.h,
               ticks < s->blocked_until ? "Please wait..." : "Sign in", 0);
        text(c, x + 32, y + 322, s->message[0] ? s->message : "Enter your password to unlock the desktop.", s->message[0] ? C_YELLOW : C_DIM);
        s->power = (struct field_rect){ x + 32, y + ch - 54, 100, 28 };
        text(c, s->power.x, s->power.y + 6, "Power off", C_DIM);
        s->fields[FIELD_NAME] = (struct field_rect){ 0, 0, 0, 0 };
        s->fields[FIELD_CONFIRM] = (struct field_rect){ 0, 0, 0, 0 };
    }
    wm_paint(w, 0, 0, 0, 0);
}

static void finish_login(win *w) {
    struct login_state *s = w->priv;
    if (!auth_home_dir()) {
        snprintf(s->message, sizeof s->message, "Could not open this user's persistent home folder.");
        draw(w);
        return;
    }
    settings_restore();
    auth_wipe(s->password, sizeof s->password);
    auth_wipe(s->confirm, sizeof s->confirm);
    wm_session_unlock(w);
    login_window = NULL;
    wm_close(w);
}

static void submit(win *w) {
    struct login_state *s = w->priv;
    if (s->mode == LOGIN_STORAGE_ERROR) {
        int state = auth_init();
        if (state == AUTH_SETUP) {
            s->mode = LOGIN_SETUP;
            s->focus = FIELD_NAME;
            s->message[0] = 0;
        } else if (state == AUTH_LOGIN) {
            s->mode = LOGIN_PASSWORD;
            auth_username(s->username, sizeof s->username);
            s->focus = FIELD_PASSWORD;
            s->message[0] = 0;
        } else {
            snprintf(s->message, sizeof s->message, "The account file is missing, damaged, or unreadable.");
        }
    } else if (s->mode == LOGIN_SETUP) {
        size_t user_len = strlen(s->username);
        if (!user_len || user_len > 31 || !strcmp(s->username, ".") || !strcmp(s->username, "..")) {
            snprintf(s->message, sizeof s->message, "Enter a username using letters, numbers, . _ or -.");
            s->focus = FIELD_NAME;
        } else if (strlen(s->password) < 12) {
            snprintf(s->message, sizeof s->message, "Choose a password that is at least 12 characters.");
            s->focus = FIELD_PASSWORD;
        } else if (strcmp(s->password, s->confirm)) {
            snprintf(s->message, sizeof s->message, "The passwords do not match. Enter them again.");
            auth_wipe(s->password, sizeof s->password);
            auth_wipe(s->confirm, sizeof s->confirm);
            s->focus = FIELD_PASSWORD;
        } else {
            int r = auth_create(s->username, s->password);
            if (r == 0) {
                auth_username(s->username, sizeof s->username);
                finish_login(w);
                return;
            }
            if (r == -2)
                snprintf(s->message, sizeof s->message, "Secure CPU random-number support is required to create an account.");
            else
                snprintf(s->message, sizeof s->message, "Could not safely save the account to the installed volume.");
            auth_wipe(s->password, sizeof s->password);
            auth_wipe(s->confirm, sizeof s->confirm);
            s->focus = FIELD_PASSWORD;
        }
    } else {
        if (ticks < s->blocked_until) {
            snprintf(s->message, sizeof s->message, "Too many attempts. Wait before trying again.");
        } else if (auth_verify(s->username, s->password) == 0) {
            finish_login(w);
            return;
        } else {
            s->failures++;
            auth_wipe(s->password, sizeof s->password);
            if (s->failures >= 5) {
                s->failures = 0;
                s->blocked_until = ticks + LOGIN_COOLDOWN;
                snprintf(s->message, sizeof s->message, "Too many attempts. Please wait 30 seconds.");
            } else {
                snprintf(s->message, sizeof s->message, "Incorrect password. Try again.");
            }
        }
    }
    draw(w);
}

static void key(win *w, int k, int mod) {
    (void)mod;
    struct login_state *s = w->priv;
    if (k == '\t') {
        int count = s->mode == LOGIN_SETUP ? 3 : 1;
        s->focus = (s->focus + 1) % count;
        if (s->mode == LOGIN_PASSWORD) s->focus = FIELD_PASSWORD;
        draw(w);
        return;
    }
    if (k == '\n') {
        if (s->mode == LOGIN_SETUP && s->focus < FIELD_CONFIRM) {
            s->focus++;
            draw(w);
        } else submit(w);
        return;
    }
    if (k == '\b') {
        char *dst = s->focus == FIELD_NAME ? s->username :
                    s->focus == FIELD_PASSWORD ? s->password : s->confirm;
        size_t n = strlen(dst);
        if (n) dst[n - 1] = 0;
        draw(w);
        return;
    }
    if (k < 0x20 || k > 0x7E || s->mode == LOGIN_STORAGE_ERROR) return;
    char *dst = s->focus == FIELD_NAME ? s->username :
                s->focus == FIELD_PASSWORD ? s->password : s->confirm;
    size_t n = strlen(dst), cap = s->focus == FIELD_NAME ? sizeof s->username - 1 : sizeof s->password - 1;
    if (n >= cap) return;
    if (s->focus == FIELD_NAME) {
        int valid = (k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z') ||
                    (k >= '0' && k <= '9') || k == '_' || k == '-' || k == '.';
        if (!valid) return;
        dst[n] = (char)k;
        dst[n + 1] = 0;
    } else {
        dst[n] = (char)k;
        dst[n + 1] = 0;
    }
    draw(w);
}

static void mouse(win *w, int x, int y, int b, int pb, int wheel) {
    (void)wheel;
    if (!(b & 1) || (pb & 1)) return;
    struct login_state *s = w->priv;
    if (inside(x, y, s->submit.x, s->submit.y, s->submit.w, s->submit.h)) {
        submit(w);
        return;
    }
    if (inside(x, y, s->power.x, s->power.y, s->power.w, s->power.h)) {
        sys_poweroff();
        return;
    }
    int fields[] = { FIELD_NAME, FIELD_PASSWORD, FIELD_CONFIRM };
    int count = s->mode == LOGIN_SETUP ? 3 : s->mode == LOGIN_PASSWORD ? 1 : 0;
    for (int i = 0; i < count; i++) {
        int id = s->mode == LOGIN_PASSWORD ? FIELD_PASSWORD : fields[i];
        struct field_rect r = s->fields[id];
        if (inside(x, y, r.x, r.y + 18, r.w, r.h - 18)) {
            s->focus = id;
            draw(w);
            return;
        }
    }
}

static void tick(win *w) {
    struct login_state *s = w->priv;
    if (s->mode == LOGIN_PASSWORD && s->blocked_until && ticks >= s->blocked_until) {
        s->blocked_until = 0;
        s->message[0] = 0;
        draw(w);
    }
}

static void close(win *w) {
    if (login_window == w) login_window = NULL;
    struct login_state *s = w->priv;
    auth_wipe(s->password, sizeof s->password);
    auth_wipe(s->confirm, sizeof s->confirm);
    kfree(s);
}

int login_start(void) {
    if (login_window) return -1;
    win *w = wm_open("Astros Login", sw, sh, WF_LOCKSCREEN | WF_NOMIN | WF_NOMAX);
    if (!w) return -1;
    struct login_state *s = kzalloc(sizeof *s);
    if (!s) { wm_close(w); return -1; }
    w->priv = s;
    w->on_key = key;
    w->on_mouse = mouse;
    w->on_tick = tick;
    w->on_close = close;
    w->ich = 'A';
    w->icol = C_RED;
    int state = auth_init();
    if (state == AUTH_SETUP) {
        s->mode = LOGIN_SETUP;
        s->focus = FIELD_NAME;
    } else if (state == AUTH_LOGIN) {
        s->mode = LOGIN_PASSWORD;
        s->focus = FIELD_PASSWORD;
        auth_username(s->username, sizeof s->username);
    } else {
        s->mode = LOGIN_STORAGE_ERROR;
        snprintf(s->message, sizeof s->message, "Connect the installed volume and retry.");
    }
    login_window = w;
    draw(w);
    wm_session_lock(w);
    return 0;
}
