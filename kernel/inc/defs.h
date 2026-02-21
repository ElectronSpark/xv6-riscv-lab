#ifndef __KERNEL_DEFS_H
#define __KERNEL_DEFS_H

#include "compiler.h"
#include "types.h"

#define major(dev) ((dev) >> 20 & 0xFFF)
#define minor(dev) ((dev) & 0xFFFFF)
#define mkdev(m, n) ((uint)((m) << 20 | (n)))

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

struct buf;
struct context;
struct file;
struct inode;
struct pipe;
struct thread;
typedef struct spinlock spinlock_t __ALIGNED_CACHELINE;
typedef struct mutex mutex_t;
struct stat;
struct superblock;
struct mbuf;
struct sock;

// start_kernel.c
void start_kernel(int hartid, void *fdt_base, bool is_boot_hart);
void start_kernel_post_init(void);

// bio.c
void binit(void);
struct buf *bread(uint, uint);
void brelse(struct buf *);
void bwrite(struct buf *);
void bwrite_async(struct buf *); // mark dirty, don't wait for I/O
void bsync(void);                // flush all dirty buffers
uint bdirty_count(void);         // get count of dirty buffers
void bpin(struct buf *);
void bunpin(struct buf *);

// console.c
void consoleinit(void);
void consoledevinit(void);
void nullranddevinit(void);
void random_fill_bytes(void *buf, size_t count);
void consoleintr(int);
void consputc(int);
void consputs(const char *, int);

// tty/tty_dev.c
void ttydevinit(void);

// tty/ptmx.c
void ptmxinit(void);

// exec.c
int exec(char *, char **, char **);

// futex.c
typedef struct vm vm_t;
void futex_init(void);
int futex_wake_addr(vm_t *vm, uint64 uaddr, int val);

// Legacy file.c removed - now using VFS exclusively (vfs/file.c)

// Legacy fs.c removed - now using VFS exclusively
// See kernel/vfs/fs.h for VFS interfaces

// ramdisk.c
void ramdiskinit(void);
void ramdiskintr(void);
void ramdiskrw(struct buf *);

// kalloc.c
void *kalloc(void);
void kfree(void *);
void kinit(void);
void *kmm_alloc(size_t);
void kmm_free(void *);
void kmm_shrink_all(void);
uint64 get_total_free_pages(void);

// Legacy log.c removed - logging now handled by VFS xv6fs subsystem
// See kernel/vfs/xv6fs/log.c for VFS logging interfaces

// swtch.S
struct context *__swtch_context(struct context *cur, struct context *target);
typedef void (*sw_noret_cb_t)(uint64, uint64);
void __switch_noreturn(uint64 irq_sp, uint64 s0, sw_noret_cb_t addr);

// spinlock.c
// Initialize a spinlock.
void spin_init(spinlock_t *, char *);
// Check if the current CPU is holding the lock.
int spin_holding(spinlock_t *);
// Basic spin lock functions. Will NOT modify preempt counter or disable
// interrupts.
void spin_acquire(spinlock_t *);
void spin_release(spinlock_t *);
// Default spin lock functions. Will modify preempt counter.
void spin_lock(spinlock_t *);
void spin_unlock(spinlock_t *);
int spin_trylock(spinlock_t *);
// Functions to save/restore interrupt state with spinlock.
int spin_lock_irqsave(spinlock_t *);
void spin_unlock_irqrestore(spinlock_t *, int);

// sleeplock.c
void mutex_lock(mutex_t *);
int mutex_trylock(mutex_t *);
void mutex_unlock(mutex_t *);
int holding_mutex(mutex_t *);
void mutex_init(mutex_t *, char *);
int mutex_lock_interruptible(mutex_t *);
int mutex_lock_timed(mutex_t *, uint64);

// syscall.c
uint64 argraw(int n);
void argint(int, int *);
void argint64(int, int64 *);
int argstr(int, char *, int);
void argaddr(int, uint64 *);
int fetchstr(uint64, char *, int);
int fetchaddr(uint64, uint64 *);
void syscall();

// trap.c
void usertrapret(void);
typedef struct ksiginfo ksiginfo_t;
typedef struct sigaction sigaction_t;
typedef struct stack stack_t;
typedef struct ucontext ucontext_t;
int push_sigframe(struct thread *p, int signo, sigaction_t *sa,
                  ksiginfo_t *info);
int restore_sigframe(struct thread *p, ucontext_t *ret_uc);

// uart.c
int uartinit(void);
void uartputc(int);
void uartputs(const char *, int);
int uartputs_nb(const char *, int);
int uart_tx_wait(void);
void uartputc_sync(int);
int uartgetc(void);

// vm.c
void kvminit(void);
void kvminithart(void);
void kvmmap(pagetable_t, uint64, uint64, uint64, int);
int mappages(pagetable_t, uint64, uint64, uint64, int);
pagetable_t uvmcreate(void);
void uvmunmap(pagetable_t, uint64, uint64, int);
pte_t *walk(pagetable_t, uint64, int, pte_t **, pte_t **);
uint64 walkaddr(pagetable_t, uint64);
int copyout(pagetable_t, uint64, char *, uint64);
int copyin(pagetable_t, char *, uint64, uint64);
int copyinstr(pagetable_t, char *, uint64, uint64);
void dump_pagetable(pagetable_t pagetable, int level, int indent,
                    uint64 va_base, uint64 va_end, bool omit_pa);

// plic.c
void plicinit(void);
void plicinithart(void);
int plic_claim(void);
void plic_complete(int);
void plic_enable_irq(int);

// virtio_disk.c
void virtio_disk_init(void);

// ramdisk.c
void ramdisk_init(void);

// backtrace.c
void print_backtrace(uint64 context, uint64 stack_start, uint64 stack_end);
void print_thread_backtrace(struct context *ctx, uint64 kstack,
                            int kstack_order);
void ksymbols_init(void);
void db_break(void);

// number of elements in fixed-size array
#define NELEM(x) (sizeof(x) / sizeof((x)[0]))

// pci.c
void pci_init();

// e1000.c
void e1000_init(uint32 *);
int e1000_transmit(struct mbuf *);

// netdev.c
void netdev_init(void);

// x1_emac.c
void x1_emac_init(void);

// x1_sdhci.c
void x1_sdhci_init(void);

// net.c / lwip_glue.c
void net_rx(struct mbuf *);
#ifndef USE_LWIP
void net_tx_udp(struct mbuf *, uint32, uint16, uint16);
#endif

// lwip_port/lwip_glue.c
#ifdef USE_LWIP
void lwip_net_init(void);
// lwip_port/sys_socket.c — syscall handlers registered in irq/syscall.c
#endif

// sysnet.c
void sockinit(void);
void sockclose(struct sock *);
int sockread(struct sock *, uint64, int);
int sockwrite(struct sock *, uint64, int);
int sockpoll(struct sock *, short);
void sockrecvudp(struct mbuf *, uint32, uint16, uint16);

#endif /* __KERNEL_DEFS_H */
