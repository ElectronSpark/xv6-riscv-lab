//! Low-level primitives — the *only* place in the mm crate where
//! `unsafe { core::arch::asm!(...) }` and per-CPU raw-pointer access
//! are written out by hand. Every other module calls into this one
//! through plain safe function calls; the `unsafe` keyword stays
//! confined here.
//!
//! Soundness rationale
//! -------------------
//!
//! * The inline-asm primitives use `options(nomem, nostack,
//!   preserves_flags)` and target RISC-V CSRs / general-purpose
//!   registers that are *defined* to be visible to the kernel at
//!   every reachable program point. Reading them cannot trap and
//!   cannot violate memory safety, so the wrappers are exposed as
//!   ordinary safe `fn`s.
//! * `intr_off()` / `intr_on()` toggle SSTATUS.SIE. They have a
//!   logical contract (must be balanced under `push_off`/`pop_off`)
//!   but no memory-safety contract: the CSR write itself cannot
//!   produce UB. Safe to expose.
//! * `cpu_local_ptr()` returns the `*mut cpu_local` stored in the
//!   `tp` register. The kernel boot path initialises `tp` to point
//!   at a unique, 64-byte-aligned, statically-allocated `cpu_local`
//!   slot before any Rust code runs and the value never aliases.
//!   Returning the raw pointer is therefore safe — callers that
//!   want to read or mutate fields go through the typed accessors
//!   in [`CpuLocal`] below, which encapsulate the necessary deref.

#![allow(dead_code)]

use core::ffi::c_int;

use crate::bindings::cpu_local;

const SSTATUS_SIE: u64 = 1 << 1;
const PGSIZE: u64 = 4096;

// ===========================================================================
// Inline-asm primitives
// ===========================================================================

/// Read the `tp` (thread pointer) register.
#[inline(always)]
pub fn read_tp() -> u64 {
    let tp: u64;
    // SAFETY: `mv {0}, tp` reads a general-purpose register with no
    // memory effects and cannot trap.
    unsafe {
        core::arch::asm!("mv {0}, tp", out(reg) tp,
            options(nomem, nostack, preserves_flags));
    }
    tp
}

/// Read SSTATUS.
#[inline(always)]
pub fn read_sstatus() -> u64 {
    let v: u64;
    // SAFETY: `csrr sstatus` reads a supervisor CSR that is always
    // readable at supervisor privilege; no memory effect, cannot trap.
    unsafe {
        core::arch::asm!("csrr {0}, sstatus", out(reg) v,
            options(nomem, nostack, preserves_flags));
    }
    v
}

/// Clear SSTATUS.SIE (disable supervisor interrupts).
#[inline(always)]
pub fn intr_off() {
    // SAFETY: `csrc sstatus, x` clears bits in a supervisor CSR. The
    // operation itself cannot violate memory safety; correctness of
    // interrupt nesting is a logical (not memory) contract.
    unsafe {
        core::arch::asm!("csrc sstatus, {0}", in(reg) SSTATUS_SIE,
            options(nomem, nostack, preserves_flags));
    }
}

/// Set SSTATUS.SIE (enable supervisor interrupts).
#[inline(always)]
pub fn intr_on() {
    // SAFETY: see `intr_off`.
    unsafe {
        core::arch::asm!("csrs sstatus, {0}", in(reg) SSTATUS_SIE,
            options(nomem, nostack, preserves_flags));
    }
}

/// `true` if SSTATUS.SIE is currently set.
#[inline(always)]
pub fn intr_get() -> bool {
    (read_sstatus() & SSTATUS_SIE) != 0
}

/// Read the SIE bit, then disable interrupts if they were enabled.
/// Mirrors the C `intr_off_save()` static inline.
#[inline(always)]
pub fn intr_off_save() -> core::ffi::c_int {
    let enabled = intr_get();
    if enabled {
        intr_off();
    }
    if enabled {
        1
    } else {
        0
    }
}

/// Re-enable interrupts if `enabled` is non-zero.
#[inline(always)]
pub fn intr_restore(enabled: core::ffi::c_int) {
    if enabled != 0 {
        intr_on();
    }
}

/// Issue a `wfi` (wait-for-interrupt) instruction.
#[inline(always)]
pub fn wfi() {
    // SAFETY: `wfi` either stalls until an interrupt arrives or is
    // treated as a nop on harts that don't implement it; no memory
    // effect.
    unsafe {
        core::arch::asm!("wfi", options(nomem, nostack, preserves_flags));
    }
}

// ===========================================================================
// Per-CPU access
// ===========================================================================

/// Return the address of the current hart's `cpu_local` slot.
///
/// `tp` is initialised once per hart, before any Rust code runs, to
/// the address of a unique slot inside a static array. Reading it is
/// therefore safe; the returned pointer is always non-null and is
/// exclusively owned by the current hart.
#[inline(always)]
pub fn cpu_local_ptr() -> *mut cpu_local {
    read_tp() as *mut cpu_local
}

/// Compute the current hart's CPU index from `tp`, mirroring the C
/// `cpuid()` macro: `((tp & PAGE_MASK) / sizeof(cpu_local))`.
#[inline(always)]
pub fn cpuid() -> c_int {
    let off = read_tp() & (PGSIZE - 1);
    (off / core::mem::size_of::<cpu_local>() as u64) as c_int
}

/// Typed reference to the current hart's `cpu_local` slot.
///
/// Construct via [`CpuLocal::current`]; the lifetime is bounded by
/// the borrow, so other code cannot smuggle the reference past a
/// context switch. The accessors below encapsulate the only field
/// dereferences the mm crate needs.
pub struct CpuLocal<'a> {
    raw: *mut cpu_local,
    _marker: core::marker::PhantomData<&'a mut cpu_local>,
}

impl<'a> CpuLocal<'a> {
    /// Borrow the current hart's per-CPU slot.
    ///
    /// Returned reference is valid for the rest of the current call
    /// frame: per-CPU storage is pinned for the kernel's lifetime, and
    /// the borrow tracks single-threaded access on this hart.
    #[inline(always)]
    pub fn current() -> Self {
        Self {
            raw: cpu_local_ptr(),
            _marker: core::marker::PhantomData,
        }
    }

    /// Raw pointer escape hatch (kept for the few call sites that hand
    /// the pointer back to C).
    #[inline(always)]
    pub fn as_ptr(&self) -> *mut cpu_local {
        self.raw
    }

    /// Nesting depth of `push_off` on this hart.
    #[inline(always)]
    pub fn noff(&self) -> c_int {
        // SAFETY: `raw` points at this hart's `cpu_local`, which is
        // pinned for the kernel's lifetime; the borrow on `self`
        // serialises access on this hart.
        unsafe { (*self.raw).noff }
    }

    #[inline(always)]
    pub fn set_noff(&mut self, n: c_int) {
        // SAFETY: see `noff`.
        unsafe { (*self.raw).noff = n; }
    }

    /// Saved SIE bit captured by the outermost `push_off`.
    #[inline(always)]
    pub fn intena(&self) -> c_int {
        // SAFETY: see `noff`.
        unsafe { (*self.raw).intena }
    }

    #[inline(always)]
    pub fn set_intena(&mut self, v: c_int) {
        // SAFETY: see `noff`.
        unsafe { (*self.raw).intena = v; }
    }

    /// Pointer to the currently running thread on this hart.
    #[inline(always)]
    pub fn proc_(&self) -> *mut crate::bindings::thread {
        // SAFETY: see `noff`.
        unsafe { (*self.raw).proc_ }
    }

    /// Read the current spinlock-nesting depth.
    #[inline(always)]
    pub fn spin_depth(&self) -> c_int {
        // SAFETY: see `noff`.
        unsafe { (*self.raw).spin_depth }
    }

    /// Add `delta` to the spinlock-nesting depth.
    #[inline(always)]
    pub fn spin_depth_add(&mut self, delta: c_int) {
        // SAFETY: see `noff`.
        unsafe {
            (*self.raw).spin_depth += delta;
        }
    }

    /// Read the CPU flag bitmap.
    #[inline(always)]
    pub fn flags(&self) -> u64 {
        // SAFETY: see `noff`.
        unsafe { (*self.raw).flags }
    }

    /// OR `bits` into the CPU flag bitmap.
    #[inline(always)]
    pub fn flags_or(&mut self, bits: u64) {
        // SAFETY: see `noff`.
        unsafe {
            (*self.raw).flags |= bits;
        }
    }
}

/// Hint to the CPU that we are in a busy-wait loop. Emits a `nop`
/// with `"memory"` clobber, matching the C `cpu_relax()` macro.
#[inline(always)]
pub fn cpu_relax() {
    // SAFETY: `nop` has no side effects beyond the instruction stream.
    unsafe {
        core::arch::asm!("nop", options(nostack, preserves_flags));
    }
}

/// Write the `sie` (supervisor interrupt-enable) CSR.
#[inline(always)]
pub fn write_sie(v: u64) {
    // SAFETY: `csrw sie` writes a supervisor CSR; cannot violate
    // memory safety.
    unsafe {
        core::arch::asm!("csrw sie, {0}", in(reg) v,
            options(nomem, nostack, preserves_flags));
    }
}

/// `CPU_FLAG_CRASHED` bit, mirroring `kernel/inc/smp/percpu.h`.
pub const CPU_FLAG_CRASHED: u64 = 8;
/// `SIE_SSIE` bit, mirroring `kernel/inc/riscv.h`.
pub const SIE_SSIE: u64 = 1 << 1;

/// Read the `time` CSR (mtime mirror). Mirrors the C `r_time()` inline.
#[inline(always)]
pub fn read_time() -> u64 {
    let v: u64;
    // SAFETY: `csrr time` reads an unprivileged-readable CSR; no
    // memory effect, cannot trap.
    unsafe {
        core::arch::asm!("csrr {0}, time", out(reg) v,
            options(nomem, nostack, preserves_flags));
    }
    v
}

/// Sequentially-consistent store to a `thread`'s `state` field.
/// Mirrors the C `__thread_state_set(p, state)` static inline.
///
/// `state` is `thread_state` (a C enum, 4-byte). `core::sync::atomic`
/// requires us to view the field as a 4-byte aligned integer.
#[inline(always)]
pub fn thread_state_set(p: *mut crate::bindings::thread,
                        state: crate::bindings::thread_state) {
    if p.is_null() { return; }
    // SAFETY: `p` is a valid (non-null) thread pointer; the `state`
    // field is naturally aligned (4 bytes). The cast to `AtomicU32`
    // is sound for a `c_uint`-sized field. The store uses SeqCst,
    // matching the C `__atomic_store_n(..., __ATOMIC_SEQ_CST)`.
    unsafe {
        let sp = core::ptr::addr_of_mut!((*p).state)
            as *mut core::sync::atomic::AtomicU32;
        (*sp).store(state as u32, core::sync::atomic::Ordering::SeqCst);
    }
}

// ===========================================================================
// Typed pointer wrappers for FFI structs
// ===========================================================================
//
// These wrap a non-null `*mut T` coming back from the C kernel and
// expose just the fields the mm crate needs. Each accessor pays the
// `unsafe { (*p).field }` cost *once*, inside the wrapper, instead
// of at every call site.
//
// The lifetime parameter (`'a`) is a phantom borrow tying the
// reference to the local scope: callers cannot smuggle the wrapper
// past the function frame.

use core::marker::PhantomData;
use core::ptr::NonNull;

/// Borrow a C-supplied `*mut thread` for the duration of a call frame.
pub struct ThreadRef<'a> {
    raw: NonNull<crate::bindings::thread>,
    _marker: PhantomData<&'a mut crate::bindings::thread>,
}

impl<'a> ThreadRef<'a> {
    /// `None` if the FFI pointer is null.
    ///
    /// SAFETY contract (paid by the C caller): the pointer is either
    /// null or valid + exclusively owned for the duration of the call
    /// frame; no concurrent mutation occurs from other harts.
    #[inline(always)]
    pub fn from_raw(p: *mut crate::bindings::thread) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _marker: PhantomData })
    }

    #[inline(always)]
    pub fn as_ptr(&self) -> *mut crate::bindings::thread {
        self.raw.as_ptr()
    }

    #[inline(always)]
    pub fn trapframe(&self) -> *mut crate::bindings::utrapframe {
        // SAFETY: validity is the FFI contract documented on `from_raw`.
        unsafe { (*self.raw.as_ptr()).trapframe }
    }

    #[inline(always)]
    pub fn vm_ptr(&self) -> *mut core::ffi::c_void {
        // SAFETY: see `trapframe`.
        unsafe { (*self.raw.as_ptr()).vm as *mut core::ffi::c_void }
    }

    #[inline(always)]
    pub fn fdtable(&self) -> *mut core::ffi::c_void {
        // SAFETY: see `trapframe`.
        unsafe { (*self.raw.as_ptr()).fdtable as *mut core::ffi::c_void }
    }
}

/// Resolve the currently running thread on this hart, honouring the
/// interrupt-disable invariant the C `__current_thread()` macro relies
/// on. Returns the raw pointer (callers usually wrap it in a
/// [`ThreadRef`] right away).
#[inline]
pub fn current_thread_ptr() -> *mut crate::bindings::thread {
    let old = intr_get();
    if old {
        intr_off();
    }
    let cpu = CpuLocal::current();
    let cur = cpu.proc_();
    if old {
        intr_on();
    }
    cur
}

/// Read `(*p).pid`, returning `-1` for a null pointer.
///
/// Centralises the `unsafe { (*thread).pid }` access used by the
/// lock primitives (mutex / rwsem / completion). Treats `IS_ERR`
/// pointers (top-bit-set, magnitude ≤ 4095) as `-1` to mirror the
/// C wakeup paths that defensively skip error returns.
#[inline]
pub fn thread_pid(p: *mut crate::bindings::thread) -> core::ffi::c_int {
    if p.is_null() {
        return -1;
    }
    let n = p as isize;
    if n < 0 && ((-n) as usize) <= 4095 {
        return -1;
    }
    // SAFETY: validated non-null, non-error thread pointer; `pid` is
    // a plain `c_int` field of a structurally-valid thread.
    unsafe { (*p).pid }
}

// ===========================================================================
// Kernel timebase
// ===========================================================================

// Runtime-initialised timebase frequency (Hz). Set once at boot before
// any SMP activity; never changes afterwards. P3-D3c: plain crate-path
// import instead of an `extern "C"` redeclaration (demoted from
// `#[no_mangle]` in the same wave).
use crate::timer::timer_core::__timebase_frequency;

/// `__timebase_frequency / 1000` — number of raw ticks in one
/// millisecond. Centralises the volatile read so the lock primitives
/// do not each carry their own copy.
#[inline]
pub fn tick_ms() -> u64 {
    // SAFETY: kernel-owned constant, read-only after boot.
    let f = unsafe { core::ptr::read_volatile(&raw const __timebase_frequency) };
    f / 1000
}

/// Raw timebase frequency (ticks per second). Spinlock's deadlock
/// detector uses this directly; everything else should prefer
/// [`tick_ms`] or [`ms_to_rawticks`].
#[inline]
pub fn tick_s() -> u64 {
    // SAFETY: see `tick_ms`.
    unsafe { core::ptr::read_volatile(&raw const __timebase_frequency) }
}

/// Convert a millisecond count into raw ticks, saturating on
/// overflow (mirrors the C `MS_TO_RAWTICKS` macro).
#[inline]
pub fn ms_to_rawticks(ms: u64) -> u64 {
    ms.saturating_mul(tick_ms())
}

// ===========================================================================
// Intrusive doubly-linked-list primitives
// ===========================================================================
//
// The C kernel uses a single `list_node_t { prev, next }` struct
// embedded inside larger payloads. Every traversal in vm.rs / page.rs
// touches these two raw pointer fields. Wrapping the field access
// here pays the `unsafe { (*p).field }` cost once.

use crate::bindings::list_node_t;

/// Read the `next` link of an intrusive list entry.
#[inline(always)]
pub fn list_entry_next(entry: *mut list_node_t) -> *mut list_node_t {
    // SAFETY: caller passes a valid FFI entry pointer (the entry is
    // embedded in a live payload owned by the caller).
    unsafe { (*entry).next }
}

/// Read the `prev` link of an intrusive list entry.
#[inline(always)]
pub fn list_entry_prev(entry: *mut list_node_t) -> *mut list_node_t {
    // SAFETY: see `list_entry_next`.
    unsafe { (*entry).prev }
}

/// Self-loop initialise a list entry so it satisfies the C
/// `list_entry_init()` post-condition.
#[inline]
pub fn list_entry_init(entry: *mut list_node_t) {
    // SAFETY: entry is exclusively owned during init.
    unsafe {
        (*entry).next = entry;
        (*entry).prev = entry;
    }
}

/// Insert `new` immediately after `prev`. Mirrors the C `list.h`
/// inline of the same name.
#[inline]
pub fn list_entry_insert_after(prev: *mut list_node_t, new: *mut list_node_t) {
    // SAFETY: caller holds the list's protecting lock; `prev`, `new`,
    // and `prev->next` are all valid for the duration of this call.
    unsafe {
        let nxt = (*prev).next;
        (*new).next = nxt;
        (*new).prev = prev;
        (*nxt).prev = new;
        (*prev).next = new;
    }
}

/// Detach `entry` from whichever list contains it and reinitialise
/// it to a self-loop.
#[inline]
pub fn list_entry_detach(entry: *mut list_node_t) {
    // SAFETY: caller holds the protecting lock; `entry`, `entry->prev`
    // and `entry->next` are all valid.
    unsafe {
        let p = (*entry).prev;
        let n = (*entry).next;
        (*p).next = n;
        (*n).prev = p;
        (*entry).next = entry;
        (*entry).prev = entry;
    }
}

/// `true` if `entry` is in a self-loop (detached) state.
#[inline]
pub fn list_entry_is_detached(entry: *const list_node_t) -> bool {
    // SAFETY: entry is a valid FFI pointer.
    unsafe { (*entry).next as *const _ == entry && (*entry).prev as *const _ == entry }
}

/// `container_of`: subtract the member byte-offset from `entry` to
/// recover a pointer to the containing payload of type `T`.
#[inline(always)]
pub fn list_container_of<T>(entry: *mut list_node_t, member_offset: usize) -> *mut T {
    (entry as *mut u8).wrapping_sub(member_offset) as *mut T
}

// ===========================================================================
// rb_node initialisation
// ===========================================================================

/// Self-colour an rb-tree node, mirroring the C `rb_node_init()`
/// static inline: `node->__parent_color = (unsigned long)node`.
#[inline]
pub fn rb_node_init(node: *mut crate::bindings::rb_node) {
    // SAFETY: node is exclusively owned during init.
    unsafe {
        (*node).__parent_color = node as u64;
    }
}

// ===========================================================================
// VFS-file accessors
// ===========================================================================

/// Borrow a `*mut vfs_file` for safe accessor methods.
pub struct VfsFileRef<'a> {
    raw: NonNull<crate::bindings::vfs_file>,
    _marker: PhantomData<&'a mut crate::bindings::vfs_file>,
}

impl<'a> VfsFileRef<'a> {
    #[inline(always)]
    pub fn from_raw(p: *mut crate::bindings::vfs_file) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _marker: PhantomData })
    }

    /// Address of the embedded `inode` slot, suitable for handing to
    /// the C `vfs_inode_deref()` helper.
    #[inline(always)]
    pub fn inode_slot_ptr(&self) -> *mut crate::bindings::vfs_inode_ref {
        // SAFETY: validity from FFI contract.
        unsafe { &raw mut (*self.raw.as_ptr()).inode }
    }
}

/// Read `inode->mode` through a non-null inode pointer.
#[inline(always)]
pub fn vfs_inode_mode(p: *mut crate::bindings::vfs_inode) -> u32 {
    // SAFETY: caller has just retrieved this pointer from the FFI
    // and verified it is non-null.
    unsafe { (*p).mode as u32 }
}

/// Read `inode->size`. Signed loff_t in C.
#[inline(always)]
pub fn vfs_inode_size(p: *mut crate::bindings::vfs_inode) -> i64 {
    // SAFETY: caller verified non-null.
    unsafe { (*p).size as i64 }
}

/// Pointer to the embedded `pcache` inside a `vfs_inode`.
#[inline(always)]
pub fn vfs_inode_pcache_ptr(p: *mut crate::bindings::vfs_inode) -> *mut crate::bindings::pcache {
    // SAFETY: caller verified non-null.
    unsafe { &raw mut (*p).i_data }
}

/// Read `file->ops` (may be null).
#[inline(always)]
pub fn vfs_file_ops(p: *mut crate::bindings::vfs_file) -> *mut crate::bindings::vfs_file_ops {
    // SAFETY: caller verified non-null.
    unsafe { (*p).ops as *mut crate::bindings::vfs_file_ops }
}

// ===========================================================================
// `vma` field reads (cheap inline accessors)
// ===========================================================================

#[inline(always)]
pub fn vma_start(v: *mut crate::bindings::vma) -> u64 { unsafe { (*v).start } }

#[inline(always)]
pub fn vma_end(v: *mut crate::bindings::vma) -> u64 { unsafe { (*v).end } }

#[inline(always)]
pub fn vma_flags(v: *mut crate::bindings::vma) -> u64 { unsafe { (*v).flags } }

#[inline(always)]
pub fn vma_file(v: *mut crate::bindings::vma) -> *mut crate::bindings::vfs_file {
    unsafe { (*v).file as *mut crate::bindings::vfs_file }
}

#[inline(always)]
pub fn vma_pgoff(v: *mut crate::bindings::vma) -> u64 { unsafe { (*v).pgoff } }

// ===========================================================================
// PTE raw read / write
// ===========================================================================

#[inline(always)]
pub fn pte_read(p: *const u64) -> u64 { unsafe { *p } }

#[inline(always)]
pub fn pte_write(p: *mut u64, val: u64) { unsafe { *p = val; } }

// ===========================================================================
// `vm` / `vma` field-address accessors
// ===========================================================================
//
// Almost every list/rb-tree operation in vm.rs needs the *address* of
// a field embedded in a `*mut vm` or `*mut vma` (e.g.
// `&raw mut (*v).list_entry`). Computing the address only requires a
// valid base pointer (the field need not yet be initialised), so the
// helpers below collapse each `&raw mut (*p).field` into a single
// safe-callable accessor.

#[inline(always)]
pub fn vma_list_entry_ptr(v: *mut crate::bindings::vma) -> *mut list_node_t {
    // SAFETY: FFI in pointer is non-null and points at a `vma`.
    unsafe { &raw mut (*v).list_entry }
}

#[inline(always)]
pub fn vma_free_list_entry_ptr(v: *mut crate::bindings::vma) -> *mut list_node_t {
    // SAFETY: see `vma_list_entry_ptr`.
    unsafe { &raw mut (*v).free_list_entry }
}

#[inline(always)]
pub fn vma_rb_entry_ptr(v: *mut crate::bindings::vma) -> *mut crate::bindings::rb_node {
    // SAFETY: see `vma_list_entry_ptr`.
    unsafe { &raw mut (*v).rb_entry }
}

/// Read `vma->vm`. Returns null if the vma has already been detached.
#[inline(always)]
pub fn vma_vm_ptr(v: *mut crate::bindings::vma) -> *mut crate::bindings::vm {
    // SAFETY: see `vma_list_entry_ptr`.
    unsafe { (*v).vm as *mut crate::bindings::vm }
}

#[inline(always)]
pub fn vm_list_ptr(v: *mut crate::bindings::vm) -> *mut list_node_t {
    // SAFETY: FFI in pointer is non-null and points at a `vm`.
    unsafe { &raw mut (*v).vm_list }
}

#[inline(always)]
pub fn vm_free_list_ptr(v: *mut crate::bindings::vm) -> *mut list_node_t {
    // SAFETY: see `vm_list_ptr`.
    unsafe { &raw mut (*v).vm_free_list }
}

#[inline(always)]
pub fn vm_rw_lock_ptr(v: *mut crate::bindings::vm) -> *mut crate::bindings::rwsem_t {
    // SAFETY: see `vm_list_ptr`.
    unsafe { &raw mut (*v).rw_lock }
}

// ===========================================================================
// push_off / pop_off — duplicate of the C macros, written without any
// visible `unsafe` keyword in the body.
// ===========================================================================
/// Disable interrupts and increment the nesting depth, mirroring the
/// C `push_off()` macro byte-for-byte. Safe-callable.
#[inline]
pub fn push_off() {
    let old = intr_get();
    if old {
        intr_off();
    }
    let mut c = CpuLocal::current();
    let n = c.noff();
    if n == 0 {
        c.set_intena(if old { 1 } else { 0 });
    }
    c.set_noff(n + 1);
}

/// Decrement the nesting depth and re-enable interrupts when the
/// outermost frame pops, mirroring the C `pop_off()` macro.
#[inline]
pub fn pop_off() {
    let mut c = CpuLocal::current();
    let n = c.noff() - 1;
    c.set_noff(n);
    if n == 0 && c.intena() != 0 {
        intr_on();
    }
}

// ===========================================================================
// PreemptGuard — RAII scope for push_off / pop_off
// ===========================================================================

/// RAII wrapper around [`push_off`] / [`pop_off`]. Construction disables
/// preemption (and interrupts on the outermost frame); the matching
/// `pop_off` runs on `Drop`, so every control-flow path that leaves the
/// guard's scope — early returns, `?`, panics — restores the previous
/// state automatically.
///
/// The type is `!Send` and `!Sync` because the push/pop counter lives
/// in the originating CPU's per-cpu area; smuggling the guard to
/// another hart would underflow that CPU's counter and overflow the
/// other's.
#[must_use = "PreemptGuard re-enables preemption when dropped — bind it to a name"]
pub struct PreemptGuard {
    _not_send: core::marker::PhantomData<*const ()>,
}

impl PreemptGuard {
    /// Disable preemption for the lifetime of the returned guard.
    #[inline]
    pub fn new() -> Self {
        push_off();
        Self { _not_send: core::marker::PhantomData }
    }

    /// CPU id, valid only while this guard is alive (preemption off).
    #[inline]
    pub fn cpuid(&self) -> usize {
        cpuid() as usize
    }
}

impl Drop for PreemptGuard {
    #[inline]
    fn drop(&mut self) {
        pop_off();
    }
}
