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
struct vfs_file;
struct fb_gpu_stats;
struct fb_gpu_virgl_resource_create;
struct fb_gpu_virgl_transfer;
struct hyperv_dxg_status {
    int global_present;
    int vgpu_present;
    int global_gpadl_ok;
    int vgpu_gpadl_ok;
    int global_open_ok;
    int vgpu_open_ok;
    uint32 global_relid;
    uint32 vgpu_relid;
    uint32 global_gpadl_status;
    uint32 vgpu_gpadl_status;
    uint32 global_open_status;
    uint32 vgpu_open_status;
    uint32 global_rx_packets;
    uint32 vgpu_rx_packets;
    uint32 adapter_type_rewrites;
    uint32 adapter_type_raw_value;
    uint32 adapter_type_wsl_value;
    uint32 adapter_render_supported;
    uint32 adapter_display_supported;
    uint32 adapter_paravirtualized;
    uint32 adapter_compute_only;
    uint32 adapter_source_count;
    uint32 adapter_sources_known;
    uint32 enum_adapter_count;
    uint32 enum_adapter_handle;
    uint32 enum_adapter_luid_low;
    uint32 enum_adapter_luid_high;
    uint32 user_adapter_luid_low;
    uint32 user_adapter_luid_high;
};
struct hyperv_video_status {
    int present;
    int gpadl_ok;
    int open_ok;
    int initialized;
    int dirt_needed;
    uint32 child_relid;
    uint32 gpadl_status;
    uint32 open_status;
    uint64 vram_gpa;
    uint32 width;
    uint32 height;
    uint32 pitch;
    uint32 bpp;
};
typedef struct page_struct page_t;

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

// buffer.c (buffer_head — replaces bio.c bcache)
void bh_global_init(void);
// console.c
void consoleinit(void);
void consoledevinit(void);
void nullranddevinit(void);
void ossaudiodevinit(void);
void random_fill_bytes(void *buf, size_t count);
void fbdevinit(void);
void fb_gpu_destroy_owner(pid_t owner_tgid);
void fb_gpu_destroy_render_owner(uint64 owner_id);
void ps2mouse_init(void);
void hyperv_input_init(void);
void hyperv_input_intr(void);
void hyperv_storvsc_init(void);
void hyperv_netvsc_init(void);
void hyperv_video_dirty(uint32 x, uint32 y, uint32 w, uint32 h);
int hyperv_video_get_status(struct hyperv_video_status *status);
int hyperv_dxg_transport_ready(void);
int hyperv_dxg_d3dkmt_ready(void);
int hyperv_dxg_get_status(struct hyperv_dxg_status *status);
struct hyperv_dxg_shared_resource_snapshot {
    uint32 kind;
    uint32 fops_kind;
    uint32 device;
    uint32 resource;
    uint32 allocation_count;
    uint32 first_allocation;
    uint32 sealed;
    uint32 shared_records_valid;
    uint32 generation;
    uint32 host_shared_refs;
    uint32 shared_parent_id;
    uint32 shared_parent_refs;
    uint32 shared_parent_children;
};
struct hyperv_dxg_display_bind_pin_snapshot {
    void *dxg_file_cookie;
    void *resource_file_cookie;
    uint32 dxg_file_pinned;
    uint32 resource_file_pinned;
    uint32 kind;
    uint32 fops_kind;
    uint32 device;
    uint32 resource;
    uint32 allocation_count;
    uint32 first_allocation;
    uint32 sealed;
    uint32 shared_records_valid;
    uint32 generation;
    uint32 host_shared_refs;
    uint32 process;
    uint32 process_generation;
    uint32 process_refs;
    uint32 process_adapter_generation;
    uint32 process_adapter_refs;
    uint32 hmgr_index_unique_valid;
    uint32 device_object_ref_active;
    uint32 resource_object_ref_active;
    uint32 allocation_object_ref_active;
    uint32 shared_parent_id;
    uint32 shared_parent_refs;
    uint32 shared_parent_children;
    uint32 shared_parent_snapshot_valid;
    uint32 opened_child_snapshot_valid;
};
struct hyperv_dxg_display_bind_request {
    uint32 present_source;
    uint64 source_generation;
    uint64 resource_generation;
    uint32 flags;
    uint32 sync_object;
    uint64 fence_value;
    int32 dxg_fd;
    int32 resource_fd;
    uint32 device;
    uint32 resource;
    uint32 allocation;
    uint32 allocation_count;
    uint32 width;
    uint32 height;
    uint32 pitch;
    uint32 format;
    uint64 modifier;
    uint32 adapter_luid_low;
    uint32 adapter_luid_high;
    uint32 adapter_identity;
    uint32 provenance_flags;
    uint64 required_metadata;
    uint64 lifetime;
    uint64 block_reason;
    uint32 pin_valid;
    struct hyperv_dxg_display_bind_pin_snapshot pin;
};
struct hyperv_dxg_display_bind_result {
    int status;
    uint32 transport;
    uint32 operation;
    uint32 completion_source;
    uint64 present_id;
    uint64 completed_id;
    uint64 source_generation;
    uint64 resource_generation;
    uint64 block_reason;
    uint32 host_abi_present;
    uint32 sender_present;
    uint32 completion_present;
    uint32 pin_revalidated;
    uint32 no_host_abi;
    uint32 no_sender;
    uint32 no_completion;
    uint32 preflight_ready;
    uint32 send_attempts;
    uint32 send_blocked_no_host_abi;
    uint32 completion_demux_attempts;
    uint32 completion_demux_blocked_no_contract;
    uint32 publication_attempted;
    uint32 publish_before_send;
    uint64 transport_pending_id;
    uint32 command_id;
    uint64 transaction_id;
    uint32 channel;
    uint32 completion_demux_registered;
    uint32 transport_source;
    uint32 host_saw_packet;
    uint32 wsl_presenthistory_completion_credit;
    uint32 resolved_or_cancelled;
    uint32 refs_released;
    uint32 no_host_abi_cancelled;
    uint32 no_host_abi_refs_released;
    uint64 pending_owner_generation;
    uint64 pending_source_generation;
    uint64 pending_resource_generation;
    uint64 pending_dxgprocess_generation;
    uint64 pending_process_adapter_generation;
    uint32 pending_hmgr_index_unique_valid;
    uint32 pending_device_object_ref_active;
    uint32 pending_resource_object_ref_active;
    uint32 pending_allocation_object_ref_active;
    uint32 pending_shared_parent_snapshot_valid;
    uint32 pending_opened_child_snapshot_valid;
    uint32 pending_syncobject_object_ref_active;
    uint32 pending_owner_close_cancelled;
    uint32 request_metadata_complete;
    uint32 request_sync_metadata_complete;
    uint64 request_missing_metadata;
};
int hyperv_dxg_shared_resource_snapshot_from_fd(
    int fd, struct hyperv_dxg_shared_resource_snapshot *snapshot);
int hyperv_dxg_shared_resource_snapshot_from_opened_resource(
    int dxg_fd, int resource_fd, uint32 device, uint32 resource,
    uint32 allocation, uint32 allocation_count,
    struct hyperv_dxg_shared_resource_snapshot *snapshot);
int hyperv_dxg_display_bind_pin_from_fds(
    int dxg_fd, int resource_fd, uint32 device, uint32 resource,
    uint32 allocation, uint32 allocation_count,
    struct hyperv_dxg_display_bind_pin_snapshot *snapshot);
void hyperv_dxg_display_bind_unpin(
    struct hyperv_dxg_display_bind_pin_snapshot *snapshot);
int hyperv_dxg_display_bind_submit(
    const struct hyperv_dxg_display_bind_request *bind,
    struct hyperv_dxg_display_bind_result *result);
int hyperv_dxg_display_bind_submit_failclosed(
    const struct hyperv_dxg_display_bind_request *bind,
    struct hyperv_dxg_display_bind_result *result);
int hyperv_dxg_display_bind_cancel(
    uint64 transport_pending_id, uint64 source_generation,
    uint64 resource_generation, uint64 reason,
    struct hyperv_dxg_display_bind_result *result);
int hyperv_dxg_display_bind_cancel_failclosed(
    uint64 transport_pending_id, uint64 source_generation,
    uint64 resource_generation, uint64 reason,
    struct hyperv_dxg_display_bind_result *result);
void hyperv_dxg_note_pci(uint32 domain, uint32 bus, uint32 dev, uint32 func,
                         uint32 vendor, uint32 device, uint32 class_code,
                         uint32 guid0, uint32 guid1, uint32 guid2,
                         uint32 guid3, uint32 vmbus_version,
                         uint32 luid_low, uint32 luid_high,
                         uint32 guestcaps_offset, uint32 guestcaps_value,
                         uint32 guestcaps_readback, int guestcaps_ret);
void ps2kbd_init(void);
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
void futex_exit_robust_list(struct thread *p);

// timerfd.c
void timerfd_init(void);
int eventfd_file_is_eventfd(struct vfs_file *file);
int eventfd_signal_file(struct vfs_file *file, uint64 value);

// Legacy file.c removed - now using VFS exclusively (vfs/file.c)

// Legacy fs.c removed - now using VFS exclusively
// See kernel/vfs/fs.h for VFS interfaces

// ramdisk.c
void ramdiskinit(void);
void ramdiskintr(void);
void ramdiskrw(struct buf *);

// dev/gendisk.c
void gendisk_init(void);

// dev/loop.c
void loop_init(void);

// kalloc.c
void *kalloc(void);
void kfree(void *);
void kinit(void);
void *kmm_alloc(size_t);
void kmm_free(void *);
void kmm_shrink_all(void);
uint64 get_total_free_pages(void);

// mm_watermark.c
void mm_watermark_init(void);
void kswapd_start(void);
void mm_watermark_dump(void);

// oom_kill.c
void oom_init(void);

// shrinker.c
void shrinker_init(void);

// Legacy log.c removed - logging now handled by VFS xv6fs subsystem
// See kernel/vfs/xv6fs/log.c for VFS logging interfaces

// swtch.S
struct context *arch_context_switch(struct context *cur, struct context *target);
typedef void (*sw_noret_cb_t)(uint64, uint64);
void __switch_noreturn(uint64 irq_sp, uint64 s0, sw_noret_cb_t addr);

// fpu.S — lazy FPU switching
struct fpu_state;
void fpu_save_state(struct fpu_state *state);
void fpu_restore_state(struct fpu_state *state);
void fpu_init_state(void);
void fpu_cpu_init(void);

// spinlock.c
// Initialize a spinlock.
void spin_init(spinlock_t *, char *);
// Check if the current CPU is holding the lock.
int spin_holding(spinlock_t *);
// Basic spin lock functions. Will NOT modify preempt counter or disable
// interrupts.
void spin_acquire(spinlock_t *lock) __acquires(lock);
void spin_release(spinlock_t *lock) __releases(lock);
// Default spin lock functions. Will modify preempt counter.
void spin_lock(spinlock_t *lock) __acquires(lock);
void spin_unlock(spinlock_t *lock) __releases(lock);
int spin_trylock(spinlock_t *lock);
// Functions to save/restore interrupt state with spinlock.
int spin_lock_irqsave(spinlock_t *lock) __acquires(lock);
void spin_unlock_irqrestore(spinlock_t *lock, int intena) __releases(lock);

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
void arch_vm_init(void);
void arch_vm_init_hart(void);
void kvmmap(pagetable_t, uint64, uint64, uint64, uint64);
int mappages(pagetable_t, uint64, uint64, uint64, uint64);
void uvmunmap(pagetable_t, uint64, uint64, int);
pte_t *walk(pagetable_t, uint64, int, pte_t **, pte_t **);
uint64 walkaddr(pagetable_t, uint64);
pde_t *pgtab_alloc(void);
void pgtab_free(void *);
void vm_slab_init(void);
void kernel_vm_init(void);
void vm_asid_init(uint16 max_asid);
uint16 vm_asid_max(void);
uint16 vm_asid_gen(void);

// plic.c
void arch_irq_init(void);
void arch_irq_init_hart(void);
int plic_claim(void);
void plic_complete(int);
void plic_enable_irq(int);

// virtio_disk.c
void virtio_disk_init(void);
void virtio_gpu_init(void);
void virtio_input_init(void);
void virtio_gpu_get_fb_stats(struct fb_gpu_stats *stats);
int virtio_gpu_has_virgl(void);
int virtio_gpu_probe_scanout(uint32 *width, uint32 *height);
int virtio_gpu_probe_edid_mode(uint32 *width, uint32 *height,
                               uint32 *refresh_millihz);
int virtio_gpu_resize_scanout(uint32 width, uint32 height);
void virtio_gpu_present_fb_rect(volatile void *fb, uint32 src_pitch,
                                uint32 x, uint32 y, uint32 w, uint32 h);
int virtio_gpu_user_context_create(uint64 owner_id, pid_t owner_tgid,
                                   uint32 capset_id, const char *name,
                                   uint32 *ctx_id);
int virtio_gpu_user_context_destroy(uint64 owner_id, pid_t owner_tgid,
                                    uint32 ctx_id);
int virtio_gpu_user_submit(uint64 owner_id, pid_t owner_tgid, uint32 ctx_id,
                           uint32 flags, const uint32 *cmds,
                           uint32 nr_dwords, uint64 *fence, uint64 *signaled);
int virtio_gpu_user_fence(uint64 wait_for, int wait, uint64 *signaled);
int virtio_gpu_user_capset_ids(uint64 *ids);
int virtio_gpu_user_get_caps_for(uint32 requested_capset_id,
                                 uint32 requested_capset_version,
                                 void *buf, uint32 buf_size,
                                 uint32 *capset_id, uint32 *capset_version,
                                 uint32 *capset_size);
int virtio_gpu_user_get_caps(void *buf, uint32 buf_size, uint32 *capset_id,
                             uint32 *capset_version, uint32 *capset_size);
int virtio_gpu_user_resource_create(uint64 owner_id, pid_t owner_tgid,
                                    struct fb_gpu_virgl_resource_create *req);
int virtio_gpu_user_resource_destroy(uint64 owner_id, pid_t owner_tgid,
                                     uint32 resource_id);
int virtio_gpu_user_resource_export_pages(uint64 owner_id, pid_t owner_tgid,
                                          uint32 resource_id, uint32 *width,
                                          uint32 *height, uint32 *pitch,
                                          uint64 *size, page_t ***pages_out,
                                          uint32 *npages_out);
int virtio_gpu_user_resource_info(uint64 owner_id, pid_t owner_tgid,
                                  uint32 resource_id, uint32 *width,
                                  uint32 *height, uint32 *format,
                                  uint64 *size);
void *virtio_gpu_user_resource_page(uint64 owner_id, pid_t owner_tgid,
                                    uint32 resource_id, uint64 page_index);
void virtio_gpu_user_destroy_owner(pid_t owner_tgid);
void virtio_gpu_user_destroy_render_owner(uint64 owner_id);
int virtio_gpu_user_transfer(uint64 owner_id, pid_t owner_tgid,
                             struct fb_gpu_virgl_transfer *req,
                             int from_host);

// ramdisk.c
void ramdisk_init(void);

// ksymbols.c
void ksymbols_init(void);
void db_break(void);

// arch backtrace.c
void print_backtrace(uint64 context, uint64 stack_start, uint64 stack_end);
void print_thread_backtrace(struct context *ctx, uint64 kstack,
                            int kstack_order);
void print_user_backtrace(pagetable_t pgtbl, uint64 fp, uint64 ra,
                          uint64 sp, uint64 sepc, int max);

// coredump.c
void do_coredump(struct thread *t);

// number of elements in fixed-size array
#define NELEM(x) (sizeof(x) / sizeof((x)[0]))

// pci.c
void pci_init();

// e1000.c
void e1000_init(uint32 *);
int e1000_transmit(struct mbuf *);
void e1000_poll_rx(void);

// netdev.c
void netdev_init(void);

// x1_emac.c
void x1_emac_init(void);

// x1_sdhci.c
void x1_sdhci_init(void);

// x1_i2c.c
int x1_i2c_init(void);

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
