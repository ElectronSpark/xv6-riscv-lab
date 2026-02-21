/**
 * @file netdev.h
 * @brief Network device abstraction layer
 *
 * Provides a simple netdev registration/lookup mechanism so that net.c
 * can transmit packets without being coupled to a specific NIC driver
 * (e1000, x1-emac, etc.).
 */
#ifndef __KERNEL_DEV_NETDEV_H
#define __KERNEL_DEV_NETDEV_H

#include "types.h"
#include "dev/net.h"

#define NETDEV_NAME_MAX 16
#define NETDEV_MAX 4

/* Forward declaration */
struct netdev;

/**
 * Network device operations – each NIC driver provides one of these.
 */
struct netdev_ops {
    int (*transmit)(struct netdev *dev, struct mbuf *m);
};

/**
 * Link-change callback type.
 * @dev:     the netdev whose link state changed
 * @link_up: 1 = link came up, 0 = link went down
 */
typedef void (*netdev_link_cb_t)(struct netdev *dev, int link_up);

/**
 * Network device instance – represents one Ethernet interface.
 */
struct netdev {
    char name[NETDEV_NAME_MAX]; /* e.g. "eth0", "e1000" */
    uint8 mac[6];               /* hardware (MAC) address */
    uint32 ip;                  /* IPv4 address (network byte order) */
    int mtu;                    /* maximum transmission unit */
    int link_up;                /* 1 = link is up */
    int speed;                  /* negotiated speed in Mbps (10/100/1000) */
    int full_duplex;            /* 1 = full duplex */
    int index;                  /* interface index (0-based) */
    struct netdev_ops *ops;     /* driver callbacks */
    void *priv;                 /* driver private data */
    struct netdev *next;        /* linked list */
    netdev_link_cb_t link_cb;   /* optional link-change callback */
};

/* Registration / lookup */
void netdev_init(void);
int netdev_register(struct netdev *dev);
struct netdev *netdev_get_default(void);
struct netdev *netdev_get_by_index(int index);
struct netdev *netdev_get_by_name(const char *name);

/**
 * netdev_set_link — Update link state and notify upper layers.
 * Call from NIC drivers when physical link state changes.
 */
void netdev_set_link(struct netdev *dev, int link_up);

/**
 * netdev_set_link_callback — Register a link-change callback.
 * Called by upper-layer code (e.g. lwIP glue) to be notified of
 * link up/down events.
 */
void netdev_set_link_callback(struct netdev *dev, netdev_link_cb_t cb);

#endif /* __KERNEL_DEV_NETDEV_H */
