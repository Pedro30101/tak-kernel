#ifndef PCI_H
#define PCI_H
#include "tak.h"
u32 pci_r32(u8 bus, u8 dev, u8 fn, u8 off);
u8 pci_r8(u8 bus, u8 dev, u8 fn, u8 off);
void pci_w16(u8 bus, u8 dev, u8 fn, u8 off, u16 v);
void pci_w32(u8 bus, u8 dev, u8 fn, u8 off, u32 v);
int pci_map_uncached(u64 address);
#endif
