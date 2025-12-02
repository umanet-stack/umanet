#ifndef CONSTANTS_H
#define CONSTANTS_H

#ifndef MAX_QUEUES
#define MAX_QUEUES 128
#endif

/* the maximum number of external ports supported */
#define MAX_SUP_PORTS 1

#define MBUF_CACHE_SIZE 128
#define MBUF_DATA_SIZE RTE_MBUF_DEFAULT_BUF_SIZE

#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */

#define BURST_RX_WAIT_US 15 /* Defines how long we wait between retries on RX */
#define BURST_RX_RETRIES 4  /* Number of retries on RX. */

#define JUMBO_FRAME_MAX_SIZE 0x2600

/* State of virtio device. */
#define DEVICE_MAC_LEARNING 0
#define DEVICE_RX 1
#define DEVICE_SAFE_REMOVE 2

/* Configurable number of RX/TX ring descriptors */
#define RTE_TEST_RX_DESC_DEFAULT 1024
#define RTE_TEST_TX_DESC_DEFAULT 512

#define INVALID_PORT_ID 0xFF

/* Maximum long option length for option parsing. */
#define MAX_LONG_OPT_SZ 64

#endif /* CONSTANTS_H */