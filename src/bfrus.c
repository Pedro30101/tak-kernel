#if __STDC_HOSTED__
#include <stdlib.h>
#endif
#include "tak.h"
#include "disk.h"
#include "vfs.h"
#include "bfrus_format.h"

int rtc_get(int *h, int *m, int *s, int *d, int *mo, int *y);

struct bfrus {
    int d;
    u64 start;
    u64 block_count;
    u64 bitmap_start;
    u32 bitmap_blocks;
    u64 inode_start;
    u32 inode_count;
    u32 inode_blocks;
    u64 data_start;
    u32 root_inode;
    u8 *bitmap;
    int dirty;
};

static int bfrus_rd(struct bfrus *f, u64 block, u32 n, void *buf) {
    u64 spb = BFRUS_BLOCK_SIZE / 512;
    return disk_read(f->d, f->start + block * spb, n * spb, buf);
}

static int bfrus_wr(struct bfrus *f, u64 block, u32 n, const void *buf) {
    u64 spb = BFRUS_BLOCK_SIZE / 512;
    return disk_write(f->d, f->start + block * spb, n * spb, buf);
}

static u32 bfrus_blocks_for(u64 bytes) {
    return (u32)((bytes + BFRUS_BLOCK_SIZE - 1) / BFRUS_BLOCK_SIZE);
}

static int bfrus_bitmap_get(struct bfrus *f, u64 block) {
    u64 byte = block / 8;
    u8 bit = 1 << (block % 8);
    return (f->bitmap[byte] & bit) != 0;
}

static void bfrus_bitmap_set(struct bfrus *f, u64 block, int val) {
    u64 byte = block / 8;
    u8 bit = 1 << (block % 8);
    if (val)
        f->bitmap[byte] |= bit;
    else
        f->bitmap[byte] &= ~bit;
    f->dirty = 1;
}

static int bfrus_bitmap_flush(struct bfrus *f) {
    if (!f->dirty)
        return 0;
    int r = bfrus_wr(f, f->bitmap_start, f->bitmap_blocks, f->bitmap);
    if (r == 0)
        f->dirty = 0;
    return r;
}

static u64 bfrus_alloc_block(struct bfrus *f) {
    u64 total = (u64)f->bitmap_blocks * BFRUS_BLOCK_SIZE * 8;
    if (total > f->block_count)
        total = f->block_count;
    /* Skip reserved blocks (superblock + bitmap + inode table) */
    u64 start = f->data_start > 0 ? f->data_start : f->bitmap_blocks + f->inode_blocks;
    for (u64 i = start; i < total; i++) {
        if (!bfrus_bitmap_get(f, i)) {
            bfrus_bitmap_set(f, i, 1);
            if (bfrus_bitmap_flush(f) < 0)
                return 0;
            return i;
        }
    }
    return 0;
}

static void bfrus_free_block(struct bfrus *f, u64 block) {
    bfrus_bitmap_set(f, block, 0);
    bfrus_bitmap_flush(f);
}

static int bfrus_read_inode(struct bfrus *f, u32 ino, struct bfrus_inode *in) {
    if (ino < 1 || ino > f->inode_count)
        return -1;
    u64 block = f->inode_start + (u64)(ino - 1) / 32;
    u32 off = ((u32)(ino - 1) % 32) * BFRUS_INODE_SIZE;
    u8 buf[BFRUS_BLOCK_SIZE];
    if (bfrus_rd(f, block, 1, buf) < 0)
        return -1;
    memcpy(in, buf + off, BFRUS_INODE_SIZE);
    return 0;
}

static int bfrus_write_inode(struct bfrus *f, u32 ino, const struct bfrus_inode *in) {
    if (ino < 1 || ino > f->inode_count)
        return -1;
    u64 block = f->inode_start + (u64)(ino - 1) / 32;
    u32 off = ((u32)(ino - 1) % 32) * BFRUS_INODE_SIZE;
    u8 buf[BFRUS_BLOCK_SIZE];
    if (bfrus_rd(f, block, 1, buf) < 0)
        return -1;
    memcpy(buf + off, in, BFRUS_INODE_SIZE);
    return bfrus_wr(f, block, 1, buf);
}

static u32 bfrus_alloc_inode(struct bfrus *f) {
    for (u32 i = 1; i <= f->inode_count; i++) {
        struct bfrus_inode in;
        if (bfrus_read_inode(f, i, &in) < 0)
            return 0;
        if (in.mode == 0)
            return i;
    }
    return 0;
}

static u64 bfrus_extent_block(const struct bfrus_inode *in, u32 block_idx) {
    u64 acc = 0;
    for (u32 i = 0; i < in->ext_count; i++) {
        if (block_idx < acc + in->extents[i].count)
            return in->extents[i].start + (block_idx - acc);
        acc += in->extents[i].count;
    }
    return 0;
}

static int bfrus_extent_append(struct bfrus_inode *in, u64 block) {
    if (in->ext_count >= BFRUS_MAX_EXTENTS)
        return -1;
    if (in->ext_count > 0 &&
        in->extents[in->ext_count - 1].start +
            in->extents[in->ext_count - 1].count == block) {
        in->extents[in->ext_count - 1].count++;
    } else {
        in->extents[in->ext_count].start = block;
        in->extents[in->ext_count].count = 1;
        in->ext_count++;
    }
    return 0;
}

static int bfrus_dir_find(struct bfrus *f, u32 dir_ino, const char *name);
static int bfrus_dir_add(struct bfrus *f, u32 dir_ino, u32 ino, const char *name, u64 *out_off);
static int bfrus_dir_remove(struct bfrus *f, u32 dir_ino, u64 off);

static i64 bfrus_file_read(struct node *n, u64 off, void *buf, u64 len) {
    struct bfrus *f = n->fs;
    struct bfrus_inode in;
    if (bfrus_read_inode(f, (u32)n->priv, &in) < 0)
        return -1;
    if (off >= in.size)
        return 0;
    if (len > in.size - off)
        len = in.size - off;
    u64 done = 0;
    u64 cur = off;
    while (done < len) {
        u32 block_idx = (u32)(cur / BFRUS_BLOCK_SIZE);
        u32 boff = cur % BFRUS_BLOCK_SIZE;
        u64 take = BFRUS_BLOCK_SIZE - boff;
        if (take > len - done)
            take = len - done;
        u64 block = bfrus_extent_block(&in, block_idx);
        if (!block)
            break;
        u8 tmp[BFRUS_BLOCK_SIZE];
        if (bfrus_rd(f, block, 1, tmp) < 0)
            return -1;
        u32 copy = (u32)take;
        if (copy > BFRUS_BLOCK_SIZE - boff)
            copy = BFRUS_BLOCK_SIZE - boff;
        memcpy((u8 *)buf + done, tmp + boff, copy);
        done += copy;
        cur += copy;
    }
    return (i64)done;
}

static i64 bfrus_file_write(struct node *n, u64 off, const void *buf, u64 len) {
    struct bfrus *f = n->fs;
    struct bfrus_inode in;
    if (bfrus_read_inode(f, (u32)n->priv, &in) < 0)
        return -1;
    u64 needed = off + len;
    if (in.size == 0 && needed > 0) {
        u64 block = bfrus_alloc_block(f);
        if (!block)
            return -1;
        if (bfrus_extent_append(&in, block) < 0) {
            bfrus_free_block(f, block);
            return -1;
        }
    }
    u64 done = 0;
    u64 cur = off;
    while (done < len) {
        u32 block_idx = (u32)(cur / BFRUS_BLOCK_SIZE);
        u32 boff = cur % BFRUS_BLOCK_SIZE;
        u64 take = BFRUS_BLOCK_SIZE - boff;
        if (take > len - done)
            take = len - done;
        u64 block = bfrus_extent_block(&in, block_idx);
        if (!block) {
            block = bfrus_alloc_block(f);
            if (!block)
                return -1;
            if (bfrus_extent_append(&in, block) < 0) {
                bfrus_free_block(f, block);
                return -1;
            }
            in.size = cur + BFRUS_BLOCK_SIZE;
        }
        u8 tmp[BFRUS_BLOCK_SIZE];
        if (take < BFRUS_BLOCK_SIZE) {
            if (bfrus_rd(f, block, 1, tmp) < 0)
                return -1;
        } else {
            memset(tmp, 0, BFRUS_BLOCK_SIZE);
        }
        u32 copy = (u32)take;
        if (copy > BFRUS_BLOCK_SIZE - boff)
            copy = BFRUS_BLOCK_SIZE - boff;
        memcpy(tmp + boff, (const u8 *)buf + done, copy);
        if (bfrus_wr(f, block, 1, tmp) < 0)
            return -1;
        done += copy;
        cur += copy;
    }
    /* Truncate if writing shorter than current size */
    if (needed < in.size) {
        u64 last_block = bfrus_blocks_for(needed);
        u64 acc = 0;
        u32 ext_i = 0;
        while (ext_i < in.ext_count) {
            if (last_block < acc + in.extents[ext_i].count) {
                /* Truncate this extent */
                u64 keep = last_block - acc;
                if (keep == 0) {
                    /* Free this entire extent */
                    for (u64 j = 0; j < in.extents[ext_i].count; j++)
                        bfrus_free_block(f, in.extents[ext_i].start + j);
                    in.extents[ext_i].start = 0;
                    in.extents[ext_i].count = 0;
                } else {
                    /* Free blocks from keep to end of this extent */
                    for (u64 j = keep; j < in.extents[ext_i].count; j++)
                        bfrus_free_block(f, in.extents[ext_i].start + j);
                    in.extents[ext_i].count = keep;
                }
                /* Free all remaining extents */
                for (u32 j = ext_i + 1; j < in.ext_count; j++) {
                    for (u64 k = 0; k < in.extents[j].count; k++)
                        bfrus_free_block(f, in.extents[j].start + k);
                    in.extents[j].start = 0;
                    in.extents[j].count = 0;
                }
                break;
            }
            acc += in.extents[ext_i].count;
            ext_i++;
        }
        /* Compact trailing empty extents */
        u32 write_idx = 0;
        for (u32 i = 0; i < in.ext_count; i++) {
            if (in.extents[i].count > 0) {
                if (i != write_idx)
                    in.extents[write_idx] = in.extents[i];
                write_idx++;
            }
        }
        in.ext_count = write_idx;
        in.size = needed;
        in.blocks = bfrus_blocks_for(needed);
    } else if (in.size < needed) {
        in.size = needed;
        in.blocks = bfrus_blocks_for(needed);
    }
    if (bfrus_write_inode(f, (u32)n->priv, &in) < 0)
        return -1;
    return (i64)done;
}

static int bfrus_dir_load(struct node *dir) {
    struct bfrus *f = dir->fs;
    struct bfrus_inode in;
    if (bfrus_read_inode(f, (u32)dir->priv, &in) < 0)
        return -1;
    if (!(in.mode & 0x10))
        return -1;
    u64 off = 0;
    while (off < in.size) {
        u64 block = f->data_start + off / BFRUS_BLOCK_SIZE;
        u32 boff = off % BFRUS_BLOCK_SIZE;
        u8 buf[BFRUS_BLOCK_SIZE];
        if (bfrus_rd(f, block, 1, buf) < 0)
            return -1;
        u64 remaining = in.size - off;
        u32 avail = BFRUS_BLOCK_SIZE - boff;
        if (avail > remaining)
            avail = remaining;
        u64 pos = 0;
        while (pos + BFRUS_DIR_ENTRY_SIZE <= avail) {
            struct bfrus_dirent *de = (struct bfrus_dirent *)(buf + boff + pos);
            if (de->inode != 0 && de->namelen > 0 && de->namelen <= BFRUS_NAME_MAX) {
                char name[BFRUS_NAME_MAX + 1];
                memcpy(name, de->name, de->namelen);
                name[de->namelen] = 0;
                node *child = vfs_add(dir, name, 0);
                if (child) {
                    child->priv = de->inode;
                    child->priv2 = off + pos;
                    child->fs = f;
                }
            }
            pos += BFRUS_DIR_ENTRY_SIZE;
        }
        off += avail;
    }
    return 0;
}

static int bfrus_dir_find(struct bfrus *f, u32 dir_ino, const char *name) {
    struct bfrus_inode in;
    if (bfrus_read_inode(f, dir_ino, &in) < 0)
        return 0;
    if (!(in.mode & 0x10))
        return 0;
    u64 off = 0;
    while (off < in.size) {
        u64 block = f->data_start + off / BFRUS_BLOCK_SIZE;
        u32 boff = off % BFRUS_BLOCK_SIZE;
        u8 buf[BFRUS_BLOCK_SIZE];
        if (bfrus_rd(f, block, 1, buf) < 0)
            return 0;
        u64 remaining = in.size - off;
        u32 avail = BFRUS_BLOCK_SIZE - boff;
        if (avail > remaining)
            avail = remaining;
        u64 pos = 0;
        while (pos + BFRUS_DIR_ENTRY_SIZE <= avail) {
            struct bfrus_dirent *de = (struct bfrus_dirent *)(buf + boff + pos);
            if (de->inode != 0 && de->namelen > 0 && de->namelen <= BFRUS_NAME_MAX &&
                de->namelen == (u8)strlen(name) &&
                memcmp(de->name, name, de->namelen) == 0) {
                return de->inode;
            }
            pos += BFRUS_DIR_ENTRY_SIZE;
        }
        off += avail;
    }
    return 0;
}

static int bfrus_dir_add(struct bfrus *f, u32 dir_ino, u32 ino, const char *name, u64 *out_off) {
    struct bfrus_inode in;
    if (bfrus_read_inode(f, dir_ino, &in) < 0)
        return -1;
    if (!(in.mode & 0x10))
        return -1;
    u64 off = 0;   /* search from start of directory for empty slot */
    u8 name_buf[BFRUS_NAME_MAX];
    u32 namelen = (u32)strlen(name);
    if (namelen > BFRUS_NAME_MAX)
        return -1;
    memcpy(name_buf, name, namelen);
    while (off < in.size) {
        u64 block = f->data_start + off / BFRUS_BLOCK_SIZE;
        u32 boff = off % BFRUS_BLOCK_SIZE;
        u8 buf[BFRUS_BLOCK_SIZE];
        if (bfrus_rd(f, block, 1, buf) < 0)
            return -1;
        u64 remaining = in.size - off;
        u32 avail = BFRUS_BLOCK_SIZE - boff;
        /* Search whole block for empty slots, not just up to in.size */
        (void)remaining;
        u64 pos = 0;
        while (pos + BFRUS_DIR_ENTRY_SIZE <= avail) {
            struct bfrus_dirent *de = (struct bfrus_dirent *)(buf + boff + pos);
            if (de->inode == 0) {
                memset(de, 0, BFRUS_DIR_ENTRY_SIZE);
                de->inode = ino;
                de->namelen = (u8)namelen;
                memcpy(de->name, name_buf, namelen);
                if (bfrus_wr(f, block, 1, buf) < 0)
                    return -1;
                in.size = off + pos + BFRUS_DIR_ENTRY_SIZE;
                in.blocks = bfrus_blocks_for(in.size);
                bfrus_write_inode(f, dir_ino, &in);
                if (out_off)
                    *out_off = off + pos;
                return 0;
            }
            pos += BFRUS_DIR_ENTRY_SIZE;
        }
        off += avail;
    }
    u64 block = bfrus_alloc_block(f);
    if (!block)
        return -1;
    u8 buf[BFRUS_BLOCK_SIZE];
    memset(buf, 0, BFRUS_BLOCK_SIZE);
    struct bfrus_dirent *de = (struct bfrus_dirent *)buf;
    de->inode = ino;
    de->namelen = (u8)namelen;
    memcpy(de->name, name_buf, namelen);
    if (bfrus_wr(f, block, 1, buf) < 0) {
        bfrus_free_block(f, block);
        return -1;
    }
    u64 new_size = off + BFRUS_DIR_ENTRY_SIZE;
    in.size = new_size;
    in.blocks = bfrus_blocks_for(new_size);
    if (bfrus_write_inode(f, dir_ino, &in) < 0) {
        bfrus_free_block(f, block);
        return -1;
    }
    if (out_off)
        *out_off = off;
    return 0;
}

static int bfrus_dir_remove(struct bfrus *f, u32 dir_ino, u64 off) {
    struct bfrus_inode in;
    if (bfrus_read_inode(f, dir_ino, &in) < 0)
        return -1;
    u64 block = f->data_start + off / BFRUS_BLOCK_SIZE;
    u32 boff = off % BFRUS_BLOCK_SIZE;
    u8 buf[BFRUS_BLOCK_SIZE];
    if (bfrus_rd(f, block, 1, buf) < 0)
        return -1;
    struct bfrus_dirent *de = (struct bfrus_dirent *)(buf + boff);
    de->inode = 0;
    if (bfrus_wr(f, block, 1, buf) < 0)
        return -1;

    /* Check if the block is now all-empty; if so free it */
    int all_empty = 1;
    for (u64 i = 0; i < BFRUS_BLOCK_SIZE / BFRUS_DIR_ENTRY_SIZE; i++) {
        struct bfrus_dirent *d2 = (struct bfrus_dirent *)(buf + i * BFRUS_DIR_ENTRY_SIZE);
        if (d2->inode != 0) {
            all_empty = 0;
            break;
        }
    }
    if (all_empty) {
        bfrus_free_block(f, block);
        /* Shrink directory size if this was the last block */
        u64 last_block = in.size / BFRUS_BLOCK_SIZE;
        if (block == f->data_start + last_block - 1) {
            in.size -= BFRUS_BLOCK_SIZE;
            in.blocks = bfrus_blocks_for(in.size);
            if (bfrus_write_inode(f, dir_ino, &in) < 0)
                return -1;
        }
    }
    return 0;
}

static int bfrus_file_create(struct node *dir, const char *name, int isdir, node *out) {
    struct bfrus *f = dir->fs;
    if (bfrus_dir_find(f, (u32)dir->priv, name))
        return -1;
    u32 ino = bfrus_alloc_inode(f);
    struct bfrus_inode in;
    memset(&in, 0, sizeof in);
    in.mode = isdir ? 0x10 : 0x20;
    in.size = 0;
    in.blocks = 0;
    in.ext_count = 0;
    int h, m, s, d, mo, y;
    if (rtc_get(&h, &m, &s, &d, &mo, &y)) {
        in.mtime = 0;
        in.atime = 0;
        in.ctime = 0;
    } else {
        u64 t = (u64)y * 10000000000ULL + (u64)mo * 100000000ULL +
                (u64)d * 1000000ULL + (u64)h * 10000ULL +
                (u64)m * 100ULL + (u64)s;
        in.mtime = t;
        in.atime = t;
        in.ctime = t;
    }
    if (bfrus_write_inode(f, ino, &in) < 0)
        return -1;
    u64 dir_off = 0;
    if (bfrus_dir_add(f, (u32)dir->priv, ino, name, &dir_off) < 0) {
        struct bfrus_inode empty;
        memset(&empty, 0, sizeof empty);
        bfrus_write_inode(f, ino, &empty);
        return -1;
    }
    out->priv = ino;
    out->priv2 = dir_off;
    out->fs = f;
    out->size = 0;
    return 0;
}

static int bfrus_file_unlink(struct node *n) {
    struct bfrus *f = n->fs;
    struct bfrus_inode in;
    if (bfrus_read_inode(f, (u32)n->priv, &in) < 0)
        return -1;
    if (in.mode & 0x10) {
        u64 off = 0;
        while (off < in.size) {
            u64 block = f->data_start + off / BFRUS_BLOCK_SIZE;
            u32 boff = off % BFRUS_BLOCK_SIZE;
            u8 buf[BFRUS_BLOCK_SIZE];
            if (bfrus_rd(f, block, 1, buf) < 0)
                return -1;
            u64 remaining = in.size - off;
            u32 avail = BFRUS_BLOCK_SIZE - boff;
            if (avail > remaining)
                avail = remaining;
            u64 pos = 0;
            while (pos + BFRUS_DIR_ENTRY_SIZE <= avail) {
                struct bfrus_dirent *de = (struct bfrus_dirent *)(buf + boff + pos);
                if (de->inode != 0)
                    return -1;
                pos += BFRUS_DIR_ENTRY_SIZE;
            }
            off += avail;
        }
    }
    for (u32 i = 0; i < in.ext_count; i++)
        for (u64 j = 0; j < in.extents[i].count; j++)
            bfrus_free_block(f, in.extents[i].start + j);
    struct bfrus_inode empty;
    memset(&empty, 0, sizeof empty);
    if (bfrus_write_inode(f, (u32)n->priv, &empty) < 0)
        return -1;
    if (bfrus_dir_remove(f, (u32)n->par->priv, n->priv2) < 0)
        return -1;
    return 0;
}

static const struct fsops bfrus_ops = {
    bfrus_file_read,
    bfrus_file_write,
    bfrus_dir_load,
    bfrus_file_create,
    bfrus_file_unlink,
};

static int __attribute__((unused)) bfrus_write_super(struct bfrus *f) {
    u8 buf[BFRUS_BLOCK_SIZE];
    memset(buf, 0, BFRUS_BLOCK_SIZE);
    struct bfrus_super *sb = (struct bfrus_super *)buf;
    sb->magic = BFRUS_MAGIC;
    sb->version = BFRUS_VERSION;
    sb->block_size = BFRUS_BLOCK_SIZE;
    sb->block_count = f->block_count;
    sb->bitmap_start = f->bitmap_start;
    sb->bitmap_blocks = f->bitmap_blocks;
    sb->inode_start = f->inode_start;
    sb->inode_count = f->inode_count;
    sb->inode_blocks = f->inode_blocks;
    sb->data_start = f->data_start;
    sb->root_inode = f->root_inode;
    sb->backup_sb_block = f->block_count - 2;
    u32 sum = 0;
    u8 *p = buf;
    for (u32 i = 0; i < 508; i++)
        sum += p[i];
    sb->checksum = sum;
    return bfrus_wr(f, 0, 1, buf);
}

static int bfrus_read_super(struct bfrus *f, struct bfrus_super *sb) {
    u8 buf[BFRUS_BLOCK_SIZE];
    if (bfrus_rd(f, 0, 1, buf) < 0)
        return -1;
    memcpy(sb, buf, sizeof *sb);
    if (sb->magic != BFRUS_MAGIC || sb->version != BFRUS_VERSION ||
        sb->block_size != BFRUS_BLOCK_SIZE)
        return -1;
    return 0;
}

struct bfrus *bfrus_mount(int d, u64 start, u64 size, node *at) {
    struct bfrus *f = kzalloc(sizeof *f);
    if (!f)
        return NULL;
    f->d = d;
    f->start = start;
    f->block_count = size;
    struct bfrus_super sb;
    if (bfrus_read_super(f, &sb) < 0) {
        kfree(f);
        return NULL;
    }
    f->bitmap_start = sb.bitmap_start;
    f->bitmap_blocks = sb.bitmap_blocks;
    f->inode_start = sb.inode_start;
    f->inode_count = sb.inode_count;
    f->inode_blocks = sb.inode_blocks;
    f->data_start = sb.data_start;
    f->root_inode = sb.root_inode;
    u64 bitmap_bytes = (f->block_count + 7) / 8;
    f->bitmap_blocks = bfrus_blocks_for(bitmap_bytes);
    f->bitmap = kalloc((u64)f->bitmap_blocks * BFRUS_BLOCK_SIZE);
    if (!f->bitmap) {
        kfree(f);
        return NULL;
    }
    if (bfrus_rd(f, f->bitmap_start, f->bitmap_blocks, f->bitmap) < 0) {
        kfree(f->bitmap);
        kfree(f);
        return NULL;
    }
    f->dirty = 0;
    at->ops = &bfrus_ops;
    at->fs = f;
    at->priv = f->root_inode;
    at->loaded = 0;
    at->kid = NULL;
    return f;
}

int bfrus_format(int d, u64 start, u64 size, const char *label) {
    (void)label;
    if (size < 65536)
        return -1;
    u64 spb = BFRUS_BLOCK_SIZE / 512;   /* sectors per block */
    u64 block_count = size / spb;       /* convert sectors to blocks */
    u64 reserved = 4;
    u64 bitmap_blocks = bfrus_blocks_for((block_count + 7) / 8);
    if (bitmap_blocks == 0)
        bitmap_blocks = 1;
    u32 inode_count = 4096;
    u32 inode_blocks = bfrus_blocks_for((u64)inode_count * BFRUS_INODE_SIZE);
    u64 data_start = reserved + bitmap_blocks + inode_blocks;
    u8 *buf = kzalloc(BFRUS_BLOCK_SIZE);
    if (!buf)
        return -1;
    struct bfrus_super *sb = (struct bfrus_super *)buf;
    sb->magic = BFRUS_MAGIC;
    sb->version = BFRUS_VERSION;
    sb->block_size = BFRUS_BLOCK_SIZE;
    sb->block_count = block_count;
    sb->bitmap_start = reserved;
    sb->bitmap_blocks = bitmap_blocks;
    sb->inode_start = reserved + bitmap_blocks;
    sb->inode_count = inode_count;
    sb->inode_blocks = inode_blocks;
    sb->data_start = data_start;
    sb->root_inode = 1;
    sb->backup_sb_block = block_count - 2;
    u32 sum = 0;
    u8 *p = buf;
    for (u32 i = 0; i < 508; i++)
        sum += p[i];
    sb->checksum = sum;
    if (disk_write(d, start, spb, buf) < 0) {
        kfree(buf);
        return -1;
    }
    u8 *zero = kzalloc(BFRUS_BLOCK_SIZE);
    if (!zero) {
        kfree(buf);
        return -1;
    }
    for (u64 i = 0; i < bitmap_blocks; i++)
        if (disk_write(d, start + (reserved + i) * spb, spb, zero) < 0) {
            kfree(zero);
            kfree(buf);
            return -1;
        }
    u8 ibuf[BFRUS_BLOCK_SIZE];
    memset(ibuf, 0, BFRUS_BLOCK_SIZE);
    struct bfrus_inode *ri = (struct bfrus_inode *)ibuf;
    ri->mode = 0x10;
    ri->size = 2 * BFRUS_DIR_ENTRY_SIZE;   /* . and .. entries */
    ri->blocks = 1;
    ri->ext_count = 1;
    ri->extents[0].start = data_start;
    ri->extents[0].count = 1;
    if (disk_write(d, start + (reserved + bitmap_blocks) * spb, spb, ibuf) < 0) {
        kfree(zero);
        kfree(buf);
        return -1;
    }
    u8 *dir_block = kzalloc(BFRUS_BLOCK_SIZE);
    if (!dir_block) {
        kfree(zero);
        kfree(buf);
        return -1;
    }
    /* Root directory block: add . and .. entries */
    struct bfrus_dirent *de = (struct bfrus_dirent *)dir_block;
    de->inode = 1;                      /* . = root itself */
    de->namelen = 1;
    de->name[0] = '.';
    de = (struct bfrus_dirent *)(dir_block + BFRUS_DIR_ENTRY_SIZE);
    de->inode = 1;                      /* .. = root itself */
    de->namelen = 2;
    de->name[0] = '.';
    de->name[1] = '.';
    if (disk_write(d, start + data_start * spb, spb, dir_block) < 0) {
        kfree(dir_block);
        kfree(zero);
        kfree(buf);
        return -1;
    }
    kfree(dir_block);

    /* Write backup superblock at end of volume */
    u8 *sb_backup = kzalloc(BFRUS_BLOCK_SIZE);
    if (!sb_backup) {
        kfree(zero);
        kfree(buf);
        return -1;
    }
    memcpy(sb_backup, buf, BFRUS_BLOCK_SIZE);
    if (disk_write(d, start + (block_count - 2) * spb, spb, sb_backup) < 0) {
        kfree(sb_backup);
        kfree(zero);
        kfree(buf);
        return -1;
    }

    kfree(sb_backup);
    kfree(zero);
    kfree(buf);
    return 0;
}
