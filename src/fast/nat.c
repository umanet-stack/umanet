#include "nat.h"
#include "log.h"

#include <rte_cycles.h>
#include <rte_ether.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_malloc.h>

#define NAT_MAX_ENTRIES 65536
#define NAT_TIMEOUT_SEC 300 // 5 minutes
#define NAT_PORT_START 32768
#define NAT_PORT_END 61000

static struct rte_hash *nat_outbound_hash = NULL; // VM IP:port -> NAT entry
static struct rte_hash *nat_inbound_hash = NULL;  // External IP:port:NAT_port -> NAT entry
static struct nat_entry *nat_entries = NULL;
static uint16_t next_nat_port = NAT_PORT_START;
static uint32_t nat_nic_ip = 0;

// Get next available NAT port
static uint16_t nat_get_next_port(void) {
    uint16_t port = next_nat_port++;
    if (next_nat_port >= NAT_PORT_END)
        next_nat_port = NAT_PORT_START;
    return port;
}

int nat_init(uint32_t nic_ip) {
    nat_nic_ip = nic_ip;

    // Allocate NAT entry pool
    nat_entries = rte_zmalloc("nat_entries", sizeof(struct nat_entry) * NAT_MAX_ENTRIES, 0);
    if (nat_entries == NULL) {
        LOG_ERROR("Failed to allocate NAT entries\n");
        return -1;
    }

    // Create outbound hash table (VM IP:port -> entry)
    struct rte_hash_parameters outbound_params = {
        .name = "nat_outbound",
        .entries = NAT_MAX_ENTRIES,
        .key_len = sizeof(uint64_t), // 32-bit IP + 16-bit port + 16-bit protocol
        .hash_func = rte_jhash,
        .hash_func_init_val = 0,
        .socket_id = rte_socket_id(),
    };
    nat_outbound_hash = rte_hash_create(&outbound_params);
    if (nat_outbound_hash == NULL) {
        LOG_ERROR("Failed to create NAT outbound hash table\n");
        return -1;
    }

    // Create inbound hash table (External IP:port:NAT_port -> entry)
    struct rte_hash_parameters inbound_params = {
        .name = "nat_inbound",
        .entries = NAT_MAX_ENTRIES,
        .key_len = sizeof(struct nat_key),
        .hash_func = rte_jhash,
        .hash_func_init_val = 0,
        .socket_id = rte_socket_id(),
    };
    nat_inbound_hash = rte_hash_create(&inbound_params);
    if (nat_inbound_hash == NULL) {
        LOG_ERROR("Failed to create NAT inbound hash table\n");
        return -1;
    }

    LOG_INFO("NAT initialized with NIC IP %u.%u.%u.%u\n", (nat_nic_ip >> 24) & 0xff, (nat_nic_ip >> 16) & 0xff,
             (nat_nic_ip >> 8) & 0xff, nat_nic_ip & 0xff);
    return 0;
}

// Calculate IP checksum
static uint16_t ip_checksum(struct rte_ipv4_hdr *iph) {
    uint16_t *buf = (uint16_t *)iph;
    uint32_t sum = 0;
    uint16_t result;

    for (int i = 0; i < 10; i++)
        sum += buf[i];

    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);

    result = ~sum;
    return result;
}

// Update TCP/UDP checksum for NAT
static void update_l4_checksum(struct rte_ipv4_hdr *iph, uint16_t old_port, uint16_t new_port, uint32_t old_ip,
                               uint32_t new_ip) {
    // Simplified: just mark for hardware checksum offload
    // For software checksum, would need to update TCP/UDP checksum properly
}

int nat_translate_outbound(struct rte_mbuf *m, int vid, uint32_t nic_ip) {
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    struct rte_ipv4_hdr *iph = (struct rte_ipv4_hdr *)(eth + 1);

    // Only handle IPv4 TCP/UDP
    if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
        return 0;

    uint8_t proto = iph->next_proto_id;
    if (proto != IPPROTO_TCP && proto != IPPROTO_UDP)
        return 0;

    uint16_t *sport_ptr, *dport_ptr;
    if (proto == IPPROTO_TCP) {
        struct rte_tcp_hdr *tcph = (struct rte_tcp_hdr *)((uint8_t *)iph + sizeof(struct rte_ipv4_hdr));
        sport_ptr = &tcph->src_port;
        dport_ptr = &tcph->dst_port;
    } else {
        struct rte_udp_hdr *udph = (struct rte_udp_hdr *)((uint8_t *)iph + sizeof(struct rte_ipv4_hdr));
        sport_ptr = &udph->src_port;
        dport_ptr = &udph->dst_port;
    }

    uint32_t vm_ip = rte_be_to_cpu_32(iph->src_addr);
    uint16_t vm_port = rte_be_to_cpu_16(*sport_ptr);
    uint32_t ext_ip = rte_be_to_cpu_32(iph->dst_addr);
    uint16_t ext_port = rte_be_to_cpu_16(*dport_ptr);

    // Create outbound lookup key (VM IP + port + ext IP + ext port)
    uint64_t out_key = ((uint64_t)vm_ip << 32) | ((uint64_t)vm_port << 16) | proto;

    // Look up existing NAT entry
    int32_t idx = rte_hash_lookup(nat_outbound_hash, &out_key);
    struct nat_entry *entry;

    if (idx < 0) {
        // Create new NAT entry
        uint16_t nat_port = nat_get_next_port();
        idx = rte_hash_add_key(nat_outbound_hash, &out_key);
        if (idx < 0) {
            LOG_WARN("NAT table full, dropping packet\n");
            return -1;
        }

        entry = &nat_entries[idx];
        entry->vm_ip = vm_ip;
        entry->vm_port = vm_port;
        entry->ext_ip = ext_ip;
        entry->ext_port = ext_port;
        entry->nat_port = nat_port;
        entry->protocol = proto;
        entry->vid = vid;
        entry->last_used = rte_rdtsc();

        // Add to inbound hash for reverse lookup
        struct nat_key in_key = {.ext_ip = ext_ip, .ext_port = ext_port, .nat_port = nat_port, .protocol = proto};
        rte_hash_add_key_data(nat_inbound_hash, &in_key, (void *)(uintptr_t)idx);

        LOG_INFO("NAT: New mapping VM %u.%u.%u.%u:%u -> NIC:%u -> Ext %u.%u.%u.%u:%u\n", (vm_ip >> 24) & 0xff,
                 (vm_ip >> 16) & 0xff, (vm_ip >> 8) & 0xff, vm_ip & 0xff, vm_port, nat_port, (ext_ip >> 24) & 0xff,
                 (ext_ip >> 16) & 0xff, (ext_ip >> 8) & 0xff, ext_ip & 0xff, ext_port);
    } else {
        entry = &nat_entries[idx];
        entry->last_used = rte_rdtsc();
    }

    // Perform SNAT: Change source IP and port
    uint32_t old_ip = rte_be_to_cpu_32(iph->src_addr);
    iph->src_addr = rte_cpu_to_be_32(nic_ip);
    *sport_ptr = rte_cpu_to_be_16(entry->nat_port);

    // Recalculate IP checksum
    iph->hdr_checksum = 0;
    iph->hdr_checksum = ip_checksum(iph);

    // Mark for L4 checksum recalculation (or recalculate in software)
    m->ol_flags |= PKT_TX_IPV4 | PKT_TX_IP_CKSUM;
    if (proto == IPPROTO_TCP)
        m->ol_flags |= PKT_TX_TCP_CKSUM;
    else if (proto == IPPROTO_UDP)
        m->ol_flags |= PKT_TX_UDP_CKSUM;

    return 0;
}

int nat_translate_inbound(struct rte_mbuf *m, int *vid_out) {
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    struct rte_ipv4_hdr *iph = (struct rte_ipv4_hdr *)(eth + 1);

    // Only handle IPv4 TCP/UDP
    if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
        return 0;

    uint8_t proto = iph->next_proto_id;
    if (proto != IPPROTO_TCP && proto != IPPROTO_UDP)
        return 0;

    uint16_t *sport_ptr, *dport_ptr;
    if (proto == IPPROTO_TCP) {
        struct rte_tcp_hdr *tcph = (struct rte_tcp_hdr *)((uint8_t *)iph + sizeof(struct rte_ipv4_hdr));
        sport_ptr = &tcph->src_port;
        dport_ptr = &tcph->dst_port;
    } else {
        struct rte_udp_hdr *udph = (struct rte_udp_hdr *)((uint8_t *)iph + sizeof(struct rte_ipv4_hdr));
        sport_ptr = &udph->src_port;
        dport_ptr = &udph->dst_port;
    }

    uint32_t ext_ip = rte_be_to_cpu_32(iph->src_addr);
    uint16_t ext_port = rte_be_to_cpu_16(*sport_ptr);
    uint16_t nat_port = rte_be_to_cpu_16(*dport_ptr);

    // Look up NAT entry
    struct nat_key in_key = {.ext_ip = ext_ip, .ext_port = ext_port, .nat_port = nat_port, .protocol = proto};
    void *data;
    int ret = rte_hash_lookup_data(nat_inbound_hash, &in_key, &data);
    if (ret < 0) {
        // No NAT entry found, not for us
        return -1;
    }

    int32_t idx = (int32_t)(uintptr_t)data;
    struct nat_entry *entry = &nat_entries[idx];
    entry->last_used = rte_rdtsc();

    // Perform DNAT: Change destination IP and port back to VM
    iph->dst_addr = rte_cpu_to_be_32(entry->vm_ip);
    *dport_ptr = rte_cpu_to_be_16(entry->vm_port);

    // Recalculate checksums
    iph->hdr_checksum = 0;
    iph->hdr_checksum = ip_checksum(iph);

    m->ol_flags |= PKT_TX_IPV4 | PKT_TX_IP_CKSUM;
    if (proto == IPPROTO_TCP)
        m->ol_flags |= PKT_TX_TCP_CKSUM;
    else if (proto == IPPROTO_UDP)
        m->ol_flags |= PKT_TX_UDP_CKSUM;

    *vid_out = entry->vid;

    LOG_INFO("NAT: Inbound Ext %u.%u.%u.%u:%u -> NIC:%u -> VM %u.%u.%u.%u:%u (vid=%d)\n", (ext_ip >> 24) & 0xff,
             (ext_ip >> 16) & 0xff, (ext_ip >> 8) & 0xff, ext_ip & 0xff, ext_port, nat_port,
             (entry->vm_ip >> 24) & 0xff, (entry->vm_ip >> 16) & 0xff, (entry->vm_ip >> 8) & 0xff, entry->vm_ip & 0xff,
             entry->vm_port, entry->vid);

    return 0;
}

void nat_cleanup_expired(void) {
    uint64_t now = rte_rdtsc();
    uint64_t timeout_cycles = rte_get_tsc_hz() * NAT_TIMEOUT_SEC;

    // TODO: Implement proper cleanup by iterating hash table
    // For now, entries will time out lazily
}
