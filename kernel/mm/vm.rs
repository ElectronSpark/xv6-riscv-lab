//! VM V2 — VMA atomic / SBI / rb-tree comparator helpers (porting in progress).
//!
//! This file is the bindgen-using counterpart to `vm_pgtab.rs` (V1 — pure
//! Sv39 page-table primitives). It uses the auto-generated `kernel_bindings`
//! to access C struct fields directly with the same memory layout the
//! kernel C code uses, which is verified at link time by all other modules
//! sharing the same types.
//!
//! Functions ported here (V2 first slice):
//!   * `__vma_cmp`         — rb-tree key comparator
//!   * `__cma_get_key`     — rb-tree key extractor (returns vma->start)
//!   * `vm_cpu_online`     — mark a CPU as using this vm + update trapframe PTE
//!   * `vm_cpu_offline`    — clear a CPU from this vm's cpumask
//!   * `vm_get_cpumask`    — acquire-load of vm->cpumask
//!   * `vm_remote_sfence`  — TLB shootdown via SBI hfence
//!
//! Functions still in `kernel/mm/vm.c` (V2 remainder + V3-V5):
//!   __vma_alloc, __vma_free, __vma_set_free, __vma_copy,
//!   __vm_unmap_trapframe, __vm_map_trampoline,
//!   vma_validate / vma_free, vm copy/destroy/heap/stack/mmap.

#![allow(non_snake_case)]

use core::ffi::{c_char, c_int, c_void};
use core::marker::PhantomData;
use core::sync::atomic::{compiler_fence, fence, AtomicU64, Ordering};

use crate::bindings::{
    list_node_t, rb_node, rb_root, rb_root_opts, rwsem_t, slab_cache_t, spinlock_t, vfs_file,
    vfs_inode, vfs_inode_ref, vm, vma, EACCES, EBADF, EFAULT, EINVAL, ENAMETOOLONG, ENOMEM,
    MADV_DONTNEED, MADV_NORMAL, MADV_RANDOM, MADV_SEQUENTIAL, MADV_WILLNEED, MAP_ANONYMOUS,
    MAP_FIXED, MAP_PRIVATE, MAP_SHARED, MAXUSTACK, MAXVA, MREMAP_FIXED, MREMAP_MAYMOVE, NCPU,
    PAGE_SHIFT, PGSHIFT, PGSIZE, PROT_EXEC, PROT_MASK, PROT_NONE, PROT_READ, PROT_WRITE, PTE_A,
    PTE_D, PTE_R, PTE_RSW_w, PTE_U, PTE_V, PTE_W, PTE_X, RWLOCK_PRIO_READ, TRAPFRAME,
    UHEAP_MAX_TOP, USERSTACK_GROWTH, USTACK_MAX_BOTTOM, USTACKTOP, UVMBOTTOM, UVMTOP,
    VMA_FLAG_PROT_MASK, VMA_FLAG_USER,
};

// ---------------------------------------------------------------------------
// Native layouts — Wave P3-N6 (mm type family, vm slice).
//
// These ARE the kernel-wide Rust definitions of `kernel/inc/mm/
// vm_types.h`'s `struct vm` (`vm_t`) / `struct vma` (`vma_t`) now:
// `build.rs` blocklists the bindgen-generated forms and injects `pub use
// crate::mm::vm::... as ...;` facade re-exports for both the struct tag
// and `_t` typedef names, so this file's `vm`/`vma` imports above resolve
// right back here. Field names/types reproduce bindgen's exactly (`_pad0`
// reproduces bindgen's `__bindgen_padding_0: [u64; 4]` verbatim — the C
// `__ALIGNED_CACHELINE` rides the `spinlock_t` typedef, the native
// `RawSpinlock` is align 8 in Rust, so the explicit pad carries the
// 64-byte lock placement, exactly as bindgen emitted it; `rw_lock`'s
// 64-alignment comes from the native `rwsem_t`'s own `align(64)`).
// Neither struct derived Copy nor Clone in the pre-nativization bindgen
// output (embedded native-lock/intrusive-node members answered No — see
// build.rs's P3-N6 FINDING note), so the natives faithfully have no
// derives; nothing in the remaining bindgen output embeds either by
// value (verified — `thread.vm` is a pointer).
//
// Layout evidence (P3-N6): temporary in-tree `offset_of!` gate on the
// live bindgen forms + cross-compiler `_Static_assert` probe (toolchain
// gcc, rv64gc/lp64d — scratchpad p3n6_static_assert_probe.c); the two
// agree on every size/align/offset.
// ---------------------------------------------------------------------------

/// `struct vma` / `vma_t` (`kernel/inc/mm/vm_types.h`) — one virtual
/// memory area (rb-tree node + list linkage + range/flags/file backing).
#[repr(C)]
pub struct Vma {
    pub rb_entry: rb_node,
    pub list_entry: list_node_t,
    pub free_list_entry: list_node_t,
    pub vm: *mut Vm,
    pub start: crate::bindings::uint64,
    pub end: crate::bindings::uint64,
    pub flags: crate::bindings::uint64,
    pub file: *mut vfs_file,
    pub pgoff: crate::bindings::uint64,
}

/// `struct vm` / `vm_t` (`kernel/inc/mm/vm_types.h`) — one address
/// space (VMA tree + pagetable + CPU mask).
#[repr(C, align(64))]
pub struct Vm {
    pub rw_lock: rwsem_t,
    pub vm_tree: rb_root,
    pub trapframe_pte: *mut crate::bindings::pte_t,
    pub stack: *mut Vma,
    pub stack_size: usize,
    pub heap: *mut Vma,
    pub heap_size: usize,
    pub vm_list: list_node_t,
    pub vm_free_list: list_node_t,
    pub cpumask: crate::bindings::cpumask_t,
    pub(crate) _pad0: [u64; 4],
    pub spinlock: spinlock_t,
    pub pagetable: crate::bindings::pagetable_t,
    pub refcount: c_int,
}

// P3-N6 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (verified in-tree by the temporary
// `offset_of!` gate) and independently confirmed by the cross-compiler
// `_Static_assert` probe (toolchain gcc agrees on every value).
const _: () = {
    assert!(core::mem::size_of::<Vma>() == 104, "vma size");
    assert!(core::mem::align_of::<Vma>() == 8, "vma align");
    assert!(core::mem::offset_of!(Vma, rb_entry) == 0, "vma.rb_entry");
    assert!(core::mem::offset_of!(Vma, list_entry) == 24, "vma.list_entry");
    assert!(core::mem::offset_of!(Vma, free_list_entry) == 40, "vma.free_list_entry");
    assert!(core::mem::offset_of!(Vma, vm) == 56, "vma.vm");
    assert!(core::mem::offset_of!(Vma, start) == 64, "vma.start");
    assert!(core::mem::offset_of!(Vma, end) == 72, "vma.end");
    assert!(core::mem::offset_of!(Vma, flags) == 80, "vma.flags");
    assert!(core::mem::offset_of!(Vma, file) == 88, "vma.file");
    assert!(core::mem::offset_of!(Vma, pgoff) == 96, "vma.pgoff");

    assert!(core::mem::size_of::<Vm>() == 384, "vm size");
    assert!(core::mem::align_of::<Vm>() == 64, "vm align");
    assert!(core::mem::offset_of!(Vm, rw_lock) == 0, "vm.rw_lock");
    assert!(core::mem::offset_of!(Vm, vm_tree) == 192, "vm.vm_tree");
    assert!(core::mem::offset_of!(Vm, trapframe_pte) == 208, "vm.trapframe_pte");
    assert!(core::mem::offset_of!(Vm, stack) == 216, "vm.stack");
    assert!(core::mem::offset_of!(Vm, stack_size) == 224, "vm.stack_size");
    assert!(core::mem::offset_of!(Vm, heap) == 232, "vm.heap");
    assert!(core::mem::offset_of!(Vm, heap_size) == 240, "vm.heap_size");
    assert!(core::mem::offset_of!(Vm, vm_list) == 248, "vm.vm_list");
    assert!(core::mem::offset_of!(Vm, vm_free_list) == 264, "vm.vm_free_list");
    assert!(core::mem::offset_of!(Vm, cpumask) == 280, "vm.cpumask");
    assert!(core::mem::offset_of!(Vm, spinlock) == 320, "vm.spinlock");
    assert!(core::mem::offset_of!(Vm, pagetable) == 344, "vm.pagetable");
    assert!(core::mem::offset_of!(Vm, refcount) == 352, "vm.refcount");
};

// Constants not picked up by bindgen (defined in headers as #defines that
// reference other macros).
// ===========================================================================
// impl Vma — KERNEL-OO wave: former `vma_*`/`__vma_*`/`xv6_vm_vma_*`/
// `xv6_vm_list_*` free fns namespaced as associated fns (same strategy).
// ===========================================================================
impl Vma {
    #[inline]
    fn xv6_vm_list_insert_after_list_entry(prev: *mut vma, node: *mut vma) {
        list_entry_insert_after(machine::vma_list_entry_ptr(prev), machine::vma_list_entry_ptr(node));
    }

    #[inline]
    fn xv6_vm_list_insert_after_free_list_entry(prev: *mut vma, node: *mut vma) {
        list_entry_insert_after(
            machine::vma_free_list_entry_ptr(prev),
            machine::vma_free_list_entry_ptr(node),
        );
    }

    #[inline]
    fn xv6_vm_list_detach_list_entry(node: *mut vma) {
        list_entry_detach(machine::vma_list_entry_ptr(node));
    }

    #[inline]
    fn xv6_vm_list_detach_free_list_entry(node: *mut vma) {
        list_entry_detach(machine::vma_free_list_entry_ptr(node));
    }

    #[inline]
    fn xv6_vm_vma_left(v: *mut vma) -> *mut vma {
        if v.is_null() || machine::vma_vm_ptr(v).is_null() {
            return core::ptr::null_mut();
        }
        let p = rb_prev_node(machine::vma_rb_entry_ptr(v));
        if p.is_null() {
            return core::ptr::null_mut();
        }
        machine::list_container_of::<vma>(
            p as *mut list_node_t,
            core::mem::offset_of!(vma, rb_entry),
        )
    }

    #[inline]
    fn xv6_vm_vma_right(v: *mut vma) -> *mut vma {
        if v.is_null() || machine::vma_vm_ptr(v).is_null() {
            return core::ptr::null_mut();
        }
        let n = rb_next_node(machine::vma_rb_entry_ptr(v));
        if n.is_null() {
            return core::ptr::null_mut();
        }
        machine::list_container_of::<vma>(
            n as *mut list_node_t,
            core::mem::offset_of!(vma, rb_entry),
        )
    }

    #[inline(always)]
    fn vma_pool_ptr() -> *mut slab_cache_t {
        __VMA_POOL.as_ptr()
    }

    /// Initialise the slab pool that backs `vma_t` objects.
    pub(crate) fn __vma_pool_init() {
        // SAFETY: `slab_cache_init` is a plain FFI call; `Vma::vma_pool_ptr()` is a
        // valid, exclusively-owned static, and the name string is a `'static`
        // NUL-terminated literal.
        unsafe {
            slab_cache_init(
                Vma::vma_pool_ptr(),
                b"vm area\0".as_ptr() as *mut c_char,
                core::mem::size_of::<vma>(),
                SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP,
            );
        }
    }

    /// Allocate and zero-initialise a fresh `vma_t`, linked into `vm`.
    /// Returns NULL on OOM. Mirrors the C `__vma_alloc`.
    pub(crate) fn __vma_alloc(vm_ptr: *mut vm) -> *mut vma {
        // SAFETY: `slab_alloc` returns either null or a fresh, exclusively-owned
        // `size_of::<vma>()`-sized allocation from `Vma::vma_pool_ptr()`; the
        // null-check below gates every subsequent write into it.
        unsafe {
            let vma_ptr = slab_alloc(Vma::vma_pool_ptr()) as *mut vma;
            if vma_ptr.is_null() {
                return core::ptr::null_mut();
            }
            core::ptr::write_bytes(vma_ptr, 0, 1);
            xv6_vm_rb_node_init(&raw mut (*vma_ptr).rb_entry);
            xv6_vm_list_entry_init(&raw mut (*vma_ptr).list_entry);
            xv6_vm_list_entry_init(&raw mut (*vma_ptr).free_list_entry);
            (*vma_ptr).vm = vm_ptr;
            vma_ptr
        }
    }

    /// Free a `vma_t` previously returned by `__vma_alloc`. Includes double-free
    /// detection via a magic value poison.
    pub(crate) fn __vma_free(vma_ptr: *mut vma) {
        if vma_ptr.is_null() {
            return;
        }
        // SAFETY: `vma_ptr` checked non-null above; caller passes a live `vma`
        // (never previously poisoned — the magic-value check below rejects a
        // double-free).
        unsafe {
            if (*vma_ptr).start == VMA_FREED_MAGIC && (*vma_ptr).end == VMA_FREED_MAGIC {
                xv6_vm_warn_vma_double_free(vma_ptr as *mut c_void);
                return;
            }
            if (*vma_ptr).vm.is_null() {
                xv6_vm_warn_vma_null_vm(vma_ptr as *mut c_void);
                return;
            }
            // Poison so that a double-free can be detected on the next call.
            (*vma_ptr).vm = core::ptr::null_mut();
            (*vma_ptr).start = VMA_FREED_MAGIC;
            (*vma_ptr).end = VMA_FREED_MAGIC;
            slab_free(vma_ptr as *mut c_void);
        }
    }

    /// Unmap every page covered by `vma`, drop its file reference, and mark it
    /// as `PROT_NONE`. Caller must hold the vm rwlock for write.
    pub(crate) fn __vma_set_free(vma_ptr: *mut vma) {
        // SAFETY: `vma_ptr`, when non-null (checked first thing below), is a
        // live `vma` owned by a live `vm`; `pte`/leaf-PTE pointers below come
        // from `PageTable::walk()`, which only returns valid PTE pointers into that same
        // live pagetable.
        unsafe {
            if vma_ptr.is_null() || (*vma_ptr).vm.is_null() {
                return;
            }
            if (*vma_ptr).flags == PROT_NONE as u64 {
                return;
            }
            if ((*vma_ptr).start & ((PGSIZE as u64) - 1)) != 0 {
                xv6_vm_panic(b"__vma_set_free: vma start not aligned\0".as_ptr() as *const c_char);
            }

            let pagetable = (*(*vma_ptr).vm).pagetable;
            if !pagetable.is_null() {
                // Page-aligned VA stride over the vma range; the per-page PTE
                // teardown below stays a raw hardware-visible write (page-table
                // floor), only the walk counter becomes an iterator.
                for a in ((*vma_ptr).start..(*vma_ptr).end).step_by(PGSIZE as usize) {
                    let pte = crate::mm::vm_pgtab::PageTable::walk(
                        pagetable as *mut u64,
                        a,
                        0,
                        core::ptr::null_mut(),
                        core::ptr::null_mut(),
                    );
                    if !pte.is_null() {
                        let pte_val = *pte;
                        if pte_flags(pte_val) == PTE_V as u64 {
                            xv6_vm_panic(b"__vma_set_free: not a leaf\0".as_ptr() as *const c_char);
                        }
                        if (pte_val & (PTE_V as u64)) != 0 {
                            let pa = pte_to_pa(pte_val);
                            *pte = 0;
                            // Notify any remote core currently using this mapping.
                            Vm::vm_remote_sfence((*vma_ptr).vm);
                            page_ref_dec(pa as *mut c_void);
                        }
                    }
                }
            }

            (*vma_ptr).flags = PROT_NONE as u64;
            if !(*vma_ptr).file.is_null() {
                VfsFile::vfs_fput((*vma_ptr).file);
            }
            (*vma_ptr).file = core::ptr::null_mut();
            (*vma_ptr).pgoff = 0;
            if !list_node_is_detached(&raw const (*vma_ptr).free_list_entry) {
                xv6_vm_panic(b"__vma_set_free: vma already in free list\0".as_ptr() as *const c_char);
            }
        }
    }

    /// Duplicate `src` into `dst`. For non-PROT_NONE source ranges, walks the
    /// source pagetable and installs COW PTEs in both src and dst (clearing PTE_W
    /// and setting PTE_RSW_w). File-backed VMAs get an extra `vfs_fdup` reference.
    ///
    /// Returns 0 on success, `-EINVAL` for bad arguments, `-EBADF` if the source
    /// file was already closed, or `-ENOMEM` if a new pagetable entry could not
    /// be allocated (in which case `dst` is reset to PROT_NONE via
    /// `__vma_set_free`).
    pub(crate) fn __vma_copy(dst: *mut vma, src: *mut vma) -> c_int {
        // SAFETY: `dst`/`src`, when non-null (checked first thing below), are
        // live `vma`s each owned by a live `vm`; PTE pointers below come from
        // `PageTable::walk()`, which only returns valid PTE pointers into a live
        // pagetable.
        unsafe {
            if dst.is_null() || src.is_null() {
                return -(EINVAL as c_int);
            }
            if (*src).vm.is_null() || (*dst).vm.is_null() {
                return -(EINVAL as c_int);
            }
            let src_size = (*src).end.wrapping_sub((*src).start);
            let dst_size = (*dst).end.wrapping_sub((*dst).start);
            if src_size != dst_size {
                return -(EINVAL as c_int);
            }
            let prot_mask = VMA_FLAG_PROT_MASK as u64;
            if ((*src).flags & prot_mask) != ((*dst).flags & prot_mask) {
                return -(EINVAL as c_int);
            }

            (*dst).flags = (*src).flags;
            (*dst).pgoff = (*src).pgoff;
            if !(*src).file.is_null() {
                let dup = VfsFile::vfs_fdup((*src).file);
                if dup.is_null() {
                    return -(EBADF as c_int);
                }
                (*dst).file = dup;
            } else {
                (*dst).file = core::ptr::null_mut();
            }

            if (*src).flags != PROT_NONE as u64 {
                let pgtb_src = (*(*src).vm).pagetable as *mut u64;
                let pgtb_dst = (*(*dst).vm).pagetable as *mut u64;
                // Page-aligned VA stride; per-page COW PTE writes below stay raw.
                for a in ((*src).start..(*src).end).step_by(PGSIZE as usize) {
                    let src_pte = crate::mm::vm_pgtab::PageTable::walk(
                        pgtb_src,
                        a,
                        0,
                        core::ptr::null_mut(),
                        core::ptr::null_mut(),
                    );
                    if !src_pte.is_null() && *src_pte != 0 {
                        let pte_val = *src_pte;
                        if pte_flags(pte_val) == PTE_V as u64 {
                            xv6_vm_panic(b"__vma_copy: not a leaf\0".as_ptr() as *const c_char);
                        }
                        if (pte_flags(pte_val) & (PTE_V as u64)) != 0 {
                            let new_pte = crate::mm::vm_pgtab::PageTable::walk(
                                pgtb_dst,
                                a,
                                1,
                                core::ptr::null_mut(),
                                core::ptr::null_mut(),
                            );
                            if new_pte.is_null() {
                                Vma::__vma_set_free(dst);
                                return -(ENOMEM as c_int);
                            }
                            // Set COW flag and clear writeable on the source PTE,
                            // then copy the (now read-only) PTE into the destination.
                            *src_pte |= PTE_RSW_w as u64;
                            *src_pte &= !(PTE_W as u64);
                            *new_pte = *src_pte;
                            let pa = pte_to_pa(*src_pte);
                            let refc = crate::mm::page::Page::page_ref_inc(pa as *mut c_void);
                            if refc <= 0 {
                                xv6_vm_panic(
                                    b"__vma_copy: page refcnt should be greater than 0\0".as_ptr()
                                        as *const c_char,
                                );
                            }
                        }
                    }
                }
                // Flush remote TLBs that may still cache the now-read-only mappings.
                Vm::vm_remote_sfence((*src).vm);
            }
            0
        }
    }

    /// Split `vma` at virtual address `va`. The original keeps `[start, va)`,
    /// a freshly-allocated new vma owns `[va, end)`. Returns the new (right)
    /// vma, or `vma` itself if `va == vma->start`, or NULL on invalid input /
    /// allocation failure. File-backed VMAs duplicate the file reference via
    /// `vfs_fdup` and shift `pgoff` accordingly. The new vma is linked into
    /// the rb-tree and the per-vm list (and the free-list if the original
    /// flags == PROT_NONE).
    pub(crate) fn vma_split(vma_ptr: *mut vma, va: u64) -> *mut vma {
        // SAFETY: `vma_ptr`, when non-null and owning a non-null `vm` (checked
        // first thing below), is a live `vma`; `new_vma` comes from
        // `__vma_alloc`, itself checked non-null before use.
        unsafe {
            if vma_ptr.is_null() || (*vma_ptr).vm.is_null() {
                return core::ptr::null_mut();
            }
            if va < (*vma_ptr).start || va >= (*vma_ptr).end {
                return core::ptr::null_mut();
            }
            if va == (*vma_ptr).start {
                return vma_ptr;
            }

            let vm_ptr = (*vma_ptr).vm;
            let new_vma = Vma::__vma_alloc(vm_ptr);
            if new_vma.is_null() {
                return core::ptr::null_mut();
            }

            (*new_vma).start = va;
            let ins = rb_insert_color(
                &mut (*vm_ptr).vm_tree as *mut _,
                &mut (*new_vma).rb_entry as *mut _,
            );
            if ins != (&mut (*new_vma).rb_entry as *mut _) {
                xv6_vm_panic(b"vma_split: rb_insert_color failed\0".as_ptr() as *const c_char);
            }

            (*new_vma).end = (*vma_ptr).end;
            (*new_vma).flags = (*vma_ptr).flags;
            if !(*vma_ptr).file.is_null() {
                (*new_vma).file = VfsFile::vfs_fdup((*vma_ptr).file);
                (*new_vma).pgoff = (*vma_ptr).pgoff + (va - (*vma_ptr).start);
            } else {
                (*new_vma).file = core::ptr::null_mut();
                (*new_vma).pgoff = 0;
            }

            (*vma_ptr).end = va;

            Vma::xv6_vm_list_insert_after_list_entry(vma_ptr, new_vma);
            if (*vma_ptr).flags == PROT_NONE as u64 {
                Vma::xv6_vm_list_insert_after_free_list_entry(vma_ptr, new_vma);
            }

            new_vma
        }
    }

    /// Merge two adjacent VMAs of identical protection (and matching file/pgoff
    /// for file-backed). Returns the surviving left-side VMA, or NULL if the
    /// merge is illegal. The right-side VMA is unlinked from the rb-tree and
    /// both lists, its file reference released, and the descriptor freed.
    pub(crate) fn vma_merge(mut vma1: *mut vma, mut vma2: *mut vma) -> *mut vma {
        // SAFETY: `vma1`/`vma2`, when non-null (checked first thing below), are
        // live `vma`s.
        unsafe {
            if vma1.is_null() || vma2.is_null() || (*vma1).vm != (*vma2).vm {
                return core::ptr::null_mut();
            }
            // VMA_ADJACENT: end1 == start2 || end2 == start1
            if !((*vma1).end == (*vma2).start || (*vma2).end == (*vma1).start) {
                return core::ptr::null_mut();
            }
            let prot_mask = VMA_FLAG_PROT_MASK as u64;
            if ((*vma1).flags & prot_mask) != ((*vma2).flags & prot_mask) {
                return core::ptr::null_mut();
            }
            // Ensure vma1 is always the left one.
            if (*vma1).start > (*vma2).start {
                core::mem::swap(&mut vma1, &mut vma2);
            }
            if (*vma1).file != (*vma2).file {
                return core::ptr::null_mut();
            }
            if !(*vma1).file.is_null()
                && ((*vma2).pgoff - (*vma1).pgoff) != ((*vma2).start - (*vma1).start)
            {
                return core::ptr::null_mut();
            }

            (*vma1).end = (*vma2).end;
            let del = rb_delete_node_color(
                &mut (*(*vma2).vm).vm_tree as *mut _,
                &mut (*vma2).rb_entry as *mut _,
            );
            if del != (&mut (*vma2).rb_entry as *mut _) {
                xv6_vm_panic(b"vma_merge: rb_delete_node_color failed\0".as_ptr() as *const c_char);
            }
            Vma::xv6_vm_list_detach_list_entry(vma2);
            Vma::xv6_vm_list_detach_free_list_entry(vma2);
            if !(*vma2).file.is_null() {
                VfsFile::vfs_fput((*vma2).file);
                (*vma2).file = core::ptr::null_mut();
            }
            Vma::__vma_free(vma2);

            vma1
        }
    }

    #[inline]
    fn vma_size(v: *mut vma) -> u64 {
        machine::vma_end(v) - machine::vma_start(v)
    }

    /// Allocate a VMA covering `[va, va+size)` with `flags`. If `va == 0`, the
    /// last free-list entry large enough is used. Returns the new (possibly
    /// split-out) VMA, or NULL on invalid input / OOM.
    pub(crate) fn vma_alloc(vm_ptr: *mut vm, mut va: u64, size: u64, flags: u64) -> *mut vma {
        // SAFETY: `vm_ptr`, when non-null (checked first thing below), is a
        // live `vm`; every `*mut vma` touched below (`free_area`, `vma2`, …)
        // comes from `vm_find_area`/`xv6_vm_last_free`/`xv6_vm_prev_free`/
        // `vma_split`, each of which is checked non-null before dereference.
        unsafe {
            Vm::xv6_vm_assert_vm_write_held(
                vm_ptr,
                b"vma_alloc: vm rwsem must be write-held\0".as_ptr() as *const c_char,
            );
            if vm_ptr.is_null() {
                return core::ptr::null_mut();
            }
            if size == 0 || (size & (PGSIZE as u64 - 1)) != 0 {
                return core::ptr::null_mut();
            }
            if (va & (PGSIZE as u64 - 1)) != 0 {
                return core::ptr::null_mut();
            }
            if (flags & VMA_FLAG_PROT_MASK as u64) == 0 {
                return core::ptr::null_mut();
            }

            let free_area: *mut vma = if va == 0 {
                // Find the last free area large enough (reverse walk).
                Vm::vm_free_list_rev_iter(vm_ptr)
                    .find(|&cur| Vma::vma_size(cur) >= size)
                    .unwrap_or(core::ptr::null_mut())
            } else {
                Vm::vm_find_area(vm_ptr, va)
            };

            if free_area.is_null() {
                return core::ptr::null_mut();
            }
            if (*free_area).flags != PROT_NONE as u64 {
                return core::ptr::null_mut();
            }

            let va_end: u64;
            if va == 0 {
                if Vma::vma_size(free_area) < size {
                    return core::ptr::null_mut();
                }
                va = (*free_area).start;
            } else if (*free_area).end - va < size {
                return core::ptr::null_mut();
            }
            va_end = va + size;

            let vma2: *mut vma;
            let mut original_left: *mut vma = core::ptr::null_mut();

            if va > (*free_area).start {
                original_left = free_area;
                vma2 = Vma::vma_split(free_area, va);
                if vma2.is_null() {
                    return core::ptr::null_mut();
                }
            } else {
                vma2 = free_area;
            }

            if va_end < (*vma2).end {
                let vma3 = Vma::vma_split(vma2, va_end);
                if vma3.is_null() {
                    if !original_left.is_null() {
                        Vma::vma_merge(original_left, vma2);
                    }
                    return core::ptr::null_mut();
                }
            }

            Vma::xv6_vm_list_detach_free_list_entry(vma2);
            (*vma2).flags = flags;
            vma2
        }
    }

    /// Mark a VMA as free, link onto the free list, and merge with any
    /// adjacent free VMAs.
    pub(crate) fn vma_free(vm_ptr: *mut vm, mut vma_ptr: *mut vma) -> c_int {
        // SAFETY: `vma_ptr`, when non-null and owned by `vm_ptr` (checked
        // first thing below), is a live `vma`; `left`/`right` come from
        // `xv6_vm_vma_left`/`xv6_vm_vma_right`, each producing either null or a
        // live neighbouring `vma`.
        unsafe {
            Vm::xv6_vm_assert_vm_write_held(
                vm_ptr,
                b"vma_free: vm rwsem must be write-held\0".as_ptr() as *const c_char,
            );
            if vma_ptr.is_null() || (*vma_ptr).vm != vm_ptr {
                return -(EINVAL as c_int);
            }
            if (*vma_ptr).flags == PROT_NONE as u64 {
                return -(EINVAL as c_int);
            }

            let left = Vma::xv6_vm_vma_left(vma_ptr);
            let right = Vma::xv6_vm_vma_right(vma_ptr);
            if left.is_null() && right.is_null() {
                return -(EINVAL as c_int);
            }

            Vma::__vma_set_free(vma_ptr);
            Vm::xv6_vm_list_push_front_free_list_entry(vm_ptr, vma_ptr);

            if !left.is_null() && (*left).flags == PROT_NONE as u64 {
                let merged = Vma::vma_merge(left, vma_ptr);
                if merged != left {
                    xv6_vm_panic(b"vma_free: vma_merge failed with left VMA\0".as_ptr() as *const c_char);
                }
                vma_ptr = left;
            }
            if !right.is_null() && (*right).flags == PROT_NONE as u64 {
                let merged = Vma::vma_merge(vma_ptr, right);
                if merged != vma_ptr {
                    xv6_vm_panic(b"vma_free: vma_merge failed with right VMA\0".as_ptr() as *const c_char);
                }
            }
            0
        }
    }

    /// Validate (and demand-fault as needed) the range `[va, va+size)` inside
    /// `vma` against the access `flags`. Walks every page; for file-backed VMAs
    /// with unmapped pages it drops the pgtable spinlock, invokes the file
    /// fault handler (sleeps allowed), then re-walks. For already-mapped pages
    /// delegates to `xv6_vm_validate_pte` (anonymous PTE / COW handling).
    pub(crate) fn vma_validate(
        vma_ptr: *mut vma,
        va_in: u64,
        size: u64,
        flags: u64,
    ) -> c_int {
        // SAFETY: `vma_ptr`, once past the null/well-formedness checks just
        // below, is a live `vma` owned by a live `vm` with a live `pagetable`;
        // every PTE pointer (`pte`/`pte2`) comes from `PageTable::walk()` into that same
        // pagetable and is checked non-null before dereference. The
        // lock-drop/re-walk around the (possibly sleeping) fault handler is
        // intentional: `_pgl` is reassigned rather than nested, so the pgtable
        // spinlock is never held across `xv6_vm_call_vma_fault`.
        unsafe {
            if flags == PROT_NONE as u64 {
                return -(EINVAL as c_int);
            }
            if vma_ptr.is_null()
                || (*vma_ptr).vm.is_null()
                || (*(*vma_ptr).vm).pagetable.is_null()
            {
                return -(EINVAL as c_int);
            }
            if (flags & !(VMA_FLAG_PROT_MASK as u64)) != 0 {
                return -(EINVAL as c_int);
            }
            if (flags & PROT_EXEC as u64) != 0 {
                if (flags & PROT_READ as u64) == 0 {
                    return -(EINVAL as c_int);
                }
                if (flags & PROT_WRITE as u64) != 0 && (flags & VMA_FLAG_USER as u64) != 0 {
                    return -(EACCES as c_int);
                }
            }

            let mut va = pg_round_down(va_in);
            let va_end: u64 = if size == 0 {
                (*vma_ptr).end
            } else {
                pg_round_up(va_in + size)
            };

            if va < (*vma_ptr).start || va_end > (*vma_ptr).end {
                return -(EFAULT as c_int);
            }
            if (flags & (*vma_ptr).flags) != flags {
                return -(EACCES as c_int);
            }

            let vm_ptr = (*vma_ptr).vm;
            let pagetable = (*vm_ptr).pagetable;
            // `_pgl` is reassigned (drop + reacquire) around the sleeping fault
            // handler below — the only spot in this loop that needs an explicit
            // drop/relock; every other exit just lets the guard's `Drop` release
            // the pgtable spinlock, replacing the original's 6 manual unlock
            // sites (a former source of unlock-on-early-return bugs).
            let mut _pgl = Vm::lock_vm_pgtable(vm_ptr);
            xv6_vm_smp_mb();

            while va < va_end {
                if !(*vma_ptr).file.is_null() {
                    let pte = PageTable::walk(pagetable as *mut u64, va, 1, core::ptr::null_mut(), core::ptr::null_mut());
                    if pte.is_null() {
                        return -(ENOMEM as c_int);
                    }
                    if *pte == 0 {
                        // Drop lock, invoke fault handler (may sleep), re-walk.
                        drop(_pgl);
                        let pa = Vma::xv6_vm_call_vma_fault((*vma_ptr).file, vma_ptr, va);
                        if pa.is_null() {
                            return -(ENOMEM as c_int);
                        }
                        // Take ownership of the freshly-allocated order-0 page
                        // via `PageHandle`: any early return below now frees it
                        // automatically through `Drop` (replacing three manual
                        // `page_free(pa, 0)` call sites — including one that was
                        // reachable on the pte2-null path even before this
                        // conversion). Ownership only transfers away from this
                        // handle on the winning-install path below, via
                        // `into_raw()`.
                        //
                        // SAFETY: `pa` is non-null and was just produced by
                        // `xv6_vm_call_vma_fault`, which allocates a fresh
                        // order-0 page (via `page_alloc`) for every code path
                        // that returns non-null; it has not been freed yet.
                        let page = match unsafe { PageHandle::from_pa(pa, 0) } {
                            Some(p) => p,
                            None => return -(ENOMEM as c_int),
                        };
                        _pgl = Vm::lock_vm_pgtable(vm_ptr);
                        let pte2 = PageTable::walk(pagetable as *mut u64, va, 1, core::ptr::null_mut(), core::ptr::null_mut());
                        if pte2.is_null() {
                            drop(_pgl);
                            // `page` drops here -> page_free(pa, 0).
                            return -(ENOMEM as c_int);
                        }
                        if *pte2 != 0 {
                            // Race: another core already mapped this page;
                            // `page` drops at the end of this arm -> page_free.
                        } else {
                            let vf = (*vma_ptr).flags;
                            let mut pte_flags: u64 = 0;
                            if (vf & PROT_READ as u64) != 0 {
                                pte_flags |= PTE_R as u64;
                            }
                            if (vf & PROT_WRITE as u64) != 0 {
                                pte_flags |= PTE_W as u64;
                            }
                            if (vf & PROT_EXEC as u64) != 0 {
                                pte_flags |= PTE_X as u64;
                            }
                            if (vf & VMA_FLAG_USER as u64) != 0 {
                                pte_flags |= PTE_U as u64;
                            }
                            pte_flags |= (PTE_V | PTE_A | PTE_D) as u64;
                            let pa_pte = ((pa as u64) >> 12) << 10;
                            *pte2 = pa_pte | pte_flags;
                            xv6_vm_sfence_vma();
                            // Ownership transfers to the page table entry; it
                            // will be freed later via unmap, not by this
                            // handle.
                            let _ = page.into_raw();
                        }
                        va += PGSIZE as u64;
                        continue;
                    }
                    // Page already mapped — fall through to normal validation.
                    if Vma::xv6_vm_validate_pte(vma_ptr, pte, flags) != 0 {
                        return -(EFAULT as c_int);
                    }
                    va += PGSIZE as u64;
                    continue;
                }

                // Anonymous VMA path.
                let pte = PageTable::walk(pagetable as *mut u64, va, 1, core::ptr::null_mut(), core::ptr::null_mut());
                if pte.is_null() {
                    return -(ENOMEM as c_int);
                }
                if Vma::xv6_vm_validate_pte(vma_ptr, pte, flags) != 0 {
                    return -(EFAULT as c_int);
                }
                va += PGSIZE as u64;
            }
            0
        }
    }

    #[inline]
    fn vma_size_of(v: *mut vma) -> u64 {
        machine::vma_end(v) - machine::vma_start(v)
    }

    /// File-backed page fault — allocate a fresh anonymous page and copy in the
    /// matching page from the file's page cache (zero-filling holes past EOF).
    /// Mirrors the C `__vma_fault_file_page`.
    fn vma_fault_file_page(vma_ptr: *mut vma, va: u64) -> *mut c_void {
        let Some(f) = machine::VfsFileRef::from_raw(machine::vma_file(vma_ptr)) else {
            return core::ptr::null_mut();
        };
        let inode = FsStruct::vfs_inode_deref(f.inode_slot_ptr());
        if inode.is_null() {
            return core::ptr::null_mut();
        }
        let pc = machine::vfs_inode_pcache_ptr(inode);

        let file_off = machine::vma_pgoff(vma_ptr) + (va - machine::vma_start(vma_ptr));

        let pa = page_alloc(0, PAGE_TYPE_ANON);
        if pa.is_null() {
            return core::ptr::null_mut();
        }
        // Take ownership of the freshly-allocated page: every early return
        // below (cache-miss / read-error) now frees it automatically via
        // `Drop`, replacing two manual `page_free(pa, 0)` call sites.
        // Ownership only leaves this handle on the two success returns,
        // via `into_raw()` (the caller — `vma_validate`, or a custom
        // `.fault` op's caller — re-wraps the same data pointer as its own
        // `PageHandle` and owns it from there).
        //
        // SAFETY: `pa` was just checked non-null and freshly allocated by
        // `page_alloc` above; it has not been freed yet.
        let page = unsafe { PageHandle::from_pa(pa, 0) }
            .unwrap_or_else(|| xv6_vm_panic(b"vma_fault_file_page: page_alloc returned untracked pa\0".as_ptr() as *const c_char));

        // Inode size is loff_t (signed). Treat negative sizes as 0.
        let inode_size = machine::vfs_inode_size(inode);
        let inode_size_u = if inode_size < 0 { 0u64 } else { inode_size as u64 };

        if file_off >= inode_size_u {
            memset(pa, 0, PGSIZE as usize);
            let _ = page.into_raw();
            return pa;
        }

        let mut bytes_to_read: u64 = PGSIZE as u64;
        if file_off + (PGSIZE as u64) > inode_size_u {
            bytes_to_read = inode_size_u - file_off;
        }

        let blkno_512 = file_off / BLK_SIZE;
        let pcpage = crate::mm::pcache::Pcache::get_page(pc, blkno_512);
        if pcpage.is_null() {
            // `page` drops here -> page_free(pa, 0).
            return core::ptr::null_mut();
        }
        let ret = crate::mm::pcache::Pcache::read_page(pc, pcpage);
        if ret != 0 {
            crate::mm::pcache::Pcache::put_page(pc, pcpage);
            // `page` drops here -> page_free(pa, 0).
            return core::ptr::null_mut();
        }

        let pcn = crate::mm::pcache::Pcache::page_get_node(pcpage);
        let data = crate::mm::pcache::PcacheNode::data(pcn);
        memmove(pa, data as *const c_void, bytes_to_read as usize);

        if bytes_to_read < (PGSIZE as u64) {
            memset(
                (pa as *mut u8).wrapping_add(bytes_to_read as usize) as *mut c_void,
                0,
                (PGSIZE as u64 - bytes_to_read) as usize,
            );
        }

        crate::mm::pcache::Pcache::put_page(pc, pcpage);
        let _ = page.into_raw();
        pa
    }

    /// PTE validation for a writable fault. Mirrors the C `__vma_validate_pte_rxw`.
    fn vma_validate_pte_rxw(vma_ptr: *mut vma, pte: *mut u64) -> Result<(), crate::mm::cffi::Errno> {
        use crate::mm::cffi::Errno;
        let pte_val = machine::pte_read(pte);
        if (pte_val & (PTE_W as u64)) != 0 {
            return Ok(());
        }
        let mut flags = pte_flags(pte_val);
        let addr = pte_to_pa(pte_val) as *mut c_void;
        let pa: *mut c_void;
        if pte_val == 0 {
            pa = page_alloc(0, PAGE_TYPE_ANON);
            if pa.is_null() {
                return Err(Errno::NoMem);
            }
            memset(pa, 0, PGSIZE as usize);
        } else if (pte_val & (PTE_V as u64)) != 0 {
            if (pte_val & (PTE_RSW_w as u64)) != 0 {
                // COW path
                pa = page_alloc(0, PAGE_TYPE_ANON);
                if pa.is_null() {
                    return Err(Errno::NoMem);
                }
                memmove(pa, addr as *const c_void, PGSIZE as usize);
                flags &= !(PTE_RSW_w as u64);
                let rc = page_ref_dec(addr);
                if rc < 0 {
                    xv6_vm_panic(b"vma_validate_pte_w: page_ref_dec failed\0".as_ptr() as *const c_char);
                }
            } else {
                pa = addr;
            }
        } else {
            return Err(Errno::Fault);
        }

        flags |= (PTE_V | PTE_W | PTE_A | PTE_D) as u64;
        let vflags = machine::vma_flags(vma_ptr);
        if (vflags & (PROT_READ as u64)) != 0 {
            flags |= PTE_R as u64;
        }
        if (vflags & (PROT_EXEC as u64)) != 0 {
            flags |= PTE_X as u64;
        }
        if (vflags & (VMA_FLAG_USER as u64)) != 0 {
            flags |= PTE_U as u64;
        }
        machine::pte_write(pte, pa_to_pte(pa as u64) | flags);
        Ok(())
    }

    /// PTE validation for a read/exec fault. Mirrors the C `__vma_validate_pte_rx`.
    fn vma_validate_pte_rx(vma_ptr: *mut vma, pte: *mut u64) -> Result<(), crate::mm::cffi::Errno> {
        use crate::mm::cffi::Errno;
        let pte_val = machine::pte_read(pte);
        let mut pa = pte_to_pa(pte_val) as *mut c_void;
        let mut flags = pte_flags(pte_val);

        if pte_val == 0 {
            pa = page_alloc(0, PAGE_TYPE_ANON);
            if pa.is_null() {
                return Err(Errno::NoMem);
            }
            memset(pa, 0, PGSIZE as usize);
            if (machine::vma_flags(vma_ptr) & (PROT_WRITE as u64)) != 0 {
                flags |= PTE_W as u64;
            }
        } else if (pte_val & (PTE_V as u64)) == 0 {
            return Err(Errno::Fault);
        }

        let vflags = machine::vma_flags(vma_ptr);
        if (vflags & (PROT_READ as u64)) != 0 {
            flags |= PTE_R as u64;
        }
        if (vflags & (PROT_EXEC as u64)) != 0 {
            flags |= PTE_X as u64;
        }
        if (vflags & (VMA_FLAG_USER as u64)) != 0 {
            flags |= PTE_U as u64;
        }
        flags |= (PTE_V | PTE_A | PTE_D) as u64;
        machine::pte_write(pte, pa_to_pte(pa as u64) | flags);
        xv6_vm_sfence_vma();
        Ok(())
    }

    /// Combined dispatcher used by `vma_validate`. Mirrors `__vma_validate_pte`.
    fn vma_validate_pte(vma_ptr: *mut vma, pte: *mut u64, flags: u64) -> Result<(), crate::mm::cffi::Errno> {
        use crate::mm::cffi::Errno;
        let pte_val = machine::pte_read(pte);
        let pte_user = (pte_val & (PTE_U as u64)) != 0;
        let vma_user = (flags & (VMA_FLAG_USER as u64)) != 0;
        if pte_val != 0 && (pte_user ^ vma_user) {
            return Err(Errno::Access);
        }
        if (flags & (PROT_WRITE as u64)) != 0 {
            if Vma::vma_validate_pte_rxw(vma_ptr, pte).is_err() {
                return Err(Errno::Fault);
            }
        } else if (flags & ((PROT_READ | PROT_EXEC) as u64)) != 0 {
            if Vma::vma_validate_pte_rx(vma_ptr, pte).is_err() {
                return Err(Errno::Fault);
            }
        }
        Ok(())
    }

    /// C-ABI wrapper for `vma_validate_pte`. Was previously the C
    /// `xv6_vm_validate_pte` shim in vm.c.
    pub(crate) fn xv6_vm_validate_pte(vma_ptr: *mut vma, pte: *mut u64, flags: u64) -> c_int {
        // C-ABI boundary: convert `Result<(), Errno>` to a negative-errno
        // `c_int` exactly once, here.
        crate::mm::cffi::result_to_neg_errno(Vma::vma_validate_pte(vma_ptr, pte, flags))
    }

    /// C-ABI wrapper for the file-fault dispatch. Was previously the C
    /// `xv6_vm_call_vma_fault` shim in vm.c. If the VMA's file provides a
    /// custom `.fault` op, dispatch there; otherwise fall back to the generic
    /// file-page loader.
    pub(crate) fn xv6_vm_call_vma_fault(
        file: *mut vfs_file,
        vma_ptr: *mut vma,
        va: u64,
    ) -> *mut c_void {
        if !file.is_null() {
            // SAFETY: `file` was just checked non-null and is a live
            // `vfs_file`; `ops` (P3-10a: `Option<&'static dyn FileOps>`)
            // when present is a live trait object whose `fault` is called
            // with the same `(file, vma_ptr, va)` triple the old fn-pointer
            // slot received. `fault`'s `Some(page)` means the driver
            // handled the fault (`page` may be null = driver failure,
            // returned as-is); `None` — the trait default, mirroring the
            // old `None` table slot — falls through to the generic loader.
            unsafe {
                if let Some(ops) = (*file).ops {
                    if let Some(page) = ops.fault(file, vma_ptr, va) {
                        return page;
                    }
                }
            }
        }
        Vma::vma_fault_file_page(vma_ptr, va)
    }
}

// ===========================================================================
// impl Vm — KERNEL-OO wave: former `vm_*`/`xv6_vm_*` free fns namespaced as
// associated fns (8283168 strategy: bodies byte-identical, raw `*mut vm`
// params kept -- no `&Vm`/`&mut Vm` of the shared pointee is formed, so the
// freeze/noalias hazard documented in `vm_wlock`'s guards above does not
// apply here).
// ===========================================================================
impl Vm {
    #[inline]
    fn xv6_vm_current_vm() -> *mut vm {
        match ThreadRef::from_raw(current_thread()) {
            None => core::ptr::null_mut(),
            Some(t) => t.vm_ptr() as *mut vm,
        }
    }

    #[inline]
    fn xv6_vm_first_vma(vm_ptr: *mut vm) -> *mut vma {
        first_in_list(machine::vm_list_ptr(vm_ptr), core::mem::offset_of!(vma, list_entry))
    }

    #[inline]
    fn xv6_vm_next_vma(vm_ptr: *mut vm, cur: *mut vma) -> *mut vma {
        next_in_list(
            machine::vm_list_ptr(vm_ptr),
            machine::vma_list_entry_ptr(cur),
            core::mem::offset_of!(vma, list_entry),
        )
    }

    #[inline]
    fn xv6_vm_last_free(vm_ptr: *mut vm) -> *mut vma {
        last_in_list(
            machine::vm_free_list_ptr(vm_ptr),
            core::mem::offset_of!(vma, free_list_entry),
        )
    }

    #[inline]
    fn xv6_vm_prev_free(vm_ptr: *mut vm, cur: *mut vma) -> *mut vma {
        prev_in_list(
            machine::vm_free_list_ptr(vm_ptr),
            machine::vma_free_list_entry_ptr(cur),
            core::mem::offset_of!(vma, free_list_entry),
        )
    }

    /// Forward iterator over `vm`'s VMA list (`vm_list` linkage), yielding each
    /// live `*mut vma`. Read-only walk: the caller must hold the vm lock and must
    /// not free/detach the yielded vma during iteration (the successor link is
    /// read lazily, so freeing the current node would invalidate the walk — the
    /// `__vm_destroy` teardown, which frees each vma, deliberately keeps its
    /// hand-rolled capture-next loop). Mirrors `xv6_vm_first_vma`/`xv6_vm_next_vma`.
    fn vm_vma_iter(vm_ptr: *mut vm) -> impl Iterator<Item = *mut vma> {
        core::iter::successors(
            Some(Vm::xv6_vm_first_vma(vm_ptr)).filter(|p| !p.is_null()),
            move |&cur| {
                let next = Vm::xv6_vm_next_vma(vm_ptr, cur);
                (!next.is_null()).then_some(next)
            },
        )
    }

    /// Reverse iterator over `vm`'s free VMA list (`free_list_entry` linkage),
    /// yielding each live `*mut vma` from last to first. Same read-only /
    /// don't-detach contract as [`vm_vma_iter`]. Mirrors
    /// `xv6_vm_last_free`/`xv6_vm_prev_free`.
    fn vm_free_list_rev_iter(vm_ptr: *mut vm) -> impl Iterator<Item = *mut vma> {
        core::iter::successors(
            Some(Vm::xv6_vm_last_free(vm_ptr)).filter(|p| !p.is_null()),
            move |&cur| {
                let prev = Vm::xv6_vm_prev_free(vm_ptr, cur);
                (!prev.is_null()).then_some(prev)
            },
        )
    }

    #[inline]
    fn xv6_vm_list_push_back_list_entry(vm_ptr: *mut vm, v: *mut vma) {
        // push_back = insert before head, i.e. insert after head->prev.
        let head = machine::vm_list_ptr(vm_ptr);
        list_entry_insert_after(machine::list_entry_prev(head), machine::vma_list_entry_ptr(v));
    }

    #[inline]
    fn xv6_vm_list_push_back_free_list_entry(vm_ptr: *mut vm, v: *mut vma) {
        let head = machine::vm_free_list_ptr(vm_ptr);
        list_entry_insert_after(machine::list_entry_prev(head), machine::vma_free_list_entry_ptr(v));
    }

    #[inline]
    fn xv6_vm_list_push_front_free_list_entry(vm_ptr: *mut vm, v: *mut vma) {
        let head = machine::vm_free_list_ptr(vm_ptr);
        list_entry_insert_after(head, machine::vma_free_list_entry_ptr(v));
    }

    #[inline]
    fn xv6_vm_assert_vm_write_held(vm_ptr: *mut vm, msg: *const c_char) {
        if current_thread().is_null() { return; }
        if rwsem_is_write_holding(machine::vm_rw_lock_ptr(vm_ptr)) { return; }
        __panic_start();
        if msg.is_null() {
            crate::kprintln!("PANIC: vm rwsem must be write-held");
        } else {
            crate::kprintln!("PANIC: {}", crate::printf::Cs(msg));
        }
        __panic_end();
    }

    /// Acquire-load of `vm->cpumask` (cpumask_t == u64).
    pub(crate) fn vm_get_cpumask(vm_ptr: *mut vm) -> u64 {
        if vm_ptr.is_null() {
            return 0;
        }
        // SAFETY: `vm_ptr` was checked non-null; caller supplies a live `vm`.
        // `cpumask` is accessed via an `AtomicU64`-typed read matching the C
        // side's `__atomic_load_n`.
        unsafe {
            let cpumask = &(*vm_ptr).cpumask as *const u64 as *const AtomicU64;
            (*cpumask).load(Ordering::Acquire)
        }
    }

    /// Mark a CPU as using this VM. Updates the per-CPU trapframe PTE if a
    /// trapframe is currently registered for the calling thread. Returns the
    /// trapframe base virtual address for `cpu`.
    pub(crate) fn vm_cpu_online(vm_ptr: *mut vm, cpu: c_int) -> u64 {
        // SAFETY: every caller passes a live `vm_ptr` (a `vm` owned by some
        // thread group); `trapframe_pte`, when non-null, points at a live PTE
        // array sized for `NCPU` trapframe slots, and `pte_idx` is derived
        // from `cpu`, which the caller guarantees is `< NCPU`.
        unsafe {
            // Bindgen sees the C `#define TRAPFRAME_POFFSET` as either a const or
            // a macro depending on the header. The value is the byte offset within
            // a trapframe page (0 for a page-aligned trapframe). The fall-back
            // default we use here matches the original C source: keep the offset
            // of `p->trapframe` within its physical page.
            let mut trapframe_poffset: u64 = 0;
            let trapframe_pte = (*vm_ptr).trapframe_pte;
            let trapframe_pa = xv6_vm_current_trapframe();
            if !trapframe_pte.is_null() && trapframe_pa != 0 {
                let pte_idx = xv6_vm_px0(TRAPFRAME as u64 + (cpu as u64) * (PGSIZE as u64)) as usize;
                let trapframe_pa_aligned = trapframe_pa & !((PGSIZE as u64) - 1);
                // PA2PTE(pa) = (pa >> 12) << 10
                let pte_val: u64 = ((trapframe_pa_aligned >> 12) << 10)
                    | (PTE_R | PTE_W | PTE_V | PTE_A | PTE_D) as u64;
                *trapframe_pte.add(pte_idx) = pte_val;
                // Ensure PTE write is visible before page-table switch.
                compiler_fence(Ordering::Release);
                fence(Ordering::Release);
                trapframe_poffset = trapframe_pa & ((PGSIZE as u64) - 1);
            }
            let cpumask = &(*vm_ptr).cpumask as *const u64 as *const AtomicU64;
            // Release: aligned with the `Acquire` loads in `vm_get_cpumask`/
            // `vm_remote_sfence` — publishes both the PTE write above and
            // the new cpumask bit to any hart that subsequently observes
            // this bit set via an `Acquire` load. The RMW's own read side
            // needs no ordering (the previous mask value is discarded).
            (*cpumask).fetch_or(1u64 << cpu, Ordering::Release);
            TRAPFRAME as u64 + (cpu as u64) * (PGSIZE as u64) + trapframe_poffset
        }
    }

    /// Clear the calling CPU from `vm->cpumask`.
    pub(crate) fn vm_cpu_offline(vm_ptr: *mut vm, cpu: c_int) {
        if vm_ptr.is_null() {
            return;
        }
        // SAFETY: `vm_ptr` checked non-null above; `cpumask` is read/modified
        // through an `AtomicU64`-typed pointer, matching the C side's atomic
        // access to the same field.
        unsafe {
            let cpumask = &(*vm_ptr).cpumask as *const u64 as *const AtomicU64;
            // Release: aligned with the `Acquire` loads in `vm_get_cpumask`/
            // `vm_remote_sfence` — publishes this hart's completed use of
            // the vm's page tables before the bit clear becomes visible.
            (*cpumask).fetch_and(!(1u64 << cpu), Ordering::Release);
        }
    }

    /// Send remote `sfence.vma` to all CPUs currently using this VM (other than
    /// the calling CPU). Disables interrupts around the SBI call to keep the
    /// `cpuid()` read coherent with the cpumask read.
    pub(crate) fn vm_remote_sfence(vm_ptr: *mut vm) {
        if vm_ptr.is_null() {
            return;
        }
        let _preempt = machine::PreemptGuard::new();
        // Kept at SeqCst (not downgraded to match the subsequent Acquire
        // load): this orders the preceding `cpuid()` read (via the
        // doc comment's "keep the cpuid() read coherent with the cpumask
        // read" contract) against the mask load below with a hardware
        // fence rather than relying on data/control dependencies alone —
        // matches the C source's explicit `smp_mb()` here.
        fence(Ordering::SeqCst);
        // SAFETY: `vm_ptr` checked non-null above; `cpumask` is read through an
        // `AtomicU64`-typed pointer, matching the C side's atomic access.
        let mut mask = unsafe {
            let cpumask = &(*vm_ptr).cpumask as *const u64 as *const AtomicU64;
            (*cpumask).load(Ordering::Acquire)
        };
        mask &= !(1u64 << xv6_vm_cpuid());
        if mask != 0 {
            xv6_vm_sbi_remote_hfence_vma(mask);
        }
    }

    #[inline(always)]
    fn vm_pool_ptr() -> *mut slab_cache_t {
        __VM_POOL.as_ptr()
    }

    /// Initialise the slab pool that backs `vm_t` objects.
    pub(crate) fn __vm_pool_init() {
        // SAFETY: see `__vma_pool_init`.
        unsafe {
            slab_cache_init(
                Vm::vm_pool_ptr(),
                b"vm\0".as_ptr() as *mut c_char,
                core::mem::size_of::<vm>(),
                SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP,
            );
        }
    }

    /// Tear down the per-CPU trapframe leaf PTEs without freeing the physical
    /// trapframe pages (those are owned by the kernel-stack allocator).
    pub(crate) fn __vm_unmap_trapframe(vm_ptr: *mut vm) {
        // SAFETY: `vm_ptr`/`trapframe_pte`, when non-null (checked first thing
        // below), point at a live `vm` and a live `NCPU`-sized leaf PTE array
        // installed by `__vm_map_trampoline`.
        unsafe {
            if vm_ptr.is_null() || (*vm_ptr).pagetable.is_null() || (*vm_ptr).trapframe_pte.is_null() {
                return;
            }
            let leaf = (*vm_ptr).trapframe_pte as *mut u64;
            for i in 0..(NCPU as u64) {
                let pte_idx = xv6_vm_px0(TRAPFRAME as u64 + i * (PGSIZE as u64)) as usize;
                *leaf.add(pte_idx) = 0;
            }
        }
    }

    /// Walk the pagetable down to the leaf table covering `TRAPFRAME`, allocating
    /// any missing intermediate page-tables. Stores the leaf base pointer in
    /// `vm->trapframe_pte`. Returns 0 on success or `-ENOMEM` on OOM.
    pub(crate) fn __vm_map_trampoline(vm_ptr: *mut vm) -> c_int {
        // SAFETY: `vm_ptr`, when non-null (checked first thing below), is a
        // live `vm` with a live `pagetable`.
        unsafe {
            if vm_ptr.is_null() || (*vm_ptr).pagetable.is_null() {
                return -(EINVAL as c_int);
            }
            let pte = crate::mm::vm_pgtab::PageTable::walk(
                (*vm_ptr).pagetable as *mut u64,
                TRAPFRAME as u64,
                1,
                core::ptr::null_mut(),
                core::ptr::null_mut(),
            );
            if pte.is_null() {
                return -(ENOMEM as c_int);
            }
            (*vm_ptr).trapframe_pte = pg_round_down(pte as u64) as *mut u64;
            0
        }
    }

    /// Increment the VM reference count. Equivalent to the C macro
    /// `atomic_inc(&vm->refcount)`. The C side uses a non-atomic `int`
    /// field that is updated through atomic GCC builtins; treating the
    /// storage as `AtomicI32` is layout-compatible.
    pub(crate) fn vm_dup(vm_ptr: *mut vm) {
        if vm_ptr.is_null() {
            return;
        }
        // SAFETY: `vm_ptr` checked non-null above; `refcount` is accessed
        // through an `AtomicI32`-typed pointer, matching the C side's atomic
        // builtin access to the same field.
        unsafe {
            let refcount =
                &(*vm_ptr).refcount as *const c_int as *const core::sync::atomic::AtomicI32;
            (*refcount).fetch_add(1, Ordering::SeqCst);
        }
    }

    /// Acquire VM read lock for VMA-tree traversal (rwsem, sleepable).
    pub(crate) fn vm_rlock(vm_ptr: *mut vm) {
        // SAFETY: caller supplies a live `vm` (C-ABI lock entry point; the
        // paired `vm_runlock` is always called before `vm_ptr` is freed).
        unsafe { rwsem_acquire_read(&mut (*vm_ptr).rw_lock as *mut rwsem_t) };
    }

    /// Release VM read lock.
    pub(crate) fn vm_runlock(vm_ptr: *mut vm) {
        // SAFETY: caller supplies a live `vm` currently read-locked by `vm_rlock`.
        unsafe { rwsem_release(&mut (*vm_ptr).rw_lock as *mut rwsem_t) };
    }

    /// Acquire VM write lock for VMA-tree modification (rwsem, sleepable).
    pub(crate) fn vm_wlock(vm_ptr: *mut vm) {
        // SAFETY: caller supplies a live `vm`.
        unsafe { rwsem_acquire_write(&mut (*vm_ptr).rw_lock as *mut rwsem_t) };
    }

    /// Release VM write lock.
    pub(crate) fn vm_wunlock(vm_ptr: *mut vm) {
        // SAFETY: caller supplies a live `vm` currently write-locked by `vm_wlock`.
        unsafe { rwsem_release(&mut (*vm_ptr).rw_lock as *mut rwsem_t) };
    }

    /// Acquire VM page-table spinlock for PTE modifications.
    /// Caller must not sleep while holding this.
    pub(crate) fn vm_pgtable_lock(vm_ptr: *mut vm) {
        // SAFETY: caller supplies a live `vm`.
        unsafe { spin_lock(&mut (*vm_ptr).spinlock as *mut spinlock_t) };
    }

    /// Release VM page-table spinlock.
    pub(crate) fn vm_pgtable_unlock(vm_ptr: *mut vm) {
        // SAFETY: caller supplies a live `vm` currently locked by `vm_pgtable_lock`.
        unsafe { spin_unlock(&mut (*vm_ptr).spinlock as *mut spinlock_t) };
    }

    fn rlock_vm<'a>(vm_ptr: *mut vm) -> VmReadGuard<'a> {
        Vm::vm_rlock(vm_ptr);
        VmReadGuard { vm_ptr, _marker: PhantomData }
    }

    fn wlock_vm<'a>(vm_ptr: *mut vm) -> VmWriteGuard<'a> {
        Vm::vm_wlock(vm_ptr);
        VmWriteGuard { vm_ptr, _marker: PhantomData }
    }

    fn lock_vm_pgtable<'a>(vm_ptr: *mut vm) -> VmPgtableGuard<'a> {
        Vm::vm_pgtable_lock(vm_ptr);
        VmPgtableGuard { vm_ptr, _marker: PhantomData }
    }

    /// Destroy a VM: free all VMAs, unmap trapframe / pagetable, then slab-free.
    /// Direct port of the C `__vm_destroy` static helper; `vm_put` is the
    /// only caller and lives in the same crate.
    pub(crate) fn __vm_destroy(vm_ptr: *mut vm) {
        // SAFETY: `vm_ptr`, when non-null (checked first thing below), is a
        // live `vm` about to be torn down; every `vma` reached through
        // `xv6_vm_first_vma`/`xv6_vm_next_vma` belongs to this `vm`'s own
        // `vm_list`.
        unsafe {
            if vm_ptr.is_null() {
                return;
            }
            // Double-destroy detection.
            let poisoned = VM_DESTROYED_MAGIC as *mut u64;
            if (*vm_ptr).pagetable == poisoned {
                xv6_vm_warn_double_destroy(vm_ptr as *mut c_void);
                return;
            }

            // Iterate vm->vm_list, freeing each vma. Use the safe-iteration shims
            // (xv6_vm_first_vma / xv6_vm_next_vma) so we capture the next pointer
            // before mutating the current entry.
            let mut cur = Vm::xv6_vm_first_vma(vm_ptr);
            while !cur.is_null() {
                let next = Vm::xv6_vm_next_vma(vm_ptr, cur);

                // Sanity: verify vma belongs to this vm.
                if (*cur).vm != vm_ptr {
                    xv6_vm_warn_vma_vm_mismatch(
                        cur as *mut c_void,
                        (*cur).vm as *mut c_void,
                        vm_ptr as *mut c_void,
                    );
                    cur = next;
                    continue;
                }
                if (*cur).start == VMA_FREED_MAGIC || (*cur).end == VMA_FREED_MAGIC {
                    xv6_vm_warn_vma_already_freed(cur as *mut c_void);
                    cur = next;
                    continue;
                }
                Vma::__vma_set_free(cur);
                Vma::__vma_free(cur);
                cur = next;
            }

            // Reset bookkeeping containers.
            xv6_vm_list_entry_init(&mut (*vm_ptr).vm_list as *mut list_node_t);
            xv6_vm_list_entry_init(&mut (*vm_ptr).vm_free_list as *mut list_node_t);
            xv6_vm_rb_init_vm_tree(&mut (*vm_ptr).vm_tree as *mut crate::bindings::rb_root);

            if !(*vm_ptr).trapframe_pte.is_null() {
                Vm::__vm_unmap_trapframe(vm_ptr);
            }
            if !(*vm_ptr).pagetable.is_null() {
                PageTable::uvmfree((*vm_ptr).pagetable, 0);
            }
            // Poison before freeing.
            (*vm_ptr).pagetable = poisoned;
            slab_free(vm_ptr as *mut c_void);
        }
    }

    /// Decrement the VM reference count; destroy when the last reference drops.
    ///
    /// The C macro `atomic_dec_unless(&refcount, 1)` returns true if it
    /// successfully decremented (i.e. the value was != 1) and false otherwise.
    /// We model the same control flow with a CAS loop over `AtomicI32`.
    pub(crate) fn vm_put(vm_ptr: *mut vm) {
        if vm_ptr.is_null() {
            return;
        }
        // SAFETY: `vm_ptr` checked non-null above; `refcount` is accessed
        // through an `AtomicI32`-typed reference, matching the C side's atomic
        // builtin access to the same field, and stays live for as long as this
        // function holds a reference to it (the CAS loop below only reads/CASes
        // the count, never frees `vm_ptr` itself).
        let atomic = unsafe {
            &*(&(*vm_ptr).refcount as *const c_int as *const core::sync::atomic::AtomicI32)
        };

        let mut cur = atomic.load(Ordering::Relaxed);
        loop {
            if cur == 1 {
                // Last reference: do not decrement, destroy instead.
                Vm::__vm_destroy(vm_ptr);
                return;
            }
            match atomic.compare_exchange_weak(cur, cur - 1, Ordering::AcqRel, Ordering::Relaxed) {
                Ok(_) => return,
                Err(observed) => cur = observed,
            }
        }
    }

    /// Allocate and initialise a new VM.
    ///
    /// On any failure the partially-constructed VM is torn down via
    /// `__vm_destroy` and the function returns `null`.
    pub(crate) fn vm_init() -> *mut vm {
        // SAFETY: `raw`, when non-null (checked first thing below), is a
        // fresh, exclusively-owned `size_of::<vm>()`-sized allocation from
        // `Vm::vm_pool_ptr()`; every subsequent field write/read is into that
        // allocation (or, for `vma_ptr`, a freshly `__vma_alloc`'d `vma`).
        unsafe {
            let raw = slab_alloc(Vm::vm_pool_ptr());
            if raw.is_null() {
                return core::ptr::null_mut();
            }
            let vm_ptr = raw as *mut vm;

            // memset(vm, 0, sizeof(vm_t))
            core::ptr::write_bytes(raw as *mut u8, 0, core::mem::size_of::<vm>());

            // rb_root_init(&vm->vm_tree, &__vm_tree_opts);
            xv6_vm_rb_init_vm_tree(&mut (*vm_ptr).vm_tree as *mut crate::bindings::rb_root);
            xv6_vm_list_entry_init(&mut (*vm_ptr).vm_list as *mut list_node_t);
            xv6_vm_list_entry_init(&mut (*vm_ptr).vm_free_list as *mut list_node_t);

            let vma_ptr = Vma::__vma_alloc(vm_ptr);
            if vma_ptr.is_null() {
                Vm::__vm_destroy(vm_ptr);
                return core::ptr::null_mut();
            }

            // Set up the initial whole-user VMA so __vm_destroy can clean up on
            // later error paths.
            (*vma_ptr).start = UVMBOTTOM as u64;
            (*vma_ptr).end = UVMTOP as u64;
            rb_insert_color(
                &mut (*vm_ptr).vm_tree as *mut crate::bindings::rb_root,
                &mut (*vma_ptr).rb_entry as *mut rb_node,
            );
            Vm::xv6_vm_list_push_back_free_list_entry(vm_ptr, vma_ptr);
            Vm::xv6_vm_list_push_back_list_entry(vm_ptr, vma_ptr);

            (*vm_ptr).pagetable = PageTable::uvmcreate();
            if (*vm_ptr).pagetable.is_null() {
                Vm::__vm_destroy(vm_ptr);
                return core::ptr::null_mut();
            }
            if Vm::__vm_map_trampoline(vm_ptr) != 0 {
                Vm::__vm_destroy(vm_ptr);
                return core::ptr::null_mut();
            }
            spin_init(
                &mut (*vm_ptr).spinlock as *mut spinlock_t,
                VM_PGTABLE_LOCK_NAME.as_ptr() as *mut c_char,
            );
            rwsem_init(
                &mut (*vm_ptr).rw_lock as *mut rwsem_t,
                RWLOCK_PRIO_READ as u64,
                VM_RW_LOCK_NAME.as_ptr() as *const c_char,
            );
            (*vm_ptr).refcount = 1;
            vm_ptr
        }
    }

    /// Find the VMA containing virtual address `va` in `vm`'s rb-tree.
    /// Returns `null` if `va` is out of bounds or not mapped.
    pub(crate) fn vm_find_area(vm_ptr: *mut vm, va: u64) -> *mut vma {
        if vm_ptr.is_null() || va >= UVMTOP as u64 || va < UVMBOTTOM as u64 {
            return core::ptr::null_mut();
        }
        // SAFETY: `vm_ptr` checked non-null above; caller supplies a live `vm`.
        let node = unsafe {
            rb_find_key_rdown(&mut (*vm_ptr).vm_tree as *mut crate::bindings::rb_root, va)
        };
        if node.is_null() {
            return core::ptr::null_mut();
        }
        // `container_of(node, vma_t, rb_entry)` — safe pointer arithmetic.
        let offset = core::mem::offset_of!(vma, rb_entry);
        let vma_ptr: *mut vma = crate::mm::cffi::container_of(node, offset);
        // SAFETY: `node` came from the rb-tree rooted at `vm->vm_tree`, which
        // only ever stores `vma::rb_entry` nodes, so `vma_ptr` is a live `vma`.
        unsafe {
            if va < (*vma_ptr).start || va >= (*vma_ptr).end {
                // Same defensive panic as the C assert.
                xv6_vm_panic(b"vm_find_area: va not in range\0".as_ptr() as *const c_char);
            }
        }
        vma_ptr
    }

    /// Find a free range of `size` bytes inside `vm` between heap-end and
    /// (stack-bottom - 16 * PGSIZE), searching the free-list back-to-front and
    /// allocating from the top of the first big-enough free area. Returns the
    /// chosen start address (page-aligned) or 0 if no fit.
    pub(crate) fn vm_find_free_range(vm_ptr: *mut vm, size: usize, _hint: u64) -> u64 {
        if vm_ptr.is_null() || size == 0 {
            return 0;
        }
        // SAFETY: `vm_ptr` checked non-null above; `vm_ptr->stack`/`heap`, when
        // non-null, are live `vma`s owned by this `vm`; the free-list walk via
        // `xv6_vm_last_free`/`xv6_vm_prev_free` only ever yields `vma`s linked
        // into this same `vm`'s free list.
        unsafe {
            let size = pg_round_up(size as u64);

            let stack_bottom = if !(*vm_ptr).stack.is_null() {
                (*(*vm_ptr).stack).start
            } else {
                (UVMTOP as u64).wrapping_sub(PGSIZE as u64)
            };
            let heap_end = if !(*vm_ptr).heap.is_null() {
                (*(*vm_ptr).heap).start + (*vm_ptr).heap_size as u64
            } else {
                UVMBOTTOM as u64
            };

            let search_top = stack_bottom.wrapping_sub(16u64 * PGSIZE as u64);
            let search_bottom = heap_end;

            if search_top <= search_bottom + size {
                return 0;
            }

            // Walk vm->vm_free_list in reverse.
            for cur in Vm::vm_free_list_rev_iter(vm_ptr) {
                if (*cur).flags != PROT_NONE as u64 {
                    continue;
                }

                let mut usable_start = (*cur).start;
                let mut usable_end = (*cur).end;
                if usable_start < search_bottom {
                    usable_start = search_bottom;
                }
                if usable_end > search_top {
                    usable_end = search_top;
                }

                if usable_end > usable_start && usable_end - usable_start >= size {
                    let result = pg_round_down(usable_end - size);
                    if result >= usable_start {
                        return result;
                    }
                }
            }
            0
        }
    }

    pub(crate) fn vm_copyout(
        vm_ptr: *mut vm,
        mut dstva: u64,
        src_in: *const c_void,
        len_in: u64,
    ) -> c_int {
        // SAFETY: `vm_ptr` is a live `vm` (read-locked for the duration via
        // `_g`); every `pte`/`pa0` used below comes from `PageTable::walk()` into that
        // `vm`'s pagetable after `vma_validate` has just faulted the page in,
        // and `src` only ever advances by the `n` bytes just proven in-range.
        unsafe {
            let mut len = len_in;
            let mut src = src_in;
            let mut ret: c_int = 0;
            let _g = Vm::rlock_vm(vm_ptr);
            while len > 0 {
                let va0 = pg_round_down(dstva);
                if va0 >= MAXVA as u64 {
                    ret = -(EFAULT as c_int);
                    break;
                }
                let vma = Vm::vm_find_area(vm_ptr, va0);
                if vma.is_null()
                    || Vma::vma_validate(vma, va0, PGSIZE as u64, (VMA_FLAG_USER | PROT_WRITE) as u64) != 0
                {
                    crate::kprintln!("vma_copyout: invalid vma for va {:x}", va0 as u64);
                    ret = -(EFAULT as c_int);
                    break;
                }
                let pte = PageTable::walk(_g.pagetable as *mut u64, va0, 0, core::ptr::null_mut(), core::ptr::null_mut());
                if pte.is_null() {
                    xv6_vm_panic(b"vma_copyout: pte should not be null\0".as_ptr() as *const c_char);
                }
                let pa0 = ((*pte) >> 10) << 12;
                let mut n = (PGSIZE as u64) - (dstva - va0);
                if n > len {
                    n = len;
                }
                memmove(
                    (pa0 + (dstva - va0)) as *mut c_void,
                    src,
                    n as usize,
                );
                len -= n;
                src = src.add(n as usize);
                dstva = va0 + (PGSIZE as u64);
            }
            ret
        }
    }

    pub(crate) fn vm_copyin(
        vm_ptr: *mut vm,
        dst_in: *mut c_void,
        mut srcva: u64,
        len_in: u64,
    ) -> c_int {
        // SAFETY: same reasoning as `vm_copyout`, mirrored for the read
        // direction.
        unsafe {
            let mut len = len_in;
            let mut dst = dst_in;
            let mut ret: c_int = 0;
            let _g = Vm::rlock_vm(vm_ptr);
            while len > 0 {
                let va0 = pg_round_down(srcva);
                let vma = Vm::vm_find_area(vm_ptr, va0);
                if vma.is_null()
                    || Vma::vma_validate(vma, va0, PGSIZE as u64, (VMA_FLAG_USER | PROT_READ) as u64) != 0
                {
                    ret = -(EFAULT as c_int);
                    break;
                }
                let pa0 = PageTable::walkaddr(_g.pagetable as *mut u64, va0);
                if pa0 == 0 {
                    ret = -(EFAULT as c_int);
                    break;
                }
                let mut n = (PGSIZE as u64) - (srcva - va0);
                if n > len {
                    n = len;
                }
                memmove(dst, (pa0 + (srcva - va0)) as *const c_void, n as usize);
                len -= n;
                dst = dst.add(n as usize);
                srcva = va0 + (PGSIZE as u64);
            }
            ret
        }
    }

    pub(crate) fn vm_copyinstr(
        vm_ptr: *mut vm,
        dst_in: *mut c_char,
        mut srcva: u64,
        max_in: u64,
    ) -> c_int {
        // SAFETY: same reasoning as `vm_copyin`; the inner byte-at-a-time loop
        // only reads/writes within the just-validated `n`-byte page window.
        unsafe {
            let mut dst = dst_in;
            let mut max = max_in;
            let mut got_null = false;
            let mut ret: c_int = 0;
            let _g = Vm::rlock_vm(vm_ptr);
            while !got_null && max > 0 {
                let va0 = pg_round_down(srcva);
                let vma = Vm::vm_find_area(vm_ptr, va0);
                if vma.is_null()
                    || Vma::vma_validate(vma, va0, PGSIZE as u64, (VMA_FLAG_USER | PROT_READ) as u64) != 0
                {
                    ret = -(EFAULT as c_int);
                    break;
                }
                let pa0 = PageTable::walkaddr(_g.pagetable as *mut u64, va0);
                if pa0 == 0 {
                    ret = -(EFAULT as c_int);
                    break;
                }
                let mut n = (PGSIZE as u64) - (srcva - va0);
                if n > max {
                    n = max;
                }
                let mut p = (pa0 + (srcva - va0)) as *mut c_char;
                while n > 0 {
                    if *p == 0 {
                        *dst = 0;
                        got_null = true;
                        break;
                    } else {
                        *dst = *p;
                    }
                    n -= 1;
                    max -= 1;
                    p = p.add(1);
                    dst = dst.add(1);
                }
                srcva = va0 + (PGSIZE as u64);
            }
            if !got_null && ret == 0 {
                ret = -(ENAMETOOLONG as c_int);
            }
            ret
        }
    }

    pub(crate) fn vm_createheap(vm_ptr: *mut vm, va: u64, size_in: u64) -> c_int {
        Vm::xv6_vm_assert_vm_write_held(
            vm_ptr,
            b"vm_createheap: vm rwsem must be write-held\0".as_ptr() as *const c_char,
        );
        let size = pg_round_up(size_in);
        if (va & (PGSIZE as u64 - 1)) != 0 {
            return -(EINVAL as c_int);
        }
        if va >= UVMTOP as u64 || va.wrapping_add(size) > UVMTOP as u64 {
            return -(EINVAL as c_int);
        }
        let flags = (PROT_READ | PROT_WRITE | VMA_FLAG_USER) as u64
            | crate::bindings::VMA_FLAG_GROWSUP as u64;
        let vma_ptr = Vma::vma_alloc(vm_ptr, va, size, flags);
        if vma_ptr.is_null() {
            return -(ENOMEM as c_int);
        }
        // SAFETY: `vm_ptr` is a live `vm` (asserted write-held above by the
        // caller's convention); `vma_ptr` was just checked non-null.
        unsafe {
            (*vm_ptr).heap = vma_ptr;
            (*vm_ptr).heap_size = size as usize;
        }
        0
    }

    pub(crate) fn vm_createstack(vm_ptr: *mut vm, stack_top: u64, size_in: u64) -> c_int {
        Vm::xv6_vm_assert_vm_write_held(
            vm_ptr,
            b"vm_createstack: vm rwsem must be write-held\0".as_ptr() as *const c_char,
        );
        let size = pg_round_up(size_in);
        if (stack_top & (PGSIZE as u64 - 1)) != 0 {
            return -(EINVAL as c_int);
        }
        if stack_top < size || stack_top > UVMTOP as u64 {
            return -(EINVAL as c_int);
        }
        let flags = (PROT_READ | PROT_WRITE | VMA_FLAG_USER) as u64
            | crate::bindings::VMA_FLAG_GROWSDOWN as u64;
        let vma_ptr = Vma::vma_alloc(vm_ptr, stack_top - size, size, flags);
        if vma_ptr.is_null() {
            return -(ENOMEM as c_int);
        }
        // SAFETY: `vm_ptr` is a live `vm`; `vma_ptr` was just checked non-null.
        unsafe {
            (*vm_ptr).stack = vma_ptr;
            (*vm_ptr).stack_size = size as usize;
        }
        0
    }

    pub(crate) fn vm_growstack(vm_ptr: *mut vm, change_size: i64) -> c_int {
        // SAFETY: `vm_ptr`, once past the null checks just below, is a live
        // `vm`; every `*mut vma` touched (`stack`, `left`, `right`, `grows`,
        // …) is checked non-null before dereference.
        unsafe {
            Vm::xv6_vm_assert_vm_write_held(
                vm_ptr,
                b"vm_growstack: vm rwsem must be write-held\0".as_ptr() as *const c_char,
            );
            if vm_ptr.is_null() || (*vm_ptr).pagetable.is_null() {
                return -(EINVAL as c_int);
            }
            if (*vm_ptr).stack.is_null() || (*vm_ptr).stack_size < PGSIZE as usize {
                return -(EINVAL as c_int);
            }
            if ((*(*vm_ptr).stack).flags & crate::bindings::VMA_FLAG_GROWSDOWN as u64) == 0 {
                return -(EINVAL as c_int);
            }
            if change_size == 0 {
                return 0;
            }
            let stack_size_u = (*vm_ptr).stack_size as u64;
            if change_size < 0 && (-change_size) as u64 > stack_size_u - (PGSIZE as u64) {
                return -(EINVAL as c_int);
            } else if change_size > 0
                && (change_size as u64) > ((MAXUSTACK as u64) << PGSHIFT) - stack_size_u
            {
                return -(ENOMEM as c_int);
            }
            let new_size = (stack_size_u as i64 + change_size) as u64;
            if new_size < PGSIZE as u64 || new_size > ((MAXUSTACK as u64) << PGSHIFT) {
                return -(EINVAL as c_int);
            }
            let delta: i64 = pg_round_up(new_size) as i64 - pg_round_up(stack_size_u) as i64;
            if delta == 0 {
                (*vm_ptr).stack_size = new_size as usize;
                return 0;
            }
            let left = Vma::xv6_vm_vma_left((*vm_ptr).stack);
            let new_start = (*(*vm_ptr).stack).start.wrapping_sub(delta as u64);

            if delta < 0 {
                let splitted = (*vm_ptr).stack;
                let right = Vma::vma_split((*vm_ptr).stack, new_start);
                if right.is_null() {
                    return -(ENOMEM as c_int);
                }
                (*vm_ptr).stack = right;
                Vma::__vma_set_free(splitted);
                if !left.is_null() && (*left).flags == PROT_NONE as u64 {
                    Vma::vma_merge(splitted, left);
                }
            } else {
                if left.is_null() || (*left).flags != PROT_NONE as u64 {
                    return -(ENOMEM as c_int);
                }
                if Vma::vma_size_of(left) < delta as u64 {
                    return -(ENOMEM as c_int);
                }
                let grows = Vma::vma_split(left, new_start);
                if grows.is_null() {
                    return -(ENOMEM as c_int);
                }
                Vma::xv6_vm_list_detach_free_list_entry(grows);
                (*grows).flags = (*(*vm_ptr).stack).flags;
                let new_stack = Vma::vma_merge(grows, (*vm_ptr).stack);
                (*vm_ptr).stack = new_stack;
            }
            (*vm_ptr).stack_size = new_size as usize;
            0
        }
    }

    pub(crate) fn vm_try_growstack(vm_ptr: *mut vm, va: u64) -> c_int {
        // Fully safe body: `_g` (a `VmWriteGuard`) carries the `vm` reference
        // for every field read below via `Deref`, and `machine::vma_start` is
        // the existing safe accessor for the one `vma` field (`stack.start`)
        // this function needs — `vm_growstack` re-derefs `vm_ptr` itself under
        // its own SAFETY contract (asserted write-held).
        let mut ret: c_int = 0;
        let _g = Vm::wlock_vm(vm_ptr);
        if vm_ptr.is_null() || _g.pagetable.is_null() {
            ret = -(EINVAL as c_int);
        } else if va < USTACK_MAX_BOTTOM as u64 || va >= USTACKTOP as u64 {
            ret = 0;
        } else if _g.stack.is_null() {
            ret = -(EINVAL as c_int);
        } else if machine::vma_start(_g.stack) <= va {
            ret = 0;
        } else {
            let ustack_bottom_after = machine::vma_start(_g.stack)
                .wrapping_sub((USERSTACK_GROWTH as u64) << PAGE_SHIFT);
            if ustack_bottom_after < USTACK_MAX_BOTTOM as u64 {
                ret = -(ENOMEM as c_int);
            } else if ustack_bottom_after > va {
                ret = -(EFAULT as c_int);
            } else {
                ret = Vm::vm_growstack(vm_ptr, ((USERSTACK_GROWTH as i64) << PAGE_SHIFT) as i64);
            }
        }
        ret
    }

    pub(crate) fn vm_growheap(vm_ptr: *mut vm, change_size: i64) -> c_int {
        let mut ret: c_int = 0;
        let mut _g = Vm::wlock_vm(vm_ptr);
        'done: {
            if vm_ptr.is_null() || _g.pagetable.is_null() {
                ret = -(EINVAL as c_int);
                break 'done;
            }
            if _g.heap.is_null() || _g.heap_size < PGSIZE as usize {
                ret = -(EINVAL as c_int);
                break 'done;
            }
            if (machine::vma_flags(_g.heap) & crate::bindings::VMA_FLAG_GROWSUP as u64) == 0 {
                ret = -(EINVAL as c_int);
                break 'done;
            }
            if change_size == 0 {
                ret = 0;
                break 'done;
            }
            let heap_size_u = _g.heap_size as u64;
            if change_size < 0 {
                if (-change_size) as u64 > heap_size_u - PGSIZE as u64 {
                    ret = -(EINVAL as c_int);
                    break 'done;
                }
            } else if (change_size as u64) > (UHEAP_MAX_TOP as u64) - machine::vma_end(_g.heap) {
                ret = -(ENOMEM as c_int);
                break 'done;
            }
            let new_size = (heap_size_u as i64 + change_size) as u64;
            let delta: i64 = pg_round_up(new_size) as i64 - Vma::vma_size_of(_g.heap) as i64;
            if delta == 0 {
                _g.heap_size = new_size as usize;
                ret = 0;
                break 'done;
            }
            let new_end = machine::vma_end(_g.heap).wrapping_add(delta as u64);
            let right = Vma::xv6_vm_vma_right(_g.heap);

            if delta < 0 {
                let splitted = Vma::vma_split(_g.heap, new_end);
                if splitted.is_null() {
                    ret = -(ENOMEM as c_int);
                    break 'done;
                }
                Vma::__vma_set_free(splitted);
                if !right.is_null() && machine::vma_flags(right) == PROT_NONE as u64 {
                    Vma::vma_merge(splitted, right);
                }
            } else {
                if right.is_null() || machine::vma_flags(right) != PROT_NONE as u64 {
                    ret = -(ENOMEM as c_int);
                    break 'done;
                }
                if Vma::vma_size_of(right) < delta as u64 {
                    ret = -(ENOMEM as c_int);
                    break 'done;
                }
                if Vma::vma_split(right, new_end).is_null() {
                    ret = -(ENOMEM as c_int);
                    break 'done;
                }
                Vma::xv6_vm_list_detach_free_list_entry(right);
                // SAFETY: `right` was just checked non-null above and is a
                // live `vma` freshly returned by `vma_split`/`xv6_vm_vma_right`
                // and not yet reachable from any other thread; there is no
                // safe `vma`-field setter for `flags` (only the getter
                // `machine::vma_flags` exists), so this one write stays a raw
                // deref.
                unsafe {
                    (*right).flags = machine::vma_flags(_g.heap);
                }
                let new_heap = Vma::vma_merge(right, _g.heap);
                _g.heap = new_heap;
            }
            _g.heap_size = new_size as usize;
            ret = 0;
        }
        ret
    }

    pub(crate) fn vm_copy(src: *mut vm) -> *mut vm {
        // Fully safe body: `_src_g`/`_dst_g` (a `VmReadGuard`/`VmWriteGuard`
        // pair) carry the `vm` references for every `src`/`dst` field access
        // below via `Deref`/`DerefMut`, and `machine::vma_start`/
        // `vma_flags` are the existing safe accessors for the `vma` fields
        // touched on `vma_cur`; `vma_alloc`/`__vma_copy`/`vm_put` all take and
        // return raw pointers without the caller needing to dereference them.
        if src.is_null() {
            return ((-(EINVAL as isize)) as isize) as *mut vm;
        }
        let dst = Vm::vm_init();
        if dst.is_null() {
            return ((-(ENOMEM as isize)) as isize) as *mut vm;
        }
        // `dst` was just allocated above and is not yet reachable from any
        // other thread, so acquiring `src`'s read lock then `dst`'s write
        // lock cannot deadlock against another thread's lock ordering —
        // `lock_two()` (sync.rs) is for two same-mode `KSpinlock`s and
        // doesn't fit this asymmetric rwsem read+write pair, so a plain
        // fixed-order acquire (matching the original) is used instead.
        let _src_g = Vm::rlock_vm(src);
        let mut _dst_g = Vm::wlock_vm(dst);

        // Read-only walk of `src`'s VMA list (src is read-locked; the body only
        // allocates into `dst`, never frees a `src` vma), so the lazy successor
        // iterator is stable.
        for vma_cur in Vm::vm_vma_iter(src) {
            if machine::vma_flags(vma_cur) != PROT_NONE as u64 {
                let new_vma = Vma::vma_alloc(
                    dst,
                    machine::vma_start(vma_cur),
                    Vma::vma_size_of(vma_cur),
                    machine::vma_flags(vma_cur),
                );
                if new_vma.is_null() {
                    drop(_dst_g);
                    drop(_src_g);
                    Vm::vm_put(dst);
                    return ((-(ENOMEM as isize)) as isize) as *mut vm;
                }
                if vma_cur == _src_g.stack {
                    _dst_g.stack = new_vma;
                    _dst_g.stack_size = _src_g.stack_size;
                } else if vma_cur == _src_g.heap {
                    _dst_g.heap = new_vma;
                    _dst_g.heap_size = _src_g.heap_size;
                }
                if Vma::__vma_copy(new_vma, vma_cur) != 0 {
                    drop(_dst_g);
                    drop(_src_g);
                    Vm::vm_put(dst);
                    return ((-(ENOMEM as isize)) as isize) as *mut vm;
                }
            }
        }
        dst
    }

    pub(crate) fn vm_mmap_region_locked(
        vm_ptr: *mut vm,
        start_in: u64,
        size_in: usize,
        flags: u64,
        file: *mut vfs_file,
        pgoff: u64,
        pa: *mut c_void,
    ) -> c_int {
        // SAFETY: `vm_ptr`, once past the null checks just below, is a live
        // `vm`; `vma_ptr` comes from `vma_alloc`, checked non-null before use.
        unsafe {
            if vm_ptr.is_null() || (*vm_ptr).pagetable.is_null() {
                return -(EINVAL as c_int);
            }
            let va_end = pg_round_up(start_in + size_in as u64);
            let start = pg_round_down(start_in);
            if va_end <= start || start < UVMBOTTOM as u64 || va_end > UVMTOP as u64 {
                return -(EINVAL as c_int);
            }
            let size = va_end - start;
            let vma_ptr = Vma::vma_alloc(vm_ptr, start, size, flags);
            if vma_ptr.is_null() {
                return -(ENOMEM as c_int);
            }
            if !file.is_null() {
                (*vma_ptr).file = VfsFile::vfs_fdup(file);
                if (*vma_ptr).file.is_null() {
                    Vma::vma_free(vm_ptr, vma_ptr);
                    return -(EBADF as c_int);
                }
                (*vma_ptr).pgoff = pgoff;
            }
            if !pa.is_null() {
                let pte_flags = vma2pte_flags(flags);
                if PageTable::mappages(
                    (*vm_ptr).pagetable as *mut u64,
                    (*vma_ptr).start,
                    size,
                    pa as u64,
                    pte_flags as c_int,
                ) != 0
                {
                    if Vma::vma_free(vm_ptr, vma_ptr) != 0 {
                        xv6_vm_panic(
                            b"vm_mmap_region_locked: failed to free vma\0".as_ptr() as *const c_char,
                        );
                    }
                    return -(ENOMEM as c_int);
                }
            }
            0
        }
    }

    pub(crate) fn vm_mmap_region(
        vm_ptr: *mut vm,
        start: u64,
        size: usize,
        flags: u64,
        file: *mut vfs_file,
        pgoff: u64,
        pa: *mut c_void,
    ) -> c_int {
        let has_current = xv6_vm_has_current() != 0;
        let _g = if has_current { Some(Vm::wlock_vm(vm_ptr)) } else { None };
        Vm::vm_mmap_region_locked(vm_ptr, start, size, flags, file, pgoff, pa)
    }

    pub(crate) fn vm_munmap_region(vm_ptr: *mut vm, start: u64, size: usize) -> c_int {
        // Fully safe body: `_g` carries the `vm` reference via `Deref`, and
        // `machine::vma_start`/`vma_end` are the existing safe accessors for
        // the `vma` fields `vma_ptr` needs; `vma_free` takes the raw pointer
        // without the caller dereferencing it.
        let mut ret: c_int = 0;
        let _g = Vm::wlock_vm(vm_ptr);
        'done: {
            if vm_ptr.is_null() || _g.pagetable.is_null() {
                ret = -(EINVAL as c_int);
                break 'done;
            }
            if start < UVMBOTTOM as u64 || start + size as u64 > UVMTOP as u64 {
                ret = -(EINVAL as c_int);
                break 'done;
            }
            if (size as u64 & (PGSIZE as u64 - 1)) != 0 || (start & (PGSIZE as u64 - 1)) != 0 {
                ret = -(EINVAL as c_int);
                break 'done;
            }
            if size == 0 {
                ret = 0;
                break 'done;
            }
            let vma_ptr = Vm::vm_find_area(vm_ptr, start);
            if vma_ptr.is_null()
                || machine::vma_start(vma_ptr) != start
                || machine::vma_end(vma_ptr) < start + size as u64
            {
                ret = -(EINVAL as c_int);
                break 'done;
            }
            if Vma::vma_free(vm_ptr, vma_ptr) != 0 {
                ret = -(EINVAL as c_int);
                break 'done;
            }
            ret = 0;
        }
        ret
    }

    pub(crate) fn vm_mprotect(
        vm_ptr: *mut vm,
        addr_in: u64,
        size_in: usize,
        prot: c_int,
    ) -> c_int {
        let mut ret: c_int = 0;
        let _g = Vm::wlock_vm(vm_ptr);
        // SAFETY: `vm_ptr`, once past the null checks just below, is a live
        // `vm`; `vma_ptr` comes from `vm_find_area`, checked non-null and
        // range-checked before use; every PTE pointer comes from `PageTable::walk()`.
        unsafe {
            'done: {
                if vm_ptr.is_null() || _g.pagetable.is_null() {
                    ret = -(EINVAL as c_int);
                    break 'done;
                }
                let addr = pg_round_down(addr_in);
                let size = pg_round_up(size_in as u64);
                if addr < UVMBOTTOM as u64 || addr + size > UVMTOP as u64 {
                    ret = -(ENOMEM as c_int);
                    break 'done;
                }
                if size == 0 {
                    ret = 0;
                    break 'done;
                }
                let vma_ptr = Vm::vm_find_area(vm_ptr, addr);
                if vma_ptr.is_null() {
                    ret = -(ENOMEM as c_int);
                    break 'done;
                }
            if addr < machine::vma_start(vma_ptr) || addr + size > machine::vma_end(vma_ptr) {
                ret = -(ENOMEM as c_int);
                break 'done;
            }
            let old_flags = machine::vma_flags(vma_ptr);
            let new_flags = (old_flags & !(PROT_MASK as u64)) | ((prot as u64) & PROT_MASK as u64);
            let pte_flags = vma2pte_flags(new_flags);

            {
                let _pgl = Vm::lock_vm_pgtable(vm_ptr);
                for va in (addr..addr + size).step_by(PGSIZE as usize) {
                    let pte = PageTable::walk(
                        _pgl.pagetable as *mut u64,
                        va,
                        0,
                        core::ptr::null_mut(),
                        core::ptr::null_mut(),
                    );
                    if !pte.is_null() && (*pte & PTE_V as u64) != 0 {
                        let pa = ((*pte) >> 10) << 12;
                        *pte = ((pa >> 12) << 10)
                            | pte_flags
                            | (PTE_V | PTE_A | PTE_D) as u64;
                    }
                }
            }

            if addr == machine::vma_start(vma_ptr) && addr + size == machine::vma_end(vma_ptr) {
                (*vma_ptr).flags = new_flags;
            }

            let prot_bits = (PROT_READ | PROT_WRITE | PROT_EXEC) as u64;
            if ((old_flags & prot_bits) & !(new_flags & prot_bits)) != 0 {
                Vm::vm_remote_sfence(vm_ptr);
            }
            ret = 0;
        }
        ret
        }
    }

    pub(crate) fn vm_mremap(
        vm_ptr: *mut vm,
        old_addr_in: u64,
        old_size_in: usize,
        new_size_in: usize,
        flags: c_int,
        _new_addr: u64,
    ) -> u64 {
        let mut ret: u64 = u64::MAX;
        let _g = Vm::wlock_vm(vm_ptr);
        // SAFETY: `vm_ptr`, once past the null checks just below, is a live
        // `vm`; every `*mut vma` touched (`vma_ptr`, `next_vma`, `new_vma`, …)
        // is checked non-null before dereference, and every PTE pointer comes
        // from `PageTable::walk()`.
        unsafe {
            'done: {
                if vm_ptr.is_null() || _g.pagetable.is_null() {
                    break 'done;
                }
                let old_addr = pg_round_down(old_addr_in);
                let old_size = pg_round_up(old_size_in as u64);
                let new_size = pg_round_up(new_size_in as u64);
                if old_addr < UVMBOTTOM as u64 || old_addr + old_size > UVMTOP as u64 {
                    break 'done;
                }
                let vma_ptr = Vm::vm_find_area(vm_ptr, old_addr);
                if vma_ptr.is_null()
                    || machine::vma_start(vma_ptr) != old_addr
                    || machine::vma_end(vma_ptr) != old_addr + old_size
                {
                    break 'done;
                }
                if new_size == 0 {
                    if Vma::vma_free(vm_ptr, vma_ptr) != 0 {
                        break 'done;
                    }
                    ret = old_addr;
                    break 'done;
                }
                if new_size == old_size && (flags & MREMAP_FIXED as c_int) == 0 {
                    ret = old_addr;
                    break 'done;
                }
                if new_size < old_size {
                    let shrink_start = old_addr + new_size;
                    let tail = Vma::vma_split(vma_ptr, shrink_start);
                    if tail.is_null() {
                        break 'done;
                    }
                    if Vma::vma_free(vm_ptr, tail) != 0 {
                        break 'done;
                    }
                    Vm::vm_remote_sfence(vm_ptr);
                    ret = old_addr;
                    break 'done;
                }
                // Expanding
                let expand_size = new_size - old_size;
                let expand_start = old_addr + old_size;
                let next_vma = Vm::vm_find_area(vm_ptr, expand_start);
                if next_vma.is_null() || machine::vma_start(next_vma) >= expand_start + expand_size {
                    (*vma_ptr).end = old_addr + new_size;
                    ret = old_addr;
                    break 'done;
                }
                if (flags & MREMAP_MAYMOVE as c_int) == 0 {
                    break 'done;
                }
                let new_location = Vm::vm_find_free_range(vm_ptr, new_size as usize, 0);
                if new_location == 0 {
                    break 'done;
                }
                let new_vma = Vma::vma_alloc(vm_ptr, new_location, new_size, machine::vma_flags(vma_ptr));
                if new_vma.is_null() {
                    break 'done;
                }
                for offset in (0..old_size).step_by(PGSIZE as usize) {
                    let old_pte = PageTable::walk(
                        _g.pagetable as *mut u64,
                        old_addr + offset,
                        0,
                        core::ptr::null_mut(),
                        core::ptr::null_mut(),
                    );
                    if !old_pte.is_null() && (*old_pte & PTE_V as u64) != 0 {
                        let pa = ((*old_pte) >> 10) << 12;
                        let pte_flags = *old_pte & ((PTE_R | PTE_W | PTE_X | PTE_U) as u64);
                        let new_pte = PageTable::walk(
                            _g.pagetable as *mut u64,
                            new_location + offset,
                            1,
                            core::ptr::null_mut(),
                            core::ptr::null_mut(),
                        );
                        if new_pte.is_null() {
                            break 'done;
                        }
                        *new_pte = ((pa >> 12) << 10) | pte_flags | PTE_V as u64;
                        crate::mm::page::Page::page_ref_inc(pa as *mut c_void);
                        *old_pte = 0;
                    }
                }
                Vma::vma_free(vm_ptr, vma_ptr);
                Vm::vm_remote_sfence(vm_ptr);
                ret = new_location;
            }
            ret
        }
    }

    pub(crate) fn vm_msync(
        vm_ptr: *mut vm,
        addr_in: u64,
        size_in: usize,
        _flags: c_int,
    ) -> c_int {
        // Fully safe body: `_g` carries the `vm` reference via `Deref`, and
        // `machine::vma_start`/`vma_end`/`vma_file` are the existing safe
        // accessors for the `vma` fields `vma_ptr` needs.
        let mut ret: c_int = 0;
        let _g = Vm::rlock_vm(vm_ptr);
        'done: {
            if vm_ptr.is_null() || _g.pagetable.is_null() {
                ret = -(EINVAL as c_int);
                break 'done;
            }
            let addr = pg_round_down(addr_in);
            let size = pg_round_up(size_in as u64);
            if addr < UVMBOTTOM as u64 || addr + size > UVMTOP as u64 {
                ret = -(ENOMEM as c_int);
                break 'done;
            }
            let vma_ptr = Vm::vm_find_area(vm_ptr, addr);
            if vma_ptr.is_null() {
                ret = -(ENOMEM as c_int);
                break 'done;
            }
            if addr < machine::vma_start(vma_ptr) || addr + size > machine::vma_end(vma_ptr) {
                ret = -(ENOMEM as c_int);
                break 'done;
            }
            if !machine::vma_file(vma_ptr).is_null() {
                // Kept at SeqCst: 1:1 port of the C source's explicit
                // full barrier guarding the file-backed-dirty-state
                // check that follows; `vm_msync` is not a hot path.
                fence(Ordering::SeqCst);
            }
            ret = 0;
        }
        ret
    }

    pub(crate) fn vm_mincore(
        vm_ptr: *mut vm,
        addr_in: u64,
        size_in: usize,
        vec: *mut u8,
    ) -> c_int {
        let mut ret: c_int = 0;
        let _g = Vm::rlock_vm(vm_ptr);
        // SAFETY: `vm_ptr`/`vec`, once past the null checks just below, are a
        // live `vm` and a caller-owned buffer of at least `num_pages` bytes
        // (`num_pages` is derived from the just-validated `size`); every
        // `*mut vma` touched comes from `vm_find_area`, checked non-null.
        unsafe {
            'done: {
                if vm_ptr.is_null() || _g.pagetable.is_null() || vec.is_null() {
                    ret = -(EINVAL as c_int);
                    break 'done;
                }
                if (addr_in & (PGSIZE as u64 - 1)) != 0 {
                    ret = -(EINVAL as c_int);
                    break 'done;
                }
                let size = pg_round_up(size_in as u64);
                if addr_in < UVMBOTTOM as u64 || addr_in + size > UVMTOP as u64 {
                    ret = -(ENOMEM as c_int);
                    break 'done;
                }
                let mut vma_ptr = Vm::vm_find_area(vm_ptr, addr_in);
                if vma_ptr.is_null() {
                    ret = -(ENOMEM as c_int);
                    break 'done;
                }
                let num_pages = (size / PGSIZE as u64) as usize;
                {
                    let _pgl = Vm::lock_vm_pgtable(vm_ptr);
                    for i in 0..num_pages {
                        let va = addr_in + (i as u64) * (PGSIZE as u64);
                        *vec.add(i) = 0;
                        if va < machine::vma_start(vma_ptr) || va >= machine::vma_end(vma_ptr) {
                            vma_ptr = Vm::vm_find_area(vm_ptr, va);
                            if vma_ptr.is_null() {
                                continue;
                            }
                        }
                        let pte = PageTable::walk(
                            _pgl.pagetable as *mut u64,
                            va,
                            0,
                            core::ptr::null_mut(),
                            core::ptr::null_mut(),
                        );
                        if !pte.is_null() && (*pte & PTE_V as u64) != 0 {
                            *vec.add(i) = 1;
                        }
                    }
                }
                ret = 0;
            }
            ret
        }
    }

    pub(crate) fn vm_madvise(
        vm_ptr: *mut vm,
        addr_in: u64,
        size_in: usize,
        advice: c_int,
    ) -> c_int {
        let mut ret: c_int = 0;
        let _g = Vm::wlock_vm(vm_ptr);
        // SAFETY: `vm_ptr`, once past the null checks just below, is a live
        // `vm`; `vma_ptr` comes from `vm_find_area`, checked non-null and
        // range-checked before use; every PTE pointer comes from `PageTable::walk()`.
        unsafe {
            'done: {
                if vm_ptr.is_null() || _g.pagetable.is_null() {
                    ret = -(EINVAL as c_int);
                    break 'done;
                }
                let addr = pg_round_down(addr_in);
                let size = pg_round_up(size_in as u64);
                if addr < UVMBOTTOM as u64 || addr + size > UVMTOP as u64 {
                    ret = -(ENOMEM as c_int);
                    break 'done;
                }
                let vma_ptr = Vm::vm_find_area(vm_ptr, addr);
                if vma_ptr.is_null() {
                    ret = -(ENOMEM as c_int);
                    break 'done;
                }
                if addr < machine::vma_start(vma_ptr) || addr + size > machine::vma_end(vma_ptr) {
                    ret = -(ENOMEM as c_int);
                    break 'done;
                }
                match advice as u32 {
                    MADV_NORMAL | MADV_RANDOM | MADV_SEQUENTIAL | MADV_WILLNEED => {
                        ret = 0;
                    }
                    MADV_DONTNEED => {
                        {
                            let _pgl = Vm::lock_vm_pgtable(vm_ptr);
                            for va in (addr..addr + size).step_by(PGSIZE as usize) {
                                let pte = PageTable::walk(
                                    _pgl.pagetable as *mut u64,
                                    va,
                                    0,
                                    core::ptr::null_mut(),
                                    core::ptr::null_mut(),
                                );
                                if !pte.is_null() && (*pte & PTE_V as u64) != 0 {
                                    let pa = ((*pte) >> 10) << 12;
                                    if pa != 0 {
                                        page_ref_dec(pa as *mut c_void);
                                    }
                                    *pte = 0;
                                }
                            }
                        }
                        Vm::vm_remote_sfence(vm_ptr);
                        ret = 0;
                    }
                    _ => {
                        ret = -(EINVAL as c_int);
                    }
                }
            }
            ret
        }
    }

    pub(crate) fn vm_alloc_thread_stack(
        vm_ptr: *mut vm,
        stack_size_in: usize,
        stack_top_out: *mut u64,
    ) -> c_int {
        if vm_ptr.is_null() || stack_top_out.is_null() {
            return -(EINVAL as c_int);
        }
        let mut stack_size = stack_size_in;
        if stack_size < (PGSIZE as usize) * 4 {
            stack_size = (PGSIZE as usize) * 4;
        }
        stack_size = pg_round_up(stack_size as u64) as usize;
        let total_size = stack_size + PGSIZE as usize;

        let _g = Vm::wlock_vm(vm_ptr);
        let stack_base = Vm::vm_find_free_range(vm_ptr, total_size, 0);
        if stack_base == 0 {
            return -(ENOMEM as c_int);
        }
        let guard_page = stack_base;
        let guard_vma = Vma::vma_alloc(
            vm_ptr,
            guard_page,
            PGSIZE as u64,
            crate::bindings::VMA_FLAG_GROWSDOWN as u64,
        );
        if guard_vma.is_null() {
            return -(ENOMEM as c_int);
        }
        let usable_stack_base = guard_page + PGSIZE as u64;
        let flags = (VMA_FLAG_USER | PROT_READ | PROT_WRITE) as u64
            | crate::bindings::VMA_FLAG_GROWSDOWN as u64;
        let stack_vma = Vma::vma_alloc(vm_ptr, usable_stack_base, stack_size as u64, flags);
        if stack_vma.is_null() {
            Vma::vma_free(vm_ptr, guard_vma);
            return -(ENOMEM as c_int);
        }
        let stack_top = usable_stack_base + stack_size as u64;
        drop(_g);
        // SAFETY: `stack_top_out` was checked non-null above; it is a
        // caller-owned out-parameter valid for one `u64` write.
        unsafe { *stack_top_out = stack_top };
        0
    }

    pub(crate) fn vm_free_thread_stack(
        vm_ptr: *mut vm,
        stack_top: u64,
        stack_size_in: usize,
    ) -> c_int {
        if vm_ptr.is_null() || stack_top == 0 {
            return -(EINVAL as c_int);
        }
        let mut stack_size = pg_round_up(stack_size_in as u64) as usize;
        if stack_size < (PGSIZE as usize) * 4 {
            stack_size = (PGSIZE as usize) * 4;
        }
        let usable_stack_base = stack_top - stack_size as u64;
        let guard_page = usable_stack_base - PGSIZE as u64;

        let _g = Vm::wlock_vm(vm_ptr);
        let stack_vma = Vm::vm_find_area(vm_ptr, usable_stack_base);
        if !stack_vma.is_null() {
            Vma::vma_free(vm_ptr, stack_vma);
        }
        let guard_vma = Vm::vm_find_area(vm_ptr, guard_page);
        if !guard_vma.is_null() {
            Vma::vma_free(vm_ptr, guard_vma);
        }
        0
    }

    pub(crate) fn vm_mmap(
        vm_ptr: *mut vm,
        addr: u64,
        length_in: usize,
        prot: c_int,
        flags: c_int,
        fd: c_int,
        offset: u64,
    ) -> u64 {
        let mut file: *mut vfs_file = core::ptr::null_mut();
        if vm_ptr.is_null() || length_in == 0 {
            return u64::MAX;
        }
        if (flags & MAP_PRIVATE as c_int) == 0 && (flags & MAP_SHARED as c_int) == 0 {
            return u64::MAX;
        }
        if (flags & MAP_PRIVATE as c_int) != 0 && (flags & MAP_SHARED as c_int) != 0 {
            return u64::MAX;
        }
        if (flags & MAP_SHARED as c_int) != 0 {
            return u64::MAX;
        }
        if fd != -1 {
            if (flags & MAP_ANONYMOUS as c_int) != 0 {
                return u64::MAX;
            }
            if (offset & (PGSIZE as u64 - 1)) != 0 {
                return u64::MAX;
            }
            let fdtable = xv6_vm_current_fdtable();
            if fdtable.is_null() {
                return u64::MAX;
            }
            file = vfs_fdtable_get_file(fdtable, fd);
            if file.is_null() {
                return u64::MAX;
            }
            if xv6_vm_inode_is_reg(file) == 0 {
                // SAFETY: `file` was just checked non-null and is a live
                // `vfs_file` reference obtained from `vfs_fdtable_get_file`.
                unsafe { VfsFile::vfs_fput(file) };
                return u64::MAX;
            }
        } else if (flags & MAP_ANONYMOUS as c_int) == 0 {
            return u64::MAX;
        }

        let length = pg_round_up(length_in as u64);
        let mut vm_flags = (VMA_FLAG_USER as u64)
            | ((prot as u64) & ((PROT_READ | PROT_WRITE | PROT_EXEC) as u64));
        if !file.is_null() {
            vm_flags |= crate::bindings::VMA_FLAG_FILE as u64;
        }

        let _g = Vm::wlock_vm(vm_ptr);

        let map_addr: u64;
        if addr == 0 || (flags & MAP_FIXED as c_int) == 0 {
            let m = Vm::vm_find_free_range(vm_ptr, length as usize, addr);
            if m == 0 {
                drop(_g);
                if !file.is_null() {
                    // SAFETY: `file` is a live `vfs_file` reference (see above).
                    unsafe { VfsFile::vfs_fput(file) };
                }
                return u64::MAX;
            }
            map_addr = m;
        } else {
            map_addr = pg_round_down(addr);
        }

        let vma_ptr = Vma::vma_alloc(vm_ptr, map_addr, length, vm_flags);
        if vma_ptr.is_null() {
            drop(_g);
            if !file.is_null() {
                // SAFETY: `file` is a live `vfs_file` reference (see above).
                unsafe { VfsFile::vfs_fput(file) };
            }
            return u64::MAX;
        }
        if !file.is_null() {
            // SAFETY: `vma_ptr` was just checked non-null and is a freshly
            // `vma_alloc`'d, exclusively-owned `vma`.
            unsafe {
                (*vma_ptr).file = file;
                (*vma_ptr).pgoff = offset;
            }
        }
        map_addr
    }

    pub(crate) fn vm_munmap(vm_ptr: *mut vm, addr: u64, length: usize) -> c_int {
        Vm::vm_munmap_region(vm_ptr, addr, length)
    }

    /// Debug helper — prints every VMA in `vm`. Mirrors the C `dump_vm`.
    pub(crate) fn dump_vm(vm_ptr: *mut vm) {
        if vm_ptr.is_null() {
            return;
        }
        // SAFETY: `vm_ptr` checked non-null above; every `*mut vma` walked
        // comes from `xv6_vm_first_vma`/`xv6_vm_next_vma`, both null-checked
        // in the loop condition; `printf` is variadic so can't be `safe`.
        unsafe {
            crate::kprintln!("VM dump:");
            crate::kprintln!("Pagetable: {}", crate::printf::Ptr((*vm_ptr).pagetable as u64));
            crate::kprintln!("VMAs:");
            for vma_ptr in Vm::vm_vma_iter(vm_ptr) {
                let mut flags_buf: [c_char; 10] = [0; 10];
                vm_dump_flags(
                    (*vma_ptr).flags,
                    flags_buf.as_mut_ptr(),
                    flags_buf.len(),
                );
                crate::kprintln!(
                    "VMA: start={:x}, end={:x}, flags={}, file={}, pgoff={:x}",
                    (*vma_ptr).start as u64,
                    (*vma_ptr).end as u64,
                    crate::printf::Cs(flags_buf.as_ptr()),
                    crate::printf::Ptr((*vma_ptr).file as u64),
                    (*vma_ptr).pgoff as u64,
                );
            }
        }
    }

}

const PAGE_TYPE_ANON: u64 = 0;
const BLK_SIZE: u64 = 512; // from kernel/inc/dev/bio.h

// C-side helpers we need that are real linkable kernel symbols.
// Macros / inline-asm helpers formerly in `kernel/mm/vm_shims.c` are now
// implemented as private Rust `unsafe fn xv6_vm_*` below (search this file
// for "// =====  xv6_vm_* shims (Rust)  =====").
mod ffi {
    // P3-D3c: `printf.rs`'s panic plumbing -- plain crate-path re-exports
    // now that the `#[no_mangle]` exports are gone.
    pub(crate) use crate::printf::{__panic_end, __panic_start};

    use super::*;
    use core::ffi::{c_char, c_int, c_void};
    unsafe extern "C" {
        // Real C kernel sync primitives (declared in defs.h / lock/rwsem.h).

        // Page-table free, defined in vm_pgtab.rs.
        // Pagetable creation, defined in vm_pgtab.rs.

        // Real C symbols pulled in via bindgen-style externs.

        // V3 fifth slice helpers.


        // SBI remote TLB shootdown (kernel/sbi.c).
        // xv6_vm_sfence_vma still lives in vm_pgtab_shims.c (until that file is
        // migrated). It wraps the riscv `sfence.vma` inline asm.

        // pcache accessors (live in pcache_shims.c).
        // Defined in kernel/rust/src/vm_pgtab.rs (no_mangle).


        // Real kernel panic helpers (kernel/printf.c) — replace the
        // `xv6_vm_panic` shim that lived in vm_pgtab_shims.c (still declared
        // here for now; see __vm_panic_str below).
    }
pub(crate) use crate::lock::rwsem::{rwsem_acquire_read, rwsem_acquire_write, rwsem_init, rwsem_is_write_holding, rwsem_release};
pub(crate) use crate::mm::vm_pgtab::{vm_dump_flags, xv6_vm_sfence_vma, PageTable};
pub(crate) use crate::sbi::sbi_remote_hfence_vma;
    // P3-1C mesh sweep: vfs/fs.rs is in scope for this wave; its
    // `vfs_inode_deref` is now a plain crate-path re-export (signature is
    // identical, no opaque-pointer mismatch). vfs/fdtable.rs's
    // `vfs_fdtable_get_file` keeps a thin wrapper: this file's established
    // per-file idiom types the fdtable handle as opaque `*mut c_void`
    // (see `xv6_vm_current_fdtable` above), rather than the real
    // `*mut vfs_fdtable`.
    pub(crate) use crate::vfs::fs::FsStruct;
    /// SAFETY: `fdtable` must be a live `vfs_fdtable` (caller's contract,
    /// unchanged from the extern declaration this replaces).
    pub fn vfs_fdtable_get_file(fdtable: *mut c_void, fd: c_int) -> *mut vfs_file {
        crate::vfs::fdtable::VfsFdtable::vfs_fdtable_get_file(
            fdtable as *mut crate::bindings::vfs_fdtable,
            fd,
        )
    }

    // The bintree/rbtree/spinlock/page/string primitives below are all
    // genuinely `unsafe extern "C" fn`; this file's original extern
    // declaration asserted `pub safe fn` (usual FFI facade) for every one
    // of them. Thin safe wrappers preserve that facade.
    /// SAFETY: see [`crate::bintree::rb_find_key_rdown`]'s contract.
    pub fn rb_find_key_rdown(root: *mut crate::bindings::rb_root, key: u64) -> *mut rb_node {
        unsafe { crate::bintree::rb_find_key_rdown(root, key) }
    }
    /// SAFETY: see [`crate::bintree::rb_next_node`]'s contract.
    pub fn rb_next_node(node: *mut rb_node) -> *mut rb_node {
        unsafe { crate::bintree::rb_next_node(node) }
    }
    /// SAFETY: see [`crate::bintree::rb_prev_node`]'s contract.
    pub fn rb_prev_node(node: *mut rb_node) -> *mut rb_node {
        unsafe { crate::bintree::rb_prev_node(node) }
    }
    /// SAFETY: see [`crate::rbtree::rb_insert_color`]'s contract.
    pub fn rb_insert_color(root: *mut crate::bindings::rb_root, node: *mut rb_node) -> *mut rb_node {
        unsafe { crate::rbtree::rb_insert_color(root, node) }
    }
    /// SAFETY: see [`crate::rbtree::rb_delete_node_color`]'s contract.
    pub fn rb_delete_node_color(root: *mut crate::bindings::rb_root, node: *mut rb_node) -> *mut rb_node {
        unsafe { crate::rbtree::rb_delete_node_color(root, node) }
    }
    /// SAFETY: see [`crate::lock::spinlock::spin_init`]'s contract.
    pub fn spin_init(lk: *mut spinlock_t, name: *mut c_char) {
        unsafe { crate::lock::spinlock::spin_init(lk, name) };
    }
    /// SAFETY: see [`crate::lock::spinlock::spin_lock`]'s contract.
    pub fn spin_lock(lk: *mut spinlock_t) {
        unsafe { crate::lock::spinlock::spin_lock(lk) };
    }
    /// SAFETY: see [`crate::lock::spinlock::spin_unlock`]'s contract.
    pub fn spin_unlock(lk: *mut spinlock_t) {
        unsafe { crate::lock::spinlock::spin_unlock(lk) };
    }
    /// SAFETY: see [`crate::mm::page::page_alloc`]'s contract.
    pub fn page_alloc(order: u64, flags: u64) -> *mut c_void {
        unsafe { crate::mm::page::Page::page_alloc(order, flags) }
    }
    /// SAFETY: `ptr` must originate from `page_alloc` above.
    pub fn page_free(ptr: *mut c_void, order: u64) {
        unsafe { crate::mm::page::Page::page_free(ptr, order) };
    }
    /// SAFETY: see [`crate::string::memset`]'s contract.
    pub fn memset(s: *mut c_void, c: c_int, n: usize) -> *mut c_void {
        unsafe { crate::string::memset(s, c, n) }
    }
    /// SAFETY: see [`crate::string::memmove`]'s contract.
    pub fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void {
        unsafe { crate::string::memmove(dst, src, n) }
    }
    // `pcache_get_page`/`pcache_put_page`/`pcache_read_page`/
    // `xv6_page_pcache_get_node`/`xv6_pcache_node_data` local facades
    // deleted (KERNEL-OO wave): the pcache wave (6cf22ac) converted their
    // targets to `Pcache::get_page`/`put_page`/`read_page`/`page_get_node`
    // and `PcacheNode::data`; this file's only caller (`Vma::
    // vma_fault_file_page`) now calls them directly.
}
use ffi::*;

// =====================================================================
// xv6_vm_* shims (Rust)  — formerly kernel/mm/vm_shims.c
// =====================================================================
//
// Each function here replaces a one-liner C shim that wrapped a macro or
// inline asm. They are private to this module; the rest of vm.rs continues
// to call `xv6_vm_*` exactly as before, so call-site churn is zero.
// =====================================================================

// --- SSTATUS.SIE save/restore for interrupt-safe percpu access -----------
//
// The asm primitives and per-CPU helpers live in `crate::machine`, which
// keeps its own inline-asm `unsafe` blocks internal to safe fn wrappers.
// Importing them here keeps the rest of vm.rs free of low-level
// boilerplate.

use crate::machine::{self, ThreadRef};
use crate::mm::mm_safe::PageHandle;


// --- mycpu / cpuid replicas (match the C macros byte-for-byte) -----------
#[inline(always)]
fn xv6_vm_mycpu() -> *mut crate::bindings::cpu_local {
    machine::cpu_local_ptr()
}


pub(crate) fn xv6_vm_cpuid() -> c_int {
    machine::cpuid()
}

// --- SBI remote hfence ---------------------------------------------------
#[inline(always)]
fn xv6_vm_sbi_remote_hfence_vma(hart_mask: u64) {
    sbi_remote_hfence_vma(hart_mask, 0, 0, 0);
}


// --- PX(0, va) -----------------------------------------------------------
#[inline(always)]
fn xv6_vm_px0(va: u64) -> u64 {
    (va >> 12) & 0x1FF
}


// --- current thread accessors -------------------------------------------
#[inline(always)]
fn current_thread() -> *mut crate::bindings::thread {
    machine::current_thread_ptr()
}

#[inline]
fn xv6_vm_current_trapframe() -> u64 {
    match ThreadRef::from_raw(current_thread()) {
        None => 0,
        Some(t) => {
            let tf = t.trapframe();
            if tf.is_null() { 0 } else { tf as u64 }
        }
    }
}


#[inline]
fn xv6_vm_current_fdtable() -> *mut c_void {
    match ThreadRef::from_raw(current_thread()) {
        None => core::ptr::null_mut(),
        Some(t) => t.fdtable(),
    }
}

#[inline]
fn xv6_vm_has_current() -> c_int {
    if current_thread().is_null() { 0 } else { 1 }
}


// --- inode-is-regular helper --------------------------------------------
#[inline]
fn xv6_vm_inode_is_reg(file: *mut vfs_file) -> c_int {
    let Some(f) = machine::VfsFileRef::from_raw(file) else { return 0; };
    let inode = FsStruct::vfs_inode_deref(f.inode_slot_ptr());
    if inode.is_null() { return 0; }
    // S_ISREG: (mode & S_IFMT) == S_IFREG, with S_IFMT=0170000, S_IFREG=0100000.
    let mode = machine::vfs_inode_mode(inode);
    if (mode & 0o170000) == 0o100000 { 1 } else { 0 }
}

// --- list_entry / rb_node initializers (formerly out-of-line wrappers) --
#[inline]
fn xv6_vm_rb_node_init(node: *mut rb_node) {
    machine::rb_node_init(node);
}
#[inline]
fn xv6_vm_list_entry_init(entry: *mut list_node_t) {
    machine::list_entry_init(entry);
}

// --- list walk helpers (mirror LIST_FIRST/NEXT/LAST/PREV_NODE macros) ---
//
// LIST_FIRST_NODE(head, type, member):
//     (head->next == head) ? NULL : container_of(head->next, type, member)
// The members on vma_t are `list_entry` and `free_list_entry`.

#[inline]
fn first_in_list(head: *mut list_node_t, member_off: usize) -> *mut vma {
    let first = machine::list_entry_next(head);
    if first as *const list_node_t == head as *const list_node_t {
        core::ptr::null_mut()
    } else {
        machine::list_container_of::<vma>(first, member_off)
    }
}
#[inline]
fn last_in_list(head: *mut list_node_t, member_off: usize) -> *mut vma {
    let last = machine::list_entry_prev(head);
    if last as *const list_node_t == head as *const list_node_t {
        core::ptr::null_mut()
    } else {
        machine::list_container_of::<vma>(last, member_off)
    }
}
#[inline]
fn next_in_list(
    head: *mut list_node_t,
    cur_entry: *mut list_node_t,
    member_off: usize,
) -> *mut vma {
    let nxt = machine::list_entry_next(cur_entry);
    if nxt as *const list_node_t == head as *const list_node_t {
        core::ptr::null_mut()
    } else {
        machine::list_container_of::<vma>(nxt, member_off)
    }
}
#[inline]
fn prev_in_list(
    head: *mut list_node_t,
    cur_entry: *mut list_node_t,
    member_off: usize,
) -> *mut vma {
    let p = machine::list_entry_prev(cur_entry);
    if p as *const list_node_t == head as *const list_node_t {
        core::ptr::null_mut()
    } else {
        machine::list_container_of::<vma>(p, member_off)
    }
}




// --- list insert / detach (mirror list.h inlines) -----------------------
#[inline]
fn list_entry_insert_after(prev: *mut list_node_t, new: *mut list_node_t) {
    machine::list_entry_insert_after(prev, new);
}
#[inline]
fn list_entry_detach(entry: *mut list_node_t) {
    machine::list_entry_detach(entry);
}


// --- rb_prev_entry_safe / rb_next_entry_safe wrappers -------------------

// --- memory barriers ----------------------------------------------------
#[inline(always)]
fn xv6_vm_smp_mb() {
    // C: __atomic_thread_fence(__ATOMIC_SEQ_CST). On riscv this lowers to
    // `fence rw,rw`. Use core::sync::atomic for equivalent semantics.
    // Kept at SeqCst (not downgraded): this is a general-purpose full
    // barrier primitive called from multiple sites with different
    // pairing needs, not a single Acquire/Release handoff — a 1:1 port
    // of the C source's explicit full fence.
    fence(Ordering::SeqCst);
}


// --- printf-only warnings (formerly out-of-line in vm_shims.c) ----------
#[inline]
fn xv6_vm_warn_vma_double_free(v: *mut c_void) {
    crate::kprintln!("__vma_free: DOUBLE FREE detected for vma={}", crate::printf::Ptr(v as u64));
}

#[inline]
fn xv6_vm_warn_vma_null_vm(v: *mut c_void) {
    crate::kprintln!(
        "__vma_free: vma->vm is NULL for vma={} (possible double-free or corruption)",
        crate::printf::Ptr(v as u64),
    );
}

#[inline]
fn xv6_vm_warn_double_destroy(v: *mut c_void) {
    crate::kprintln!("__vm_destroy: DOUBLE DESTROY detected for vm={}", crate::printf::Ptr(v as u64));
}

#[inline]
fn xv6_vm_warn_vma_vm_mismatch(
    v: *mut c_void,
    vma_vm: *mut c_void,
    expected: *mut c_void,
) {
    crate::kprintln!(
        "__vm_destroy: vma->vm mismatch! vma={} vma->vm={} expected={}",
        crate::printf::Ptr(v as u64),
        crate::printf::Ptr(vma_vm as u64),
        crate::printf::Ptr(expected as u64),
    );
}

#[inline]
fn xv6_vm_warn_vma_already_freed(v: *mut c_void) {
    crate::kprintln!("__vm_destroy: vma already freed! vma={}", crate::printf::Ptr(v as u64));
}


// --- assert (current == NULL || rwsem_is_write_holding(&vm->rw_lock)) ---

/// rb-tree key comparator. Bound to `__vm_tree_opts.keys_cmp_fun`.
///
/// Returns 0 for equal, -1 if `a < b`, +1 otherwise — same contract as the
/// original C implementation.
///
/// Kept `extern "C"`: passed *by value* as `Some(__vma_cmp)` into
/// `bindings::rb_root::keys_cmp_fun`, a bindgen-generated `unsafe extern "C"
/// fn` pointer field — the calling convention is part of the function's
/// type here, not just a link-time symbol name, so it must stay even though
/// `#[no_mangle]` (nothing looks this up by C symbol name) is dropped.
pub(crate) extern "C" fn __vma_cmp(a: u64, b: u64) -> c_int {
    if a == b {
        0
    } else if a < b {
        -1
    } else {
        1
    }
}

/// rb-tree key extractor. Bound to `__vm_tree_opts.get_key_fun`.
///
/// Returns `container_of(node, vma_t, rb_entry)->start`. Implemented using
/// `core::mem::offset_of!` against the bindgen-generated `vma` layout, which
/// is guaranteed to match the C `vma_t` because both compilation units share
/// the same header (`mm/vm_types.h`).
///
/// Kept `extern "C"` for the same reason as [`__vma_cmp`]: bound by value
/// into `bindings::rb_root::get_key_fun`, a bindgen `unsafe extern "C" fn`
/// pointer field.
pub(crate) extern "C" fn __cma_get_key(node: *mut rb_node) -> u64 {
    if node.is_null() {
        return 0;
    }
    let offset = core::mem::offset_of!(vma, rb_entry);
    // `container_of` itself is safe pointer arithmetic (see
    // `crate::mm::cffi::container_of`).
    let vma_ptr: *mut vma = crate::mm::cffi::container_of(node, offset);
    // `node` is a live rb-tree node embedded in a `vma` (the rb-tree only
    // ever stores `vma::rb_entry` nodes), so `vma_ptr` points at a valid
    // `vma`; `machine::vma_start` is the existing safe accessor for the
    // one field this needs and carries its own soundness argument for the
    // raw dereference behind it, so this function stays fully safe.
    machine::vma_start(vma_ptr)
}





// ---------------------------------------------------------------------------
// V2 second slice: slab pools, VMA alloc/free, trapframe map/unmap
// ---------------------------------------------------------------------------

// SLAB flag bits (defined inside `mm/slab_type.h`; bindgen does not always
// pick up `#define`s nested in struct bodies, so mirror them here).
const SLAB_FLAG_STATIC: u64 = 1;
const SLAB_FLAG_DEBUG_BITMAP: u64 = 4;

// Magic value to detect double-free of a vma_t.
const VMA_FREED_MAGIC: u64 = 0xDEAD_0BAD_DEAD_0BADu64;

// PTE bit helpers (PTE_FLAGS and PTE2PA are macros in `riscv.h`).
#[inline(always)]
fn pte_flags(pte: u64) -> u64 {
    pte & 0x3FF
}

#[inline(always)]
fn pte_to_pa(pte: u64) -> u64 {
    (pte >> 10) << 12
}

#[inline(always)]
fn pg_round_down(a: u64) -> u64 {
    a & !((PGSIZE as u64) - 1)
}


// Slab pools — storage moved to Rust (formerly in vm_shims.c as
// `slab_cache_t __vma_pool = {0};` and `slab_cache_t __vm_pool = {0};`).
// They are not referenced by any surviving C code; everything goes through
// `__vma_pool_init` / `__vm_pool_init` (`pub(crate)` Rust fns) for setup
// and through __vma_alloc / __vma_free / etc. internally.
// SAFETY: `VmPoolStorage` is used only for `__VMA_POOL`/`__VM_POOL`
// below; both are written in full by `slab_cache_init` (called from
// `__vma_pool_init`/`__vm_pool_init` before any `slab_alloc`/
// `slab_free` on the same pool) and otherwise only accessed through
// the C slab-allocator primitives, which serialize their own internal
// state.
#[repr(transparent)]
struct VmPoolStorage(
    core::cell::UnsafeCell<core::mem::MaybeUninit<crate::bindings::slab_cache_t>>,
);
unsafe impl Sync for VmPoolStorage {}
impl VmPoolStorage {
    // `uninit()` not `zeroed()`: `slab_cache_init` (via `init_impl` /
    // `init_internal` in `slab.rs`) unconditionally overwrites every
    // field rather than reading-then-modifying, and nothing reads this
    // storage before that call, so no zeroed bit pattern is ever
    // observed.
    const fn zeroed() -> Self {
        Self(core::cell::UnsafeCell::new(core::mem::MaybeUninit::uninit()))
    }
    #[inline(always)]
    fn as_ptr(&self) -> *mut slab_cache_t {
        self.0.get() as *mut slab_cache_t
    }
}
static __VMA_POOL: VmPoolStorage = VmPoolStorage::zeroed();
static __VM_POOL: VmPoolStorage = VmPoolStorage::zeroed();




unsafe extern "C" {

    fn rb_node_init(node: *mut rb_node);
    fn list_entry_init(entry: *mut list_node_t);

    // xv6_vm_panic still lives in vm_pgtab_shims.c (it uses __FILE__/
    // __LINE__/__FUNCTION__ via the C `panic` macro). Migrated when that
    // shim file is eliminated.
}
// P3-1C mesh sweep: vfs/file.rs is in scope for this wave; signatures are
// identical (`*mut vfs_file`, same type this file already imports), so
// these become plain crate-path re-exports instead of `extern "C"`
// redeclarations.
use crate::vfs::file::{FileOps, VfsFile};
pub(crate) use crate::mm::slab::slab_free;
pub(crate) use crate::mm::vm_pgtab::xv6_vm_panic;

/// `crate::mm::page::page_ref_dec` is genuinely `unsafe fn`; this file's
/// original extern declaration asserted `pub safe fn` (usual FFI facade;
/// contrast with `page_ref_inc` above, whose original declaration was a
/// plain non-`safe` `fn` — its call sites already wrap `unsafe {}`).
fn page_ref_dec(ptr: *mut c_void) -> c_int {
    // SAFETY: see `crate::mm::page::page_ref_dec`'s contract.
    unsafe { crate::mm::page::Page::page_ref_dec(ptr) }
}

/// `slab_alloc`/`slab_cache_init`/`slab_free`: this file's original extern
/// declaration typed the cache pointer as the bindgen `slab_cache_t`
/// rather than `crate::mm::slab::SlabCache` (same "locally convenient
/// pointer type" idiom used throughout this crate). Call sites here
/// already wrap these in `unsafe {}` (the original declaration was a
/// plain, non-`safe` `fn`), so only the type needs adapting.
fn slab_cache_init(
    cache: *mut slab_cache_t, name: *mut c_char, obj_size: usize, flags: u64,
) -> c_int {
    // SAFETY: caller's `unsafe` block carries the real contract; this is
    // a pure pointer-type reinterpretation (see `crate::mm::slab::SlabCache`).
    unsafe {
        crate::mm::slab::slab_cache_init(cache as *mut crate::mm::slab::SlabCache, name, obj_size, flags)
    }
}
fn slab_alloc(cache: *mut slab_cache_t) -> *mut c_void {
    // SAFETY: see `slab_cache_init` above.
    unsafe { crate::mm::slab::slab_alloc(cache as *mut crate::mm::slab::SlabCache) }
}

#[inline(always)]
fn list_node_is_detached(entry: *const list_node_t) -> bool {
    machine::list_entry_is_detached(entry)
}









// ---------------------------------------------------------------------------
// V3 first slice: trivial flag converters + refcount bump
// ---------------------------------------------------------------------------

/// Convert VMA prot/flag bits into RISC-V PTE permission bits.
/// Mirrors C `vma2pte_flags`.
pub(crate) fn vma2pte_flags(flags: u64) -> u64 {
    let mut pte: u64 = 0;
    if flags & (PROT_READ as u64) != 0 {
        pte |= PTE_R as u64;
    }
    if flags & (PROT_WRITE as u64) != 0 {
        pte |= PTE_W as u64;
    }
    if flags & (PROT_EXEC as u64) != 0 {
        pte |= PTE_X as u64;
    }
    if flags & (VMA_FLAG_USER as u64) != 0 {
        pte |= PTE_U as u64;
    }
    pte
}

/// Inverse of `vma2pte_flags`.
pub(crate) fn pte2vma_flags(pte_flags: u64) -> u64 {
    let mut flags: u64 = 0;
    if pte_flags & (PTE_R as u64) != 0 {
        flags |= PROT_READ as u64;
    }
    if pte_flags & (PTE_W as u64) != 0 {
        flags |= PROT_WRITE as u64;
    }
    if pte_flags & (PTE_X as u64) != 0 {
        flags |= PROT_EXEC as u64;
    }
    if pte_flags & (PTE_U as u64) != 0 {
        flags |= VMA_FLAG_USER as u64;
    }
    flags
}


// ---------------------------------------------------------------------------
// V3 second slice: lock wrappers (thin forwards to C primitives)
// ---------------------------------------------------------------------------







// ---------------------------------------------------------------------------
// RAII guards over the three lock wrappers above. `vm_rlock` / `vm_runlock`
// / `vm_wlock` / `vm_wunlock` / `vm_pgtable_lock` / `vm_pgtable_unlock`
// themselves stay exactly as-is (`pub(crate)` fns since P3-D3a, called in
// paired form from `irq/trap.rs` / `exec.rs` across function boundaries). These
// guards only wrap *internal* same-function lock/unlock call sites within
// this file, mirroring the `PcLocalGuard`-style helpers in `pcache.rs`.
//
// Unlike the original bare-newtype guards (Drop-unlock only, requiring every
// caller to separately raw-deref `vm_ptr` / index `(*vm_ptr).field` under
// the held lock, each such site carrying its own hand-written SAFETY
// argument), these guards *carry* the protected `vm` reference via `Deref`
// (`DerefMut` on the write guard). Each guard's `deref`/`deref_mut` impl
// below has exactly one raw-pointer access, and its doc comment is the
// entire soundness argument for every call site that now writes
// `guard.field` instead of a manual raw deref: the reference's lifetime is
// tied to `&self`/`&mut self`, i.e. to the guard binding itself, so it
// cannot outlive the lock hold (the lock is released in `Drop`) and no
// other racing access can alias it while it's live — exactly what each call
// site's now-removed per-site justification used to have to argue by hand.
// ---------------------------------------------------------------------------
#[must_use = "vm read lock is released when the guard is dropped"]
struct VmReadGuard<'a> {
    vm_ptr: *mut vm,
    _marker: PhantomData<&'a vm>,
}
impl Drop for VmReadGuard<'_> {
    fn drop(&mut self) { Vm::vm_runlock(self.vm_ptr); }
}
impl<'a> core::ops::Deref for VmReadGuard<'a> {
    type Target = vm;
    fn deref(&self) -> &vm {
        // SAFETY: `VmReadGuard` is only ever constructed by `rlock_vm`,
        // which has just called `vm_rlock` (rwsem acquire-read) on
        // `vm_ptr`; the returned reference's lifetime is bound to `&self`
        // (this guard), which cannot outlive the read-lock hold (`Drop`
        // releases it via `vm_runlock`).
        unsafe { &*self.vm_ptr }
    }
}

#[must_use = "vm write lock is released when the guard is dropped"]
struct VmWriteGuard<'a> {
    vm_ptr: *mut vm,
    _marker: PhantomData<&'a mut vm>,
}
impl Drop for VmWriteGuard<'_> {
    fn drop(&mut self) { Vm::vm_wunlock(self.vm_ptr); }
}
impl<'a> core::ops::Deref for VmWriteGuard<'a> {
    type Target = vm;
    fn deref(&self) -> &vm {
        // SAFETY: see `VmReadGuard::deref`; `vm_wlock` (rwsem
        // acquire-write) additionally guarantees no other reader/writer
        // is concurrently active for as long as this guard lives.
        unsafe { &*self.vm_ptr }
    }
}
impl<'a> core::ops::DerefMut for VmWriteGuard<'a> {
    fn deref_mut(&mut self) -> &mut vm {
        // SAFETY: `VmWriteGuard` is only ever constructed by `wlock_vm`,
        // which has just called `vm_wlock` (rwsem acquire-write) — the
        // write lock is exclusive, so this is the only live reference to
        // `*vm_ptr`; the returned reference's lifetime is bound to
        // `&mut self` and cannot outlive the write-lock hold (`Drop`
        // releases it via `vm_wunlock`).
        unsafe { &mut *self.vm_ptr }
    }
}

#[must_use = "vm pgtable lock is released when the guard is dropped"]
struct VmPgtableGuard<'a> {
    vm_ptr: *mut vm,
    _marker: PhantomData<&'a vm>,
}
impl Drop for VmPgtableGuard<'_> {
    fn drop(&mut self) { Vm::vm_pgtable_unlock(self.vm_ptr); }
}
impl<'a> core::ops::Deref for VmPgtableGuard<'a> {
    type Target = vm;
    fn deref(&self) -> &vm {
        // SAFETY: `VmPgtableGuard` is only ever constructed by
        // `lock_vm_pgtable`, which has just called `vm_pgtable_lock`
        // (spinlock acquire); the returned reference's lifetime is bound
        // to `&self`, cannot outlive the lock hold (`Drop` releases it
        // via `vm_pgtable_unlock`). This guard protects PTE-level
        // mutation through `vm->pagetable`; the `pagetable` pointer value
        // itself is set once at `vm_init`/torn down at `__vm_destroy` and
        // otherwise read-only, so a shared borrow of `vm` here is sound
        // without also requiring the rwsem.
        unsafe { &*self.vm_ptr }
    }
}

// ---------------------------------------------------------------------------
// V3 third slice: vm_put + __vm_destroy
// ---------------------------------------------------------------------------

/// Poison value written to `vm->pagetable` after destruction to catch
/// accidental reuse. Matches the C `VM_DESTROYED_MAGIC`.
const VM_DESTROYED_MAGIC: u64 = 0xDEAD0BADu64;



// ---------------------------------------------------------------------------
// V3 fourth slice: vm_init
// ---------------------------------------------------------------------------

/// Lock name strings — kept as static byte arrays so we can pass stable
/// `*mut c_char` / `*const c_char` into the C lock initialisers.
static VM_PGTABLE_LOCK_NAME: &[u8] = b"vm_pgtable_lock\0";
static VM_RW_LOCK_NAME: &[u8] = b"vm_rw_lock\0";


// ---------------------------------------------------------------------------
// V3 fifth slice: vm_find_area + either_copyin/out
// ---------------------------------------------------------------------------


/// Copy data to either a user or kernel destination, dispatched on
/// `user_dst`. Mirrors the C `either_copyout`.
pub(crate) fn either_copyout(
    user_dst: c_int,
    dst: u64,
    src: *mut c_void,
    len: u64,
) -> c_int {
    if user_dst != 0 {
        let cur_vm = Vm::xv6_vm_current_vm();
        Vm::vm_copyout(cur_vm, dst, src as *const c_void, len)
    } else {
        // SAFETY: caller guarantees `dst`/`src` are valid for `len` bytes
        // and non-overlapping-or-`memmove`-safe kernel pointers when
        // `user_dst == 0` (kernel-to-kernel copy path).
        unsafe { memmove(dst as *mut c_void, src as *const c_void, len as usize) };
        0
    }
}

/// Copy data from either a user or kernel source, dispatched on `user_src`.
/// Mirrors the C `either_copyin`.
pub(crate) fn either_copyin(
    dst: *mut c_void,
    user_src: c_int,
    src: u64,
    len: u64,
) -> c_int {
    if user_src != 0 {
        let cur_vm = Vm::xv6_vm_current_vm();
        Vm::vm_copyin(cur_vm, dst, src, len)
    } else {
        // SAFETY: caller guarantees `dst`/`src` are valid for `len` bytes
        // when `user_src == 0` (kernel-to-kernel copy path).
        unsafe { memmove(dst, src as *const c_void, len as usize) };
        0
    }
}

// ---------------------------------------------------------------------------
// V4 first slice: vm_find_free_range (leaf)
// ---------------------------------------------------------------------------

#[inline]
fn pg_round_up(x: u64) -> u64 {
    (x + (PGSIZE as u64 - 1)) & !(PGSIZE as u64 - 1)
}



// ---------------------------------------------------------------------------
// V4 second slice: vma_split
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// V4 d/e/f: vma_alloc, vma_free, vma_validate
// ---------------------------------------------------------------------------





// ===========================================================================
// V5: Remaining vm.c port — copyin/out/instr, vm_copy, heap/stack create/grow,
//     mmap region helpers, mprotect/mremap/msync/mincore/madvise, thread
//     stack allocators, top-level vm_mmap/vm_munmap.
// ===========================================================================


// ---------------------------------------------------------------------------
// V5.1: user-memory access — vm_copyout / vm_copyin / vm_copyinstr
// ---------------------------------------------------------------------------




// ---------------------------------------------------------------------------
// V5.2: vm_createheap / vm_createstack
// ---------------------------------------------------------------------------



// ---------------------------------------------------------------------------
// V5.3: vm_growstack / vm_try_growstack / vm_growheap
// ---------------------------------------------------------------------------




// ---------------------------------------------------------------------------
// V5.4: vm_copy (fork-time VM duplication)
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// V5.5: vm_mmap_region_locked / vm_mmap_region / vm_munmap_region
// ---------------------------------------------------------------------------




// ---------------------------------------------------------------------------
// V5.6: vm_mprotect / vm_mremap
// ---------------------------------------------------------------------------



// ---------------------------------------------------------------------------
// V5.7: vm_msync / vm_mincore / vm_madvise
// ---------------------------------------------------------------------------




// ---------------------------------------------------------------------------
// V5.8: vm_alloc_thread_stack / vm_free_thread_stack
// ---------------------------------------------------------------------------



// ---------------------------------------------------------------------------
// V5.9: vm_mmap / vm_munmap (top-level syscall paths)
// ---------------------------------------------------------------------------



// ---------------------------------------------------------------------------
// Phase 1A — final residual policy ported from kernel/mm/vm.c.
// vm.c is being deleted; the following functions absorb everything that used
// to live there. They keep the same C ABI (unsafe extern "C") so existing
// callers in other C translation units link unchanged.
// ---------------------------------------------------------------------------

// rb_root_opts static — replaces the C `__vm_tree_opts` global in vm.c.
// rb_root_opts contains function pointers; wrap to assert Sync (the struct
// is immutable for the lifetime of the kernel).
// SAFETY: `VmTreeOptsWrap` is only ever constructed as the single
// `const` `__VM_TREE_OPTS` below, whose fields are set once at
// compile time and never mutated afterward (no interior mutability,
// no `UnsafeCell`) — sharing an immutable value across harts is
// inherently data-race-free.
#[repr(transparent)]
struct VmTreeOptsWrap(rb_root_opts);
unsafe impl Sync for VmTreeOptsWrap {}
static __VM_TREE_OPTS: VmTreeOptsWrap = VmTreeOptsWrap(rb_root_opts {
    keys_cmp_fun: Some(__vma_cmp),
    get_key_fun: Some(__cma_get_key),
});

/// Replacement for the C `xv6_vm_rb_init_vm_tree` shim. Initialises a
/// `struct rb_root` with the static `__VM_TREE_OPTS` (vma key cmp/extract).
/// Inlines the body of `rb_root_init` (a `static inline` in bintree.h that
/// merely validates and stores the opts pointer).
pub(crate) fn xv6_vm_rb_init_vm_tree(root: *mut rb_root) {
    if root.is_null() {
        return;
    }
    // SAFETY: `root` checked non-null above; caller supplies a live
    // `rb_root` about to be initialised.
    unsafe {
        (*root).node = core::ptr::null_mut();
        (*root).opts = &__VM_TREE_OPTS.0 as *const rb_root_opts as *mut rb_root_opts;
    }
}


// PA → PTE conversion: PA2PTE(pa) = (pa >> 12) << 10
#[inline(always)]
fn pa_to_pte(pa: u64) -> u64 {
    (pa >> 12) << 10
}







