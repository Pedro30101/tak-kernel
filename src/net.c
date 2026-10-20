#include "net.h"
#include "pci.h"

#define E1000_VENDOR 0x8086
#define E1000_DEVICE 0x100E
#define E1000_REG_CTRL 0x0000
#define E1000_REG_STATUS 0x0008
#define E1000_REG_IMC 0x00D8
#define E1000_REG_RCTL 0x0100
#define E1000_REG_TCTL 0x0400
#define E1000_REG_TIPG 0x0410
#define E1000_REG_RDBAL 0x2800
#define E1000_REG_RDBAH 0x2804
#define E1000_REG_RDLEN 0x2808
#define E1000_REG_RDH 0x2810
#define E1000_REG_RDT 0x2818
#define E1000_REG_TDBAL 0x3800
#define E1000_REG_TDBAH 0x3804
#define E1000_REG_TDLEN 0x3808
#define E1000_REG_TDH 0x3810
#define E1000_REG_TDT 0x3818
#define E1000_REG_RAL0 0x5400
#define E1000_REG_RAH0 0x5404
#define E1000_REG_MTA 0x5200

#define RING_COUNT 16
#define FRAME_SIZE 2048
#define DMA_PAGES 18
#define RX_RING_OFFSET 0
#define TX_RING_OFFSET 4096
#define RX_BUFFER_OFFSET 8192
#define TX_BUFFER_OFFSET 40960
#define RX_BUDGET 16
#define MIN_ETH_FRAME 14
#define MAX_ETH_FRAME 1518

struct e1000_rx_desc {
    u64 address;
    u16 length;
    u16 checksum;
    u8 status;
    u8 errors;
    u16 special;
} __attribute__((packed));

struct e1000_tx_desc {
    u64 address;
    u16 length;
    u8 checksum_offset;
    u8 command;
    u8 status;
    u8 checksum_start;
    u16 special;
} __attribute__((packed));

static volatile u32 *regs;
static u8 *dma;
static u8 mac_address[6];
static u16 rx_head;
static u16 tx_tail;
static int initialized;
static u64 rx_count;
static u64 rx_drop_count;
static u64 tx_count;
static void (*rx_handler)(const u8 *frame, u16 length);

static u32 reg_read(u32 offset) {
    return regs[offset / sizeof(u32)];
}

static void reg_write(u32 offset, u32 value) {
    regs[offset / sizeof(u32)] = value;
}

static u64 dma_address(const volatile void *pointer) {
    return (u64)(uintptr_t)pointer;
}

static volatile struct e1000_rx_desc *rx_ring(void) {
    return (volatile struct e1000_rx_desc *)(dma + RX_RING_OFFSET);
}

static volatile struct e1000_tx_desc *tx_ring(void) {
    return (volatile struct e1000_tx_desc *)(dma + TX_RING_OFFSET);
}

static int find_adapter(u8 *bus_out, u8 *dev_out, u8 *fn_out) {
    for (unsigned bus = 0; bus < 256; bus++)
        for (unsigned dev = 0; dev < 32; dev++) {
            if ((u16)pci_r32((u8)bus, (u8)dev, 0, 0) == 0xFFFF) continue;
            u8 functions = (pci_r8((u8)bus, (u8)dev, 0, 0x0E) & 0x80) ? 8 : 1;
            for (unsigned fn = 0; fn < functions; fn++) {
                u32 id = pci_r32((u8)bus, (u8)dev, (u8)fn, 0);
                if ((u16)id == E1000_VENDOR && (u16)(id >> 16) == E1000_DEVICE) {
                    *bus_out = (u8)bus;
                    *dev_out = (u8)dev;
                    *fn_out = (u8)fn;
                    return 1;
                }
            }
        }
    return 0;
}

static int map_registers(u8 bus, u8 dev, u8 fn) {
    u32 low = pci_r32(bus, dev, fn, 0x10);
    if (low == 0xFFFFFFFF || (low & 1)) return 0;
    u32 type = (low >> 1) & 3;
    if (type != 0 && type != 2) return 0;

    u64 address = low & ~0xFULL;
    if (type == 2) {
        u32 high = pci_r32(bus, dev, fn, 0x14);
        address |= (u64)high << 32;
    }
    if (!address || address >= 0x100000000ULL || address > 0x100000000ULL - 0x6000)
        return 0;
    if (!pci_map_uncached(address)) return 0;

    u16 command = (u16)pci_r32(bus, dev, fn, 0x04);
    pci_w16(bus, dev, fn, 0x04, command | 0x0002);
    regs = (volatile u32 *)(uintptr_t)address;
    return 1;
}

static int init_dma(void) {
    dma = (u8 *)(uintptr_t)palloc_n(DMA_PAGES);
    if (!dma) return 0;
    memset(dma, 0, DMA_PAGES * 4096);

    volatile struct e1000_rx_desc *rx = rx_ring();
    volatile struct e1000_tx_desc *tx = tx_ring();
    for (u16 i = 0; i < RING_COUNT; i++) {
        rx[i].address = dma_address(dma + RX_BUFFER_OFFSET + (size_t)i * FRAME_SIZE);
        tx[i].address = dma_address(dma + TX_BUFFER_OFFSET + (size_t)i * FRAME_SIZE);
        tx[i].status = 1;
    }
    return 1;
}

int net_init(void) {
    u8 bus, dev, fn;
    if (initialized || !find_adapter(&bus, &dev, &fn)) return 0;
    if (!map_registers(bus, dev, fn)) {
        klog("e1000: unsupported or invalid MMIO BAR\n");
        return 0;
    }

    reg_write(E1000_REG_CTRL, reg_read(E1000_REG_CTRL) | (1u << 26));
    unsigned reset_wait = 100000;
    while ((reg_read(E1000_REG_CTRL) & (1u << 26)) && --reset_wait) { }
    if (!reset_wait) {
        klog("e1000: reset timed out\n");
        return 0;
    }
    reg_write(E1000_REG_IMC, 0xFFFFFFFF);

    if (!init_dma()) {
        klog("e1000: could not allocate DMA rings and buffers\n");
        return 0;
    }

    u32 rah = reg_read(E1000_REG_RAH0);
    if (!(rah & (1u << 31))) {
        for (u32 offset = 0; offset < DMA_PAGES * 4096; offset += 4096)
            pfree((u64)(uintptr_t)(dma + offset));
        dma = 0;
        klog("e1000: hardware MAC address is unavailable\n");
        return 0;
    }
    u32 ral = reg_read(E1000_REG_RAL0);
    for (unsigned i = 0; i < 4; i++) mac_address[i] = (u8)(ral >> (i * 8));
    for (unsigned i = 0; i < 2; i++) mac_address[i + 4] = (u8)(rah >> (i * 8));

    u64 rx_pa = dma_address(rx_ring());
    u64 tx_pa = dma_address(tx_ring());
    reg_write(E1000_REG_RDBAL, (u32)rx_pa);
    reg_write(E1000_REG_RDBAH, (u32)(rx_pa >> 32));
    reg_write(E1000_REG_RDLEN, RING_COUNT * sizeof(struct e1000_rx_desc));
    reg_write(E1000_REG_RDH, 0);
    reg_write(E1000_REG_RDT, RING_COUNT - 1);
    reg_write(E1000_REG_TDBAL, (u32)tx_pa);
    reg_write(E1000_REG_TDBAH, (u32)(tx_pa >> 32));
    reg_write(E1000_REG_TDLEN, RING_COUNT * sizeof(struct e1000_tx_desc));
    reg_write(E1000_REG_TDH, 0);
    reg_write(E1000_REG_TDT, 0);
    for (u32 offset = 0; offset < 128 * 4; offset += 4)
        reg_write(E1000_REG_MTA + offset, 0);
    reg_write(E1000_REG_TIPG, 0x0060200A);

    u16 command = (u16)pci_r32(bus, dev, fn, 0x04);
    pci_w16(bus, dev, fn, 0x04, command | 0x0006);
    __sync_synchronize();
    reg_write(E1000_REG_RCTL, (1u << 1) | (1u << 15) | (1u << 26));
    reg_write(E1000_REG_TCTL, 0x0004010A);
    initialized = 1;
    klog("e1000: initialized %02x:%02x:%02x:%02x:%02x:%02x; link %s\n",
         mac_address[0], mac_address[1], mac_address[2],
         mac_address[3], mac_address[4], mac_address[5],
         (reg_read(E1000_REG_STATUS) & (1u << 1)) ? "up" : "down");
    return 1;
}

void net_poll(void) {
    if (!initialized) return;
    volatile struct e1000_rx_desc *ring = rx_ring();
    for (unsigned processed = 0; processed < RX_BUDGET; processed++) {
        volatile struct e1000_rx_desc *desc = &ring[rx_head];
        if (!(desc->status & 1)) break;
        __sync_synchronize();
        u16 length = desc->length;
        if (desc->errors || length < MIN_ETH_FRAME || length > MAX_ETH_FRAME) {
            rx_drop_count++;
        } else {
            const u8 *frame = dma + RX_BUFFER_OFFSET + (size_t)rx_head * FRAME_SIZE;
            rx_count++;
            if (rx_handler) rx_handler(frame, length);
        }
        desc->length = 0;
        desc->checksum = 0;
        desc->errors = 0;
        desc->status = 0;
        __sync_synchronize();
        reg_write(E1000_REG_RDT, rx_head);
        rx_head = (u16)((rx_head + 1) % RING_COUNT);
    }
}

int net_active(void) { return initialized; }
int net_link_up(void) { return initialized && (reg_read(E1000_REG_STATUS) & (1u << 1)); }
const u8 *net_mac(void) { return initialized ? mac_address : 0; }
u64 net_rx_packets(void) { return rx_count; }
u64 net_rx_dropped(void) { return rx_drop_count; }
u64 net_tx_packets(void) { return tx_count; }

int net_send(const void *frame, size_t length) {
    if (!initialized || !frame || length < MIN_ETH_FRAME || length > MAX_ETH_FRAME)
        return 0;
    volatile struct e1000_tx_desc *desc = &tx_ring()[tx_tail];
    if (!(desc->status & 1)) return 0;

    memcpy(dma + TX_BUFFER_OFFSET + (size_t)tx_tail * FRAME_SIZE, frame, length);
    desc->length = (u16)length;
    desc->checksum_offset = 0;
    desc->checksum_start = 0;
    desc->status = 0;
    desc->command = 0x0B;
    desc->special = 0;
    __sync_synchronize();
    tx_tail = (u16)((tx_tail + 1) % RING_COUNT);
    reg_write(E1000_REG_TDT, tx_tail);
    tx_count++;
    return 1;
}

void net_set_rx_handler(void (*handler)(const u8 *frame, u16 length)) {
    rx_handler = handler;
}
