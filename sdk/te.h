#ifndef TE_H
#define TE_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
typedef signed char i8;
typedef short i16;
typedef int i32;
typedef long i64;
typedef unsigned long size_t;
#define NULL ((void *)0)

static inline long sc(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "memory");
    return r;
}

enum { SYS_EXIT, SYS_WRITE, SYS_READ, SYS_SLEEP, SYS_YIELD, SYS_PID, SYS_MS, SYS_WNEW, SYS_WFILL, SYS_WTEXT,
       SYS_WBLIT, SYS_WFLIP, SYS_WEV, SYS_WCLOSE, SYS_WTITLE, SYS_WSIZE, SYS_FREAD, SYS_FWRITE, SYS_READDIR,
       SYS_SPAWN, SYS_SBRK };

static inline void exit(int c) { sc(SYS_EXIT, c, 0, 0, 0, 0, 0); for (;;); }
static inline long write(int fd, const void *b, long n) { return sc(SYS_WRITE, fd, (long)b, n, 0, 0, 0); }
static inline long read(int fd, void *b, long n) { return sc(SYS_READ, fd, (long)b, n, 0, 0, 0); }
static inline void sleep_ms(long ms) { sc(SYS_SLEEP, ms, 0, 0, 0, 0, 0); }
static inline void yield(void) { sc(SYS_YIELD, 0, 0, 0, 0, 0, 0); }
static inline int getpid(void) { return (int)sc(SYS_PID, 0, 0, 0, 0, 0, 0); }
static inline long uptime(void) { return sc(SYS_MS, 0, 0, 0, 0, 0, 0); }
static inline void *sbrk(long n) { return (void *)sc(SYS_SBRK, n, 0, 0, 0, 0, 0); }
static inline long file_read(const char *p, void *b, long max) { return sc(SYS_FREAD, (long)p, (long)b, 0, max, 0, 0); }
static inline long file_write(const char *p, const void *b, long n) { return sc(SYS_FWRITE, (long)p, (long)b, n, 0, 0, 0); }
static inline long dir_entry(const char *p, long i, char *name) { return sc(SYS_READDIR, (long)p, i, (long)name, 0, 0, 0); }
static inline long spawn(const char *p, const char *arg) { return sc(SYS_SPAWN, (long)p, (long)arg, 0, 0, 0, 0); }

void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strchr(const char *s, int c);
int atoi(const char *s);
int abs(int x);
void srand(unsigned s);
int rand(void);
void *malloc(size_t n);
void *calloc(size_t a, size_t b);
void *realloc(void *p, size_t n);
void free(void *p);
int snprintf(char *d, size_t n, const char *f, ...);
int printf(const char *f, ...);
int puts(const char *s);
int getline(char *buf, int max);

int main(const char *arg);

#endif
