//! Memory-management syscall thunks — Rust port of `kernel/mm/sysmm.c`.
//!
//! Thin wrappers that pull syscall arguments off the trapframe via
//! `argaddr` / `argint`, look up the current process's `vm`, and dispatch
//! to the `vm_*` functions in `kernel/mm/vm.c`.
//!
//! All algorithmic logic lives in safe Rust; only the FFI boundary
//! (`mod ffi`), the per-CPU `tp` register read, and the `#[no_mangle]
//! extern "C"` syscall entry points are `unsafe`.


use core::ffi::{c_int, c_uchar, c_void};
use crate::bindings::{cpu_local, thread};

// ---------------------------------------------------------------------------
// Constants mirrored from `kernel/inc/errno.h` and `kernel/inc/riscv.h`.
// ---------------------------------------------------------------------------
const EINVAL: i32 = 22;
const EFAULT: i32 = 14;
const PGSIZE: u64 = 4096;
const SSTATUS_SIE: u64 = 1 << 1;

fn pgroundup(sz: u64) -> u64 {
    (sz + PGSIZE - 1) & !(PGSIZE - 1)
}

fn neg_errno(e: i32) -> u64 {
    (-e) as i64 as u64
}

// ---------------------------------------------------------------------------
// FFI boundary.
// ---------------------------------------------------------------------------
mod ffi {
    use super::*;
    unsafe extern "C" {
        // Arg extraction (kernel/irq/syscall.c).
        pub safe fn argaddr(n: c_int, ip: *mut u64);
        pub safe fn argint(n: c_int, ip: *mut c_int);

        // vm.h prototypes.
        pub safe fn vm_mmap(vm: *mut c_void, addr: u64, length: usize,
                       prot: c_int, flags: c_int, fd: c_int, offset: u64) -> u64;
        pub safe fn vm_munmap(vm: *mut c_void, addr: u64, length: usize) -> c_int;
        pub safe fn vm_mprotect(vm: *mut c_void, addr: u64, size: usize, prot: c_int) -> c_int;
        pub safe fn vm_mremap(vm: *mut c_void, old_addr: u64, old_size: usize,
                         new_size: usize, flags: c_int, new_addr: u64) -> u64;
        pub safe fn vm_msync(vm: *mut c_void, addr: u64, size: usize, flags: c_int) -> c_int;
        pub safe fn vm_mincore(vm: *mut c_void, addr: u64, size: usize,
                          vec: *mut c_uchar) -> c_int;
        pub safe fn vm_madvise(vm: *mut c_void, addr: u64, size: usize, advice: c_int) -> c_int;
        pub safe fn vm_copyout(vm: *mut c_void, dstva: u64, src: *const c_void, len: u64) -> c_int;
    }
}

// ---------------------------------------------------------------------------
// `Vm` — typed handle around `*mut vm_t`. All methods are safe FFI dispatch.
// ---------------------------------------------------------------------------
#[derive(Copy, Clone)]
struct Vm(*mut c_void);

impl Vm {
    fn current() -> Self {
        // SAFETY: read_current_vm() only reads the per-CPU `tp` register
        // and the resulting `cpu_local`/`thread` pointers with interrupts
        // disabled around the read; see its SAFETY doc.
        Self(unsafe { read_current_vm() })
    }

    fn mmap(self, addr: u64, length: usize, prot: i32, flags: i32, fd: i32, offset: u64) -> u64 {
        ffi::vm_mmap(self.0, addr, length, prot, flags, fd, offset)
    }
    fn munmap(self, addr: u64, length: usize) -> i32 {
        ffi::vm_munmap(self.0, addr, length)
    }
    fn mprotect(self, addr: u64, size: usize, prot: i32) -> i32 {
        ffi::vm_mprotect(self.0, addr, size, prot)
    }
    fn mremap(self, old_addr: u64, old_size: usize, new_size: usize, flags: i32, new_addr: u64) -> u64 {
        ffi::vm_mremap(self.0, old_addr, old_size, new_size, flags, new_addr)
    }
    fn msync(self, addr: u64, size: usize, flags: i32) -> i32 {
        ffi::vm_msync(self.0, addr, size, flags)
    }
    fn mincore_chunk(self, addr: u64, size: usize, vec: &mut [u8]) -> i32 {
        ffi::vm_mincore(self.0, addr, size, vec.as_mut_ptr())
    }
    fn madvise(self, addr: u64, size: usize, advice: i32) -> i32 {
        ffi::vm_madvise(self.0, addr, size, advice)
    }
    fn copyout_bytes(self, dstva: u64, src: &[u8]) -> i32 {
            ffi::vm_copyout(self.0, dstva, src.as_ptr() as *const c_void, src.len() as u64)
    }
}

/// Read `current->vm`. Reads the per-CPU `tp` register with interrupts off
/// so the value of `tp` cannot change underneath us.
///
/// SAFETY: caller must ensure the kernel uses `tp` to hold the current
/// CPU's `cpu_local*` pointer (true for every context this runs in), and
/// that inline asm reading/toggling `sstatus`/`tp` is sound at the call
/// site (i.e. not called from a context where clobbering flags is unsafe).
unsafe fn read_current_vm() -> *mut c_void {
    let sstatus_before: u64;
    core::arch::asm!("csrr {0}, sstatus", out(reg) sstatus_before,
        options(nomem, nostack, preserves_flags));
    let sie_was_on = (sstatus_before & SSTATUS_SIE) != 0;
    if sie_was_on {
        core::arch::asm!("csrc sstatus, {0}", in(reg) SSTATUS_SIE,
            options(nomem, nostack, preserves_flags));
    }
    let tp: u64;
    core::arch::asm!("mv {0}, tp", out(reg) tp,
        options(nomem, nostack, preserves_flags));
    let cpu = tp as *mut cpu_local;
    let proc: *mut thread = (*cpu).proc_;
    let vm = if proc.is_null() {
        core::ptr::null_mut()
    } else {
        (*proc).vm as *mut c_void
    };
    if sie_was_on {
        core::arch::asm!("csrs sstatus, {0}", in(reg) SSTATUS_SIE,
            options(nomem, nostack, preserves_flags));
    }
    vm
}

// ---------------------------------------------------------------------------
// Syscall argument helpers.
// ---------------------------------------------------------------------------
fn arg_addr(n: i32) -> u64 {
    let mut v: u64 = 0;
    ffi::argaddr(n, &mut v);
    v
}
fn arg_int(n: i32) -> i32 {
    let mut v: c_int = 0;
    ffi::argint(n, &mut v);
    v
}

// ---------------------------------------------------------------------------
// Public C ABI — thin wrappers.
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn sys_mmap()-> u64 {
    Vm::current().mmap(arg_addr(0), arg_int(1) as usize,
                       arg_int(2), arg_int(3), arg_int(4), arg_addr(5))
}

#[no_mangle]
pub extern "C" fn sys_munmap()-> u64 {
    let length = arg_int(1);
    if length <= 0 { return neg_errno(EINVAL); }
    Vm::current().munmap(arg_addr(0), length as usize) as i64 as u64
}

#[no_mangle]
pub extern "C" fn sys_mprotect()-> u64 {
    let length = arg_int(1);
    if length <= 0 { return neg_errno(EINVAL); }
    Vm::current().mprotect(arg_addr(0), length as usize, arg_int(2)) as i64 as u64
}

#[no_mangle]
pub extern "C" fn sys_mremap()-> u64 {
    let old_size = arg_int(1);
    let new_size = arg_int(2);
    if old_size < 0 || new_size <= 0 { return neg_errno(EINVAL); }
    Vm::current().mremap(arg_addr(0), old_size as usize,
                         new_size as usize, arg_int(3), arg_addr(4))
}

#[no_mangle]
pub extern "C" fn sys_msync()-> u64 {
    let length = arg_int(1);
    if length <= 0 { return neg_errno(EINVAL); }
    Vm::current().msync(arg_addr(0), length as usize, arg_int(2)) as i64 as u64
}

#[no_mangle]
pub extern "C" fn sys_mincore()-> u64 {
    let addr = arg_addr(0);
    let length = arg_int(1);
    let vec_uaddr = arg_addr(2);
    if length <= 0 { return neg_errno(EINVAL); }

    let num_pages = pgroundup(length as u64) / PGSIZE;
    let mut kbuf = [0u8; 256];
    let vm = Vm::current();
    let mut done: u64 = 0;
    while done < num_pages {
        let chunk = core::cmp::min(num_pages - done, kbuf.len() as u64);
        let slice = &mut kbuf[..chunk as usize];
        let ret = vm.mincore_chunk(addr + done * PGSIZE,
                                   (chunk * PGSIZE) as usize, slice);
        if ret < 0 {
            return ret as i64 as u64;
        }
        if vm.copyout_bytes(vec_uaddr + done, slice) < 0 {
            return neg_errno(EFAULT);
        }
        done += chunk;
    }
    0
}

#[no_mangle]
pub extern "C" fn sys_madvise()-> u64 {
    let length = arg_int(1);
    if length <= 0 { return neg_errno(EINVAL); }
    Vm::current().madvise(arg_addr(0), length as usize, arg_int(2)) as i64 as u64
}