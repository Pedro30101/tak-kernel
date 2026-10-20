#ifndef TAK_H
#define TAK_H
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

#define VER "0.9.6b (0.9.6 Beta)"

static inline u8 inb(u16 p) { u8 v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline u16 inw(u16 p) { u16 v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline u32 inl(u16 p) { u32 v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outb(u16 p, u8 v) { __asm__ volatile("outb %0,%1" : : "a"(v), "Nd"(p)); }
static inline void outw(u16 p, u16 v) { __asm__ volatile("outw %0,%1" : : "a"(v), "Nd"(p)); }
static inline void outl(u16 p, u32 v) { __asm__ volatile("outl %0,%1" : : "a"(v), "Nd"(p)); }
static inline void cli(void) { __asm__ volatile("cli" : : : "memory"); }
static inline void sti(void) { __asm__ volatile("sti" : : : "memory"); }
static inline void halt(void) { __asm__ volatile("sti; hlt" : : : "memory"); }
static inline u64 irqsave(void) { u64 f; __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) : : "memory"); return f; }
static inline void irqrest(u64 f) { if (f & 0x200) sti(); }

void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
int atoi(const char *s);
int tolower(int c);
int toupper(int c);
int vsnprintf(char *d, size_t n, const char *f, va_list ap);
int snprintf(char *d, size_t n, const char *f, ...);

void ser_init(void);
void ser_putc(char c);
void klog(const char *f, ...);

extern u32 *mbi;
void *mb_tag(u32 type);
int mb_mod(int i, u64 *start, u64 *end, const char **name);

void mem_init(void);
u64 palloc(void);
u64 palloc_n(u32 n);
void pfree(u64 pa);
void *kalloc(size_t n);
void *kzalloc(size_t n);
void *krealloc(void *p, size_t n);
void kfree(void *p);
u64 mem_free_kb(void);
u64 mem_total_kb(void);
u64 heap_free_kb(void);

void cpu_init(void);
void irq_set(int n, void (*fn)(void));
extern volatile u64 ticks;
u64 uptime_ms(void);

void panic(const char *f, ...) __attribute__((noreturn));

#endif
