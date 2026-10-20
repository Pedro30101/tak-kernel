#include "netstack.h"
#include "net.h"
#include "cpu.h"

#define ETH_HEADER 14
#define ETH_ARP 0x0806
#define ETH_IPV4 0x0800
#define IP_HEADER 20
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP 6
#define IP_PROTO_UDP 17
#define DNS_PORT 53
#define DNS_LOCAL_PORT 53053
#define ARP_CACHE_SIZE 8
#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_FIXED_SIZE 240
#define DHCP_RETRY_TICKS 200
#define DHCP_MAX_RETRIES 5
#define NET_BROADCAST 0xFFFFFFFFu
#define TCP_RX_CAPACITY 8192
#define TCP_MSS 1000
#define TCP_PORT 49152
#define TCP_HTTP_PORT 80

enum { TCP_IDLE, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_FIN_WAIT, TCP_CLOSED, TCP_FAILED };

static u32 local_ip;
static u32 local_mask;
static u32 local_gateway;
static u32 local_dns;
static u32 dhcp_xid;
static u32 offered_ip;
static u32 offered_server;
static u64 dhcp_retry_at;
static unsigned dhcp_retries;
static u8 dhcp_state;
static u16 ip_identification;
static int initialized;

struct arp_entry {
    u32 address;
    u8 mac[6];
    u64 learned;
};

static struct arp_entry arp_cache[ARP_CACHE_SIZE];
static u8 dns_query_name[256];
static char dns_query_host[254];
static u16 dns_query_id;
static u16 dns_query_length;
static u64 dns_query_deadline;
static u32 dns_result;
static int dns_query_active;
static int dns_query_done;
static u8 tcp_remote_mac[6];
static u32 tcp_remote_ip;
static u16 tcp_remote_port;
static u16 tcp_local_port;
static u32 tcp_send_next;
static u32 tcp_receive_next;
static u32 tcp_pending_seq;
static u32 tcp_pending_end;
static u8 tcp_pending_flags;
static u8 tcp_pending_data[TCP_MSS];
static u16 tcp_pending_length;
static u64 tcp_retry_at;
static unsigned tcp_retries;
static u8 tcp_state;
static u8 tcp_rx_data[TCP_RX_CAPACITY];
static size_t tcp_rx_length;
static int tcp_rx_overflow;
static int tcp_peer_closed;
static u16 tcp_port_counter;

static u16 get16(const u8 *p) {
    return (u16)((u16)p[0] << 8 | p[1]);
}

static u32 get32(const u8 *p) {
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static void put16(u8 *p, u16 value) {
    p[0] = (u8)(value >> 8);
    p[1] = (u8)value;
}

static void put32(u8 *p, u32 value) {
    p[0] = (u8)(value >> 24);
    p[1] = (u8)(value >> 16);
    p[2] = (u8)(value >> 8);
    p[3] = (u8)value;
}

static u16 checksum(const u8 *data, size_t length) {
    u32 sum = 0;
    while (length >= 2) {
        sum += get16(data);
        data += 2;
        length -= 2;
    }
    if (length) sum += (u16)data[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

static u16 udp_checksum(const u8 *ip, const u8 *udp, size_t length) {
    u32 sum = 0;
    sum += get16(ip + 12);
    sum += get16(ip + 14);
    sum += get16(ip + 16);
    sum += get16(ip + 18);
    sum += IP_PROTO_UDP;
    sum += (u16)length;
    while (length >= 2) {
        sum += get16(udp);
        udp += 2;
        length -= 2;
    }
    if (length) sum += (u16)udp[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

static u16 tcp_checksum(const u8 *ip, const u8 *tcp, size_t length) {
    u32 sum = 0;
    sum += get16(ip + 12);
    sum += get16(ip + 14);
    sum += get16(ip + 16);
    sum += get16(ip + 18);
    sum += IP_PROTO_TCP;
    sum += (u16)length;
    while (length >= 2) {
        sum += get16(tcp);
        tcp += 2;
        length -= 2;
    }
    if (length) sum += (u16)tcp[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

static int arp_lookup(u32 address, u8 mac[6]) {
    for (unsigned i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].address == address &&
            ticks - arp_cache[i].learned < 6000) {
            memcpy(mac, arp_cache[i].mac, 6);
            return 1;
        }
    }
    return 0;
}

static void arp_remember(u32 address, const u8 mac[6]) {
    if (!address || (mac[0] & 1)) return;
    unsigned slot = 0;
    for (unsigned i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].address == address) { slot = i; goto save; }
        if (!arp_cache[i].address ||
            ticks - arp_cache[i].learned >= 6000) { slot = i; goto save; }
        if (arp_cache[i].learned < arp_cache[slot].learned) slot = i;
    }
save:
    arp_cache[slot].address = address;
    memcpy(arp_cache[slot].mac, mac, 6);
    arp_cache[slot].learned = ticks;
}

static void arp_send_request(u32 address) {
    u8 request[ETH_HEADER + 28] = { 0 };
    memset(request, 0xFF, 6);
    memcpy(request + 6, net_mac(), 6);
    put16(request + 12, ETH_ARP);
    put16(request + 14, 1);
    put16(request + 16, ETH_IPV4);
    request[18] = 6;
    request[19] = 4;
    put16(request + 20, 1);
    memcpy(request + 22, net_mac(), 6);
    put32(request + 28, local_ip);
    put32(request + 38, address);
    (void)net_send(request, sizeof(request));
}

static int send_ipv4(const u8 *destination_mac, u32 destination_ip,
                     u8 protocol, const u8 *payload, size_t payload_length) {
    if (payload_length > 1500 - IP_HEADER) return 0;
    u8 frame[ETH_HEADER + IP_HEADER + 1500 - IP_HEADER];
    memset(frame, 0, ETH_HEADER + IP_HEADER + payload_length);
    memcpy(frame, destination_mac, 6);
    memcpy(frame + 6, net_mac(), 6);
    put16(frame + 12, ETH_IPV4);

    u8 *ip = frame + ETH_HEADER;
    ip[0] = 0x45;
    put16(ip + 2, (u16)(IP_HEADER + payload_length));
    put16(ip + 4, ++ip_identification);
    put16(ip + 6, 0x4000);
    ip[8] = 64;
    ip[9] = protocol;
    put32(ip + 12, local_ip);
    put32(ip + 16, destination_ip);
    put16(ip + 10, checksum(ip, IP_HEADER));
    if (payload_length) memcpy(ip + IP_HEADER, payload, payload_length);
    return net_send(frame, ETH_HEADER + IP_HEADER + payload_length);
}

static int tcp_send(u8 flags, const u8 *payload, u16 payload_length,
                   int track_retransmit) {
    u8 packet[20 + TCP_MSS];
    if (payload_length > TCP_MSS) return 0;
    memset(packet, 0, 20 + payload_length);
    put16(packet, tcp_local_port);
    put16(packet + 2, tcp_remote_port);
    put32(packet + 4, tcp_send_next);
    put32(packet + 8, tcp_receive_next);
    packet[12] = 5 << 4;
    packet[13] = flags;
    put16(packet + 14, 32768);
    if (payload_length) memcpy(packet + 20, payload, payload_length);

    u8 pseudo_ip[20] = { 0 };
    put32(pseudo_ip + 12, local_ip);
    put32(pseudo_ip + 16, tcp_remote_ip);
    put16(packet + 16, tcp_checksum(pseudo_ip, packet, 20 + payload_length));

    u32 old_send = tcp_send_next;
    u32 consumed = payload_length + ((flags & 0x02) ? 1 : 0) +
                   ((flags & 0x01) ? 1 : 0);
    if (!send_ipv4(tcp_remote_mac, tcp_remote_ip, IP_PROTO_TCP,
                   packet, 20 + payload_length))
        return 0;
    if (consumed) tcp_send_next += consumed;
    if (track_retransmit && consumed) {
        tcp_pending_seq = old_send;
        tcp_pending_end = tcp_send_next;
        tcp_pending_flags = flags;
        tcp_pending_length = payload_length;
        if (payload_length) memcpy(tcp_pending_data, payload, payload_length);
        tcp_retry_at = ticks + 100;
        tcp_retries = 0;
    }
    return 1;
}

static void tcp_ack(void) {
    (void)tcp_send(0x10, NULL, 0, 0);
}

static int send_udp(const u8 *destination_mac, u32 destination_ip,
                    u16 source_port, u16 destination_port,
                    const u8 *payload, size_t payload_length) {
    if (payload_length > 1472) return 0;
    u8 datagram[1480];
    put16(datagram, source_port);
    put16(datagram + 2, destination_port);
    put16(datagram + 4, (u16)(8 + payload_length));
    put16(datagram + 6, 0);
    if (payload_length) memcpy(datagram + 8, payload, payload_length);
    return send_ipv4(destination_mac, destination_ip, IP_PROTO_UDP,
                     datagram, 8 + payload_length);
}

static void dhcp_send(u8 message_type) {
    u8 packet[DHCP_FIXED_SIZE + 32];
    memset(packet, 0, sizeof(packet));
    packet[0] = 1;
    packet[1] = 1;
    packet[2] = 6;
    put32(packet + 4, dhcp_xid);
    put16(packet + 10, 0x8000);
    memcpy(packet + 28, net_mac(), 6);
    put32(packet + 236, 0x63825363);

    size_t at = DHCP_FIXED_SIZE;
    packet[at++] = 53; packet[at++] = 1; packet[at++] = message_type;
    packet[at++] = 55; packet[at++] = 4;
    packet[at++] = 1; packet[at++] = 3; packet[at++] = 6; packet[at++] = 51;
    if (message_type == 3) {
        packet[at++] = 50; packet[at++] = 4;
        put32(packet + at, offered_ip); at += 4;
        packet[at++] = 54; packet[at++] = 4;
        put32(packet + at, offered_server); at += 4;
    }
    packet[at++] = 255;
    static const u8 broadcast_mac[6] = { 255, 255, 255, 255, 255, 255 };
    if (send_udp(broadcast_mac, NET_BROADCAST, DHCP_CLIENT_PORT,
                 DHCP_SERVER_PORT, packet, at)) {
        dhcp_retries++;
        dhcp_retry_at = ticks + DHCP_RETRY_TICKS;
        klog("dhcp: sent %s (%u/%u)\n",
             message_type == 1 ? "discover" : "request",
             dhcp_retries, DHCP_MAX_RETRIES);
    } else dhcp_retry_at = ticks + 10;
}

static int dns_name_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-';
}

static int dhcp_options(const u8 *packet, size_t length, u8 *message_type,
                        u32 *server, u32 *mask, u32 *gateway, u32 *dns) {
    size_t at = DHCP_FIXED_SIZE;
    int have_type = 0;
    while (at < length) {
        u8 code = packet[at++];
        if (code == 0) continue;
        if (code == 255) break;
        if (at >= length) return 0;
        u8 option_length = packet[at++];
        if (option_length > length - at) return 0;
        switch (code) {
        case 53:
            if (option_length != 1) return 0;
            *message_type = packet[at];
            have_type = 1;
            break;
        case 54:
            if (option_length != 4) return 0;
            *server = get32(packet + at);
            break;
        case 1:
            if (option_length != 4) return 0;
            *mask = get32(packet + at);
            break;
        case 3:
            if (option_length < 4 || (option_length & 3)) return 0;
            *gateway = get32(packet + at);
            break;
        case 6:
            if (option_length < 4 || (option_length & 3)) return 0;
            *dns = get32(packet + at);
            break;
        default:
            break;
        }
        at += option_length;
    }
    return have_type;
}

static void dhcp_receive(const u8 *packet, size_t length) {
    if (length < DHCP_FIXED_SIZE || packet[0] != 2 ||
        packet[1] != 1 || packet[2] != 6 ||
        get32(packet + 4) != dhcp_xid ||
        memcmp(packet + 28, net_mac(), 6) != 0 ||
        get32(packet + 236) != 0x63825363)
        return;

    u8 message_type = 0;
    u32 server = 0, mask = 0, gateway = 0, dns = 0;
    if (!dhcp_options(packet, length, &message_type, &server,
                      &mask, &gateway, &dns))
        return;

    if (message_type == 2 && dhcp_state == 1) {
        u32 address = get32(packet + 16);
        if (!address || !server) return;
        offered_ip = address;
        offered_server = server;
        dhcp_state = 2;
        dhcp_retries = 0;
        klog("dhcp: offer received\n");
        dhcp_send(3);
    } else if (message_type == 5 && dhcp_state == 2 &&
               get32(packet + 16) == offered_ip &&
               (!server || server == offered_server)) {
        local_ip = offered_ip;
        local_mask = mask;
        local_gateway = gateway;
        local_dns = dns;
        dhcp_state = 3;
        klog("dhcp: lease configured ip=%u.%u.%u.%u mask=%u.%u.%u.%u gateway=%u.%u.%u.%u dns=%u.%u.%u.%u\n",
             (u8)(local_ip >> 24), (u8)(local_ip >> 16), (u8)(local_ip >> 8), (u8)local_ip,
             (u8)(local_mask >> 24), (u8)(local_mask >> 16), (u8)(local_mask >> 8), (u8)local_mask,
             (u8)(local_gateway >> 24), (u8)(local_gateway >> 16), (u8)(local_gateway >> 8), (u8)local_gateway,
             (u8)(local_dns >> 24), (u8)(local_dns >> 16), (u8)(local_dns >> 8), (u8)local_dns);
    } else if (message_type == 6) {
        dhcp_state = 0;
        dhcp_retries = 0;
        dhcp_retry_at = ticks + DHCP_RETRY_TICKS;
        klog("dhcp: server rejected the address request\n");
    }
}

static void arp_receive(const u8 *frame, size_t length) {
    if (length < ETH_HEADER + 28) return;
    const u8 *arp = frame + ETH_HEADER;
    if (get16(arp) != 1 || get16(arp + 2) != ETH_IPV4 ||
        arp[4] != 6 || arp[5] != 4)
        return;

    u16 operation = get16(arp + 6);
    if (operation != 1 && operation != 2) return;
    u32 sender_ip = get32(arp + 14);
    const u8 *sender_mac = arp + 8;
    arp_remember(sender_ip, sender_mac);
    if (operation != 1 || !local_ip || get32(arp + 24) != local_ip)
        return;

    u8 reply[ETH_HEADER + 28] = { 0 };
    memcpy(reply, sender_mac, 6);
    memcpy(reply + 6, net_mac(), 6);
    put16(reply + 12, ETH_ARP);
    put16(reply + 14, 1);
    put16(reply + 16, ETH_IPV4);
    reply[18] = 6;
    reply[19] = 4;
    put16(reply + 20, 2);
    memcpy(reply + 22, net_mac(), 6);
    put32(reply + 28, local_ip);
    memcpy(reply + 32, sender_mac, 6);
    put32(reply + 38, sender_ip);
    (void)net_send(reply, sizeof(reply));
}

static int dns_skip_name(const u8 *packet, size_t length,
                         size_t *offset, u8 *name, size_t name_size) {
    size_t cursor = *offset, next = 0, out = 0;
    unsigned jumps = 0;
    int jumped = 0;
    while (cursor < length) {
        u8 label = packet[cursor++];
        if (!label) {
            if (!jumped) next = cursor;
            if (out >= name_size) return 0;
            name[out] = 0;
            *offset = next;
            return 1;
        }
        if ((label & 0xC0) == 0xC0) {
            if (cursor >= length || ++jumps > 16) return 0;
            u16 target = (u16)(((label & 0x3F) << 8) | packet[cursor++]);
            if (target >= length) return 0;
            if (!jumped) next = cursor;
            cursor = target;
            jumped = 1;
            continue;
        }
        if (label & 0xC0 || label > 63 || label > length - cursor)
            return 0;
        if (out && out + 1 >= name_size) return 0;
        if (out) name[out++] = '.';
        if (out + label >= name_size) return 0;
        for (u8 i = 0; i < label; i++)
            name[out++] = (u8)tolower(packet[cursor++]);
    }
    return 0;
}

static void dns_receive(u32 source_ip, const u8 *packet, size_t length) {
    if (!dns_query_active || length < 12 ||
        get16(packet) != dns_query_id ||
        !(get16(packet + 2) & 0x8000) ||
        (get16(packet + 2) & 0x7800) ||
        (get16(packet + 2) & 0x0200) ||
        get16(packet + 4) != 1 || source_ip != local_dns)
        return;

    u16 flags = get16(packet + 2);
    if (flags & 0x000F) {
        dns_result = 0;
        dns_query_done = 1;
        return;
    }

    size_t offset = 12;
    u8 qname[256];
    if (!dns_skip_name(packet, length, &offset, qname, sizeof(qname)) ||
        offset > length || length - offset < 4 ||
        strcasecmp((const char *)qname, dns_query_host) ||
        get16(packet + offset) != 1 || get16(packet + offset + 2) != 1)
        return;
    offset += 4;
    u16 answers = get16(packet + 6);
    for (u16 i = 0; i < answers; i++) {
        u8 answer_name[256];
        if (!dns_skip_name(packet, length, &offset,
                           answer_name, sizeof(answer_name)) ||
            offset > length || length - offset < 10)
            return;
        u16 type = get16(packet + offset);
        u16 class_code = get16(packet + offset + 2);
        u16 data_length = get16(packet + offset + 8);
        offset += 10;
        if (data_length > length - offset) return;
        if (type == 1 && class_code == 1 && data_length == 4) {
            dns_result = get32(packet + offset);
            dns_query_done = 1;
            return;
        }
        offset += data_length;
    }
    dns_result = 0;
    dns_query_done = 1;
}

static void tcp_receive(const u8 *ip, const u8 *packet, size_t length) {
    u32 source_ip = get32(ip + 12);
    if (length < 20 || tcp_state == TCP_IDLE || tcp_state == TCP_CLOSED ||
        source_ip != tcp_remote_ip ||
        get16(packet) != tcp_remote_port ||
        get16(packet + 2) != tcp_local_port)
        return;
    size_t header_length = (size_t)(packet[12] >> 4) * 4;
    if (header_length < 20 || header_length > length ||
        tcp_checksum(ip, packet, length) != 0)
        return;

    u8 flags = packet[13];
    u32 sequence = get32(packet + 4);
    u32 acknowledgment = get32(packet + 8);
    size_t data_length = length - header_length;
    const u8 *data = packet + header_length;
    if (flags & 0x04) {
        tcp_state = TCP_FAILED;
        return;
    }

    if (tcp_state == TCP_SYN_SENT) {
        if ((flags & 0x12) != 0x12 ||
            acknowledgment != tcp_send_next)
            return;
        tcp_receive_next = sequence + 1;
        tcp_state = TCP_ESTABLISHED;
        tcp_pending_length = 0;
        tcp_pending_flags = 0;
        tcp_ack();
        return;
    }
    if (tcp_state != TCP_ESTABLISHED && tcp_state != TCP_FIN_WAIT) return;

    if ((flags & 0x10) && tcp_pending_flags &&
        acknowledgment == tcp_pending_end) {
        tcp_pending_flags = 0;
        tcp_pending_length = 0;
        tcp_retries = 0;
    }
    if (data_length && sequence == tcp_receive_next) {
        size_t available = TCP_RX_CAPACITY - tcp_rx_length;
        size_t copy_length = data_length < available ? data_length : available;
        if (copy_length) {
            memcpy(tcp_rx_data + tcp_rx_length, data, copy_length);
            tcp_rx_length += copy_length;
            tcp_receive_next += (u32)copy_length;
        }
        if (copy_length < data_length) {
            tcp_receive_next += (u32)(data_length - copy_length);
            tcp_rx_overflow = 1;
        }
        tcp_ack();
    } else if (data_length) {
        tcp_ack();
    }
    if ((flags & 0x01) && sequence + (u32)data_length == tcp_receive_next) {
        tcp_receive_next++;
        tcp_peer_closed = 1;
        tcp_ack();
    }
}

static void ipv4_receive(const u8 *frame, size_t length) {
    if (length < ETH_HEADER + IP_HEADER) return;
    const u8 *ip = frame + ETH_HEADER;
    size_t available = length - ETH_HEADER;
    if ((ip[0] >> 4) != 4) return;
    size_t header_length = (size_t)(ip[0] & 0x0F) * 4;
    if (header_length < IP_HEADER || header_length > available) return;
    u16 total_length = get16(ip + 2);
    if (total_length < header_length || total_length > available ||
        checksum(ip, header_length) != 0 || (get16(ip + 6) & 0x3FFF))
        return;

    u32 destination = get32(ip + 16);
    if (destination != local_ip && destination != NET_BROADCAST) return;
    const u8 *payload = ip + header_length;
    size_t payload_length = total_length - header_length;
    if (ip[9] == IP_PROTO_TCP) {
        if (destination == local_ip && payload_length >= 20)
            tcp_receive(ip, payload, payload_length);
    } else if (ip[9] == IP_PROTO_UDP) {
        if (payload_length < 8) return;
        u16 udp_length = get16(payload + 4);
        if (udp_length < 8 || udp_length > payload_length) return;
        if (get16(payload + 6) && udp_checksum(ip, payload, udp_length) != 0)
            return;
        if (get16(payload) == DHCP_SERVER_PORT &&
            get16(payload + 2) == DHCP_CLIENT_PORT)
            dhcp_receive(payload + 8, udp_length - 8);
        else if (get16(payload + 2) == DNS_LOCAL_PORT &&
                 get16(payload) == DNS_PORT)
            dns_receive(get32(ip + 12), payload + 8, udp_length - 8);
    } else if (ip[9] == IP_PROTO_ICMP && payload_length >= 8 &&
               destination == local_ip &&
               payload[0] == 8 && payload[1] == 0 &&
               checksum(payload, payload_length) == 0) {
        u8 response[1500];
        memcpy(response, payload, payload_length);
        response[0] = 0;
        put16(response + 2, 0);
        put16(response + 2, checksum(response, payload_length));
        (void)send_ipv4(frame + 6, get32(ip + 12),
                        IP_PROTO_ICMP, response, payload_length);
    }
}

static void ethernet_receive(const u8 *frame, u16 length) {
    if (length < ETH_HEADER) return;
    switch (get16(frame + 12)) {
    case ETH_ARP:
        arp_receive(frame, length);
        break;
    case ETH_IPV4:
        ipv4_receive(frame, length);
        break;
    default:
        break;
    }
}

static void tcp_maintenance(void) {
    if (!tcp_pending_flags || ticks < tcp_retry_at) return;
    if (tcp_retries >= 5) {
        tcp_state = TCP_FAILED;
        tcp_pending_flags = 0;
        return;
    }
    u32 saved_next = tcp_send_next;
    tcp_send_next = tcp_pending_seq;
    (void)tcp_send(tcp_pending_flags, tcp_pending_data,
                   tcp_pending_length, 0);
    tcp_send_next = saved_next;
    tcp_retries++;
    tcp_retry_at = ticks + (100u << (tcp_retries > 3 ? 3 : tcp_retries));
}

void net_stack_init(void) {
    if (initialized || !net_active()) return;
    const u8 *mac = net_mac();
    dhcp_xid = 0x54414B00u | (u32)mac[4] << 8 | mac[5];
    dhcp_state = 1;
    dhcp_retry_at = ticks;
    initialized = 1;
    net_set_rx_handler(ethernet_receive);
    klog("net: IPv4 Ethernet stack ready; requesting DHCP configuration\n");
}

void net_stack_poll(void) {
    if (!initialized) return;
    tcp_maintenance();
    if (dhcp_state == 3 || ticks < dhcp_retry_at) return;
    if (dhcp_state == 0) {
        dhcp_state = 1;
        dhcp_retries = 0;
    }
    if (dhcp_retries >= DHCP_MAX_RETRIES) {
        klog("dhcp: no lease after %u attempts; retrying later\n", dhcp_retries);
        dhcp_state = 0;
        dhcp_retries = 0;
        dhcp_retry_at = ticks + 1000;
        return;
    }
    dhcp_send(dhcp_state == 2 ? 3 : 1);
}

static int dns_encode_name(const char *name, u8 *out, size_t capacity,
                           size_t *written) {
    size_t length = strlen(name), start = 0, out_at = 0;
    if (!length || length > 253 || name[length - 1] == '.') return 0;
    while (start < length) {
        size_t end = start;
        while (end < length && name[end] != '.') end++;
        size_t label_length = end - start;
        if (!label_length || label_length > 63 ||
            label_length + 1 > capacity - out_at)
            return 0;
        out[out_at++] = (u8)label_length;
        for (size_t i = start; i < end; i++) {
            unsigned char c = (unsigned char)name[i];
            if (!dns_name_char(c)) return 0;
            out[out_at++] = (u8)tolower(c);
        }
        start = end + 1;
    }
    if (out_at >= capacity) return 0;
    out[out_at++] = 0;
    *written = out_at;
    return 1;
}

int net_dns_resolve(const char *name, u32 *address, u64 timeout_ticks) {
    if (!initialized || !net_dhcp_ready() || !name || !address ||
        !local_dns || !timeout_ticks || dns_query_active)
        return 0;

    u8 query[512];
    size_t name_length;
    memset(query, 0, 12);
    if (!dns_encode_name(name, query + 12, sizeof(query) - 16, &name_length))
        return 0;
    size_t query_length = 12 + name_length;
    if (query_length + 4 > sizeof(query)) return 0;
    put16(query + 2, 0x0100);
    put16(query + 4, 1);
    put16(query + query_length, 1);
    put16(query + query_length + 2, 1);
    query_length += 4;

    u32 next_hop = ((local_dns ^ local_ip) & local_mask) ? local_gateway : local_dns;
    if (!next_hop) return 0;
    u8 destination_mac[6];
    u64 deadline = ticks + timeout_ticks;
    u64 arp_retry = 0;
    while (!arp_lookup(next_hop, destination_mac) && ticks < deadline) {
        if (ticks >= arp_retry) {
            arp_send_request(next_hop);
            arp_retry = ticks + 100;
        }
        net_poll();
        net_stack_poll();
        halt();
    }
    if (!arp_lookup(next_hop, destination_mac)) return 0;

    dns_query_id = (u16)(ticks ^ dhcp_xid ^ 0xA501);
    if (!dns_query_id) dns_query_id = 1;
    strcpy(dns_query_host, name);
    put16(query, dns_query_id);
    dns_query_length = (u16)query_length;
    memcpy(dns_query_name, query, query_length);
    dns_query_active = 1;
    dns_query_done = 0;
    dns_result = 0;
    dns_query_deadline = deadline;
    if (!send_udp(destination_mac, local_dns, DNS_LOCAL_PORT,
                  DNS_PORT, dns_query_name, dns_query_length)) {
        dns_query_active = 0;
        return 0;
    }
    while (!dns_query_done && ticks < dns_query_deadline) {
        net_poll();
        net_stack_poll();
        halt();
    }
    dns_query_active = 0;
    if (!dns_query_done || !dns_result) return 0;
    *address = dns_result;
    return 1;
}

static int tcp_wait_for(unsigned state, u64 deadline) {
    while (tcp_state != state && tcp_state != TCP_FAILED &&
           ticks < deadline) {
        net_poll();
        net_stack_poll();
        halt();
    }
    return tcp_state == state;
}

static int valid_http_host(const char *host) {
    size_t length = strlen(host);
    if (!length || length > 253) return 0;
    for (size_t i = 0; i < length; i++)
        if (!dns_name_char((unsigned char)host[i]) && host[i] != '.')
            return 0;
    return 1;
}

int net_tcp_connect(const char *host, u16 port, u64 timeout_ticks) {
    if (!host || !port || !timeout_ticks || !net_dhcp_ready() ||
        tcp_state == TCP_SYN_SENT || tcp_state == TCP_ESTABLISHED ||
        tcp_state == TCP_FIN_WAIT || !valid_http_host(host))
        return 0;
    u64 deadline = ticks + timeout_ticks;
    u32 remote_ip;
    if (!net_dns_resolve(host, &remote_ip, timeout_ticks)) return 0;
    deadline = ticks + timeout_ticks;
    u32 next_hop = ((remote_ip ^ local_ip) & local_mask) ? local_gateway : remote_ip;
    if (!next_hop) return 0;

    memset(&tcp_remote_mac, 0, sizeof(tcp_remote_mac));
    u64 arp_retry = 0;
    while (!arp_lookup(next_hop, tcp_remote_mac) && ticks < deadline) {
        if (ticks >= arp_retry) {
            arp_send_request(next_hop);
            arp_retry = ticks + 100;
        }
        net_poll();
        net_stack_poll();
        halt();
    }
    if (!arp_lookup(next_hop, tcp_remote_mac)) return 0;

    tcp_state = TCP_SYN_SENT;
    tcp_remote_ip = remote_ip;
    tcp_remote_port = port;
    tcp_local_port = (u16)(TCP_PORT + (tcp_port_counter++ % 1024));
    tcp_send_next = (u32)(ticks ^ dhcp_xid ^ ((u32)tcp_local_port << 16));
    tcp_receive_next = 0;
    tcp_pending_flags = 0;
    tcp_pending_length = 0;
    tcp_rx_length = 0;
    tcp_rx_overflow = 0;
    tcp_peer_closed = 0;
    if (!tcp_send(0x02, NULL, 0, 1) ||
        !tcp_wait_for(TCP_ESTABLISHED, deadline)) {
        tcp_state = TCP_CLOSED;
        tcp_pending_flags = 0;
        return 0;
    }
    return 1;
}

long net_tcp_send(const void *data, size_t length, u64 timeout_ticks) {
    if ((!data && length) || !timeout_ticks || tcp_state != TCP_ESTABLISHED)
        return -1;
    const u8 *bytes = data;
    size_t sent = 0;
    while (sent < length) {
        size_t chunk = length - sent;
        if (chunk > TCP_MSS) chunk = TCP_MSS;
        u64 deadline = ticks + timeout_ticks;
        if (!tcp_send(0x18, bytes + sent, (u16)chunk, 1)) return sent ? (long)sent : -1;
        while (tcp_pending_flags && tcp_state != TCP_FAILED &&
               ticks < deadline) {
            net_poll();
            net_stack_poll();
            halt();
        }
        if (tcp_pending_flags || tcp_state == TCP_FAILED)
            return sent ? (long)sent : -1;
        sent += chunk;
    }
    return (long)sent;
}

long net_tcp_receive(void *data, size_t capacity, u64 timeout_ticks) {
    if (!data || !capacity || !timeout_ticks ||
        (tcp_state != TCP_ESTABLISHED && tcp_state != TCP_FIN_WAIT))
        return -1;
    u64 deadline = ticks + timeout_ticks;
    while (!tcp_rx_length && !tcp_peer_closed &&
           !tcp_rx_overflow && tcp_state != TCP_FAILED &&
           ticks < deadline) {
        net_poll();
        net_stack_poll();
        halt();
    }
    if (tcp_rx_overflow) {
        tcp_rx_overflow = 0;
        return -1;
    }
    if (tcp_state == TCP_FAILED) return -1;
    if (!tcp_rx_length) return 0;
    size_t copied = tcp_rx_length < capacity ? tcp_rx_length : capacity;
    memcpy(data, tcp_rx_data, copied);
    tcp_rx_length -= copied;
    if (tcp_rx_length) memmove(tcp_rx_data, tcp_rx_data + copied, tcp_rx_length);
    return (long)copied;
}

void net_tcp_close(u64 timeout_ticks) {
    if (tcp_state == TCP_ESTABLISHED && !tcp_peer_closed) {
        tcp_state = TCP_FIN_WAIT;
        if (tcp_send(0x11, NULL, 0, 1)) {
            u64 deadline = ticks + timeout_ticks;
            while (tcp_pending_flags && tcp_state != TCP_FAILED &&
                   ticks < deadline) {
                net_poll();
                net_stack_poll();
                halt();
            }
        }
    }
    tcp_state = TCP_CLOSED;
    tcp_pending_flags = 0;
    tcp_pending_length = 0;
}

long net_http_get(const char *host, const char *path, char *response,
                  size_t capacity, u64 timeout_ticks) {
    if (!host || !path || !response || capacity < 2 ||
        !timeout_ticks || path[0] != '/')
        return -1;
    size_t path_length = strlen(path);
    if (path_length > 512) return -1;
    for (size_t i = 0; i < path_length; i++)
        if ((u8)path[i] < 0x20 || path[i] == 0x7F) return -1;

    if (!net_tcp_connect(host, TCP_HTTP_PORT, timeout_ticks)) return -1;
    char request[TCP_MSS];
    int request_length = snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\nUser-Agent: TAK/0.9.6b (0.9.6 Beta)\r\n\r\n",
        path, host);
    if (request_length <= 0 || (size_t)request_length >= sizeof(request) ||
        net_tcp_send(request, (size_t)request_length, timeout_ticks) != request_length) {
        net_tcp_close(timeout_ticks);
        return -1;
    }

    size_t total = 0;
    while (total < capacity - 1) {
        long received = net_tcp_receive(response + total,
                                        capacity - 1 - total,
                                        timeout_ticks);
        if (received < 0) {
            net_tcp_close(timeout_ticks);
            return -1;
        }
        if (!received) break;
        total += (size_t)received;
        if (tcp_peer_closed && !tcp_rx_length) break;
    }
    response[total] = 0;
    int overflow = tcp_rx_overflow || (total == capacity - 1 &&
                                        !tcp_peer_closed);
    net_tcp_close(timeout_ticks);
    return overflow ? -2 : (long)total;
}

u32 net_ipv4_address(void) { return local_ip; }
u32 net_ipv4_netmask(void) { return local_mask; }
u32 net_ipv4_gateway(void) { return local_gateway; }
u32 net_ipv4_dns(void) { return local_dns; }
int net_dhcp_ready(void) { return dhcp_state == 3; }
