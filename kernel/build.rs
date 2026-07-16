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
            // kernel/proc/signal.rs (P3-N4). All derived Copy/Clone in
            // the pre-nativization bindgen output; `sigpending`/
            // `sigpending_t` is still embedded by value by the remaining
            // bindgen `thread_signal` (`sig_pending: [sigpending_t; 32]`),
            // so an accurate Yes is what keeps *its* derive line
            // unchanged.
            "sigaction",
            "sigacts",
            "sigacts_t",
            "sigpending",
            "sigpending_t",
            "ksiginfo",
            "tg_shared_pending",
            // kernel/dev/{dev,cdev,blkdev}.rs (P3-N4). The ops tables +
            // `device_major` derived Copy/Clone in the pre-nativization
            // bindgen output; the instance structs (`device_instance`/
            // `cdev`/`blkdev`) derived neither — see NONCOPY below.
            "device_major",
            "device_major_t",
            "device_ops",
            "device_ops_t",
            "cdev_ops",
            "cdev_ops_t",
            "blkdev_ops",
            "blkdev_ops_t",
            // kernel/dev/bio.rs + kernel/bufcache.rs (P3-N4). Both
            // derived Copy/Clone in the pre-nativization bindgen output
            // (`bio` itself did not — see NONCOPY below).
            "bio_vec",
            "buf",
            // kernel/dev/netdev.rs (P3-N4). Derived Copy/Clone in the
            // pre-nativization bindgen output.
            "netdev",
            // kernel/vfs/{inode,pipe,fdtable,fs}.rs (P3-N5, small
            // sub-family). All derived Copy/Clone in the
            // pre-nativization bindgen output; `vfs_dir_iter` is still
            // embedded by value by the (for now) bindgen `vfs_file`'s
            // position union, so an accurate Yes is what keeps *its*
            // derive line unchanged.
            "vfs_dentry",
            "vfs_dir_iter",
            "pipe",
            "vfs_fdtable",
            "fs_struct",
            // kernel/vfs/file.rs (P3-N5, file sub-family — nativized in
            // the same step as the small sub-family above because
            // `vfs_file`'s anonymous position union directly embeds the
            // now-blocklisted `vfs_dir_iter`/`pipe`, which would
            // otherwise degrade it to `__BindgenUnionField` blobs, N1's
            // documented bindgen limitation / N2's `tnode` precedent).
            // Both derived Copy/Clone in the pre-nativization bindgen
            // output.
            "vfs_file",
            "vfs_file_ops",
            // kernel/vfs/fs.rs (P3-N5, fs_type + superblock
            // sub-family). The two ops tables derived Copy/Clone in the
            // pre-nativization bindgen output; `vfs_fs_type` and
            // `vfs_superblock` derived neither — see NONCOPY below.
            "vfs_fs_type_ops",
            "vfs_superblock_ops",
            // kernel/vfs/inode.rs (P3-N5, inode hub sub-family).
            // Derived Copy/Clone in the pre-nativization bindgen
            // output (`vfs_inode` itself did not — see NONCOPY below).
            "vfs_inode_ops",
            // kernel/mm/slab.rs (P3-N6, slab sub-family). All three
            // derived Copy/Clone in the pre-nativization bindgen
            // output; `slab_cache_t` is still embedded BY VALUE by the
            // remaining bindgen `xv6fs_block_cache` (`extent_cache`),
            // so an accurate Yes here is what keeps *its* (absent)
            // derive line unchanged.
            "percpu_slab_cache_t",
            "slab_cache_struct",
            "slab_cache_t",
            "slab_struct",
            "slab_t",
            // kernel/dev/fdt.rs (P3-N6, allocator-POD slice). Derived
            // Copy/Clone in the pre-nativization bindgen output; the
            // still-bindgen `platform_info` embeds `[mem_region; 8]` BY
            // VALUE and derives Copy/Clone itself — the accurate Yes
            // here is what keeps *its* derive line unchanged. Listed in
            // BOTH bare and tag-prefixed forms: `mem_region` has no
            // typedef, and bindgen asks about non-typedef'd records as
            // `"struct X"` (see the P3-N6 FINDING note below the lists).
            "mem_region",
            "struct mem_region",
            // kernel/mm/pcache.rs (P3-N6, pcache sub-family). The ops
            // table derived Copy/Clone in the pre-nativization bindgen
            // output (`pcache`/`pcache_node` derived neither — see
            // NONCOPY below). Tag-prefixed form included (no typedef).
            "pcache_ops",
            "struct pcache_ops",
            // kernel/mm/page.rs (P3-N6, page sub-family — the wave's
            // gnarly closer). Derived Copy/Clone in the pre-nativization
            // bindgen output (as did all five anonymous-union arm
            // shells). Nothing in the remaining bindgen output embeds
            // `page_t` by value (verified — all uses are pointers).
            // Tag-prefixed form included alongside the typedef name.
            "page_struct",
            "struct page_struct",
            "page_t",
            // kernel/lock/rcu.rs (P3-N7, rcu sub-family). Derived
            // Copy/Clone in the pre-nativization bindgen output; the
            // `thread` hub (still-bindgen until P3-N9, native since)
            // embeds `rcu_head_t` BY VALUE (`rcu_head`, its last field)
            // and derives Copy/Clone itself — the accurate Yes here is
            // what keeps *its* derive line unchanged. Listed in bare, `_t`, and tag-prefixed
            // forms (the P3-N6 FINDING: bindgen queries non-typedef'd
            // field spellings as `"struct X"`).
            "rcu_head",
            "rcu_head_t",
            "struct rcu_head",
            // kernel/proc/rq.rs + kernel/proc/sched.rs (P3-N7, sched
            // sub-family PODs/ops tables). All derived Copy/Clone in
            // the pre-nativization bindgen output (plain integers,
            // pointers, and fn-pointer tables; `rq_percpu`'s embedded
            // `spinlock_t rq_lock` is typedef-spelled, so its Copy
            // query already matched the P3-N2 `spinlock_t` entry).
            // Nothing in the remaining bindgen output embeds any of
            // them by value (verified). Tag-prefixed forms included
            // (none has a typedef).
            "load_weight",
            "struct load_weight",
            "sched_attr",
            "struct sched_attr",
            "sched_class",
            "struct sched_class",
            "rq",
            "struct rq",
            "rq_percpu",
            "struct rq_percpu",
            // kernel/vfs/inode.rs (P3-N8, vfs_inode_ref). Derived
            // Copy/Clone in the pre-nativization bindgen output (two
            // raw pointers); the NATIVE `vfs_file`/`fs_struct` embed it
            // by value and derive Copy themselves, which requires the
            // real impl on `VfsInodeRef` (nothing still-bindgen embeds
            // it — verified). Tag-prefixed form included (no typedef).
            "vfs_inode_ref",
            "struct vfs_inode_ref",
            // kernel/tty/tty.rs (P3-N8, tty pair). Both derived
            // Copy/Clone in the pre-nativization bindgen output (the
            // embedded `spinlock_t`/`tq_t` are typedef-spelled and
            // already answered Yes via the P3-N2 entries;
            // `termios`/`winsize` stay bindgen-emitted uabi types).
            // Nothing in the remaining bindgen output embeds either by
            // value (verified). Tag-prefixed forms included (no
            // typedefs).
            "tty",
            "struct tty",
            "tty_ops",
            "struct tty_ops",
            // kernel/vfs/tmpfs/{superblock,inode}.rs (P3-N8, tmpfs
            // POD slices). `tmpfs_sb_private` and the dir arm of
            // `tmpfs_inode`'s anonymous union derived Copy/Clone in the
            // pre-nativization bindgen output. Nothing in the remaining
            // bindgen output embeds them by value (verified — the only
            // by-value embedders, `tmpfs_superblock`/`tmpfs_inode`, are
            // native in the same wave). Tag-prefixed forms included (no
            // typedefs).
            "tmpfs_sb_private",
            "struct tmpfs_sb_private",
            // kernel/vfs/xv6fs/log.rs (P3-N8). Both derived Copy/Clone
            // in the pre-nativization bindgen output (`xv6fs_logheader`
            // is the ON-DISK log-header record, memcpy'd into buffer
            // cache blocks; `xv6fs_log`'s embedded `spinlock_t`/`tq_t`
            // answered Yes via the P3-N2 entries). Nothing still-bindgen
            // embeds them by value (their only embedder,
            // `xv6fs_superblock`, is native in the same wave).
            // Tag-prefixed forms included (no typedefs).
            "xv6fs_logheader",
            "struct xv6fs_logheader",
            "xv6fs_log",
            "struct xv6fs_log",
            // kernel/proc/thread.rs + kernel/proc/signal.rs (P3-N9, the
            // thread family hub). Both derived Copy/Clone in the
            // pre-nativization bindgen output (`thread`'s embedded
            // `spinlock_t`/`list_node_t`/`hlist_entry_t`/`rcu_head_t`/
            // `sigpending_t` are typedef-spelled natives that already
            // answered Yes via their own entries; `thread_signal`'s
            // `sig_stack` embeds the still-bindgen uabi `stack_t`, which
            // bindgen derives Copy for natively). Nothing in the
            // remaining bindgen output embeds either by value (verified:
            // the only by-value embedder of `thread_signal` was `thread`
            // itself, native in the same wave; all remaining `thread`
            // uses are pointers). Tag-prefixed + typedef forms included.
            "thread",
            "struct thread",
            "thread_signal",
            "struct thread_signal",
            "thread_signal_t",
            // kernel/irq/trap.rs + kernel/proc/sched.rs + kernel/ipi.rs
            // (P3-5, the asm-offset-locked trio). All four derived
            // Copy/Clone in the pre-nativization bindgen output (plain
            // scalar/pointer fields throughout). Nothing still-bindgen
            // embeds any of them by value (verified: the only by-value
            // embedder of `trapframe` was `utrapframe`, native in the
            // same wave; `sched_entity.context` is native since P3-N7;
            // every remaining use is a pointer). Tag-prefixed forms
            // included (no typedefs exist for any of the four).
            "trapframe",
            "struct trapframe",
            "utrapframe",
            "struct utrapframe",
            "context",
            "struct context",
            "cpu_local",
            "struct cpu_local",
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
            // kernel/dev/{dev,cdev,blkdev}.rs (P3-N4): the instance
            // structs derived neither Copy nor Clone in the
            // pre-nativization bindgen output (kobject-embedder derive
            // pattern, same as the still-bindgen `vfs_fs_type`); the
            // natives faithfully have no derives. Nothing in the
            // remaining bindgen output embeds them by value (verified),
            // but an accurate No keeps any future embedder honest.
            "device_instance",
            "device_t",
            "cdev",
            "cdev_t",
            "blkdev",
            "blkdev_t",
            // kernel/dev/bio.rs (P3-N4): same kobject-embedder derive
            // pattern as `device_instance` above.
            "bio",
            // kernel/vfs/fs.rs (P3-N5): `vfs_fs_type` (kobject-embedder
            // class) and `vfs_superblock` derived neither Copy nor
            // Clone in the pre-nativization bindgen output; the natives
            // faithfully have no derives. The still-bindgen
            // `tmpfs_superblock`/`xv6fs_superblock` embed
            // `vfs_superblock` BY VALUE and derived neither themselves —
            // the accurate No here is what keeps their derive lines
            // unchanged.
            "vfs_fs_type",
            "vfs_superblock",
            // kernel/vfs/inode.rs (P3-N5): `vfs_inode` derived neither
            // Copy nor Clone in the pre-nativization bindgen output;
            // the native faithfully has no derives. The still-bindgen
            // `tmpfs_inode`/`xv6fs_inode` embed it BY VALUE (first
            // field) and derived neither themselves — the accurate No
            // here is what keeps their derive lines unchanged.
            "vfs_inode",
            // kernel/vfs/xv6fs/block_cache.rs (P3-N6): `free_extent`
            // derived neither Copy nor Clone in the pre-nativization
            // bindgen output (intrusive `rb_node` embedder class, N1
            // precedent); the native faithfully has no derives. Nothing
            // in the remaining bindgen output embeds it by value
            // (verified — `xv6fs_block_cache` reaches extents only
            // through the rb-tree/pointers). Tag-prefixed form included
            // (no typedef — see the P3-N6 FINDING note below).
            "free_extent",
            "struct free_extent",
            // kernel/mm/vm.rs (P3-N6): neither `vm` nor `vma` derived
            // Copy/Clone in the pre-nativization bindgen output (their
            // embedded `struct spinlock`/rb_node members answered No via
            // this callback's tag-prefix quirk — see the FINDING note
            // below); the natives faithfully have no derives. Nothing in
            // the remaining bindgen output embeds them by value
            // (verified — `thread.vm` is a pointer). Tag-prefixed forms
            // included alongside the `_t` typedef names.
            "vm",
            "vm_t",
            "struct vm",
            "vma",
            "vma_t",
            "struct vma",
            // kernel/mm/pcache.rs (P3-N6): neither `pcache` nor
            // `pcache_node` derived Copy/Clone in the pre-nativization
            // bindgen output; the natives faithfully have no derives.
            // The NATIVE `VfsInode` embeds `pcache` BY VALUE (`i_data`)
            // and has no derives either; nothing in the REMAINING
            // bindgen output embeds either by value (verified).
            // Tag-prefixed forms included (no typedefs).
            "pcache",
            "struct pcache",
            "pcache_node",
            "struct pcache_node",
            // kernel/timer/timer_core.rs + kernel/proc/rq.rs (P3-N7):
            // `timer_node`/`timer_root`/`sched_entity` derived neither
            // Copy nor Clone in the pre-nativization bindgen output.
            // P3-N6 flagged these three as "silently lost Copy/Clone to
            // the struct-X quirk when P3-N2 nativized spinlock"; the
            // P3-N7 first-principles decision is that no-derive is also
            // the *correct* form, not just the boot-verified one: all
            // three are intrusive structures (embedded rb_node/
            // list_node links, an owned spinlock, `sched_entity`'s
            // per-thread `context` save area) whose bitwise duplication
            // would corrupt list/tree invariants — the N1/N2 `tnode`
            // NONCOPY precedent — and no consumer `=`-copies or
            // literal-constructs any of them (grep-verified; the only
            // by-value uses are embeds in native structs plus
            // `mem::zeroed()` init, neither of which needs Copy). The
            // natives faithfully have no derives. Nothing in the
            // remaining bindgen output embeds them by value (verified).
            // Tag-prefixed forms included (none has a typedef).
            "sched_entity",
            "struct sched_entity",
            "timer_node",
            "struct timer_node",
            "timer_root",
            "struct timer_root",
            // kernel/sysnet.rs (P3-N8): `struct sock` was only ever
            // forward-declared to C (bindgen emitted an opaque
            // `_unused: [u8; 0]` shell that happened to carry
            // Copy/Clone); the canonical definition has always been the
            // native `sysnet::sock`, which is an intrusive list node
            // owning an mbuf queue — NOT Copy. The accurate No keeps
            // any future embedder honest (nothing embeds it by value —
            // all uses are pointers, verified).
            "sock",
            "struct sock",
            // kernel/vfs/tmpfs/{superblock,inode}.rs (P3-N8): none of
            // the three derived Copy/Clone in the pre-nativization
            // bindgen output (each embeds the NONCOPY `vfs_superblock`/
            // `vfs_inode`/`hlist_entry_t`-headed record by value, and
            // `tmpfs_inode`'s union shell degraded to
            // `__BindgenUnionField` form). The natives faithfully have
            // no derives. Nothing still-bindgen embeds them by value
            // (verified). Tag-prefixed forms included (no typedefs).
            "tmpfs_superblock",
            "struct tmpfs_superblock",
            "tmpfs_inode",
            "struct tmpfs_inode",
            "tmpfs_dentry",
            "struct tmpfs_dentry",
            // kernel/vfs/xv6fs/{superblock,inode,block_cache}.rs
            // (P3-N8): none derived Copy/Clone in the pre-nativization
            // bindgen output. `xv6fs_block_cache` is one of the N6
            // flagged "silently lost Copy/Clone to the struct-X quirk"
            // survivors; the P3-N8 first-principles decision is that
            // no-derive is also the *correct* form, not just the
            // boot-verified one: it owns a live rb-tree of free
            // extents, an embedded slab cache, and a spinlock — bitwise
            // duplication would corrupt the extent tree and double-own
            // the allocator (the N1/N2 `tnode` NONCOPY precedent) — and
            // no consumer `=`-copies or literal-constructs it
            // (grep-verified; the only by-value use is the embed in the
            // native `xv6fs_superblock` plus zeroed init). The natives
            // faithfully have no derives. Tag-prefixed forms included
            // (no typedefs).
            "xv6fs_superblock",
            "struct xv6fs_superblock",
            "xv6fs_inode",
            "struct xv6fs_inode",
            "xv6fs_block_cache",
            "struct xv6fs_block_cache",
        ];
        // P3-N6 FINDING — bindgen asks this callback about typedef'd
        // types by their bare typedef name (`slab_cache_t`) but about
        // non-typedef'd records with the tag prefix (`struct
        // mem_region`). Observed empirically when `platform_info`
        // silently lost its Copy/Clone derive: `"struct mem_region"`
        // matched nothing in the lists above. The fix is the explicit
        // `"struct X"` entries above rather than stripping the prefix
        // here: a blanket strip was tried and RESURRECTED Copy/Clone on
        // nine remaining bindgen types (`pcache`/`pcache_node`/`vm`/
        // `vma`/`xv6fs_block_cache`/`sched_entity`(+ty_1)/`timer_node`/
        // `timer_root`) that had silently lost their derives to this
        // same quirk when `struct spinlock`/`struct rwlock`/... were
        // nativized in P3-N2 — the established (and boot-verified)
        // emission is the no-derive form, and a nativization wave must
        // not change remaining types' derive lines as a side effect.
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

    // ------------------------------------------------------------------
    // P3-N4 nativization: the signal + device type families. Same
    // blocklist + `pub use` re-export technique as P3-N2/N3 above.
    builder = builder
        // kernel/inc/uabi/signal.h `struct sigaction` +
        // kernel/inc/signal_types.h `struct sigacts`/`struct sigpending`/
        // `struct ksiginfo` + kernel/inc/proc/thread_group_types.h
        // `struct tg_shared_pending` -> kernel/proc/signal.rs.
        // `sigaction__bindgen_ty_1` is the anonymous-union shell bindgen
        // would otherwise still emit as an orphan (native: the real Rust
        // union `SigActionHandler`, N2's `tnode` precedent). The
        // `sigacts_t`/`sigpending_t` typedefs are blocklisted alongside
        // and re-exported under both names; no `sigaction_t`/
        // `ksiginfo_t`/`tg_shared_pending` typedefs ever appeared in the
        // bindgen output (consumers alias in their `use` items). NOTE:
        // `siginfo`/`sigval`/`stack` deliberately stay bindgen-emitted
        // (uabi class, kernel/inc/uabi/signal.h — P3-4 scrutiny; the
        // P3-N9 determination); the native `KsigInfo` embeds `siginfo_t`
        // *by value* via its `crate::bindings` path — the sanctioned
        // mixed-tier pattern. `thread_signal` stayed bindgen through
        // N4..N8 and nativized with its embedder in P3-N9 below.
        .blocklist_type("sigaction|sigaction__bindgen_ty_1|sigacts|sigacts_t|sigpending|sigpending_t|ksiginfo|tg_shared_pending")
        .raw_line("pub use crate::proc::signal::SigAction as sigaction;")
        .raw_line("pub use crate::proc::signal::SigActs as sigacts;")
        .raw_line("pub use crate::proc::signal::SigActs as sigacts_t;")
        .raw_line("pub use crate::proc::signal::SigPending as sigpending;")
        .raw_line("pub use crate::proc::signal::SigPending as sigpending_t;")
        .raw_line("pub use crate::proc::signal::KsigInfo as ksiginfo;")
        .raw_line("pub use crate::proc::signal::TgSharedPending as tg_shared_pending;")
        // kernel/inc/dev/dev_types.h device core family ->
        // kernel/dev/{dev,cdev,blkdev}.rs. Every struct has a `_t`
        // typedef; both names are re-exported. `dev_type_e` (constified
        // enum) stays bindgen-emitted. `cdev__bindgen_ty_1`/
        // `blkdev__bindgen_ty_1` are the anonymous-bitfield-struct
        // shells bindgen would otherwise still emit as orphans (natives:
        // `CdevFlagBits`/`BlkdevFlagBits`).
        .blocklist_type("device_major|device_major_t|device_ops|device_ops_t|device_instance|device_t")
        .raw_line("pub use crate::dev::dev::DeviceMajor as device_major;")
        .raw_line("pub use crate::dev::dev::DeviceMajor as device_major_t;")
        .raw_line("pub use crate::dev::dev::DeviceOps as device_ops;")
        .raw_line("pub use crate::dev::dev::DeviceOps as device_ops_t;")
        .raw_line("pub use crate::dev::dev::DeviceInstance as device_instance;")
        .raw_line("pub use crate::dev::dev::DeviceInstance as device_t;")
        .blocklist_type("cdev|cdev_t|cdev_ops|cdev_ops_t|cdev__bindgen_ty_1")
        .raw_line("pub use crate::dev::cdev::CdevOps as cdev_ops;")
        .raw_line("pub use crate::dev::cdev::CdevOps as cdev_ops_t;")
        .raw_line("pub use crate::dev::cdev::Cdev as cdev;")
        .raw_line("pub use crate::dev::cdev::Cdev as cdev_t;")
        .blocklist_type("blkdev|blkdev_t|blkdev_ops|blkdev_ops_t|blkdev__bindgen_ty_1")
        .raw_line("pub use crate::dev::blkdev::BlkdevOps as blkdev_ops;")
        .raw_line("pub use crate::dev::blkdev::BlkdevOps as blkdev_ops_t;")
        .raw_line("pub use crate::dev::blkdev::Blkdev as blkdev;")
        .raw_line("pub use crate::dev::blkdev::Blkdev as blkdev_t;")
        // kernel/inc/dev/bio_types.h `struct bio_vec`/`struct bio` (no
        // typedefs) -> kernel/dev/bio.rs. `bio__bindgen_ty_1` is the
        // anonymous-bitfield-struct shell (native: `BioFlagBits`). The
        // C flexible array member keeps bindgen's own zero-sized
        // `__IncompleteArrayField` helper (still bindgen-emitted).
        .blocklist_type("bio|bio_vec|bio__bindgen_ty_1")
        .raw_line("pub use crate::dev::bio::BioVec as bio_vec;")
        .raw_line("pub use crate::dev::bio::Bio as bio;")
        // kernel/inc/dev/buf.h `struct buf` (no typedef) ->
        // kernel/bufcache.rs.
        .blocklist_type("buf")
        .raw_line("pub use crate::bufcache::Buf as buf;")
        // kernel/inc/dev/netdev.h `struct netdev` (no typedef) ->
        // kernel/dev/netdev.rs. `netdev_ops` and the `netdev_link_cb_t`
        // fn-pointer typedef stay bindgen-emitted (pointer-only
        // references; their `*mut netdev` parameters resolve to the
        // native via the re-export — the bindgen blocklist regex is
        // anchored, so `netdev` does not match them).
        .blocklist_type("netdev")
        .raw_line("pub use crate::dev::netdev::Netdev as netdev;");

    // ------------------------------------------------------------------
    // P3-N5 nativization: the VFS type family. Same blocklist + `pub
    // use` re-export technique as P3-N2/N3/N4 above. None of these
    // structs has a `_t` typedef (kernel/inc/vfs/vfs_types.h declares
    // `cdev_t`/`blkdev_t` forward typedefs only).
    builder = builder
        // kernel/inc/vfs/vfs_types.h `struct vfs_dentry`/`struct
        // vfs_dir_iter` -> kernel/vfs/inode.rs (the inode/dentry
        // family's owning module).
        .blocklist_type("vfs_dentry|vfs_dir_iter")
        .raw_line("pub use crate::vfs::inode::VfsDentry as vfs_dentry;")
        .raw_line("pub use crate::vfs::inode::VfsDirIter as vfs_dir_iter;")
        // kernel/inc/vfs/pipe_types.h `struct pipe` -> kernel/vfs/
        // pipe.rs. NOTE: the native reproduces the BINDGEN-emitted
        // runtime layout (192/64, writer_lock @88), which diverges from
        // the C header's own layout (256/64, writer_lock @128) — see
        // pipe.rs's layout note; `struct pipe` has no C consumers and
        // both allocation and every access live in pipe.rs, so the
        // bindgen layout is the working truth being preserved.
        .blocklist_type("pipe")
        .raw_line("pub use crate::vfs::pipe::Pipe as pipe;")
        // kernel/inc/vfs/vfs_types.h `struct vfs_fdtable` ->
        // kernel/vfs/fdtable.rs.
        .blocklist_type("vfs_fdtable")
        .raw_line("pub use crate::vfs::fdtable::VfsFdtable as vfs_fdtable;")
        // kernel/inc/vfs/vfs_types.h `struct fs_struct` ->
        // kernel/vfs/fs.rs (owner of the `vfs_struct_*` lifecycle).
        // `vfs_inode_ref` (kernel/inc/types.h) deliberately stays
        // bindgen-emitted (out of this wave's scope); the native
        // `FsStruct` embeds it by value via its `crate::bindings` path —
        // the sanctioned mixed-tier pattern.
        .blocklist_type("fs_struct")
        .raw_line("pub use crate::vfs::fs::FsStruct as fs_struct;")
        // kernel/inc/vfs/vfs_types.h `struct vfs_file`/`struct
        // vfs_file_ops` -> kernel/vfs/file.rs. Nativized in the same
        // step as vfs_dir_iter/pipe: the anonymous position union
        // directly embeds both, so leaving `vfs_file` bindgen would
        // degrade it to the `__BindgenUnionField` blob form (N1's
        // documented limitation); the native carries the real Rust
        // union `VfsFilePos` instead (N2's `tnode` precedent).
        // `vfs_file__bindgen_ty_1` is the anonymous-union shell bindgen
        // would otherwise still emit as an orphan. `vfs_inode_ref`
        // stays bindgen-emitted (embedded by value via the
        // `crate::bindings` path — mixed-tier, same as `fs_struct`).
        .blocklist_type("vfs_file|vfs_file_ops|vfs_file__bindgen_ty_1")
        .raw_line("pub use crate::vfs::file::VfsFile as vfs_file;")
        .raw_line("pub use crate::vfs::file::VfsFileOps as vfs_file_ops;")
        // kernel/inc/vfs/vfs_types.h fs_type + superblock family ->
        // kernel/vfs/fs.rs. `vfs_fs_type__bindgen_ty_1`/
        // `vfs_superblock__bindgen_ty_2` are the anonymous-bitfield-
        // struct shells bindgen would otherwise still emit as orphans
        // (natives: `VfsFsTypeFlagBits`/`VfsSuperblockFlagBits`);
        // `vfs_superblock__bindgen_ty_1` is the anonymous inode-hash
        // struct shell (native: flattened into direct
        // `inodes`/`inodes_buckets` fields at identical offsets).
        // `statfs` (kernel/inc/uabi/statfs.h — uabi) deliberately stays
        // bindgen-emitted; the native ops table references it by
        // `crate::bindings` path.
        .blocklist_type("vfs_fs_type|vfs_fs_type_ops|vfs_fs_type__bindgen_ty_1")
        .raw_line("pub use crate::vfs::fs::VfsFsType as vfs_fs_type;")
        .raw_line("pub use crate::vfs::fs::VfsFsTypeOps as vfs_fs_type_ops;")
        .blocklist_type("vfs_superblock|vfs_superblock_ops|vfs_superblock__bindgen_ty_1|vfs_superblock__bindgen_ty_2")
        .raw_line("pub use crate::vfs::fs::VfsSuperblock as vfs_superblock;")
        .raw_line("pub use crate::vfs::fs::VfsSuperblockOps as vfs_superblock_ops;")
        // kernel/inc/vfs/vfs_types.h inode hub -> kernel/vfs/inode.rs.
        // `vfs_inode__bindgen_ty_1` is the anonymous-bitfield-struct
        // shell (native: `VfsInodeFlagBits`); `vfs_inode__bindgen_ty_2`
        // + `vfs_inode__bindgen_ty_2__bindgen_ty_1` are the anonymous
        // device/mount union + its nested-struct shell (natives: the
        // real Rust union `VfsInodeDevMnt` + `VfsInodeMnt`). `pcache`
        // (embedded BY VALUE as `i_data`), `stat` (uabi — P3-4 scrutiny
        // class) and `thread` stay bindgen-emitted, referenced by
        // `crate::bindings` paths — the sanctioned mixed-tier pattern.
        .blocklist_type("vfs_inode|vfs_inode_ops|vfs_inode__bindgen_ty_1|vfs_inode__bindgen_ty_2|vfs_inode__bindgen_ty_2__bindgen_ty_1")
        .raw_line("pub use crate::vfs::inode::VfsInode as vfs_inode;")
        .raw_line("pub use crate::vfs::inode::VfsInodeOps as vfs_inode_ops;");

    // ------------------------------------------------------------------
    // P3-N6 nativization: the mm type family. Same blocklist + `pub use`
    // re-export technique as P3-N2..N5 above. Layout evidence for the
    // whole family: temporary in-tree `offset_of!` gate on the live
    // bindgen forms + cross-compiler `_Static_assert` probe (toolchain
    // gcc, rv64gc/lp64d — scratchpad p3n6_static_assert_probe.c); the
    // two AGREE on every size/align/offset (no pipe-style divergence,
    // P3-N5 precedent checked).
    builder = builder
        // kernel/inc/mm/slab_type.h slab family -> kernel/mm/slab.rs
        // (that module's private `SlabCache`/`Slab`/`PercpuCache`
        // atomic-view mirrors stay, byte-identical and cross-asserted).
        .blocklist_type("percpu_slab_cache_t|slab_cache_struct|slab_cache_t|slab_struct|slab_t")
        .raw_line("pub use crate::mm::slab::PercpuSlabCache as percpu_slab_cache_t;")
        .raw_line("pub use crate::mm::slab::SlabCacheStruct as slab_cache_struct;")
        .raw_line("pub use crate::mm::slab::SlabCacheStruct as slab_cache_t;")
        .raw_line("pub use crate::mm::slab::SlabStruct as slab_struct;")
        .raw_line("pub use crate::mm::slab::SlabStruct as slab_t;")
        // kernel/vfs/xv6fs/block_cache.h `struct free_extent` ->
        // kernel/vfs/xv6fs/block_cache.rs (the extent cache's owning
        // module). `xv6fs_block_cache` itself stays bindgen (P3-N7+).
        .blocklist_type("free_extent")
        .raw_line("pub use crate::vfs::xv6fs::block_cache::FreeExtent as free_extent;")
        // kernel/inc/dev/fdt.h `struct mem_region` -> kernel/dev/fdt.rs.
        // A plain packed POD (not a DMA/hardware descriptor — P3-4's
        // scrutiny class is untouched); `platform_info` stays bindgen
        // and embeds the native by value via the facade.
        .blocklist_type("mem_region")
        .raw_line("pub use crate::dev::fdt::MemRegion as mem_region;")
        // kernel/inc/mm/vm_types.h vm family -> kernel/mm/vm.rs.
        .blocklist_type("vm|vm_t|vma|vma_t")
        .raw_line("pub use crate::mm::vm::Vm as vm;")
        .raw_line("pub use crate::mm::vm::Vm as vm_t;")
        .raw_line("pub use crate::mm::vm::Vma as vma;")
        .raw_line("pub use crate::mm::vm::Vma as vma_t;")
        // kernel/inc/mm/pcache_types.h pcache family -> kernel/mm/
        // pcache.rs. `pcache__bindgen_ty_1`/`..__bindgen_ty_1__bindgen_
        // ty_1`/`pcache_node__bindgen_ty_1` are the anonymous flags
        // union/bitfield shells bindgen would otherwise still emit as
        // orphans (natives: `PcacheFlags`/`PcacheFlagBits`/
        // `PcacheNodeFlagBits`).
        .blocklist_type("pcache|pcache_ops|pcache_node|pcache__bindgen_ty_1|pcache__bindgen_ty_1__bindgen_ty_1|pcache_node__bindgen_ty_1")
        .raw_line("pub use crate::mm::pcache::Pcache as pcache;")
        .raw_line("pub use crate::mm::pcache::PcacheOps as pcache_ops;")
        .raw_line("pub use crate::mm::pcache::PcacheNode as pcache_node;")
        // kernel/inc/mm/page_type.h page family -> kernel/mm/page.rs
        // (that module's private `Page` byte-accessor mirror stays,
        // byte-identical and cross-asserted). The six `__bindgen_ty`
        // shells are the anonymous flags-union (flattened to a direct
        // `flags` field in the native) and the five-arm per-type union
        // (native real Rust union `PageTypeData` + named arm structs).
        .blocklist_type("page_struct|page_t|page_struct__bindgen_ty_1|page_struct__bindgen_ty_2|page_struct__bindgen_ty_2__bindgen_ty_1|page_struct__bindgen_ty_2__bindgen_ty_2|page_struct__bindgen_ty_2__bindgen_ty_3|page_struct__bindgen_ty_2__bindgen_ty_4|page_struct__bindgen_ty_2__bindgen_ty_5")
        .raw_line("pub use crate::mm::page::PageStruct as page_struct;")
        .raw_line("pub use crate::mm::page::PageStruct as page_t;");

    // ------------------------------------------------------------------
    // P3-N7 nativization: the scheduler + timer + rcu type families.
    // Same blocklist + `pub use` re-export technique as P3-N2..N6 above.
    // Layout evidence for the whole family: temporary in-tree
    // `offset_of!` gate on the live bindgen forms + cross-compiler
    // `_Static_assert` probe (toolchain gcc, rv64gc/lp64d — scratchpad
    // p3n7_static_assert_probe.c); the two AGREE on every size/align/
    // offset (no pipe-style divergence, P3-N5 precedent checked —
    // notably gcc places the cacheline-aligned `spinlock_t` members of
    // `sched_entity`/`rq_percpu`/`timer_root` at exactly the offsets
    // bindgen emitted).
    builder = builder
        // kernel/inc/lock/rcu_type.h `struct rcu_head` (typedef
        // `rcu_head_t`) -> kernel/lock/rcu.rs (the module's P3-3A
        // internal mirror `RawRcuHead`, promoted to the canonical
        // definition). `rcu_head__bindgen_ty_1` is the anonymous
        // 1-bit-`embedded_head` bitfield shell bindgen would otherwise
        // still emit as an orphan (native: flattened `flags: u64`,
        // N5/N6 single-member-flags precedent). The `rcu_callback_t`
        // fn-pointer typedef stays bindgen-emitted.
        .blocklist_type("rcu_head|rcu_head_t|rcu_head__bindgen_ty_1")
        .raw_line("pub use crate::lock::rcu::RawRcuHead as rcu_head;")
        .raw_line("pub use crate::lock::rcu::RawRcuHead as rcu_head_t;")
        // kernel/inc/timer/timer_types.h `struct timer_root`/`struct
        // timer_node` (no typedefs) -> kernel/timer/timer_core.rs.
        // `timer_root__bindgen_ty_1` is the anonymous 1-bit-`valid`
        // bitfield shell bindgen would otherwise still emit as an
        // orphan (native: `TimerRootFlagBits`, N3 flag-bits precedent).
        .blocklist_type("timer_root|timer_node|timer_root__bindgen_ty_1")
        .raw_line("pub use crate::timer::timer_core::TimerRoot as timer_root;")
        .raw_line("pub use crate::timer::timer_core::TimerNode as timer_node;")
        // kernel/inc/proc/rq_types.h `struct sched_class`/`struct
        // sched_attr` (no typedefs) -> kernel/proc/sched.rs.
        // `SchedClass` keeps bindgen's `Option<unsafe extern "C" fn>`
        // ops-table form verbatim (trait-ification is P3-10);
        // kernel/proc/cffi.rs's layout-pinned mirror is promoted to a
        // re-export of the native.
        .blocklist_type("sched_class|sched_attr")
        .raw_line("pub use crate::proc::sched::SchedClass as sched_class;")
        .raw_line("pub use crate::proc::sched::SchedAttr as sched_attr;")
        // kernel/inc/proc/rq_types.h `struct load_weight` (no typedef)
        // -> kernel/proc/rq.rs. Zero field consumers (emitted by
        // allowlist only); nativized for family completeness.
        .blocklist_type("load_weight")
        .raw_line("pub use crate::proc::rq::LoadWeight as load_weight;")
        // kernel/inc/proc/rq_types.h `struct rq`/`struct rq_percpu`/
        // `struct sched_entity` (no typedefs) -> kernel/proc/rq.rs (the
        // scheduler hot core). `sched_entity__bindgen_ty_1` is the
        // degraded `__BindgenUnionField` blob shell of the leading
        // anonymous rb/list union (emitted that way since P3-N1
        // blocklisted its arm types) that bindgen would otherwise still
        // emit as an orphan; the native carries the real Rust union
        // `SchedEntityLink` (N2 `tnode` precedent). The blocklist
        // regexes are anchored, so `rq` matches neither `rq_percpu` nor
        // the `rq_*` functions.
        .blocklist_type("rq|rq_percpu|sched_entity|sched_entity__bindgen_ty_1")
        .raw_line("pub use crate::proc::rq::Rq as rq;")
        .raw_line("pub use crate::proc::rq::RqPercpu as rq_percpu;")
        .raw_line("pub use crate::proc::rq::SchedEntity as sched_entity;");

    // ------------------------------------------------------------------
    // P3-N8 nativization: the fs-driver private (tmpfs + xv6fs) + tty +
    // vfs_inode_ref + sock type families. Same blocklist + `pub use`
    // re-export technique as P3-N2..N7 above. Layout evidence for the
    // whole family: temporary in-tree `offset_of!` gate on the live
    // bindgen forms + cross-compiler value probe (toolchain gcc,
    // rv64gc/lp64d — scratchpad p3n8_probe_values.c); the two AGREE on
    // every size/align/offset (no pipe-style divergence, P3-N5
    // precedent checked). `termios`/`winsize` (kernel/inc/uabi/
    // termios.h) stay bindgen-emitted: userspace-ABI layout contracts
    // (P3-4 scrutiny class), out of nativization scope. `mbuf`
    // (kernel/inc/dev/net.h) stays bindgen-emitted: its `buf[]` backing
    // store is DMA-written by the x1_emac rx/tx descriptor rings —
    // DMA-adjacent, P3-4 scrutiny class.
    builder = builder
        // kernel/inc/types.h `struct vfs_inode_ref` (no typedef) ->
        // kernel/vfs/inode.rs (with the rest of the vfs family, N5).
        .blocklist_type("vfs_inode_ref")
        .raw_line("pub use crate::vfs::inode::VfsInodeRef as vfs_inode_ref;")
        // kernel/inc/tty/tty_types.h `struct tty`/`struct tty_ops` (no
        // typedefs) -> kernel/tty/tty.rs. `TtyOps` keeps bindgen's
        // `Option<unsafe extern "C" fn>` ops-table form verbatim
        // (trait-ification is P3-10). The blocklist regexes are
        // anchored, so `tty` matches neither `tty_ops` nor the `tty_*`
        // functions.
        .blocklist_type("tty|tty_ops")
        .raw_line("pub use crate::tty::tty::Tty as tty;")
        .raw_line("pub use crate::tty::tty::TtyOps as tty_ops;")
        // `struct sock` (kernel/inc/defs.h forward declaration only —
        // the C tree never had a header definition) -> kernel/sysnet.rs,
        // which has been the canonical native definition since its
        // Phase 2 port. bindgen only ever emitted an opaque
        // `_unused: [u8; 0]` shell for the forward declaration; the
        // facade re-export unifies the type identity so `vfs_file`'s
        // `sock` pointer field points at the real native type. No
        // layout gate needed (there is no C layout to agree with —
        // pointers only cross the boundary).
        .blocklist_type("sock")
        .raw_line("pub use crate::sysnet::sock;")
        // kernel/vfs/tmpfs/tmpfs_private.h `struct tmpfs_superblock`/
        // `struct tmpfs_sb_private` -> kernel/vfs/tmpfs/superblock.rs;
        // `struct tmpfs_inode`/`struct tmpfs_dentry` ->
        // kernel/vfs/tmpfs/inode.rs. The three
        // `tmpfs_inode__bindgen_ty_*` shells are the degraded
        // `__BindgenUnionField` blob forms of the anonymous
        // `{ dir; sym; file; }` union nest bindgen would otherwise
        // still emit as orphans; the native carries the real Rust union
        // `TmpfsInodeData` + arms (N6 `PageTypeData` precedent).
        .blocklist_type("tmpfs_sb_private|tmpfs_superblock|tmpfs_dentry")
        .blocklist_type("tmpfs_inode(__bindgen_ty_1(__bindgen_ty_[123])?)?")
        .raw_line("pub use crate::vfs::tmpfs::superblock::TmpfsSbPrivate as tmpfs_sb_private;")
        .raw_line("pub use crate::vfs::tmpfs::superblock::TmpfsSuperblock as tmpfs_superblock;")
        .raw_line("pub use crate::vfs::tmpfs::inode::TmpfsInode as tmpfs_inode;")
        .raw_line("pub use crate::vfs::tmpfs::inode::TmpfsDentry as tmpfs_dentry;")
        // kernel/vfs/xv6fs/xv6fs_private.h `struct xv6fs_logheader`/
        // `struct xv6fs_log` -> kernel/vfs/xv6fs/log.rs (NOTE:
        // `xv6fs_logheader` is the ON-DISK log-header record — see
        // log.rs's loud P3-4-scrutiny note and byte-exact asserts);
        // `struct xv6fs_superblock` -> kernel/vfs/xv6fs/superblock.rs;
        // `struct xv6fs_inode` -> kernel/vfs/xv6fs/inode.rs;
        // kernel/vfs/xv6fs/block_cache.h `struct xv6fs_block_cache` ->
        // kernel/vfs/xv6fs/block_cache.rs. The on-disk
        // `superblock`/`dinode`/`dirent` records (vfs/xv6fs/ondisk.h)
        // stay bindgen-emitted (P3-4 scrutiny class).
        .blocklist_type("xv6fs_logheader|xv6fs_log|xv6fs_block_cache|xv6fs_superblock|xv6fs_inode")
        .raw_line("pub use crate::vfs::xv6fs::log::Xv6fsLogHeader as xv6fs_logheader;")
        .raw_line("pub use crate::vfs::xv6fs::log::Xv6fsLog as xv6fs_log;")
        .raw_line("pub use crate::vfs::xv6fs::block_cache::Xv6fsBlockCache as xv6fs_block_cache;")
        .raw_line("pub use crate::vfs::xv6fs::superblock::Xv6fsSuperblock as xv6fs_superblock;")
        .raw_line("pub use crate::vfs::xv6fs::inode::Xv6fsInode as xv6fs_inode;")
        // `__IncompleteArrayField` used to be bindgen-emitted because
        // (only) the `tmpfs_dentry` emission spelled its C flexible
        // array member with it; with `tmpfs_dentry` blocklisted, bindgen
        // no longer emits the helper, but the native `Bio` (P3-N4) and
        // `TmpfsDentry` (P3-N8) still use it. Injected verbatim —
        // byte-identical to bindgen's own definition (same repr, same
        // API), so every consumer keeps compiling unchanged.
        .raw_line(concat!(
            "#[repr(C)]\n",
            "#[derive(Default)]\n",
            "pub struct __IncompleteArrayField<T>(::core::marker::PhantomData<T>, [T; 0]);\n",
            "impl<T> __IncompleteArrayField<T> {\n",
            "    #[inline]\n",
            "    pub const fn new() -> Self {\n",
            "        __IncompleteArrayField(::core::marker::PhantomData, [])\n",
            "    }\n",
            "    #[inline]\n",
            "    pub fn as_ptr(&self) -> *const T {\n",
            "        self as *const _ as *const T\n",
            "    }\n",
            "    #[inline]\n",
            "    pub fn as_mut_ptr(&mut self) -> *mut T {\n",
            "        self as *mut _ as *mut T\n",
            "    }\n",
            "    #[inline]\n",
            "    pub unsafe fn as_slice(&self, len: usize) -> &[T] {\n",
            "        ::core::slice::from_raw_parts(self.as_ptr(), len)\n",
            "    }\n",
            "    #[inline]\n",
            "    pub unsafe fn as_mut_slice(&mut self, len: usize) -> &mut [T] {\n",
            "        ::core::slice::from_raw_parts_mut(self.as_mut_ptr(), len)\n",
            "    }\n",
            "}\n",
            "impl<T> ::core::fmt::Debug for __IncompleteArrayField<T> {\n",
            "    fn fmt(&self, fmt: &mut ::core::fmt::Formatter<'_>) -> ::core::fmt::Result {\n",
            "        fmt.write_str(\"__IncompleteArrayField\")\n",
            "    }\n",
            "}"
        ));

    // ------------------------------------------------------------------
    // P3-N9 nativization: the thread family — `struct thread`, the most
    // consumed type in the kernel, plus its embedded `thread_signal_t`.
    // Same blocklist + `pub use` re-export technique as P3-N2..N8 above.
    // Layout evidence: temporary in-tree `offset_of!` gate on the live
    // bindgen forms + cross-compiler `_Static_assert` probe (toolchain
    // gcc, rv64gc/lp64d — scratchpad p3n9_static_assert_probe.c) + the
    // gcc-generated build/kernel/inc/asm-offsets.h THREAD_* defines;
    // all three agree on every size/align/offset (no pipe-style
    // divergence, P3-N5 precedent checked).
    //
    // ASM-OFFSET NOTE: `thread` is in kernel/inc/CMakeLists.txt's
    // gen_asm_offsets.py set, but no .S file consumes any THREAD_*
    // macro (grep-verified: kernelvec.S/trampoline.S use only
    // TRAPFRAME_*/UTRAPFRAME_*/CPU_LOCAL_*; swtch.S hardcodes context
    // offsets). The generator keeps reading the untouched C header, and
    // thread.rs's per-field hardcoded asserts pin the native to those
    // same values.
    //
    // uabi determination: `siginfo`/`sigval`/`stack` (stack_t) are
    // defined in kernel/inc/uabi/signal.h — userspace-ABI layout
    // contracts (P3-4 scrutiny class), they stay bindgen-emitted; the
    // native `ThreadSignal` embeds `stack_t` by value via its
    // `crate::bindings` path (mixed-tier, cf. `KsigInfo::info`).
    //
    // `context`/`trapframe`/`utrapframe`/`cpu_local` are untouched
    // (P3-5 asm-offset set); `thread` holds only POINTERS to them.
    builder = builder
        // kernel/inc/signal_types.h `struct thread_signal` /
        // `thread_signal_t` -> kernel/proc/signal.rs (with the N4
        // signal family).
        .blocklist_type("thread_signal|thread_signal_t")
        .raw_line("pub use crate::proc::signal::ThreadSignal as thread_signal;")
        .raw_line("pub use crate::proc::signal::ThreadSignal as thread_signal_t;")
        // kernel/inc/proc/thread_types.h `struct thread` (no typedef) ->
        // kernel/proc/thread.rs. The four `thread__bindgen_ty_*` shells
        // are the zero-sized align(64) `__STRUCT_CACHELINE_PADDING`
        // markers bindgen would otherwise still emit as orphans (the
        // native carries explicit `_pad*` arrays instead — nothing ever
        // named the shells outside `thread` itself, grep-verified). The
        // regexes are anchored, so `thread` matches neither
        // `thread_state` (stays bindgen: constified enum) nor
        // `thread_group`/`thread_signal` (native, own entries).
        .blocklist_type("thread|thread__bindgen_ty_[1234]")
        .raw_line("pub use crate::proc::thread::Thread as thread;");

    // ------------------------------------------------------------------
    // P3-5 nativization: the asm-offset-locked trio — `struct trapframe`
    // + `struct utrapframe` (kernel/inc/trapframe.h), `struct context`
    // (same header), `struct cpu_local` (kernel/inc/smp/percpu_types.h).
    // Same blocklist + `pub use` re-export technique as P3-N2..N9 above.
    //
    // DANGER-ZONE NOTE: these are the only nativized types with
    // *assembly* consumers — kernelvec.S/trampoline.S address trapframe/
    // utrapframe/cpu_local fields through asm-offsets.h macros, and
    // swtch.S hardcodes the context offsets as literal displacements.
    // As of this wave asm-offsets.h is a static checked-in file
    // (kernel/inc/asm-offsets.h, byte-value-identical to the last
    // gcc-generated output); the natives' hardcoded per-field const
    // asserts (each citing its .S consumer) are the drift enforcement —
    // they fail this target build on any layout change. scripts/
    // gen_asm_offsets.py and its cmake hook are gone.
    //
    // Layout evidence: golden gcc-generated asm-offsets.h values +
    // toolchain-gcc `_Static_assert` probe (rv64gc/lp64d, scratchpad
    // p3_5_static_assert_probe.c) + the pre-nativization bindgen output —
    // three-way agreement on every size/align/offset.
    //
    // The C headers themselves stay in wrapper.h's include graph (other
    // still-bindgen headers `#include` them); only these type EMISSIONS
    // move. No typedefs exist for any of the four (bare tag names only);
    // the regexes are anchored, so `context` does not match
    // `context_switch_finish` (a fn) and `trapframe` does not match
    // `utrapframe` (own entry).
    builder = builder
        // kernel/inc/trapframe.h `struct trapframe` / `struct utrapframe`
        // -> kernel/irq/trap.rs (the trap entry/exit surface that owns
        // the .S contract).
        .blocklist_type("trapframe|utrapframe")
        .raw_line("pub use crate::irq::trap::Trapframe as trapframe;")
        .raw_line("pub use crate::irq::trap::Utrapframe as utrapframe;")
        // kernel/inc/trapframe.h `struct context` -> kernel/proc/sched.rs
        // (home of `__swtch_context`'s extern and every context-switch
        // call site).
        .blocklist_type("context")
        .raw_line("pub use crate::proc::sched::Context as context;")
        // kernel/inc/smp/percpu_types.h `struct cpu_local` ->
        // kernel/ipi.rs (owner of the `cpus[]` backing array; the
        // accessor wrapper stays `machine::CpuLocal` — see ipi.rs's
        // name-split note).
        .blocklist_type("cpu_local")
        .raw_line("pub use crate::ipi::CpuLocal as cpu_local;");

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
        // (P3-N6: `page_struct__bindgen_ty_2__bindgen_ty_1` is no longer
        // emitted — the whole page family is blocklisted-native now, the
        // empty `anon` arm included (`kernel/mm/page.rs`'s `PageAnon`,
        // deliberately unattributed for the same shifting reason). The
        // entry stays as the documented pattern for any future empty
        // placeholder arm.)
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
