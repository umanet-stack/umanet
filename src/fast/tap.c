#include "tap.h"
#include "src/include/tas.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <rte_mbuf.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static int tap_fd = -1;
static const char *tap_ifname = "vtap0";

int tap_init(void) {
    struct ifreq ifr;
    struct sockaddr_ll sll;
    int fd;

    /* Create a raw packet socket to bind to existing veth interface */
    fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        LOG_ERROR("Failed to create packet socket: %s\n", strerror(errno));
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, tap_ifname, IFNAMSIZ);

    /* Get interface index */
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        LOG_ERROR("Failed to get interface index for %s: %s\n", tap_ifname, strerror(errno));
        LOG_ERROR("Make sure vtap0 exists (run setup/dpdk/setup_vtap.sh first)\n");
        close(fd);
        return -1;
    }

    /* Bind socket to the interface */
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = ifr.ifr_ifindex;
    sll.sll_protocol = htons(ETH_P_ALL);

    if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        LOG_ERROR("Failed to bind to interface %s: %s\n", tap_ifname, strerror(errno));
        close(fd);
        return -1;
    }

    /* Set non-blocking mode */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        LOG_WARN("Failed to set non-blocking mode on %s\n", tap_ifname);
    }

    tap_fd = fd;
    LOG_INFO("Connected to existing veth interface %s (fd=%d, ifindex=%d)\n", tap_ifname, tap_fd, ifr.ifr_ifindex);
    return 0;
}

void tap_cleanup(void) {
    if (tap_fd >= 0) {
        close(tap_fd);
        tap_fd = -1;
        LOG_INFO("Disconnected from veth interface %s\n", tap_ifname);
    }
}

/* Forward a single packet to veth interface */
int tap_tx_one(struct rte_mbuf *pkt) {
    if (tap_fd < 0) {
        LOG_WARN("veth interface not connected, dropping packet\n");
        rte_pktmbuf_free(pkt);
        return -1;
    }

    /* Get packet data */
    uint16_t pkt_len = rte_pktmbuf_pkt_len(pkt);
    void *pkt_data = rte_pktmbuf_mtod(pkt, void *);

    /* Send packet via socket */
    ssize_t written = send(tap_fd, pkt_data, pkt_len, 0);

    if (written < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* Socket buffer full, drop packet */
            LOG_WARN("veth interface buffer full, dropping packet\n");
        } else {
            LOG_ERROR("Failed to send to veth interface: %s\n", strerror(errno));
        }
        rte_pktmbuf_free(pkt);
        return -1;
    }

    if (written != pkt_len) {
        LOG_WARN("Partial write to veth: %zd/%u bytes\n", written, pkt_len);
    }

    /* Free the packet */
    rte_pktmbuf_free(pkt);
    return 0;
}

/* Forward multiple packets to veth interface */
int tap_tx_burst(struct rte_mbuf **pkts, uint16_t count) {
    int sent = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (tap_tx_one(pkts[i]) == 0) {
            sent++;
        }
    }
    return sent;
}
