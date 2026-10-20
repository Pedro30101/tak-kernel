#ifndef DISK_H
#define DISK_H
#include "tak.h"
#include "vfs.h"

struct disk {
    char name[8], model[44];
    int chan, dev, ok;
    u64 sectors;
    int lba48;
};

struct part {
    int disk;
    u64 start, size;
    u8 type, boot;
};

#define MAXD 4

extern struct disk disks[MAXD];
extern int ndisks;

void disk_init(void);
int disk_read(int d, u64 lba, u32 n, void *buf);
int disk_write(int d, u64 lba, u32 n, const void *buf);
int disk_parts(int d, struct part *out, int max);

struct fat;
struct fat *fat_mount(int d, u64 start, u64 size, node *at);
int fat_format(int d, u64 start, u64 size, const char *label);
extern struct fat *fat_main;

struct bfrus;
struct bfrus *bfrus_mount(int d, u64 start, u64 size, node *at);
int bfrus_format(int d, u64 start, u64 size, const char *label);
extern struct bfrus *bfrus_main;

extern u8 *initrd_ptr;
extern u64 initrd_len;
extern u8 *kimg_ptr;
extern u64 kimg_len;
#endif
