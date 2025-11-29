#include "tcp_offload.h"
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>

/* Check if packet is TCP and parse headers */
int tcp_parse_packet(struct rte_mbuf *m, uint32_t *sip, uint32_t *dip, uint16_t *sport, uint16_t *dport) {
    struct rte_ether_hdr *eth;
    struct rte_ipv4_hdr *ip;
    struct rte_tcp_hdr *tcp;

    eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

    /* Check if IPv4 */
    if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
        return -1;

    ip = (struct rte_ipv4_hdr *)(eth + 1);

    /* Check if TCP */
    if (ip->next_proto_id != IPPROTO_TCP)
        return -1;

    tcp = (struct rte_tcp_hdr *)((uint8_t *)ip + (ip->version_ihl & 0x0f) * 4);

    *sip = rte_be_to_cpu_32(ip->src_addr);
    *dip = rte_be_to_cpu_32(ip->dst_addr);
    *sport = rte_be_to_cpu_16(tcp->src_port);
    *dport = rte_be_to_cpu_16(tcp->dst_port);

    return 0;
}

/* Update flow state based on packet */
// void tcp_flow_update(struct tcp_flow_state *flow, struct rte_mbuf *m,
//                      int direction) // 0=outbound, 1=inbound
// {
//     struct rte_tcp_hdr *tcp = /* extract from m */;
//     uint32_t seq = rte_be_to_cpu_32(tcp->sent_seq);
//     uint32_t ack = rte_be_to_cpu_32(tcp->recv_ack);
//     uint16_t data_len = /* calculate payload length */;

//     if (direction == 0) { // Outbound (VM -> Network)
//         flow->tx_next_seq = seq + data_len;
//     } else { // Inbound (Network -> VM)
//         flow->rx_next_seq = seq + data_len;
//     }

//     RTE_LOG_DP(DEBUG, VHOST_DATA, "Flow update: TX seq=%u, RX seq=%u\n", flow->tx_next_seq, flow->rx_next_seq);
// }