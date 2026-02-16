#ifndef __USER_ABI_POLL_H
#define __USER_ABI_POLL_H

/*
 * poll(2) event bits — shared between VFS poll, TTY poll, and device drivers.
 * Values match the POSIX / Linux definitions.
 */

#define POLLIN 0x0001     /* Data available for reading */
#define POLLPRI 0x0002    /* Urgent data available */
#define POLLOUT 0x0004    /* Writing now will not block */
#define POLLERR 0x0008    /* Error condition (output only) */
#define POLLHUP 0x0010    /* Hang up (output only) */
#define POLLNVAL 0x0020   /* Invalid fd (output only) */
#define POLLRDNORM 0x0040 /* Normal data available for reading */
#define POLLWRNORM 0x0100 /* Writing normal data will not block */

#endif /* __USER_ABI_POLL_H */
