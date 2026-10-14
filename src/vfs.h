#ifndef VFS_H
#define VFS_H
#include "tak.h"

typedef struct node node;

struct fsops {
    i64 (*read)(node *n, u64 off, void *buf, u64 len);
    i64 (*write)(node *n, u64 off, const void *buf, u64 len);
    int (*load)(node *dir);
    int (*create)(node *dir, const char *name, int isdir, node *out);
    int (*unlink)(node *n);
};

struct node {
    char name[64];
    u8 dir, loaded;
    u64 size, cap;
    u8 *data;
    node *par, *kid, *nxt;
    const struct fsops *ops;
    u64 priv, priv2;
    void *fs;
};

extern node *vroot;

void vfs_init(void);
node *vfs_find(const char *path, node *cwd);
node *vfs_child(node *dir, const char *name);
node *vfs_add(node *dir, const char *name, int isdir);
node *vfs_mk(node *dir, const char *name, int isdir);
int vfs_rm(node *n);
int vfs_rename(node *n, const char *name);
i64 vfs_rd(node *n, u64 off, void *buf, u64 len);
i64 vfs_wr(node *n, u64 off, const void *buf, u64 len);
int vfs_load(node *dir);
void vfs_path(node *n, char *out, int cap);
void vfs_untar(const u8 *p, u64 len);
const char *ext_of(const char *name);
u8 *vfs_slurp(node *n, u64 *size);
int vfs_save(node *dir, const char *name, const void *data, u64 len);

#endif
