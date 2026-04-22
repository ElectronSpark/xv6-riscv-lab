#ifndef __KERNEL_PIPE_H
#define __KERNEL_PIPE_H

#include "param.h"
#include "pipe_types.h"

struct vfs_file;

#define PIPESIZE (16 * PAGE_SIZE)

#define PIPE_FLAGS_READABLE 1
#define PIPE_FLAGS_WRITABLE 2
#define PIPE_FLAGS_NONBLOCK_RD 3
#define PIPE_FLAGS_NONBLOCK_WR 4
#define PIPE_FLAGS_RW ((1 << PIPE_FLAGS_READABLE) | (1 << PIPE_FLAGS_WRITABLE))

#define PIPE_WRITABLE(pi)                                                      \
    (!!(smp_load_acquire(&(pi)->flags) & (1 << PIPE_FLAGS_WRITABLE)))
#define PIPE_READABLE(pi)                                                      \
    (!!(smp_load_acquire(&(pi)->flags) & (1 << PIPE_FLAGS_READABLE)))
#define PIPE_NONBLOCK_RD(pi)                                                   \
    (!!(smp_load_acquire(&(pi)->flags) & (1 << PIPE_FLAGS_NONBLOCK_RD)))
#define PIPE_NONBLOCK_WR(pi)                                                   \
    (!!(smp_load_acquire(&(pi)->flags) & (1 << PIPE_FLAGS_NONBLOCK_WR)))
// Clear the writable flag; returns true if both readable and writable are
// cleared after the operation
#define PIPE_CLEAR_WRITABLE(pi)                                                \
    ({                                                                         \
        uint64 __flags;                                                        \
        __flags = __atomic_fetch_and(                                          \
            &(pi)->flags, ~(1 << PIPE_FLAGS_WRITABLE), __ATOMIC_SEQ_CST);      \
        !!(__flags == (1 << PIPE_FLAGS_WRITABLE));                             \
    })
#define PIPE_CLEAR_READABLE(pi)                                                \
    ({                                                                         \
        uint64 __flags;                                                        \
        __flags = __atomic_fetch_and(                                          \
            &(pi)->flags, ~(1 << PIPE_FLAGS_READABLE), __ATOMIC_SEQ_CST);      \
        !!(__flags == (1 << PIPE_FLAGS_READABLE));                             \
    })

#define PIPE_NONBLOCK_MASK                                                     \
    ((1 << PIPE_FLAGS_NONBLOCK_RD) | (1 << PIPE_FLAGS_NONBLOCK_WR))

void pipe_init(void);
struct pipe *pipe_alloc(int flags);
void pipe_close(struct pipe *, int);
void pipe_set_flags(struct pipe *pi, int flags);
int pipe_get_flags(struct pipe *pi);
ssize_t pipe_read(struct pipe *, char *, size_t, bool);
ssize_t pipe_write(struct pipe *, const char *, size_t, bool);
void pipe_open(struct vfs_file *file, struct pipe *pi, int f_flags);

#endif // __KERNEL_PIPE_H
