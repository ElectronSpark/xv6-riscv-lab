//! Rust replacement for `kernel/mm/vm_pgtab_shims.c`.
//!
//! Provides all `xv6_vm_*` helpers consumed by `vm_pgtab.rs` and the
//! Rust port of `kvmmake()`. Storage of `kernel_pagetable` lives here.
//!
//! Phase 1B 5/8: eliminates the last C shim file for the pagetable code.

#![allow(non_upper_case_globals)]
#![allow(non_snake_case)]

use core::ffi::{c_char, c_int, c_void};
use core::sync::atomic::{compiler_fence, fence, Ordering};

use crate::bindings::{
    page_struct, platform_info, EINVAL, ENOMEM, ERANGE,
    MAXVA, N_VIRTIO, EMAC_MAX, SDHCI_MAX,
};

// =====================================================================
// Constants
// =====================================================================
const PGSIZE: u64 = 4096;
const PTE_V: u64 = 1 << 0;
const PTE_R: u64 = 1 << 1;
const PTE_W: u64 = 1 << 2;
const PTE_X: u64 = 1 << 3;
const PTE_U: u64 = 1 << 4;
const TRAMPOLINE: u64 = MAXVA - PGSIZE;
const TRAMPOLINE_DATA: u64 = TRAMPOLINE - PGSIZE;
const TRAMPOLINE_CPULOCAL: u64 = TRAMPOLINE - PGSIZE * 2;
const SIG_TRAMPOLINE: u64 = TRAMPOLINE - PGSIZE * 3;

// page_type::PAGE_TYPE_PGTABLE = 3 (ANON=0, BUDDY=1, SLAB=2, PGTABLE=3)
const PAGE_TYPE_PGTABLE: c_int = 3;
const PAGE_FLAG_TYPE_MASK: u64 = (1u64 << 8) - 1;

#[inline] fn pgrounddown(a: u64) -> u64 { a & !(PGSIZE - 1) }
#[inline] fn pgroundup(a: u64) -> u64 { (a + PGSIZE - 1) & !(PGSIZE - 1) }

// =====================================================================
// Externs from C kernel
// =====================================================================
mod ffi {
    use super::*;
    use core::ffi::{c_char, c_int, c_void};
    unsafe extern "C" {
        pub safe fn printf(fmt: *const c_char, ...);
        pub safe fn memset(s: *mut c_void, c: c_int, n: usize) -> *mut c_void;
        pub safe fn __panic_start();
        pub safe fn __panic_end() -> !;
        pub safe fn cpuid() -> c_int;
        pub safe fn page_lock_acquire(p: *mut c_void);
        pub safe fn page_lock_release(p: *mut c_void);

        // Pool init implementations live in vm.rs (no_mangle).
        pub safe fn __vma_pool_init();
        pub safe fn __vm_pool_init();

        // Rust-owned pagetable primitives (no_mangle in vm_pgtab.rs).
        pub safe fn kvmmap(kpgtbl: *mut u64, va: u64, pa: u64, sz: u64, perm: c_int);
        pub safe fn walk(
            pt: *mut u64, va: u64, alloc: c_int,
            retl2: *mut *mut u64, retl1: *mut *mut u64,
        ) -> *mut u64;

        // MMIO base extern variables (resolved by FDT at boot).
        pub safe static __physical_memory_start: u64;
        pub safe static __physical_memory_end: u64;
        pub safe static __uart0_mmio_base: u64;
        pub safe static __goldfish_rtc_mmio_base: u64;
        pub safe static __plic_mmio_base: u64;
        pub safe static __pcie_ecam_mmio_base: u64;
        pub safe static __e1000_pci_mmio_base: u64;

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
        pub safe static cpus: u8;

        // Kernel symbols sections.
        pub safe static _ksymbols_start: u8;
        pub safe static _ksymbols_end: u8;
        pub safe static _ksymbols_idx_start: u8;
        pub safe static _ksymbols_idx_end: u8;

        // Global platform info (populated by fdt_init).
        pub safe static platform: platform_info;

        // Defined in trampoline.S.
        pub safe static mut trampoline_ksatp: u64;
    }
}
use ffi::*;

// Page struct flags-field accessor uses the bindgen-generated layout.
// page_struct.__bindgen_anon_1.flags is the type+flags word.
extern "C" {}

// =====================================================================
// kernel_pagetable storage
// =====================================================================
#[no_mangle]
pub static mut kernel_pagetable: *mut u64 = core::ptr::null_mut();

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_kernel_pagetable_get() -> *mut u64 {
    kernel_pagetable
}

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_kernel_pagetable_set(pt: *mut u64) {
    kernel_pagetable = pt;
}

// =====================================================================
// SATP / trampoline_ksatp
// =====================================================================
#[inline]
fn make_satp(pt: *mut u64) -> u64 {
    // SV39 = mode 8.
    (8u64 << 60) | ((pt as u64) >> 12)
}

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_make_satp(pt: *mut u64) -> u64 {
    make_satp(pt)
}

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_set_trampoline_ksatp(v: u64) {
    // smp_store_release + smp_mb equivalent.
    core::ptr::write_volatile(&raw mut trampoline_ksatp, v);
    compiler_fence(Ordering::Release);
    fence(Ordering::SeqCst);
}

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_get_trampoline_ksatp() -> u64 {
    core::ptr::read_volatile(&raw const trampoline_ksatp)
}

// =====================================================================
// hart-control: w_satp / sfence_vma
// =====================================================================
#[no_mangle]
pub unsafe extern "C" fn xv6_vm_w_satp(v: u64) {
    core::arch::asm!("csrw satp, {0}", in(reg) v, options(nostack, preserves_flags));
}

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_sfence_vma() {
    core::arch::asm!("sfence.vma zero, zero", options(nostack, preserves_flags));
}

// xv6_vm_cpuid is defined in vm.rs (kept there to avoid duplication).

// =====================================================================
// Pool init shims (delegate to Rust impls in vm.rs)
// =====================================================================
#[no_mangle]
pub unsafe extern "C" fn xv6_vm_vma_pool_init() { __vma_pool_init(); }

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_vm_pool_init() { __vm_pool_init(); }

// =====================================================================
// Panic helpers
// =====================================================================
unsafe fn panic_msg(fmt: *const c_char) -> ! {
    __panic_start();
    printf(b"PANIC: %s\n\0".as_ptr() as *const c_char, fmt);
    __panic_end();
}
#[no_mangle]
pub unsafe extern "C" fn xv6_vm_panic(msg: *const c_char) -> ! {
    panic_msg(msg);
}

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_panic_mappages_va(va: u64) -> ! {
    __panic_start();
    printf(
        b"mappages: va not aligned, va %p\n\0".as_ptr() as *const c_char,
        va as *const c_void,
    );
    __panic_end();
}

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_panic_mappages_size(va: u64, size: u64) -> ! {
    __panic_start();
    printf(
        b"mappages: size not aligned, va %p, size %p\n\0".as_ptr() as *const c_char,
        va as *const c_void,
        size as *const c_void,
    );
    __panic_end();
}

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_panic_mappages_zero(va: u64) -> ! {
    __panic_start();
    printf(
        b"mappages: size zero, va %p\n\0".as_ptr() as *const c_char,
        va as *const c_void,
    );
    __panic_end();
}

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_panic_mappages_remap(a: u64) -> ! {
    __panic_start();
    printf(
        b"mappages: remap, %p\n\0".as_ptr() as *const c_char,
        a as *const c_void,
    );
    __panic_end();
}

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_panic_uvmunmap_notmapped(
    va: u64, pa: u64, flags: u64,
) -> ! {
    __panic_start();
    printf(
        b"uvmunmap: not mapped, va=%p, pa=%p, flags: %lx\n\0".as_ptr() as *const c_char,
        va as *const c_void,
        pa as *const c_void,
        flags,
    );
    __panic_end();
}

// =====================================================================
// printf helpers used by the Rust port
// =====================================================================
#[no_mangle]
pub unsafe extern "C" fn xv6_vm_printf_kvminithart(hart: u64, satp: u64) {
    printf(
        b"hart %ld switched to kernel page table, satp: %lx\n\0".as_ptr() as *const c_char,
        hart, satp,
    );
}

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_printf_invalid_level(level: c_int) {
    printf(
        b"Invalid level %d for pagetable dump\n\0".as_ptr() as *const c_char,
        level,
    );
}

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_printf_dump_pte_single(
    indent: c_int, idx: c_int, pte_ptr: *mut c_void,
    flags_no_v: u64,
    sv: *const c_char, su: *const c_char, sw: *const c_char,
    sx: *const c_char, sr: *const c_char, src: *const c_char,
    va: u64, pa: u64,
) {
    printf(
        b"%*sPTE[%d](%p): %lx(%s%s%s%s%s%s), (va, pa): (%p, %p)\n\0".as_ptr() as *const c_char,
        indent, b"\0".as_ptr() as *const c_char,
        idx, pte_ptr,
        flags_no_v, sv, su, sw, sx, sr, src,
        va as *const c_void, pa as *const c_void,
    );
}

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_printf_dump_pte_range(
    indent: c_int, start_idx: c_int, end_idx: c_int,
    flags_no_v: u64,
    sv: *const c_char, su: *const c_char, sw: *const c_char,
    sx: *const c_char, sr: *const c_char, src: *const c_char,
    va_start: u64, va_end: u64, pa_start: u64, pa_end: u64,
    count: c_int,
) {
    printf(
        b"%*sPTE[%d-%d]: %lx(%s%s%s%s%s%s), (va, pa): (%p-%p, %p-%p) [%d pages]\n\0"
            .as_ptr() as *const c_char,
        indent, b"\0".as_ptr() as *const c_char,
        start_idx, end_idx,
        flags_no_v, sv, su, sw, sx, sr, src,
        va_start as *const c_void, va_end as *const c_void,
        pa_start as *const c_void, pa_end as *const c_void,
        count,
    );
}

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_printf_dump_pte_inner(
    indent: c_int, idx: c_int, pte_ptr: *mut c_void,
    flags: u32,
    sv: *const c_char, su: *const c_char, sw: *const c_char,
    sx: *const c_char, sr: *const c_char, src: *const c_char,
    va: u64, pa: *mut c_void,
) {
    printf(
        b"%*sPTE[%d](%p): %x(%s%s%s%s%s%s), (va, pa): (%p, %p)\0".as_ptr() as *const c_char,
        indent, b"\0".as_ptr() as *const c_char,
        idx, pte_ptr,
        flags, sv, su, sw, sx, sr, src,
        va as *const c_void, pa,
    );
}

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_printf_newline() {
    printf(b"\n\0".as_ptr() as *const c_char);
}

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_printf_colon_newline() {
    printf(b":\n\0".as_ptr() as *const c_char);
}

// =====================================================================
// Constants
// =====================================================================
#[no_mangle]
pub unsafe extern "C" fn xv6_vm_kernbase() -> u64 { __physical_memory_start }

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_physstop() -> u64 { __physical_memory_end }

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_maxva() -> u64 { MAXVA }

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_einval() -> c_int { EINVAL as c_int }

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_erange() -> c_int { ERANGE as c_int }

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_enomem() -> c_int { ENOMEM as c_int }

// =====================================================================
// Page helpers
// =====================================================================
#[no_mangle]
pub unsafe extern "C" fn xv6_vm_page_type_pgtable() -> c_int { PAGE_TYPE_PGTABLE }

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_page_is_pgtable(p: *mut c_void) -> c_int {
    if p.is_null() { return 0; }
    let page = p as *const page_struct;
    let flags = (*page).__bindgen_anon_1.flags;
    if (flags & PAGE_FLAG_TYPE_MASK) as c_int == PAGE_TYPE_PGTABLE { 1 } else { 0 }
}

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_page_lock_acquire(p: *mut c_void) {
    page_lock_acquire(p);
}

#[no_mangle]
pub unsafe extern "C" fn xv6_vm_page_lock_release(p: *mut c_void) {
    page_lock_release(p);
}

// =====================================================================
// kvmmap_safe + kvmmake (Rust port of the C originals)
// =====================================================================
fn kvmmap_safe(kpgtbl: *mut u64, va: u64, pa: u64, sz: u64, perm: c_int) { unsafe {
    let mut off: u64 = 0;
    while off < sz {
        let pte = walk(kpgtbl, va + off, 0, core::ptr::null_mut(), core::ptr::null_mut());
        if !pte.is_null() && (*pte & PTE_V) != 0 {
            off += PGSIZE;
            continue;
        }
        kvmmap(kpgtbl, va + off, pa + off, PGSIZE, perm);
        off += PGSIZE;
    }
} }

#[no_mangle]
pub unsafe extern "C" fn kvmmake() -> *mut u64 {
    let kpgtbl = &raw const _data_ktlb as *mut u64;

    memset(kpgtbl as *mut c_void, 0, PGSIZE as usize);

    // UART.
    let uart_page = pgrounddown(__uart0_mmio_base);
    kvmmap(kpgtbl, uart_page, uart_page, PGSIZE, (PTE_R | PTE_W) as c_int);

    // Goldfish RTC.
    if __goldfish_rtc_mmio_base != 0 {
        kvmmap(
            kpgtbl, __goldfish_rtc_mmio_base, __goldfish_rtc_mmio_base,
            PGSIZE, (PTE_R | PTE_W) as c_int,
        );
    }

    // VirtIO devices.
    if platform.has_virtio != 0 && platform.virtio_count > 0 {
        let n = core::cmp::min(platform.virtio_count as usize, N_VIRTIO as usize);
        let limit = core::cmp::min(n, platform.virtio_base.len());
        for i in 0..limit {
            let base = platform.virtio_base[i];
            if base != 0 {
                kvmmap(kpgtbl, base, base, PGSIZE, (PTE_R | PTE_W) as c_int);
            }
        }
    }

    // PCIe regions.
    if platform.has_pcie != 0 && __pcie_ecam_mmio_base != 0 {
        for i in 0..(platform.pcie_reg_count as usize) {
            let reg = &platform.pcie_reg[i];
            let base = reg.base;
            let size = reg.size;
            if base == 0 || size == 0 { continue; }
            let aligned_base = pgrounddown(base);
            let aligned_size = pgroundup(base + size) - aligned_base;
            kvmmap_safe(
                kpgtbl, aligned_base, aligned_base, aligned_size,
                (PTE_R | PTE_W) as c_int,
            );
        }
        if platform.has_virtio != 0 && __e1000_pci_mmio_base != 0 {
            kvmmap(
                kpgtbl, __e1000_pci_mmio_base, __e1000_pci_mmio_base,
                0x20000, (PTE_R | PTE_W) as c_int,
            );
        }
    }

    // PLIC.
    if platform.plic_base != 0 && platform.plic_size != 0 {
        kvmmap(
            kpgtbl, __plic_mmio_base, __plic_mmio_base,
            platform.plic_size, (PTE_R | PTE_W) as c_int,
        );
    }

    // EMAC.
    if platform.has_emac != 0 {
        let n = core::cmp::min(platform.emac_count as usize, EMAC_MAX as usize);
        let limit = core::cmp::min(n, platform.emac.len());
        for i in 0..limit {
            let e = &platform.emac[i];
            if e.base != 0 && e.size != 0 {
                let base = pgrounddown(e.base);
                let size = pgroundup(e.base + e.size) - base;
                kvmmap_safe(kpgtbl, base, base, size, (PTE_R | PTE_W) as c_int);
            }
            if e.apmu_base != 0 && e.ctrl_reg != 0 {
                let pg = pgrounddown(e.apmu_base as u64 + e.ctrl_reg as u64);
                kvmmap_safe(kpgtbl, pg, pg, PGSIZE, (PTE_R | PTE_W) as c_int);
            }
            if e.apmu_base != 0 && e.dline_reg != 0 {
                let pg = pgrounddown(e.apmu_base as u64 + e.dline_reg as u64);
                kvmmap_safe(kpgtbl, pg, pg, PGSIZE, (PTE_R | PTE_W) as c_int);
            }
        }
    }

    // SDHCI.
    if platform.has_sdhci != 0 {
        let n = core::cmp::min(platform.sdhci_count as usize, SDHCI_MAX as usize);
        let limit = core::cmp::min(n, platform.sdhci.len());
        for i in 0..limit {
            let s = &platform.sdhci[i];
            if s.base != 0 && s.size != 0 {
                let base = pgrounddown(s.base);
                let size = pgroundup(s.base + s.size) - base;
                kvmmap_safe(kpgtbl, base, base, size, (PTE_R | PTE_W) as c_int);
            }
            if s.apmu_base != 0 && s.apmu_offset != 0 {
                let pg = pgrounddown(s.apmu_base as u64 + s.apmu_offset as u64);
                kvmmap_safe(kpgtbl, pg, pg, PGSIZE, (PTE_R | PTE_W) as c_int);
            }
        }
        if platform.sdhci_count > 0 && platform.sdhci[0].apbc_base != 0 {
            let apbc_aib = platform.sdhci[0].apbc_base as u64 + 0x3C;
            let pg = pgrounddown(apbc_aib);
            kvmmap_safe(kpgtbl, pg, pg, PGSIZE, (PTE_R | PTE_W) as c_int);
        }
    }

    // Kernel text.
    let entry_addr = &raw const _entry as u64;
    let etext_addr = &raw const etext as u64;
    kvmmap(
        kpgtbl, entry_addr, entry_addr,
        etext_addr - entry_addr,
        (PTE_R | PTE_X) as c_int,
    );

    // Trampoline / per-cpu mapping.
    let trampoline_addr = &raw const trampoline as u64;
    let tramp_data_addr = &raw const _trampoline_data as u64;
    let sig_tramp_addr  = &raw const sig_trampoline as u64;
    let cpus_addr       = &raw const cpus as u64;

    kvmmap(kpgtbl, TRAMPOLINE, trampoline_addr, PGSIZE, (PTE_R | PTE_X) as c_int);
    kvmmap(kpgtbl, TRAMPOLINE_DATA, tramp_data_addr, PGSIZE, PTE_R as c_int);
    kvmmap(kpgtbl, tramp_data_addr, tramp_data_addr, PGSIZE, (PTE_R | PTE_W) as c_int);
    kvmmap(kpgtbl, TRAMPOLINE_CPULOCAL, cpus_addr, PGSIZE, (PTE_R | PTE_W) as c_int);
    kvmmap(kpgtbl, cpus_addr, cpus_addr, PGSIZE, (PTE_R | PTE_W) as c_int);
    kvmmap(
        kpgtbl, SIG_TRAMPOLINE, sig_tramp_addr, PGSIZE,
        (PTE_R | PTE_X | PTE_U) as c_int,
    );

    printf(
        b"trampoline 0x%lx -> %p\n\0".as_ptr() as *const c_char,
        TRAMPOLINE, trampoline_addr as *const c_void,
    );
    printf(
        b"trampoline data 0x%lx -> %p\n\0".as_ptr() as *const c_char,
        TRAMPOLINE_DATA, tramp_data_addr as *const c_void,
    );
    printf(
        b"trampoline cpu local 0x%lx -> %p\n\0".as_ptr() as *const c_char,
        TRAMPOLINE_CPULOCAL, cpus_addr as *const c_void,
    );
    printf(
        b"signal trampoline 0x%lx -> %p\n\0".as_ptr() as *const c_char,
        SIG_TRAMPOLINE, sig_tramp_addr as *const c_void,
    );

    // rodata / data / bss / remainder of physical memory.
    let rodata_addr = &raw const _rodata as u64;
    let rodata_end_addr = &raw const _rodata_end as u64;
    let data_ktlb_addr = &raw const _data_ktlb as u64;
    let data_addr = &raw const _data as u64;
    let data_end_addr = &raw const _data_end as u64;
    let bss_addr = &raw const _bss as u64;
    let bss_end_addr = &raw const _bss_end as u64;

    kvmmap(kpgtbl, rodata_addr, rodata_addr, rodata_end_addr - rodata_addr, PTE_R as c_int);
    kvmmap(kpgtbl, data_ktlb_addr, data_ktlb_addr, PGSIZE, (PTE_R | PTE_W) as c_int);
    kvmmap(kpgtbl, data_addr, data_addr, data_end_addr - data_addr, (PTE_R | PTE_W) as c_int);
    kvmmap(kpgtbl, bss_addr, bss_addr, bss_end_addr - bss_addr, (PTE_R | PTE_W) as c_int);
    kvmmap(
        kpgtbl, bss_end_addr, bss_end_addr,
        __physical_memory_end - bss_end_addr,
        (PTE_R | PTE_W) as c_int,
    );

    // Kernel symbols sections.
    let ksym_start = &raw const _ksymbols_start as u64;
    let ksym_end = &raw const _ksymbols_end as u64;
    let ksym_size = ksym_end - ksym_start;
    if ksym_size > 0 {
        kvmmap(kpgtbl, ksym_start, ksym_start, ksym_size, PTE_R as c_int);
        printf(
            b"kernel symbols 0x%lx - 0x%lx (size: %ld)\n\0".as_ptr() as *const c_char,
            ksym_start, ksym_end, ksym_size,
        );
    }

    let ksym_idx_start = &raw const _ksymbols_idx_start as u64;
    let ksym_idx_end = &raw const _ksymbols_idx_end as u64;
    let ksym_idx_size = ksym_idx_end - ksym_idx_start;
    if ksym_idx_size > 0 {
        printf(
            b"kernel symbols index 0x%lx - 0x%lx (size: %ld)\n\0".as_ptr() as *const c_char,
            ksym_idx_start, ksym_idx_end, ksym_idx_size,
        );
    }

    kpgtbl
}

// Silence dead-code lints on the unused mask constant when not referenced.
#[allow(dead_code)]
const _UNUSED_KEEP: u64 = PAGE_FLAG_TYPE_MASK;
