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
