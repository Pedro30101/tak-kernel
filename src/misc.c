#include "tak.h"
#include "wm.h"
#include "gfx.h"

static u8 cmos(u8 r) { outb(0x70, r); return inb(0x71); }

int rtc_get(int *h, int *m, int *s, int *d, int *mo, int *y) {
    for (int i = 0; i < 20000 && (cmos(0x0A) & 0x80); i++);
    u8 sec = cmos(0), min = cmos(2), hr = cmos(4), day = cmos(7), mon = cmos(8), yr = cmos(9), b = cmos(0x0B);
    if (!(b & 4)) {
        sec = (sec & 15) + (sec >> 4) * 10;
        min = (min & 15) + (min >> 4) * 10;
        hr = ((hr & 15) + ((hr & 0x70) >> 4) * 10) | (hr & 0x80);
        day = (day & 15) + (day >> 4) * 10;
        mon = (mon & 15) + (mon >> 4) * 10;
        yr = (yr & 15) + (yr >> 4) * 10;
    }
    if (!(b & 2) && (hr & 0x80)) hr = ((hr & 0x7F) + 12) % 24;
    *s = sec; *m = min; *h = hr; *d = day; *mo = mon; *y = 2000 + yr;
    return mon >= 1 && mon <= 12 && day >= 1 && day <= 31;
}

void sys_reboot(void) {
    cli();
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++);
    outb(0x64, 0xFE);
    struct { u16 l; u64 b; } __attribute__((packed)) z = { 0, 0 };
    __asm__ volatile("lidt %0; int3" : : "m"(z));
    for (;;) __asm__ volatile("hlt");
}

void sys_poweroff(void) {
    cli();
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    fill(&scr, 0, 0, sw, sh, 0x000000);
    const char *t = "It is now safe to turn off your computer.";
    text2(&scr, (sw - (int)strlen(t) * 16) / 2, sh / 2 - 16, t, 0xFB923C);
    present(0, 0, sw, sh);
    for (;;) __asm__ volatile("hlt");
}

void sys_halt(void) {
    cli();
    fill(&scr, 0, 0, sw, sh, 0x000000);
    const char *t = "TAK halted. You can now restart or shutdown your machine";
    text2(&scr, (sw - (int)strlen(t) * 16) / 2, sh / 2 - 16, t, 0xE5E7EB);
    present(0, 0, sw, sh);
    for (;;) __asm__ volatile("hlt");
}
