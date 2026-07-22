//! Pagetable primitives — Rust port of vm.c V1 chunk.
//!
//! Ports: `walk`, `walkaddr`, `kvmmap`, `mappages`, `uvmunmap`,
//! `uvmcreate`, `freewalk`, `uvmfree`, `kvminit`, `kvminithart`,
//! `dump_pagetable`, `vm_dump_flags`, and the static helpers
//! `__pgtab_alloc` and `__pgtab_free`.
//!
//! Algorithmic logic lives in safe Rust against the `PageTable` / `Pte`
//! newtypes; the only `unsafe` is the FFI boundary (`mod ffi`), pointer
//! construction from raw addresses, and the `#[no_mangle] extern "C"`
//! entry points.


use core::ffi::{c_char, c_int, c_void};
use core::ptr::NonNull;
use crate::machine;

// ---------------------------------------------------------------------------
// Sv39 page-table constants (mirror of riscv.h)
// ---------------------------------------------------------------------------
pub const PGSIZE:  u64 = 4096;
pub const PGSHIFT: u32 = 12;

pub const PTE_V:     u64 = 1 << 0;
pub const PTE_R:     u64 = 1 << 1;
pub const PTE_W:     u64 = 1 << 2;
pub const PTE_X:     u64 = 1 << 3;
pub const PTE_U:     u64 = 1 << 4;
pub const PTE_A:     u64 = 1 << 6;
pub const PTE_D:     u64 = 1 << 7;
pub const PTE_RSW_W: u64 = 1 << 8;

pub const PROT_READ:     u64 = 1;
pub const PROT_WRITE:    u64 = 2;
pub const PROT_EXEC:     u64 = 4;
pub const VMA_FLAG_USER: u64 = 1 << 8;

pub const fn pgroundup(sz: u64)      -> u64 { (sz + PGSIZE - 1) & !(PGSIZE - 1) }
pub const fn pgrounddown(a: u64)     -> u64 { a & !(PGSIZE - 1) }
pub const fn pa2pte(pa: u64)         -> u64 { (pa >> 12) << 10 }
pub const fn pte2pa(pte: u64)        -> u64 { (pte >> 10) << 12 }
pub const fn pte_flags(pte: u64)     -> u64 { pte & 0x3FF & !(PTE_A | PTE_D) }
pub const fn pxshift(level: u32)     -> u32 { PGSHIFT + 9 * level }
pub const fn px(level: u32, va: u64) -> usize {
    ((va >> pxshift(level)) & 0x1FF) as usize
}

fn trampoline_pte_idx() -> usize {
    let maxva = crate::bindings::MAXVA;
    let trampoline = maxva - PGSIZE;
    ((trampoline >> 30) & 0x1FF) as usize
}

// ---------------------------------------------------------------------------
// FFI boundary.
// ---------------------------------------------------------------------------

/// Canonical `Page` type — the real `#[repr(C, align(64))]` mirror defined
/// in `page.rs`. Previously this module carried its own opaque `[u8; 0]`
/// stand-in with the same name (latent type confusion flagged by the mm
/// audit); every other `mm` submodule that needs a page pointer now goes
/// through `page.rs`'s definition too.
use crate::mm::page::Page;

mod ffi {
    use super::*;

    // `__panic_start`/`__panic_end` are plain crate-path re-exports
    // (P3-D3c: `printf.rs` dropped their `#[no_mangle]` exports, so
    // `cffi::raw`'s re-export is `pub(crate)` now -- match it here).
    pub(crate) use crate::mm::cffi::raw::{__panic_start, __panic_end};

    // P3-1C mesh sweep: `uart.rs` is in scope for this wave; its
    // `__uart0_mmio_base` is now referenced via crate path. It's a
    // `static mut` (single boot-time writer, `dev/fdt.rs`, before
    // secondary harts start -- same convention as the FDT-published
    // globals above), so this file's `pub safe static` extern mirror (which
    // could be read without `unsafe`) becomes a thin accessor fn instead of
    // a bare re-export.
    pub fn __uart0_mmio_base() -> u64 {
        // SAFETY: read-only after boot-time FDT probe, which completes
        // before this function's only caller (`kvmmake`, boot-hart only).
        unsafe { crate::uart::__uart0_mmio_base }
    }

    // P3-1D mesh sweep: `pci.rs`/`e1000.rs` are in scope for this wave;
    // same "`static mut` -> thin accessor fn" treatment as
    // `__uart0_mmio_base` above (both demoted from `#[no_mangle]` in the
    // same wave, this file being their only other reader).
    pub fn __pcie_ecam_mmio_base() -> u64 {
        // SAFETY: read-only after boot-time FDT probe, which completes
        // before this function's only caller (`kvmmake`, boot-hart only).
        unsafe { crate::pci::__pcie_ecam_mmio_base }
    }
    pub fn __e1000_pci_mmio_base() -> u64 {
        // SAFETY: see `__pcie_ecam_mmio_base` above.
        unsafe { crate::e1000::__e1000_pci_mmio_base }
    }

    // P3-D3c mesh sweep: same "`static mut` -> thin accessor fn" treatment
    // for the runtime physical-memory bounds (`start_kernel.rs`), the MMIO
    // bases (`timer/goldfish_rtc.rs`, `irq/plic.rs`), the per-cpu array
    // base (`ipi.rs`) and the boot-probed platform info (`dev/fdt.rs`) --
    // all demoted from `#[no_mangle]` in the same wave.
    pub fn __physical_memory_start() -> u64 {
        // SAFETY: written only during single-hart early boot
        // (`start_kernel.rs`/`dev/fdt.rs`), read-only afterwards.
        unsafe { crate::start_kernel::__physical_memory_start }
    }
    pub fn __physical_memory_end() -> u64 {
        // SAFETY: see `__physical_memory_start` above.
        unsafe { crate::start_kernel::__physical_memory_end }
    }
    pub fn __goldfish_rtc_mmio_base() -> u64 {
        // SAFETY: boot-time-constant (never written after its initializer;
        // see that file's module doc), read-only for the kernel's life.
        unsafe { crate::timer::goldfish_rtc::__goldfish_rtc_mmio_base }
    }
    pub fn __plic_mmio_base() -> u64 {
        // SAFETY: single boot-time writer (`dev/fdt.rs`), read-only after.
        unsafe { crate::irq::plic::__plic_mmio_base }
    }
    pub fn cpus_base() -> u64 {
        // Address-of only (no dereference): `&raw const` on a (non-extern)
        // `static mut` is a safe operation.
        (&raw const crate::ipi::cpus) as u64
    }
    pub fn platform() -> &'static crate::bindings::platform_info {
        // SAFETY: populated once by `fdt_init()` during single-hart early
        // boot, read-only by the time `kvmmake` runs; shared-borrowing it
        // afterwards is race-free.
        unsafe { &*(&raw const crate::dev::fdt::platform) }
    }

    unsafe extern "C" {
        // memory / pages


        // Slab-pool init for the vma/vm-area allocators (implemented in
        // vm.rs); formerly wrapped as `xv6_vm_vma_pool_init`/
        // `xv6_vm_vm_pool_init` by the deleted `vm_pgtab_shims.rs` for no
        // reason beyond naming symmetry -- call directly.

        // Kernel image / per-cpu / trampoline symbols.
        pub safe static _entry: u8;
        pub safe static etext: u8;
        pub safe static _rodata: u8;
        pub safe static _rodata_end: u8;
        pub safe static _data: u8;
        pub safe static _data_end: u8;
        pub safe static _bss: u8;
        pub safe static _bss_end: u8;
        pub safe static trampoline: u8;
        pub safe static _trampoline_data: u8;
        pub safe static sig_trampoline: u8;
        pub safe static _data_ktlb: u8;

        // Kernel symbols sections.
        pub safe static _ksymbols_start: u8;
        pub safe static _ksymbols_end: u8;
        pub safe static _ksymbols_idx_start: u8;
        pub safe static _ksymbols_idx_end: u8;

        // Defined in trampoline.S.
        pub safe static mut trampoline_ksatp: u64;

        // printf is variadic, so it cannot be declared `safe`.
    }
pub(crate) use crate::mm::vm::xv6_vm_cpuid;
pub(crate) use crate::mm::vm::{Vm, Vma};

    // The functions below are genuinely `unsafe fn`/`unsafe extern "C" fn`
    // in `crate::mm::{page,string}`; this file's original extern
    // declaration asserted `pub safe fn` (usual FFI facade) and also typed
    // `page_alloc`'s/`page_free`'s `order`/`ptype` as `c_int` rather than
    // the real `u64`, and `__pa_to_page`'s return as `*mut c_void` rather
    // than `crate::mm::page::Page` (same layout, different Rust name).
    /// SAFETY: see [`crate::string::memset`]'s contract.
    pub fn memset(s: *mut c_void, c: c_int, n: usize) -> *mut c_void {
        unsafe { crate::string::memset(s, c, n) }
    }
    /// SAFETY: see [`crate::mm::page::page_alloc`]'s contract.
    pub fn page_alloc(order: c_int, ptype: c_int) -> *mut c_void {
        unsafe { crate::mm::page::page_alloc(order as u64, ptype as u64) }
    }
    /// SAFETY: `pa` must originate from `page_alloc` above.
    pub fn page_free(pa: *mut c_void, order: c_int) {
        unsafe { crate::mm::page::page_free(pa, order as u64) };
    }
    /// SAFETY: see [`crate::mm::page::__pa_to_page`]'s contract.
    pub fn __pa_to_page(pa: u64) -> *mut c_void {
        unsafe { crate::mm::page::__pa_to_page(pa) as *mut c_void }
    }
    /// SAFETY: `p` must be a live `Page` (`crate::mm::page::page_lock_acquire`'s contract).
    pub fn page_lock_acquire(p: *mut Page) {
        unsafe { crate::mm::page::page_lock_acquire(p) };
    }
    /// SAFETY: see `page_lock_acquire` above.
    pub fn page_lock_release(p: *mut Page) {
        unsafe { crate::mm::page::page_lock_release(p) };
    }
}

/// `xv6_vm_panic` is called from both this module and `vm.rs` (out of
/// scope for this refactor), so it must keep its exported name and
/// signature; its body is now a direct `printf` + panic instead of a
/// round-trip through the deleted `vm_pgtab_shims.rs`.
pub(crate) fn xv6_vm_panic(msg: *const c_char) -> ! {
    ffi::__panic_start();
    crate::kprintln!("PANIC: {}", crate::printf::Cs(msg));
    ffi::__panic_end()
}

fn panic_vm(msg: &[u8]) -> ! {
    debug_assert!(msg.last() == Some(&0));
    xv6_vm_panic(msg.as_ptr() as *const c_char)
}

// ---------------------------------------------------------------------------
// mappages / uvmunmap panic helpers -- previously `xv6_vm_panic_mappages_*`
// / `xv6_vm_panic_uvmunmap_notmapped` in the deleted `vm_pgtab_shims.rs`.
// Only this module calls them, so they are plain private fns now.
// ---------------------------------------------------------------------------
fn panic_mappages_va(va: u64) -> ! {
    ffi::__panic_start();
    crate::kprintln!("mappages: va not aligned, va {}", crate::printf::Ptr(va));
    ffi::__panic_end()
}
fn panic_mappages_size(va: u64, size: u64) -> ! {
    ffi::__panic_start();
    crate::kprintln!(
        "mappages: size not aligned, va {}, size {}",
        crate::printf::Ptr(va),
        crate::printf::Ptr(size),
    );
    ffi::__panic_end()
}
fn panic_mappages_zero(va: u64) -> ! {
    ffi::__panic_start();
    crate::kprintln!("mappages: size zero, va {}", crate::printf::Ptr(va));
    ffi::__panic_end()
}
fn panic_mappages_remap(a: u64) -> ! {
    ffi::__panic_start();
    crate::kprintln!("mappages: remap, {}", crate::printf::Ptr(a));
    ffi::__panic_end()
}
fn panic_uvmunmap_notmapped(va: u64, pa: u64, flags: u64) -> ! {
    ffi::__panic_start();
    crate::kprintln!(
        "uvmunmap: not mapped, va={}, pa={}, flags: {:x}",
        crate::printf::Ptr(va),
        crate::printf::Ptr(pa),
        flags,
    );
    ffi::__panic_end()
}

// ---------------------------------------------------------------------------
// printf helpers used by `dump_pagetable`/`kvminithart` -- previously
// `xv6_vm_printf_*` in the deleted `vm_pgtab_shims.rs`. Only this module
// calls them.
// ---------------------------------------------------------------------------
fn printf_kvminithart(hart: u64, satp: u64) {
    crate::kprintln!("hart {} switched to kernel page table, satp: {:x}", hart, satp);
}
fn printf_invalid_level(level: c_int) {
    crate::kprintln!("Invalid level {} for pagetable dump", level);
}
#[allow(clippy::too_many_arguments)]
fn printf_dump_pte_single(
    indent: c_int, idx: c_int, pte_ptr: *mut c_void,
    flags_no_v: u64,
    sv: *const c_char, su: *const c_char, sw: *const c_char,
    sx: *const c_char, sr: *const c_char, src: *const c_char,
    va: u64, pa: u64,
) {
    crate::kprintln!(
        "{:w$}{}PTE[{}]({}): {:x}({}{}{}{}{}{}), (va, pa): ({}, {})",
        "",
        crate::printf::Cs(b"\0".as_ptr() as *const c_char),
        idx,
        crate::printf::Ptr(pte_ptr as u64),
        flags_no_v,
        crate::printf::Cs(sv),
        crate::printf::Cs(su),
        crate::printf::Cs(sw),
        crate::printf::Cs(sx),
        crate::printf::Cs(sr),
        crate::printf::Cs(src),
        crate::printf::Ptr(va),
        crate::printf::Ptr(pa),
        w = indent as usize,
    );
}
#[allow(clippy::too_many_arguments)]
fn printf_dump_pte_range(
    indent: c_int, start_idx: c_int, end_idx: c_int,
    flags_no_v: u64,
    sv: *const c_char, su: *const c_char, sw: *const c_char,
    sx: *const c_char, sr: *const c_char, src: *const c_char,
    va_start: u64, va_end: u64, pa_start: u64, pa_end: u64,
    count: c_int,
) {
    crate::kprintln!(
        "{:w$}{}PTE[{}-{}]: {:x}({}{}{}{}{}{}), (va, pa): ({}-{}, {}-{}) [{} pages]",
        "",
        crate::printf::Cs(b"\0".as_ptr() as *const c_char),
        start_idx,
        end_idx,
        flags_no_v,
        crate::printf::Cs(sv),
        crate::printf::Cs(su),
        crate::printf::Cs(sw),
        crate::printf::Cs(sx),
        crate::printf::Cs(sr),
        crate::printf::Cs(src),
        crate::printf::Ptr(va_start),
        crate::printf::Ptr(va_end),
        crate::printf::Ptr(pa_start),
        crate::printf::Ptr(pa_end),
        count,
        w = indent as usize,
    );
}
#[allow(clippy::too_many_arguments)]
fn printf_dump_pte_inner(
    indent: c_int, idx: c_int, pte_ptr: *mut c_void,
    flags: u32,
    sv: *const c_char, su: *const c_char, sw: *const c_char,
    sx: *const c_char, sr: *const c_char, src: *const c_char,
    va: u64, pa: *mut c_void,
) {
    crate::kprint!(
        "{:w$}{}PTE[{}]({}): {:x}({}{}{}{}{}{}), (va, pa): ({}, {})",
        "",
        crate::printf::Cs(b"\0".as_ptr() as *const c_char),
        idx,
        crate::printf::Ptr(pte_ptr as u64),
        ((flags as i32) as i64) as u64,
        crate::printf::Cs(sv),
        crate::printf::Cs(su),
        crate::printf::Cs(sw),
        crate::printf::Cs(sx),
        crate::printf::Cs(sr),
        crate::printf::Cs(src),
        crate::printf::Ptr(va),
        crate::printf::Ptr(pa as u64),
        w = indent as usize,
    );
}
fn printf_newline() {
    crate::kprintln!();
}
fn printf_colon_newline() {
    crate::kprintln!(":");
}

// ---------------------------------------------------------------------------
// Constant accessors -- previously `xv6_vm_kernbase`/`xv6_vm_physstop`/
// `xv6_vm_einval`/`xv6_vm_erange`/`xv6_vm_enomem` round-trips through the
// deleted `vm_pgtab_shims.rs`; `MAXVA` is already a bindgen constant and
// needs no accessor at all.
// ---------------------------------------------------------------------------
const EINVAL: c_int = crate::bindings::EINVAL as c_int;
const ERANGE: c_int = crate::bindings::ERANGE as c_int;

#[inline]
fn kernbase() -> u64 { ffi::__physical_memory_start() }
#[inline]
fn physstop() -> u64 { ffi::__physical_memory_end() }

// ---------------------------------------------------------------------------
// Page-type helpers -- previously `xv6_vm_page_type_pgtable`/
// `xv6_vm_page_is_pgtable` in the deleted `vm_pgtab_shims.rs`.
// ---------------------------------------------------------------------------
const PAGE_TYPE_PGTABLE: c_int = 3;
const PAGE_FLAG_TYPE_MASK: u64 = (1u64 << 8) - 1;

fn page_is_pgtable(p: *mut Page) -> bool {
    if p.is_null() {
        return false;
    }
    // SAFETY: `p` is a live page-array entry (checked non-null above; every
    // caller passes a pointer resolved via `__pa_to_page`).
    let flags = unsafe { (*p).flags };
    (flags & PAGE_FLAG_TYPE_MASK) as c_int == PAGE_TYPE_PGTABLE
}

// ---------------------------------------------------------------------------
// kernel_pagetable storage -- previously in the deleted
// `vm_pgtab_shims.rs`.
// ---------------------------------------------------------------------------
pub(crate) static mut kernel_pagetable: *mut u64 = core::ptr::null_mut();

fn kernel_pagetable_get() -> *mut u64 {
    // SAFETY: addr-of read of a `static mut`; written once by `PageTable::kvminit()`
    // on the single boot CPU before any other hart can observe it.
    unsafe { *(&raw const kernel_pagetable) }
}
fn kernel_pagetable_set(pt: *mut u64) {
    // SAFETY: only `PageTable::kvminit()` writes this, once, at boot.
    unsafe { (&raw mut kernel_pagetable).write(pt) };
}

// ---------------------------------------------------------------------------
// SATP / trampoline_ksatp -- previously `xv6_vm_make_satp`/
// `xv6_vm_set_trampoline_ksatp`/`xv6_vm_get_trampoline_ksatp`/
// `xv6_vm_w_satp` in the deleted `vm_pgtab_shims.rs`.
// ---------------------------------------------------------------------------
#[inline]
fn make_satp(pt: *mut u64) -> u64 {
    // SV39 = mode 8.
    (8u64 << 60) | ((pt as u64) >> 12)
}

fn set_trampoline_ksatp(v: u64) {
    // SAFETY: `trampoline_ksatp` is a boot-time-shared location written
    // once here and read by every hart via `get_trampoline_ksatp`; the
    // fence below publishes the write before any hart can observe it.
    unsafe {
        core::ptr::write_volatile(&raw mut ffi::trampoline_ksatp, v);
        core::sync::atomic::compiler_fence(core::sync::atomic::Ordering::Release);
        core::sync::atomic::fence(core::sync::atomic::Ordering::SeqCst);
    }
}
fn get_trampoline_ksatp() -> u64 {
    // SAFETY: paired with the volatile write + fence in
    // `set_trampoline_ksatp`.
    unsafe { core::ptr::read_volatile(&raw const ffi::trampoline_ksatp) }
}

fn w_satp(v: u64) {
    // SAFETY: writing `satp` is always valid; the caller (`kvminithart`)
    // has already fenced the TLB around this.
    unsafe { core::arch::asm!("csrw satp, {0}", in(reg) v, options(nostack, preserves_flags)); }
}

/// Also called from `vm.rs` (out of scope for this refactor) via its own
/// extern declaration, so it keeps its exported name.
pub(crate) fn xv6_vm_sfence_vma() {
    // SAFETY: `sfence.vma zero, zero` flushes the entire TLB; always valid.
    unsafe { core::arch::asm!("sfence.vma zero, zero", options(nostack, preserves_flags)); }
}

// ---------------------------------------------------------------------------
// `Pte` — semantic view of a single page-table entry's raw u64.
// ---------------------------------------------------------------------------
#[derive(Copy, Clone)]
struct Pte(u64);

impl Pte {
    fn raw(self)      -> u64  { self.0 }
    fn valid(self)    -> bool { self.0 & PTE_V != 0 }
    fn cow(self)      -> bool { self.0 & PTE_RSW_W != 0 }
    fn user(self)     -> bool { self.0 & PTE_U != 0 }
    fn flags(self)    -> u64  { pte_flags(self.0) }
    fn pa(self)       -> u64  { pte2pa(self.0) }
    fn is_leaf(self)  -> bool { self.flags() != PTE_V }
}

// ---------------------------------------------------------------------------
// `PageTable` — typed handle around a 512-entry, page-aligned u64 array.
// ---------------------------------------------------------------------------
#[derive(Copy, Clone)]
pub(crate) struct PageTable(NonNull<u64>);

impl PageTable {
    /// SAFETY: `ptr` must point to a PGSIZE-aligned page of 512 u64 entries
    /// (i.e. a page-table page allocated by `pgtab_alloc` or handed in by C
    /// via a `*pagetable` root that satisfies the same contract).
    unsafe fn from_raw(ptr: *mut u64) -> Self {
        // SAFETY: caller contract above; `NonNull::new_unchecked` just
        // requires `ptr` to be non-null, which every call site upholds.
        unsafe { Self(NonNull::new_unchecked(ptr)) }
    }
    /// SAFETY: `pa` must be the physical address of a live page-table page
    /// (same contract as `from_raw`, reached via a valid PTE's `pa()`).
    unsafe fn from_pa(pa: u64) -> Self {
        // SAFETY: forwarded to caller contract above.
        unsafe { Self::from_raw(pa as *mut u64) }
    }
    fn as_ptr(self) -> *mut u64 { self.0.as_ptr() }

    fn entry(self, idx: usize) -> Pte {
        debug_assert!(idx < 512);
        // SAFETY: `idx < 512` (checked above) and `self.0` points to a
        // 512-entry page-table page (`PageTable` invariant).
        Pte(unsafe { *self.0.as_ptr().add(idx) })
    }
    fn set_entry(self, idx: usize, v: u64) {
        debug_assert!(idx < 512);
        // SAFETY: same as `entry` above.
        unsafe { *self.0.as_ptr().add(idx) = v };
    }
    fn entry_ptr(self, idx: usize) -> *mut u64 {
        debug_assert!(idx < 512);
        // SAFETY: same as `entry` above.
        unsafe { self.0.as_ptr().add(idx) }
    }
    fn zero(self) {
        ffi::memset(self.0.as_ptr() as *mut c_void, 0, PGSIZE as usize);
    }
}
// ===========================================================================
// impl PageTable (continued) — KERNEL-OO wave: former page-table free fns
// (`walk`/`mappages`/`uvm*`/`kvm*` C-ABI entry points plus their private
// `PageTable`-taking internal helpers) namespaced as associated fns
// (8283168 strategy: bodies byte-identical, raw `*mut u64`/`PageTable`
// params kept as-is -- not rebound to `self` -- PTE writes stay raw, the
// hardware floor). A second `impl PageTable` block (Rust permits several
// per type in one module); kept separate from the pre-existing accessor
// block above to keep this wave's diff self-contained.
// ===========================================================================
impl PageTable {
    fn pgtab_alloc() -> Option<PageTable> {
        let pa = ffi::page_alloc(0, PAGE_TYPE_PGTABLE);
        if pa.is_null() {
            return None;
        }
        ffi::memset(pa, 0, PGSIZE as usize);
        // SAFETY: `pa` was just checked non-null and is a freshly allocated,
        // zeroed, PGSIZE page from `page_alloc`.
        Some(unsafe { PageTable::from_raw(pa as *mut u64) })
    }

    fn pgtab_free(pt: PageTable) {
        let pa = pt.as_ptr() as *mut c_void;
        let page = ffi::__pa_to_page(pa as u64) as *mut Page;
        if page.is_null() {
            panic_vm(b"__pgtab_free: invalid page table address\0");
        }
            ffi::page_lock_acquire(page);
            let is_pgtable = page_is_pgtable(page);
            ffi::page_lock_release(page);
            if !is_pgtable {
                panic_vm(b"__pgtab_free: trying to free a non-pagetable page\0");
            }
            ffi::page_free(pa, 0);
    }

    /// Walk `va` through the Sv39 page table. Returns the L0 PTE pointer plus
    /// the L2/L1 PTE pointers that led to it. With `alloc = false`, returns
    /// `None` if any level is unmapped.
    fn walk_internal(
        root: PageTable,
        va: u64,
        alloc: bool,
    ) -> Option<(*mut u64, *mut u64, *mut u64)> {
        if va >= crate::bindings::MAXVA {
            panic_vm(b"walk: va out of range\0");
        }

        let mut pt = root;
        let mut l2_ptr: *mut u64 = core::ptr::null_mut();
        let mut l1_ptr: *mut u64 = core::ptr::null_mut();

        for level in (1..=2).rev() {
            let idx = px(level as u32, va);
            let pte_ptr = pt.entry_ptr(idx);
            match level {
                2 => l2_ptr = pte_ptr,
                1 => l1_ptr = pte_ptr,
                _ => {}
            }
            let pte = Pte(machine::pte_read(pte_ptr));
            if pte.valid() {
                // SAFETY: `pte.pa()` is a physical address extracted from a
                // valid (`pte.valid()`) non-leaf PTE, i.e. it points at a
                // page-table page previously installed by this same walk.
                pt = unsafe { PageTable::from_pa(pte.pa()) };
            } else if !alloc {
                return None;
            } else {
                let new_pt = PageTable::pgtab_alloc()?;
                new_pt.zero();
                machine::pte_write(pte_ptr, pa2pte(new_pt.as_ptr() as u64) | PTE_V);
                pt = new_pt;
            }
        }

        Some((pt.entry_ptr(px(0, va)), l2_ptr, l1_ptr))
    }

    fn map_pages(root: PageTable, va: u64, size: u64, mut pa: u64, perm: i32) -> Result<(), crate::mm::cffi::Errno> {
        if va % PGSIZE != 0 {
            panic_mappages_va(va);
        }
        if size % PGSIZE != 0 {
            panic_mappages_size(va, size);
        }
        if size == 0 {
            panic_mappages_zero(va);
        }

        let last = va + size - PGSIZE;
        let mut a = va;
        loop {
            let Some((pte_ptr, _, _)) = PageTable::walk_internal(root, a, true) else {
                // ENOMEM partway through the range: `[va, a)` already has valid
                // leaf PTEs installed by the loop iterations above, and the
                // failed `PageTable::walk_internal(root, a, true)` call itself may have
                // allocated (and linked in) an intermediate L1/L0 page-table
                // page before failing one level deeper. Unwind both, instead of
                // leaving live-but-unowned mappings plus orphaned page-table
                // pages behind for the rest of this address space's lifetime.
                PageTable::unwind_partial_map(root, va, a);
                return Err(crate::mm::cffi::Errno::NoMem);
            };
            let pte = Pte(machine::pte_read(pte_ptr));
            if pte.valid() {
                panic_mappages_remap(a);
            }
            machine::pte_write(pte_ptr, pa2pte(pa) | (perm as u64) | PTE_V | PTE_A | PTE_D);
            if a == last { break; }
            a  += PGSIZE;
            pa += PGSIZE;
        }
        Ok(())
    }

    /// Undo a failed `PageTable::map_pages(root, va, ..)` call: `fail_va` is the address
    /// at which `walk_internal` first failed, so `[va, fail_va)` holds leaf
    /// PTEs this same call installed successfully.
    ///
    /// Two things need cleaning up:
    ///  1. The already-installed leaf PTEs in `[va, fail_va)`. These are
    ///     cleared (not "freed" -- `map_pages` never owns the physical pages
    ///     backing them; that's the caller's `pa` argument) so the range goes
    ///     back to fully unmapped, matching what a caller would see had the
    ///     call failed immediately.
    ///  2. Any intermediate (L1/L0) page-table pages that are now completely
    ///     empty as a result of step 1, including one that `walk_internal`
    ///     may have allocated for `fail_va` itself before failing one level
    ///     deeper (never linked to a leaf, but already linked into its own
    ///     parent table).
    ///
    /// Freeing is gated purely on "now empty", not on "did this call allocate
    /// it": an intermediate table can only be non-empty if something still
    /// references one of its entries, so a table that reads back all-zero
    /// after step 1 cannot be shared with any mapping outside this call -- pre-
    /// existing neighbouring mappings in a boundary table simply keep it
    /// non-empty and it is left untouched.
    ///
    /// No TLB/`sfence.vma` is needed: this whole range was never installed
    /// successfully (that's why we're unwinding), so no hart -- this one
    /// included -- could have translated through any of it yet.
    fn unwind_partial_map(root: PageTable, va: u64, fail_va: u64) {
        let mut a = va;
        while a < fail_va {
            // SAFETY-relevant invariant: every page in `[va, fail_va)` was
            // just mapped by the loop in `map_pages` above, so this walk is
            // guaranteed to resolve to the leaf PTE it wrote.
            if let Some((pte_ptr, _, _)) = PageTable::walk_internal(root, a, false) {
                machine::pte_write(pte_ptr, 0);
            }
            a += PGSIZE;
        }

        // Sweep every L0-table-sized (2MiB) region touched by this call --
        // `[va, fail_va]`, inclusive of `fail_va` so the partially-built branch
        // left behind by the failing walk (if any) is covered too.
        const L0_SPAN: u64 = 1u64 << 21; // pxshift(1) == PGSHIFT + 9 == 21.
        let mut region = va & !(L0_SPAN - 1);
        let last_region = fail_va & !(L0_SPAN - 1);
        loop {
            PageTable::prune_empty_region(root, region);
            if region >= last_region { break; }
            region += L0_SPAN;
        }
    }

    /// Free the L0 table covering `region` (a 2MiB-aligned VA) if every entry
    /// in it now reads zero, then re-check its parent L1 table on the same
    /// basis. No-op if nothing was ever walked down to `region`.
    fn prune_empty_region(root: PageTable, region: u64) {
        let l2_idx = px(2, region);
        let l2_pte = root.entry(l2_idx);
        if !l2_pte.valid() { return; }
        // SAFETY: `l2_pte.pa()` is the pa of a valid, non-leaf L2 entry (xv6
        // never installs Sv39 superpage leaves at this level), i.e. a live L1
        // page-table page.
        let l1_table = unsafe { PageTable::from_pa(l2_pte.pa()) };

        let l1_idx = px(1, region);
        let l1_pte = l1_table.entry(l1_idx);
        if l1_pte.valid() {
            // SAFETY: same reasoning one level down -- a valid, non-leaf L1
            // entry, i.e. a live L0 page-table page.
            let l0_table = unsafe { PageTable::from_pa(l1_pte.pa()) };
            if PageTable::table_is_empty(l0_table) {
                l1_table.set_entry(l1_idx, 0);
                PageTable::pgtab_free(l0_table);
            }
        }
        if PageTable::table_is_empty(l1_table) {
            root.set_entry(l2_idx, 0);
            PageTable::pgtab_free(l1_table);
        }
    }

    fn table_is_empty(pt: PageTable) -> bool {
        (0..512).all(|i| pt.entry(i).raw() == 0)
    }

    fn unmap_pages(root: PageTable, va: u64, npages: u64, do_free: bool) {
        if va % PGSIZE != 0 {
            panic_vm(b"uvmunmap: not aligned\0");
        }
        let end = va + npages * PGSIZE;
        let mut a = va;
        while a < end {
            let Some((pte_ptr, _, _)) = PageTable::walk_internal(root, a, false) else {
                panic_vm(b"uvmunmap: walk\0");
            };
            let pte = Pte(machine::pte_read(pte_ptr));
            if !pte.valid() {
                panic_uvmunmap_notmapped(a, pte.pa(), pte.flags());
            }
            if !pte.is_leaf() {
                panic_vm(b"uvmunmap: not a leaf\0");
            }
            let pa = pte.pa();
            machine::pte_write(pte_ptr, 0);
            if do_free {
                // SAFETY: `pa` is the physical address extracted from a valid
                // leaf PTE (checked via `pte.valid()`/`pte.is_leaf()` above),
                // i.e. a live mapped physical page; `pgtab_free` itself
                // re-validates the page type and panics if it isn't a
                // page-table page before freeing it.
                PageTable::pgtab_free(unsafe { PageTable::from_pa(pa) });
            }
            a += PGSIZE;
        }
    }

    fn freewalk_internal(pt: PageTable, skip_idx: Option<usize>) {
        for i in 0..512usize {
            if Some(i) == skip_idx { continue; }
            let pte = pt.entry(i);
            if pte.valid() && pte.0 & (PTE_R | PTE_W | PTE_RSW_W | PTE_X) == 0 {
                // SAFETY: `pte.pa()` comes from a valid PTE with no R/W/X/COW
                // bits set, i.e. a non-leaf entry pointing at a child
                // page-table page.
                let child = unsafe { PageTable::from_pa(pte.pa()) };
                PageTable::freewalk_internal(child, None);
                pt.set_entry(i, 0);
            } else if pte.valid() {
                panic_vm(b"freewalk: leaf\0");
            }
        }
        PageTable::pgtab_free(pt);
    }

    pub(crate) fn walk(
        pagetable: *mut u64,
        va: u64,
        alloc: c_int,
        retl2: *mut *mut u64,
        retl1: *mut *mut u64,
    ) -> *mut u64 {
        if pagetable.is_null() {
            panic_vm(b"walk: pagetable is null\0");
        }
        // SAFETY: `pagetable` was just checked non-null; callers pass a live
        // page-table root (kernel or a `PageTable::uvmcreate()`d user root).
        let root = unsafe { PageTable::from_raw(pagetable) };
        let Some((l0, l2, l1)) = PageTable::walk_internal(root, va, alloc != 0) else {
            return core::ptr::null_mut();
        };
        // SAFETY: `retl2`/`retl1` are out-parameters from C; null-checked
        // before each write, and when non-null they are valid `*mut u64`
        // slots owned by the caller for the duration of this call.
        unsafe {
            if !retl2.is_null() { *retl2 = l2; }
            if !retl1.is_null() { *retl1 = l1; }
        }
        l0
    }

    pub(crate) fn walkaddr(pagetable: *mut u64, va: u64) -> u64 {
        if va >= crate::bindings::MAXVA {
            return 0;
        }
        // SAFETY: `pagetable` is a live page-table root supplied by C (walk's
        // C callers never pass a dangling/null root for a live process).
        let root = unsafe { PageTable::from_raw(pagetable) };
        let Some((pte_ptr, _, _)) = PageTable::walk_internal(root, va, false) else {
            return 0;
        };
        // SAFETY: `pte_ptr` is the L0 PTE pointer `walk_internal` just
        // resolved from `root`; it is valid for a read.
        let pte = Pte(unsafe { *pte_ptr });
        if !pte.valid() || !pte.user() { return 0; }
        pte.pa()
    }

    pub(crate) fn kvmmap(
        kpgtbl: *mut u64,
        va: u64,
        pa: u64,
        sz: u64,
        perm: c_int,
    ) {
        // SAFETY: `kpgtbl` is always the kernel pagetable root, live for the
        // whole kernel lifetime once `kvminit` has allocated it.
        let root = unsafe { PageTable::from_raw(kpgtbl) };
        if PageTable::map_pages(root, va, sz, pa, perm).is_err() {
            panic_vm(b"kvmmap\0");
        }
    }

    pub(crate) fn mappages(
        pagetable: *mut u64,
        va: u64,
        size: u64,
        pa: u64,
        perm: c_int,
    ) -> c_int {
        // SAFETY: caller supplies a live page-table root.
        let root = unsafe { PageTable::from_raw(pagetable) };
        // C-ABI boundary: convert `Result<(), Errno>` to a negative-errno
        // `c_int` exactly once, here.
        crate::mm::cffi::result_to_neg_errno(PageTable::map_pages(root, va, size, pa, perm))
    }

    pub(crate) fn uvmunmap(
        pagetable: *mut u64,
        va: u64,
        npages: u64,
        do_free: c_int,
    ) {
        // SAFETY: caller supplies a live page-table root.
        let root = unsafe { PageTable::from_raw(pagetable) };
        PageTable::unmap_pages(root, va, npages, do_free != 0);
    }

    pub(crate) fn uvmcreate() -> *mut u64 {
        let Some(pt) = PageTable::pgtab_alloc() else { return core::ptr::null_mut() };
        pt.zero();

        // Copy trampoline PTE from kernel pagetable.
        // SAFETY: `kernel_pagetable_get()` returns the kernel root set once by
        // `PageTable::kvminit()` before any user pagetable can be created.
        let kpt = unsafe { PageTable::from_raw(kernel_pagetable_get()) };
        let idx = trampoline_pte_idx();
        pt.set_entry(idx, kpt.entry(idx).raw());
        pt.as_ptr()
    }

    pub(crate) fn freewalk(pagetable: *mut u64) {
        // SAFETY: caller supplies a live page-table root about to be torn
        // down (no more live mappings/refs into it).
        let root = unsafe { PageTable::from_raw(pagetable) };
        PageTable::freewalk_internal(root, Some(trampoline_pte_idx()));
    }

    pub(crate) fn uvmfree(pagetable: *mut u64, _sz: u64) {
        PageTable::freewalk(pagetable);
    }

    pub(crate) fn kvminit() {
        ffi::Vma::__vma_pool_init();
        ffi::Vm::__vm_pool_init();
        let kpt = PageTable::kvmmake();
        kernel_pagetable_set(kpt);
        set_trampoline_ksatp(make_satp(kpt));
    }

    pub(crate) fn kvminithart() {
        xv6_vm_sfence_vma();
        let satp = get_trampoline_ksatp();
        w_satp(satp);
        xv6_vm_sfence_vma();
        printf_kvminithart(ffi::xv6_vm_cpuid() as u64, satp);
    }

    pub(crate) fn dump_pagetable(
        pagetable: *mut u64,
        level: c_int,
        indent: c_int,
        va_base: u64,
        va_end: u64,
        omit_pa: bool,
    ) {
        if !(0..=2).contains(&level) {
            printf_invalid_level(level);
            return;
        }
        // SAFETY: caller supplies a live page-table root (or, in the
        // recursive call below, a `pa()` reached from a valid non-leaf PTE).
        let pt = unsafe { PageTable::from_raw(pagetable) };
        let kernbase = kernbase();
        let physstop = physstop();

        let idx_start = px(2, va_base) as i32;
        let mut idx_end = 512i32;
        if level == 2 && va_end != 0 {
            idx_end = px(2, va_end) as i32;
        }

        if level == 0 {
            let mut chunk_start: i32 = -1;
            let mut chunk_va_start: u64 = 0;
            let mut chunk_pa_start: u64 = 0;
            let mut chunk_flags: u32 = 0;
            let mut chunk_count: i32 = 0;

            let mut i = idx_start;
            while i <= idx_end {
                let raw = if i < idx_end { pt.entry(i as usize).raw() } else { 0 };
                let pte = Pte(raw);
                let va = va_base | ((i as u64) << 12);
                let pa = pte.pa();
                let flags = (pte.flags() as u32)
                    | (if pte.cow() { PTE_V as u32 } else { 0 });

                let valid_entry = (i < idx_end)
                    && (raw & (PTE_V | PTE_RSW_W)) != 0
                    && !(omit_pa && va >= kernbase && va < physstop);

                if valid_entry && chunk_start == -1 {
                    chunk_start    = i;
                    chunk_va_start = va;
                    chunk_pa_start = pa;
                    chunk_flags    = flags;
                    chunk_count    = 1;
                } else if valid_entry
                    && chunk_start != -1
                    && pa == chunk_pa_start + (chunk_count as u64 * PGSIZE)
                    && flags == chunk_flags
                {
                    chunk_count += 1;
                } else {
                    if chunk_start != -1 {
                        let cf = chunk_flags as u64;
                        let sv  = flag_str(cf & PTE_V     != 0, b"V\0");
                        let su  = flag_str(cf & PTE_U     != 0, b"U\0");
                        let sw  = flag_str(cf & PTE_W     != 0, b"W\0");
                        let sx  = flag_str(cf & PTE_X     != 0, b"X\0");
                        let sr  = flag_str(cf & PTE_R     != 0, b"R\0");
                        let src = flag_str(cf & PTE_RSW_W != 0, b"C\0");
                        let flags_no_v = (chunk_flags as u64) & !PTE_V;

                        if chunk_count == 1 {
                            printf_dump_pte_single(
                                indent, chunk_start,
                                pt.entry_ptr(i as usize) as *mut c_void,
                                flags_no_v, sv, su, sw, sx, sr, src,
                                chunk_va_start, chunk_pa_start,
                            );
                        } else {
                            printf_dump_pte_range(
                                indent, chunk_start, chunk_start + chunk_count - 1,
                                flags_no_v, sv, su, sw, sx, sr, src,
                                chunk_va_start,
                                chunk_va_start + (chunk_count as u64 - 1) * PGSIZE,
                                chunk_pa_start,
                                chunk_pa_start + (chunk_count as u64 - 1) * PGSIZE,
                                chunk_count,
                            );
                        }
                    }
                    if valid_entry {
                        chunk_start    = i;
                        chunk_va_start = va;
                        chunk_pa_start = pa;
                        chunk_flags    = flags;
                        chunk_count    = 1;
                    } else {
                        chunk_start = -1;
                    }
                }
                i += 1;
            }
        } else {
            for i in idx_start..idx_end {
                let pte = pt.entry(i as usize);
                if pte.raw() & (PTE_V | PTE_RSW_W) == 0 { continue; }
                let va = va_base | ((i as u64) << (12 + 9 * level));
                if omit_pa && va >= kernbase && va < physstop { continue; }

                let sv  = flag_str(pte.raw() & PTE_V     != 0, b"V\0");
                let su  = flag_str(pte.raw() & PTE_U     != 0, b"U\0");
                let sw  = flag_str(pte.raw() & PTE_W     != 0, b"W\0");
                let sx  = flag_str(pte.raw() & PTE_X     != 0, b"X\0");
                let sr  = flag_str(pte.raw() & PTE_R     != 0, b"R\0");
                let src = flag_str(pte.raw() & PTE_RSW_W != 0, b"C\0");
                printf_dump_pte_inner(
                    indent, i, pt.entry_ptr(i as usize) as *mut c_void,
                    pte.flags() as u32, sv, su, sw, sx, sr, src,
                    va, pte.pa() as *mut c_void,
                );
                if level > 0 && pte.flags() == PTE_V {
                    printf_colon_newline();
                    PageTable::dump_pagetable(pte.pa() as *mut u64, level - 1, indent + 2, va, 0, omit_pa);
                } else {
                    printf_newline();
                }
            }
        }
    }

    fn kvmmap_safe(kpgtbl: *mut u64, va: u64, pa: u64, sz: u64, perm: c_int) {
        let mut off: u64 = 0;
        while off < sz {
            let pte = PageTable::walk(kpgtbl, va + off, 0, core::ptr::null_mut(), core::ptr::null_mut());
            // SAFETY: `pte`, when non-null, is the L0 PTE pointer `walk` just
            // resolved from the live kernel pagetable `kpgtbl`.
            let already_mapped = !pte.is_null() && (unsafe { *pte } & PTE_V) != 0;
            if already_mapped {
                off += PGSIZE;
                continue;
            }
            PageTable::kvmmap(kpgtbl, va + off, pa + off, PGSIZE, perm);
            off += PGSIZE;
        }
    }

    fn kvmmake() -> *mut u64 {
        let kpgtbl = &raw const ffi::_data_ktlb as *mut u64;

        ffi::memset(kpgtbl as *mut c_void, 0, PGSIZE as usize);

        // UART.
        let uart_page = pgrounddown(ffi::__uart0_mmio_base());
        PageTable::kvmmap(kpgtbl, uart_page, uart_page, PGSIZE, (PTE_R | PTE_W) as c_int);

        // Goldfish RTC.
        if ffi::__goldfish_rtc_mmio_base() != 0 {
            PageTable::kvmmap(
                kpgtbl, ffi::__goldfish_rtc_mmio_base(), ffi::__goldfish_rtc_mmio_base(),
                PGSIZE, (PTE_R | PTE_W) as c_int,
            );
        }

        // VirtIO devices.
        if ffi::platform().has_virtio != 0 && ffi::platform().virtio_count > 0 {
            let n = core::cmp::min(ffi::platform().virtio_count as usize, crate::bindings::N_VIRTIO as usize);
            let limit = core::cmp::min(n, ffi::platform().virtio_base.len());
            for i in 0..limit {
                let base = ffi::platform().virtio_base[i];
                if base != 0 {
                    PageTable::kvmmap(kpgtbl, base, base, PGSIZE, (PTE_R | PTE_W) as c_int);
                }
            }
        }

        // PCIe regions.
        if ffi::platform().has_pcie != 0 && ffi::__pcie_ecam_mmio_base() != 0 {
            for i in 0..(ffi::platform().pcie_reg_count as usize) {
                let reg = &ffi::platform().pcie_reg[i];
                let base = reg.base;
                let size = reg.size;
                if base == 0 || size == 0 { continue; }
                let aligned_base = pgrounddown(base);
                let aligned_size = pgroundup(base + size) - aligned_base;
                PageTable::kvmmap_safe(
                    kpgtbl, aligned_base, aligned_base, aligned_size,
                    (PTE_R | PTE_W) as c_int,
                );
            }
            if ffi::platform().has_virtio != 0 && ffi::__e1000_pci_mmio_base() != 0 {
                PageTable::kvmmap(
                    kpgtbl, ffi::__e1000_pci_mmio_base(), ffi::__e1000_pci_mmio_base(),
                    0x20000, (PTE_R | PTE_W) as c_int,
                );
            }
        }

        // PLIC.
        if ffi::platform().plic_base != 0 && ffi::platform().plic_size != 0 {
            PageTable::kvmmap(
                kpgtbl, ffi::__plic_mmio_base(), ffi::__plic_mmio_base(),
                ffi::platform().plic_size, (PTE_R | PTE_W) as c_int,
            );
        }

        // EMAC.
        if ffi::platform().has_emac != 0 {
            let n = core::cmp::min(ffi::platform().emac_count as usize, crate::bindings::EMAC_MAX as usize);
            let limit = core::cmp::min(n, ffi::platform().emac.len());
            for i in 0..limit {
                let e = &ffi::platform().emac[i];
                if e.base != 0 && e.size != 0 {
                    let base = pgrounddown(e.base);
                    let size = pgroundup(e.base + e.size) - base;
                    PageTable::kvmmap_safe(kpgtbl, base, base, size, (PTE_R | PTE_W) as c_int);
                }
                if e.apmu_base != 0 && e.ctrl_reg != 0 {
                    let pg = pgrounddown(e.apmu_base as u64 + e.ctrl_reg as u64);
                    PageTable::kvmmap_safe(kpgtbl, pg, pg, PGSIZE, (PTE_R | PTE_W) as c_int);
                }
                if e.apmu_base != 0 && e.dline_reg != 0 {
                    let pg = pgrounddown(e.apmu_base as u64 + e.dline_reg as u64);
                    PageTable::kvmmap_safe(kpgtbl, pg, pg, PGSIZE, (PTE_R | PTE_W) as c_int);
                }
            }
        }

        // SDHCI.
        if ffi::platform().has_sdhci != 0 {
            let n = core::cmp::min(ffi::platform().sdhci_count as usize, crate::bindings::SDHCI_MAX as usize);
            let limit = core::cmp::min(n, ffi::platform().sdhci.len());
            for i in 0..limit {
                let s = &ffi::platform().sdhci[i];
                if s.base != 0 && s.size != 0 {
                    let base = pgrounddown(s.base);
                    let size = pgroundup(s.base + s.size) - base;
                    PageTable::kvmmap_safe(kpgtbl, base, base, size, (PTE_R | PTE_W) as c_int);
                }
                if s.apmu_base != 0 && s.apmu_offset != 0 {
                    let pg = pgrounddown(s.apmu_base as u64 + s.apmu_offset as u64);
                    PageTable::kvmmap_safe(kpgtbl, pg, pg, PGSIZE, (PTE_R | PTE_W) as c_int);
                }
            }
            if ffi::platform().sdhci_count > 0 && ffi::platform().sdhci[0].apbc_base != 0 {
                let apbc_aib = ffi::platform().sdhci[0].apbc_base as u64 + 0x3C;
                let pg = pgrounddown(apbc_aib);
                PageTable::kvmmap_safe(kpgtbl, pg, pg, PGSIZE, (PTE_R | PTE_W) as c_int);
            }
        }

        // Kernel text.
        let entry_addr = &raw const ffi::_entry as u64;
        let etext_addr = &raw const ffi::etext as u64;
        PageTable::kvmmap(
            kpgtbl, entry_addr, entry_addr,
            etext_addr - entry_addr,
            (PTE_R | PTE_X) as c_int,
        );

        // Trampoline / per-cpu mapping.
        let trampoline_addr = &raw const ffi::trampoline as u64;
        let tramp_data_addr = &raw const ffi::_trampoline_data as u64;
        let sig_tramp_addr  = &raw const ffi::sig_trampoline as u64;
        let cpus_addr       = ffi::cpus_base();

        PageTable::kvmmap(kpgtbl, TRAMPOLINE, trampoline_addr, PGSIZE, (PTE_R | PTE_X) as c_int);
        PageTable::kvmmap(kpgtbl, TRAMPOLINE_DATA, tramp_data_addr, PGSIZE, PTE_R as c_int);
        PageTable::kvmmap(kpgtbl, tramp_data_addr, tramp_data_addr, PGSIZE, (PTE_R | PTE_W) as c_int);
        PageTable::kvmmap(kpgtbl, TRAMPOLINE_CPULOCAL, cpus_addr, PGSIZE, (PTE_R | PTE_W) as c_int);
        PageTable::kvmmap(kpgtbl, cpus_addr, cpus_addr, PGSIZE, (PTE_R | PTE_W) as c_int);
        PageTable::kvmmap(
            kpgtbl, SIG_TRAMPOLINE, sig_tramp_addr, PGSIZE,
            (PTE_R | PTE_X | PTE_U) as c_int,
        );

        crate::kprintln!("trampoline 0x{:x} -> {}", TRAMPOLINE, crate::printf::Ptr(trampoline_addr));
        crate::kprintln!("trampoline data 0x{:x} -> {}", TRAMPOLINE_DATA, crate::printf::Ptr(tramp_data_addr));
        crate::kprintln!("trampoline cpu local 0x{:x} -> {}", TRAMPOLINE_CPULOCAL, crate::printf::Ptr(cpus_addr));
        crate::kprintln!("signal trampoline 0x{:x} -> {}", SIG_TRAMPOLINE, crate::printf::Ptr(sig_tramp_addr));

        // rodata / data / bss / remainder of physical memory.
        let rodata_addr = &raw const ffi::_rodata as u64;
        let rodata_end_addr = &raw const ffi::_rodata_end as u64;
        let data_ktlb_addr = &raw const ffi::_data_ktlb as u64;
        let data_addr = &raw const ffi::_data as u64;
        let data_end_addr = &raw const ffi::_data_end as u64;
        let bss_addr = &raw const ffi::_bss as u64;
        let bss_end_addr = &raw const ffi::_bss_end as u64;

        PageTable::kvmmap(kpgtbl, rodata_addr, rodata_addr, rodata_end_addr - rodata_addr, PTE_R as c_int);
        PageTable::kvmmap(kpgtbl, data_ktlb_addr, data_ktlb_addr, PGSIZE, (PTE_R | PTE_W) as c_int);
        PageTable::kvmmap(kpgtbl, data_addr, data_addr, data_end_addr - data_addr, (PTE_R | PTE_W) as c_int);
        PageTable::kvmmap(kpgtbl, bss_addr, bss_addr, bss_end_addr - bss_addr, (PTE_R | PTE_W) as c_int);
        PageTable::kvmmap(
            kpgtbl, bss_end_addr, bss_end_addr,
            ffi::__physical_memory_end() - bss_end_addr,
            (PTE_R | PTE_W) as c_int,
        );

        // Kernel symbols sections.
        let ksym_start = &raw const ffi::_ksymbols_start as u64;
        let ksym_end = &raw const ffi::_ksymbols_end as u64;
        let ksym_size = ksym_end - ksym_start;
        if ksym_size > 0 {
            PageTable::kvmmap(kpgtbl, ksym_start, ksym_start, ksym_size, PTE_R as c_int);
            crate::kprintln!("kernel symbols 0x{:x} - 0x{:x} (size: {})", ksym_start, ksym_end, ksym_size);
        }

        let ksym_idx_start = &raw const ffi::_ksymbols_idx_start as u64;
        let ksym_idx_end = &raw const ffi::_ksymbols_idx_end as u64;
        let ksym_idx_size = ksym_idx_end - ksym_idx_start;
        if ksym_idx_size > 0 {
            crate::kprintln!("kernel symbols index 0x{:x} - 0x{:x} (size: {})", ksym_idx_start, ksym_idx_end, ksym_idx_size);
        }

        kpgtbl
    }
}


// ---------------------------------------------------------------------------
// Pagetable-page (de)allocation.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Walk implementation.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Internal mapping helpers (safe variants).
// ---------------------------------------------------------------------------







// ---------------------------------------------------------------------------
// Public C ABI — thin wrappers.
// ---------------------------------------------------------------------------



// P3-D3c: `#[no_mangle] extern "C"` dropped -- callers (`irq/trap.rs` and
// this file) reach it by crate path now.






// P3-D3c: `#[no_mangle] extern "C"` dropped -- the only external caller
// (`start_kernel.rs`) imports it by crate path.

// P3-D3c: same demotion as `kvminit` above.

pub(crate) fn vm_dump_flags(
    flags: u64,
    buf: *mut c_char,
    buf_size: usize,
) -> c_int {
    if buf.is_null() { return -EINVAL; }
    if buf_size < 5 { return -ERANGE; }

    // SAFETY: `buf` was checked non-null and `buf_size >= 5`, so a 5-byte
    // `u8` slice over it is in-bounds; C callers own `buf` for the call.
    let slice = unsafe { core::slice::from_raw_parts_mut(buf as *mut u8, 5) };
    slice[0] = if flags & PROT_READ     != 0 { b'R' } else { b' ' };
    slice[1] = if flags & PROT_WRITE    != 0 { b'W' } else { b' ' };
    slice[2] = if flags & PROT_EXEC     != 0 { b'X' } else { b' ' };
    slice[3] = if flags & VMA_FLAG_USER != 0 { b'U' } else { b' ' };
    slice[4] = 0;
    4
}

// ---------------------------------------------------------------------------
// dump_pagetable
// ---------------------------------------------------------------------------
fn flag_str(flag_set: bool, letter: &'static [u8]) -> *const c_char {
    if flag_set { letter.as_ptr() as *const c_char } else { b" \0".as_ptr() as *const c_char }
}


// ---------------------------------------------------------------------------
// kvmmap_safe + kvmmake -- previously in the deleted `vm_pgtab_shims.rs`.
// Builds the kernel page table from the platform's MMIO layout (populated
// by `fdt_init` before `kvminit` runs) and the kernel image's linker
// symbols. `kvminit` (above) is the only caller -- `kvmmake` is not part
// of the C-ABI surface (unlike `kvminit`/`kvminithart`), so it is a plain
// private fn.
// ---------------------------------------------------------------------------
const TRAMPOLINE: u64 = crate::bindings::MAXVA - PGSIZE;
const TRAMPOLINE_DATA: u64 = TRAMPOLINE - PGSIZE;
const TRAMPOLINE_CPULOCAL: u64 = TRAMPOLINE - PGSIZE * 2;
const SIG_TRAMPOLINE: u64 = TRAMPOLINE - PGSIZE * 3;


