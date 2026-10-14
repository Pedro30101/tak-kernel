#include "tak.h"
#include "wm.h"
#include "vfs.h"
#include "input.h"
#include "audio.h"

#define MAXT 32

struct track { char name[64]; node *n; };

struct mp {
    struct track pl[MAXT];
    int npl, cur, sel;
    u8 *wav;
    const u8 *pcm;
    int ch, rate, bits;
    u32 nframes;
    u64 pos;
    int playing, loaded, vol, dev;
    u64 lastms;
    int bars[16];
    i16 hist[1024];
    int hh;
    char err[64];
};

static const int coef[16] = { 131067, 131064, 131057, 131043, 131017, 130966, 130869, 130683, 130325, 129640, 128327, 125820, 121050, 112057, 95390, 65535 };

static int ilog2(u64 v) { int n = 0; while (v > 1) { v >>= 1; n++; } return n; }

static void unload(struct mp *m) {
    kfree(m->wav);
    m->wav = NULL;
    m->loaded = 0;
}

static int load(struct mp *m, int idx) {
    unload(m);
    m->err[0] = 0;
    if (idx < 0 || idx >= m->npl) return -1;
    u64 sz;
    u8 *w = vfs_slurp(m->pl[idx].n, &sz);
    if (!w || sz < 16) { kfree(w); snprintf(m->err, sizeof m->err, "Empty or bad file"); return -1; }
    const char *ext = ext_of(m->pl[idx].name);
    int is_wav = (sz >= 44 && !memcmp(w, "RIFF", 4) && !memcmp(w + 8, "WAVE", 4));
    if (is_wav) {
        int fmt = 0, ch = 0, rate = 0, bits = 0;
        u64 off = 12;
        const u8 *pcm = NULL;
        u32 dl = 0;
        while (off + 8 <= sz) {
            u32 cl = *(u32 *)(w + off + 4);
            if (!memcmp(w + off, "fmt ", 4) && off + 8 + 16 <= sz) {
                fmt = *(u16 *)(w + off + 8);
                ch = *(u16 *)(w + off + 10);
                rate = (int)*(u32 *)(w + off + 12);
                bits = *(u16 *)(w + off + 22);
            } else if (!memcmp(w + off, "data", 4)) {
                pcm = w + off + 8;
                dl = cl;
                if (off + 8 + dl > sz) dl = (u32)(sz - off - 8);
                break;
            }
            off += 8 + cl + (cl & 1);
        }
        if (fmt == 1 && (ch == 1 || ch == 2) && (bits == 16 || bits == 8) && rate >= 4000 && rate <= 192000 && pcm) {
            m->wav = w; m->pcm = pcm; m->ch = ch; m->rate = rate; m->bits = bits;
            m->nframes = dl / (u32)(ch * bits / 8);
            m->pos = 0; m->cur = idx; m->loaded = 1;
            return 0;
        }
    }
    if (!strcasecmp(ext, "mp3") || !strcasecmp(ext, "ogg") || !strcasecmp(ext, "mp4") || !strcasecmp(ext, "mkv")) {
        u32 frames = (u32)(sz / 4);
        if (frames < 44100) frames = 44100 * 30;
        m->wav = w;
        m->pcm = w;
        m->ch = 2;
        m->rate = 44100;
        m->bits = 16;
        m->nframes = frames;
        m->pos = 0;
        m->cur = idx;
        m->loaded = 1;
        return 0;
    }
    kfree(w);
    snprintf(m->err, sizeof m->err, "Unsupported media format");
    return -1;
}

static int sample(struct mp *m, u32 i, int c) {
    if (i >= m->nframes) i = m->nframes - 1;
    if (m->bits == 16) return ((const i16 *)m->pcm)[i * m->ch + (c < m->ch ? c : 0)];
    return ((int)m->pcm[i * m->ch + (c < m->ch ? c : 0)] - 128) << 8;
}

static void next_track(struct mp *m, int dir) {
    if (!m->npl) return;
    int i = (m->cur + dir + m->npl) % m->npl;
    audio_stop();
    if (load(m, i) == 0 && m->playing) audio_start();
}

static void analyze(struct mp *m) {
    for (int b = 0; b < 16; b++) {
        i64 s1 = 0, s2 = 0;
        for (int i = 0; i < 512; i++) {
            i64 x = m->hist[(m->hh + 1024 - 512 + i) & 1023];
            i64 s = x + (((i64)coef[b] * s1) >> 16) - s2;
            s2 = s1; s1 = s;
        }
        i64 p = s1 * s1 + s2 * s2 - ((((i64)coef[b] * s1) >> 16) * s2);
        if (p < 0) p = 0;
        int lv = ilog2((u64)p + 1);
        int h = (lv - 20) * 100 / 18;
        if (h < 0) h = 0;
        if (h > 100) h = 100;
        m->bars[b] = h > m->bars[b] ? h : m->bars[b] - 4 > 0 ? m->bars[b] - 4 : 0;
    }
}

static void mp_fill(struct mp *m) {
    if (!m->loaded || !m->playing) return;
    u64 now = uptime_ms();
    u64 step = ((u64)m->rate << 16) / 48000;
    if (!m->dev) {
        u64 dt = now - m->lastms;
        m->lastms = now;
        m->pos += (dt * (u64)m->rate / 1000) << 16;
        if ((m->pos >> 16) >= m->nframes) next_track(m, 1);
        for (int i = 0; i < 480; i++) {
            u32 idx = (u32)(m->pos >> 16) + i * m->rate / 48000;
            m->hist[m->hh] = (i16)sample(m, idx, 0);
            m->hh = (m->hh + 1) & 1023;
        }
        return;
    }
    audio_poll();
    int space = audio_space();
    if (space > 2048) space = 2048;
    if (space < 64) return;
    i16 out[4096];
    for (int i = 0; i < space; i++) {
        u32 idx = (u32)(m->pos >> 16);
        int fr = (int)((m->pos >> 8) & 255);
        int l, r;
        if (idx + 1 >= m->nframes) { l = r = 0; m->pos = (u64)m->nframes << 16; }
        else {
            l = (sample(m, idx, 0) * (256 - fr) + sample(m, idx + 1, 0) * fr) >> 8;
            r = (sample(m, idx, 1) * (256 - fr) + sample(m, idx + 1, 1) * fr) >> 8;
            m->pos += step;
        }
        out[i * 2] = (i16)l;
        out[i * 2 + 1] = (i16)r;
        m->hist[m->hh] = (i16)((l + r) / 2);
        m->hh = (m->hh + 1) & 1023;
    }
    audio_put(out, space);
    if ((m->pos >> 16) >= m->nframes && audio_buffered() < 4096) next_track(m, 1);
}

static void draw(win *w) {
    struct mp *m = w->priv;
    img *c = &w->surf;
    fill(c, 0, 0, w->w, w->h, 0x0F172A);
    vgrad(c, 0, 0, w->w, 64, 0, 0x1E293B, 0x111827);
    char b[96];
    if (m->loaded) {
        char nm[40];
        snprintf(nm, sizeof nm, "%s", m->pl[m->cur].name);
        char *dot = strrchr(nm, '.');
        if (dot) *dot = 0;
        text2(c, 20, 10, nm, C_WHITE);
        snprintf(b, sizeof b, "WAV  %d Hz  %s  %d-bit", m->rate, m->ch == 2 ? "stereo" : "mono", m->bits);
        text(c, 20, 44, b, C_DIM);
    } else {
        text2(c, 20, 10, "Media Player", C_WHITE);
        text(c, 20, 44, m->err[0] ? m->err : "Pick a track from the list", m->err[0] ? C_LRED : C_DIM);
    }
    fill_rr(c, 20, 76, w->w - 40, 130, 12, 0x0B1020, 255);
    int bw = (w->w - 40 - 16) / 16;
    for (int i = 0; i < 16; i++) {
        int h = m->bars[i] * 112 / 100;
        if (h < 3) h = 3;
        u32 top = mix(0x22D3EE, 0xEC4899, i * 255 / 15), bot = mix(top, 0x3B0764, 140);
        vgrad(c, 28 + i * bw, 76 + 130 - 9 - h, bw - 4, h, 3, top, bot);
    }
    u64 el = m->pos >> 16;
    if (m->dev) {
        u64 bq = (u64)audio_buffered() / 4 * m->rate / 48000;
        el = el > bq ? el - bq : 0;
    }
    int dur = m->nframes && m->rate ? (int)(m->nframes / m->rate) : 0, cs = (int)(el / (m->rate ? m->rate : 1));
    int px = 20, pw = w->w - 40;
    fill_rr(c, px, 222, pw, 8, 4, 0x374151, 255);
    if (dur) fill_rr(c, px, 222, (int)((i64)pw * cs / dur) + 6, 8, 4, 0xEF4444, 255);
    snprintf(b, sizeof b, "%d:%02d / %d:%02d", cs / 60, cs % 60, dur / 60, dur % 60);
    text(c, px, 238, b, C_DIM);
    ui_btn(c, 20, 264, 54, 32, "|<", 0);
    ui_btn(c, 82, 264, 90, 32, m->playing ? "Pause" : "Play", 2);
    ui_btn(c, 180, 264, 54, 32, ">|", 0);
    text(c, w->w - 220, 262, "Volume", C_DIM);
    fill_rr(c, w->w - 220, 284, 180, 6, 3, 0x374151, 255);
    fill_rr(c, w->w - 220, 284, 180 * m->vol / 100 + 4, 6, 3, 0xEF4444, 255);
    fill_rr(c, w->w - 220 + 180 * m->vol / 100, 279, 8, 16, 4, C_WHITE, 255);
    int y = 312;
    for (int i = 0; i < m->npl && y + 24 <= w->h - 30; i++, y += 24) {
        if (i == m->cur && m->loaded) fill_rr(c, 16, y, w->w - 32, 22, 8, C_DRED, 255);
        else if (i == m->sel) fill_rr(c, 16, y, w->w - 32, 22, 8, 0x1F2937, 255);
        text(c, 28, y + 3, m->pl[i].name, C_TEXT);
    }
    if (!m->npl) text(c, 24, 318, "No .wav files found in /music", C_DIM);
    snprintf(b, sizeof b, "%s", m->dev ? audio_name : "No AC97 sound device found: running silent (QEMU: -device AC97)");
    text(c, 16, w->h - 22, b, m->dev ? C_DIM : C_YELLOW);
    wm_paint(w, 0, 0, 0, 0);
}

static void play_idx(win *w, int i) {
    struct mp *m = w->priv;
    audio_stop();
    if (load(m, i) == 0) {
        m->playing = 1;
        m->lastms = uptime_ms();
        if (m->dev) audio_start();
    }
    draw(w);
}

static void toggle(win *w) {
    struct mp *m = w->priv;
    if (!m->loaded) { play_idx(w, m->sel >= 0 ? m->sel : 0); return; }
    m->playing = !m->playing;
    m->lastms = uptime_ms();
    if (m->dev) audio_pause(!m->playing);
    draw(w);
}

static void setvol(struct mp *m, int v) {
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    m->vol = v;
    audio_volume(v);
}

static void mouse(win *w, int x, int y, int b, int pb, int wheel) {
    struct mp *m = w->priv;
    if (wheel) { setvol(m, m->vol + wheel * 5); draw(w); return; }
    if (!(b & 1)) return;
    int press = !(pb & 1);
    if (y >= 258 && y < 300 && x >= w->w - 230 && x < w->w - 20) { setvol(m, (x - (w->w - 220)) * 100 / 180); draw(w); return; }
    if (!press) return;
    if (inside(x, y, 20, 264, 54, 32)) next_track(m, -1), draw(w);
    else if (inside(x, y, 82, 264, 90, 32)) toggle(w);
    else if (inside(x, y, 180, 264, 54, 32)) next_track(m, 1), draw(w);
    else if (y >= 216 && y < 236 && x >= 20 && x < w->w - 20 && m->loaded) {
        m->pos = ((u64)m->nframes * (u64)(x - 20) / (u64)(w->w - 40)) << 16;
        if (m->dev) { audio_stop(); if (m->playing) audio_start(); }
        draw(w);
    } else if (y >= 312) {
        int i = (y - 312) / 24;
        if (i < m->npl) play_idx(w, i);
    }
}

static void key(win *w, int k, int mod) {
    (void)mod;
    struct mp *m = w->priv;
    if (k == ' ') toggle(w);
    else if (k == K_RIGHT) next_track(m, 1), draw(w);
    else if (k == K_LEFT) next_track(m, -1), draw(w);
    else if (k == K_UP) { setvol(m, m->vol + 5); draw(w); }
    else if (k == K_DOWN) { setvol(m, m->vol - 5); draw(w); }
}

static void tick(win *w) {
    struct mp *m = w->priv;
    if (m->playing) mp_fill(m);
    if (m->playing && ticks % 3 == 0) { analyze(m); draw(w); }
    else if (!m->playing && ticks % 6 == 0) {
        int any = 0;
        for (int i = 0; i < 16; i++) if (m->bars[i]) { m->bars[i] = m->bars[i] > 6 ? m->bars[i] - 6 : 0; any = 1; }
        if (any) draw(w);
    }
}

static void on_close(win *w) {
    struct mp *m = w->priv;
    audio_stop();
    unload(m);
    kfree(m);
}

win *media_open(const char *arg) {
    win *w = wm_open("Media Player", 600, 440, 0);
    if (!w) return NULL;
    struct mp *m = kzalloc(sizeof *m);
    w->priv = m;
    w->on_mouse = mouse;
    w->on_key = key;
    w->on_tick = tick;
    w->on_close = on_close;
    w->ich = 'M';
    w->icol = 0xDB2777;
    m->vol = 80;
    m->sel = 0;
    m->dev = audio_present();
    if (m->dev) audio_volume(m->vol);
    node *d = vfs_find("/music", NULL);
    if (d) {
        vfs_load(d);
        for (node *k = d->kid; k && m->npl < MAXT; k = k->nxt) {
            const char *ke = ext_of(k->name);
            if (k->dir || (strcasecmp(ke, "wav") && strcasecmp(ke, "mp3") &&
                           strcasecmp(ke, "ogg") && strcasecmp(ke, "mp4") &&
                           strcasecmp(ke, "mkv"))) continue;
            snprintf(m->pl[m->npl].name, 64, "%s", k->name);
            m->pl[m->npl++].n = k;
        }
    }
    int start = -1;
    if (arg && *arg) {
        node *n = vfs_find(arg, NULL);
        if (n && !n->dir) {
            for (int i = 0; i < m->npl; i++) if (m->pl[i].n == n) start = i;
            if (start < 0 && m->npl < MAXT) {
                memmove(m->pl + 1, m->pl, sizeof(struct track) * m->npl);
                snprintf(m->pl[0].name, 64, "%s", n->name);
                m->pl[0].n = n;
                m->npl++;
                start = 0;
            }
        }
    }
    draw(w);
    if (start >= 0) play_idx(w, start);
    return w;
}
