#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>
void *kalloc(size_t n);
void *krealloc(void *p, size_t n);
void kfree(void *p);
static inline int abs(int x) { return x < 0 ? -x : x; }
#endif
