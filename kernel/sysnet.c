/*
 * sysnet.c - Network system calls
 *
 * Socket management and UDP packet handling.
 * Legacy sockalloc removed - VFS uses vfs_sockalloc in kernel/vfs/file.c
 * instead.
 */

#include "types.h"
#include "param.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "lock/spinlock.h"
#include "lock/mutex_types.h"
#include "proc/thread.h"
#include "defs.h"
#include "printf.h"
#include "dev/net.h"
#include <mm/vm.h>
#include "signal.h"
#include "proc/sched.h"

struct sock {
    struct sock *next; // the next socket in the list
    uint32 raddr;      // the remote IPv4 address
    uint16 lport;      // the local UDP port number
    uint16 rport;      // the remote UDP port number
    spinlock_t lock;   // protects the rxq
    struct mbufq rxq;  // a queue of packets waiting to be received
};

spinlock_t sock_lock = SPINLOCK_INITIALIZED("socktbl");
struct sock *sockets;

void sockinit(void) {}

// Legacy sockalloc removed - VFS uses vfs_sockalloc in vfs/file.c

void sockclose(struct sock *si) {
    struct sock **pos;
    struct mbuf *m;

    // remove from list of sockets
    spin_lock(&sock_lock);
    pos = &sockets;
    while (*pos) {
        if (*pos == si) {
            *pos = si->next;
            break;
        }
        pos = &(*pos)->next;
    }
    spin_unlock(&sock_lock);

    // free any pending mbufs
    while (!mbufq_empty(&si->rxq)) {
        m = mbufq_pophead(&si->rxq);
        mbuffree(m);
    }

    kfree((char *)si);
}

int sockread(struct sock *si, uint64 addr, int n) {
    struct thread *pr = current;
    struct mbuf *m;
    int len;

    spin_lock(&si->lock);
    while (mbufq_empty(&si->rxq) && !killed(pr)) {
        sleep_on_chan(&si->rxq, &si->lock);
    }
    if (killed(pr)) {
        spin_unlock(&si->lock);
        return -1;
    }
    m = mbufq_pophead(&si->rxq);
    spin_unlock(&si->lock);

    len = m->len;
    if (len > n)
        len = n;
    if (vm_copyout(pr->vm, addr, m->head, len) == -1) {
        mbuffree(m);
        return -1;
    }
    mbuffree(m);
    return len;
}

int sockwrite(struct sock *si, uint64 addr, int n) {
    struct thread *pr = current;
    struct mbuf *m;

    m = mbufalloc(MBUF_DEFAULT_HEADROOM);
    if (!m)
        return -1;

    if (vm_copyin(pr->vm, mbufput(m, n), addr, n) == -1) {
        mbuffree(m);
        return -1;
    }
    net_tx_udp(m, si->raddr, si->lport, si->rport);
    return n;
}

// called by protocol handler layer to deliver UDP packets
void sockrecvudp(struct mbuf *m, uint32 raddr, uint16 lport, uint16 rport) {
    //
    // Find the socket that handles this mbuf and deliver it, waking
    // any sleeping reader. Free the mbuf if there are no sockets
    // registered to handle it.
    //
    struct sock *si;

    spin_lock(&sock_lock);
    si = sockets;
    while (si) {
        if (si->raddr == raddr && si->lport == lport && si->rport == rport)
            goto found;
        si = si->next;
    }
    spin_unlock(&sock_lock);
    mbuffree(m);
    return;

found:
    spin_lock(&si->lock);
    mbufq_pushtail(&si->rxq, m);
    wakeup_on_chan(&si->rxq);
    spin_unlock(&si->lock);
    spin_unlock(&sock_lock);
}
