#include "te.h"
#include <stdarg.h>

__asm__(
    ".section .te_hdr,\"a\"\n"
    ".ascii \"TE02\"\n"
    ".long _start\n"
    ".long __te_fend - 0x40000000\n"
    ".long __te_end - 0x40000000\n"
    ".long __te_data_start - 0x40000000\n"
    ".skip 32\n"
    ".section .text._start,\"ax\"\n"
    ".global _start\n"
    "_start:\n"
    "    andq $-16, %rsp\n"
    "    call te_start\n"
    "    ud2\n"
);

void te_start(const char *arg) { exit(main(arg)); }

void *memcpy(void *d, const void *s, size_t n) {
    void *r = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}

void *memmove(void *d, const void *s, size_t n) {
    u8 *a = d;
    const u8 *b = s;
    if (a == b || !n) return d;
    if (a < b || a >= b + n) return memcpy(d, s, n);
    while (n--) a[n] = b[n];
    return d;
}

void *memset(void *d, int c, size_t n) {
    void *r = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}

int memcmp(const void *a, const void *b, size_t n) {
    const u8 *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (u8)*a - (u8)*b; }
int strncmp(const char *a, const char *b, size_t n) { while (n && *a && *a == *b) { a++; b++; n--; } return n ? (u8)*a - (u8)*b : 0; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)); return r; }
char *strchr(const char *s, int c) { for (; *s; s++) if (*s == (char)c) return (char *)s; return c ? NULL : (char *)s; }
int abs(int x) { return x < 0 ? -x : x; }

int atoi(const char *s) {
    int n = 0, neg = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return neg ? -n : n;
}

static unsigned rs = 12345;
void srand(unsigned s) { rs = s ? s : 1; }
int rand(void) { rs = rs * 1103515245 + 12345; return (rs >> 16) & 0x7FFF; }

struct blk { size_t sz, free; struct blk *prev, *next; };
#define HDR 32
static struct blk *head, *tail;

void *malloc(size_t n) {
    n = (n + 15) & ~15UL;
    if (!n) n = 16;
    for (struct blk *b = head; b; b = b->next) {
        if (!b->free || b->sz < n) continue;
        if (b->sz >= n + HDR + 32) {
            struct blk *r = (struct blk *)((u8 *)b + HDR + n);
            r->sz = b->sz - n - HDR; r->free = 1; r->prev = b; r->next = b->next;
            if (b->next) b->next->prev = r; else tail = r;
            b->next = r; b->sz = n;
        }
        b->free = 0;
        return (u8 *)b + HDR;
    }
    size_t need = n + HDR;
    size_t chunk = (need + 65535) & ~65535UL;
    void *m = sbrk((long)chunk);
    if ((long)m == -1) return NULL;
    struct blk *b = m;
    b->sz = chunk - HDR; b->free = 1; b->prev = tail; b->next = NULL;
    if (tail) tail->next = b; else head = b;
    tail = b;
    if (tail->prev && tail->prev->free) {
        tail->prev->sz += HDR + tail->sz;
        tail->prev->next = NULL;
        tail = tail->prev;
    }
    return malloc(n);
}

void free(void *p) {
    if (!p) return;
    struct blk *b = (struct blk *)((u8 *)p - HDR);
    b->free = 1;
    if (b->next && b->next->free) {
        b->sz += HDR + b->next->sz;
        b->next = b->next->next;
        if (b->next) b->next->prev = b; else tail = b;
    }
    if (b->prev && b->prev->free) {
        b->prev->sz += HDR + b->sz;
        b->prev->next = b->next;
        if (b->next) b->next->prev = b->prev; else tail = b->prev;
    }
}

void *calloc(size_t a, size_t b) { void *p = malloc(a * b); if (p) memset(p, 0, a * b); return p; }

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    struct blk *b = (struct blk *)((u8 *)p - HDR);
    if (b->sz >= n) return p;
    void *q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, b->sz);
    free(p);
    return q;
}

static char *putnum(char *p, char *end, u64 v, int base, int width, int zero, int left, int neg) {
    char tmp[24];
    int n = 0;
    do { tmp[n++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
    int pad = width > n + neg ? width - n - neg : 0;
    if (!left && !zero) while (pad-- > 0) { if (p < end) *p = ' '; p++; }
    if (neg) { if (p < end) *p = '-'; p++; }
    if (!left && zero) while (pad-- > 0) { if (p < end) *p = '0'; p++; }
    while (n) { if (p < end) *p = tmp[--n]; p++; }
    if (left) while (pad-- > 0) { if (p < end) *p = ' '; p++; }
    return p;
}

static int vfmt(char *d, size_t n, const char *f, va_list ap) {
    char *p = d, *end = n ? d + n - 1 : d;
    for (; *f; f++) {
        if (*f != '%') { if (p < end) *p = *f; p++; continue; }
        f++;
        int left = 0, zero = 0, width = 0, lng = 0;
        for (;; f++) { if (*f == '-') left = 1; else if (*f == '0') zero = 1; else break; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        while (*f == 'l') { lng++; f++; }
        switch (*f) {
        case 'd': { i64 v = lng ? va_arg(ap, i64) : va_arg(ap, int); int ng = v < 0; p = putnum(p, end, ng ? -(u64)v : (u64)v, 10, width, zero, left, ng); break; }
        case 'u': p = putnum(p, end, lng ? va_arg(ap, u64) : va_arg(ap, u32), 10, width, zero, left, 0); break;
        case 'x': p = putnum(p, end, lng ? va_arg(ap, u64) : va_arg(ap, u32), 16, width, zero, left, 0); break;
        case 'c': { char c = (char)va_arg(ap, int); if (p < end) *p = c; p++; break; }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)strlen(s), pad = width > len ? width - len : 0;
            if (!left) while (pad-- > 0) { if (p < end) *p = ' '; p++; }
            while (*s) { if (p < end) *p = *s; p++; s++; }
            if (left) while (pad-- > 0) { if (p < end) *p = ' '; p++; }
            break;
        }
        case '%': if (p < end) *p = '%'; p++; break;
        case 0: f--; break;
        default: break;
        }
    }
    if (n) *(p < end ? p : end) = 0;
    return (int)(p - d);
}

int snprintf(char *d, size_t n, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    int r = vfmt(d, n, f, ap);
    va_end(ap);
    return r;
}

int printf(const char *f, ...) {
    char b[512];
    va_list ap;
    va_start(ap, f);
    int r = vfmt(b, sizeof b, f, ap);
    va_end(ap);
    if (r >= (int)sizeof b) r = sizeof b - 1;
    write(1, b, r);
    return r;
}

int puts(const char *s) { write(1, s, (long)strlen(s)); write(1, "\n", 1); return 0; }

int getline(char *buf, int max) {
    int n = 0;
    for (;;) {
        char c;
        if (read(0, &c, 1) <= 0) return n ? n : -1;
        if (c == '\b') { if (n) { n--; write(1, "\b \b", 3); } continue; }
        if (c == '\n') { write(1, "\n", 1); buf[n] = 0; return n; }
        if (n < max - 1 && c >= 32) { buf[n++] = c; write(1, &c, 1); }
    }
}
