#include "tak.h"

void *memcpy(void *d, const void *s, size_t n) {
    void *r = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}

void *memmove(void *d, const void *s, size_t n) {
    u8 *a = d;
    const u8 *b = s;
    if (a == b || n == 0) return d;
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
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (u8)*a - (u8)*b : 0;
}

int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

int strcasecmp(const char *a, const char *b) {
    while (*a && tolower(*a) == tolower(*b)) { a++; b++; }
    return tolower((u8)*a) - tolower((u8)*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    while (n && *a && tolower(*a) == tolower(*b)) { a++; b++; n--; }
    return n ? tolower((u8)*a) - tolower((u8)*b) : 0;
}

char *strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++));
    return r;
}

char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

char *strchr(const char *s, int c) {
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return c == 0 ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *r = NULL;
    for (; *s; s++) if (*s == (char)c) r = s;
    return (char *)r;
}

int atoi(const char *s) {
    int n = 0, neg = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return neg ? -n : n;
}

static char *putnum(char *p, char *end, u64 v, int base, int upper, int width, int zero, int left, int neg) {
    char tmp[24];
    int n = 0;
    const char *dg = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { tmp[n++] = dg[v % base]; v /= base; } while (v);
    int len = n + neg;
    int pad = width > len ? width - len : 0;
    if (!left && !zero) while (pad-- > 0) { if (p < end) *p = ' '; p++; }
    if (neg) { if (p < end) *p = '-'; p++; }
    if (!left && zero) while (pad-- > 0) { if (p < end) *p = '0'; p++; }
    while (n) { if (p < end) *p = tmp[--n]; p++; }
    if (left) while (pad-- > 0) { if (p < end) *p = ' '; p++; }
    return p;
}

int vsnprintf(char *d, size_t n, const char *f, va_list ap) {
    char *p = d, *end = n ? d + n - 1 : d;
    for (; *f; f++) {
        if (*f != '%') { if (p < end) *p = *f; p++; continue; }
        f++;
        int left = 0, zero = 0, width = 0, lng = 0;
        for (;; f++) {
            if (*f == '-') left = 1;
            else if (*f == '0') zero = 1;
            else break;
        }
        if (*f == '*') { width = va_arg(ap, int); f++; }
        else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        while (*f == 'l' || *f == 'z') { lng++; f++; }
        switch (*f) {
        case 'd': case 'i': {
            i64 v = lng ? va_arg(ap, i64) : va_arg(ap, int);
            int neg = v < 0;
            p = putnum(p, end, neg ? -(u64)v : (u64)v, 10, 0, width, zero, left, neg);
            break;
        }
        case 'u': p = putnum(p, end, lng ? va_arg(ap, u64) : va_arg(ap, u32), 10, 0, width, zero, left, 0); break;
        case 'x': p = putnum(p, end, lng ? va_arg(ap, u64) : va_arg(ap, u32), 16, 0, width, zero, left, 0); break;
        case 'X': p = putnum(p, end, lng ? va_arg(ap, u64) : va_arg(ap, u32), 16, 1, width, zero, left, 0); break;
        case 'p': if (p < end) *p = '0'; p++; if (p < end) *p = 'x'; p++;
                  p = putnum(p, end, (u64)va_arg(ap, void *), 16, 0, 0, 0, 0, 0); break;
        case 'c': { char c = (char)va_arg(ap, int); if (p < end) *p = c; p++; break; }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)strlen(s);
            int pad = width > len ? width - len : 0;
            if (!left) while (pad-- > 0) { if (p < end) *p = ' '; p++; }
            while (*s) { if (p < end) *p = *s; p++; s++; }
            if (left) while (pad-- > 0) { if (p < end) *p = ' '; p++; }
            break;
        }
        case '%': if (p < end) *p = '%'; p++; break;
        case 0: f--; break;
        default: if (p < end) *p = '%'; p++; if (p < end) *p = *f; p++; break;
        }
    }
    if (n) *(p < end ? p : end) = 0;
    return (int)(p - d);
}

int snprintf(char *d, size_t n, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    int r = vsnprintf(d, n, f, ap);
    va_end(ap);
    return r;
}
