#include "tak.h"
#include "vfs.h"

node *vroot;

void vfs_init(void) {
    vroot = kzalloc(sizeof *vroot);
    vroot->dir = 1;
    vroot->loaded = 1;
    strcpy(vroot->name, "/");
}

int vfs_load(node *d) {
    if (!d->dir) return -1;
    if (!d->loaded) {
        d->loaded = 1;
        if (d->ops && d->ops->load) d->ops->load(d);
    }
    return 0;
}

node *vfs_child(node *d, const char *name) {
    if (!d || !d->dir) return NULL;
    vfs_load(d);
    for (node *k = d->kid; k; k = k->nxt)
        if (!strcasecmp(k->name, name)) return k;
    return NULL;
}

node *vfs_add(node *d, const char *name, int isdir) {
    node *n = kzalloc(sizeof *n);
    if (!n) return NULL;
    strncpy(n->name, name, sizeof n->name - 1);
    n->dir = isdir;
    n->loaded = !isdir || !d->ops;
    n->par = d;
    n->ops = d->ops;
    n->fs = d->fs;
    node **pp = &d->kid;
    while (*pp) pp = &(*pp)->nxt;
    *pp = n;
    return n;
}

node *vfs_mk(node *d, const char *name, int isdir) {
    if (!d || !d->dir || !*name || strlen(name) >= 60) return NULL;
    if (strchr(name, '/')) return NULL;
    vfs_load(d);
    if (vfs_child(d, name)) return NULL;
    node *n = vfs_add(d, name, isdir);
    if (!n) return NULL;
    if (d->ops && d->ops->create && d->ops->create(d, name, isdir, n) < 0) {
        vfs_rm(n);
        return NULL;
    }
    return n;
}

node *vfs_find(const char *path, node *cwd) {
    node *cur = path[0] == '/' ? vroot : (cwd ? cwd : vroot);
    char part[64];
    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        int n = 0;
        while (*path && *path != '/' && n < 63) part[n++] = *path++;
        part[n] = 0;
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) { if (cur->par) cur = cur->par; continue; }
        cur = vfs_child(cur, part);
        if (!cur) return NULL;
    }
    return cur;
}

int vfs_rm(node *n) {
    if (!n || !n->par) return -1;
    if (n->dir) { vfs_load(n); if (n->kid) return -1; }
    if (n->ops && n->ops->unlink && n->ops->unlink(n) < 0) return -1;
    node **pp = &n->par->kid;
    while (*pp && *pp != n) pp = &(*pp)->nxt;
    if (*pp) *pp = n->nxt;
    if (!n->ops) kfree(n->data);
    kfree(n);
    return 0;
}

int vfs_rename(node *n, const char *name) {
    if (!n->par || n->ops || vfs_child(n->par, name)) return -1;
    strncpy(n->name, name, sizeof n->name - 1);
    return 0;
}

i64 vfs_rd(node *n, u64 off, void *buf, u64 len) {
    if (!n || n->dir) return -1;
    if (n->ops && n->ops->read) return n->ops->read(n, off, buf, len);
    if (off >= n->size) return 0;
    if (len > n->size - off) len = n->size - off;
    memcpy(buf, n->data + off, len);
    return (i64)len;
}

i64 vfs_wr(node *n, u64 off, const void *buf, u64 len) {
    if (!n || n->dir) return -1;
    if (n->ops && n->ops->write) return n->ops->write(n, off, buf, len);
    if (off + len > n->cap) {
        u64 c = n->cap ? n->cap : 256;
        while (c < off + len) c *= 2;
        u8 *d = krealloc(n->data, c);
        if (!d) return -1;
        memset(d + n->cap, 0, c - n->cap);
        n->data = d;
        n->cap = c;
    }
    memcpy(n->data + off, buf, len);
    if (off + len > n->size) n->size = off + len;
    return (i64)len;
}

void vfs_path(node *n, char *out, int cap) {
    char tmp[256];
    int p = sizeof tmp - 1;
    tmp[p] = 0;
    if (n == vroot) { snprintf(out, cap, "/"); return; }
    while (n && n != vroot) {
        int l = (int)strlen(n->name);
        if (p < l + 1) break;
        p -= l;
        memcpy(tmp + p, n->name, l);
        tmp[--p] = '/';
        n = n->par;
    }
    snprintf(out, cap, "%s", tmp + p);
}

const char *ext_of(const char *name) {
    const char *d = strrchr(name, '.');
    return d ? d + 1 : "";
}

u8 *vfs_slurp(node *n, u64 *size) {
    if (!n || n->dir) return NULL;
    u8 *b = kalloc(n->size + 1);
    if (!b) return NULL;
    i64 r = vfs_rd(n, 0, b, n->size);
    if (r < 0) { kfree(b); return NULL; }
    b[r] = 0;
    if (size) *size = (u64)r;
    return b;
}

int vfs_save(node *dir, const char *name, const void *data, u64 len) {
    if (!dir || !dir->dir || !name || !*name || (!data && len)) return -1;
    node *f = vfs_child(dir, name);
    if (!f) f = vfs_mk(dir, name, 0);
    if (!f) return -1;
    if (!f->ops) f->size = 0;
    else if (f->ops->write) {
        f->size = 0;
    }
    return vfs_wr(f, 0, data, len) == (i64)len ? 0 : -1;
}

static u64 oct(const char *s, int n) {
    u64 v = 0;
    for (int i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++) v = v * 8 + (s[i] - '0');
    return v;
}

void vfs_untar(const u8 *p, u64 len) {
    u64 off = 0;
    while (off + 512 <= len) {
        const char *h = (const char *)p + off;
        if (!h[0]) break;
        char name[256];
        char pre[156] = {0};
        memcpy(pre, h + 345, 155);
        char nm[101] = {0};
        memcpy(nm, h, 100);
        if (!memcmp(h + 257, "ustar", 5) && pre[0]) snprintf(name, sizeof name, "%s/%s", pre, nm);
        else snprintf(name, sizeof name, "%s", nm);
        u64 sz = oct(h + 124, 12);
        char type = h[156];
        node *cur = vroot;
        char *s = name;
        while (*s) {
            while (*s == '/') s++;
            if (!*s) break;
            char part[64];
            int n = 0;
            while (*s && *s != '/' && n < 63) part[n++] = *s++;
            part[n] = 0;
            int last = *s == 0 || (*s == '/' && !s[1]);
            if (!strcmp(part, ".")) continue;
            node *c = vfs_child(cur, part);
            if (!c) c = vfs_add(cur, part, !(last && (type == '0' || type == 0)));
            if (!c) break;
            cur = c;
            if (last && !cur->dir && (type == '0' || type == 0)) {
                cur->data = kalloc(sz + 1);
                if (cur->data) {
                    memcpy(cur->data, p + off + 512, sz);
                    cur->cap = sz + 1;
                    cur->size = sz;
                }
            }
        }
        off += 512 + ((sz + 511) & ~511ULL);
    }
}
