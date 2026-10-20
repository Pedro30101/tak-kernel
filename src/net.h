#ifndef NET_H
#define NET_H
#include "tak.h"

int net_init(void);
void net_poll(void);
int net_active(void);
int net_link_up(void);
const u8 *net_mac(void);
u64 net_rx_packets(void);
u64 net_rx_dropped(void);
u64 net_tx_packets(void);
int net_send(const void *frame, size_t length);
void net_set_rx_handler(void (*handler)(const u8 *frame, u16 length));

#endif
