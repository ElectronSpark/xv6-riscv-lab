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
    let maxva = ffi::xv6_vm_maxva();
    let trampoline = maxva - PGSIZE;
    ((trampoline >> 30) & 0x1FF) as usize
}

// ---------------------------------------------------------------------------
// FFI boundary.
// ---------------------------------------------------------------------------
#[repr(C)] pub struct Page { _opaque: [u8; 0] }

mod ffi {
    use super::*;
    unsafe extern "C" {
        // memory / pages
        pub safe fn memset(s: *mut c_void, c: c_int, n: usize) -> *mut c_void;
        pub safe fn page_alloc(order: c_int, ptype: c_int) -> *mut c_void;
        pub safe fn page_free(pa: *mut c_void, order: c_int);
        pub safe fn __pa_to_page(pa: u64) -> *mut c_void;

        // shims
        pub safe fn xv6_vm_kernel_pagetable_get() -> *mut u64;
        pub safe fn xv6_vm_kernel_pagetable_set(pt: *mut u64);
        pub safe fn xv6_vm_make_satp(pt: *mut u64) -> u64;
        pub safe fn xv6_vm_set_trampoline_ksatp(v: u64);
        pub safe fn xv6_vm_get_trampoline_ksatp() -> u64;
        pub safe fn xv6_vm_w_satp(v: u64);
        pub safe fn xv6_vm_sfence_vma();
        pub safe fn xv6_vm_cpuid() -> c_int;
        pub safe fn xv6_vm_vma_pool_init();
        pub safe fn xv6_vm_vm_pool_init();

        pub safe fn xv6_vm_panic(msg: *const c_char) -> !;
        pub safe fn xv6_vm_panic_mappages_va(va: u64) -> !;
        pub safe fn xv6_vm_panic_mappages_size(va: u64, size: u64) -> !;
        pub safe fn xv6_vm_panic_mappages_zero(va: u64) -> !;
        pub safe fn xv6_vm_panic_mappages_remap(a: u64) -> !;
        pub safe fn xv6_vm_panic_uvmunmap_notmapped(va: u64, pa: u64, flags: u64) -> !;

        pub safe fn xv6_vm_printf_kvminithart(hart: u64, satp: u64);
        pub safe fn xv6_vm_printf_invalid_level(level: c_int);
        pub safe fn xv6_vm_printf_dump_pte_single(indent: c_int, idx: c_int, pte_ptr: *mut c_void,
            flags_no_v: u64, sv: *const c_char, su: *const c_char,
            sw: *const c_char, sx: *const c_char, sr: *const c_char,
            src: *const c_char, va: u64, pa: u64);
        pub safe fn xv6_vm_printf_dump_pte_range(indent: c_int, start_idx: c_int, end_idx: c_int,
            flags_no_v: u64, sv: *const c_char, su: *const c_char,
            sw: *const c_char, sx: *const c_char, sr: *const c_char,
            src: *const c_char, va_start: u64, va_end: u64,
            pa_start: u64, pa_end: u64, count: c_int);
        pub safe fn xv6_vm_printf_dump_pte_inner(indent: c_int, idx: c_int, pte_ptr: *mut c_void,
            flags: u32, sv: *const c_char, su: *const c_char,
            sw: *const c_char, sx: *const c_char, sr: *const c_char,
            src: *const c_char, va: u64, pa: *mut c_void);
        pub safe fn xv6_vm_printf_newline();
        pub safe fn xv6_vm_printf_colon_newline();

        pub safe fn xv6_vm_kernbase() -> u64;
        pub safe fn xv6_vm_physstop() -> u64;
        pub safe fn xv6_vm_maxva() -> u64;

        pub safe fn xv6_vm_einval() -> c_int;
        pub safe fn xv6_vm_erange() -> c_int;
        pub safe fn xv6_vm_enomem() -> c_int;

        pub safe fn xv6_vm_page_type_pgtable() -> c_int;
        pub safe fn xv6_vm_page_is_pgtable(p: *mut Page) -> c_int;
        pub safe fn xv6_vm_page_lock_acquire(p: *mut Page);
        pub safe fn xv6_vm_page_lock_release(p: *mut Page);

        pub safe fn kvmmake() -> *mut u64;
    }
}

fn panic_vm(msg: &[u8]) -> ! {
    debug_assert!(msg.last() == Some(&0));
    ffi::xv6_vm_panic(msg.as_ptr() as *const c_char)
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
struct PageTable(NonNull<u64>);

impl PageTable {
    /// SAFETY: `ptr` must point to a PGSIZE-aligned page of 512 u64 entries.
    unsafe fn from_raw(ptr: *mut u64) -> Self {
        Self(NonNull::new_unchecked(ptr))
    }
    fn from_pa(pa: u64) -> Self {
        unsafe { Self::from_raw(pa as *mut u64) }
    }
    fn as_ptr(self) -> *mut u64 { self.0.as_ptr() }

    fn entry(self, idx: usize) -> Pte {
        debug_assert!(idx < 512);
        Pte(unsafe { *self.0.as_ptr().add(idx) })
    }
    fn set_entry(self, idx: usize, v: u64) {
        debug_assert!(idx < 512);
        unsafe { *self.0.as_ptr().add(idx) = v };
    }
    fn entry_ptr(self, idx: usize) -> *mut u64 {
        debug_assert!(idx < 512);
        unsafe { self.0.as_ptr().add(idx) }
    }
    fn zero(self) {
        ffi::memset(self.0.as_ptr() as *mut c_void, 0, PGSIZE as usize);
    }
}

// ---------------------------------------------------------------------------
// Pagetable-page (de)allocation.
// ---------------------------------------------------------------------------
fn pgtab_alloc() -> Option<PageTable> {
    let pa = ffi::page_alloc(0, ffi::xv6_vm_page_type_pgtable());
    if pa.is_null() {
        return None;
    }
    ffi::memset(pa, 0, PGSIZE as usize);
    Some(unsafe { PageTable::from_raw(pa as *mut u64) })
}

fn pgtab_free(pt: PageTable) {
    let pa = pt.as_ptr() as *mut c_void;
    let page = ffi::__pa_to_page(pa as u64) as *mut Page;
    if page.is_null() {
        panic_vm(b"__pgtab_free: invalid page table address\0");
    }
        ffi::xv6_vm_page_lock_acquire(page);
        let is_pgtable = ffi::xv6_vm_page_is_pgtable(page) != 0;
        ffi::xv6_vm_page_lock_release(page);
        if !is_pgtable {
            panic_vm(b"__pgtab_free: trying to free a non-pagetable page\0");
        }
        ffi::page_free(pa, 0);
}

// ---------------------------------------------------------------------------
// Walk implementation.
// ---------------------------------------------------------------------------

/// Walk `va` through the Sv39 page table. Returns the L0 PTE pointer plus
/// the L2/L1 PTE pointers that led to it. With `alloc = false`, returns
/// `None` if any level is unmapped.
fn walk_internal(
    root: PageTable,
    va: u64,
    alloc: bool,
) -> Option<(*mut u64, *mut u64, *mut u64)> {
    if va >= ffi::xv6_vm_maxva() {
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
            pt = PageTable::from_pa(pte.pa());
        } else if !alloc {
            return None;
        } else {
            let new_pt = pgtab_alloc()?;
            new_pt.zero();
            machine::pte_write(pte_ptr, pa2pte(new_pt.as_ptr() as u64) | PTE_V);
            pt = new_pt;
        }
    }

    Some((pt.entry_ptr(px(0, va)), l2_ptr, l1_ptr))
}

// ---------------------------------------------------------------------------
// Internal mapping helpers (safe variants).
// ---------------------------------------------------------------------------

fn map_pages(root: PageTable, va: u64, size: u64, mut pa: u64, perm: i32) -> i32 {
    if va % PGSIZE != 0 {
        ffi::xv6_vm_panic_mappages_va(va);
    }
    if size % PGSIZE != 0 {
        ffi::xv6_vm_panic_mappages_size(va, size);
    }
    if size == 0 {
        ffi::xv6_vm_panic_mappages_zero(va);
    }

    let last = va + size - PGSIZE;
    let mut a = va;
    loop {
        let Some((pte_ptr, _, _)) = walk_internal(root, a, true) else {
            return -ffi::xv6_vm_enomem();
        };
        let pte = Pte(machine::pte_read(pte_ptr));
        if pte.valid() {
            ffi::xv6_vm_panic_mappages_remap(a);
        }
        machine::pte_write(pte_ptr, pa2pte(pa) | (perm as u64) | PTE_V | PTE_A | PTE_D);
        if a == last { break; }
        a  += PGSIZE;
        pa += PGSIZE;
    }
    0
}

fn unmap_pages(root: PageTable, va: u64, npages: u64, do_free: bool) {
    if va % PGSIZE != 0 {
        panic_vm(b"uvmunmap: not aligned\0");
    }
    let end = va + npages * PGSIZE;
    let mut a = va;
    while a < end {
        let Some((pte_ptr, _, _)) = walk_internal(root, a, false) else {
            panic_vm(b"uvmunmap: walk\0");
        };
        let pte = Pte(machine::pte_read(pte_ptr));
        if !pte.valid() {
            ffi::xv6_vm_panic_uvmunmap_notmapped(a, pte.pa(), pte.flags());
        }
        if !pte.is_leaf() {
            panic_vm(b"uvmunmap: not a leaf\0");
        }
        let pa = pte.pa();
        machine::pte_write(pte_ptr, 0);
        if do_free {
            pgtab_free(PageTable::from_pa(pa));
        }
        a += PGSIZE;
    }
}

fn freewalk_internal(pt: PageTable, skip_idx: Option<usize>) {
    for i in 0..512usize {
        if Some(i) == skip_idx { continue; }
        let pte = pt.entry(i);
        if pte.valid() && pte.0 & (PTE_R | PTE_W | PTE_RSW_W | PTE_X) == 0 {
            let child = PageTable::from_pa(pte.pa());
            freewalk_internal(child, None);
            pt.set_entry(i, 0);
        } else if pte.valid() {
            panic_vm(b"freewalk: leaf\0");
        }
    }
    pgtab_free(pt);
}

// ---------------------------------------------------------------------------
// Public C ABI — thin wrappers.
// ---------------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn walk(
    pagetable: *mut u64,
    va: u64,
    alloc: c_int,
    retl2: *mut *mut u64,
    retl1: *mut *mut u64,
) -> *mut u64 {
    if pagetable.is_null() {
        panic_vm(b"walk: pagetable is null\0");
    }
    let Some((l0, l2, l1)) = walk_internal(PageTable::from_raw(pagetable), va, alloc != 0) else {
        return core::ptr::null_mut();
    };
    if !retl2.is_null() { *retl2 = l2; }
    if !retl1.is_null() { *retl1 = l1; }
    l0
}

#[no_mangle]
pub unsafe extern "C" fn walkaddr(pagetable: *mut u64, va: u64) -> u64 {
    if va >= ffi::xv6_vm_maxva() {
        return 0;
    }
    let Some((pte_ptr, _, _)) = walk_internal(PageTable::from_raw(pagetable), va, false) else {
        return 0;
    };
    let pte = Pte(*pte_ptr);
    if !pte.valid() || !pte.user() { return 0; }
    pte.pa()
}

#[no_mangle]
pub unsafe extern "C" fn kvmmap(
    kpgtbl: *mut u64,
    va: u64,
    pa: u64,
    sz: u64,
    perm: c_int,
) {
    if map_pages(PageTable::from_raw(kpgtbl), va, sz, pa, perm) != 0 {
        panic_vm(b"kvmmap\0");
    }
}

#[no_mangle]
pub unsafe extern "C" fn mappages(
    pagetable: *mut u64,
    va: u64,
    size: u64,
    pa: u64,
    perm: c_int,
) -> c_int {
    map_pages(PageTable::from_raw(pagetable), va, size, pa, perm)
}

#[no_mangle]
pub unsafe extern "C" fn uvmunmap(
    pagetable: *mut u64,
    va: u64,
    npages: u64,
    do_free: c_int,
) {
    unmap_pages(PageTable::from_raw(pagetable), va, npages, do_free != 0);
}

#[no_mangle]
pub unsafe extern "C" fn uvmcreate() -> *mut u64 {
    let Some(pt) = pgtab_alloc() else { return core::ptr::null_mut() };
    pt.zero();

    // Copy trampoline PTE from kernel pagetable.
    let kpt = PageTable::from_raw(ffi::xv6_vm_kernel_pagetable_get());
    let idx = trampoline_pte_idx();
    pt.set_entry(idx, kpt.entry(idx).raw());
    pt.as_ptr()
}

#[no_mangle]
pub unsafe extern "C" fn freewalk(pagetable: *mut u64) {
    freewalk_internal(PageTable::from_raw(pagetable), Some(trampoline_pte_idx()));
}

#[no_mangle]
pub unsafe extern "C" fn uvmfree(pagetable: *mut u64, _sz: u64) {
    freewalk(pagetable);
}

#[no_mangle]
pub unsafe extern "C" fn kvminit() {
    ffi::xv6_vm_vma_pool_init();
    ffi::xv6_vm_vm_pool_init();
    let kpt = ffi::kvmmake();
    ffi::xv6_vm_kernel_pagetable_set(kpt);
    ffi::xv6_vm_set_trampoline_ksatp(ffi::xv6_vm_make_satp(kpt));
}

#[no_mangle]
pub unsafe extern "C" fn kvminithart() {
    ffi::xv6_vm_sfence_vma();
    let satp = ffi::xv6_vm_get_trampoline_ksatp();
    ffi::xv6_vm_w_satp(satp);
    ffi::xv6_vm_sfence_vma();
    ffi::xv6_vm_printf_kvminithart(ffi::xv6_vm_cpuid() as u64, satp);
}

#[no_mangle]
pub unsafe extern "C" fn vm_dump_flags(
    flags: u64,
    buf: *mut c_char,
    buf_size: usize,
) -> c_int {
    if buf.is_null() { return -ffi::xv6_vm_einval(); }
    if buf_size < 5 { return -ffi::xv6_vm_erange(); }

    let slice = core::slice::from_raw_parts_mut(buf as *mut u8, 5);
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

#[no_mangle]
pub unsafe extern "C" fn dump_pagetable(
    pagetable: *mut u64,
    level: c_int,
    indent: c_int,
    va_base: u64,
    va_end: u64,
    omit_pa: bool,
) {
    if !(0..=2).contains(&level) {
        ffi::xv6_vm_printf_invalid_level(level);
        return;
    }
    let pt = PageTable::from_raw(pagetable);
    let kernbase = ffi::xv6_vm_kernbase();
    let physstop = ffi::xv6_vm_physstop();

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
                        ffi::xv6_vm_printf_dump_pte_single(
                            indent, chunk_start,
                            pt.entry_ptr(i as usize) as *mut c_void,
                            flags_no_v, sv, su, sw, sx, sr, src,
                            chunk_va_start, chunk_pa_start,
                        );
                    } else {
                        ffi::xv6_vm_printf_dump_pte_range(
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
            ffi::xv6_vm_printf_dump_pte_inner(
                indent, i, pt.entry_ptr(i as usize) as *mut c_void,
                pte.flags() as u32, sv, su, sw, sx, sr, src,
                va, pte.pa() as *mut c_void,
            );
            if level > 0 && pte.flags() == PTE_V {
                ffi::xv6_vm_printf_colon_newline();
                dump_pagetable(pte.pa() as *mut u64, level - 1, indent + 2, va, 0, omit_pa);
            } else {
                ffi::xv6_vm_printf_newline();
            }
        }
    }
}
