/**
 * @file netdev.c
 * @brief Network device abstraction layer implementation
 */

#include "types.h"
#include "string.h"
#include "dev/netdev.h"
#include "printf.h"

static struct netdev *netdev_list = 0; /* head of linked list */
static int netdev_count = 0;

void netdev_init(void) {
    netdev_list = 0;
    netdev_count = 0;
}

/**
 * Register a network device.  The first registered device becomes
 * the default transmit interface.
 */
int netdev_register(struct netdev *dev) {
    if (!dev || !dev->ops || !dev->ops->transmit)
        return -1;
    if (netdev_count >= NETDEV_MAX)
        return -1;

    dev->index = netdev_count++;
    dev->next = netdev_list;
    netdev_list = dev;

    printf("netdev: registered %s (MAC %x:%x:%x:%x:%x:%x) idx %d\n",
           dev->name, dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3],
           dev->mac[4], dev->mac[5], dev->index);
    return 0;
}

/**
 * Return the default (first-registered) network device.
 */
struct netdev *netdev_get_default(void) {
    /* Walk to the tail — the first registered is at the end of the list
     * because we prepend on registration. */
    struct netdev *d = netdev_list;
    struct netdev *last = 0;
    while (d) {
        last = d;
        d = d->next;
    }
    return last;
}

struct netdev *netdev_get_by_index(int index) {
    for (struct netdev *d = netdev_list; d; d = d->next) {
        if (d->index == index)
            return d;
    }
    return 0;
}

struct netdev *netdev_get_by_name(const char *name) {
    for (struct netdev *d = netdev_list; d; d = d->next) {
        if (strncmp(d->name, name, NETDEV_NAME_MAX) == 0)
            return d;
    }
    return 0;
}
