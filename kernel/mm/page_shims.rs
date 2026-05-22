//! Rust port of `kernel/mm/page_shims.c`.
//!
//! Replaces the deleted C shim. Provides the `xv6_*` C-ABI helpers that
//! `kernel/rust/src/page.rs` calls into via its `extern "C"` block, plus
//! the statistics/integrity printers and the `sys_memstat` syscall body
//! that the rest of the kernel links against.
//!
//! Layout `_Static_assert`s from the C file are replicated as Rust
//! compile-time `const _: () = { assert!(...); };` so any ABI drift
//! between `page.rs`'s mirror types and the bindgen-generated C types
//! fails the build immediately.

#![allow(non_snake_case)]
#![allow(non_upper_case_globals)]


macro_rules! u {
    ($($tokens:tt)*) => {
        unsafe { $($tokens)* }
    };
}

use core::ffi::{c_char, c_int, c_void};
use core::mem::{align_of, size_of};

use crate::bindings::{list_node_t, platform};
use crate::mm::page::{
    BuddyPool, Page, PAGE_BUDDY_MAX_ORDER, PAGE_SIZE, PAGE_TYPE_BUDDY,
    __buddy_pools,
};

// ---------------------------------------------------------------------------
// Layout pins (compile-time). These were `_Static_assert`s in the C file.
// page.rs uses its own hand-rolled `Page` / `BuddyPool`, so we pin the
// Rust-side sizes here. The Rust types must match the C ABI exactly;
// page_shims.c (now deleted) used to also pin the C-side layout.
// ---------------------------------------------------------------------------
const _: () = {
    assert!(size_of::<Page>() == 128, "Page size != 128");
    assert!(align_of::<Page>() == 64, "Page alignment != 64");
    assert!(size_of::<BuddyPool>() == 128, "BuddyPool size != 128");
    assert!(align_of::<BuddyPool>() == 64, "BuddyPool alignment != 64");
    assert!(size_of::<list_node_t>() == 16, "list_node_t size != 16");
    assert!(PAGE_BUDDY_MAX_ORDER == 10, "PAGE_BUDDY_MAX_ORDER must be 10");
};

// SLAB_DEFAULT_ORDER — pcpu cache covers orders 0..=8.
const LOCAL_PCPU_CACHE_MAX_ORDER: u64 = 8;
const NCPU: usize = 8;

// MEMSTAT_* flag bits (mirror of kernel/inc/uabi/memstat.h).
const MEMSTAT_VERBOSE: u32 = 1 << 0;
const MEMSTAT_DETAILED: u32 = 1 << 1;
const MEMSTAT_INCLUDE_SLAB: u32 = 1 << 2;
const MEMSTAT_INCLUDE_BUDDY: u32 = 1 << 3;
const MEMSTAT_ADD_FREE: u32 = 1 << 4;
const MEMSTAT_ADD_USED: u32 = 1 << 5;

// ---------------------------------------------------------------------------
// Externs into the C kernel + into peer Rust modules.
// ---------------------------------------------------------------------------
mod ffi {
    use super::*;
    use core::ffi::{c_char, c_int, c_void};
    unsafe extern "C" {
        pub safe fn printf(fmt: *const c_char, ...) -> c_int;
        pub safe fn __panic_start();
        pub safe fn __panic_end() -> !;
        pub safe fn argint(n: c_int, ip: *mut c_int);

        // Implemented in slab_shims.rs.
        pub safe fn slab_dump_all(detail: c_int);

        // Implemented in page.rs.
        pub safe fn page_buddy_stat(ret_arr: *mut u64, empty_arr: *mut bool, n: u64);
        pub safe fn xv6_buddy_pool_count(order: u64) -> u64;
        pub safe fn xv6_pcpu_cache_count(cpu: u32, order: u64) -> u32;
        pub safe fn xv6_buddy_pool_lock_range_all();
        pub safe fn xv6_buddy_pool_unlock_range_all();
        pub safe fn xv6_total_managed_pages() -> u64;
        pub safe fn __check_page_pointer_in_range(ptr: *mut c_void);
        pub safe fn __page_to_pa(page: *mut Page) -> u64;
    }
}
use ffi::*;

// ---------------------------------------------------------------------------
// (2) Platform info accessors used by Rust during init (page.rs).
// ---------------------------------------------------------------------------
#[no_mangle]
pub extern "C" fn xv6_platform_has_ramdisk() -> c_int {
    u! {

    platform.has_ramdisk

    }
}
#[no_mangle]
pub extern "C" fn xv6_platform_ramdisk_base() -> u64 {
    u! {

    platform.ramdisk_base

    }
}
#[no_mangle]
pub extern "C" fn xv6_platform_ramdisk_size() -> u64 {
    u! {

    platform.ramdisk_size

    }
}
#[no_mangle]
pub extern "C" fn xv6_platform_reserved_count() -> c_int {
    u! {

    platform.reserved_count

    }
}
#[no_mangle]
pub extern "C" fn xv6_platform_reserved_base(i: c_int) -> u64 {
    u! {

    if i < 0 || i >= platform.reserved_count {
        return 0;
    }
    (*platform.reserved.offset(i as isize)).base

    }
}
#[no_mangle]
pub extern "C" fn xv6_platform_reserved_size(i: c_int) -> u64 {
    u! {

    if i < 0 || i >= platform.reserved_count {
        return 0;
    }
    (*platform.reserved.offset(i as isize)).size

    }
}

// ---------------------------------------------------------------------------
// (3) Logging shims — one per printf shape the buddy init path needs.
// ---------------------------------------------------------------------------
static FMT_INIT_RANGE: &[u8] = b"init pages from 0x%lx to 0x%lx with flags 0x%lx\n\0";
static FMT_INVALID_RANGE: &[u8] = b"invalid range, pa_start: 0x%lx, pa_end: 0x%lx\n\0";
static FMT_INVALID_BASE: &[u8] = b"invalid range base, pa_start: 0x%lx, pa_end: 0x%lx\n\0";
static FMT_INVALID_FLAGS: &[u8] = b"invalid flags: 0x%lx\n\0";
static FMT_GET_PAGE: &[u8] = b"failed to get page for physical address 0x%lx\n\0";
static FMT_RESERVED_OOR: &[u8] = b"reserved mem out of range: 0x%lx to 0x%lx\n\0";
static FMT_RESERVING: &[u8] = b"reserving pages from 0x%lx to 0x%lx\n\0";
static FMT_INIT_SUMMARY: &[u8] =
    b"page_buddy_init(): page array at 0x%lx, size 0x%lx\n\0";
static FMT_MANAGED_RANGE: &[u8] = b"__managed_start: 0x%lx, __managed_end: 0x%lx\n\0";
static FMT_BUDDY_RANGE: &[u8] = b"buddy init range: 0x%lx - 0x%lx\n\0";

#[no_mangle]
pub extern "C" fn xv6_buddy_log_init_range(s: u64, e: u64, f: u64) {
    u! {

    printf(FMT_INIT_RANGE.as_ptr() as *const c_char, s, e, f);

    }
}
#[no_mangle]
pub extern "C" fn xv6_buddy_log_init_invalid_range(s: u64, e: u64) {
    u! {

    printf(FMT_INVALID_RANGE.as_ptr() as *const c_char, s, e);

    }
}
#[no_mangle]
pub extern "C" fn xv6_buddy_log_init_invalid_base(s: u64, e: u64) {
    u! {

    printf(FMT_INVALID_BASE.as_ptr() as *const c_char, s, e);

    }
}
#[no_mangle]
pub extern "C" fn xv6_buddy_log_init_invalid_flags(f: u64) {
    u! {

    printf(FMT_INVALID_FLAGS.as_ptr() as *const c_char, f);

    }
}
#[no_mangle]
pub extern "C" fn xv6_buddy_log_init_get_page(pa: u64) {
    u! {

    printf(FMT_GET_PAGE.as_ptr() as *const c_char, pa);

    }
}
#[no_mangle]
pub extern "C" fn xv6_buddy_log_reserved_out_of_range(s: u64, e: u64) {
    u! {

    printf(FMT_RESERVED_OOR.as_ptr() as *const c_char, s, e);

    }
}
#[no_mangle]
pub extern "C" fn xv6_buddy_log_reserving(s: u64, e: u64) {
    u! {

    printf(FMT_RESERVING.as_ptr() as *const c_char, s, e);

    }
}
#[no_mangle]
pub extern "C" fn xv6_buddy_log_init_summary(p: *mut c_void, sz: usize) {
    u! {

    printf(FMT_INIT_SUMMARY.as_ptr() as *const c_char, p as u64, sz as u64);

    }
}
#[no_mangle]
pub extern "C" fn xv6_buddy_log_init_range_phys(s: u64, e: u64) {
    u! {

    printf(FMT_MANAGED_RANGE.as_ptr() as *const c_char, s, e);

    }
}
#[no_mangle]
pub extern "C" fn xv6_buddy_log_buddy_range(s: u64, e: u64) {
    u! {

    printf(FMT_BUDDY_RANGE.as_ptr() as *const c_char, s, e);

    }
}

// ---------------------------------------------------------------------------
// (4) Panic entry point. `!`-returning so callers can use Rust's `!` type.
// ---------------------------------------------------------------------------
static PANIC_FMT: &[u8] = b"%s\n\0";

#[no_mangle]
pub extern "C" fn xv6_buddy_panic(msg: *const c_char) -> ! {
    u! {

    __panic_start();
    printf(PANIC_FMT.as_ptr() as *const c_char, msg);
    __panic_end()

    }
}

// ---------------------------------------------------------------------------
// (5) Statistics helpers.
// ---------------------------------------------------------------------------
fn print_size(bytes: u64) { u! {
    static F_G: &[u8] = b"%ld.%ldG\0";
    static F_M: &[u8] = b"%ld.%ldM\0";
    static F_K: &[u8] = b"%ldK\0";
    static F_B: &[u8] = b"%ldB\0";
    if bytes >= (1u64 << 30) {
        let gb = bytes >> 30;
        let mb = (bytes & ((1u64 << 30) - 1)) >> 20;
        printf(F_G.as_ptr() as *const c_char, gb, (mb * 10) / 1024);
    } else if bytes >= (1u64 << 20) {
        let mb = bytes >> 20;
        let kb = (bytes & ((1u64 << 20) - 1)) >> 10;
        printf(F_M.as_ptr() as *const c_char, mb, (kb * 10) / 1024);
    } else if bytes >= (1u64 << 10) {
        let kb = bytes >> 10;
        printf(F_K.as_ptr() as *const c_char, kb);
    } else {
        printf(F_B.as_ptr() as *const c_char, bytes);
    }
}}
const ORDER_COUNT: usize = (PAGE_BUDDY_MAX_ORDER + 1) as usize;

fn buddy_stat_totals(
    total_free_pages: &mut u64,
    total_cached_pages: &mut u64,
    ret_arr: &mut [u64; ORDER_COUNT],
    empty_arr: &mut [bool; ORDER_COUNT],
) {
    *total_free_pages = 0;
    *total_cached_pages = 0;

    page_buddy_stat(
        ret_arr.as_mut_ptr(),
        empty_arr.as_mut_ptr(),
        ORDER_COUNT as u64,
    );

    for i in 0..=(PAGE_BUDDY_MAX_ORDER as usize) {
        let order_pages = (1u64 << i) * ret_arr[i];
        *total_free_pages += order_pages;

        if (i as u64) <= LOCAL_PCPU_CACHE_MAX_ORDER {
            let mut cache_total: u64 = 0;
            for cpu in 0..NCPU {
                cache_total += xv6_pcpu_cache_count(cpu as u32, i as u64) as u64;
            }
            if cache_total > 0 {
                *total_cached_pages += (1u64 << i) * cache_total;
            }
        }
    }
}
#[no_mangle]
pub extern "C" fn print_buddy_system_stat(detailed: c_int) {
    u! {

    static F_SUMMARY: &[u8] = b"Buddy: %ld free + %ld cached = %ld pages (\0";
    static F_HDR: &[u8] = b"Buddy System Statistics:\n\0";
    static F_BAR: &[u8] = b"========================\n\0";
    static F_BAR2: &[u8] = b"------------------------\n\0";
    static F_ORDER: &[u8] = b"order(%d): %ld blocks (\0";
    static F_CACHED: &[u8] = b" + %ld cached (\0";
    static F_CLOSE: &[u8] = b")\n\0";
    static F_RCLOSE: &[u8] = b")\0";
    static F_NEWLINE: &[u8] = b"\n\0";

    let mut total_free_pages: u64 = 0;
    let mut total_cached_pages: u64 = 0;
    let mut ret_arr: [u64; ORDER_COUNT] = [0; ORDER_COUNT];
    let mut empty_arr: [bool; ORDER_COUNT] = [false; ORDER_COUNT];

    buddy_stat_totals(
        &mut total_free_pages,
        &mut total_cached_pages,
        &mut ret_arr,
        &mut empty_arr,
    );

    if detailed <= 0 {
        printf(
            F_SUMMARY.as_ptr() as *const c_char,
            total_free_pages,
            total_cached_pages,
            total_free_pages + total_cached_pages,
        );
        print_size((total_free_pages + total_cached_pages) * PAGE_SIZE);
        printf(F_CLOSE.as_ptr() as *const c_char);
        return;
    }

    printf(F_HDR.as_ptr() as *const c_char);
    printf(F_BAR.as_ptr() as *const c_char);

    for i in 0..=(PAGE_BUDDY_MAX_ORDER as usize) {
        let order_pages = (1u64 << i) * ret_arr[i];
        let order_bytes = order_pages * PAGE_SIZE;

        printf(F_ORDER.as_ptr() as *const c_char, i as c_int, ret_arr[i]);
        print_size(order_bytes);
        printf(F_RCLOSE.as_ptr() as *const c_char);

        if (i as u64) <= LOCAL_PCPU_CACHE_MAX_ORDER {
            let mut cache_total: u64 = 0;
            for cpu in 0..NCPU {
                cache_total += xv6_pcpu_cache_count(cpu as u32, i as u64) as u64;
            }
            if cache_total > 0 {
                let cached_pages = (1u64 << i) * cache_total;
                let cached_bytes = cached_pages * PAGE_SIZE;
                printf(F_CACHED.as_ptr() as *const c_char, cache_total);
                print_size(cached_bytes);
                printf(F_RCLOSE.as_ptr() as *const c_char);
            }
        }
        printf(F_NEWLINE.as_ptr() as *const c_char);
    }

    printf(F_BAR2.as_ptr() as *const c_char);
    printf(
        F_SUMMARY.as_ptr() as *const c_char,
        total_free_pages,
        total_cached_pages,
        total_free_pages + total_cached_pages,
    );
    print_size((total_free_pages + total_cached_pages) * PAGE_SIZE);
    printf(F_CLOSE.as_ptr() as *const c_char);

    }
}

// ---------------------------------------------------------------------------
// check_buddy_system_integrity — walk every buddy pool and verify counts +
// pointers. Diagnostic only; not on the hot path. Replicates the
// `list_foreach_node_safe` traversal from the C original by hand.
// ---------------------------------------------------------------------------
static FMT_PREVNEXT: &[u8] = b"prev page: %p, next page: %p\n\0";
static FMT_ENTRY: &[u8] = b"count = %d, buddy page: %p, order: %d, physical: 0x%lx\n\0";

fn page_from_lru_entry(node: *mut list_node_t)-> *mut Page  { u! {
    // The `lru_entry` lives at the start of Page::union_bytes, which sits
    // at byte offset 48 of `Page` (see page.rs L84-93).
    (node as *mut u8).wrapping_sub(48) as *mut Page
}}
fn assert_msg(cond: bool, msg: &'static [u8]) { u! {
    if !cond {
        xv6_buddy_panic(msg.as_ptr() as *const c_char);
    }
}}

#[no_mangle]
pub extern "C" fn check_buddy_system_integrity() {
    u! {

    let mut total_free_pages: u64 = 0;
    xv6_buddy_pool_lock_range_all();
    for i in 0..=(PAGE_BUDDY_MAX_ORDER as usize) {
        let pool: *mut BuddyPool = &raw mut __buddy_pools[i];
        let count_field = (*pool).count as i64;
        // page.rs's `ListNode` has private fields; reinterpret as the
        // bindgen-generated `list_node_t` (public `prev` / `next`) so we
        // can walk the list here. Both types are `#[repr(C)]` with the
        // same two-pointer layout.
        let head = (&raw mut (*pool).lru_head) as *mut list_node_t;
        let head_prev = (*head).prev;
        let head_next = (*head).next;
        let empty = head_prev == head || head_prev.is_null();

        assert_msg(count_field >= 0, b"buddy pool count is negative\0");
        assert_msg(
            empty || count_field > 0,
            b"buddy pool is not empty but count is zero\0",
        );
        assert_msg(
            !empty || count_field == 0,
            b"buddy pool is empty but count is not zero\0",
        );
        total_free_pages += (1u64 << i) * count_field as u64;

        if !empty {
            __check_page_pointer_in_range(head_prev as *mut c_void);
            __check_page_pointer_in_range(head_next as *mut c_void);
            printf(
                FMT_PREVNEXT.as_ptr() as *const c_char,
                head_prev,
                head_next,
            );
        }

        // list_foreach_node_safe: walk from head.next until we loop back.
        let mut count_check = count_field as i32;
        let mut pos = head_next;
        while !pos.is_null() && pos != head {
            let next = (*pos).next;
            let page = page_from_lru_entry(pos);
            assert_msg(
                (*page).is_type(PAGE_TYPE_BUDDY),
                b"buddy page is not a group head\0",
            );
            assert_msg(
                (*page).buddy_order() as usize == i,
                b"buddy page order mismatch\0",
            );
            __check_page_pointer_in_range(page as *mut c_void);
            assert_msg(
                __page_to_pa(page) == (*page).physical_address,
                b"buddy page physical address mismatch\0",
            );
            count_check -= 1;
            printf(
                FMT_ENTRY.as_ptr() as *const c_char,
                count_check as c_int,
                page,
                (*page).buddy_order() as c_int,
                (*page).physical_address,
            );
            pos = next;
        }
        assert_msg(count_check == 0, b"buddy pool count mismatch\0");
    }
    xv6_buddy_pool_unlock_range_all();
    let _ = total_free_pages;

    }
}

// ---------------------------------------------------------------------------
// sys_memstat — syscall body. Reads syscall arg 0 (flags), prints requested
// stats, returns combined byte counts depending on flags.
// ---------------------------------------------------------------------------
static FMT_FREE_LABEL: &[u8] = b"Free: \0";
static FMT_USED_LABEL: &[u8] = b"Used: \0";
static FMT_NEWLINE_ONLY: &[u8] = b"\n\0";

#[no_mangle]
pub extern "C" fn sys_memstat() -> u64 {
    u! {

    let mut flags_arg: c_int = 0;
    argint(0, &mut flags_arg);
    let flags = flags_arg as u32;

    let mut total_free_pages: u64 = 0;
    let mut total_cached_pages: u64 = 0;
    let mut ret_arr: [u64; ORDER_COUNT] = [0; ORDER_COUNT];
    let mut empty_arr: [bool; ORDER_COUNT] = [false; ORDER_COUNT];

    if (flags & MEMSTAT_INCLUDE_BUDDY) != 0 {
        if (flags & MEMSTAT_DETAILED) != 0 {
            print_buddy_system_stat(1);
        } else if (flags & MEMSTAT_VERBOSE) != 0 {
            print_buddy_system_stat(0);
        }
    }
    if (flags & MEMSTAT_INCLUDE_SLAB) != 0 {
        if (flags & MEMSTAT_DETAILED) != 0 {
            slab_dump_all(2);
        } else if (flags & MEMSTAT_VERBOSE) != 0 {
            slab_dump_all(1);
        }
    }

    buddy_stat_totals(
        &mut total_free_pages,
        &mut total_cached_pages,
        &mut ret_arr,
        &mut empty_arr,
    );
    let free_bytes = (total_free_pages + total_cached_pages) * PAGE_SIZE;
    let managed_bytes = xv6_total_managed_pages() * PAGE_SIZE;
    let used_bytes = if managed_bytes > free_bytes {
        managed_bytes - free_bytes
    } else {
        0
    };

    if (flags & (MEMSTAT_VERBOSE | MEMSTAT_DETAILED)) != 0 {
        if (flags & MEMSTAT_ADD_FREE) != 0 {
            printf(FMT_FREE_LABEL.as_ptr() as *const c_char);
            print_size(free_bytes);
            printf(FMT_NEWLINE_ONLY.as_ptr() as *const c_char);
        }
        if (flags & MEMSTAT_ADD_USED) != 0 {
            printf(FMT_USED_LABEL.as_ptr() as *const c_char);
            print_size(used_bytes);
            printf(FMT_NEWLINE_ONLY.as_ptr() as *const c_char);
        }
    }

    let mut ret: u64 = 0;
    if (flags & MEMSTAT_ADD_FREE) != 0 {
        ret += free_bytes;
    }
    if (flags & MEMSTAT_ADD_USED) != 0 {
        ret += used_bytes;
    }
    ret

    }
}