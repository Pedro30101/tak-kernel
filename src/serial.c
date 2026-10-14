#include "tak.h"

#define COM1 0x3F8

void ser_init(void) {
    outb(COM1 + 1, 0);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 1);
    outb(COM1 + 1, 0);
    outb(COM1 + 3, 3);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

void ser_putc(char c) {
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++);
    outb(COM1, (u8)c);
}

void klog(const char *f, ...) {
    char b[256];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    for (char *p = b; *p; p++) {
        if (*p == '\n') ser_putc('\r');
        ser_putc(*p);
    }
}
