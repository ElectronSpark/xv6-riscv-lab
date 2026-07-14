//! Rust port of `kernel/irq/syscall.c` (Phase 2 Wave 7 -- the syscall
//! dispatch table and the argument-fetch helpers). This completes the
//! `irq` module: [`super::plic`]/[`super::irq_core`] (Wave 5) and
//! [`super::trap`] (Wave 6) are already Rust; `trap.rs`'s `usertrap()`
//! calls [`syscall`] by name exactly as `kernelvec.S`/`trampoline.S` call
//! its own asm-visible entry points -- see that file's extern-block entry
//! `// irq/syscall.c (still C -- Wave 7)`, which is now stale text (this
//! file is not touched in this wave, see the task's file-touch scope) but
//! harmless: it is a plain declaration and resolves to this file's
//! `#[no_mangle]` definition at link time either way.
//!
//! # Arg-fetch helpers
//!
//! [`fetchaddr`]/[`fetchstr`]/[`argraw`]/[`argint`]/[`argint64`]/
//! [`argaddr`]/[`argstr`] keep the exact C names and signatures from
//! `kernel/inc/defs.h` -- roughly 90 call sites across both the remaining
//! C tree (`kernel/exec.c`, `kernel/vfs/vfs_syscall.c`) and this crate's
//! own `#[no_mangle] pub extern "C" fn sys_*` implementations
//! (`proc/sysproc.rs`, `proc/sys_signal.rs`, `mm/sysmm.rs`) depend on
//! zero deviation here. `vm_copyin`/`vm_copyinstr` (both already Rust,
//! `mm/vm.rs`) do their own full bounds validation internally and return
//! a negative errno on failure -- `fetchaddr`/`fetchstr` only need to
//! check that return value, exactly like the C originals.
//!
//! # Dispatch table
//!
//! `kernel/irq/syscall.c`'s `syscalls[]` was a `STATIC` C array built with
//! designated initializers (`[SYS_x] sys_x,`), sized to `SYS_dumpinode + 1`
//! (517 + 1 = 518) by the highest designator used, with every other slot
//! implicitly `NULL`. [`SYSCALLS`] reproduces this with a `const fn` +
//! mutable local array (stable since well before this crate's MSRV) built
//! by [`build_syscalls`]: one `t[SYS_x] = Some(sys_x);` line per C table
//! entry, in the same order, so the two are diffable line-for-line. A
//! plain array literal with 518 positional slots (most of them `None`)
//! would work just as well at runtime but would defeat exactly the
//! greppability the C designated-initializer syntax gave the original --
//! this is the judgment call the task description flagged as open.
//!
//! `kernel/inc/uabi/syscall.h`'s `SYS_*` numbers are hand-duplicated below
//! as local `usize` consts (same convention `irq/trap.rs` already uses for
//! `kernel/inc/trap.h`'s scause constants and `kernel/inc/riscv.h`'s
//! `SSTATUS_*` bits): the header is a plain `#define` list with no
//! static-inline bodies, so it *could* be added to `wrapper.h`'s bindgen
//! surface, but `wrapper.h` is outside this wave's file-touch scope.
//!
//! # Unsafe scoping
//!
//! Matches `irq/trap.rs`'s style: every ported function that was a plain
//! (non-`unsafe`) C function stays `pub extern "C" fn`, with `unsafe {}`
//! scoped to the individual raw-pointer dereferences/FFI calls inside,
//! each carrying a `// SAFETY:` comment.

#![allow(non_upper_case_globals)]

use core::ffi::{c_char, c_int, c_void};
use core::mem::size_of;

use crate::bindings::{vm, ENOSYS};
use crate::machine;

// ===========================================================================
// Externs -- every cross-module C-ABI symbol this file calls or exposes a
// table entry for, declared locally rather than imported by Rust path.
// Matches the established convention across the crate (see `irq/trap.rs`,
// `irq/irq_core.rs`, `printf.rs`'s own local redeclarations) rather than
// reaching through `crate::mm::...` / `crate::proc::...` module paths.
// ===========================================================================

// P3-1B mesh sweep: the callees below whose defining file is *in scope* for
// this wave (`proc/sysproc.rs`, `proc/sys_signal.rs`, `proc/pid.rs`,
// `proc/sched.rs`, `proc/rq.rs`, `exec.rs`, `proc/proc_shims.rs`) are now
// referenced as plain crate-path items instead of `extern "C"`
// redeclarations -- their definitions dropped `#[no_mangle]` accordingly
// (each still keeps its `extern "C" fn() -> u64` *type*, since this table
// stores them as `SyscallFn` function-pointer values, not calls by name).
// Everything else here (mm/sysmm.rs, mm/page.rs, mm/pcache.rs,
// vfs/vfs_syscall.rs, string.rs, mm/vm.rs) is out of this wave's scope and
// keeps its `extern` declaration untouched.
use crate::proc::{
    sys_clone, sys_exit, sys_getpid, sys_getppid, sys_gettid, sys_exit_group, sys_vfork,
    sys_wait, sys_waitpid, sys_sbrk, sys_sleep, sys_uptime, sys_gettimeofday, sys_nanosleep,
    sys_uname, sys_kernbase, sys_setpgid, sys_getpgid, sys_setsid, sys_getsid, sys_getrandom,
    sys_sigprocmask, sys_sigaction, sys_sigpending, sys_sigreturn, sys_pause, sys_kill,
    sys_tgkill, sys_tkill, sys_sigsuspend, sys_sigwait, sys_dumpproc, sys_dumpchan, sys_dumprq,
};
use crate::exec::sys_exec;

unsafe extern "C" {
    // printf.rs panic/print infra
    // printf is variadic, so it cannot be declared `safe`.
    fn printf(fmt: *const c_char, ...) -> c_int;

    // proc/proc_shims.rs. Kept as a local `extern` (not a `crate::proc::`
    // path): `xv6_panic` isn't being demoted (it has plenty of
    // out-of-scope callers), and several other `proc/*.rs` files declare
    // their own separate `xv6_panic` extern item too -- resolving through
    // `proc/mod.rs`'s glob re-export would hit the same `E0659`
    // name-ambiguity class documented in `timer/sched_timer.rs`.
    safe fn xv6_panic(msg: *const c_char) -> !;

    // string.rs
    fn strlen(s: *const c_char) -> usize;

    // mm/vm.rs
    safe fn vm_copyin(vm_ptr: *mut vm, dst: *mut c_void, srcva: u64, len: u64) -> c_int;
    safe fn vm_copyinstr(vm_ptr: *mut vm, dst: *mut c_char, srcva: u64, max: u64) -> c_int;

    // ---- Syscall implementations already ported to Rust ----
    // mm/sysmm.rs
    safe fn sys_mmap() -> u64;
    safe fn sys_munmap() -> u64;
    safe fn sys_mprotect() -> u64;
    safe fn sys_mremap() -> u64;
    safe fn sys_msync() -> u64;
    safe fn sys_mincore() -> u64;
    safe fn sys_madvise() -> u64;
    // mm/page.rs, mm/pcache.rs
    safe fn sys_memstat() -> u64;
    safe fn sys_dumppcache() -> u64;
    safe fn sys_sync() -> u64;

    // ---- Syscall implementations still C ----
    // vfs/vfs_syscall.c
    safe fn sys_vfs_dup() -> u64;
    safe fn sys_vfs_read() -> u64;
    safe fn sys_vfs_write() -> u64;
    safe fn sys_vfs_close() -> u64;
    safe fn sys_vfs_fstat() -> u64;
    safe fn sys_vfs_open() -> u64;
    safe fn sys_vfs_lseek() -> u64;
    safe fn sys_vfs_dup2() -> u64;
    safe fn sys_vfs_fcntl() -> u64;
    safe fn sys_vfs_access() -> u64;
    safe fn sys_vfs_rename() -> u64;
    safe fn sys_vfs_readlink() -> u64;
    safe fn sys_vfs_stat() -> u64;
    safe fn sys_vfs_lstat() -> u64;
    safe fn sys_vfs_ftruncate() -> u64;
    safe fn sys_vfs_mkdir() -> u64;
    safe fn sys_vfs_mknod() -> u64;
    safe fn sys_vfs_unlink() -> u64;
    safe fn sys_vfs_link() -> u64;
    safe fn sys_vfs_symlink() -> u64;
    safe fn sys_vfs_chdir() -> u64;
    safe fn sys_vfs_pipe() -> u64;
    safe fn sys_vfs_connect() -> u64;
    safe fn sys_getdents() -> u64;
    safe fn sys_statfs() -> u64;
    safe fn sys_chroot() -> u64;
    safe fn sys_mount() -> u64;
    safe fn sys_umount() -> u64;
    safe fn sys_getcwd() -> u64;
    safe fn sys_vfs_ioctl() -> u64;
    safe fn sys_tcgetattr() -> u64;
    safe fn sys_tcsetattr() -> u64;
    safe fn sys_vfs_poll() -> u64;
    safe fn sys_dumpinode() -> u64;
}

// ===========================================================================
// Arg-fetch helpers (`kernel/inc/defs.h`'s `argraw`/`argint`/`argint64`/
// `argaddr`/`argstr`/`fetchaddr`/`fetchstr` prototypes).
// ===========================================================================

/// Fetch the `u64` at `addr` from the current thread's user address space.
/// Rust port of `fetchaddr()`.
#[no_mangle]
pub extern "C" fn fetchaddr(addr: u64, ip: *mut u64) -> c_int {
    let p = machine::current_thread_ptr();
    // if(addr >= p->sz || addr+sizeof(uint64) > p->sz) // both tests needed,
    // in case of overflow
    //   return -1;
    // (kept commented out in the C original too -- `vm_copyin` does its own
    // full bounds validation, see the module doc.)
    // SAFETY: `p` is the live current thread (called only from a syscall's
    // own context, i.e. from a `sys_*` implementation reached through
    // `syscall()` below); `ip` is caller-owned space for one `u64`, per this
    // function's C-ABI out-parameter contract.
    unsafe {
        if vm_copyin((*p).vm, ip as *mut c_void, addr, size_of::<u64>() as u64) != 0 {
            return -1;
        }
    }
    0
}

/// Fetch the nul-terminated string at `addr` from the current thread's user
/// address space. Returns the length of the string (not including the nul),
/// or -1 on error. Rust port of `fetchstr()`.
#[no_mangle]
pub extern "C" fn fetchstr(addr: u64, buf: *mut c_char, max: c_int) -> c_int {
    let p = machine::current_thread_ptr();
    // SAFETY: `p` is the live current thread; `buf` is caller-owned space of
    // at least `max` bytes, per this function's C-ABI contract. `vm_copyinstr`
    // nul-terminates `buf` on success (0 return), so `strlen` below reads a
    // valid nul-terminated string.
    unsafe {
        if vm_copyinstr((*p).vm, buf, addr, max as u64) < 0 {
            return -1;
        }
        strlen(buf) as c_int
    }
}

/// Fetch the raw `u64` value of the `n`th system-call argument register
/// (`a0`..`a5`). Rust port of `argraw()`.
#[no_mangle]
pub extern "C" fn argraw(n: c_int) -> u64 {
    let p = machine::current_thread_ptr();
    // SAFETY: `p` is the live current thread trapping into the kernel via a
    // syscall; `trapframe` is its live `utrapframe`, populated by
    // `uservec`/`usertrap` before `syscall()` (and therefore any `sys_*` ->
    // `argN`) ever runs.
    unsafe {
        match n {
            0 => (*(*p).trapframe).trapframe.a0,
            1 => (*(*p).trapframe).trapframe.a1,
            2 => (*(*p).trapframe).trapframe.a2,
            3 => (*(*p).trapframe).trapframe.a3,
            4 => (*(*p).trapframe).trapframe.a4,
            5 => (*(*p).trapframe).trapframe.a5,
            _ => xv6_panic(c"argraw".as_ptr()),
        }
    }
}

/// Fetch the nth 32-bit system call argument. Rust port of `argint()`.
#[no_mangle]
pub extern "C" fn argint(n: c_int, ip: *mut c_int) {
    // SAFETY: `ip` is caller-owned space for one `c_int`, per this
    // function's C-ABI out-parameter contract (matches every call site
    // across the crate/tree).
    unsafe { *ip = argraw(n) as c_int };
}

/// Fetch the nth 64-bit system call argument. Rust port of `argint64()`.
#[no_mangle]
pub extern "C" fn argint64(n: c_int, ip: *mut i64) {
    // SAFETY: see `argint`.
    unsafe { *ip = argraw(n) as i64 };
}

/// Retrieve an argument as a pointer. Doesn't check for legality, since
/// copyin/copyout will do that. Rust port of `argaddr()`.
#[no_mangle]
pub extern "C" fn argaddr(n: c_int, ip: *mut u64) {
    // SAFETY: see `argint`.
    unsafe { *ip = argraw(n) };
}

/// Fetch the nth word-sized system call argument as a null-terminated
/// string. Copies into `buf`, at most `max` bytes. Returns the string
/// length on success (not including the nul, per `fetchstr`), or -1 on
/// error. Rust port of `argstr()`.
#[no_mangle]
pub extern "C" fn argstr(n: c_int, buf: *mut c_char, max: c_int) -> c_int {
    let mut addr: u64 = 0;
    argaddr(n, &mut addr);
    fetchstr(addr, buf, max)
}

// ===========================================================================
// Syscall numbers (`kernel/inc/uabi/syscall.h`). Hand-duplicated locally --
// see the module doc for why this isn't a `wrapper.h`/bindgen addition in
// this wave.
// ===========================================================================

// --- File system / VFS ---
const SYS_getcwd: usize = 17;
const SYS_dup: usize = 23;
const SYS_dup2: usize = 24;
const SYS_fcntl: usize = 25;
const SYS_ioctl: usize = 29;
const SYS_mknod: usize = 33;
const SYS_mkdir: usize = 34;
const SYS_unlink: usize = 35;
const SYS_symlink: usize = 36;
const SYS_link: usize = 37;
const SYS_umount: usize = 39;
const SYS_mount: usize = 40;
const SYS_statfs: usize = 43;
const SYS_ftruncate: usize = 46;
const SYS_access: usize = 48;
const SYS_chdir: usize = 49;
const SYS_chroot: usize = 51;
const SYS_open: usize = 56;
const SYS_close: usize = 57;
const SYS_pipe: usize = 59;
const SYS_getdents: usize = 61;
const SYS_lseek: usize = 62;
const SYS_read: usize = 63;
const SYS_write: usize = 64;
const SYS_readlink: usize = 78;
const SYS_stat: usize = 79;
const SYS_fstat: usize = 80;
const SYS_sync: usize = 81;
const SYS_rename: usize = 276;

// --- Process management ---
const SYS_exit: usize = 93;
const SYS_exit_group: usize = 94;
const SYS_nanosleep: usize = 101;
const SYS_kill: usize = 129;
const SYS_tkill: usize = 130;
const SYS_tgkill: usize = 131;
const SYS_setpgid: usize = 154;
const SYS_getpgid: usize = 155;
const SYS_getsid: usize = 156;
const SYS_setsid: usize = 157;
const SYS_uname: usize = 160;
const SYS_gettimeofday: usize = 169;
const SYS_getpid: usize = 172;
const SYS_getppid: usize = 173;
const SYS_gettid: usize = 178;
const SYS_clone: usize = 220;
const SYS_exec: usize = 221;
const SYS_getrandom: usize = 278;

// --- Networking ---
const SYS_connect: usize = 203;

// --- Memory management ---
const SYS_sbrk: usize = 214;
const SYS_munmap: usize = 215;
const SYS_mremap: usize = 216;
const SYS_mmap: usize = 222;
const SYS_mprotect: usize = 226;
const SYS_msync: usize = 227;
const SYS_mincore: usize = 232;
const SYS_madvise: usize = 233;

// --- Signals ---
const SYS_sigsuspend: usize = 133;
const SYS_sigaction: usize = 134;
const SYS_sigprocmask: usize = 135;
const SYS_sigpending: usize = 136;
const SYS_sigreturn: usize = 139;

// --- Polling ---
const SYS_poll: usize = 73;

// --- xv6-specific (no Linux equivalent) --- range 500+
const SYS_vfork: usize = 500;
const SYS_wait: usize = 501;
const SYS_waitpid: usize = 502;
const SYS_sleep: usize = 503;
const SYS_pause: usize = 504;
const SYS_uptime: usize = 505;
const SYS_tcgetattr: usize = 506;
const SYS_tcsetattr: usize = 507;
// SYS_sigalarm = 508: number reserved, never implemented; C's extern
// prototype and table entry are both commented out (`kernel/irq/
// syscall.c:92,197`) -- omitted here too, so it correctly falls through
// `syscall()`'s unknown-syscall path if ever invoked.
const SYS_sigwait: usize = 509;
const SYS_lstat: usize = 510;

// --- Debug / introspection (xv6-specific) ---
const SYS_memstat: usize = 511;
const SYS_dumpproc: usize = 512;
const SYS_dumpchan: usize = 513;
const SYS_dumppcache: usize = 514;
const SYS_dumprq: usize = 515;
const SYS_kernbase: usize = 516;
const SYS_dumpinode: usize = 517;

// ===========================================================================
// Syscall routing table
//
// All file system syscalls (pipe, read, write, open, close, etc.) are now
// routed to VFS implementations (sys_vfs_*). The legacy sysfile.c has been
// removed from the build.
//
// VFS syscalls use:
//   - vfs_fdtable for file descriptor management (replaces ofile[])
//   - vfs_file for file operations (replaces struct file)
//   - vfs_inode for inode operations (replaces struct inode)
// ===========================================================================

type SyscallFn = extern "C" fn() -> u64;

/// One past the highest `SYS_*` number with a table entry (`SYS_dumpinode`
/// = 517), matching the C original's designated-initializer array size.
const NSYSCALLS: usize = SYS_dumpinode + 1;

/// Build [`SYSCALLS`] at compile time. One `t[SYS_x] = Some(sys_x);` line
/// per entry in `kernel/irq/syscall.c`'s `syscalls[]`, same order, so the
/// two are diffable line-for-line (see the module doc).
const fn build_syscalls() -> [Option<SyscallFn>; NSYSCALLS] {
    let mut t: [Option<SyscallFn>; NSYSCALLS] = [None; NSYSCALLS];
    t[SYS_clone] = Some(sys_clone);
    t[SYS_exit] = Some(sys_exit);
    t[SYS_wait] = Some(sys_wait);
    t[SYS_pipe] = Some(sys_vfs_pipe); // VFS
    t[SYS_read] = Some(sys_vfs_read); // VFS
    t[SYS_kill] = Some(sys_kill);
    t[SYS_exec] = Some(sys_exec);
    t[SYS_fstat] = Some(sys_vfs_fstat); // VFS
    t[SYS_chdir] = Some(sys_vfs_chdir); // VFS
    t[SYS_dup] = Some(sys_vfs_dup); // VFS
    t[SYS_getpid] = Some(sys_getpid);
    t[SYS_getppid] = Some(sys_getppid);
    t[SYS_sbrk] = Some(sys_sbrk);
    t[SYS_sleep] = Some(sys_sleep);
    t[SYS_uptime] = Some(sys_uptime);
    t[SYS_open] = Some(sys_vfs_open); // VFS
    t[SYS_write] = Some(sys_vfs_write); // VFS
    t[SYS_mknod] = Some(sys_vfs_mknod); // VFS
    t[SYS_unlink] = Some(sys_vfs_unlink); // VFS
    t[SYS_link] = Some(sys_vfs_link); // VFS
    t[SYS_mkdir] = Some(sys_vfs_mkdir); // VFS
    t[SYS_close] = Some(sys_vfs_close); // VFS
    t[SYS_connect] = Some(sys_vfs_connect); // VFS
    t[SYS_symlink] = Some(sys_vfs_symlink); // VFS
    // SYS_sigalarm: dead, see the const's own comment above.
    t[SYS_sigaction] = Some(sys_sigaction);
    t[SYS_sigreturn] = Some(sys_sigreturn);
    t[SYS_sigpending] = Some(sys_sigpending);
    t[SYS_sigprocmask] = Some(sys_sigprocmask);
    t[SYS_pause] = Some(sys_pause);
    t[SYS_sigsuspend] = Some(sys_sigsuspend);
    t[SYS_sigwait] = Some(sys_sigwait);
    t[SYS_tkill] = Some(sys_tkill);
    t[SYS_gettid] = Some(sys_gettid);
    t[SYS_exit_group] = Some(sys_exit_group);
    t[SYS_tgkill] = Some(sys_tgkill);
    t[SYS_vfork] = Some(sys_vfork);
    t[SYS_setpgid] = Some(sys_setpgid);
    t[SYS_getpgid] = Some(sys_getpgid);
    t[SYS_setsid] = Some(sys_setsid);
    t[SYS_getsid] = Some(sys_getsid);
    t[SYS_getrandom] = Some(sys_getrandom);
    t[SYS_mmap] = Some(sys_mmap);
    t[SYS_munmap] = Some(sys_munmap);
    t[SYS_mprotect] = Some(sys_mprotect);
    t[SYS_mremap] = Some(sys_mremap);
    t[SYS_msync] = Some(sys_msync);
    t[SYS_mincore] = Some(sys_mincore);
    t[SYS_madvise] = Some(sys_madvise);
    t[SYS_memstat] = Some(sys_memstat);
    t[SYS_dumpproc] = Some(sys_dumpproc);
    t[SYS_dumpchan] = Some(sys_dumpchan);
    t[SYS_dumppcache] = Some(sys_dumppcache);
    t[SYS_dumprq] = Some(sys_dumprq);
    t[SYS_kernbase] = Some(sys_kernbase);
    t[SYS_dumpinode] = Some(sys_dumpinode);
    t[SYS_sync] = Some(sys_sync);
    t[SYS_ioctl] = Some(sys_vfs_ioctl);
    t[SYS_tcgetattr] = Some(sys_tcgetattr);
    t[SYS_tcsetattr] = Some(sys_tcsetattr);
    t[SYS_lseek] = Some(sys_vfs_lseek);
    t[SYS_dup2] = Some(sys_vfs_dup2);
    t[SYS_fcntl] = Some(sys_vfs_fcntl);
    t[SYS_access] = Some(sys_vfs_access);
    t[SYS_rename] = Some(sys_vfs_rename);
    t[SYS_readlink] = Some(sys_vfs_readlink);
    t[SYS_stat] = Some(sys_vfs_stat);
    t[SYS_lstat] = Some(sys_vfs_lstat);
    t[SYS_poll] = Some(sys_vfs_poll);
    t[SYS_ftruncate] = Some(sys_vfs_ftruncate);
    t[SYS_gettimeofday] = Some(sys_gettimeofday);
    t[SYS_waitpid] = Some(sys_waitpid);
    t[SYS_nanosleep] = Some(sys_nanosleep);
    t[SYS_uname] = Some(sys_uname);
    t[SYS_getdents] = Some(sys_getdents);
    t[SYS_statfs] = Some(sys_statfs);
    t[SYS_chroot] = Some(sys_chroot);
    t[SYS_mount] = Some(sys_mount);
    t[SYS_umount] = Some(sys_umount);
    t[SYS_getcwd] = Some(sys_getcwd);
    t
}

static SYSCALLS: [Option<SyscallFn>; NSYSCALLS] = build_syscalls();

/// Rust port of `syscall()`: dispatch the current thread's pending system
/// call (its number is in trapframe register `a7`) and write the return
/// value back into `a0`. Called from `usertrap()` (`irq/trap.rs`) exactly
/// where the C original was.
// P3-1B: only caller is `irq/trap.rs::usertrap` (crate-path `use`, not an
// `extern` redeclaration) -- demoted.
pub(crate) extern "C" fn syscall() {
    let p = machine::current_thread_ptr();

    // SAFETY: `p` is the live current thread that just trapped into the
    // kernel via `ecall` (this function's only caller, `usertrap()`, is
    // reached exclusively on that path); `trapframe` is its live
    // `utrapframe`.
    let num = unsafe { (*(*p).trapframe).trapframe.a7 } as c_int;

    if num > 0 && (num as usize) < SYSCALLS.len() {
        if let Some(f) = SYSCALLS[num as usize] {
            let ret = f();
            // SAFETY: same trapframe as the read above; writing `a0` is
            // exactly the C original's `p->trapframe->trapframe.a0 =
            // syscalls[num]();`.
            unsafe { (*(*p).trapframe).trapframe.a0 = ret };
            return;
        }
    }

    // SAFETY: `(*p).pid`/`(*p).name` are live fields of the current
    // thread; `num` is a plain scalar already read above. Matches
    // `printf("%d %s: unknown sys call %d\n", p->pid, p->name, num);`.
    unsafe {
        printf(
            c"%d %s: unknown sys call %d\n".as_ptr(),
            (*p).pid,
            (*p).name.as_ptr(),
            num,
        );
        (*(*p).trapframe).trapframe.a0 = (-(ENOSYS as i32)) as i64 as u64;
    }
}
