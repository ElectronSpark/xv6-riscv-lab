//! VFS system-call layer — Rust port of `kernel/vfs/vfs_syscall.c`
//! (Phase 2 Wave 17, see `docs/rustify/phase2_plan.md`). This is the
//! last file in the `vfs` core: every filesystem-facing syscall
//! (`open`/`read`/`write`/`close`/`stat`/`mkdir`/`mknod`/`unlink`/
//! `link`/`symlink`/`rename`/`chdir`/`getcwd`/`pipe`/`getdents`/
//! `chroot`/`mount`/`umount`/`ioctl`/`tcgetattr`/`tcsetattr`/`statfs`/
//! `poll`/`dup`/`dup2`/`lseek`/`ftruncate`/`fcntl`/`connect`/
//! `dumpinode`) is implemented here, dispatched on `argint`/`argaddr`/
//! `argstr` (Wave 7, `irq/syscall.rs`) and built on top of the fully
//! Rust `vfs` core (`inode.rs`/`file.rs`/`pipe.rs`/`fdtable.rs`/`fs.rs`,
//! Waves 13-16). `tmpfs/`, `xv6fs/`, `devtmpfs/` remain C filesystem
//! drivers reached only indirectly (through `vfs_inode_ops`/
//! `vfs_superblock_ops` vtables) — this file never calls into them by
//! name.
//!
//! # Calling convention
//!
//! Matches the established per-file convention (`vfs/inode.rs`,
//! `vfs/fs.rs`, ...): every cross-module C-ABI symbol this file calls
//! is declared in a local `unsafe extern "C"` block below rather than
//! reached through a `crate::vfs::...` module path, even though several
//! of the callees (`inode.rs`, `file.rs`, `pipe.rs`, `fdtable.rs`,
//! `fs.rs`) are themselves Rust and live in the same `mod vfs` — those
//! modules' internal helpers are not `pub`, only their `#[no_mangle]
//! extern "C"` surface is, so sibling files reach them exactly as any
//! other C translation unit would. `kernel/irq/syscall.rs`'s dispatch
//! table already declares `extern "C" fn sys_vfs_*() -> u64;` for every
//! symbol this file exports (unchanged since Wave 7) — the exact names
//! below are load-bearing.
//!
//! # `TIOCGPTN` sign-extension fix (mandated this wave)
//!
//! Userspace's `ioctl(int fd, int cmd, ...)` passes `cmd` as a plain
//! `int`. Per the RISC-V LP64 calling convention, a 32-bit **signed**
//! argument is sign-extended to fill its 64-bit register, so
//! `TIOCGPTN` (`0x8004_5430`, bit 31 set) arrives in `a1` as
//! `0xFFFF_FFFF_8004_5430`. [`sys_vfs_ioctl`] fetches that raw 64-bit
//! register value via `argaddr` (needed for the general opaque-`arg`
//! case), so the C original's `switch (cmd)` compared this
//! sign-extended value against `TIOCGPTN`'s zero-extended `#define`
//! value and never matched — `/dev/ptmx`'s `TIOCGPTN` fell through to
//! the `default:` opaque-pointer path instead of the dedicated
//! copy-out branch (Iteration 23's tracked "Known issue", now fixed
//! here). Every *other* ioctl command constant in this file's `switch`
//! (`TCGETS`, `TIOCGWINSZ`, ...) has bit 31 clear, so sign-extending a
//! positive `int` is a no-op for them — this is why only `TIOCGPTN` was
//! ever affected. The fix normalizes `cmd` to its zero-extended 32-bit
//! form immediately after the raw fetch (`(cmd as u32) as u64`) and
//! uses that single canonical value for both the `switch` *and* every
//! downstream call (`vfs_ioctl`, and transitively `tty::tty_ioctl`/
//! `tty::ptmx_fops_ioctl`'s own `cmd == TIOCGPTN` comparisons) — so the
//! fix is centralized at the one point raw user input enters the
//! kernel, rather than needing matching truncation at every driver
//! that compares `cmd` against a UAPI constant.
//!
//! # ABI-exact copies
//!
//! `struct stat`/`struct statfs`/`struct termios`/`struct winsize` are
//! the real bindgen-derived kernel/uapi layouts (`crate::bindings`);
//! `either_copyout`/`vm_copyout` always pass `size_of::<T>()` of the
//! concrete Rust type, so the byte count copied to userspace matches
//! the C `sizeof(kt)`/`sizeof(kst)` exactly. `struct linux_dirent64`
//! has a C99 flexible array member (`char d_name[]`), which bindgen
//! cannot represent — [`LinuxDirent64Header`] hand-mirrors the fixed
//! part only (matching the established `sysproc.rs` precedent for
//! UAPI structs bindgen can't emit cleanly, e.g. `Utsname`); its
//! `size_of` is 24 bytes (8 + 8 + 2 + 1, rounded up to 8-byte
//! alignment), identical to the C `sizeof(struct linux_dirent64)`, and
//! [`sys_getdents`] writes the variable-length name and NUL terminator
//! itself via raw pointer arithmetic at `header_size + name_len + 1`,
//! exactly like the C original's `de->d_name[name_len] = '\0'`.
//! `struct pollfd_k` (userspace's `struct pollfd`, never exposed in any
//! kernel header — the C original defined it locally too) is
//! hand-mirrored as [`PollfdK`] for the same reason.

#![allow(non_upper_case_globals)]

use core::ffi::{c_char, c_int, c_short, c_void};
use core::ptr;

use crate::bindings::{
    cdev_t, fs_struct, mode_t, stat, statfs, termios, thread, vfs_dentry, vfs_dir_iter, vfs_fdtable,
    vfs_file, vfs_inode, vfs_inode_ref, vfs_superblock, vm, winsize, work_struct, workqueue,
    EACCES, EBADF, EEXIST, EFAULT, EINTR, EINVAL, EISDIR, ELOOP, ENAMETOOLONG, ENODEV,
    ENOENT, ENOMEM, ENOSYS, ENOTDIR, ENOTTY, EOPNOTSUPP, EPERM, ERANGE,
};
use crate::sync::KSpinlock;

// ===========================================================================
// Externs — every cross-module C-ABI symbol this file calls, declared
// locally per this crate's established convention (see the module doc).
// ===========================================================================

unsafe extern "C" {
    // printf.rs — C-variadic.

    // string.rs.
    safe fn strlen(s: *const c_char) -> usize;
    safe fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;
    safe fn memset(dst: *mut c_void, c: c_int, n: usize) -> *mut c_void;

    // mm/kalloc.rs.
    safe fn kmm_alloc(n: usize) -> *mut c_void;
    safe fn kmm_free(ptr: *mut c_void);

    // proc module (kernel/proc/thread.rs, kernel/proc/sched.rs).
    safe fn xv6_current_thread() -> *mut thread;
    safe fn signal_pending(p: *mut thread) -> c_int;
    safe fn sleep_ms(ms: u64);

    // proc/workqueue.rs — deferred vfs_fput after an RCU grace period.
    safe fn queue_work(wq: *mut workqueue, work: *mut work_struct) -> bool;
    safe fn create_work_struct(
        func: Option<unsafe extern "C" fn(*mut work_struct)>,
        data: u64,
    ) -> *mut work_struct;
    safe fn free_work_struct(work: *mut work_struct);

    // lock/rcu.rs.
    safe fn call_rcu(
        head: *mut crate::bindings::rcu_head,
        func: crate::bindings::rcu_callback_t,
        data: *mut c_void,
    );

    // irq/syscall.rs — arg-fetch helpers.
    safe fn argint(n: c_int, ip: *mut c_int);
    safe fn argint64(n: c_int, ip: *mut i64);
    safe fn argaddr(n: c_int, ip: *mut u64);
    safe fn argstr(n: c_int, buf: *mut c_char, max: c_int) -> c_int;

    // mm/vm.rs.
    safe fn vm_copyout(vm_ptr: *mut vm, dstva: u64, src: *const c_void, len: u64) -> c_int;
    safe fn either_copyin(dst: *mut c_void, user_src: c_int, src: u64, len: u64) -> c_int;
    safe fn either_copyout(user_dst: c_int, dst: u64, src: *mut c_void, len: u64) -> c_int;

}

// P3-1C mesh sweep: vfs/{inode,file,fdtable,fs}.rs are in scope for this
// wave; converted from `extern "C"` redeclarations to plain crate-path
// items (identical signatures, same `crate::bindings::*` types this file
// already imports).
use crate::vfs::fdtable::{
    vfs_fdtable_alloc_fd, vfs_fdtable_alloc_fd_from, vfs_fdtable_dealloc_fd,
    vfs_fdtable_get_fdflags, vfs_fdtable_get_file, vfs_fdtable_set_fdflags,
};
use crate::vfs::file::{
    truncate, vfs_fileopen, vfs_fileread, vfs_filestat, vfs_filewrite, vfs_filelseek, vfs_fput,
    vfs_ioctl, vfs_pipealloc, vfs_sockalloc,
};
use crate::vfs::fs::{
    vfs_dump_inodes, vfs_dump_sb_inodes, vfs_get_dentry_inode, vfs_get_deferred_iput_wq,
    vfs_inode_deref, vfs_inode_get_ref, vfs_inode_put_ref, vfs_mount, vfs_mount_lock,
    vfs_mount_unlock, vfs_release_dentry, vfs_superblock_unlock, vfs_superblock_wlock,
    vfs_unmount,
};
use crate::vfs::inode::{
    vfs_chdir, vfs_chroot, vfs_create, vfs_dir_iter, vfs_idup, vfs_ilock, vfs_ilookup,
    vfs_iput, vfs_itruncate, vfs_iunlock, vfs_link, vfs_mkdir, vfs_mknod, vfs_move, vfs_namei,
    vfs_nameiparent, vfs_readlink, vfs_symlink, vfs_unlink, IRef,
};

// ===========================================================================
// Small helpers: negative-errno constants, ERR_PTR family, mode bits, O_*/
// F_*/ioctl/poll UAPI constants (none reachable through the bindgen
// allowlist for this file — same "hand-mirror locally" precedent as
// `vfs/file.rs`/`vfs/pipe.rs`/`vfs/fdtable.rs`), MAXPATH/DIRSIZ/NOFILE.
// ===========================================================================

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

/// Sign-extend a C `int` return value into this file's `u64` syscall
/// ABI, matching the C original's implicit `int` -> `uint64_t` return
/// conversion (two's-complement bit pattern, not a `0..=u32::MAX` zero
/// extension).
#[inline(always)]
const fn ret64(v: c_int) -> u64 {
    v as i64 as u64
}

// `is_err`/`is_err_or_null`/`ptr_err`'s canonical home is `crate::kstd`
// (P3-CS2 centralization). Note `kstd::ptr_err` returns `c_int`, not
// `isize` — every call site below already casts its result to `c_int`/
// `u64` or compares it against another `c_int` (`neg(...)`), so the
// narrower return type is a no-op change.
use crate::kstd::{is_err, is_err_or_null, ptr_err};

// `uabi/stat.h`'s `S_IF*`/`S_IS*` macros and permission bits.
const S_IFMT: u32 = 0o170000;
const S_IFDIR: u32 = 0o040000;
const S_IFCHR: u32 = 0o020000;
const S_IFBLK: u32 = 0o060000;
const S_IFREG: u32 = 0o100000;
const S_IFLNK: u32 = 0o120000;
const S_IFIFO: u32 = 0o010000;
const S_IFSOCK: u32 = 0o140000;

const S_IRUSR: u32 = 0o400;
const S_IWUSR: u32 = 0o200;
const S_IXUSR: u32 = 0o100;
const S_IRGRP: u32 = 0o040;
const S_IWGRP: u32 = 0o020;
const S_IXGRP: u32 = 0o010;
const S_IROTH: u32 = 0o004;
const S_IWOTH: u32 = 0o002;
const S_IXOTH: u32 = 0o001;

#[inline(always)]
fn is_dir(mode: u32) -> bool { mode & S_IFMT == S_IFDIR }
#[inline(always)]
fn is_chr(mode: u32) -> bool { mode & S_IFMT == S_IFCHR }
#[inline(always)]
fn is_blk(mode: u32) -> bool { mode & S_IFMT == S_IFBLK }
#[inline(always)]
fn is_reg(mode: u32) -> bool { mode & S_IFMT == S_IFREG }
#[inline(always)]
fn is_lnk(mode: u32) -> bool { mode & S_IFMT == S_IFLNK }
#[inline(always)]
fn is_fifo(mode: u32) -> bool { mode & S_IFMT == S_IFIFO }
#[inline(always)]
fn is_sock(mode: u32) -> bool { mode & S_IFMT == S_IFSOCK }

// `uabi/fcntl.h`'s `O_*` bits (musl layout: `O_ACCMODE = 03 | O_SEARCH`,
// matches `vfs/file.rs`/`vfs/pipe.rs`).
const O_RDONLY: c_int = 0o0;
const O_WRONLY: c_int = 0o1;
const O_RDWR: c_int = 0o2;
const O_ACCMODE: c_int = 0o3 | 0o10000000;
const O_CREAT: c_int = 0o100;
const O_EXCL: c_int = 0o200;
const O_TRUNC: c_int = 0o1000;
const O_NONBLOCK: c_int = 0o4000;
const O_NOFOLLOW: c_int = 0o400000;
const O_CLOEXEC: c_int = 0o2000000;

// `uabi/fcntl.h`'s `F_*`/`FD_CLOEXEC` constants.
const F_DUPFD: c_int = 0;
const F_GETFD: c_int = 1;
const F_SETFD: c_int = 2;
const F_GETFL: c_int = 3;
const F_SETFL: c_int = 4;
const F_DUPFD_CLOEXEC: c_int = 1030;
const FD_CLOEXEC: c_int = 1;

// `uabi/termios.h`'s ioctl request numbers.
const TCGETS: u64 = 0x5401;
const TCSETS: u64 = 0x5402;
const TCSETSW: u64 = 0x5403;
const TCSETSF: u64 = 0x5404;
const TIOCGWINSZ: u64 = 0x5413;
const TIOCSWINSZ: u64 = 0x5414;
const TIOCGPGRP: u64 = 0x540F;
const TIOCSPGRP: u64 = 0x5410;
const TIOCSCTTY: u64 = 0x540E;
const TIOCGPTN: u64 = 0x8004_5430;

// `tcsetattr`'s `optional_actions`.
const TCSANOW: c_int = 0;
const TCSADRAIN: c_int = 1;
const TCSAFLUSH: c_int = 2;

// `uabi/poll.h`'s event bits.
const POLLIN: c_short = 0x0001;
const POLLOUT: c_short = 0x0004;
const POLLERR: c_short = 0x0008;
const POLLNVAL: c_short = 0x0020;
const POLLRDNORM: c_short = 0x0040;
const POLLWRNORM: c_short = 0x0100;

// `uabi/linux_dirent64.h`'s `DT_*` constants.
const DT_UNKNOWN: u8 = 0;
const DT_FIFO: u8 = 1;
const DT_CHR: u8 = 2;
const DT_DIR: u8 = 4;
const DT_BLK: u8 = 6;
const DT_REG: u8 = 8;
const DT_LNK: u8 = 10;
const DT_SOCK: u8 = 12;

const MAXPATH: usize = 128;
const DIRSIZ: usize = 14;
const NOFILE: usize = 64;
const VFS_SYMLOOP_MAX: i32 = 8;

/// Rust port of `struct linux_dirent64`'s fixed prefix
/// (`kernel/inc/uabi/linux_dirent64.h`) — see the module doc's "ABI-exact
/// copies" section for why the C99 flexible-array-member tail
/// (`char d_name[]`) is not part of this type.
///
/// IMPORTANT: `size_of::<LinuxDirent64Header>()` (24, rounded up to the
/// struct's 8-byte alignment) is **not** the same as `offsetof(struct
/// linux_dirent64, d_name)` (19 = 8 + 8 + 2 + 1, right after `d_type`,
/// with no padding). A C flexible array member starts at the natural
/// (unpadded) end of the preceding fields, not at the padded struct
/// size — the same distinction `reclen`'s own C formula deliberately
/// glosses over (see [`LINUX_DIRENT64_NAME_OFFSET`]'s doc). Writing
/// `d_name` at `size_of()` instead of the true offset was caught during
/// this wave's `ls` acceptance test (every entry showed the same
/// truncated name) and fixed before landing.
#[repr(C)]
struct LinuxDirent64Header {
    d_ino: u64,
    d_off: i64,
    d_reclen: u16,
    d_type: u8,
}

/// True byte offset of the flexible-array-member `d_name` within
/// `struct linux_dirent64` (19), as opposed to
/// `size_of::<LinuxDirent64Header>()` (24) — see that type's doc.
const LINUX_DIRENT64_NAME_OFFSET: usize =
    core::mem::offset_of!(LinuxDirent64Header, d_type) + core::mem::size_of::<u8>();

/// Rust mirror of userspace's `struct pollfd` (never defined in any
/// kernel header — the C original, `struct pollfd_k`, was file-local
/// too).
#[repr(C)]
#[derive(Clone, Copy)]
struct PollfdK {
    fd: c_int,
    events: c_short,
    revents: c_short,
}

#[inline(always)]
fn mkdev(major: c_int, minor: c_int) -> crate::bindings::dev_t {
    ((major << 20) | minor) as crate::bindings::dev_t
}

/// Rust port of `fs.h`'s `static inline vfs_inode_is_local_root()` --
/// header-only in the C original (no external symbol), so it has no
/// `fs.rs` counterpart to extern; reimplemented locally like `vfs.h`'s
/// other `static inline` helpers already are elsewhere in the crate.
fn vfs_inode_is_local_root(inode: *mut vfs_inode) -> bool {
    if inode.is_null() {
        return false;
    }
    // SAFETY: non-null `inode`.
    let sb = unsafe { (*inode).sb };
    if sb.is_null() {
        return false;
    }
    // SAFETY: non-null `sb`.
    core::ptr::eq(inode, unsafe { (*sb).root_inode })
}

fn current() -> *mut thread {
    xv6_current_thread()
}

/// The current thread's `vfs_fdtable`. Every syscall in this file reads
/// this once per call, matching the C original's repeated
/// `current->fdtable` field reads.
fn current_fdtable() -> *mut vfs_fdtable {
    // SAFETY: `current()` is the live running thread.
    unsafe { (*current()).fdtable }
}

fn current_fs() -> *mut fs_struct {
    // SAFETY: `current()` is the live running thread.
    unsafe { (*current()).fs }
}

/// Acquire `fs->lock` (RAII). Mirrors `fs.h`'s `static inline
/// vfs_struct_lock()`/`vfs_struct_unlock()` pair -- header-only in the
/// C original, so reimplemented locally (see `vfs_inode_is_local_root`'s
/// doc for the same situation), matching `vfs/inode.rs`'s private
/// `fs_lock` helper (not reusable across sibling files -- see the module
/// doc's "Calling convention" section).
fn fs_lock(fs: *mut fs_struct) -> crate::sync::KSpinGuard {
    // SAFETY: `fs` is a live `fs_struct`; `.lock` is a plain embedded
    // field.
    KSpinlock::from_bindings(unsafe { ptr::addr_of_mut!((*fs).lock) }).lock()
}

/******************************************************************************
 * Helper functions (deferred file release, fd allocation)
 ******************************************************************************/

unsafe extern "C" fn vfs_fput_work_func(work: *mut work_struct) {
    // SAFETY: `work` is a live work item queued by `vfs_fd_rcucb` below,
    // whose `.data` is the `vfs_file*` to release.
    let file = unsafe { (*work).data } as *mut vfs_file;
    vfs_fput(file);
    free_work_struct(work);
}

unsafe extern "C" fn vfs_fd_rcucb(data: *mut c_void) {
    let file = data as *mut vfs_file;
    let wq = vfs_get_deferred_iput_wq();

    if wq.is_null() {
        // Workqueue not available (early init or shutdown): fall back to
        // a direct call.
        vfs_fput(file);
        return;
    }

    let work = create_work_struct(Some(vfs_fput_work_func), file as u64);
    if work.is_null() {
        crate::kprintln!("__vfs_fd_rcucb: failed to allocate work_struct, falling back to direct vfs_fput");
        vfs_fput(file);
        return;
    }

    queue_work(wq, work);
}

/// Defer `vfs_fput()` until the current RCU grace period completes, so
/// no concurrent `vfs_fdtable_get_file()` can still be observing `file`.
fn vfs_fput_call_rcu(file: *mut vfs_file) {
    call_rcu(ptr::null_mut(), Some(vfs_fd_rcucb), file as *mut c_void);
}

/// Look up `fd` in the current thread's fdtable with a `+1` refcount.
/// Caller must call `vfs_fput()`.
fn vfs_argfd(fd: c_int) -> *mut vfs_file {
    if fd < 0 || fd as usize >= NOFILE {
        return ptr::null_mut();
    }
    vfs_fdtable_get_file(current_fdtable(), fd)
}

/// Allocate an fd for `file`. Caller must hold `current->fdtable->lock`.
fn vfs_fdalloc(file: *mut vfs_file) -> c_int {
    vfs_fdtable_alloc_fd(current_fdtable(), file)
}

/// Deallocate `fd`. Caller must hold `current->fdtable->lock`.
fn vfs_fdfree(fd: c_int) -> *mut vfs_file {
    vfs_fdtable_dealloc_fd(current_fdtable(), fd)
}

/******************************************************************************
 * File Operations Syscalls
 ******************************************************************************/

pub(crate) extern "C" fn sys_vfs_dup() -> u64 {
    let mut fd: c_int = 0;
    argint(0, &mut fd);

    let f = vfs_argfd(fd);
    if f.is_null() {
        return ret64(neg(EBADF));
    }

    let newfd;
    {
        let _g = KSpinlock::from_bindings(unsafe { ptr::addr_of_mut!((*current_fdtable()).lock) }).lock();
        newfd = vfs_fdalloc(f);
    }

    vfs_fput(f); // remove the reference from vfs_argfd
    ret64(newfd)
}

pub(crate) extern "C" fn sys_vfs_dup2() -> u64 {
    let mut oldfd: c_int = 0;
    let mut newfd: c_int = 0;
    argint(0, &mut oldfd);
    argint(1, &mut newfd);

    if newfd < 0 || newfd as usize >= NOFILE {
        return ret64(neg(EBADF));
    }

    let f = vfs_argfd(oldfd);
    if f.is_null() {
        return ret64(neg(EBADF));
    }

    if oldfd == newfd {
        vfs_fput(f);
        return ret64(newfd);
    }

    let old_newfd;
    let ret;
    {
        let _g = KSpinlock::from_bindings(unsafe { ptr::addr_of_mut!((*current_fdtable()).lock) }).lock();
        old_newfd = vfs_fdfree(newfd);
        ret = vfs_fdtable_alloc_fd_from(current_fdtable(), f, newfd);
    }

    if !old_newfd.is_null() {
        vfs_fput_call_rcu(old_newfd);
    }
    vfs_fput(f);
    ret64(ret)
}

pub(crate) extern "C" fn sys_vfs_read() -> u64 {
    let mut fd: c_int = 0;
    let mut n: c_int = 0;
    let mut p: u64 = 0;

    argint(0, &mut fd);
    argaddr(1, &mut p);
    argint(2, &mut n);

    let f = vfs_argfd(fd);
    if f.is_null() {
        return ret64(neg(EBADF));
    }

    let ret = vfs_fileread(f, p as *mut c_void, n as usize, 1);
    vfs_fput(f);
    ret as u64
}

pub(crate) extern "C" fn sys_vfs_write() -> u64 {
    let mut fd: c_int = 0;
    let mut n: c_int = 0;
    let mut p: u64 = 0;

    argint(0, &mut fd);
    argaddr(1, &mut p);
    argint(2, &mut n);

    let f = vfs_argfd(fd);
    if f.is_null() {
        return ret64(neg(EBADF));
    }

    let ret = vfs_filewrite(f, p as *const c_void, n as usize, 1);
    vfs_fput(f);
    ret as u64
}

pub(crate) extern "C" fn sys_vfs_close() -> u64 {
    let mut fd: c_int = 0;
    argint(0, &mut fd);

    let f;
    {
        let _g = KSpinlock::from_bindings(unsafe { ptr::addr_of_mut!((*current_fdtable()).lock) }).lock();
        f = vfs_fdfree(fd);
        if f.is_null() {
            return ret64(neg(EBADF));
        }
    }

    vfs_fput_call_rcu(f);
    0
}

pub(crate) extern "C" fn sys_vfs_fstat() -> u64 {
    let mut fd: c_int = 0;
    let mut st: u64 = 0;

    argint(0, &mut fd);
    argaddr(1, &mut st);

    let f = vfs_argfd(fd);
    if f.is_null() {
        return ret64(neg(EBADF));
    }

    let mut kst: stat = unsafe { core::mem::zeroed() };
    let ret = vfs_filestat(f, &mut kst);
    if ret != 0 {
        vfs_fput(f);
        return ret64(ret);
    }

    if vm_copyout(
        unsafe { (*current()).vm },
        st,
        &kst as *const stat as *const c_void,
        core::mem::size_of::<stat>() as u64,
    ) < 0
    {
        vfs_fput(f);
        return ret64(neg(EFAULT));
    }

    vfs_fput(f);
    0
}

pub(crate) extern "C" fn sys_vfs_lseek() -> u64 {
    let mut fd: c_int = 0;
    let mut whence: c_int = 0;
    let mut offset: i64 = 0;
    argint(0, &mut fd);
    argint64(1, &mut offset);
    argint(2, &mut whence);

    let f = vfs_argfd(fd);
    if f.is_null() {
        return ret64(neg(EBADF));
    }

    let ret = vfs_filelseek(f, offset, whence);
    vfs_fput(f);
    ret as u64
}

pub(crate) extern "C" fn sys_vfs_ftruncate() -> u64 {
    let mut fd: c_int = 0;
    let mut length: i64 = 0;
    argint(0, &mut fd);
    argint64(1, &mut length);

    let f = vfs_argfd(fd);
    if f.is_null() {
        return ret64(neg(EBADF));
    }

    let ret = truncate(f, length);
    vfs_fput(f);
    ret64(ret)
}

pub(crate) extern "C" fn sys_vfs_fcntl() -> u64 {
    let mut fd: c_int = 0;
    let mut cmd: c_int = 0;
    let mut arg: c_int = 0;
    argint(0, &mut fd);
    argint(1, &mut cmd);
    argint(2, &mut arg);

    if fd < 0 || fd as usize >= NOFILE {
        return ret64(neg(EBADF));
    }

    if cmd == F_GETFD || cmd == F_SETFD {
        let ret;
        {
            let _g =
                KSpinlock::from_bindings(unsafe { ptr::addr_of_mut!((*current_fdtable()).lock) }).lock();
            ret = if cmd == F_GETFD {
                vfs_fdtable_get_fdflags(current_fdtable(), fd)
            } else {
                vfs_fdtable_set_fdflags(current_fdtable(), fd, arg & FD_CLOEXEC)
            };
        }
        return ret64(ret);
    }

    let f = vfs_argfd(fd);
    if f.is_null() {
        return ret64(neg(EBADF));
    }

    let mut ret = neg(EINVAL);
    match cmd {
        F_GETFL => {
            // SAFETY: non-null `f`.
            ret = unsafe { (*f).f_flags } & !O_CLOEXEC;
        }
        F_SETFL => {
            // SAFETY: non-null `f`.
            unsafe {
                (*f).f_flags = ((*f).f_flags & O_ACCMODE) | (arg & !(O_ACCMODE | O_CLOEXEC));
            }
            ret = 0;
        }
        F_DUPFD | F_DUPFD_CLOEXEC => {
            if arg < 0 || arg as usize >= NOFILE {
                ret = neg(EINVAL);
            } else {
                let _g =
                    KSpinlock::from_bindings(unsafe { ptr::addr_of_mut!((*current_fdtable()).lock) })
                        .lock();
                ret = vfs_fdtable_alloc_fd_from(current_fdtable(), f, arg);
                if ret >= 0 && cmd == F_DUPFD_CLOEXEC {
                    let _ = vfs_fdtable_set_fdflags(current_fdtable(), ret, FD_CLOEXEC);
                }
            }
        }
        _ => {
            ret = neg(EINVAL);
        }
    }

    vfs_fput(f);
    ret64(ret)
}

/// Fallback `getattr` when the inode has no filesystem-supplied
/// callback. Mirrors the C `__vfs_inode_stat()`.
fn vfs_inode_stat_fallback(inode: *mut vfs_inode, kst: *mut stat) -> c_int {
    // SAFETY: caller guarantees non-null `inode`/`kst`.
    unsafe {
        let ops = (*inode).ops;
        if !ops.is_null() {
            if let Some(getattr) = (*ops).getattr {
                return getattr(inode, kst);
            }
        }
        vfs_ilock(inode);
        memset(kst as *mut c_void, 0, core::mem::size_of::<stat>());
        (*kst).dev = if !(*inode).sb.is_null() { (*inode).sb as u64 as i32 } else { 0 };
        (*kst).ino = (*inode).ino;
        (*kst).mode = (*inode).mode;
        (*kst).nlink = (*inode).n_links;
        (*kst).size = (*inode).size as u64;
        vfs_iunlock(inode);
    }
    0
}

pub(crate) extern "C" fn sys_vfs_stat() -> u64 {
    let mut path: [c_char; MAXPATH] = [0; MAXPATH];
    let mut st_addr: u64 = 0;
    let n = argstr(0, path.as_mut_ptr(), MAXPATH as c_int);
    argaddr(1, &mut st_addr);
    if n < 0 {
        return ret64(neg(EFAULT));
    }

    let mut inode: IRef;
    let mut symloop_count = 0i32;

    loop {
        let raw_inode = vfs_namei(path.as_ptr(), unsafe { strlen(path.as_ptr()) });
        if is_err(raw_inode) {
            return ptr_err(raw_inode) as u64;
        }
        if raw_inode.is_null() {
            return ret64(neg(ENOENT));
        }
        // `IRef::from_raw`: `vfs_namei` succeeded (non-null, non-error),
        // whose postcondition is an owned, already-held reference.
        // Reassigning `inode` here drops the *previous* iteration's
        // reference automatically -- replaces the manual
        // `vfs_iput(inode); inode = ptr::null_mut();` pair below (P3-9d).
        // SAFETY: see the comment above.
        inode = unsafe { IRef::from_raw(raw_inode) };

        if !is_lnk(inode.mode) {
            break;
        }

        let link_len = vfs_readlink(IRef::as_ptr(&inode), path.as_mut_ptr(), MAXPATH - 1);
        if link_len < 0 {
            return link_len as u64;
        }
        unsafe { path[link_len as usize] = 0 };

        symloop_count += 1;
        if symloop_count >= VFS_SYMLOOP_MAX {
            return ret64(neg(ELOOP));
        }
    }

    let mut kst: stat = unsafe { core::mem::zeroed() };
    let ret = vfs_inode_stat_fallback(IRef::as_ptr(&inode), &mut kst);
    if ret != 0 {
        return ret64(ret);
    }
    if either_copyout(1, st_addr, &mut kst as *mut stat as *mut c_void, core::mem::size_of::<stat>() as u64) < 0 {
        return ret64(neg(EFAULT));
    }
    0
}

pub(crate) extern "C" fn sys_vfs_lstat() -> u64 {
    let mut path: [c_char; MAXPATH] = [0; MAXPATH];
    let mut name: [c_char; DIRSIZ + 1] = [0; DIRSIZ + 1];
    let mut st_addr: u64 = 0;
    let n = argstr(0, path.as_mut_ptr(), MAXPATH as c_int);
    argaddr(1, &mut st_addr);
    if n < 0 {
        return ret64(neg(EFAULT));
    }

    let parent = vfs_nameiparent(path.as_ptr(), n as usize, name.as_mut_ptr(), DIRSIZ + 1);
    if is_err(parent) {
        return ptr_err(parent) as u64;
    }
    if parent.is_null() {
        return ret64(neg(ENOENT));
    }

    let mut dentry: vfs_dentry = unsafe { core::mem::zeroed() };
    dentry.sb = unsafe { (*parent).sb };
    dentry.parent = parent;
    let ret = vfs_ilookup(parent, &mut dentry, name.as_ptr(), unsafe { strlen(name.as_ptr()) });
    if ret != 0 {
        vfs_iput(parent);
        return ret64(ret);
    }

    let inode = vfs_get_dentry_inode(&mut dentry);
    vfs_release_dentry(&mut dentry);
    vfs_iput(parent);
    if is_err(inode) {
        return ptr_err(inode) as u64;
    }
    if inode.is_null() {
        return ret64(neg(ENOENT));
    }

    let mut kst: stat = unsafe { core::mem::zeroed() };
    let ret = vfs_inode_stat_fallback(inode, &mut kst);
    vfs_iput(inode);
    if ret != 0 {
        return ret64(ret);
    }
    if either_copyout(1, st_addr, &mut kst as *mut stat as *mut c_void, core::mem::size_of::<stat>() as u64) < 0 {
        return ret64(neg(EFAULT));
    }
    0
}

pub(crate) extern "C" fn sys_vfs_access() -> u64 {
    let mut path: [c_char; MAXPATH] = [0; MAXPATH];
    let mut mode: c_int = 0;
    let n = argstr(0, path.as_mut_ptr(), MAXPATH as c_int);
    argint(1, &mut mode);
    if n < 0 {
        return ret64(neg(EFAULT));
    }

    let inode = vfs_namei(path.as_ptr(), n as usize);
    if is_err(inode) {
        return ptr_err(inode) as u64;
    }
    if inode.is_null() {
        return ret64(neg(ENOENT));
    }
    // `IRef::from_raw`: `vfs_namei` succeeded (non-null, non-error),
    // whose postcondition is an owned, already-held reference -- this
    // `IRef`'s `Drop` replaces all four manual `vfs_iput(inode)` calls
    // below (three error-path, one success-path), eliminating the
    // "forget on a new branch" leak shape (P3-9d).
    // SAFETY: see the comment above.
    let inode = unsafe { IRef::from_raw(inode) };

    if mode != 0 {
        let perm = inode.mode;
        if (mode & 4) != 0 && (perm & (S_IRUSR | S_IRGRP | S_IROTH)) == 0 {
            return ret64(neg(EACCES));
        }
        if (mode & 2) != 0 && (perm & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0 {
            return ret64(neg(EACCES));
        }
        if (mode & 1) != 0 && (perm & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0 {
            return ret64(neg(EACCES));
        }
    }

    0
}

pub(crate) extern "C" fn sys_vfs_readlink() -> u64 {
    let mut path: [c_char; MAXPATH] = [0; MAXPATH];
    let mut name: [c_char; DIRSIZ + 1] = [0; DIRSIZ + 1];
    let mut buf_addr: u64 = 0;
    let mut bufsz: c_int = 0;

    let n = argstr(0, path.as_mut_ptr(), MAXPATH as c_int);
    argaddr(1, &mut buf_addr);
    argint(2, &mut bufsz);
    if n < 0 {
        return ret64(neg(EFAULT));
    }
    if bufsz <= 0 {
        return ret64(neg(EINVAL));
    }

    let parent = vfs_nameiparent(path.as_ptr(), n as usize, name.as_mut_ptr(), DIRSIZ + 1);
    if is_err(parent) {
        return ptr_err(parent) as u64;
    }
    if parent.is_null() {
        return ret64(neg(ENOENT));
    }

    let mut dentry: vfs_dentry = unsafe { core::mem::zeroed() };
    dentry.sb = unsafe { (*parent).sb };
    dentry.parent = parent;
    let ret = vfs_ilookup(parent, &mut dentry, name.as_ptr(), unsafe { strlen(name.as_ptr()) });
    if ret != 0 {
        vfs_iput(parent);
        return ret64(ret);
    }

    let inode = vfs_get_dentry_inode(&mut dentry);
    vfs_release_dentry(&mut dentry);
    vfs_iput(parent);
    if is_err(inode) {
        return ptr_err(inode) as u64;
    }
    if inode.is_null() {
        return ret64(neg(ENOENT));
    }

    let kbuf = kmm_alloc(bufsz as usize);
    if kbuf.is_null() {
        vfs_iput(inode);
        return ret64(neg(ENOMEM));
    }

    let len = vfs_readlink(inode, kbuf as *mut c_char, bufsz as usize);
    vfs_iput(inode);
    if len < 0 {
        kmm_free(kbuf);
        return len as u64;
    }

    if either_copyout(1, buf_addr, kbuf, len as u64) < 0 {
        kmm_free(kbuf);
        return ret64(neg(EFAULT));
    }
    kmm_free(kbuf);
    len as u64
}

pub(crate) extern "C" fn sys_vfs_rename() -> u64 {
    let mut oldpath: [c_char; MAXPATH] = [0; MAXPATH];
    let mut newpath: [c_char; MAXPATH] = [0; MAXPATH];
    let mut oldname: [c_char; DIRSIZ + 1] = [0; DIRSIZ + 1];
    let mut newname: [c_char; DIRSIZ + 1] = [0; DIRSIZ + 1];
    let n1 = argstr(0, oldpath.as_mut_ptr(), MAXPATH as c_int);
    let n2 = argstr(1, newpath.as_mut_ptr(), MAXPATH as c_int);
    if n1 < 0 || n2 < 0 {
        return ret64(neg(EFAULT));
    }

    let old_parent = vfs_nameiparent(oldpath.as_ptr(), n1 as usize, oldname.as_mut_ptr(), DIRSIZ + 1);
    if is_err(old_parent) {
        return ptr_err(old_parent) as u64;
    }
    if old_parent.is_null() {
        return ret64(neg(ENOENT));
    }

    let new_parent = vfs_nameiparent(newpath.as_ptr(), n2 as usize, newname.as_mut_ptr(), DIRSIZ + 1);
    if is_err(new_parent) {
        vfs_iput(old_parent);
        return ptr_err(new_parent) as u64;
    }
    if new_parent.is_null() {
        vfs_iput(old_parent);
        return ret64(neg(ENOENT));
    }

    let mut old_dentry: vfs_dentry = unsafe { core::mem::zeroed() };
    old_dentry.sb = unsafe { (*old_parent).sb };
    old_dentry.parent = old_parent;
    let ret = vfs_ilookup(old_parent, &mut old_dentry, oldname.as_ptr(), unsafe { strlen(oldname.as_ptr()) });
    if ret != 0 {
        vfs_iput(old_parent);
        vfs_iput(new_parent);
        return ret64(ret);
    }

    let ret = vfs_move(
        old_parent,
        &mut old_dentry,
        new_parent,
        newname.as_ptr(),
        unsafe { strlen(newname.as_ptr()) },
    );
    vfs_release_dentry(&mut old_dentry);
    vfs_iput(old_parent);
    vfs_iput(new_parent);
    ret64(ret)
}

/******************************************************************************
 * File System Namespace Syscalls
 ******************************************************************************/

pub(crate) extern "C" fn sys_vfs_open() -> u64 {
    let mut path: [c_char; MAXPATH] = [0; MAXPATH];
    let mut name: [c_char; DIRSIZ + 1] = [0; DIRSIZ + 1];
    let mut omode: c_int = 0;

    argint(1, &mut omode);
    let n = argstr(0, path.as_mut_ptr(), MAXPATH as c_int);
    if n < 0 {
        return ret64(neg(EFAULT));
    }

    let mut inode: *mut vfs_inode = ptr::null_mut();

    if omode & O_CREAT != 0 {
        let parent = vfs_nameiparent(path.as_ptr(), n as usize, name.as_mut_ptr(), DIRSIZ + 1);
        if is_err(parent) {
            return ptr_err(parent) as u64;
        }
        if parent.is_null() {
            return ret64(neg(ENOENT));
        }

        let name_len = unsafe { strlen(name.as_ptr()) };
        inode = vfs_create(parent, 0o644, name.as_ptr(), name_len);
        vfs_iput(parent);

        if is_err(inode) {
            if ptr_err(inode) == neg(EEXIST) && omode & O_EXCL == 0 {
                inode = vfs_namei(path.as_ptr(), n as usize);
                if !is_err_or_null(inode) && is_dir(unsafe { (*inode).mode }) {
                    vfs_iput(inode);
                    return ret64(neg(EISDIR));
                }
            } else {
                return ptr_err(inode) as u64;
            }
        }
    } else {
        let mut symloop_count = 0i32;
        loop {
            inode = vfs_namei(path.as_ptr(), unsafe { strlen(path.as_ptr()) });
            if is_err(inode) {
                return ptr_err(inode) as u64;
            }
            if inode.is_null() {
                return ret64(neg(ENOENT));
            }

            if !is_lnk(unsafe { (*inode).mode }) || omode & O_NOFOLLOW != 0 {
                break;
            }

            let link_len = vfs_readlink(inode, path.as_mut_ptr(), MAXPATH - 1);
            vfs_iput(inode);
            inode = ptr::null_mut();

            if link_len < 0 {
                return link_len as u64;
            }
            unsafe { path[link_len as usize] = 0 };

            symloop_count += 1;
            if symloop_count >= VFS_SYMLOOP_MAX {
                break;
            }
        }

        if symloop_count >= VFS_SYMLOOP_MAX {
            return ret64(neg(ELOOP));
        }
    }

    if is_err(inode) {
        return ptr_err(inode) as u64;
    }
    if inode.is_null() {
        return ret64(neg(ENOENT));
    }

    if is_dir(unsafe { (*inode).mode }) && (omode & O_WRONLY != 0 || omode & O_RDWR != 0) {
        vfs_iput(inode);
        return ret64(neg(EISDIR));
    }

    let should_truncate = omode & O_TRUNC != 0 && is_reg(unsafe { (*inode).mode });

    let f = vfs_fileopen(inode, omode);
    vfs_iput(inode);

    if is_err(f) {
        return ptr_err(f) as u64;
    }

    if should_truncate {
        let ret = vfs_itruncate(vfs_inode_deref(unsafe { ptr::addr_of_mut!((*f).inode) }), 0);
        if ret != 0 {
            vfs_fput(f);
            return ret64(ret);
        }
    }

    let fd;
    {
        let _g = KSpinlock::from_bindings(unsafe { ptr::addr_of_mut!((*current_fdtable()).lock) }).lock();
        fd = vfs_fdalloc(f);
        if fd >= 0 && omode & O_CLOEXEC != 0 {
            let _ = vfs_fdtable_set_fdflags(current_fdtable(), fd, FD_CLOEXEC);
        }
    }
    // When success, the refcount of f will be increased by fdtable, thus we do
    // not put f here. When failure, we need to put f anyway.
    vfs_fput(f);
    ret64(fd)
}

pub(crate) extern "C" fn sys_vfs_mkdir() -> u64 {
    let mut path: [c_char; MAXPATH] = [0; MAXPATH];
    let mut name: [c_char; DIRSIZ + 1] = [0; DIRSIZ + 1];
    let n = argstr(0, path.as_mut_ptr(), MAXPATH as c_int);
    if n < 0 {
        return ret64(neg(EFAULT));
    }

    let parent = vfs_nameiparent(path.as_ptr(), n as usize, name.as_mut_ptr(), DIRSIZ + 1);
    if is_err(parent) {
        return ptr_err(parent) as u64;
    }
    if parent.is_null() {
        return ret64(neg(ENOENT));
    }

    let name_len = unsafe { strlen(name.as_ptr()) };
    let dir = vfs_mkdir(parent, 0o755, name.as_ptr(), name_len);
    vfs_iput(parent);

    if is_err(dir) {
        return ptr_err(dir) as u64;
    }

    vfs_iput(dir);
    0
}

pub(crate) extern "C" fn sys_vfs_mknod() -> u64 {
    let mut path: [c_char; MAXPATH] = [0; MAXPATH];
    let mut name: [c_char; DIRSIZ + 1] = [0; DIRSIZ + 1];
    let mut mode: c_int = 0;
    let mut major: c_int = 0;
    let mut minor: c_int = 0;

    let n = argstr(0, path.as_mut_ptr(), MAXPATH as c_int);
    if n < 0 {
        return ret64(neg(EFAULT));
    }
    argint(1, &mut mode);
    argint(2, &mut major);
    argint(3, &mut minor);

    let parent = vfs_nameiparent(path.as_ptr(), n as usize, name.as_mut_ptr(), DIRSIZ + 1);
    if is_err(parent) {
        return ptr_err(parent) as u64;
    }
    if parent.is_null() {
        return ret64(neg(ENOENT));
    }

    let name_len = unsafe { strlen(name.as_ptr()) };
    let dev = mkdev(major, minor);
    let node = vfs_mknod(parent, mode as mode_t, dev, name.as_ptr(), name_len);
    vfs_iput(parent);

    if is_err(node) {
        return ptr_err(node) as u64;
    }

    vfs_iput(node);
    0
}

pub(crate) extern "C" fn sys_vfs_unlink() -> u64 {
    let mut path: [c_char; MAXPATH] = [0; MAXPATH];
    let mut name: [c_char; DIRSIZ + 1] = [0; DIRSIZ + 1];
    let n = argstr(0, path.as_mut_ptr(), MAXPATH as c_int);
    if n < 0 {
        return ret64(neg(EFAULT));
    }

    let parent = vfs_nameiparent(path.as_ptr(), n as usize, name.as_mut_ptr(), DIRSIZ + 1);
    if is_err(parent) {
        return ptr_err(parent) as u64;
    }
    if parent.is_null() {
        return ret64(neg(ENOENT));
    }

    let name_len = unsafe { strlen(name.as_ptr()) };
    let ret = vfs_unlink(parent, name.as_ptr(), name_len);
    vfs_iput(parent);

    ret64(ret)
}

pub(crate) extern "C" fn sys_vfs_link() -> u64 {
    let mut old: [c_char; MAXPATH] = [0; MAXPATH];
    let mut new: [c_char; MAXPATH] = [0; MAXPATH];
    let mut name: [c_char; DIRSIZ + 1] = [0; DIRSIZ + 1];
    let n1 = argstr(0, old.as_mut_ptr(), MAXPATH as c_int);
    let n2 = argstr(1, new.as_mut_ptr(), MAXPATH as c_int);
    if n1 < 0 || n2 < 0 {
        return ret64(neg(EFAULT));
    }

    let src = vfs_namei(old.as_ptr(), n1 as usize);
    if is_err(src) {
        return ptr_err(src) as u64;
    }
    if src.is_null() {
        return ret64(neg(ENOENT));
    }

    if is_dir(unsafe { (*src).mode }) {
        vfs_iput(src);
        return ret64(neg(EPERM));
    }

    let parent = vfs_nameiparent(new.as_ptr(), n2 as usize, name.as_mut_ptr(), DIRSIZ + 1);
    if is_err(parent) {
        vfs_iput(src);
        return ptr_err(parent) as u64;
    }
    if parent.is_null() {
        vfs_iput(src);
        return ret64(neg(ENOENT));
    }

    let name_len = unsafe { strlen(name.as_ptr()) };

    let mut old_dentry: vfs_dentry = unsafe { core::mem::zeroed() };
    old_dentry.sb = unsafe { (*src).sb };
    old_dentry.ino = unsafe { (*src).ino };

    let ret = vfs_link(&mut old_dentry, parent, name.as_ptr(), name_len);

    vfs_iput(src);
    vfs_iput(parent);

    ret64(ret)
}

/// Convert a relative path to absolute based on the current thread's cwd.
/// Mirrors the C `vfs_make_absolute_path()`.
fn vfs_make_absolute_path(relpath: &[c_char], relpath_len: c_int, abspath: &mut [c_char; MAXPATH]) -> c_int {
    if relpath_len <= 0 {
        return neg(EINVAL);
    }

    if relpath[0] == b'/' as c_char {
        if relpath_len as usize >= MAXPATH {
            return neg(ENAMETOOLONG);
        }
        unsafe {
            memmove(
                abspath.as_mut_ptr() as *mut c_void,
                relpath.as_ptr() as *const c_void,
                relpath_len as usize,
            );
        }
        abspath[relpath_len as usize] = 0;
        return relpath_len;
    }

    let fs = current_fs();
    let (cwd, root) = {
        let _g = fs_lock(fs);
        // SAFETY: `fs` is live; `.cwd`/`.rooti` are plain embedded fields.
        unsafe { (vfs_inode_deref(ptr::addr_of_mut!((*fs).cwd)), vfs_inode_deref(ptr::addr_of_mut!((*fs).rooti))) }
    };

    if cwd.is_null() {
        return neg(ENOENT);
    }

    let mut names: [*mut c_char; MAXPATH / 2] = [ptr::null_mut(); MAXPATH / 2];
    let mut name_count = 0usize;

    let mut inode = cwd;
    while !core::ptr::eq(inode, root) {
        // SAFETY: `inode` is a live, non-null vfs_inode throughout this walk
        // (checked/reassigned only from other live inode pointers below).
        unsafe {
            if core::ptr::eq((*inode).parent, inode) {
                let mountpoint = (*(*inode).sb).mountpoint;
                if mountpoint.is_null() {
                    break;
                }
                if !(*mountpoint).name.is_null() {
                    names[name_count] = (*mountpoint).name;
                    name_count += 1;
                }
                inode = (*mountpoint).parent;
                if inode.is_null() || core::ptr::eq(inode, mountpoint) {
                    break;
                }
                continue;
            }

            if !(*inode).name.is_null() {
                names[name_count] = (*inode).name;
                name_count += 1;
            }
            inode = (*inode).parent;
            if inode.is_null() {
                break;
            }
        }
    }

    let mut pathlen: usize = 0;
    abspath[pathlen] = b'/' as c_char;
    pathlen += 1;
    for i in (0..name_count).rev() {
        // SAFETY: `names[i]` is a live, NUL-terminated inode/mountpoint name.
        let len = unsafe { strlen(names[i]) };
        if pathlen + len + 1 >= MAXPATH {
            return neg(ENAMETOOLONG);
        }
        unsafe {
            memmove(
                abspath.as_mut_ptr().add(pathlen) as *mut c_void,
                names[i] as *const c_void,
                len,
            );
        }
        pathlen += len;
        abspath[pathlen] = b'/' as c_char;
        pathlen += 1;
    }
    if pathlen + relpath_len as usize >= MAXPATH {
        return neg(ENAMETOOLONG);
    }
    unsafe {
        memmove(
            abspath.as_mut_ptr().add(pathlen) as *mut c_void,
            relpath.as_ptr() as *const c_void,
            relpath_len as usize,
        );
    }
    pathlen += relpath_len as usize;
    abspath[pathlen] = 0;

    pathlen as c_int
}

pub(crate) extern "C" fn sys_vfs_symlink() -> u64 {
    let mut target: [c_char; MAXPATH] = [0; MAXPATH];
    let mut linkpath: [c_char; MAXPATH] = [0; MAXPATH];
    let mut name: [c_char; DIRSIZ + 1] = [0; DIRSIZ + 1];
    let n1 = argstr(0, target.as_mut_ptr(), MAXPATH as c_int);
    let n2 = argstr(1, linkpath.as_mut_ptr(), MAXPATH as c_int);
    if n1 < 0 || n2 < 0 {
        return ret64(neg(EFAULT));
    }

    let mut abs_target: [c_char; MAXPATH] = [0; MAXPATH];
    let abs_len = vfs_make_absolute_path(&target, n1, &mut abs_target);
    if abs_len < 0 {
        return ret64(abs_len);
    }

    let parent = vfs_nameiparent(linkpath.as_ptr(), n2 as usize, name.as_mut_ptr(), DIRSIZ + 1);
    if is_err(parent) {
        return ptr_err(parent) as u64;
    }
    if parent.is_null() {
        return ret64(neg(ENOENT));
    }

    let name_len = unsafe { strlen(name.as_ptr()) };

    let sym = vfs_symlink(parent, 0o777, name.as_ptr(), name_len, abs_target.as_ptr(), abs_len as usize);
    vfs_iput(parent);

    if is_err(sym) {
        return ptr_err(sym) as u64;
    }

    vfs_iput(sym);
    0
}

pub(crate) extern "C" fn sys_vfs_chdir() -> u64 {
    let mut path: [c_char; MAXPATH] = [0; MAXPATH];
    let n = argstr(0, path.as_mut_ptr(), MAXPATH as c_int);
    if n < 0 {
        return ret64(neg(EFAULT));
    }

    let inode = vfs_namei(path.as_ptr(), n as usize);
    if is_err(inode) {
        return ptr_err(inode) as u64;
    }
    if inode.is_null() {
        return ret64(neg(ENOENT));
    }

    if !is_dir(unsafe { (*inode).mode }) {
        vfs_iput(inode);
        return ret64(neg(ENOTDIR));
    }

    // Get a reference to the new cwd BEFORE acquiring the spinlock
    // (vfs_inode_get_ref may acquire the inode mutex internally).
    let mut new_cwd_ref: vfs_inode_ref = unsafe { core::mem::zeroed() };
    let ret = vfs_inode_get_ref(inode, &mut new_cwd_ref);
    if ret != 0 {
        vfs_iput(inode);
        return ret64(ret);
    }

    // Update the process cwd (only the assignment happens under the
    // spinlock).
    let fs = current_fs();
    let mut old_cwd: vfs_inode_ref;
    {
        let _g = fs_lock(fs);
        // SAFETY: `fs` is live; `.cwd` is a plain embedded field.
        old_cwd = unsafe { (*fs).cwd };
        unsafe { (*fs).cwd = new_cwd_ref };
    }

    vfs_inode_put_ref(&mut old_cwd);
    vfs_iput(inode);

    0
}

/******************************************************************************
 * Getcwd Syscall
 ******************************************************************************/

pub(crate) extern "C" fn sys_getcwd() -> u64 {
    let mut buf_addr: u64 = 0;
    let mut size: c_int = 0;

    argaddr(0, &mut buf_addr);
    argint(1, &mut size);

    if size <= 0 {
        return ret64(neg(EINVAL));
    }

    let mut path: [c_char; MAXPATH] = [0; MAXPATH];
    let mut pathlen: usize = 0;

    let p = current();
    let fs = current_fs();
    let (cwd, root) = {
        let _g = fs_lock(fs);
        // SAFETY: `fs` is live; `.cwd`/`.rooti` are plain embedded fields.
        unsafe { (vfs_inode_deref(ptr::addr_of_mut!((*fs).cwd)), vfs_inode_deref(ptr::addr_of_mut!((*fs).rooti))) }
    };

    if cwd.is_null() {
        return ret64(neg(ENOENT));
    }

    let mut names: [*mut c_char; MAXPATH / 2] = [ptr::null_mut(); MAXPATH / 2];
    let mut name_count = 0usize;

    let mut inode = cwd;
    while !core::ptr::eq(inode, root) {
        // SAFETY: see `vfs_make_absolute_path`'s identical walk.
        unsafe {
            if core::ptr::eq((*inode).parent, inode) {
                let mountpoint = (*(*inode).sb).mountpoint;
                if mountpoint.is_null() {
                    break;
                }
                if !(*mountpoint).name.is_null() {
                    names[name_count] = (*mountpoint).name;
                    name_count += 1;
                }
                inode = (*mountpoint).parent;
                if inode.is_null() || core::ptr::eq(inode, mountpoint) {
                    break;
                }
                continue;
            }

            if !(*inode).name.is_null() {
                names[name_count] = (*inode).name;
                name_count += 1;
            }
            inode = (*inode).parent;
            if inode.is_null() {
                break;
            }
        }
    }

    path[pathlen] = b'/' as c_char;
    pathlen += 1;
    for i in (0..name_count).rev() {
        // SAFETY: `names[i]` is a live, NUL-terminated inode/mountpoint name.
        let len = unsafe { strlen(names[i]) };
        if pathlen + len + 1 >= MAXPATH {
            return ret64(neg(ENAMETOOLONG));
        }
        unsafe {
            memmove(path.as_mut_ptr().add(pathlen) as *mut c_void, names[i] as *const c_void, len);
        }
        pathlen += len;
        if i > 0 {
            path[pathlen] = b'/' as c_char;
            pathlen += 1;
        }
    }
    path[pathlen] = 0;

    if pathlen + 1 > size as usize {
        return ret64(neg(ERANGE));
    }

    if vm_copyout(unsafe { (*p).vm }, buf_addr, path.as_ptr() as *const c_void, (pathlen + 1) as u64) < 0 {
        return ret64(neg(EFAULT));
    }

    buf_addr
}

/******************************************************************************
 * Pipe Syscall
 ******************************************************************************/

pub(crate) extern "C" fn sys_vfs_pipe() -> u64 {
    let mut fdarray: u64 = 0;
    argaddr(0, &mut fdarray);

    let mut rf: *mut vfs_file = ptr::null_mut();
    let mut wf: *mut vfs_file = ptr::null_mut();
    let ret = vfs_pipealloc(&mut rf, &mut wf);
    if ret != 0 {
        return ret64(ret);
    }

    let fd0;
    let fd1;
    {
        let _g = KSpinlock::from_bindings(unsafe { ptr::addr_of_mut!((*current_fdtable()).lock) }).lock();
        fd0 = vfs_fdalloc(rf);
        if fd0 < 0 {
            drop(_g);
            vfs_fput(rf);
            vfs_fput(wf);
            return ret64(fd0);
        }

        fd1 = vfs_fdalloc(wf);
        if fd1 < 0 {
            vfs_fdfree(fd0);
            drop(_g);
            vfs_fput(rf);
            vfs_fput(wf);
            vfs_fput_call_rcu(rf);
            return ret64(fd1);
        }
    }

    // vm_copyout may sleep (acquires rwsem), so it must run outside the
    // spinlock.
    let p = current();
    if vm_copyout(unsafe { (*p).vm }, fdarray, &fd0 as *const c_int as *const c_void, core::mem::size_of::<c_int>() as u64) < 0
        || vm_copyout(
            unsafe { (*p).vm },
            fdarray + core::mem::size_of::<c_int>() as u64,
            &fd1 as *const c_int as *const c_void,
            core::mem::size_of::<c_int>() as u64,
        ) < 0
    {
        {
            let _g = KSpinlock::from_bindings(unsafe { ptr::addr_of_mut!((*current_fdtable()).lock) }).lock();
            vfs_fdfree(fd0);
            vfs_fdfree(fd1);
        }

        vfs_fput(rf);
        vfs_fput(wf);
        vfs_fput_call_rcu(rf);
        vfs_fput_call_rcu(wf);
        return ret64(neg(EFAULT));
    }

    // Release the references from vfs_pipealloc - fdtable holds its own
    // references now (same pattern as sys_vfs_open's vfs_fput after
    // vfs_fdalloc).
    vfs_fput(rf);
    vfs_fput(wf);

    0
}

/******************************************************************************
 * Socket Syscall
 ******************************************************************************/

pub(crate) extern "C" fn sys_vfs_connect() -> u64 {
    let mut raddr: c_int = 0;
    let mut lport: c_int = 0;
    let mut rport: c_int = 0;

    argint(0, &mut raddr);
    argint(1, &mut lport);
    argint(2, &mut rport);

    let mut f: *mut vfs_file = ptr::null_mut();
    let ret = vfs_sockalloc(&mut f, raddr as u32, lport as u16, rport as u16);
    if ret != 0 {
        return ret64(ret);
    }

    let fd;
    {
        let _g = KSpinlock::from_bindings(unsafe { ptr::addr_of_mut!((*current_fdtable()).lock) }).lock();
        fd = vfs_fdalloc(f);
    }

    // When success, the refcount of f will be increased by fdtable, thus we do
    // not put f here. When failure, we need to put f anyway.
    vfs_fput(f);
    ret64(fd)
}

/******************************************************************************
 * Directory Operations - getdents
 ******************************************************************************/

fn mode_to_dtype(mode: mode_t) -> u8 {
    if is_reg(mode) { return DT_REG; }
    if is_dir(mode) { return DT_DIR; }
    if is_chr(mode) { return DT_CHR; }
    if is_blk(mode) { return DT_BLK; }
    if is_fifo(mode) { return DT_FIFO; }
    if is_lnk(mode) { return DT_LNK; }
    if is_sock(mode) { return DT_SOCK; }
    DT_UNKNOWN
}

pub(crate) extern "C" fn sys_getdents() -> u64 {
    let mut fd: c_int = 0;
    let mut dirp: u64 = 0;
    let mut count: c_int = 0;

    argint(0, &mut fd);
    argaddr(1, &mut dirp);
    argint(2, &mut count);

    let f = vfs_argfd(fd);
    if f.is_null() {
        return ret64(neg(EBADF));
    }

    // SAFETY: non-null `f`.
    let inode = vfs_inode_deref(unsafe { ptr::addr_of_mut!((*f).inode) });
    if inode.is_null() || !is_dir(unsafe { (*inode).mode }) {
        vfs_fput(f);
        return ret64(neg(ENOTDIR));
    }

    let kbuf = kmm_alloc(count as usize);
    if kbuf.is_null() {
        vfs_fput(f);
        return ret64(neg(ENOMEM));
    }

    let mut bytes_written: usize = 0;
    let mut dentry: vfs_dentry = unsafe { core::mem::zeroed() };

    while (bytes_written as c_int) < count {
        // Save iterator state before calling vfs_dir_iter, in case we need
        // to revert it.
        // SAFETY: non-null `f`.
        let (saved_cookies, saved_index) =
            unsafe { ((*f).__bindgen_anon_1.dir_iter.cookies, (*f).__bindgen_anon_1.dir_iter.index) };

        let ret = vfs_dir_iter(inode, unsafe { ptr::addr_of_mut!((*f).__bindgen_anon_1.dir_iter) }, &mut dentry);
        if ret != 0 {
            kmm_free(kbuf);
            vfs_fput(f);
            return ret64(ret);
        }

        if dentry.name.is_null() {
            // End of directory.
            break;
        }

        let name_len = dentry.name_len as usize;
        let mut reclen = core::mem::size_of::<LinuxDirent64Header>() + name_len + 1;
        reclen = (reclen + 7) & !7; // Align to 8 bytes.

        if bytes_written + reclen > count as usize {
            // Not enough space; restore the iterator state for the next call.
            // SAFETY: non-null `f`.
            unsafe {
                (*f).__bindgen_anon_1.dir_iter.cookies = saved_cookies;
                (*f).__bindgen_anon_1.dir_iter.index = saved_index;
            }
            vfs_release_dentry(&mut dentry);
            break;
        }

        // Get the inode's d_type.
        let child = vfs_get_dentry_inode(&mut dentry);
        let mut d_type = DT_UNKNOWN;
        if !is_err_or_null(child) {
            d_type = mode_to_dtype(unsafe { (*child).mode });
            vfs_iput(child);
        }

        // Fill in the dirent.
        // SAFETY: `kbuf` has `count` live bytes; `bytes_written + reclen <=
        // count` was just checked above.
        unsafe {
            let de = (kbuf as *mut u8).add(bytes_written) as *mut LinuxDirent64Header;
            (*de).d_ino = dentry.ino;
            (*de).d_off = (*f).__bindgen_anon_1.dir_iter.index;
            (*de).d_reclen = reclen as u16;
            (*de).d_type = d_type;
            let name_dst = (de as *mut u8).add(LINUX_DIRENT64_NAME_OFFSET);
            memmove(name_dst as *mut c_void, dentry.name as *const c_void, name_len);
            *name_dst.add(name_len) = 0;
        }

        bytes_written += reclen;
        vfs_release_dentry(&mut dentry);
        dentry = unsafe { core::mem::zeroed() };
    }

    if bytes_written > 0 {
        if vm_copyout(unsafe { (*current()).vm }, dirp, kbuf, bytes_written as u64) < 0 {
            kmm_free(kbuf);
            vfs_fput(f);
            return ret64(neg(EFAULT));
        }
    }

    kmm_free(kbuf);
    vfs_fput(f);
    bytes_written as u64
}

/******************************************************************************
 * chroot - Change root directory
 ******************************************************************************/

pub(crate) extern "C" fn sys_chroot() -> u64 {
    let mut path: [c_char; MAXPATH] = [0; MAXPATH];
    let n = argstr(0, path.as_mut_ptr(), MAXPATH as c_int);
    if n < 0 {
        return ret64(neg(EFAULT));
    }

    let new_root = vfs_namei(path.as_ptr(), n as usize);
    if is_err(new_root) {
        return ptr_err(new_root) as u64;
    }
    if new_root.is_null() {
        return ret64(neg(ENOENT));
    }

    if !is_dir(unsafe { (*new_root).mode }) {
        vfs_iput(new_root);
        return ret64(neg(ENOTDIR));
    }

    let ret = vfs_chroot(new_root);
    if ret < 0 {
        vfs_iput(new_root);
        return ret64(ret);
    }

    let ret = vfs_chdir(new_root);
    vfs_iput(new_root);

    ret64(ret)
}

/******************************************************************************
 * mount - Mount a filesystem
 ******************************************************************************/

/// Kernel-internal mount helper: resolves the target path, the (optional)
/// source device, acquires the mount/superblock/inode locks in order, and
/// calls `vfs_mount()`. Called both from `sys_mount` and from
/// `vfs_init()`'s `/tmp`/`/dev` bring-up (`vfs/fs.rs`, via its own local
/// extern). Mirrors the C `vfs_mount_path()`.
pub(crate) extern "C" fn vfs_mount_path(
    fstype: *const c_char,
    target: *const c_char,
    target_len: c_int,
    source: *const c_char,
    source_len: c_int,
) -> c_int {
    let target_dir = vfs_namei(target, target_len as usize);
    if is_err(target_dir) {
        return ptr_err(target_dir) as c_int;
    }
    if target_dir.is_null() {
        return neg(ENOENT);
    }

    if !is_dir(unsafe { (*target_dir).mode }) {
        vfs_iput(target_dir);
        return neg(ENOTDIR);
    }

    let mut source_inode: *mut vfs_inode = ptr::null_mut();
    if !source.is_null() && source_len > 0 {
        let source_dev = vfs_namei(source, source_len as usize);
        if !is_err_or_null(source_dev) {
            if is_blk(unsafe { (*source_dev).mode }) {
                source_inode = source_dev;
            } else {
                vfs_iput(source_dev);
            }
        }
    }

    // Acquire the locks vfs_mount() requires: mount mutex, superblock write
    // lock, mountpoint inode lock.
    vfs_mount_lock();
    vfs_superblock_wlock(unsafe { (*target_dir).sb });
    vfs_ilock(target_dir);

    let ret = vfs_mount(fstype, target_dir, source_inode, 0, ptr::null());

    // On success, release the locks. On failure, vfs_mount() already
    // released them.
    if ret == 0 {
        vfs_iunlock(target_dir);
        vfs_superblock_unlock(unsafe { (*target_dir).sb });
    }
    vfs_mount_unlock();

    if !source_inode.is_null() {
        vfs_iput(source_inode);
    }
    vfs_iput(target_dir);

    ret
}

pub(crate) extern "C" fn sys_mount() -> u64 {
    let mut source: [c_char; MAXPATH] = [0; MAXPATH];
    let mut target: [c_char; MAXPATH] = [0; MAXPATH];
    let mut fstype: [c_char; 32] = [0; 32];

    let n1 = argstr(0, source.as_mut_ptr(), MAXPATH as c_int);
    let n2 = argstr(1, target.as_mut_ptr(), MAXPATH as c_int);
    if n1 < 0 || n2 < 0 || argstr(2, fstype.as_mut_ptr(), 32) < 0 {
        return ret64(neg(EFAULT));
    }

    ret64(vfs_mount_path(fstype.as_ptr(), target.as_ptr(), n2, source.as_ptr(), n1))
}

/******************************************************************************
 * umount - Unmount a filesystem
 ******************************************************************************/

/// Kernel-internal unmount helper: resolves the mounted filesystem's root
/// via `vfs_namei` (which follows mounts), validates it is actually a
/// mount root, acquires the lock nest `vfs_unmount()` requires, and calls
/// it. Mirrors the C `vfs_umount_path()`.
pub(crate) extern "C" fn vfs_umount_path(target: *const c_char, target_len: c_int) -> c_int {
    let mounted_root = vfs_namei(target, target_len as usize);
    if is_err(mounted_root) {
        return ptr_err(mounted_root) as c_int;
    }
    if mounted_root.is_null() {
        return neg(ENOENT);
    }

    if !vfs_inode_is_local_root(mounted_root) {
        vfs_iput(mounted_root);
        return neg(EINVAL); // Not a mounted filesystem root.
    }

    // SAFETY: non-null `mounted_root`.
    let child_sb = unsafe { (*mounted_root).sb };
    // SAFETY: non-null `child_sb` checked next line.
    if child_sb.is_null() || unsafe { (*child_sb).mountpoint }.is_null() {
        vfs_iput(mounted_root);
        return neg(EINVAL); // Not mounted, or no mountpoint.
    }

    // SAFETY: `child_sb` checked non-null above.
    let target_dir = unsafe { (*child_sb).mountpoint };
    // SAFETY: non-null `target_dir`.
    if unsafe { (*target_dir).__bindgen_anon_1.mount() } == 0 {
        vfs_iput(mounted_root);
        return neg(EINVAL); // Mountpoint not marked as a mount.
    }

    // Acquire the locks vfs_unmount() requires: mount mutex, parent
    // superblock write lock, child superblock write lock, mountpoint inode
    // lock, mounted-root inode lock.
    vfs_mount_lock();
    vfs_superblock_wlock(unsafe { (*target_dir).sb });
    vfs_superblock_wlock(child_sb);
    vfs_ilock(target_dir);
    vfs_ilock(mounted_root);

    let ret = vfs_unmount(target_dir);

    if ret != 0 {
        // On failure, release the locks in reverse order (vfs_unmount()
        // did not free anything).
        vfs_iunlock(mounted_root);
        vfs_iunlock(target_dir);
        vfs_superblock_unlock(child_sb);
        vfs_superblock_unlock(unsafe { (*target_dir).sb });
        vfs_mount_unlock();
        vfs_iput(mounted_root);
        return ret;
    }

    // On success, vfs_unmount() has already unlocked and freed
    // mounted_root/child_sb; only release what's left.
    vfs_iunlock(target_dir);
    vfs_superblock_unlock(unsafe { (*target_dir).sb });
    vfs_mount_unlock();

    0
}

pub(crate) extern "C" fn sys_umount() -> u64 {
    let mut target: [c_char; MAXPATH] = [0; MAXPATH];
    let n = argstr(0, target.as_mut_ptr(), MAXPATH as c_int);
    if n < 0 {
        return ret64(neg(EFAULT));
    }

    ret64(vfs_umount_path(target.as_ptr(), n))
}

/******************************************************************************
 * Debug: Dump active inodes
 ******************************************************************************/

pub(crate) extern "C" fn sys_dumpinode() -> u64 {
    let mut path: [c_char; MAXPATH] = [0; MAXPATH];

    let n = argstr(0, path.as_mut_ptr(), MAXPATH as c_int);
    if n < 0 {
        // No path argument: dump every superblock.
        vfs_dump_inodes();
        return 0;
    }

    let inode = vfs_namei(path.as_ptr(), n as usize);
    if inode.is_null() {
        crate::kprintln!("dumpinode: cannot find path '{}'", crate::printf::Cs(path.as_ptr()));
        return ret64(neg(ENOENT));
    }

    // SAFETY: non-null `inode`.
    let sb = unsafe { (*inode).sb };
    vfs_iput(inode);

    if sb.is_null() {
        crate::kprintln!("dumpinode: inode has no superblock");
        return ret64(neg(EINVAL));
    }

    vfs_dump_sb_inodes(sb);
    0
}

/******************************************************************************
 * TTY / ioctl Syscalls
 ******************************************************************************/

pub(crate) extern "C" fn sys_vfs_ioctl() -> u64 {
    let mut fd: c_int = 0;
    let mut cmd_raw: u64 = 0;
    let mut arg: u64 = 0;

    argint(0, &mut fd);
    argaddr(1, &mut cmd_raw);
    argaddr(2, &mut arg);

    // Normalize `cmd` to its zero-extended 32-bit form -- see the module
    // doc's "TIOCGPTN sign-extension fix" section. `argaddr` returns the
    // raw a1 register, which the RISC-V calling convention sign-extends
    // from userspace's `int cmd` argument; truncating/zero-extending here
    // is a no-op for every ioctl constant with bit 31 clear (all of them
    // except TIOCGPTN) and makes TIOCGPTN compare correctly both in the
    // `switch` below and in every downstream `cmd`-comparing driver this
    // normalized value is subsequently passed to.
    let cmd: u64 = (cmd_raw as u32) as u64;

    let f = vfs_argfd(fd);
    if f.is_null() {
        return ret64(neg(EBADF));
    }

    let mut ret: c_int;

    // For known TTY ioctls, copy the data in/out of user space here, then
    // pass the kernel buffer to vfs_ioctl. For unknown commands, pass the
    // raw arg through as an opaque void* (the handler interprets it).
    match cmd {
        TCGETS => {
            let mut kt: termios = unsafe { core::mem::zeroed() };
            ret = vfs_ioctl(f, cmd, &mut kt as *mut termios as *mut c_void);
            if ret == 0 {
                if either_copyout(1, arg, &mut kt as *mut termios as *mut c_void, core::mem::size_of::<termios>() as u64) < 0 {
                    ret = neg(EFAULT);
                }
            }
        }
        TCSETS | TCSETSW | TCSETSF => {
            let mut kt: termios = unsafe { core::mem::zeroed() };
            if either_copyin(&mut kt as *mut termios as *mut c_void, 1, arg, core::mem::size_of::<termios>() as u64) < 0 {
                ret = neg(EFAULT);
            } else {
                ret = vfs_ioctl(f, cmd, &mut kt as *mut termios as *mut c_void);
            }
        }
        TIOCGWINSZ => {
            let mut kws: winsize = unsafe { core::mem::zeroed() };
            ret = vfs_ioctl(f, cmd, &mut kws as *mut winsize as *mut c_void);
            if ret == 0 {
                if either_copyout(1, arg, &mut kws as *mut winsize as *mut c_void, core::mem::size_of::<winsize>() as u64) < 0 {
                    ret = neg(EFAULT);
                }
            }
        }
        TIOCSWINSZ => {
            let mut kws: winsize = unsafe { core::mem::zeroed() };
            if either_copyin(&mut kws as *mut winsize as *mut c_void, 1, arg, core::mem::size_of::<winsize>() as u64) < 0 {
                ret = neg(EFAULT);
            } else {
                ret = vfs_ioctl(f, cmd, &mut kws as *mut winsize as *mut c_void);
            }
        }
        TIOCGPGRP => {
            let mut kpgid: crate::bindings::pid_t = 0;
            ret = vfs_ioctl(f, cmd, &mut kpgid as *mut crate::bindings::pid_t as *mut c_void);
            if ret == 0 {
                if either_copyout(1, arg, &mut kpgid as *mut crate::bindings::pid_t as *mut c_void, core::mem::size_of::<crate::bindings::pid_t>() as u64) < 0 {
                    ret = neg(EFAULT);
                }
            }
        }
        TIOCSPGRP => {
            let mut kpgid: crate::bindings::pid_t = 0;
            if either_copyin(&mut kpgid as *mut crate::bindings::pid_t as *mut c_void, 1, arg, core::mem::size_of::<crate::bindings::pid_t>() as u64) < 0 {
                ret = neg(EFAULT);
            } else {
                ret = vfs_ioctl(f, cmd, &mut kpgid as *mut crate::bindings::pid_t as *mut c_void);
            }
        }
        TIOCGPTN => {
            let mut kptn: c_int = 0;
            ret = vfs_ioctl(f, cmd, &mut kptn as *mut c_int as *mut c_void);
            if ret == 0 {
                if either_copyout(1, arg, &mut kptn as *mut c_int as *mut c_void, core::mem::size_of::<c_int>() as u64) < 0 {
                    ret = neg(EFAULT);
                }
            }
        }
        TIOCSCTTY => {
            // `arg` is an integer flag (usually 0); pass it through as-is.
            ret = vfs_ioctl(f, cmd, arg as *mut c_void);
        }
        _ => {
            // Unknown ioctl -- pass arg through as an opaque pointer.
            ret = vfs_ioctl(f, cmd, arg as *mut c_void);
        }
    }

    vfs_fput(f);
    ret64(ret)
}

pub(crate) extern "C" fn sys_tcgetattr() -> u64 {
    let mut fd: c_int = 0;
    let mut termios_p: u64 = 0;

    argint(0, &mut fd);
    argaddr(1, &mut termios_p);

    let f = vfs_argfd(fd);
    if f.is_null() {
        return ret64(neg(EBADF));
    }

    let mut kt: termios = unsafe { core::mem::zeroed() };
    let mut ret = vfs_ioctl(f, TCGETS, &mut kt as *mut termios as *mut c_void);
    if ret == 0 {
        if either_copyout(1, termios_p, &mut kt as *mut termios as *mut c_void, core::mem::size_of::<termios>() as u64) < 0 {
            ret = neg(EFAULT);
        }
    }
    vfs_fput(f);
    ret64(ret)
}

pub(crate) extern "C" fn sys_tcsetattr() -> u64 {
    let mut fd: c_int = 0;
    let mut optional_actions: c_int = 0;
    let mut termios_p: u64 = 0;

    argint(0, &mut fd);
    argint(1, &mut optional_actions);
    argaddr(2, &mut termios_p);

    let cmd: u64 = match optional_actions {
        TCSANOW => TCSETS,
        TCSADRAIN => TCSETSW,
        TCSAFLUSH => TCSETSF,
        _ => return ret64(neg(EINVAL)),
    };

    let f = vfs_argfd(fd);
    if f.is_null() {
        return ret64(neg(EBADF));
    }

    let mut kt: termios = unsafe { core::mem::zeroed() };
    if either_copyin(&mut kt as *mut termios as *mut c_void, 1, termios_p, core::mem::size_of::<termios>() as u64) < 0 {
        vfs_fput(f);
        return ret64(neg(EFAULT));
    }

    let ret = vfs_ioctl(f, cmd, &mut kt as *mut termios as *mut c_void);
    vfs_fput(f);
    ret64(ret)
}

/// `sys_statfs` - get filesystem statistics.
pub(crate) extern "C" fn sys_statfs() -> u64 {
    let mut path: [c_char; MAXPATH] = [0; MAXPATH];
    let mut buf_addr: u64 = 0;
    let n = argstr(0, path.as_mut_ptr(), MAXPATH as c_int);
    argaddr(1, &mut buf_addr);
    if n < 0 {
        return ret64(neg(EFAULT));
    }

    let inode = vfs_namei(path.as_ptr(), unsafe { strlen(path.as_ptr()) });
    if is_err(inode) {
        return ptr_err(inode) as u64;
    }
    if inode.is_null() {
        return ret64(neg(ENOENT));
    }

    // SAFETY: non-null `inode`.
    let sb = unsafe { (*inode).sb };
    if sb.is_null() {
        vfs_iput(inode);
        return ret64(neg(ENOSYS));
    }

    let mut kbuf: statfs = unsafe { core::mem::zeroed() };

    // SAFETY: non-null `sb`.
    unsafe {
        kbuf.f_bsize = (*sb).block_size as u64;
        kbuf.f_frsize = (*sb).block_size as u64;
        kbuf.f_blocks = (*sb).total_blocks;
        kbuf.f_bfree = if (*sb).total_blocks > (*sb).used_blocks {
            (*sb).total_blocks - (*sb).used_blocks
        } else {
            0
        };
        kbuf.f_bavail = kbuf.f_bfree;
    }
    kbuf.f_namelen = DIRSIZ as u64;

    // Let the filesystem fill in additional details if it implements statfs.
    // SAFETY: non-null `sb`.
    let ops = unsafe { (*sb).ops };
    if !ops.is_null() {
        if let Some(statfs_cb) = unsafe { (*ops).statfs } {
            // SAFETY: `sb`/`kbuf` are both live; `statfs_cb` is a
            // filesystem driver callback.
            let ret = unsafe { statfs_cb(sb, &mut kbuf) };
            if ret < 0 {
                vfs_iput(inode);
                return ret64(ret);
            }
        }
    }

    vfs_iput(inode);

    if either_copyout(1, buf_addr, &mut kbuf as *mut statfs as *mut c_void, core::mem::size_of::<statfs>() as u64) < 0 {
        return ret64(neg(EFAULT));
    }
    0
}

/// Report requested events as ready based on the file's access mode.
/// Fallback for file types with no dedicated poll callback (regular
/// files, block devices, /dev/null, ...). Mirrors the C
/// `__vfs_poll_always_ready()`.
fn vfs_poll_always_ready(events: c_short, f_flags: c_int) -> c_short {
    let mut revents: c_short = 0;
    if events & (POLLIN | POLLRDNORM) != 0 && (f_flags & O_ACCMODE) != O_WRONLY {
        revents |= events & (POLLIN | POLLRDNORM);
    }
    if events & (POLLOUT | POLLWRNORM) != 0 && (f_flags & O_ACCMODE) != O_RDONLY {
        revents |= events & (POLLOUT | POLLWRNORM);
    }
    revents
}

/// Check readiness for a set of file descriptors. Dispatches on file
/// type: `vfs_file_ops.poll` (pipes, sockets, any future pollable file
/// type) -> legacy `struct sock` (stubbed, matches the C original's own
/// commented-out `sockpoll()` call) -> character device `cdev_ops.poll`
/// -> "always ready" fallback. Mirrors the C `__vfs_poll_scan()`.
fn vfs_poll_scan(pfds: &mut [PollfdK]) -> c_int {
    let mut ready: c_int = 0;

    for pfd in pfds.iter_mut() {
        pfd.revents = 0;

        if pfd.fd < 0 {
            continue;
        }

        let f = vfs_argfd(pfd.fd);
        if f.is_null() {
            pfd.revents |= POLLNVAL;
            ready += 1;
            continue;
        }

        // SAFETY: non-null `f`.
        let ops = unsafe { (*f).ops };
        let poll_cb = if !ops.is_null() { unsafe { (*ops).poll } } else { None };

        if let Some(poll_fn) = poll_cb {
            // SAFETY: `f` is live; `poll_fn` is a driver callback.
            pfd.revents = unsafe { poll_fn(f, pfd.events) } as c_short;
        } else {
            // SAFETY: non-null `f`.
            let inode = unsafe { (*f).inode.inode };

            if inode.is_null() {
                // No inode AND no `vfs_file_ops.poll`. If `ops` is NULL
                // entirely this is a legacy socket (`vfs_sockalloc()`);
                // polling its rxq is not yet implemented (matches the C
                // original's own commented-out `sockpoll()` call).
                // SAFETY: non-null `f`.
                let sock = unsafe { (*f).__bindgen_anon_1.sock };
                if ops.is_null() && !sock.is_null() {
                    // pfd.revents = sockpoll(sock, pfd.events); // not implemented
                } else {
                    // SAFETY: non-null `f`.
                    pfd.revents = vfs_poll_always_ready(pfd.events, unsafe { (*f).f_flags });
                }
            } else if is_chr(unsafe { (*inode).mode }) {
                // Character device: delegate to its poll callback if one
                // is registered, else "always ready".
                // SAFETY: non-null `f`.
                let cdev: *mut cdev_t = unsafe { (*f).__bindgen_anon_1.cdev };
                let cdev_poll = if !cdev.is_null() { unsafe { (*cdev).ops.poll } } else { None };
                if let Some(cdev_poll_fn) = cdev_poll {
                    // SAFETY: `cdev` is live; `cdev_poll_fn` is a driver
                    // callback.
                    pfd.revents = unsafe { cdev_poll_fn(cdev, pfd.events) } as c_short;
                } else {
                    // SAFETY: non-null `f`.
                    pfd.revents = vfs_poll_always_ready(pfd.events, unsafe { (*f).f_flags });
                }
            } else {
                // Regular files, directories, block devices: report
                // "always ready" (standard POSIX behaviour).
                // SAFETY: non-null `f`.
                pfd.revents = vfs_poll_always_ready(pfd.events, unsafe { (*f).f_flags });
            }
        }

        vfs_fput(f);

        if pfd.revents != 0 {
            ready += 1;
        }
    }

    ready
}

pub(crate) extern "C" fn sys_vfs_poll() -> u64 {
    let mut fds_addr: u64 = 0;
    let mut nfds: c_int = 0;
    let mut timeout_ms: c_int = 0;

    argaddr(0, &mut fds_addr);
    argint(1, &mut nfds);
    argint(2, &mut timeout_ms);

    if nfds < 0 || nfds as usize > NOFILE {
        return ret64(neg(EINVAL));
    }

    if nfds == 0 {
        if timeout_ms == 0 {
            return 0;
        }
        let timeout_ticks = if timeout_ms > 0 { crate::machine::ms_to_rawticks(timeout_ms as u64) } else { 0 };
        let start = crate::machine::read_time();
        loop {
            if !(timeout_ms < 0 || (crate::machine::read_time() - start) < timeout_ticks) {
                break;
            }
            sleep_ms(1);
            if signal_pending(current()) != 0 {
                return ret64(neg(EINTR));
            }
        }
        return 0;
    }

    let bytes = nfds as usize * core::mem::size_of::<PollfdK>();
    let pfds_raw = kmm_alloc(bytes) as *mut PollfdK;
    if pfds_raw.is_null() {
        return ret64(neg(ENOMEM));
    }

    if either_copyin(pfds_raw as *mut c_void, 1, fds_addr, bytes as u64) < 0 {
        kmm_free(pfds_raw as *mut c_void);
        return ret64(neg(EFAULT));
    }

    // SAFETY: `pfds_raw` is a fresh, exclusively-owned `nfds`-element
    // array, just filled in by `either_copyin` above.
    let pfds = unsafe { core::slice::from_raw_parts_mut(pfds_raw, nfds as usize) };

    let timeout_ticks = if timeout_ms > 0 { crate::machine::ms_to_rawticks(timeout_ms as u64) } else { 0 };
    let start = crate::machine::read_time();
    let mut ready: c_int;
    loop {
        ready = vfs_poll_scan(pfds);
        if ready > 0 {
            break;
        }
        if timeout_ms == 0 {
            break;
        }
        if timeout_ms > 0 && (crate::machine::read_time() - start) >= timeout_ticks {
            break;
        }
        sleep_ms(1);
        if signal_pending(current()) != 0 {
            ready = neg(EINTR);
            break;
        }
    }

    if ready == neg(EINTR) {
        kmm_free(pfds_raw as *mut c_void);
        return ret64(neg(EINTR));
    }

    if either_copyout(1, fds_addr, pfds_raw as *mut c_void, bytes as u64) < 0 {
        kmm_free(pfds_raw as *mut c_void);
        return ret64(neg(EFAULT));
    }

    kmm_free(pfds_raw as *mut c_void);
    ready as u64
}
