// Build script for the xv6_rust crate.
//
// Uses bindgen to generate Rust declarations from a curated set of kernel
// C headers (see `wrapper.h`). The include paths used by the kernel C
// build are passed in via the `XV6_KERNEL_INCLUDES` environment variable
// as a `;`-separated list (set by the top-level CMakeLists.txt rule that
// invokes `cargo build`). When the variable is absent (e.g. running
// `cargo build` outside the CMake build) we fall back to deriving the
// paths relative to `CARGO_MANIFEST_DIR`.

use std::env;
use std::path::PathBuf;

/// P3-N1 nativization support: bindgen cannot inspect blocklisted types,
/// so by default it assumes they implement none of the derivable traits.
/// That silently changes codegen for *other* structs that embed them —
/// e.g. `page_struct`'s anonymous union degrades from a real `union` to
/// `__BindgenUnionField` wrappers when its `list_node` member is no
/// longer known to be `Copy`. Answer the trait queries for our
/// hand-written native replacements (all are `#[derive(Copy, Clone)]`,
/// none derive `Debug`/`Default`/`Hash`/`PartialEq` — matching
/// `derive_debug(false)`/`derive_default(false)` above).
#[derive(Debug)]
struct NativeTypeCallbacks;

impl bindgen::callbacks::ParseCallbacks for NativeTypeCallbacks {
    fn blocklisted_type_implements_trait(
        &self,
        name: &str,
        derive_trait: bindgen::callbacks::DeriveTrait,
    ) -> Option<bindgen::callbacks::ImplementsTrait> {
        use bindgen::callbacks::{DeriveTrait, ImplementsTrait};
        const NATIVE_TYPES: &[&str] = &[
            // kernel/list.rs
            "list_node",
            "list_node_t",
            // kernel/hlist.rs
            "hlist_bucket_t",
            "hlist_entry",
            "hlist_entry_t",
            "hlist_func_struct",
            "hlist_func_t",
            "hlist_struct",
            "hlist_t",
            // kernel/bintree.rs
            "rb_node",
            "rb_root",
            "rb_root_opts",
            // kernel/lock/spinlock.rs (P3-N2)
            "spinlock",
            "spinlock_t",
            // kernel/lock/rwlock.rs (P3-N2)
            "rwlock",
            // kernel/proc/thread_queue.rs (P3-N2). `ttree` derived
            // no Copy in the old bindgen output (derive-analysis quirk
            // around its blocklisted `rb_root` member), but the native
            // `Ttree` genuinely is Copy and nothing in the remaining
            // bindgen output embeds it by value (verified), so Yes is
            // the accurate answer. `tq_type_t` is a plain c_uint alias.
            "tq",
            "tq_t",
            "ttree",
            "ttree_t",
            "tq_type_t",
            // kernel/lock/{mutex,rwsem,semaphore,completion}.rs (P3-N2)
            "mutex",
            "mutex_t",
            "rwsem",
            "rwsem_t",
            "semaphore",
            "sem_t",
            "completion_t",
            // kernel/kobject.rs (P3-N3). Both derive Copy/Clone exactly
            // as the pre-nativization bindgen output did; `kobject` is
            // still embedded by value by remaining bindgen structs
            // (`device.kobj`, `bio.kobj`, `inode.kobj`), so an accurate
            // Yes here is what keeps their own derive lines unchanged.
            "kobject",
            "kobject_ops",
            // kernel/proc/workqueue.rs (P3-N3). `work_struct` is still
            // embedded by value by the remaining bindgen `blkdev`
            // (`flush_work`); all three derived Copy/Clone in the
            // pre-nativization bindgen output.
            "work_struct",
            "workqueue",
            "workqueue_callbacks",
            // kernel/proc/pgroup.rs (P3-N3)
            "pgroup",
            // kernel/proc/thread_group.rs (P3-N3)
            "thread_group",
            // kernel/tty/session.rs (P3-N3)
            "session",
        ];
        // P3-N2: natives whose hand-written definitions deliberately do
        // NOT derive Copy/Clone (matching the pre-nativization bindgen
        // output, where `tnode` derived neither). Kept separate so the
        // trait answers stay per-type accurate: nothing left in the
        // bindgen output embeds these by value (verified), but a wrong
        // Yes here could silently re-derive Copy on a future embedder.
        const NONCOPY_NATIVE_TYPES: &[&str] = &[
            // kernel/proc/thread_queue.rs (P3-N2)
            "tnode",
            "tnode_t",
        ];
        let is_copy = if NATIVE_TYPES.contains(&name) {
            true
        } else if NONCOPY_NATIVE_TYPES.contains(&name) {
            false
        } else {
            return None;
        };
        match derive_trait {
            DeriveTrait::Copy => Some(if is_copy {
                ImplementsTrait::Yes
            } else {
                ImplementsTrait::No
            }),
            DeriveTrait::Debug
            | DeriveTrait::Default
            | DeriveTrait::Hash
            | DeriveTrait::PartialEqOrPartialOrd => Some(ImplementsTrait::No),
        }
    }
}

fn main() {
    println!("cargo:rerun-if-changed=wrapper.h");
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=XV6_KERNEL_INCLUDES");

    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    // manifest_dir is the `kernel/` directory itself.
    let kernel_dir = manifest_dir.clone();
    let src_dir = kernel_dir.parent().expect("kernel must live under workspace root");

    let mut include_dirs: Vec<PathBuf> = Vec::new();
    if let Ok(env_includes) = env::var("XV6_KERNEL_INCLUDES") {
        for path in env_includes.split(';').filter(|s| !s.is_empty()) {
            include_dirs.push(PathBuf::from(path));
        }
    }
    if include_dirs.is_empty() {
        // Fallback: mirror the flags from kernel/CMakeLists.txt
        include_dirs.push(src_dir.to_path_buf());
        include_dirs.push(kernel_dir.join("inc"));
    }

    let mut builder = bindgen::Builder::default()
        .header("wrapper.h")
        .use_core()
        .ctypes_prefix("::core::ffi")
        .derive_default(false)
        .derive_debug(false)
        .layout_tests(false)
        // Target the kernel ABI, not the host. libclang needs the explicit
        // target so it picks the right size_t / pointer width and the
        // RISC-V predefined macros the headers rely on.
        .clang_arg("-target").clang_arg("riscv64-unknown-elf")
        .clang_arg("-mcmodel=medany")
        .clang_arg("-march=rv64gc")
        .clang_arg("-mabi=lp64d")
        .clang_arg("-ffreestanding")
        .clang_arg("-nostdinc")
        .clang_arg("-D__riscv")
        .clang_arg("-D__riscv_xlen=64")
        // Kernel headers use C11 `_Atomic` types; without explicit
        // `-std=gnu11` clang's bindgen front-end rejects atomic builtins
        // on those fields.
        .clang_arg("-std=gnu11")
        // Stub the GCC atomic builtins for bindgen's clang front-end:
        // they cannot operate on `_Atomic`-qualified types in clang's
        // type checker (see kernel/inc/proc/thread_group.h). Bindgen
        // only needs the header to *parse*, not to behave correctly at
        // runtime, so reducing the builtins to plain deref/assign is safe.
        .clang_arg("-D__atomic_load_n(p,m)=(*(p))")
        .clang_arg("-D__atomic_store_n(p,v,m)=((void)(*(p)=(v)))")
        .clang_arg("-D__atomic_exchange_n(p,v,m)=(*(p))")
        // Suppress noisy warnings from kernel headers.
        .clang_arg("-Wno-everything");

    for dir in &include_dirs {
        builder = builder.clang_arg(format!("-I{}", dir.display()));
    }

    // LAB defines are required because some headers gate code on LAB_FS etc.
    if let Ok(lab) = env::var("LAB") {
        let upper = lab.to_uppercase();
        builder = builder
            .clang_arg(format!("-DLAB_{}", upper))
            .clang_arg(format!("-DSOL_{}", upper));
    }
    builder = builder.clang_arg("-DUSE_SOFTWARE_FFS=1");

    // Only allow the symbols we actually want exposed. Keeping the
    // allow-list explicit avoids bloating the generated bindings with
    // unrelated kernel APIs and keeps build times reasonable.
    builder = builder
        .allowlist_type("vma_t|vm_t|vm|vma")
        .allowlist_type("cpu_local|thread")
        // Added to support full Rust port of kernel/proc/proc_rust_shims.c.
        .allowlist_type("pgroup|thread_group|session|tg_shared_pending")
        .allowlist_type("ksiginfo|sigpending_t|sigaction|sigset_t|signal_struct")
        .allowlist_type("sched_attr|hlist_bucket_t|hlist_entry_t|hlist_func_t")
        .allowlist_type("ht_hash_t|ht_hash_func_t")
        .allowlist_type("rb_node|rb_root|rb_root_opts")
        .allowlist_type("kobject|kobject_ops")
        .allowlist_type("list_node_t|list_node|list_entry_t")
        .allowlist_type("rwsem_t|spinlock_t")
        .allowlist_type("rwlock|rwlock_t")
        .allowlist_type("completion|completion_t")
        .allowlist_type("mutex|mutex_t|semaphore|semaphore_t")
        .allowlist_type("tq|tq_t|ttree|ttree_t|tnode|tnode_t|tq_type_t")
        .allowlist_type("workqueue|work_struct")
        .allowlist_type("rq|rq_percpu|sched_entity|sched_class|load_weight")
        .allowlist_type("pcache|pcache_node|pcache_ops")
        .allowlist_type("pagetable_t|pte_t|pde_t|cpumask_t")
        .allowlist_type("slab_cache_t|slab_t|slab_struct")
        .allowlist_type("vfs_file|vfs_inode")
        // Phase 2 Wave 13 (vfs/inode.c port): full field layouts for the
        // rest of the vfs_types.h graph (already reachable transitively
        // through vfs_file/vfs_inode's own fields; listed explicitly
        // here for the same defensiveness as the wrapper.h comment).
        .allowlist_type("vfs_superblock|vfs_superblock_ops|vfs_inode_ops")
        .allowlist_type("vfs_dentry|vfs_dir_iter|vfs_fdtable|vfs_fs_type|vfs_fs_type_ops")
        .allowlist_type("vfs_file_ops|vfs_inode_ref|fs_struct")
        .allowlist_type("platform_info|mem_region")
        .allowlist_type("page_type")
        .allowlist_type("timer_node|timer_root")
        .allowlist_type("thread_state")
        // Phase 2 Wave 4 (console.c port): device/cdev vtable types +
        // the tty/termios layout consolewrite reads (c_oflag).
        .allowlist_type("device_t|device_instance|device_ops_t|device_major_t")
        .allowlist_type("cdev_t|cdev|cdev_ops_t|dev_type_e")
        .allowlist_type("tty|tty_ops|termios|winsize")
        // Phase 2 Wave 18 (tmpfs port): tmpfs's private in-memory inode/
        // superblock/dentry layout types (kernel/vfs/tmpfs/tmpfs_private.h,
        // added to wrapper.h). Real field layouts, not opaque stand-ins --
        // kernel/vfs/devtmpfs/superblock.c (still C) needs the identical
        // layout via container_of on the very same header.
        .allowlist_type("tmpfs_inode|tmpfs_superblock|tmpfs_sb_private|tmpfs_dentry")
        // Phase 2 Wave 19 (xv6fs port): xv6fs's private in-memory structures
        // (kernel/vfs/xv6fs/xv6fs_private.h, added to wrapper.h) plus,
        // transitively, the on-disk format structs shared with mkfs
        // (vfs/xv6fs/ondisk.h -- untouched, ABI surface) and the Wave-1
        // Rust-rbtree-indexed free-block-extent cache
        // (vfs/xv6fs/block_cache.h). Real field layouts throughout, no
        // opaque stand-ins -- same rationale as every other filesystem
        // driver wave.
        .allowlist_type("xv6fs_superblock|xv6fs_inode|xv6fs_log|xv6fs_logheader")
        .allowlist_type("xv6fs_block_cache|free_extent")
        .allowlist_type("^superblock$|^dinode$|^dirent$")
        // dev/buf.h's classic xv6 buffer-cache `struct buf` (kernel/bio.c,
        // unchanged C) -- xv6fs reads/writes `bp->data`/`bp->blockno`
        // directly. dev/bio_types.h's `struct bio`/`bio_vec` (dev/bio.c,
        // unchanged C) -- xv6fs's per-inode pcache read_page/write_page
        // set `bio->blkno` directly before submitting. dev/dev_types.h's
        // `blkdev_t` -- xv6fs reads `blkdev->dev.{major,minor}` via the
        // `xv6fs_sb_dev` macro (reimplemented natively in Rust).
        .allowlist_type("^buf$")
        .allowlist_type("^bio$|bio_vec")
        .allowlist_type("blkdev_t|blkdev|blkdev_ops_t")
        // Phase 2 Wave 24 (dev/nullrand.c + dev/netdev.c port): the
        // network device registration/lookup table's ABI, shared with
        // still-C `kernel/e1000.c`/`kernel/dev/x1_emac.c` (registrants)
        // and `kernel/net.c` (consumer).
        .allowlist_type("netdev|netdev_ops|netdev_link_cb_t")
        // Phase 2 Wave 25 (dev/{yt8531,x1_emac,x1_sdhci}.c port): the
        // MDIO link-state struct shared between yt8531.rs/x1_emac.rs, and
        // the hardware DMA descriptor ring layout (x1_emac.h) -- see
        // wrapper.h's matching comment.
        .allowlist_type("phy_state|x1_rx_desc|x1_tx_desc")
        // Phase 2 Wave 26 (exec.c port): the ELF64 file/program header
        // structs `kernel/exec.rs` reads directly -- see wrapper.h's
        // matching comment. `ELF_MAGIC`/`ELF_PROG_LOAD`/`ELF_PROG_FLAG_*`
        // are hand-copied local consts in exec.rs, not bindgen vars (same
        // convention as every other small-integer-macro header).
        .allowlist_type("elfhdr|proghdr")
        // Phase 2 Wave 28 (virtio_disk.c/ramdisk.c, then pci.c/e1000.c/
        // net.c/sysnet.c port -- the final porting wave). `virtq_desc`/
        // `virtq_avail`/`virtq_used`/`virtq_used_elem`/`virtio_blk_req`
        // (dev/virtio.h) are the virtio-mmio virtqueue's hardware-defined
        // DMA layout -- the qemu device reads/writes these directly, so
        // real bindgen layouts (not hand-rolled `#[repr(C)]` guesses) are
        // required, same rationale as Wave 25's `x1_rx_desc`/`x1_tx_desc`.
        // `pci_common_confspace_header` (dev/pci.h) is the PCI-E
        // Configuration Space Header the qemu ECAM region exposes --
        // likewise hardware ABI. `tx_desc`/`rx_desc` (dev/e1000_dev.h,
        // anchored to avoid matching the unrelated `x1_tx_desc`/
        // `x1_rx_desc`) are the e1000's DMA descriptor ring layout.
        .allowlist_type("virtq_desc|virtq_avail|virtq_used|virtq_used_elem|virtio_blk_req")
        .allowlist_type("pci_common_confspace_header")
        .allowlist_type("^tx_desc$|^rx_desc$")
        .allowlist_var("PROT_.*|VMA_FLAG_.*|PTE_.*|PGSIZE|PGSHIFT|PAGE_SHIFT|MAXVA|UVMTOP|UVMBOTTOM|TRAPFRAME|TRAPFRAME_POFFSET|NCPU")
        .allowlist_var("MAP_.*|MREMAP_.*|MS_ASYNC|MS_SYNC|MS_INVALIDATE|MADV_.*")
        .allowlist_var("MAXUSTACK|USTACKTOP|USTACK_MAX_BOTTOM|USERSTACK_GROWTH|USERSTACK_MINSZ|UHEAP_MAX_TOP|PROT_MASK")
        .allowlist_var("SLAB_FLAG_.*")
        .allowlist_var("RWLOCK_PRIO_.*")
        .allowlist_var("E[A-Z]+")
        .allowlist_var("N_VIRTIO|EMAC_MAX|SDHCI_MAX|PCIE_REG_.*")
        .allowlist_var("PAGE_TYPE_.*")
        .allowlist_var("platform")
        // Phase 2 Wave 4 (console.c port): OPOST/ONLCR termios output
        // flags read by consolewrite.
        .allowlist_var("OPOST|ONLCR")
        .allowlist_function("slab_alloc|slab_free|slab_cache_init")
        .allowlist_function("rcu_read_lock|rcu_read_unlock|synchronize_rcu|call_rcu")
        .allowlist_function("hlist_init|hlist_get|hlist_get_rcu|hlist_put_rcu|hlist_pop_rcu|hlist_hash_int")
        .allowlist_function("rwlock_init|rwlock_wlock|rwlock_wunlock|rwlock_rlock|rwlock_runlock|rwlock_try_update")
        .allowlist_function("printf|panic|safestrcpy|memset|memcpy")
        .allowlist_function("rb_node_init|rb_root_init|rb_insert_color|rb_erase|rb_first|rb_next|rb_prev|rb_last|rb_find_key_rdown|rb_delete_node_color")
        .allowlist_function("list_entry_init|list_node_push_back|list_node_push_front|list_node_detach")
        .allowlist_function("page_ref_dec|page_ref_inc|__page_ref_dec|page_ref_dec_unlocked")
        .allowlist_function("vfs_fput|vfs_fdup|vfs_fdtable_get_file|vfs_inode_deref")
        .allowlist_function("walkaddr|mappages")
        .allowlist_function("spin_lock|spin_unlock")
        .allowlist_function("spin_init|rwsem_init")
        .allowlist_function("rwsem_acquire_read|rwsem_acquire_write|rwsem_release|rwsem_is_write_holding")
        .allowlist_function("smp_load_acquire_u64|smp_store_release_u64")
        .allowlist_function("tq_init|tq_set_lock|tq_size|tq_push|tq_remove|tq_bulk_move|tq_wait|tq_wait_cb|tq_wakeup|tq_wakeup_all|tnode_init")
        .allowlist_function("signal_pending")
        .allowlist_function("sched_timer_set|sched_timer_done|scheduler_sleep|sleep_ms")
        .blocklist_function("__assert.*");

    // ------------------------------------------------------------------
    // P3-N1 nativization: the intrusive-node type families below are
    // hand-written `#[repr(C)]` Rust types now (their owning modules
    // carry compile-time layout asserts against the C layout). Blocklist
    // the bindgen-generated definitions and redirect every remaining
    // bindgen reference (embedding structs, extern fn signatures) to the
    // native definitions via `raw_line` aliases. The C headers stay
    // unchanged for the remaining C consumers.
    //
    // Known side effect (bindgen limitation, verified harmless): a C
    // union whose member is *directly* one of these blocklisted types
    // (`tnode`'s list/tree union, `sched_entity`'s rb_entry/list_entry
    // union) is emitted in the `__BindgenUnionField` + `bindgen_union_field`
    // blob representation instead of a native Rust `union`. The blob is
    // sized/aligned from clang's real C layout, so the byte layout of the
    // embedding structs is unchanged; no Rust code accesses those union
    // fields (unions whose members merely *contain* these types, e.g.
    // `page_struct`'s, stay native thanks to `NativeTypeCallbacks`).
    builder = builder
        .parse_callbacks(Box::new(NativeTypeCallbacks))
        // kernel/inc/list_type.h `struct list_node` -> kernel/list.rs
        .blocklist_type("list_node|list_node_t")
        .raw_line("pub type list_node = crate::list::ListNode;")
        .raw_line("pub type list_node_t = crate::list::ListNode;")
        // kernel/inc/hlist_type.h hash-list family -> kernel/hlist.rs
        // (`hlist_bucket_t` is literally `struct list_node` in C, so it
        // redirects to the same native list node type).
        .blocklist_type("hlist_bucket_t|hlist_entry|hlist_entry_t|hlist_func_struct|hlist_func_t|hlist_struct|hlist_t")
        .raw_line("pub type hlist_bucket_t = crate::list::ListNode;")
        .raw_line("pub type hlist_entry = crate::hlist::RawHlistEntry;")
        .raw_line("pub type hlist_entry_t = crate::hlist::RawHlistEntry;")
        .raw_line("pub type hlist_func_struct = crate::hlist::RawHlistFunc;")
        .raw_line("pub type hlist_func_t = crate::hlist::RawHlistFunc;")
        .raw_line("pub type hlist_struct = crate::hlist::RawHlist;")
        .raw_line("pub type hlist_t = crate::hlist::RawHlist;")
        // kernel/inc/bintree_type.h red-black tree family -> kernel/bintree.rs
        .blocklist_type("rb_node|rb_root|rb_root_opts")
        .raw_line("pub type rb_node = crate::bintree::RawRbNode;")
        .raw_line("pub type rb_root = crate::bintree::RawRbRoot;")
        .raw_line("pub type rb_root_opts = crate::bintree::RawRbRootOpts;");

    // ------------------------------------------------------------------
    // P3-N2 nativization: the lock + thread-queue type family. Same
    // blocklist+redirect technique as P3-N1 above, with one twist: these
    // natives live in *private* top-level modules (`mod lock`, `mod
    // proc`), so a plain `pub type` alias would leave their effective
    // visibility crate-capped and trip the `private_interfaces` lint on
    // every generated `pub fn` signature that mentions them. `pub use`
    // re-exports (the standard facade pattern) lift the items to public
    // effective visibility instead, which is also exactly what a
    // bindgen-generated definition would have had.
    builder = builder
        // kernel/inc/lock/spinlock.h `struct spinlock` ->
        // kernel/lock/spinlock.rs. NOTE: the C `__ALIGNED_CACHELINE`
        // rides the *typedef*, not the struct; the record type is 24/8
        // (see spinlock.rs's layout proof) and embedding structs carry
        // their own repr(align(64)), exactly as bindgen emitted them.
        .blocklist_type("spinlock|spinlock_t")
        .raw_line("pub use crate::lock::spinlock::RawSpinlock as spinlock;")
        .raw_line("pub use crate::lock::spinlock::RawSpinlock as spinlock_t;")
        // kernel/inc/lock/rwlock_types.h `struct rwlock` (no typedef) ->
        // kernel/lock/rwlock.rs. Plain-field twin (`Rwlock`), NOT the
        // atomic-view `RawRwlock`: the C `_Atomic` members lowered to
        // plain u64/c_int in bindgen and `proc_shims.rs` constructs the
        // type with a plain field literal.
        .blocklist_type("rwlock")
        .raw_line("pub use crate::lock::rwlock::Rwlock as rwlock;")
        // kernel/inc/proc/tq_type.h thread-queue family ->
        // kernel/proc/thread_queue.rs. `tnode`'s anonymous union was
        // emitted in the degraded `__BindgenUnionField` blob form since
        // P3-N1 (its arms directly embed the blocklisted
        // `list_node_t`/`rb_node`); the native `Tnode` carries the real
        // Rust union, proven blob-identical (40/8) by thread_queue.rs's
        // layout gate + the cross-compiler probe. `tq_type_t` is the
        // constified-enum typedef; nothing on the Rust side reads its
        // variants, so a bare c_uint alias (bindgen's own lowering)
        // fully replaces it.
        // (`tnode__bindgen_ty_1.*` covers the anonymous union + arm
        // shells bindgen would otherwise still emit as orphans after
        // `tnode` itself is blocklisted.)
        .blocklist_type("tq|tq_t|ttree|ttree_t|tnode|tnode_t|tnode__bindgen_ty_1.*|tq_type_t")
        .raw_line("pub use crate::proc::thread_queue::Tq as tq;")
        .raw_line("pub use crate::proc::thread_queue::Tq as tq_t;")
        .raw_line("pub use crate::proc::thread_queue::Ttree as ttree;")
        .raw_line("pub use crate::proc::thread_queue::Ttree as ttree_t;")
        .raw_line("pub use crate::proc::thread_queue::Tnode as tnode;")
        .raw_line("pub use crate::proc::thread_queue::Tnode as tnode_t;")
        .raw_line("pub type tq_type_t = ::core::ffi::c_uint;")
        // The tq_t-embedding sleeping locks ->
        // kernel/lock/{mutex,rwsem,semaphore,completion}.rs. NOTE: the
        // C `sem_t` typedef (semaphore_types.h) never appeared in the
        // bindgen output (unreferenced by any allowlisted item), so
        // only `semaphore` is redirected — surface kept identical.
        // `completion_t` is an anonymous-struct typedef; that is the
        // type's only name.
        .blocklist_type("mutex|mutex_t|rwsem|rwsem_t|semaphore|sem_t|completion_t")
        .raw_line("pub use crate::lock::mutex::RawMutex as mutex;")
        .raw_line("pub use crate::lock::mutex::RawMutex as mutex_t;")
        .raw_line("pub use crate::lock::rwsem::RawRwsem as rwsem;")
        .raw_line("pub use crate::lock::rwsem::RawRwsem as rwsem_t;")
        .raw_line("pub use crate::lock::semaphore::RawSemaphore as semaphore;")
        .raw_line("pub use crate::lock::completion::RawCompletion as completion_t;");

    // ------------------------------------------------------------------
    // P3-N3 nativization: the kobject + workqueue + process-object type
    // families. Same blocklist + `pub use` re-export technique as P3-N2
    // above (the facade `pub use` lifts natives out of the private
    // `proc`/`tty` modules to public effective visibility; `kobject` is
    // a `pub mod`, but the uniform spelling is kept for symmetry).
    builder = builder
        // kernel/inc/kobject.h `struct kobject`/`struct kobject_ops` ->
        // kernel/kobject.rs. No typedef aliases exist for either.
        .blocklist_type("kobject|kobject_ops")
        .raw_line("pub use crate::kobject::Kobject as kobject;")
        .raw_line("pub use crate::kobject::KobjectOps as kobject_ops;")
        // kernel/inc/proc/workqueue_types.h workqueue family ->
        // kernel/proc/workqueue.rs. No typedef aliases exist for the
        // three structs; the `workqueue_lifecycle_cb_t`/
        // `workqueue_thread_lifecycle_cb_t` fn-pointer typedefs stay
        // bindgen-emitted (their `*mut workqueue` parameter resolves to
        // the native via the re-export). `workqueue__bindgen_ty_1` is
        // the anonymous-bitfield-struct shell bindgen would otherwise
        // still emit as an orphan; the native `WorkqueueFlagBits`
        // replaces it (nothing outside `workqueue` ever named it).
        .blocklist_type("work_struct|workqueue|workqueue_callbacks|workqueue__bindgen_ty_1")
        .raw_line("pub use crate::proc::workqueue::WorkStruct as work_struct;")
        .raw_line("pub use crate::proc::workqueue::Workqueue as workqueue;")
        .raw_line("pub use crate::proc::workqueue::WorkqueueCallbacks as workqueue_callbacks;")
        // kernel/inc/proc/pgroup_types.h `struct pgroup` (no typedef) ->
        // kernel/proc/pgroup.rs. `pgroup__bindgen_ty_1` is the
        // anonymous-bitfield-struct shell (native: `PgroupFlagBits`).
        .blocklist_type("pgroup|pgroup__bindgen_ty_1")
        .raw_line("pub use crate::proc::pgroup::Pgroup as pgroup;")
        // kernel/inc/proc/thread_group_types.h `struct thread_group`
        // (no typedef) -> kernel/proc/thread_group.rs. NOTE:
        // `tg_shared_pending` deliberately stays bindgen-emitted (it is
        // signal-family, out of this wave's scope); the native
        // `ThreadGroup` embeds it by value via its `crate::bindings`
        // path. `thread_group__bindgen_ty_1` is the
        // anonymous-bitfield-struct shell (native: `ThreadGroupFlagBits`).
        .blocklist_type("thread_group|thread_group__bindgen_ty_1")
        .raw_line("pub use crate::proc::thread_group::ThreadGroup as thread_group;")
        // kernel/inc/tty/session_types.h `struct session` (no typedef)
        // -> kernel/tty/session.rs (already a `pub mod`).
        // `session__bindgen_ty_1` is the anonymous-bitfield-struct shell
        // (native: `SessionFlagBits`).
        .blocklist_type("session|session__bindgen_ty_1")
        .raw_line("pub use crate::tty::session::Session as session;");

    let bindings = builder
        .generate()
        .expect("bindgen failed to generate kernel bindings");

    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap()).join("kernel_bindings.rs");
    bindings
        .write_to_file(&out_path)
        .expect("failed to write kernel_bindings.rs");

    // Post-process: bindgen emits empty anonymous structs (`*_bindgen_ty_N {}`)
    // for the kernel's `__STRUCT_CACHELINE_PADDING` macro, which expands to
    // `struct {} __attribute__((aligned(CACHELINE_SIZE)))`. bindgen drops the
    // alignment attribute, so the resulting Rust struct has alignment 1 instead
    // of 64. This corrupts the offsets of all subsequent fields in any parent
    // struct that uses the macro (e.g. `struct thread`, `struct vm`).
    //
    // Fix it by adding `#[repr(align(64))]` to every empty `_bindgen_ty_N`
    // struct so its alignment matches the C definition.
    {
        let contents = std::fs::read_to_string(&out_path)
            .expect("failed to read generated kernel_bindings.rs");
        let mut out = String::with_capacity(contents.len() + 4096);

        // Empty anonymous structs that are NOT `__STRUCT_CACHELINE_PADDING`
        // and must NOT receive `#[repr(align(64))]`. (E.g. the `anon`
        // variant of `page_struct`'s data union is just a placeholder
        // marker; forcing align 64 silently shifts the union's offset
        // and corrupts every embedding struct.)
        let no_align_empty: &[&str] = &["page_struct__bindgen_ty_2__bindgen_ty_1"];

        for line in contents.split_inclusive('\n') {
            let trimmed = line.trim_start();
            // Match e.g. `pub struct foo__bindgen_ty_3 {}` (empty body on same line).
            if trimmed.starts_with("pub struct ")
                && trimmed.contains("_bindgen_ty_")
                && trimmed.trim_end().ends_with("{}")
            {
                let after = &trimmed["pub struct ".len()..];
                let name_end = after
                    .find(|c: char| !c.is_ascii_alphanumeric() && c != '_')
                    .unwrap_or(after.len());
                let name = &after[..name_end];
                if !no_align_empty.contains(&name) {
                    let indent_len = line.len() - trimmed.len();
                    out.push_str(&line[..indent_len]);
                    out.push_str("#[repr(align(64))]\n");
                }
            }
            out.push_str(line);
        }
        std::fs::write(&out_path, out)
            .expect("failed to write post-processed kernel_bindings.rs");
    }
}
