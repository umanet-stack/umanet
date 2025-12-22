#include "tap.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <rte_mbuf.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int tap_fd = -1;
static const char *tap_ifname = "vtap0";

int tap_init(void) {
    struct ifreq ifr;
    int fd;

    /* Open the TUN/TAP device */
    fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        LOG_ERROR("Failed to open /dev/net/tun: %s\n", strerror(errno));
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, tap_ifname, IFNAMSIZ);

    /* IFF_TAP = layer 2 TAP device, IFF_NO_PI = no packet info header */
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;

    /* Attach to existing TAP device (created by setup_vtap.sh) */
    if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) {
        LOG_ERROR("Failed to attach to TAP interface %s: %s\n", tap_ifname, strerror(errno));
        LOG_ERROR("Make sure setup/dpdk/setup_vtap.sh was run first\n");
        close(fd);
        return -1;
    }

    /* Set non-blocking mode */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        LOG_WARN("Failed to set non-blocking mode on %s\n", tap_ifname);
    }

    tap_fd = fd;
    LOG_INFO("Attached to TAP interface %s (fd=%d)\n", tap_ifname, tap_fd);
    return 0;
}

void tap_cleanup(void) {
    if (tap_fd >= 0) {
        close(tap_fd);
        tap_fd = -1;
        LOG_INFO("Disconnected from TAP interface %s\n", tap_ifname);
    }
}

/* Forward a single packet to TAP interface */
int tap_tx_one(struct rte_mbuf *pkt) {
    if (tap_fd < 0) {
        LOG_WARN("TAP interface not connected, dropping packet\n");
        rte_pktmbuf_free(pkt);
        return -1;
    }

    /* Get packet data */
    uint16_t pkt_len = rte_pktmbuf_pkt_len(pkt);
    void *pkt_data = rte_pktmbuf_mtod(pkt, void *);

    /* Write to TAP interface */
    ssize_t written = write(tap_fd, pkt_data, pkt_len);

    if (written < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* TAP buffer full, drop packet */
            LOG_WARN("TAP interface buffer full, dropping packet\n");
        } else {
            LOG_ERROR("Failed to write to TAP interface: %s (errno=%d)\n", strerror(errno), errno);
        }
        rte_pktmbuf_free(pkt);
        return -1;
    }

    if (written != pkt_len) {
        LOG_WARN("Partial write to TAP: %zd/%u bytes\n", written, pkt_len);
    }

    /* Free the packet */
    rte_pktmbuf_free(pkt);
    return 0;
}

/* Forward multiple packets to TAP interface */
int tap_tx_burst(struct rte_mbuf **pkts, uint16_t count) {
    int sent = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (tap_tx_one(pkts[i]) == 0) {
            sent++;
        }
    }
    return sent;
}
