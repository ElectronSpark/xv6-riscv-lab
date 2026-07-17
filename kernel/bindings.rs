//! Hand-written bindings surface for the xv6 kernel crate (P3-6).
//!
//! This module replaces the last piece of C-type machinery: the
//! bindgen-generated `kernel_bindings.rs` (produced at build time from
//! `kernel/wrapper.h`, both deleted in wave P3-6). By that point type
//! nativization was complete — bindgen emitted **zero** struct layouts;
//! its entire output was (a) `pub use` facades re-exporting native Rust
//! types under their historical C spellings, (b) scalar/fn-pointer
//! typedefs, (c) integer constants lifted from C headers, and (d)
//! `extern "C"` fn/static declarations that no consumer referenced
//! through `crate::bindings::` any more (every caller carries its own
//! local `extern "C"` block — the crate-wide "mesh" convention).
//!
//! This file reproduces exactly the LIVE surface — every item some
//! `crate::bindings::…` path or `use crate::bindings::{…}` import
//! actually names (extraction: qualified-path + braced-import scan over
//! `kernel/**/*.rs`, comments stripped). Dead bindgen residue (unused
//! facade aliases, unused typedefs/consts, and ALL extern "C"
//! declarations) was dropped; the linker and the crate build are the
//! completeness proof.
//!
//! Constant VALUES below are copied verbatim from the last
//! bindgen-generated `kernel_bindings.rs` (rust-bindgen 0.72.1, riscv64
//! lp64d target, LAB=fs), NOT re-derived by hand from the headers; each
//! group's comment cites the C header the value originally came from.
//! The C headers stay in-tree for the remaining non-Rust consumers
//! (mkfs, user, .S); if a header value changes, this file must be
//! updated in the same change.

// ---------------------------------------------------------------------
// (a) Native-type facades: the historical C type names, re-exported
// from their native Rust definitions. Copied verbatim from the
// generated file (P3-N1..P3-4c raw_line facades); only the spellings
// live consumers use are kept.
// ---------------------------------------------------------------------

// kernel/inc/list_type.h -> kernel/list.rs
pub type list_node_t = crate::list::ListNode;
// kernel/inc/hlist_type.h -> kernel/hlist.rs
pub type hlist_bucket_t = crate::list::ListNode;
pub type hlist_entry = crate::hlist::RawHlistEntry;
pub type hlist_entry_t = crate::hlist::RawHlistEntry;
pub type hlist_func_struct = crate::hlist::RawHlistFunc;
pub type hlist_func_t = crate::hlist::RawHlistFunc;
pub type hlist_struct = crate::hlist::RawHlist;
pub type hlist_t = crate::hlist::RawHlist;
// kernel/inc/rbtree_type.h -> kernel/bintree.rs
pub type rb_node = crate::bintree::RawRbNode;
pub type rb_root = crate::bintree::RawRbRoot;
pub type rb_root_opts = crate::bintree::RawRbRootOpts;
// lock primitives (P3-N2)
pub use crate::lock::spinlock::RawSpinlock as spinlock;
pub use crate::lock::spinlock::RawSpinlock as spinlock_t;
pub use crate::lock::rwlock::Rwlock as rwlock;
pub use crate::lock::mutex::RawMutex as mutex_t;
pub use crate::lock::rwsem::RawRwsem as rwsem;
pub use crate::lock::rwsem::RawRwsem as rwsem_t;
pub use crate::lock::semaphore::RawSemaphore as semaphore;
pub use crate::lock::completion::RawCompletion as completion_t;
// thread queues (P3-N2)
pub use crate::proc::thread_queue::Tq as tq;
pub use crate::proc::thread_queue::Tq as tq_t;
pub use crate::proc::thread_queue::Ttree as ttree;
pub use crate::proc::thread_queue::Ttree as ttree_t;
pub use crate::proc::thread_queue::Tnode as tnode;
pub use crate::proc::thread_queue::Tnode as tnode_t;
// kobject/workqueue/process-group family (P3-N3)
pub use crate::kobject::Kobject as kobject;
pub use crate::proc::workqueue::WorkStruct as work_struct;
pub use crate::proc::workqueue::Workqueue as workqueue;
pub use crate::proc::workqueue::WorkqueueCallbacks as workqueue_callbacks;
pub use crate::proc::pgroup::Pgroup as pgroup;
pub use crate::proc::thread_group::ThreadGroup as thread_group;
pub use crate::tty::session::Session as session;
// signal family (P3-N4 + P3-4b)
pub use crate::proc::signal::SigAction as sigaction;
pub use crate::proc::signal::SigActs as sigacts;
pub use crate::proc::signal::SigActs as sigacts_t;
pub use crate::proc::signal::SigPending as sigpending;
pub use crate::proc::signal::SigPending as sigpending_t;
pub use crate::proc::signal::KsigInfo as ksiginfo;
pub use crate::proc::signal::TgSharedPending as tg_shared_pending;
pub use crate::proc::signal::SigStack as stack;
// device layer (P3-N4)
pub use crate::dev::dev::DeviceMajor as device_major_t;
pub use crate::dev::dev::DeviceOps as device_ops_t;
pub use crate::dev::dev::DeviceInstance as device_t;
pub use crate::dev::cdev::CdevOps as cdev_ops_t;
pub use crate::dev::cdev::Cdev as cdev_t;
pub use crate::dev::blkdev::BlkdevOps as blkdev_ops_t;
pub use crate::dev::blkdev::Blkdev as blkdev_t;
pub use crate::dev::bio::BioVec as bio_vec;
pub use crate::dev::bio::Bio as bio;
pub use crate::bufcache::Buf as buf;
pub use crate::dev::netdev::Netdev as netdev;
pub use crate::dev::netdev::NetdevOps as netdev_ops;
// vfs family (P3-N5, P3-N8)
pub use crate::vfs::inode::VfsDentry as vfs_dentry;
pub use crate::vfs::inode::VfsDirIter as vfs_dir_iter;
pub use crate::vfs::pipe::Pipe as pipe;
pub use crate::vfs::fdtable::VfsFdtable as vfs_fdtable;
pub use crate::vfs::fs::FsStruct as fs_struct;
pub use crate::vfs::file::VfsFile as vfs_file;
// `vfs_file_ops` is GONE (P3-10a): the fn-pointer table became the
// `crate::vfs::file::FileOps` trait (`&'static dyn` dispatch) — the
// first ops family with no C-compatible spelling at all.
pub use crate::vfs::fs::VfsFsType as vfs_fs_type;
// `vfs_fs_type_ops`/`vfs_superblock_ops` are GONE (P3-10b): the
// fn-pointer tables became the `crate::vfs::fs::FsTypeOps`/
// `SuperblockOps` traits (`&'static dyn` dispatch), P3-10a precedent.
pub use crate::vfs::fs::VfsSuperblock as vfs_superblock;
pub use crate::vfs::inode::VfsInode as vfs_inode;
// `vfs_inode_ops` is GONE (P3-10b): the 19-slot fn-pointer table became
// the `crate::vfs::inode::InodeOps` trait (`&'static dyn` dispatch).
pub use crate::vfs::inode::VfsInodeRef as vfs_inode_ref;
// mm family (P3-N6)
pub use crate::mm::slab::PercpuSlabCache as percpu_slab_cache_t;
pub use crate::mm::slab::SlabCacheStruct as slab_cache_t;
pub use crate::mm::slab::SlabStruct as slab_t;
pub use crate::mm::vm::Vm as vm;
pub use crate::mm::vm::Vm as vm_t;
pub use crate::mm::vm::Vma as vma;
pub use crate::mm::pcache::Pcache as pcache;
pub use crate::mm::pcache::PcacheOps as pcache_ops;
pub use crate::mm::pcache::PcacheNode as pcache_node;
pub use crate::mm::page::PageStruct as page_struct;
pub use crate::mm::page::PageStruct as page_t;
// rcu / timer / sched (P3-N7)
pub use crate::lock::rcu::RawRcuHead as rcu_head;
pub use crate::lock::rcu::RawRcuHead as rcu_head_t;
pub use crate::timer::timer_core::TimerRoot as timer_root;
pub use crate::timer::timer_core::TimerNode as timer_node;
pub use crate::proc::sched::SchedClass as sched_class;
pub use crate::proc::sched::SchedAttr as sched_attr;
pub use crate::proc::rq::Rq as rq;
pub use crate::proc::rq::RqPercpu as rq_percpu;
pub use crate::proc::rq::SchedEntity as sched_entity;
// tty / sock (P3-N8) + uabi terminal pair (P3-4b)
pub use crate::tty::tty::Tty as tty;
pub use crate::tty::tty::TtyOps as tty_ops;
pub use crate::tty::tty::Termios as termios;
pub use crate::tty::tty::Winsize as winsize;
pub use crate::sysnet::sock;
// tmpfs / xv6fs privates (P3-N8) + on-disk trio (P3-4a)
pub use crate::vfs::tmpfs::superblock::TmpfsSbPrivate as tmpfs_sb_private;
pub use crate::vfs::tmpfs::superblock::TmpfsSuperblock as tmpfs_superblock;
pub use crate::vfs::tmpfs::inode::TmpfsInode as tmpfs_inode;
pub use crate::vfs::tmpfs::inode::TmpfsDentry as tmpfs_dentry;
pub use crate::vfs::xv6fs::log::Xv6fsLogHeader as xv6fs_logheader;
pub use crate::vfs::xv6fs::log::Xv6fsLog as xv6fs_log;
pub use crate::vfs::xv6fs::block_cache::Xv6fsBlockCache as xv6fs_block_cache;
pub use crate::vfs::xv6fs::block_cache::FreeExtent as free_extent;
pub use crate::vfs::xv6fs::superblock::Xv6fsSuperblock as xv6fs_superblock;
pub use crate::vfs::xv6fs::inode::Xv6fsInode as xv6fs_inode;
pub use crate::vfs::xv6fs::inode::Dinode as dinode;
pub use crate::vfs::xv6fs::inode::Dirent as dirent;
// uabi stat pair (P3-4b)
pub use crate::vfs::file::Stat as stat;
pub use crate::vfs::fs::Statfs as statfs;
// exec format pair (P3-4c)
pub use crate::exec::Elfhdr as elfhdr;
pub use crate::exec::Proghdr as proghdr;
// platform / net / pci / NIC DMA / virtio (P3-4c)
pub use crate::dev::fdt::PlatformInfo as platform_info;
pub use crate::dev::fdt::MemRegion as mem_region;
pub use crate::net::Mbuf as mbuf;
pub use crate::pci::PciCommonConfspaceHeader as pci_common_confspace_header;
pub use crate::e1000::TxDesc as tx_desc;
pub use crate::e1000::RxDesc as rx_desc;
pub use crate::dev::x1_emac::X1RxDesc as x1_rx_desc;
pub use crate::dev::x1_emac::X1TxDesc as x1_tx_desc;
pub use crate::dev::yt8531::PhyState as phy_state;
pub use crate::virtio_disk::VirtqDesc as virtq_desc;
pub use crate::virtio_disk::VirtqAvail as virtq_avail;
pub use crate::virtio_disk::VirtqUsed as virtq_used;
pub use crate::virtio_disk::VirtioBlkReq as virtio_blk_req;
// thread family hub (P3-N9) + asm-locked trio (P3-5)
pub use crate::proc::signal::ThreadSignal as thread_signal_t;
pub use crate::proc::thread::Thread as thread;
pub use crate::irq::trap::Trapframe as trapframe;
pub use crate::irq::trap::Utrapframe as utrapframe;
pub use crate::proc::sched::Context as context;
pub use crate::ipi::CpuLocal as cpu_local;

// ---------------------------------------------------------------------
// Flexible-array-member helper. Byte-identical to bindgen's own
// definition (zero-sized; used by the native `Bio` and `TmpfsDentry`
// for their C flexible array members).
// ---------------------------------------------------------------------
#[repr(C)]
#[derive(Default)]
pub struct __IncompleteArrayField<T>(::core::marker::PhantomData<T>, [T; 0]);
impl<T> __IncompleteArrayField<T> {
    #[inline]
    pub const fn new() -> Self {
        __IncompleteArrayField(::core::marker::PhantomData, [])
    }
    #[inline]
    pub fn as_ptr(&self) -> *const T {
        self as *const _ as *const T
    }
    #[inline]
    pub fn as_mut_ptr(&mut self) -> *mut T {
        self as *mut _ as *mut T
    }
    #[inline]
    pub unsafe fn as_slice(&self, len: usize) -> &[T] {
        ::core::slice::from_raw_parts(self.as_ptr(), len)
    }
    #[inline]
    pub unsafe fn as_mut_slice(&mut self, len: usize) -> &mut [T] {
        ::core::slice::from_raw_parts_mut(self.as_mut_ptr(), len)
    }
}
impl<T> ::core::fmt::Debug for __IncompleteArrayField<T> {
    fn fmt(&self, fmt: &mut ::core::fmt::Formatter<'_>) -> ::core::fmt::Result {
        fmt.write_str("__IncompleteArrayField")
    }
}

// ---------------------------------------------------------------------
// (b) Scalar / fn-pointer typedefs. Copied from the generated file;
// origin headers cited per group.
// ---------------------------------------------------------------------

// kernel/inc/types.h
pub type uint = ::core::ffi::c_uint;
pub type ushort = ::core::ffi::c_ushort;
pub type uchar = ::core::ffi::c_uchar;
pub type uint8 = ::core::ffi::c_uchar;
pub type uint16 = ::core::ffi::c_ushort;
pub type uint32 = ::core::ffi::c_uint;
pub type uint64 = ::core::ffi::c_ulong;
pub type int16 = ::core::ffi::c_short;
pub type int32 = ::core::ffi::c_int;
pub type int64 = ::core::ffi::c_long;
pub type cpumask_t = uint64;
pub type mode_t = uint32;
pub type pid_t = ::core::ffi::c_int;
pub type loff_t = int64;
pub type dev_t = uint32;
/// C `bool` (kernel/inc/types.h): an enum in the kernel headers, so it
/// crossed bindgen as `c_uint`, spelled `bool_`.
pub type bool_ = ::core::ffi::c_uint;
// kernel/inc/riscv.h
pub type pte_t = uint64;
pub type pagetable_t = *mut uint64;
// kernel/inc/types.h (sleep/wakeup callback pair used by proc/tq.h)
pub type sleep_callback_t = ::core::option::Option<
    unsafe extern "C" fn(data: *mut ::core::ffi::c_void) -> ::core::ffi::c_int,
>;
pub type wakeup_callback_t = ::core::option::Option<
    unsafe extern "C" fn(data: *mut ::core::ffi::c_void, status: ::core::ffi::c_int),
>;
// kernel/inc/lock/rcu_type.h
pub type rcu_callback_t =
    ::core::option::Option<unsafe extern "C" fn(data: *mut ::core::ffi::c_void)>;
// kernel/inc/hlist_type.h
pub type ht_hash_t = uint64;
// kernel/inc/uabi/signal.h
pub type sigset_t = uint64;
pub type siginfo_t = crate::proc::signal::SigInfo;
pub type stack_t = stack;
// kernel/inc/proc/workqueue_types.h
pub type workqueue_lifecycle_cb_t =
    ::core::option::Option<unsafe extern "C" fn(arg1: *mut workqueue)>;
pub type workqueue_thread_lifecycle_cb_t =
    ::core::option::Option<unsafe extern "C" fn(arg1: *mut workqueue, arg2: *mut thread)>;
// kernel/inc/uabi/termios.h
pub type tcflag_t = uint32;
pub type cc_t = uint8;
pub type speed_t = uint32;
// kernel/inc/dev/netdev.h
/// Link-change callback type.
/// @dev:     the netdev whose link state changed
/// @link_up: 1 = link came up, 0 = link went down
pub type netdev_link_cb_t =
    ::core::option::Option<unsafe extern "C" fn(dev: *mut netdev, link_up: ::core::ffi::c_int)>;

// Constified C enums (bindgen's default representation: a `c_uint`
// alias + one const per live variant).
// kernel/inc/dev/dev_types.h `enum dev_type_e`
pub const dev_type_e_DEV_TYPE_BLOCK: dev_type_e = 1;
pub const dev_type_e_DEV_TYPE_CHAR: dev_type_e = 2;
pub type dev_type_e = ::core::ffi::c_uint;
// kernel/inc/proc/thread_types.h `enum thread_state`
pub const thread_state_THREAD_INTERRUPTIBLE: thread_state = 2;
pub const thread_state_THREAD_UNINTERRUPTIBLE: thread_state = 6;
pub type thread_state = ::core::ffi::c_uint;
// kernel/inc/mm/slab_type.h `enum` slab_state_t (no live variant consts)
pub type slab_state_t = ::core::ffi::c_uint;

// ---------------------------------------------------------------------
// (c) Integer constants. Values copied verbatim from the generated
// file; origin headers cited per group.
// ---------------------------------------------------------------------

// kernel/inc/riscv.h: page geometry, PTE bits, canonical VA limit.
pub const PGSIZE: u32 = 4096;
pub const PGSHIFT: u32 = 12;
pub const PTE_V: u32 = 1;
pub const PTE_R: u32 = 2;
pub const PTE_W: u32 = 4;
pub const PTE_X: u32 = 8;
pub const PTE_U: u32 = 16;
pub const PTE_A: u32 = 64;
pub const PTE_D: u32 = 128;
pub const PTE_RSW_w: u32 = 256;
pub const MAXVA: u64 = 274877906944;

// kernel/inc/errno.h (musl-libc numbering; positive values — Rust-side
// consumers negate at the C-ABI boundary per the kstd convention).
pub const EPERM: u32 = 1;
pub const ENOENT: u32 = 2;
pub const ESRCH: u32 = 3;
pub const EINTR: u32 = 4;
pub const EIO: u32 = 5;
pub const ENXIO: u32 = 6;
pub const EBADF: u32 = 9;
pub const EAGAIN: u32 = 11;
pub const ENOMEM: u32 = 12;
pub const EACCES: u32 = 13;
pub const EFAULT: u32 = 14;
pub const EBUSY: u32 = 16;
pub const EEXIST: u32 = 17;
pub const EXDEV: u32 = 18;
pub const ENODEV: u32 = 19;
pub const ENOTDIR: u32 = 20;
pub const EISDIR: u32 = 21;
pub const EINVAL: u32 = 22;
pub const EMFILE: u32 = 24;
pub const ENOTTY: u32 = 25;
pub const ETXTBSY: u32 = 26;
pub const EFBIG: u32 = 27;
pub const ENOSPC: u32 = 28;
pub const ESPIPE: u32 = 29;
pub const EPIPE: u32 = 32;
pub const ERANGE: u32 = 34;
pub const EDEADLK: u32 = 35;
pub const ENAMETOOLONG: u32 = 36;
pub const ENOSYS: u32 = 38;
pub const ENOTEMPTY: u32 = 39;
pub const ELOOP: u32 = 40;
pub const EOVERFLOW: u32 = 75;
pub const EOPNOTSUPP: u32 = 95;
pub const EADDRINUSE: u32 = 98;
pub const ETIMEDOUT: u32 = 110;
pub const EALREADY: u32 = 114;
pub const EINPROGRESS: u32 = 115;

// kernel/inc/param.h
pub const NCPU: u32 = 8;
pub const USERSTACK_GROWTH: u32 = 8;
pub const MAXUSTACK: u32 = 32;
pub const PAGE_SHIFT: u32 = 12;

// kernel/inc/lock/rwsem_types.h
pub const RWLOCK_PRIO_READ: u32 = 0;
pub const RWLOCK_PRIO_WRITE: u32 = 1;

// kernel/inc/uabi/mman.h
pub const PROT_NONE: u32 = 0;
pub const PROT_READ: u32 = 1;
pub const PROT_WRITE: u32 = 2;
pub const PROT_EXEC: u32 = 4;
pub const MAP_SHARED: u32 = 1;
pub const MAP_PRIVATE: u32 = 2;
pub const MAP_FIXED: u32 = 16;
pub const MAP_ANONYMOUS: u32 = 32;
pub const MREMAP_MAYMOVE: u32 = 1;
pub const MREMAP_FIXED: u32 = 2;
pub const MADV_NORMAL: u32 = 0;
pub const MADV_RANDOM: u32 = 1;
pub const MADV_SEQUENTIAL: u32 = 2;
pub const MADV_WILLNEED: u32 = 3;
pub const MADV_DONTNEED: u32 = 4;

// kernel/inc/mm/vm_types.h
pub const PROT_MASK: u32 = 199;
pub const VMA_FLAG_USER: u32 = 8;
pub const VMA_FLAG_GROWSDOWN: u32 = 256;
pub const VMA_FLAG_GROWSUP: u32 = 512;
pub const VMA_FLAG_FILE: u32 = 1024;
pub const VMA_FLAG_PROT_MASK: u32 = 1807;

// kernel/inc/uabi/termios.h
pub const OPOST: u32 = 1;
pub const ONLCR: u32 = 2;

// kernel/inc/mm/slab_type.h
pub const SLAB_FLAG_STATIC: u32 = 1;
pub const SLAB_FLAG_EMBEDDED: u32 = 2;
pub const SLAB_FLAG_DEBUG_BITMAP: u32 = 4;

// kernel/inc/mm/memlayout.h (user address-space layout)
pub const UVMBOTTOM: u32 = 4096;
pub const UVMTOP: u64 = 273804165120;
pub const TRAPFRAME: u64 = 273803902976;
pub const USTACKTOP: u64 = 273803898880;
pub const USTACK_MAX_BOTTOM: u64 = 273803767808;
pub const UHEAP_MAX_TOP: u64 = 68719480832;

// kernel/inc/dev/fdt.h (platform_info table geometry)
pub const PCIE_REG_MAX: u32 = 8;
pub const EMAC_MAX: u32 = 2;
pub const SDHCI_MAX: u32 = 3;

// kernel/inc/dev/virtio.h
pub const N_VIRTIO: u32 = 3;

// ---------------------------------------------------------------------
// (d) extern "C" declarations: NONE. The generated file's 52 fn
// declarations (spin_*, rwsem_*, tq_*/tnode_init, hlist_*, rb_*,
// rcu_*/call_rcu, slab_*, page_ref_*, vfs_f*/vfs_inode_deref/
// vfs_fdtable_get_file, mappages/walkaddr, scheduler_sleep/
// sched_timer_*/sleep_ms/signal_pending, printf) and the one extern
// static (`platform`) had zero `crate::bindings::` consumers — every
// caller declares its own local `extern "C"` block, and all of these
// symbols are `#[no_mangle]` Rust (or .S) definitions elsewhere in the
// kernel, so dropping the unused declarations changes nothing at link
// time.
// ---------------------------------------------------------------------
