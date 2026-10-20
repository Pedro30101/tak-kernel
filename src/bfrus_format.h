#ifndef BFRUS_FORMAT_H
#define BFRUS_FORMAT_H

#include "tak.h"

#define BFRUS_MAGIC 0x42524655 /* "BFRU" */
#define BFRUS_VERSION 1
#define BFRUS_BLOCK_SIZE 4096
#define BFRUS_NAME_MAX 63
#define BFRUS_INODE_SIZE 128
#define BFRUS_DIR_ENTRY_SIZE 68
#define BFRUS_MAX_EXTENTS 8

struct bfrus_super {
    u32 magic;
    u32 version;
    u32 block_size;
    u64 block_count;
    u64 bitmap_start;
    u32 bitmap_blocks;
    u64 inode_start;
    u32 inode_count;
    u32 inode_blocks;
    u64 data_start;
    u32 root_inode;
    u64 backup_sb_block;
    u32 checksum;
    u8 pad[480];
} __attribute__((packed));

struct bfrus_extent {
    u32 start;
    u32 count;
};

struct bfrus_inode {
    u32 mode;
    u64 size;
    u32 blocks;
    u32 ext_count;
    struct bfrus_extent extents[BFRUS_MAX_EXTENTS];
    u64 mtime;
    u64 atime;
    u64 ctime;
    u8 pad[20];
} __attribute__((packed));

struct bfrus_dirent {
    u32 inode;
    u8 namelen;
    char name[BFRUS_NAME_MAX];
} __attribute__((packed));

#endif
