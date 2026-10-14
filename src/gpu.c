#include "tak.h"
#include "pci.h"

char gpu_name[64] = "Unknown display adapter";
u16 gpu_ven, gpu_dev;
int gpu_kind;

void gpu_init(void) {
    for (unsigned bus = 0; bus < 256; bus++)
        for (unsigned dev = 0; dev < 32; dev++) {
            if ((u16)pci_r32(bus, dev, 0, 0) == 0xFFFF) continue;
            unsigned nf = (pci_r8(bus, dev, 0, 0x0E) & 0x80) ? 8 : 1;
            for (unsigned fn = 0; fn < nf; fn++) {
                u32 id = pci_r32(bus, dev, fn, 0);
                if ((u16)id == 0xFFFF) continue;
                u32 ci = pci_r32(bus, dev, fn, 8);
                if ((u8)(ci >> 24) != 3) continue;
                gpu_ven = (u16)id;
                gpu_dev = id >> 16;
                const char *v = "Unknown";
                gpu_kind = 0;
                switch (gpu_ven) {
                case 0x8086: v = "Intel"; gpu_kind = 1; break;
                case 0x10DE: v = "NVIDIA"; gpu_kind = 2; break;
                case 0x1002: case 0x1022: v = "AMD"; gpu_kind = 3; break;
                case 0x1234: v = "QEMU/Bochs VGA"; gpu_kind = 4; break;
                case 0x1AF4: v = "virtio GPU"; gpu_kind = 5; break;
                case 0x15AD: v = "VMware SVGA"; gpu_kind = 6; break;
                case 0x80EE: v = "VirtualBox"; gpu_kind = 7; break;
                }
                snprintf(gpu_name, sizeof gpu_name, "%s [%04x:%04x]", v, gpu_ven, gpu_dev);
                klog("gpu: %s\n", gpu_name);
                return;
            }
        }
}
