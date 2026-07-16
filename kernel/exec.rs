//! Program execution — Rust port of `kernel/exec.c` (Phase 2 Wave 26, see
//! `docs/rustify/phase2_plan.md`). This is the last file before
//! `start_kernel.c` (Wave 27) completes the `core/misc` area.
//!
//! [`exec`] loads and runs an ELF binary using file-backed `mmap` for lazy
//! demand paging (LOAD segments are faulted in from the page cache on
//! first access rather than eagerly read at exec time — see the loop in
//! `exec` for the three-region split: file-backed mmap / eager boundary
//! page / anonymous BSS mmap, matching Linux's `load_elf_binary()`
//! `elf_map()`/`padzero()`/`set_brk()` split). [`sys_exec`] is the
//! syscall entry point: it copies `path`/`argv[]`/`envp[]` out of user
//! memory into kernel buffers, then calls `exec`. Every caller of either
//! symbol is same-crate Rust now that the whole VFS tree (Waves 13-20)
//! and `mm/vm.rs` are ported:
//!
//! * `exec` — called by [`crate::proc::thread`]'s `init_entry` to launch
//!   `/bin/init` at boot (boot-critical: a regression here bricks every
//!   process spawn, not just `exec(2)`).
//! * `sys_exec` — called by [`crate::irq::syscall`]'s dispatch table.
//!
//! # Calling convention
//!
//! Matches the established per-file convention (`vfs/vfs_syscall.rs`,
//! `vfs/file.rs`, ...): every cross-module C-ABI symbol this file calls
//! is declared in the local `unsafe extern "C"` block below rather than
//! reached through a `crate::mm::vm`/`crate::vfs::...`/`crate::proc::...`
//! module path — those modules are private (`mod mm;`/`mod vfs;`/
//! `mod proc;` at the crate root), so privacy forces the same
//! cross-translation-unit-style call convention every other file in this
//! tree already uses, even though the callees are themselves Rust.
//! `kernel/proc/thread.rs` and `kernel/irq/syscall.rs` already declare
//! their own `extern "C" fn exec(...)`/`fn sys_exec() -> u64;` for the
//! two symbols this file exports — the exact names and signatures below
//! are load-bearing.
//!
//! # Stack ABI fidelity
//!
//! The startup stack frame `exec` builds is userspace ABI (newlib/crt0
//! reads it via `sp`): `sp[0] = argc`, `sp[1..=argc] = argv[]` pointers,
//! `sp[argc+1] = NULL`, `sp[argc+2..argc+1+envc] = envp[]` pointers,
//! `sp[argc+2+envc] = NULL`. The push-strings-then-reverse-then-frame
//! algorithm (`ustack[]`, a fixed `MAXARG + MAXENV + 4`-slot buffer) is
//! ported control-flow-for-control-flow against the C, including the
//! exact per-string `sp -= len + 1; sp -= sp % 16;` 16-byte-alignment
//! step (RISC-V calling convention) and the `stackbase` underflow guard
//! after every push. The `argc`/`envc` bounds (`>= MAXARG`/`>= MAXENV`
//! aborts the whole `exec`) are checked in the same order relative to
//! each loop's other work as the C (see the loop bodies below), so the
//! observable failure point for an oversized argument list is unchanged.
//!
//! # The `copyinstr2` "argument-size validation gap" — investigated, not
//! an exec bug
//!
//! This wave's mandate flagged `usertests -q`'s `copyinstr2` failure
//! (`"exec(echo, BIG) succeeded, should have failed"`) as a suspected
//! exec argv-size validation gap and asked for a fix. Empirical
//! investigation (temporary debug instrumentation in the pre-port C
//! `exec.c`, built and run under the standard boot gate, then reverted
//! before this file was written — see the wave report) found the
//! opposite: **the check already exists and already fires correctly**.
//! `copyinstr2`'s embedded big-argument sub-test execs `/bin/echo` with
//! three arguments each exactly `PGSIZE` (4096) bytes long (no room for
//! the NUL terminator within a `PGSIZE`-sized kernel buffer) — this is
//! the *exact* `fetchstr(uarg, argv[i], PGSIZE)` bound classic xv6-riscv
//! itself uses (this file's `sys_exec` is byte-for-byte the same shape).
//! Debug instrumentation confirmed `fetchstr` returns `-ENAMETOOLONG` on
//! the very first oversized argument and `sys_exec` takes the `goto bad`
//! path, returning `-1` to userspace — exactly as the test expects.
//!
//! The actual, *different* root cause: the test's forked child, having
//! correctly observed `exec()` fail, calls `exit(747)` as its "the check
//! worked" sentinel. `kernel/proc/proc_shims.rs`'s `xv6_exit_reap_zombie`
//! encodes the parent's observed `wait()` status POSIX-style —
//! `*xstate_out = ((*child).xstate & 0xff) << 8;` — instead of classic
//! xv6's raw pass-through. Because `747 > 255`, `747 & 0xff` can **never**
//! equal `747` for *any* possible encoding shift, so the parent's
//! `if (st != 747)` check is mathematically guaranteed to fire regardless
//! of whether `exec` behaved correctly. This is the same code path (same
//! function, same encoding) already tracked in `RUST_REWRITE.md`'s Known
//! issues as the `usertests killstatus` failure (`wait` status of a
//! SIGKILLed process not reporting `-1`) — `copyinstr2`'s failure is a
//! second, independent symptom of that one pre-existing proc-side bug,
//! not a new exec-side one. Per this wave's touch-scope (exec files
//! only), the fix belongs in a future proc-focused wave; documented here
//! and in `RUST_REWRITE.md` rather than fixed out-of-scope.
//!
//! # Dead code dropped
//!
//! `ustack_alloc` (allocated a grows-down stack VMA via `vma_alloc`) had
//! zero callers anywhere in the C tree (not even declared in
//! `kernel/inc/defs.h`) — confirmed by a whole-tree grep before deletion;
//! `exec` builds the stack via `vm_mmap_region_locked` instead (a design
//! that predates this port). `flags2vmperm` has exactly one caller (this
//! file's own `exec`) and is not declared in `defs.h` either, so it stays
//! a private Rust helper (no `#[no_mangle]`, no C-ABI surface) rather
//! than a redundant `extern "C" fn`.

#![allow(non_camel_case_types, non_snake_case)]

use core::ffi::{c_char, c_int, c_void};
use core::mem::{size_of, MaybeUninit};
use core::ptr;

use crate::bindings::{
    elfhdr, loff_t, proghdr, sigacts_t, thread, vfs_fdtable, vfs_file, vfs_inode, vm, vma,
    PGSIZE, PROT_EXEC, PROT_READ, PROT_WRITE, USTACKTOP, VMA_FLAG_FILE, VMA_FLAG_GROWSDOWN,
    VMA_FLAG_GROWSUP, VMA_FLAG_USER,
};
// P3-1B mesh sweep: both are same-crate `pub(crate)` items as of this wave
// (only caller anywhere in the tree is this file), referenced directly
// instead of via their own `extern "C"` redeclarations.
use crate::proc::proc_shims::xv6_current_thread;
use crate::proc::sigacts_exec;
use crate::proc::vfork_done as raw_vfork_done;
// P3-1C mesh sweep: vfs/{inode,file,fdtable}.rs are in scope for this wave,
// so their callees are referenced as plain crate-path items instead of
// `extern "C"` redeclarations -- their definitions dropped `#[no_mangle]`
// accordingly. Signatures verified identical (same `crate::bindings::*`
// types this file already imports above), so this is a pure mesh
// simplification, not an ABI change.
use crate::vfs::fdtable::vfs_fdtable_close_on_exec;
use crate::vfs::file::{vfs_fileopen, vfs_filelseek, vfs_fileread, vfs_fput};
use crate::vfs::inode::{vfs_iput, vfs_namei};

/// `proc/exit.rs::vfork_done` takes its own local opaque `Thread` marker
/// type (`proc::exit::Thread`), not `crate::bindings::thread` -- both are
/// `#[repr(C)]` zero-sized marker structs standing in for the same
/// underlying object, so reinterpreting the pointer is sound (same
/// precedent as `sysproc.rs`'s `thread_clone` wrapper). `Thread` itself
/// isn't nameable from here (module-private to `proc`, and ambiguous
/// through `proc`'s glob re-export besides -- `pgroup.rs`/`clone.rs` etc.
/// each define their own same-named opaque marker); `as _` lets the
/// compiler infer it from `raw_vfork_done`'s own signature instead.
#[inline(always)]
fn vfork_done(p: *mut thread) {
    raw_vfork_done(p as *mut c_void as *mut _);
}

// ===========================================================================
// Externs — every cross-module C-ABI symbol this file calls, declared
// locally per this crate's established convention (see the module doc).
// Nearly everything is `safe`: passing a raw pointer is never itself
// unsafe, and every entry point here either null-checks internally or is
// called only with pointers this file already proved non-null (same
// rationale as `vfs/file.rs`'s externs-block doc).
// ===========================================================================

unsafe extern "C" {
    // string.rs.
    safe fn strlen(s: *const c_char) -> usize;
    safe fn memset(dst: *mut c_void, c: c_int, n: usize) -> *mut c_void;
    safe fn safestrcpy(s: *mut c_char, t: *const c_char, n: usize) -> *mut c_char;

    // irq/syscall.rs — arg-fetch helpers (`sys_exec` only).
    safe fn argaddr(n: c_int, ip: *mut u64);
    safe fn argstr(n: c_int, buf: *mut c_char, max: c_int) -> c_int;
    safe fn fetchaddr(addr: u64, ip: *mut u64) -> c_int;
    safe fn fetchstr(addr: u64, buf: *mut c_char, max: c_int) -> c_int;
}

// P3-D3a: the mm/vm.rs entry points are ordinary (safe) Rust fns now that
// their `#[no_mangle]` exports are gone; reached as crate-path items
// instead of the `extern "C"` redeclarations that used to sit in the
// block above. Signatures are identical to the old redeclarations.
use crate::mm::{
    vm_copyout, vm_find_area, vm_init, vm_mmap_region_locked, vm_put, vm_rlock, vm_runlock,
    vm_wlock, vm_wunlock, vma_validate,
};

// `kalloc`/`kfree` are genuinely `unsafe fn` in `crate::mm::kalloc`; this
// file's original extern declarations asserted `safe fn` (the usual
// FFI-boilerplate-collapsing facade). Thin safe wrappers preserve that
// facade, per the `cffi::raw`/`bufcache.rs` precedent.
/// SAFETY: see [`crate::mm::kalloc::kalloc`]'s contract.
#[inline]
fn kalloc() -> *mut c_void {
    unsafe { crate::mm::kalloc() }
}
/// SAFETY: `pa` must originate from `kalloc` above.
#[inline]
fn kfree(pa: *mut c_void) {
    unsafe { crate::mm::kfree(pa) };
}

// ===========================================================================
// Small helpers: hand-copied small-integer macros, ERR_PTR family,
// page-round arithmetic. Reimplemented locally per this crate's
// established per-file convention (see `vfs/inode.rs`'s externs-block
// doc for the rationale).
// ===========================================================================

// `kernel/inc/param.h`.
const MAXARG: usize = 32;
const MAXENV: usize = 64;
const MAXPATH: usize = 128;
/// `USERSTACK` (`param.h`) — user stack size in pages.
const USERSTACK: u64 = 32;

// `kernel/inc/uabi/fcntl.h`.
const O_RDONLY: c_int = 0o0;
const SEEK_SET: c_int = 0;

// `kernel/inc/elf.h` — hand-copied plain `#define`s (no bindgen var; the
// structs `elfhdr`/`proghdr` are real bindgen layouts, see wrapper.h).
const ELF_MAGIC: u32 = 0x464C457F;
const ELF_PROG_LOAD: u32 = 1;
const ELF_PROG_FLAG_EXEC: u32 = 1;
const ELF_PROG_FLAG_WRITE: u32 = 2;
const ELF_PROG_FLAG_READ: u32 = 4;

// `is_err`/`is_err_or_null`'s canonical home is `crate::kstd` (P3-CS2
// centralization).
use crate::kstd::{is_err, is_err_or_null};

/// Mirrors `riscv.h`'s `PGROUNDUP`.
#[inline(always)]
const fn pg_round_up(a: u64) -> u64 {
    (a + PGSIZE as u64 - 1) & !(PGSIZE as u64 - 1)
}
/// Mirrors `riscv.h`'s `PGROUNDDOWN`.
#[inline(always)]
const fn pg_round_down(a: u64) -> u64 {
    a & !(PGSIZE as u64 - 1)
}

/// Mirrors the C `flags2vmperm()`: translate an ELF program-header
/// `p_flags` bitmask (`ELF_PROG_FLAG_{EXEC,WRITE,READ}`) into this
/// kernel's `PROT_*` VMA permission bits. Not a `#[no_mangle]` C-ABI
/// function: it has exactly one caller (`exec`, below) and is not
/// declared in `kernel/inc/defs.h` (see the module doc's "dead code
/// dropped" note).
const fn flags2vmperm(flags: u32) -> u64 {
    let mut perm: u64 = 0;
    if flags & ELF_PROG_FLAG_EXEC != 0 {
        perm = PROT_EXEC as u64;
    }
    if flags & ELF_PROG_FLAG_WRITE != 0 {
        perm |= PROT_WRITE as u64;
    }
    if flags & ELF_PROG_FLAG_READ != 0 {
        perm |= PROT_READ as u64;
    }
    perm
}

// ===========================================================================
// exec — ELF loader + argv/envp stack setup.
// ===========================================================================

/// Load and execute an ELF binary, replacing the calling thread's address
/// space. `path`/`argv`/`envp` are kernel-resident (either `sys_exec`'s
/// local fixed-size buffers, copied in from userspace, or
/// `proc/thread.rs::init_entry`'s static `/bin/init` argv at boot).
///
/// Returns `argc` (>= 0) on success — this ends up in `a0`, the syscall
/// return value, which is also `main`'s first argument per the RISC-V
/// calling convention. Returns `-1` on any failure, leaving the caller's
/// existing address space untouched (the new `vm_t` is built up
/// out-of-place in `tmp_vm` and only installed into `p->vm` once every
/// fallible step has succeeded).
// P3-1B: only caller anywhere in the tree is `proc/thread.rs::init_entry`
// (crate-path `use`, not an `extern` redeclaration) -- demoted.
pub(crate) extern "C" fn exec(path: *mut c_char, argv: *mut *mut c_char, envp: *mut *mut c_char) -> c_int {
    let stackbase = USTACKTOP - USERSTACK * PGSIZE as u64;
    let mut tmp_vm: *mut vm = ptr::null_mut();
    let mut file: *mut vfs_file = ptr::null_mut();
    let mut heap_start: u64 = 0;

    // Goto-cleanup emulation (see `vfs/fs.rs`'s Wave-16 precedent for
    // labeled-block goto emulation; a pair of local macros reads more
    // plainly here since there are exactly two cleanup shapes and many
    // call sites). `bad_locked!()` mirrors the C `bad_locked:` label
    // (drops the write lock, then falls through to `bad:`); `bad!()`
    // mirrors `bad:` (drops the VM reference and closes `file` if still
    // open). Both reference this function's `tmp_vm`/`file` locals by
    // name, resolved at the macro's expansion site (ordinary Rust
    // `macro_rules!` name resolution, not a hygiene violation).
    macro_rules! bad {
        () => {{
            vm_put(tmp_vm);
            if !file.is_null() {
                vfs_fput(file);
            }
            return -1;
        }};
    }
    macro_rules! bad_locked {
        () => {{
            vm_wunlock(tmp_vm);
            bad!();
        }};
    }

    // Look up the file using VFS.
    // SAFETY: `path` is a caller-owned, NUL-terminated C string (contract
    // shared with the C original's `strlen(path)`).
    let path_len = unsafe { strlen(path) };
    let inode = vfs_namei(path as *const c_char, path_len);
    if is_err_or_null(inode) {
        return -1;
    }

    // Open the file for reading.
    file = vfs_fileopen(inode, O_RDONLY);
    vfs_iput(inode); // vfs_fileopen takes its own reference.

    if is_err(file) {
        return -1;
    }
    if file.is_null() {
        return -1;
    }

    // Read ELF header.
    let mut elf_mu = MaybeUninit::<elfhdr>::uninit();
    let n = vfs_fileread(file, elf_mu.as_mut_ptr() as *mut c_void, size_of::<elfhdr>(), 0);
    if n as usize != size_of::<elfhdr>() {
        bad!();
    }
    // SAFETY: `vfs_fileread` returned exactly `size_of::<elfhdr>()`,
    // proving the whole buffer was written before this read.
    let elf = unsafe { elf_mu.assume_init() };

    if elf.magic != ELF_MAGIC {
        bad!();
    }

    tmp_vm = vm_init();
    if tmp_vm.is_null() {
        bad!();
    }

    // Because by this time no one else can see tmp_vm, we don't need to
    // worry about lock contention. But we still need to hold the write
    // lock to suppress assertions in vma_alloc() called by
    // vm_mmap_region_locked().
    vm_wlock(tmp_vm);

    // Load program into memory using mmap for lazy demand paging.
    //
    // For each LOAD segment we create up to three regions:
    //   1. File-backed mmap  [vaddr, file_pg_end)        - demand-paged
    //      from pcache
    //   2. Boundary page     [file_pg_end, file_pg_end+PGSIZE) - eagerly
    //      populated
    //   3. Anonymous mmap    [anon_start, total_end)     - lazy zero-fill
    //      (BSS)
    let mut off: i64 = elf.phoff as i64;
    for _ in 0..elf.phnum {
        // Seek to program header.
        if vfs_filelseek(file, off, SEEK_SET) != off {
            bad_locked!();
        }

        // Read program header.
        let mut ph_mu = MaybeUninit::<proghdr>::uninit();
        if vfs_fileread(file, ph_mu.as_mut_ptr() as *mut c_void, size_of::<proghdr>(), 0)
            != size_of::<proghdr>() as isize
        {
            bad_locked!();
        }
        // SAFETY: `vfs_fileread` returned exactly `size_of::<proghdr>()`.
        let ph = unsafe { ph_mu.assume_init() };

        off += size_of::<proghdr>() as i64;

        if ph.type_ != ELF_PROG_LOAD {
            continue;
        }
        if ph.memsz < ph.filesz {
            bad_locked!();
        }
        // Overflow check (unsigned wraparound test, matching the C's
        // `ph.vaddr + ph.memsz < ph.vaddr` exactly): `wrapping_add` makes
        // the intentional-wraparound comparison explicit.
        if ph.vaddr.wrapping_add(ph.memsz) < ph.vaddr {
            bad_locked!();
        }
        // This kernel assumes page-aligned LOAD segments. Non-page-aligned
        // ELF segments are not supported.
        if (ph.vaddr & (PGSIZE as u64 - 1)) != 0 || (ph.off & (PGSIZE as u64 - 1)) != 0 {
            bad_locked!();
        }

        let va = ph.vaddr;
        let filesz = ph.filesz;
        let memsz = ph.memsz;
        let file_off = ph.off;
        let vm_flags = flags2vmperm(ph.flags) | VMA_FLAG_USER as u64;
        let total_end = pg_round_up(va + memsz);
        // End of full pages entirely covered by file data.
        let file_pg_end = if filesz > 0 { pg_round_down(va + filesz) } else { va };

        // Region 1: file-backed mmap - pages are demand-paged from the
        // file's page cache on the first instruction/load/store fault.
        if file_pg_end > va {
            let ret = vm_mmap_region_locked(
                tmp_vm,
                va,
                (file_pg_end - va) as usize,
                vm_flags | VMA_FLAG_FILE as u64,
                file,
                file_off,
                ptr::null_mut(),
            );
            if ret != 0 {
                bad_locked!();
            }
        }

        // Has a partial (boundary) page at the end of the file data?
        let has_boundary = filesz > 0 && (filesz & (PGSIZE as u64 - 1)) != 0;
        let anon_start = if has_boundary { file_pg_end + PGSIZE as u64 } else { file_pg_end };

        // Region 2: boundary page - the single page that straddles the
        // file-data / BSS boundary. We allocate it eagerly, copy the
        // partial file data, and zero-fill the rest (same as Linux
        // padzero).
        if has_boundary {
            let nbytes = ((va + filesz) - file_pg_end) as u32;
            let pa = kalloc();
            if pa.is_null() {
                bad_locked!();
            }
            memset(pa, 0, PGSIZE as usize);

            let foff = (file_off + (file_pg_end - va)) as loff_t;
            if vfs_filelseek(file, foff, SEEK_SET) != foff {
                kfree(pa);
                bad_locked!();
            }
            if vfs_fileread(file, pa, nbytes as usize, 0) != nbytes as isize {
                kfree(pa);
                bad_locked!();
            }

            let ret = vm_mmap_region_locked(tmp_vm, file_pg_end, PGSIZE as usize, vm_flags, ptr::null_mut(), 0, pa);
            if ret != 0 {
                kfree(pa);
                bad_locked!();
            }
        }

        // Region 3: anonymous mmap for the remaining BSS pages. Faulted
        // in lazily as zero-filled pages.
        if total_end > anon_start {
            let ret = vm_mmap_region_locked(
                tmp_vm,
                anon_start,
                (total_end - anon_start) as usize,
                vm_flags,
                ptr::null_mut(),
                0,
                ptr::null_mut(),
            );
            if ret != 0 {
                bad_locked!();
            }
        }

        // Track the end of loaded segments for heap start.
        let size1 = ph.vaddr + ph.memsz;
        if heap_start < size1 {
            heap_start = size1;
        }
    }

    // Done with the file.
    vfs_fput(file);
    file = ptr::null_mut();

    let p = xv6_current_thread();

    // Create heap via mmap (grows-up, demand-paged zero-fill).
    let heap_sz = pg_round_up(USERSTACK * PGSIZE as u64);
    heap_start = pg_round_up(heap_start);
    if vm_mmap_region_locked(
        tmp_vm,
        heap_start,
        heap_sz as usize,
        (PROT_READ | PROT_WRITE) as u64 | VMA_FLAG_USER as u64 | VMA_FLAG_GROWSUP as u64,
        ptr::null_mut(),
        0,
        ptr::null_mut(),
    ) != 0
    {
        bad_locked!();
    }
    // SAFETY: `tmp_vm` is exclusively owned by this function (not yet
    // published to any other thread/CPU — see the comment above
    // `vm_wlock` above); `heap`/`heap_size` are plain embedded fields.
    unsafe {
        (*tmp_vm).heap = vm_find_area(tmp_vm, heap_start);
        (*tmp_vm).heap_size = heap_sz as usize;
    }

    // Create user stack via mmap (grows-down, demand-paged zero-fill).
    let stack_sz = pg_round_up(USERSTACK * PGSIZE as u64);
    if vm_mmap_region_locked(
        tmp_vm,
        USTACKTOP - stack_sz,
        stack_sz as usize,
        (PROT_READ | PROT_WRITE) as u64 | VMA_FLAG_USER as u64 | VMA_FLAG_GROWSDOWN as u64,
        ptr::null_mut(),
        0,
        ptr::null_mut(),
    ) != 0
    {
        bad_locked!();
    }
    // SAFETY: see the heap field-write above.
    unsafe {
        (*tmp_vm).stack = vm_find_area(tmp_vm, USTACKTOP - stack_sz);
        (*tmp_vm).stack_size = stack_sz as usize;
    }

    vm_wunlock(tmp_vm);
    let mut sp: u64 = USTACKTOP;

    // Preload pages near the entry point to avoid initial page faults.
    // This is similar to Linux's fault-ahead in load_elf_binary().
    {
        const EXEC_PRELOAD_PAGES: u64 = 4;
        let entry_start = pg_round_down(elf.entry);
        let preload_size = EXEC_PRELOAD_PAGES * PGSIZE as u64;

        vm_rlock(tmp_vm);
        let entry_vma = vm_find_area(tmp_vm, entry_start);
        if !entry_vma.is_null() {
            // Clamp to VMA bounds.
            let mut preload_end = entry_start + preload_size;
            // SAFETY: `entry_vma` is non-null and live: `vm_find_area`
            // just returned it from `tmp_vm`'s own tree under the read
            // lock this block still holds.
            let vma_end = unsafe { (*entry_vma).end };
            if preload_end > vma_end {
                preload_end = vma_end;
            }
            vma_validate(entry_vma, entry_start, preload_end - entry_start, (PROT_READ | VMA_FLAG_USER) as u64);
        }
        vm_runlock(tmp_vm);
    }

    // Push argument strings, prepare rest of stack in ustack.
    let mut ustack = [0u64; MAXARG + MAXENV + 4];
    let mut argc: usize = 0;
    loop {
        // SAFETY: `argv` is a caller-owned, NULL-terminated array of
        // string pointers (contract shared with the C original's
        // `argv[argc]`); indices up to `MAXARG` stay in-bounds of every
        // real caller (`sys_exec`'s fixed `[*mut c_char; MAXARG]` buffer,
        // `proc/thread.rs::init_entry`'s 2-element static array checked
        // against `argc >= MAXARG` below before ever reading past it).
        let a = unsafe { *argv.add(argc) };
        if a.is_null() {
            break;
        }
        if argc >= MAXARG {
            bad!();
        }
        // SAFETY: `a` is a non-null, caller-owned NUL-terminated string.
        let alen = unsafe { strlen(a) } as u64;
        sp -= alen + 1;
        sp -= sp % 16; // riscv sp must be 16-byte aligned
        if sp < stackbase {
            bad!();
        }
        if vm_copyout(tmp_vm, sp, a as *const c_void, alen + 1) < 0 {
            bad!();
        }
        ustack[argc] = sp;
        argc += 1;
    }

    // Push environment strings.
    let mut envc: usize = 0;
    if !envp.is_null() {
        loop {
            // SAFETY: see the `argv` loop above; same contract for `envp`.
            let e = unsafe { *envp.add(envc) };
            if e.is_null() {
                break;
            }
            if envc >= MAXENV {
                bad!();
            }
            // SAFETY: `e` is a non-null, caller-owned NUL-terminated string.
            let elen = unsafe { strlen(e) } as u64;
            sp -= elen + 1;
            sp -= sp % 16;
            if sp < stackbase {
                bad!();
            }
            if vm_copyout(tmp_vm, sp, e as *const c_void, elen + 1) < 0 {
                bad!();
            }
            ustack[argc + 2 + envc] = sp;
            envc += 1;
        }
    }

    // Build startup stack frame expected by newlib/crt0:
    //   sp[0] = argc
    //   sp[1..argc] = argv pointers
    //   sp[argc+1] = NULL        (argv terminator)
    //   sp[argc+2..argc+1+envc] = envp pointers
    //   sp[argc+2+envc] = NULL   (envp terminator)
    for i in (0..argc).rev() {
        ustack[i + 1] = ustack[i];
    }
    ustack[0] = argc as u64;
    ustack[argc + 1] = 0;
    // envp pointers are already at ustack[argc+2 .. argc+1+envc].
    ustack[argc + 2 + envc] = 0;

    let nslots = argc + envc + 3; // argc + argv[] + NULL + envp[] + NULL
    sp -= (nslots * size_of::<u64>()) as u64;
    sp -= sp % 16;
    if sp < stackbase {
        bad!();
    }
    if vm_copyout(tmp_vm, sp, ustack.as_ptr() as *const c_void, (nslots * size_of::<u64>()) as u64) < 0 {
        bad!();
    }

    // arguments to user main(argc, argv)
    // argc is returned via the system call return value, which goes in a0.
    // SAFETY: `p` is the live current thread; `trapframe` is its populated
    // `utrapframe` (set up by `uservec`/`usertrap` before any `sys_*`, or
    // by `userinit`'s trapframe seeding at boot for the `init_entry` caller).
    unsafe {
        (*(*p).trapframe).trapframe.a1 = sp + size_of::<u64>() as u64;
    }

    // Save program name for debugging.
    let mut last = path;
    let mut s = path;
    // SAFETY: `path` is a caller-owned, NUL-terminated C string (walked
    // read-only, same contract as the C original's `for (last=s=path;
    // *s; s++)`).
    unsafe {
        while *s != 0 {
            if *s == b'/' as c_char {
                last = s.add(1);
            }
            s = s.add(1);
        }
    }
    // SAFETY: `p` is the live current thread; `name` is a plain embedded
    // `[c_char; 16]` array field (its length taken from the real bindgen
    // layout via `.len()`, so it tracks `proc/thread_types.h`
    // automatically instead of hand-copying the 16).
    unsafe {
        let name_len = (*p).name.len();
        safestrcpy(ptr::addr_of_mut!((*p).name) as *mut c_char, last, name_len);
    }

    // Commit to the user image.
    // SAFETY: `p` is the live current thread, exclusively executing this
    // exec() call (no other thread mutates `p->vm` concurrently); `tmp_vm`
    // has passed every fallible step above and is about to become the
    // sole owner of the new address space.
    unsafe {
        vm_put((*p).vm); // Destroy the old VM.
        (*p).vm = ptr::null_mut();
        (*p).vm = tmp_vm; // Set the new VM.
        (*(*p).trapframe).trapframe.sepc = elf.entry; // initial pc = _start
        (*(*p).trapframe).trapframe.sp = sp; // initial stack pointer
    }

    // Reset caught signal handlers to SIG_DFL (POSIX: the old handler
    // addresses are meaningless in the new address space).
    // SAFETY: `p` is the live current thread.
    unsafe {
        sigacts_exec((*p).sigacts);
    }

    // Close descriptors marked close-on-exec now that exec is committed.
    // SAFETY: `p` is the live current thread.
    unsafe {
        vfs_fdtable_close_on_exec((*p).fdtable);
    }

    // Wake vfork parent - we've replaced our address space so parent can
    // resume.
    vfork_done(p);

    argc as c_int // this ends up in a0, the first argument to main(argc, argv)
}

// ===========================================================================
// sys_exec — syscall entry point. Parses user arguments and calls exec().
// ===========================================================================

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_exec() -> u64 {
    let mut path: [c_char; MAXPATH] = [0; MAXPATH];
    let mut argv: [*mut c_char; MAXARG] = [ptr::null_mut(); MAXARG];
    let mut envp: [*mut c_char; MAXENV] = [ptr::null_mut(); MAXENV];
    let mut uargv: u64 = 0;
    let mut uenvp: u64 = 0;

    argaddr(1, &mut uargv);
    argaddr(2, &mut uenvp);
    if argstr(0, path.as_mut_ptr(), MAXPATH as c_int) < 0 {
        return (-1_i32) as u64;
    }

    // Goto-cleanup emulation (see `exec`'s own `bad!()`/`bad_locked!()`
    // doc above): frees every argv/envp buffer allocated so far, stopping
    // at the first still-NULL slot exactly like the C original's
    // `for (i = 0; i < NELEM(argv) && argv[i] != 0; i++) kfree(argv[i]);`.
    macro_rules! cleanup_and_fail {
        () => {{
            for k in 0..MAXARG {
                if argv[k].is_null() {
                    break;
                }
                kfree(argv[k] as *mut c_void);
            }
            for k in 0..MAXENV {
                if envp[k].is_null() {
                    break;
                }
                kfree(envp[k] as *mut c_void);
            }
            return (-1_i32) as u64;
        }};
    }

    let mut i: usize = 0;
    loop {
        if i >= MAXARG {
            cleanup_and_fail!();
        }
        let mut uarg: u64 = 0;
        if fetchaddr(uargv + (i * size_of::<u64>()) as u64, &mut uarg) < 0 {
            cleanup_and_fail!();
        }
        if uarg == 0 {
            argv[i] = ptr::null_mut();
            break;
        }
        let buf = kalloc();
        if buf.is_null() {
            cleanup_and_fail!();
        }
        argv[i] = buf as *mut c_char;
        if fetchstr(uarg, argv[i], PGSIZE as c_int) < 0 {
            cleanup_and_fail!();
        }
        i += 1;
    }

    // Parse envp from userspace (may be NULL or garbage from old 2-arg exec).
    let mut envc: usize = 0;
    if uenvp != 0 {
        let mut j: usize = 0;
        loop {
            if j >= MAXENV {
                break; // too many env vars, just truncate
            }
            let mut uenv: u64 = 0;
            if fetchaddr(uenvp + (j * size_of::<u64>()) as u64, &mut uenv) < 0 {
                break; // invalid pointer, treat as no envp
            }
            if uenv == 0 {
                envp[j] = ptr::null_mut();
                break;
            }
            let buf = kalloc();
            if buf.is_null() {
                cleanup_and_fail!();
            }
            envp[j] = buf as *mut c_char;
            if fetchstr(uenv, envp[j], PGSIZE as c_int) < 0 {
                kfree(envp[j] as *mut c_void);
                envp[j] = ptr::null_mut();
                break; // invalid string, stop here
            }
            envc += 1;
            j += 1;
        }
    }
    let _ = envc; // matches the C: `envc` is computed but only used for parsing bookkeeping.

    let ret = exec(path.as_mut_ptr(), argv.as_mut_ptr(), envp.as_mut_ptr());

    for k in 0..MAXARG {
        if argv[k].is_null() {
            break;
        }
        kfree(argv[k] as *mut c_void);
    }
    for k in 0..MAXENV {
        if envp[k].is_null() {
            break;
        }
        kfree(envp[k] as *mut c_void);
    }

    ret as u64
}
