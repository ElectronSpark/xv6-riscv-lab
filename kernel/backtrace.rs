//! Kernel symbol table + frame-pointer backtrace walking.
//!
//! Rust port of `kernel/backtrace.c` (Phase 2 Wave 5, see
//! `docs/rustify/phase2_plan.md`). Builds an in-memory rb-tree index
//! ([`rbtree.rs`]/[`bintree.rs`], Wave 1) over the `.ksymbols` section
//! embedded in the kernel image by `scripts/embed_ksymbols.py` (raw text
//! format parsed byte-for-byte below, matching `gen_ksymbols_section.py`'s
//! output), then answers "what symbol/file/line is this return address
//! in?" for the two public backtrace printers.
//!
//! # Panic-path safety
//!
//! [`print_backtrace`]/[`print_thread_backtrace`] are called from the
//! panic path (`printf.rs`) and from interrupt/scheduler code (`ipi.c`,
//! `proc/proc_shims.rs`) where sleeping or blocking is not an option. This
//! file therefore performs **no allocation and takes no locks**, exactly
//! like the C original: [`ksymbols_init`] populates a fixed-capacity pool
//! (`[_ksymbols_idx_start, _ksymbols_idx_end)`, sized by
//! `KERNEL_SYMBOLS_IDX_SIZE` in `kernel/CMakeLists.txt`) once, early in
//! boot, on the boot hart alone, strictly before any other hart or
//! interrupt can observe it; every access after that point is read-only,
//! so no synchronisation is needed for the lookup path either.
//!
//! `extern char stack0[];` from the C original is dropped: grep confirms
//! it was declared but never referenced anywhere in `backtrace.c`.

#![allow(non_upper_case_globals)]

use core::ffi::{c_char, c_int, c_void};
use core::ptr;

use crate::bindings::{context, rb_node, rb_root};
use crate::bintree::RbOps;

// ===========================================================================
// Externs: printf/panic (`printf.rs`) and the linker-defined boundaries of
// the embedded symbol sections (`kernel/kernel.ld.in`), mirroring the
// `mm/vm_pgtab.rs` precedent for referencing the same symbols.
// ===========================================================================
mod ffi {
    use core::ffi::c_char;

    unsafe extern "C" {
        pub safe static _ksymbols_start: u8;
        pub safe static _ksymbols_end: u8;
        pub safe static _ksymbols_idx_start: u8;
        pub safe static _ksymbols_idx_end: u8;

        // printf is variadic, so it cannot be declared `safe`.
    }
}

// ===========================================================================
// Kernel symbol pool entry -- one per address-range record in the embedded
// `.ksymbols` text blob. Mirrors the C `__ksymbols_t`; `rb` is a real
// bindgen `rb_node` so `container_of`/`rb_node_init` interop with
// `bintree.rs`/`rbtree.rs` exactly as the C original did.
// ===========================================================================
#[repr(C)]
struct KSymEntry {
    rb: rb_node,
    start_addr: *mut c_void,
    line: u32,
    symbol: *const c_char,
    symbol_len: u16,
    filename: *const c_char,
    filename_len: u16,
}

// ---------------------------------------------------------------------------
// rb-tree comparators. Two-level key (start_addr, then node address as a
// tie-breaker) so duplicate `start_addr`s are still totally ordered --
// ported 1:1 from the C `ksym_keys_cmp`/`ksym_keys_cmp_rdown`/
// `ksym_get_key`, including `ksym_keys_cmp_rdown`'s `a == 0` guard, which
// is dead under `bintree.rs`'s current calling convention (`rb_keys_cmp`
// always passes real node/search-key addresses, never a literal 0) but is
// kept for exact behavioural parity with the original.
// ---------------------------------------------------------------------------

/// Shared `get_key` body for both [`KsymRbOps`] and [`KsymRdownRbOps`]
/// (was the single free fn `ksym_get_key`, address-taken in both of the
/// old `rb_root_opts` tables).
///
/// # Safety
/// `node` must be a live `rb_node` embedded in a `KSymEntry` (the only
/// tree this ops table is ever installed on).
unsafe fn ksym_get_key_impl(node: *mut rb_node) -> u64 {
    let entry: *mut KSymEntry =
        crate::mm::cffi::container_of(node, core::mem::offset_of!(KSymEntry, rb));
    entry as u64
}

/// TRAIT-OPS: `RbOps` impl for the forward (`bt_search_sym`'s primary)
/// ksym lookup order. Was the free-fn pair `ksym_keys_cmp`/`ksym_get_key`
/// bound into the old `KSYM_RB_OPTS` fn-pointer table; bodies moved
/// verbatim.
struct KsymRbOps;
impl RbOps for KsymRbOps {
    unsafe fn keys_cmp(&self, a: u64, b: u64) -> c_int {
        let sym_a = a as *const KSymEntry;
        let sym_b = b as *const KSymEntry;
        // SAFETY: the rb-tree machinery only ever calls this with `a`/`b`
        // equal to a live `KSymEntry` address: `a` from `get_key` (a pool
        // entry already linked into the tree) or `b` from a caller-owned,
        // still-live `KSymEntry`/dummy search key on the stack (see
        // `bt_search_sym`).
        unsafe {
            let sa = (*sym_a).start_addr as u64;
            let sb = (*sym_b).start_addr as u64;
            if sa < sb {
                -1
            } else if sa > sb {
                1
            } else if a < b {
                -1
            } else if a > b {
                1
            } else {
                0
            }
        }
    }
    unsafe fn get_key(&self, node: *mut rb_node) -> u64 {
        unsafe { ksym_get_key_impl(node) }
    }
}

/// TRAIT-OPS: `RbOps` impl for the "round-down" ksym lookup order (finds
/// the greatest entry `<=` a search key). Was the free-fn pair
/// `ksym_keys_cmp_rdown`/`ksym_get_key` bound into the old
/// `KSYM_RB_RDOWN_OPTS` fn-pointer table; bodies moved verbatim.
struct KsymRdownRbOps;
impl RbOps for KsymRdownRbOps {
    unsafe fn keys_cmp(&self, a: u64, b: u64) -> c_int {
        let sym_a = a as *const KSymEntry;
        let sym_b = b as *const KSymEntry;
        // SAFETY: see `KsymRbOps::keys_cmp`.
        unsafe {
            let sa = (*sym_a).start_addr as u64;
            let sb = (*sym_b).start_addr as u64;
            if sa < sb {
                return -1;
            }
            if sa > sb {
                return 1;
            }
        }
        // Equal start_addr: return 1 to keep searching left (find the
        // minimum entry with this start_addr).
        if a == 0 {
            0 // Dummy node comparison (see module doc above).
        } else {
            1
        }
    }
    unsafe fn get_key(&self, node: *mut rb_node) -> u64 {
        unsafe { ksym_get_key_impl(node) }
    }
}

static KSYM_RB_OPS: KsymRbOps = KsymRbOps;
static KSYM_RB_RDOWN_OPS: KsymRdownRbOps = KsymRdownRbOps;

// ===========================================================================
// Global symbol-table state. Single-writer-during-`ksymbols_init`,
// read-only thereafter (see module doc) -- the same lifecycle the crate
// already relies on elsewhere for FDT-populated boot state
// (`__physical_memory_start`, `platform`, ...), just Rust-owned here
// instead of C-owned.
// ===========================================================================
static mut KSYM_RB_ROOT: rb_root = rb_root { node: ptr::null_mut(), opts: None };
static mut KSYM_ENTRIES_BASE: *mut KSymEntry = ptr::null_mut();
static mut KSYM_COUNT: i32 = -1;

const BACKTRACE_MAX_DEPTH: u32 = 32;
const PAGE_SHIFT: u32 = 12;
const PGSIZE: u64 = 1 << PAGE_SHIFT;

/// Zero-sized facade for the kernel symbol table + frame-pointer
/// backtrace walker (KERNEL-OO final wave): no per-instance state
/// (everything is a module `static`), so these become associated fns,
/// raw params, no `&self`.
pub(crate) struct Backtrace;

impl Backtrace {
    /// Port of the C `static inline uint64 strtoul(...)` in
    /// `kernel/inc/string.h`. That function is a per-translation-unit inline
    /// (never emitted as an out-of-line symbol string.c/string.rs could
    /// export), so it is reimplemented here rather than declared `extern`.
    /// Returns `(value, bytes_consumed)`.
    fn strtoul(s: &[u8], base: u32) -> (u64, usize) {
        let mut result: u64 = 0;
        let mut i = 0;
        while i < s.len() {
            let digit: u32 = match s[i] {
                c @ b'0'..=b'9' => (c - b'0') as u32,
                c @ b'a'..=b'f' => (c - b'a' + 10) as u32,
                c @ b'A'..=b'F' => (c - b'A' + 10) as u32,
                _ => break,
            };
            if digit >= base {
                break;
            }
            // Matches the C original's plain `result = result * base + digit`,
            // which is well-defined wraparound for `uint64_t` -- no UB to
            // replicate, just the same modular arithmetic.
            result = result.wrapping_mul(base as u64).wrapping_add(digit as u64);
            i += 1;
        }
        (result, i)
    }
}

/// Rust port of `ksymbols_init()`. Parses the embedded `.ksymbols` text
/// blob (`_ksymbols_start..​_ksymbols_end`) into the rb-tree-indexed pool
/// living in `[_ksymbols_idx_start, _ksymbols_idx_end)`. Called exactly
/// once from `start_kernel.c`, early in boot on the boot hart alone.
///
/// # Safety
/// Must be called at most once, before any other hart or interrupt can
/// observe `KSYM_RB_ROOT`/`KSYM_ENTRIES_BASE`/`KSYM_COUNT` -- matches the
/// call site's existing contract (`start_kernel.c`'s init-order comment in
/// `docs/rustify/phase2_plan.md` §1).
// P3-1D mesh sweep: only caller is `start_kernel.rs` (crate-path `use`, not
// an `extern` redeclaration) -- demoted.
impl Backtrace {
    pub(crate) unsafe extern "C" fn ksymbols_init() {
    // SAFETY: see function-level `# Safety`; the whole body operates on
    // the boot-time-single-writer statics above and on the read-only
    // embedded `.ksymbols` section (part of the loaded kernel image, never
    // freed).
    unsafe {
        let idx_start = &raw const ffi::_ksymbols_idx_start as *mut u8;
        let idx_end = &raw const ffi::_ksymbols_idx_end as *mut u8;
        KSYM_ENTRIES_BASE = idx_start as *mut KSymEntry;
        KSYM_COUNT = 0;
        KSYM_RB_ROOT = rb_root { node: ptr::null_mut(), opts: Some(&KSYM_RB_OPS) };

        let sym_start = &raw const ffi::_ksymbols_start as *const u8;
        let sym_end = &raw const ffi::_ksymbols_end as *const u8;
        let sym_size = (sym_end as usize).wrapping_sub(sym_start as usize);

        if sym_size == 0 {
            crate::kprintln!("ksymbols: no embedded symbols found");
            return;
        }
        crate::kprintln!(
            "ksymbols: loading embedded symbols from 0x{:x}-0x{:x} ({} bytes)",
            sym_start as u64,
            sym_end as u64,
            sym_size as u64,
        );

        let max_entries =
            (idx_end as usize).wrapping_sub(idx_start as usize) / core::mem::size_of::<KSymEntry>();
        let data = core::slice::from_raw_parts(sym_start, sym_size);

        let mut current_file: &[u8] = &[];
        let mut current_symbol: &[u8] = &[];
        let mut line_start = 0usize;
        let mut i = 0usize;
        while i < data.len() {
            let c = data[i];
            if c == b'\n' || c == 0 {
                let line = &data[line_start..i];
                if line.is_empty() {
                    // Empty line, skip.
                } else if line[line.len() - 1] == b':' && line[0] != b':' {
                    // File header: "filename:".
                    current_file = &line[..line.len() - 1];
                } else if line[0] == b':' {
                    // Symbol header: ":symbol".
                    current_symbol = &line[1..];
                } else {
                    // Address line: "start_addr line_number".
                    let (start_addr, consumed) = Backtrace::strtoul(line, 16);
                    if consumed < line.len() && line[consumed] == b' ' {
                        let (line_num, _) = Backtrace::strtoul(&line[consumed + 1..], 10);
                        if (KSYM_COUNT as usize) < max_entries {
                            let entry = KSYM_ENTRIES_BASE.add(KSYM_COUNT as usize);
                            (*entry).start_addr = start_addr as *mut c_void;
                            (*entry).line = line_num as u32;
                            (*entry).symbol = current_symbol.as_ptr() as *const c_char;
                            (*entry).symbol_len = current_symbol.len() as u16;
                            (*entry).filename = current_file.as_ptr() as *const c_char;
                            (*entry).filename_len = current_file.len() as u16;

                            crate::machine::Riscv::rb_node_init(&raw mut (*entry).rb);
                            crate::rbtree::RbRoot::insert_color(&raw mut KSYM_RB_ROOT, &raw mut (*entry).rb);
                            KSYM_COUNT += 1;
                        }
                    }
                }
                line_start = i + 1;
                if c == 0 {
                    break;
                }
            }
            i += 1;
        }

        crate::kprintln!("Kernel symbols initialized: {} entries", KSYM_COUNT);
    }
    }
}

/// Find the pool entry whose `start_addr` is the greatest one `<= addr`
/// (i.e. "which symbol contains this address"). Guard entries (the C
/// original's `':/'`-terminated end-of-file markers, `line == 0`) are
/// filtered out, matching `bt_search_sym`'s C original.
impl Backtrace {
fn bt_search_sym(addr: u64) -> Option<*mut KSymEntry> {
    // SAFETY: read of `KSYM_COUNT`/`KSYM_RB_ROOT`; both are write-once
    // (during `ksymbols_init`, single boot hart) then read-only (see
    // module doc). Before `ksymbols_init` runs, `KSYM_COUNT == -1`, so the
    // early-return below is taken and `KSYM_RB_ROOT` is never touched.
    let count = unsafe { KSYM_COUNT };
    if count <= 0 {
        return None;
    }

    let dummy = KSymEntry {
        // SAFETY: an all-zero `rb_node` is a valid (if unlinked) node; the
        // dummy is never inserted into any tree, only its address is used
        // as a search key.
        rb: unsafe { core::mem::zeroed() },
        start_addr: addr as *mut c_void,
        line: 0,
        symbol: ptr::null(),
        symbol_len: 0,
        filename: ptr::null(),
        filename_len: 0,
    };

    // SAFETY: copies the current (read-only) root, then swaps in the
    // "round-down" comparator -- mirrors the C `search_root = __ksym_rb_root;
    // search_root.opts = &__ksym_rb_rdown_opts;`.
    let mut search_root = unsafe { KSYM_RB_ROOT };
    search_root.opts = Some(&KSYM_RB_RDOWN_OPS);

    // SAFETY: `search_root` is a valid (possibly-empty) `rb_root`;
    // `&dummy` is a live stack value for the duration of this call.
    let node = unsafe {
        crate::bintree::RbRoot::find_key_rdown(&raw mut search_root, &dummy as *const KSymEntry as u64)
    };
    if node.is_null() {
        return None;
    }

    let entry: *mut KSymEntry =
        crate::mm::cffi::container_of(node, core::mem::offset_of!(KSymEntry, rb));
    // SAFETY: `entry` came from the rb-tree, so it is a live pool entry.
    if unsafe { (*entry).line } == 0 {
        return None; // Guard entry.
    }
    Some(entry)
}
}

/// Copies `entry`'s symbol name into `buf` (NUL-terminated, truncated to
/// fit) and returns an index "for backwards compatibility" (matches the C
/// original's `(int)(sym - __ksymbols)`), or writes an empty string and
/// returns -1 if `addr` resolves to no symbol.
///
/// # Safety
/// `buf` must be valid and writable for `buflen` bytes.
// P3-1D mesh sweep: not declared in `kernel/inc/defs.h` (unlike
// `print_backtrace`/`print_thread_backtrace` next to it) and no live caller
// anywhere in the tree today, in C or Rust (full-tree grep) -- was already
// non-`static` but unreferenced in the original `backtrace.c` too. Demoted;
// `#[allow(dead_code)]` documents the gap rather than deleting still-
// plausible public API, same precedent as `ipi.rs`'s `get_cpu_active_mask`.
impl Backtrace {
#[allow(dead_code)]
pub(crate) unsafe extern "C" fn bt_search(
    addr: u64,
    buf: *mut c_char,
    buflen: usize,
    return_addr: *mut *mut c_void,
) -> c_int {
    // SAFETY: caller contract (see `# Safety` above).
    let out = unsafe { core::slice::from_raw_parts_mut(buf as *mut u8, buflen) };
    let Some(entry) = Backtrace::bt_search_sym(addr) else {
        if !out.is_empty() {
            out[0] = 0;
        }
        return -1;
    };
    Backtrace::fill_symbol(entry, out);
    if !return_addr.is_null() {
        // SAFETY: caller contract; `entry` is a live pool entry.
        unsafe {
            *return_addr = (*entry).start_addr;
        }
    }
    // SAFETY: `entry` was derived from `KSYM_ENTRIES_BASE` via `.add()` in
    // `ksymbols_init`, so both share the same base address for this plain
    // pointer-difference arithmetic.
    let base = unsafe { KSYM_ENTRIES_BASE };
    ((entry as usize - base as usize) / core::mem::size_of::<KSymEntry>()) as c_int
}
}

/// Writes `entry`'s symbol name into `out` (NUL-terminated, truncated to
/// fit `out.len()`), or just NUL-terminates `out` if `entry` has no
/// (or an empty) symbol name. Shared by `print_backtrace` and
/// `print_thread_backtrace`'s three symbol-printing sites (the C original
/// inlined this at each site; factored out here to avoid a second
/// redundant rb-tree lookup per frame -- see `print_backtrace`'s doc).
impl Backtrace {
fn fill_symbol(entry: *mut KSymEntry, out: &mut [u8]) {
    // SAFETY: `entry` is a live pool entry (from `bt_search_sym`).
    let (sym, len) = unsafe { ((*entry).symbol, (*entry).symbol_len as usize) };
    if !sym.is_null() && len > 0 {
        let copy_len = len.min(out.len().saturating_sub(1));
        // SAFETY: `sym`/`len` point into the embedded, kernel-lifetime
        // `.ksymbols` blob.
        let src = unsafe { core::slice::from_raw_parts(sym as *const u8, copy_len) };
        out[..copy_len].copy_from_slice(src);
        out[copy_len] = 0;
    } else if !out.is_empty() {
        out[0] = 0;
    }
}

/// Writes `entry`'s file name into `out` (same truncation rule as
/// `fill_symbol`) and returns its line number, or empties `out` and
/// returns 0 if `entry` is `None`.
fn bt_get_location(entry: Option<*mut KSymEntry>, out: &mut [u8]) -> u32 {
    let Some(entry) = entry else {
        if !out.is_empty() {
            out[0] = 0;
        }
        return 0;
    };
    // SAFETY: `entry` is a live pool entry.
    let (fname, flen, line) =
        unsafe { ((*entry).filename, (*entry).filename_len as usize, (*entry).line) };
    if !fname.is_null() && flen > 0 {
        let copy_len = flen.min(out.len().saturating_sub(1));
        // SAFETY: see `fill_symbol`.
        let src = unsafe { core::slice::from_raw_parts(fname as *const u8, copy_len) };
        out[..copy_len].copy_from_slice(src);
        out[copy_len] = 0;
    } else if !out.is_empty() {
        out[0] = 0;
    }
    line
}

/// Offset of `addr` from `entry`'s `start_addr`, or 0 if `entry` is
/// `None`.
fn bt_get_offset(entry: Option<*mut KSymEntry>, addr: u64) -> i32 {
    match entry {
        None => 0,
        // SAFETY: `entry` is a live pool entry.
        Some(e) => (addr as i64).wrapping_sub(unsafe { (*e).start_addr } as i64) as i32,
    }
}

// ===========================================================================
// Frame-pointer walking. Mirrors the C `BT_FRAME_TOP`/`BT_RETURN_ADDRESS`/
// `BT_IS_TOP_FRAME` macros exactly, including their trust model: the very
// first frame (`context`/`ctx->s0`) is dereferenced *before* any bounds
// check (the caller -- `tf->s0`, `r_fp()`, or a saved `context.s0` -- is
// always a currently-live frame pointer); every subsequent frame is
// bounds-checked against `[stack_start, stack_end)` by the loop body
// before it is ever dereferenced.
// ===========================================================================

#[inline(always)]
fn frame_top(fp: u64) -> u64 {
    if fp == 0 {
        0
    } else {
        // SAFETY: see module-level note above this section; callers only
        // invoke this on a frame pointer already known to be live (either
        // the caller-supplied entry frame, or one just bounds-checked
        // against the live kernel-stack window).
        unsafe { *((fp - 16) as *const u64) }
    }
}

#[inline(always)]
fn return_address(fp: u64) -> u64 {
    if fp == 0 {
        0
    } else {
        // SAFETY: see `frame_top`.
        unsafe { *((fp - 8) as *const u64) }
    }
}

#[inline(always)]
fn is_top_frame(fp: u64) -> bool {
    fp == 0 || fp == (fp & !(PGSIZE - 1))
}
}

/// Print a backtrace starting at frame pointer `context`, walking while
/// each frame stays within `[stack_start, stack_end)`. Rust port of
/// `print_backtrace()`. No allocation, no locks -- safe to call from the
/// panic path or any interrupt context (see module doc).
///
/// # Safety
/// `context` must be 0 or a currently-live frame pointer (e.g. `tf->s0`,
/// `r_fp()`, or a saved `context.s0`); `[stack_start, stack_end)` must
/// bound the stack that frame pointer chains through.
// P3-1D mesh sweep: every caller (`printf.rs`, `ipi.rs`, `irq/trap.rs`) now
// imports this via crate-path `use` instead of an `extern` redeclaration --
// demoted.
impl Backtrace {
pub(crate) unsafe extern "C" fn print_backtrace(context: u64, stack_start: u64, stack_end: u64) {
    // SAFETY: see function-level `# Safety` and the frame-walking section
    // doc above.
    unsafe {
        crate::kprintln!("backtrace:");

        let mut last_fp = context;
        let mut fp = Backtrace::frame_top(context);
        let mut depth = 0u32;
        while !Backtrace::is_top_frame(fp) && depth < BACKTRACE_MAX_DEPTH {
            if fp < stack_start || fp >= stack_end {
                crate::kprintln!("  * unknown frame: {}", crate::printf::Ptr(fp));
                break;
            }

            let mut symbuf = [0u8; 64];
            let mut filebuf = [0u8; 128];
            let return_addr_val = Backtrace::return_address(last_fp);
            if return_addr_val == 0 {
                crate::kprintln!("  top frame");
                break;
            }

            let entry = Backtrace::bt_search_sym(return_addr_val);
            match entry {
                None => {
                    crate::kprintln!(
                        "  * {}: unknown",
                        crate::printf::Ptr(return_addr_val),
                    );
                }
                Some(e) => {
                    Backtrace::fill_symbol(e, &mut symbuf);
                    let line = Backtrace::bt_get_location(entry, &mut filebuf);
                    let offset = Backtrace::bt_get_offset(entry, return_addr_val);
                    crate::kprintln!(
                        "  * {}:{}: {}+{}",
                        crate::printf::Cs(filebuf.as_ptr() as *const c_char),
                        line,
                        crate::printf::Cs(symbuf.as_ptr() as *const c_char),
                        offset,
                    );
                }
            }

            last_fp = fp;
            fp = Backtrace::frame_top(fp);
            depth += 1;
        }
    }
}

/// Backtrace a sleeping/blocked thread from its saved `context` (`s0` is
/// the RISC-V frame pointer). Rust port of `print_thread_backtrace()`.
///
/// # Safety
/// The thread must not be running on any CPU; `ctx` must be null or point
/// at a live `struct context`; `kstack`/`kstack_order` must describe that
/// thread's kernel stack (`size = 1 << (PAGE_SHIFT + kstack_order)`).
// P3-1D mesh sweep: only caller is `proc/proc_shims.rs` (crate-path `use`,
// not an `extern` redeclaration) -- demoted.
pub(crate) unsafe extern "C" fn print_thread_backtrace(ctx: *mut context, kstack: u64, kstack_order: c_int) {
    // SAFETY: see function-level `# Safety` and the frame-walking section
    // doc above; `ctx`/`kstack` are checked before any dereference,
    // matching the C original.
    unsafe {
        if ctx.is_null() || kstack == 0 {
            crate::kprintln!("backtrace: invalid context or stack");
            return;
        }

        let fp0 = (*ctx).s0;
        let stack_size = 1u64 << ((PAGE_SHIFT as i32 + kstack_order) as u32);
        let stack_start = kstack;
        let stack_end = kstack + stack_size;

        crate::kprintln!("backtrace:");

        let mut symbuf = [0u8; 64];
        let mut filebuf = [0u8; 128];

        // Print the resume point (ctx->ra) first.
        let entry0 = Backtrace::bt_search_sym((*ctx).ra);
        match entry0 {
            None => {
                crate::kprintln!(
                    "  > {}: unknown (resume point)",
                    crate::printf::Ptr((*ctx).ra),
                );
            }
            Some(e) => {
                Backtrace::fill_symbol(e, &mut symbuf);
                let line = Backtrace::bt_get_location(entry0, &mut filebuf);
                let offset = Backtrace::bt_get_offset(entry0, (*ctx).ra);
                crate::kprintln!(
                    "  > {}:{}: {}+{} (resume point) [{}]",
                    crate::printf::Cs(filebuf.as_ptr() as *const c_char),
                    line,
                    crate::printf::Cs(symbuf.as_ptr() as *const c_char),
                    offset,
                    crate::printf::Ptr((*ctx).ra),
                );
            }
        }

        let mut last_fp = fp0;
        let mut last_return_addr = (*ctx).ra;
        let mut repeat_count = 0u32;
        const MAX_REPEATS: u32 = 3;

        let mut curr_fp = Backtrace::frame_top(fp0);
        let mut depth = 0u32;
        while !Backtrace::is_top_frame(curr_fp) && depth < BACKTRACE_MAX_DEPTH {
            if curr_fp < stack_start || curr_fp >= stack_end {
                crate::kprintln!("  * frame outside stack: {}", crate::printf::Ptr(curr_fp));
                break;
            }

            let return_addr_val = Backtrace::return_address(last_fp);
            if return_addr_val == 0 {
                break;
            }

            if return_addr_val == last_return_addr {
                repeat_count += 1;
                if repeat_count >= MAX_REPEATS {
                    crate::kprintln!("  * ... ({} more repeated frames)", depth as u64);
                    break;
                }
            } else {
                repeat_count = 0;
                last_return_addr = return_addr_val;
            }

            let entry = Backtrace::bt_search_sym(return_addr_val);
            match entry {
                None => {
                    crate::kprintln!(
                        "  * {}: unknown",
                        crate::printf::Ptr(return_addr_val),
                    );
                }
                Some(e) => {
                    Backtrace::fill_symbol(e, &mut symbuf);
                    let line = Backtrace::bt_get_location(entry, &mut filebuf);
                    let offset = Backtrace::bt_get_offset(entry, return_addr_val);
                    crate::kprintln!(
                        "  * {}:{}: {}+{} [{}]",
                        crate::printf::Cs(filebuf.as_ptr() as *const c_char),
                        line,
                        crate::printf::Cs(symbuf.as_ptr() as *const c_char),
                        offset,
                        crate::printf::Ptr(return_addr_val),
                    );
                }
            }

            last_fp = curr_fp;
            curr_fp = Backtrace::frame_top(curr_fp);
            depth += 1;
        }
    }
}
}

/// Set breakpoint for GDB (`b db_break`). Deliberately empty -- a stable
/// symbol name to break on, matching the C original.
// P3-1D: kept `#[no_mangle]` deliberately -- this symbol has no Rust or C
// caller by design (an external debugger sets a breakpoint on it by linked
// name, e.g. `b db_break` in a `.gdbinit`), the same "genuinely external
// consumer" class as the linker-script/`.S`-defined symbols, not a mesh
// leftover.
// P3-D3c (final sweep) re-verified: still 0 in-tree references by design;
// stays in the mandated keep set as the one debugger-by-name anchor.
#[no_mangle]
pub extern "C" fn db_break() {}
