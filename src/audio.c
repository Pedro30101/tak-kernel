#include "tak.h"
#include "pci.h"
#include "audio.h"

#define BUFB 16384
#define NBUF 32
#define RB (BUFB * NBUF)
#define TARGET 36864

char audio_name[48] = "none";
static u16 nam, nabm;
static int ok, running;
static u8 *ring;
static u64 ring_pa, bdl_pa;
static u32 wpos, zpos;

int audio_present(void) { return ok; }

int audio_init(void) {
    for (unsigned bus = 0; bus < 256 && !ok; bus++)
        for (unsigned dev = 0; dev < 32 && !ok; dev++) {
            if ((u16)pci_r32(bus, dev, 0, 0) == 0xFFFF) continue;
            unsigned nf = (pci_r8(bus, dev, 0, 0x0E) & 0x80) ? 8 : 1;
            for (unsigned fn = 0; fn < nf && !ok; fn++) {
                u32 id = pci_r32(bus, dev, fn, 0);
                if ((u16)id == 0xFFFF) continue;
                u32 ci = pci_r32(bus, dev, fn, 8);
                if ((u8)(ci >> 24) != 4 || (u8)(ci >> 16) != 1) continue;
                u32 b0 = pci_r32(bus, dev, fn, 0x10), b1 = pci_r32(bus, dev, fn, 0x14);
                if (!(b0 & 1) || !(b1 & 1)) { klog("audio: %04x:%04x is not AC97 (HDA?), skipped\n", (u16)id, id >> 16); continue; }
                nam = (u16)(b0 & ~3u);
                nabm = (u16)(b1 & ~3u);
                u16 cmd = (u16)pci_r32(bus, dev, fn, 4);
                pci_w16(bus, dev, fn, 4, cmd | 5);
                outl(nabm + 0x2C, 2);
                int ready = 0;
                for (int i = 0; i < 400000 && !ready; i++) ready = inl(nabm + 0x30) & 0x100;
                if (!ready) { klog("audio: codec not ready\n"); continue; }
                outw(nam + 0x00, 0);
                outw(nam + 0x02, 0x0000);
                outw(nam + 0x18, 0x0808);
                ring = (u8 *)palloc_n(RB / 4096);
                u8 *bdl = (u8 *)palloc();
                if (!ring || !bdl) { klog("audio: no memory\n"); return 0; }
                memset(ring, 0, RB);
                ring_pa = (u64)ring;
                bdl_pa = (u64)bdl;
                for (int i = 0; i < NBUF; i++) {
                    ((u32 *)bdl)[i * 2] = (u32)(ring_pa + (u64)i * BUFB);
                    ((u32 *)bdl)[i * 2 + 1] = BUFB / 2;
                }
                snprintf(audio_name, sizeof audio_name, "AC97 %04x:%04x", (u16)id, id >> 16);
                ok = 1;
                klog("audio: %s nam=%x nabm=%x\n", audio_name, nam, nabm);
            }
        }
    return ok;
}

static u32 hwpos(void) {
    u8 civ = inb(nabm + 0x14);
    u16 picb = inw(nabm + 0x18);
    u32 rem = (u32)picb * 2;
    if (rem > BUFB) rem = BUFB;
    return (u32)civ * BUFB + (BUFB - rem);
}

void audio_start(void) {
    if (!ok) return;
    outb(nabm + 0x1B, 2);
    for (int i = 0; i < 100000 && (inb(nabm + 0x1B) & 2); i++);
    outl(nabm + 0x10, (u32)bdl_pa);
    outb(nabm + 0x15, NBUF - 1);
    outb(nabm + 0x1B, 1);
    running = 1;
}

void audio_pause(int on) {
    if (!ok) return;
    outb(nabm + 0x1B, on ? 0 : 1);
    running = !on;
}

void audio_stop(void) {
    if (!ok) return;
    outb(nabm + 0x1B, 2);
    for (int i = 0; i < 100000 && (inb(nabm + 0x1B) & 2); i++);
    memset(ring, 0, RB);
    wpos = zpos = 0;
    running = 0;
}

void audio_poll(void) {
    if (!ok || !running) return;
    u8 civ = inb(nabm + 0x14);
    outb(nabm + 0x15, (u8)((civ + NBUF - 1) % NBUF));
    u32 hp = hwpos();
    while (zpos != hp) {
        u32 n = hp > zpos ? hp - zpos : RB - zpos;
        memset(ring + zpos, 0, n);
        zpos = (zpos + n) % RB;
    }
    if (inw(nabm + 0x16) & 1) outb(nabm + 0x1B, 1);
}

int audio_buffered(void) {
    if (!ok) return 0;
    return (int)((wpos + RB - hwpos()) % RB);
}

int audio_space(void) {
    if (!ok) return 0;
    int b = audio_buffered();
    if (b >= TARGET) return 0;
    return (TARGET - b) / 4;
}

void audio_put(const i16 *f, int n) {
    if (!ok) return;
    u32 bytes = (u32)n * 4;
    const u8 *s = (const u8 *)f;
    while (bytes) {
        u32 c = RB - wpos < bytes ? RB - wpos : bytes;
        memcpy(ring + wpos, s, c);
        wpos = (wpos + c) % RB;
        s += c; bytes -= c;
    }
}

void audio_volume(int pct) {
    if (!ok) return;
    int att = 31 - pct * 31 / 100;
    if (pct <= 0) { outw(nam + 0x02, 0x8000); return; }
    outw(nam + 0x02, (u16)((att << 8) | att));
}
