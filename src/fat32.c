#include "tak.h"
#include "disk.h"
#include "vfs.h"

int rtc_get(int *h, int *m, int *s, int *d, int *mo, int *y);

struct fat {
    int d;
    u64 start;
    u32 spc, rsvd, nfats, fatsz, rootclus, tot, nclus, dstart, cs, hint;
    u32 *fat;
    char label[12];
};

struct dir {
    u32 *chain;
    int nch;
    u8 *buf;
    u32 len;
};

#define EOC 0x0FFFFFFF

static int rd(struct fat *f, u64 lba, u32 n, void *b) { return disk_read(f->d, f->start + lba, n, b); }
static int wr(struct fat *f, u64 lba, u32 n, const void *b) { return disk_write(f->d, f->start + lba, n, b); }
static u64 clba(struct fat *f, u32 c) { return f->dstart + (u64)(c - 2) * f->spc; }
static u32 fget(struct fat *f, u32 c) { return f->fat[c] & 0x0FFFFFFF; }

static void fset(struct fat *f, u32 c, u32 v) {
    f->fat[c] = (f->fat[c] & 0xF0000000) | (v & 0x0FFFFFFF);
    u32 sec = c / 128;
    for (u32 k = 0; k < f->nfats; k++) wr(f, f->rsvd + k * f->fatsz + sec, 1, f->fat + sec * 128);
}

static u32 falloc(struct fat *f, int zero) {
    for (u32 i = 0; i < f->nclus; i++) {
        u32 c = 2 + (f->hint + i) % f->nclus;
        if (fget(f, c) == 0) {
            fset(f, c, EOC);
            f->hint = c - 1;
            if (zero) {
                u8 *z = kzalloc(f->cs);
                if (z) { wr(f, clba(f, c), f->spc, z); kfree(z); }
            }
            return c;
        }
    }
    return 0;
}

static void ffree_chain(struct fat *f, u32 c) {
    while (c >= 2 && c < 0x0FFFFFF8) {
        u32 n = fget(f, c);
        fset(f, c, 0);
        c = n;
    }
}

static int dir_load(struct fat *f, u32 c, struct dir *d) {
    d->nch = 0;
    for (u32 x = c; x >= 2 && x < 0x0FFFFFF8 && d->nch < 4096; x = fget(f, x)) d->nch++;
    d->chain = kalloc(sizeof(u32) * (d->nch + 1));
    d->len = (u32)d->nch * f->cs;
    d->buf = kalloc(d->len + 1);
    if (!d->chain || !d->buf) return -1;
    int i = 0;
    for (u32 x = c; i < d->nch; x = fget(f, x), i++) {
        d->chain[i] = x;
        if (rd(f, clba(f, x), f->spc, d->buf + (u64)i * f->cs) < 0) return -1;
    }
    return 0;
}

static void dir_free(struct dir *d) { kfree(d->chain); kfree(d->buf); d->chain = NULL; d->buf = NULL; }

static void dir_flush(struct fat *f, struct dir *d, u32 off, u32 n) {
    u32 first = off / 512, last = (off + n - 1) / 512;
    for (u32 s = first; s <= last; s++) {
        u32 ci = s * 512 / f->cs, within = (s * 512) % f->cs;
        wr(f, clba(f, d->chain[ci]) + within / 512, 1, d->buf + (u64)s * 512);
    }
}

static int dir_grow(struct fat *f, struct dir *d) {
    u32 last = d->chain[d->nch - 1];
    u32 n = falloc(f, 1);
    if (!n) return -1;
    fset(f, last, n);
    u32 *nc = krealloc(d->chain, sizeof(u32) * (d->nch + 2));
    u8 *nb = krealloc(d->buf, d->len + f->cs + 1);
    if (!nc || !nb) return -1;
    d->chain = nc; d->buf = nb;
    d->chain[d->nch++] = n;
    memset(d->buf + d->len, 0, f->cs);
    d->len += f->cs;
    return 0;
}

static u8 lfn_sum(const u8 *s) {
    u8 sum = 0;
    for (int i = 0; i < 11; i++) sum = (u8)(((sum & 1) << 7) | (sum >> 1)) + s[i];
    return sum;
}

static void dos_now(u16 *date, u16 *time) {
    int h, m, s, d, mo, y;
    if (!rtc_get(&h, &m, &s, &d, &mo, &y)) { *date = (44 << 9) | (1 << 5) | 1; *time = 0; return; }
    *date = (u16)(((y - 1980) << 9) | (mo << 5) | d);
    *time = (u16)((h << 11) | (m << 5) | (s / 2));
}

static int f_load(node *dn) {
    struct fat *f = dn->fs;
    struct dir d;
    if (dir_load(f, (u32)dn->priv, &d) < 0) { dir_free(&d); return -1; }
    char lfn[260];
    int have = 0, nl = 0;
    u8 csum = 0;
    for (u32 off = 0; off + 32 <= d.len; off += 32) {
        u8 *e = d.buf + off;
        if (e[0] == 0) break;
        if (e[0] == 0xE5) { have = 0; nl = 0; continue; }
        if (e[11] == 0x0F) {
            int seq = e[0] & 0x1F;
            if (e[0] & 0x40) { memset(lfn, 0, sizeof lfn); have = 1; nl = 0; csum = e[13]; }
            if (!have || seq < 1 || seq > 20) { have = 0; continue; }
            nl++;
            static const u8 pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (int i = 0; i < 13; i++) {
                u16 ch = e[pos[i]] | (e[pos[i] + 1] << 8);
                int idx = (seq - 1) * 13 + i;
                if (idx < 255) lfn[idx] = (ch == 0 || ch == 0xFFFF) ? 0 : (ch < 128 ? (char)ch : '?');
            }
            continue;
        }
        if (e[11] & 0x08) { have = 0; nl = 0; continue; }
        char name[260];
        if (have && lfn_sum(e) == csum && lfn[0]) snprintf(name, sizeof name, "%s", lfn);
        else {
            char b[9], x[4];
            memcpy(b, e, 8); b[8] = 0;
            memcpy(x, e + 8, 3); x[3] = 0;
            for (int i = 7; i >= 0 && b[i] == ' '; i--) b[i] = 0;
            for (int i = 2; i >= 0 && x[i] == ' '; i--) x[i] = 0;
            if (e[12] & 0x08) for (char *p = b; *p; p++) *p = (char)tolower(*p);
            if (e[12] & 0x10) for (char *p = x; *p; p++) *p = (char)tolower(*p);
            if (b[0] == 5) b[0] = (char)0xE5;
            if (x[0]) snprintf(name, sizeof name, "%s.%s", b, x);
            else snprintf(name, sizeof name, "%s", b);
        }
        int isdir = (e[11] & 0x10) != 0;
        int skip = !strcmp(name, ".") || !strcmp(name, "..");
        if (!skip && strlen(name) < 60) {
            node *n = vfs_add(dn, name, isdir);
            if (n) {
                n->size = isdir ? 0 : *(u32 *)(e + 28);
                n->priv = ((u32)*(u16 *)(e + 20) << 16) | *(u16 *)(e + 26);
                n->priv2 = (u64)off | ((u64)nl << 40);
                n->fs = f;
            }
        }
        have = 0; nl = 0;
    }
    dir_free(&d);
    return 0;
}

static i64 f_read(node *n, u64 off, void *buf, u64 len) {
    struct fat *f = n->fs;
    if (off >= n->size) return 0;
    if (len > n->size - off) len = n->size - off;
    u32 c = (u32)n->priv;
    u64 skip = off / f->cs;
    while (skip-- && c >= 2 && c < 0x0FFFFFF8) c = fget(f, c);
    u8 *tmp = kalloc(f->cs);
    if (!tmp) return -1;
    u64 done = 0, co = off % f->cs;
    while (done < len && c >= 2 && c < 0x0FFFFFF8) {
        u64 take = f->cs - co;
        if (take > len - done) take = len - done;
        if (rd(f, clba(f, c), f->spc, tmp) < 0) { kfree(tmp); return -1; }
        memcpy((u8 *)buf + done, tmp + co, take);
        done += take; co = 0;
        c = fget(f, c);
    }
    kfree(tmp);
    return (i64)done;
}

static int upd_entry(node *n) {
    struct fat *f = n->fs;
    node *p = n->par;
    struct dir d;
    if (dir_load(f, (u32)p->priv, &d) < 0) { dir_free(&d); return -1; }
    u32 off = (u32)(n->priv2 & 0xFFFFFFFFFFULL);
    if (off + 32 > d.len) { dir_free(&d); return -1; }
    u8 *e = d.buf + off;
    u16 date, time;
    dos_now(&date, &time);
    *(u16 *)(e + 20) = (u16)(n->priv >> 16);
    *(u16 *)(e + 26) = (u16)n->priv;
    *(u32 *)(e + 28) = n->dir ? 0 : (u32)n->size;
    *(u16 *)(e + 22) = time;
    *(u16 *)(e + 24) = date;
    dir_flush(f, &d, off, 32);
    dir_free(&d);
    return 0;
}

static i64 f_write(node *n, u64 off, const void *buf, u64 len) {
    struct fat *f = n->fs;
    u32 c = (u32)n->priv;
    int dirty = 0;
    if (off == 0 && n->size == 0 && c) { ffree_chain(f, c); c = 0; n->priv = 0; dirty = 1; }
    if (!len) { if (dirty) upd_entry(n); return 0; }
    if (!c) {
        c = falloc(f, 0);
        if (!c) return -1;
        n->priv = c;
        dirty = 1;
    }
    u64 skip = off / f->cs;
    u32 cur = c;
    while (skip--) {
        u32 nx = fget(f, cur);
        if (nx < 2 || nx >= 0x0FFFFFF8) {
            nx = falloc(f, 1);
            if (!nx) return -1;
            fset(f, cur, nx);
        }
        cur = nx;
    }
    u8 *tmp = kalloc(f->cs);
    if (!tmp) return -1;
    u64 done = 0, co = off % f->cs;
    while (done < len) {
        u64 take = f->cs - co;
        if (take > len - done) take = len - done;
        if (take < f->cs) { if (rd(f, clba(f, cur), f->spc, tmp) < 0) { kfree(tmp); return -1; } }
        memcpy(tmp + co, (const u8 *)buf + done, take);
        if (wr(f, clba(f, cur), f->spc, tmp) < 0) { kfree(tmp); return -1; }
        done += take; co = 0;
        if (done < len) {
            u32 nx = fget(f, cur);
            if (nx < 2 || nx >= 0x0FFFFFF8) {
                nx = falloc(f, 0);
                if (!nx) { kfree(tmp); return -1; }
                fset(f, cur, nx);
            }
            cur = nx;
        }
    }
    kfree(tmp);
    if (off + len > n->size) { n->size = off + len; dirty = 1; }
    if (dirty) upd_entry(n);
    return (i64)done;
}

static int valid_ch(int c) {
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    return c && strchr("!#$%&'()-@^_`{}~", c) != NULL;
}

static int make83(const char *name, u8 *out, u8 *nt, int *lfn) {
    memset(out, ' ', 11);
    *nt = 0;
    *lfn = 0;
    const char *dot = strrchr(name, '.');
    if (dot == name) dot = NULL;
    int bl = dot ? (int)(dot - name) : (int)strlen(name);
    const char *ext = dot ? dot + 1 : "";
    int el = (int)strlen(ext);
    int bu = 0, bw = 0, eu = 0, ew = 0;
    for (int i = 0; i < bl; i++) { if (name[i] >= 'a' && name[i] <= 'z') bw = 1; if (name[i] >= 'A' && name[i] <= 'Z') bu = 1; }
    for (int i = 0; i < el; i++) { if (ext[i] >= 'a' && ext[i] <= 'z') ew = 1; if (ext[i] >= 'A' && ext[i] <= 'Z') eu = 1; }
    if (bl > 8 || el > 3 || (bu && bw) || (eu && ew)) *lfn = 1;
    int bi = 0, ei = 0;
    for (int i = 0; i < bl; i++) {
        int c = toupper((u8)name[i]);
        if (!valid_ch(c)) { c = '_'; *lfn = 1; }
        if (bi < 8) out[bi++] = (u8)c;
    }
    for (int i = 0; i < el; i++) {
        int c = toupper((u8)ext[i]);
        if (!valid_ch(c)) { c = '_'; *lfn = 1; }
        if (ei < 3) out[8 + ei++] = (u8)c;
    }
    if (strchr(name, ' ') || (dot && strchr(name, '.') != dot)) *lfn = 1;
    if (!*lfn) {
        if (bw && !bu) *nt |= 0x08;
        if (ew && !eu) *nt |= 0x10;
    }
    return 0;
}

static int f_create(node *dn, const char *name, int isdir, node *out) {
    struct fat *f = dn->fs;
    struct dir d;
    if (dir_load(f, (u32)dn->priv, &d) < 0) { dir_free(&d); return -1; }
    u8 sn[11], nt;
    int lfn;
    make83(name, sn, &nt, &lfn);
    if (lfn) {
        int bl = 0;
        while (bl < 8 && sn[bl] != ' ') bl++;
        if (bl > 6) bl = 6;
        for (int k = 1; k < 100; k++) {
            u8 t[11];
            memcpy(t, sn, 11);
            char sfx[4];
            int sl = snprintf(sfx, sizeof sfx, "~%d", k);
            for (int i = bl; i < 8; i++) t[i] = ' ';
            memcpy(t + bl, sfx, sl);
            int clash = 0;
            for (u32 o = 0; o + 32 <= d.len && d.buf[o]; o += 32)
                if (d.buf[o] != 0xE5 && d.buf[o + 11] != 0x0F && !memcmp(d.buf + o, t, 11)) clash = 1;
            if (!clash) { memcpy(sn, t, 11); break; }
        }
    }
    int nl = lfn ? ((int)strlen(name) + 12) / 13 : 0, need = nl + 1;
    u32 slot = 0;
    int found = 0;
    for (;;) {
        int run = 0;
        for (u32 o = 0; o + 32 <= d.len; o += 32) {
            if (d.buf[o] == 0 || d.buf[o] == 0xE5) { if (!run) slot = o; if (++run == need) { found = 1; break; } }
            else run = 0;
        }
        if (found) break;
        if (dir_grow(f, &d) < 0) { dir_free(&d); return -1; }
    }
    u16 date, time;
    dos_now(&date, &time);
    u8 sum = lfn_sum(sn);
    u32 off = slot;
    for (int i = nl; i >= 1; i--, off += 32) {
        u8 *e = d.buf + off;
        memset(e, 0, 32);
        e[0] = (u8)(i | (i == nl ? 0x40 : 0));
        e[11] = 0x0F;
        e[13] = sum;
        static const u8 pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
        int len = (int)strlen(name);
        for (int k = 0; k < 13; k++) {
            int idx = (i - 1) * 13 + k;
            u16 ch = idx < len ? (u8)name[idx] : (idx == len ? 0 : 0xFFFF);
            e[pos[k]] = ch & 255;
            e[pos[k] + 1] = ch >> 8;
        }
    }
    u8 *e = d.buf + off;
    memset(e, 0, 32);
    memcpy(e, sn, 11);
    e[11] = isdir ? 0x10 : 0x20;
    e[12] = nt;
    *(u16 *)(e + 14) = time; *(u16 *)(e + 16) = date; *(u16 *)(e + 18) = date;
    *(u16 *)(e + 22) = time; *(u16 *)(e + 24) = date;
    u32 cl = 0;
    if (isdir) {
        cl = falloc(f, 1);
        if (!cl) { dir_free(&d); return -1; }
        *(u16 *)(e + 20) = (u16)(cl >> 16);
        *(u16 *)(e + 26) = (u16)cl;
        u8 *z = kzalloc(f->cs);
        if (z) {
            memset(z, ' ', 11); z[0] = '.'; z[11] = 0x10;
            *(u16 *)(z + 22) = time; *(u16 *)(z + 24) = date; *(u16 *)(z + 16) = date;
            *(u16 *)(z + 20) = (u16)(cl >> 16); *(u16 *)(z + 26) = (u16)cl;
            memset(z + 32, ' ', 11); z[32] = '.'; z[33] = '.'; z[43] = 0x10;
            *(u16 *)(z + 54) = time; *(u16 *)(z + 56) = date; *(u16 *)(z + 48) = date;
            u32 pc = dn->par ? (u32)dn->priv : 0;
            if (pc == f->rootclus) pc = 0;
            *(u16 *)(z + 32 + 20) = (u16)(pc >> 16); *(u16 *)(z + 32 + 26) = (u16)pc;
            wr(f, clba(f, cl), f->spc, z);
            kfree(z);
        }
    }
    dir_flush(f, &d, slot, off + 32 - slot);
    out->priv = cl;
    out->priv2 = (u64)off | ((u64)nl << 40);
    out->fs = f;
    out->size = 0;
    dir_free(&d);
    return 0;
}

static int f_unlink(node *n) {
    struct fat *f = n->fs;
    struct dir d;
    if (dir_load(f, (u32)n->par->priv, &d) < 0) { dir_free(&d); return -1; }
    u32 off = (u32)(n->priv2 & 0xFFFFFFFFFFULL), nl = (u32)(n->priv2 >> 40);
    u32 first = off - nl * 32;
    for (u32 o = first; o <= off; o += 32) d.buf[o] = 0xE5;
    dir_flush(f, &d, first, off + 32 - first);
    dir_free(&d);
    if (n->priv) ffree_chain(f, (u32)n->priv);
    return 0;
}

static const struct fsops fatops = { f_read, f_write, f_load, f_create, f_unlink };

struct fat *fat_mount(int d, u64 start, u64 size, node *at) {
    u8 *s = kalloc(512);
    if (!s) return NULL;
    if (disk_read(d, start, 1, s) < 0 || s[510] != 0x55 || s[511] != 0xAA ||
        *(u16 *)(s + 11) != 512 || *(u16 *)(s + 22) != 0 || *(u32 *)(s + 36) == 0) { kfree(s); return NULL; }
    struct fat *f = kzalloc(sizeof *f);
    f->d = d; f->start = start;
    f->spc = s[13];
    f->rsvd = *(u16 *)(s + 14);
    f->nfats = s[16];
    f->tot = *(u32 *)(s + 32) ? *(u32 *)(s + 32) : (u32)size;
    f->fatsz = *(u32 *)(s + 36);
    f->rootclus = *(u32 *)(s + 44);
    f->dstart = f->rsvd + f->nfats * f->fatsz;
    f->cs = f->spc * 512;
    f->nclus = (f->tot - f->dstart) / f->spc;
    memcpy(f->label, s + 71, 11);
    for (int i = 10; i >= 0 && f->label[i] == ' '; i--) f->label[i] = 0;
    kfree(s);
    if (!f->spc || f->fatsz > (64u << 11) || f->nclus < 65525 || (u64)f->nclus + 2 > (u64)f->fatsz * 128) { kfree(f); return NULL; }
    f->fat = kalloc((u64)f->fatsz * 512);
    if (!f->fat || disk_read(d, start + f->rsvd, f->fatsz, f->fat) < 0) { kfree(f->fat); kfree(f); return NULL; }
    at->ops = &fatops;
    at->fs = f;
    at->priv = f->rootclus;
    at->loaded = 0;
    at->kid = NULL;
    klog("fat: mounted %s, %u clusters of %u bytes\n", f->label, f->nclus, f->cs);
    return f;
}

int fat_format(int d, u64 start, u64 size, const char *label) {
    if (size < 70000 || size > 0xFFFFFFFFULL || start > 0xFFFFFFFFULL) return -1;
    u32 spc = size <= 532480 ? 1 : size <= 16777216 ? 8 : 16;
    u32 rsvd = 32, nfats = 2;
    u32 fatsz = 1, clus = 0;
    for (int i = 0; i < 16; i++) {
        u64 data_secs = size - rsvd - (u64)nfats * fatsz;
        clus = (u32)(data_secs / spc);
        u32 needed = (u32)(((u64)(clus + 2) * 4 + 511) / 512);
        if (needed == fatsz) break;
        fatsz = needed;
    }
    clus = (u32)((size - rsvd - (u64)nfats * fatsz) / spc);
    if (clus < 65525 || (u64)clus + 2 > (u64)fatsz * 128) return -1;
    u8 *b = kzalloc(512);
    if (!b) return -1;
    b[0] = 0xEB; b[1] = 0x58; b[2] = 0x90;
    memcpy(b + 3, "TAKFAT32", 8);
    *(u16 *)(b + 11) = 512; b[13] = (u8)spc;
    *(u16 *)(b + 14) = (u16)rsvd; b[16] = (u8)nfats;
    b[21] = 0xF8;
    *(u16 *)(b + 24) = 63; *(u16 *)(b + 26) = 255;
    *(u32 *)(b + 28) = (u32)start;
    *(u32 *)(b + 32) = (u32)size;
    *(u32 *)(b + 36) = fatsz;
    *(u32 *)(b + 44) = 2;
    *(u16 *)(b + 48) = 1; *(u16 *)(b + 50) = 6;
    b[64] = 0x80; b[66] = 0x29;
    *(u32 *)(b + 67) = (u32)ticks ^ 0x5A4B3C2D;
    memset(b + 71, ' ', 11);
    for (int i = 0; i < 11 && label[i]; i++) b[71 + i] = (u8)toupper((u8)label[i]);
    memcpy(b + 82, "FAT32   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
    u8 *fi = kzalloc(512);
    *(u32 *)(fi + 0) = 0x41615252; *(u32 *)(fi + 484) = 0x61417272;
    *(u32 *)(fi + 488) = 0xFFFFFFFF; *(u32 *)(fi + 492) = 0xFFFFFFFF;
    fi[510] = 0x55; fi[511] = 0xAA;
    int r = 0;
    r |= disk_write(d, start, 1, b);
    r |= disk_write(d, start + 1, 1, fi);
    r |= disk_write(d, start + 6, 1, b);
    r |= disk_write(d, start + 7, 1, fi);
    u8 *z = kzalloc(512 * 64);
    if (!z) { kfree(b); kfree(fi); return -1; }
    for (u32 k = 0; k < nfats; k++) {
        for (u32 s = 0; s < fatsz; s += 64) {
            u32 n = fatsz - s < 64 ? fatsz - s : 64;
            memset(z, 0, 512 * 64);
            if (s == 0) { ((u32 *)z)[0] = 0x0FFFFFF8; ((u32 *)z)[1] = 0x0FFFFFFF; ((u32 *)z)[2] = EOC; }
            r |= disk_write(d, start + rsvd + k * fatsz + s, n, z);
        }
    }
    memset(z, 0, 512 * 64);
    memset(z, ' ', 11);
    for (int i = 0; i < 11 && label[i]; i++) z[i] = (u8)toupper((u8)label[i]);
    z[11] = 0x08;
    for (u32 s = 0; s < spc; s++) { r |= disk_write(d, start + rsvd + nfats * fatsz + s, 1, z); memset(z, 0, 512); }
    kfree(z); kfree(b); kfree(fi);
    return r ? -1 : 0;
}
