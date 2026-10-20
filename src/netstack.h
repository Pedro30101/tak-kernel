#ifndef NETSTACK_H
#define NETSTACK_H
#include "tak.h"

void net_stack_init(void);
void net_stack_poll(void);
u32 net_ipv4_address(void);
u32 net_ipv4_netmask(void);
u32 net_ipv4_gateway(void);
u32 net_ipv4_dns(void);
int net_dhcp_ready(void);
int net_dns_resolve(const char *name, u32 *address, u64 timeout_ticks);
int net_tcp_connect(const char *host, u16 port, u64 timeout_ticks);
long net_tcp_send(const void *data, size_t length, u64 timeout_ticks);
long net_tcp_receive(void *data, size_t capacity, u64 timeout_ticks);
void net_tcp_close(u64 timeout_ticks);
long net_http_get(const char *host, const char *path, char *response,
                  size_t capacity, u64 timeout_ticks);

#endif
