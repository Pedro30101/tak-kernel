#include "pci.h"

static u32 addr(u8 b, u8 d, u8 f, u8 o) {
    return 0x80000000u | ((u32)b << 16) | ((u32)d << 11) | ((u32)f << 8) | (o & 0xFC);
}

u32 pci_r32(u8 b, u8 d, u8 f, u8 o) {
    outl(0xCF8, addr(b, d, f, o));
    return inl(0xCFC);
}

u8 pci_r8(u8 b, u8 d, u8 f, u8 o) { return (u8)(pci_r32(b, d, f, o) >> ((o & 3) * 8)); }

void pci_w16(u8 b, u8 d, u8 f, u8 o, u16 v) {
    outl(0xCF8, addr(b, d, f, o));
    outw((u16)(0xCFC + (o & 2)), v);
}

void pci_w32(u8 b, u8 d, u8 f, u8 o, u32 v) {
    outl(0xCF8, addr(b, d, f, o));
    outl(0xCFC, v);
}
