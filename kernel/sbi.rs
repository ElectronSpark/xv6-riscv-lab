//! SBI (Supervisor Binary Interface) — Rust port of `kernel/sbi.c`.
//!
//! S-mode kernels invoke SBI firmware (OpenSBI, etc.) services via the
//! RISC-V `ecall` instruction. `kernel/inc/sbi.h`'s `sbi_ecall()` was a
//! `static inline` (no external linkage — could not be called from
//! Rust) wrapping raw `ecall` inline asm per the SBI calling convention:
//! `a7`=extension ID, `a6`=function ID, `a0..a5`=call arguments, and on
//! return `a0`=error, `a1`=value. Reimplemented below with
//! `core::arch::asm!`, following the same register-allocation pattern
//! already used crate-wide in `kernel/machine/machine.rs` (bare CSR/asm
//! primitives, one `unsafe` block per unavoidable hazard, SAFETY
//! comment on each). The `sbi_ecall` asm block intentionally passes no
//! `options(...)`: SBI calls are opaque firmware calls that may read or
//! write arbitrary memory (e.g. `SBI_DBCN_WRITE`/`HSM_HART_START`) and
//! use the stack normally, matching the C version's blanket `"memory"`
//! clobber with no `nomem`/`nostack` equivalent.
//!
//! Every public `sbi_*` function below keeps the exact C name and
//! signature from `kernel/inc/sbi.h`, so its three call sites elsewhere
//! in the tree keep linking unchanged: `start_kernel.c`
//! (`sbi_probe_extensions`, `sbi_start_secondary_harts`), `console.c`
//! (`sbi_console_putchar`/`sbi_console_getchar`), `ipi/ipi.c`
//! (`sbi_send_ipi`), and `mm/vm.rs` (`sbi_remote_hfence_vma`, already
//! declared there as `unsafe extern "C" { pub safe fn
//! sbi_remote_hfence_vma(u64, u64, u64, u64) -> i64; }` — matches this
//! file's signature exactly, untouched by this port).
//!
//! `struct sbiret` is purely an internal implementation detail of
//! `sbi_ecall()` in the original C (never part of any function's public
//! signature — every public wrapper unpacks it into a `long`/`void`
//! return), so it's a private Rust struct here with no bindgen
//! involvement and no `wrapper.h` changes needed.

#![allow(dead_code)]

use core::ffi::{c_char, c_int};
use core::sync::atomic::{AtomicBool, Ordering};

use crate::machine;

unsafe extern "C" {
    pub safe fn printf(fmt: *const c_char, ...) -> c_int;
    pub safe fn __panic_start();
    pub safe fn __panic_end() -> !;
}

// ===========================================================================
// SBI return error codes (`kernel/inc/sbi.h`).
// ===========================================================================

const SBI_SUCCESS: i64 = 0;
const SBI_ERR_FAILED: i64 = -1;
const SBI_ERR_NOT_SUPPORTED: i64 = -2;
const SBI_ERR_INVALID_PARAM: i64 = -3;
const SBI_ERR_DENIED: i64 = -4;
const SBI_ERR_INVALID_ADDRESS: i64 = -5;
const SBI_ERR_ALREADY_AVAILABLE: i64 = -6;
const SBI_ERR_ALREADY_STARTED: i64 = -7;
const SBI_ERR_ALREADY_STOPPED: i64 = -8;

// ===========================================================================
// SBI extension IDs.
// ===========================================================================

const SBI_EXT_BASE: i64 = 0x10;
const SBI_EXT_TIMER: i64 = 0x54494D45;
const SBI_EXT_IPI: i64 = 0x735049;
const SBI_EXT_RFENCE: i64 = 0x52464E43;
const SBI_EXT_HSM: i64 = 0x48534D;
const SBI_EXT_SRST: i64 = 0x53525354;
const SBI_EXT_PMU: i64 = 0x504D55;
const SBI_EXT_DBCN: i64 = 0x4442434E;
const SBI_EXT_SUSP: i64 = 0x53555350;
const SBI_EXT_CPPC: i64 = 0x43505043;
const SBI_EXT_NACL: i64 = 0x4E41434C;
const SBI_EXT_STA: i64 = 0x535441;

// SBI extension indices (for use with sbi_ext_is_available). Plain
// `c_int` constants, not a Rust enum: `enum sbi_ext_id` is part of two
// public C signatures (`sbi_ext_is_available`, `sbi_ext_name`), and C
// enums are passed as plain `int` at the ABI boundary — no callers
// exist today (checked via full-tree grep), but keeping the parameter
// type as `c_int` preserves the exact calling convention `sbi.h`
// promises to any future C caller.
const SBI_EXT_ID_BASE: usize = 0;
const SBI_EXT_ID_TIMER: usize = 1;
const SBI_EXT_ID_IPI: usize = 2;
const SBI_EXT_ID_RFENCE: usize = 3;
const SBI_EXT_ID_HSM: usize = 4;
const SBI_EXT_ID_SRST: usize = 5;
const SBI_EXT_ID_PMU: usize = 6;
const SBI_EXT_ID_DBCN: usize = 7;
const SBI_EXT_ID_SUSP: usize = 8;
const SBI_EXT_ID_CPPC: usize = 9;
const SBI_EXT_ID_NACL: usize = 10;
const SBI_EXT_ID_STA: usize = 11;
const SBI_EXT_ID_COUNT: usize = 12;

// ===========================================================================
// SBI function IDs / states / reasons.
// ===========================================================================

const SBI_BASE_GET_SPEC_VERSION: i64 = 0;
const SBI_BASE_GET_IMPL_ID: i64 = 1;
const SBI_BASE_GET_IMPL_VERSION: i64 = 2;
const SBI_BASE_PROBE_EXT: i64 = 3;
const SBI_BASE_GET_MVENDORID: i64 = 4;
const SBI_BASE_GET_MARCHID: i64 = 5;
const SBI_BASE_GET_MIMPID: i64 = 6;

const SBI_TIMER_SET_TIMER: i64 = 0;

const SBI_IPI_SEND_IPI: i64 = 0;

const SBI_RFENCE_REMOTE_HFENCE_I: i64 = 0;
const SBI_RFENCE_REMOTE_HFENCE_VMA: i64 = 1;
const SBI_RFENCE_REMOTE_HFENCE_VMA_ASID: i64 = 2;
const SBI_RFENCE_REMOTE_HFENCE_GVMA_VMID: i64 = 3;
const SBI_RFENCE_REMOTE_HFENCE_GVMA: i64 = 4;
const SBI_RFENCE_REMOTE_HFENCE_VVMA_ASID: i64 = 5;
const SBI_RFENCE_REMOTE_HFENCE_VVMA: i64 = 6;

const SBI_HSM_HART_START: i64 = 0;
const SBI_HSM_HART_STOP: i64 = 1;
const SBI_HSM_HART_GET_STATUS: i64 = 2;
const SBI_HSM_HART_SUSPEND: i64 = 3;

const SBI_HSM_STATE_STARTED: i64 = 0;
const SBI_HSM_STATE_STOPPED: i64 = 1;
const SBI_HSM_STATE_START_PENDING: i64 = 2;
const SBI_HSM_STATE_STOP_PENDING: i64 = 3;
const SBI_HSM_STATE_SUSPENDED: i64 = 4;
const SBI_HSM_STATE_SUSPEND_PENDING: i64 = 5;
const SBI_HSM_STATE_RESUME_PENDING: i64 = 6;

const SBI_SRST_RESET: i64 = 0;

const SBI_SRST_TYPE_SHUTDOWN: u32 = 0;
const SBI_SRST_TYPE_COLD_REBOOT: u32 = 1;
#[allow(dead_code)]
const SBI_SRST_TYPE_WARM_REBOOT: u32 = 2;

const SBI_SRST_REASON_NONE: u32 = 0;
#[allow(dead_code)]
const SBI_SRST_REASON_SYSFAIL: u32 = 1;

const SBI_DBCN_WRITE_BYTE: i64 = 2;

const SBI_EXT_LEGACY_CONSOLE_PUTCHAR: i64 = 0x01;
const SBI_EXT_LEGACY_CONSOLE_GETCHAR: i64 = 0x02;

// ===========================================================================
// Raw ecall.
// ===========================================================================

/// SBI return value pair. Purely internal (see module doc) — every
/// public wrapper below unpacks this into a bare `long`/`void`.
struct SbiRet {
    error: i64,
    value: i64,
}

impl SbiRet {
    /// Mirrors the C `__SBI_RETVAL(ret)` macro: value on success, error
    /// code otherwise.
    #[inline(always)]
    fn retval(self) -> i64 {
        if self.error == SBI_SUCCESS {
            self.value
        } else {
            self.error
        }
    }

    /// Mirrors the C `__SBI_ERRNO(ret)` macro.
    #[inline(always)]
    fn errno(self) -> i64 {
        self.error
    }
}

/// Generic SBI ecall. Mirrors `kernel/inc/sbi.h`'s `sbi_ecall()` static
/// inline register-for-register: `a7`=ext, `a6`=fid, `a0..a5`=args,
/// `a0`/`a1`=(error, value) on return.
#[inline(always)]
fn sbi_ecall(
    ext: i64,
    fid: i64,
    arg0: u64,
    arg1: u64,
    arg2: u64,
    arg3: u64,
    arg4: u64,
    arg5: u64,
) -> SbiRet {
    let mut a0 = arg0;
    let mut a1 = arg1;
    // SAFETY: standard RISC-V SBI calling convention `ecall`. No
    // `options(...)` given — SBI firmware calls are opaque and may
    // touch arbitrary memory/state, matching the C version's blanket
    // `"memory"` clobber (the conservative/default asm! behavior here
    // is the direct equivalent).
    unsafe {
        core::arch::asm!(
            "ecall",
            inout("a0") a0,
            inout("a1") a1,
            in("a2") arg2,
            in("a3") arg3,
            in("a4") arg4,
            in("a5") arg5,
            in("a6") fid,
            in("a7") ext,
        );
    }
    SbiRet { error: a0 as i64, value: a1 as i64 }
}

// ===========================================================================
// Extension probing tables.
// ===========================================================================

const SBI_EXT_IDS: [i64; SBI_EXT_ID_COUNT] = [
    SBI_EXT_BASE, SBI_EXT_TIMER, SBI_EXT_IPI, SBI_EXT_RFENCE,
    SBI_EXT_HSM, SBI_EXT_SRST, SBI_EXT_PMU, SBI_EXT_DBCN,
    SBI_EXT_SUSP, SBI_EXT_CPPC, SBI_EXT_NACL, SBI_EXT_STA,
];

const SBI_EXT_OPTIONAL: [bool; SBI_EXT_ID_COUNT] = [
    false, true, false, false,
    false, true, true, true,
    true, true, true, true,
];

const SBI_EXT_NAMES: [&core::ffi::CStr; SBI_EXT_ID_COUNT] = [
    c"BASE", c"TIMER", c"IPI", c"RFENCE",
    c"HSM", c"SRST", c"PMU", c"DBCN",
    c"SUSP", c"CPPC", c"NACL", c"STA",
];

const UNKNOWN_EXT_NAME: &core::ffi::CStr = c"UNKNOWN";

/// Extension availability, filled in by `sbi_probe_extensions()`.
/// Mirrors the C `static int sbi_ext_available[SBI_EXT_ID_COUNT]`; all
/// writes happen during single-hart early boot before any other hart
/// starts running Rust code (same ordering guarantee the C original
/// relied on for its plain, non-atomic array), so `Relaxed` access
/// (rather than a lock) reproduces the original's actual concurrency
/// contract while staying free of `static mut`.
static SBI_EXT_AVAILABLE: [AtomicBool; SBI_EXT_ID_COUNT] = [
    AtomicBool::new(false), AtomicBool::new(false), AtomicBool::new(false), AtomicBool::new(false),
    AtomicBool::new(false), AtomicBool::new(false), AtomicBool::new(false), AtomicBool::new(false),
    AtomicBool::new(false), AtomicBool::new(false), AtomicBool::new(false), AtomicBool::new(false),
];

// ===========================================================================
// Base extension.
// ===========================================================================

#[no_mangle]
pub extern "C" fn sbi_get_spec_version() -> i64 {
    sbi_ecall(SBI_EXT_BASE, SBI_BASE_GET_SPEC_VERSION, 0, 0, 0, 0, 0, 0).retval()
}

#[no_mangle]
pub extern "C" fn sbi_get_impl_id() -> i64 {
    sbi_ecall(SBI_EXT_BASE, SBI_BASE_GET_IMPL_ID, 0, 0, 0, 0, 0, 0).retval()
}

#[no_mangle]
pub extern "C" fn sbi_get_impl_version() -> i64 {
    sbi_ecall(SBI_EXT_BASE, SBI_BASE_GET_IMPL_VERSION, 0, 0, 0, 0, 0, 0).retval()
}

#[no_mangle]
pub extern "C" fn sbi_probe_extension(extid: i64) -> i64 {
    sbi_ecall(SBI_EXT_BASE, SBI_BASE_PROBE_EXT, extid as u64, 0, 0, 0, 0, 0).retval()
}

#[no_mangle]
pub extern "C" fn sbi_get_mvendorid() -> i64 {
    sbi_ecall(SBI_EXT_BASE, SBI_BASE_GET_MVENDORID, 0, 0, 0, 0, 0, 0).retval()
}

#[no_mangle]
pub extern "C" fn sbi_get_marchid() -> i64 {
    sbi_ecall(SBI_EXT_BASE, SBI_BASE_GET_MARCHID, 0, 0, 0, 0, 0, 0).retval()
}

#[no_mangle]
pub extern "C" fn sbi_get_mimpid() -> i64 {
    sbi_ecall(SBI_EXT_BASE, SBI_BASE_GET_MIMPID, 0, 0, 0, 0, 0, 0).retval()
}

// ===========================================================================
// Timer extension.
// ===========================================================================

#[no_mangle]
pub extern "C" fn sbi_set_timer(stime_value: u64) {
    sbi_ecall(SBI_EXT_TIMER, SBI_TIMER_SET_TIMER, stime_value, 0, 0, 0, 0, 0);
}

// ===========================================================================
// IPI extension.
// ===========================================================================

#[no_mangle]
pub extern "C" fn sbi_send_ipi(hart_mask: u64, hart_mask_base: u64) -> i64 {
    sbi_ecall(SBI_EXT_IPI, SBI_IPI_SEND_IPI, hart_mask, hart_mask_base, 0, 0, 0, 0).errno()
}

// ===========================================================================
// Remote fence extension.
// ===========================================================================

#[no_mangle]
pub extern "C" fn sbi_remote_hfence_i(hart_mask: u64, hart_mask_base: u64) {
    sbi_ecall(SBI_EXT_RFENCE, SBI_RFENCE_REMOTE_HFENCE_I, hart_mask, hart_mask_base, 0, 0, 0, 0);
}

#[no_mangle]
pub extern "C" fn sbi_remote_hfence_vma(
    hart_mask: u64,
    hart_mask_base: u64,
    start_addr: u64,
    size: u64,
) -> i64 {
    sbi_ecall(
        SBI_EXT_RFENCE, SBI_RFENCE_REMOTE_HFENCE_VMA,
        hart_mask, hart_mask_base, start_addr, size, 0, 0,
    ).errno()
}

#[no_mangle]
pub extern "C" fn sbi_remote_hfence_vma_asid(
    hart_mask: u64,
    hart_mask_base: u64,
    start_addr: u64,
    size: u64,
    asid: u64,
) -> i64 {
    sbi_ecall(
        SBI_EXT_RFENCE, SBI_RFENCE_REMOTE_HFENCE_VMA_ASID,
        hart_mask, hart_mask_base, start_addr, size, asid, 0,
    ).errno()
}

#[no_mangle]
pub extern "C" fn sbi_remote_hfence_gvma_vmid(
    hart_mask: u64,
    hart_mask_base: u64,
    start_addr: u64,
    size: u64,
    vmid: u64,
) -> i64 {
    sbi_ecall(
        SBI_EXT_RFENCE, SBI_RFENCE_REMOTE_HFENCE_GVMA_VMID,
        hart_mask, hart_mask_base, start_addr, size, vmid, 0,
    ).errno()
}

#[no_mangle]
pub extern "C" fn sbi_remote_hfence_gvma(
    hart_mask: u64,
    hart_mask_base: u64,
    start_addr: u64,
    size: u64,
) -> i64 {
    sbi_ecall(
        SBI_EXT_RFENCE, SBI_RFENCE_REMOTE_HFENCE_GVMA,
        hart_mask, hart_mask_base, start_addr, size, 0, 0,
    ).errno()
}

#[no_mangle]
pub extern "C" fn sbi_remote_hfence_vvma_asid(
    hart_mask: u64,
    hart_mask_base: u64,
    start_addr: u64,
    size: u64,
    asid: u64,
) -> i64 {
    sbi_ecall(
        SBI_EXT_RFENCE, SBI_RFENCE_REMOTE_HFENCE_VVMA_ASID,
        hart_mask, hart_mask_base, start_addr, size, asid, 0,
    ).errno()
}

#[no_mangle]
pub extern "C" fn sbi_remote_hfence_vvma(
    hart_mask: u64,
    hart_mask_base: u64,
    start_addr: u64,
    size: u64,
) -> i64 {
    sbi_ecall(
        SBI_EXT_RFENCE, SBI_RFENCE_REMOTE_HFENCE_VVMA,
        hart_mask, hart_mask_base, start_addr, size, 0, 0,
    ).errno()
}

// ===========================================================================
// HSM (Hart State Management) extension.
// ===========================================================================

#[no_mangle]
pub extern "C" fn sbi_hart_start(hartid: u64, start_addr: u64, opaque: u64) -> i64 {
    sbi_ecall(SBI_EXT_HSM, SBI_HSM_HART_START, hartid, start_addr, opaque, 0, 0, 0).errno()
}

#[no_mangle]
pub extern "C" fn sbi_hart_stop() -> i64 {
    sbi_ecall(SBI_EXT_HSM, SBI_HSM_HART_STOP, 0, 0, 0, 0, 0, 0).errno()
}

#[no_mangle]
pub extern "C" fn sbi_hart_get_status(hartid: u64) -> i64 {
    sbi_ecall(SBI_EXT_HSM, SBI_HSM_HART_GET_STATUS, hartid, 0, 0, 0, 0, 0).retval()
}

#[no_mangle]
pub extern "C" fn sbi_hart_suspend(suspend_type: u32, resume_addr: u64, opaque: u64) -> i64 {
    sbi_ecall(
        SBI_EXT_HSM, SBI_HSM_HART_SUSPEND,
        suspend_type as u64, resume_addr, opaque, 0, 0, 0,
    ).errno()
}

// ===========================================================================
// System reset extension.
// ===========================================================================

#[no_mangle]
pub extern "C" fn sbi_system_reset(reset_type: u32, reset_reason: u32) {
    sbi_ecall(
        SBI_EXT_SRST, SBI_SRST_RESET,
        reset_type as u64, reset_reason as u64, 0, 0, 0, 0,
    );
    // Should not return.
    loop {
        machine::wfi();
    }
}

#[no_mangle]
pub extern "C" fn sbi_shutdown() {
    sbi_system_reset(SBI_SRST_TYPE_SHUTDOWN, SBI_SRST_REASON_NONE);
}

#[no_mangle]
pub extern "C" fn sbi_reboot() {
    sbi_system_reset(SBI_SRST_TYPE_COLD_REBOOT, SBI_SRST_REASON_NONE);
}

// ===========================================================================
// Extension probing.
// ===========================================================================

/// Panics with a message equivalent to the C `assert(..., "Required SBI
/// extension %s not available!", sbi_ext_names[i])` — same
/// `__panic_start`/`printf`/`__panic_end` sequence the `assert()` macro
/// in `kernel/inc/printf.h` expands to.
fn required_extension_missing(idx: usize) -> ! {
    __panic_start();
    printf(
        c"ASSERTION_FAILURE %s:%d: In function '%s':\n".as_ptr(),
        c"kernel/sbi.rs".as_ptr(),
        line!() as c_int,
        c"sbi_probe_extensions".as_ptr(),
    );
    printf(
        c"Required SBI extension %s not available!\n".as_ptr(),
        SBI_EXT_NAMES[idx].as_ptr(),
    );
    __panic_end();
}

#[no_mangle]
pub extern "C" fn sbi_probe_extensions() {
    printf(c"SBI extensions:\n".as_ptr());
    for i in 0..SBI_EXT_ID_COUNT {
        let result = sbi_probe_extension(SBI_EXT_IDS[i]);
        let available = result > 0;
        SBI_EXT_AVAILABLE[i].store(available, Ordering::Relaxed);
        printf(
            c"  %s: %s%s\n".as_ptr(),
            SBI_EXT_NAMES[i].as_ptr(),
            if available { c"AVAILABLE".as_ptr() } else { c"UNSUPPORTED".as_ptr() },
            if SBI_EXT_OPTIONAL[i] { c" (OPTIONAL)".as_ptr() } else { c"".as_ptr() },
        );
        if !(available || SBI_EXT_OPTIONAL[i]) {
            required_extension_missing(i);
        }
    }
}

#[no_mangle]
pub extern "C" fn sbi_ext_is_available(ext_id: c_int) -> c_int {
    if ext_id < 0 || ext_id as usize >= SBI_EXT_ID_COUNT {
        return 0;
    }
    SBI_EXT_AVAILABLE[ext_id as usize].load(Ordering::Relaxed) as c_int
}

#[no_mangle]
pub extern "C" fn sbi_ext_name(ext_id: c_int) -> *const c_char {
    if ext_id < 0 || ext_id as usize >= SBI_EXT_ID_COUNT {
        return UNKNOWN_EXT_NAME.as_ptr();
    }
    SBI_EXT_NAMES[ext_id as usize].as_ptr()
}

// ===========================================================================
// Convenience functions.
// ===========================================================================

#[no_mangle]
pub extern "C" fn sbi_error_str(error: i64) -> *const c_char {
    (match error {
        SBI_SUCCESS => c"success",
        SBI_ERR_FAILED => c"failed",
        SBI_ERR_NOT_SUPPORTED => c"not supported",
        SBI_ERR_INVALID_PARAM => c"invalid parameter",
        SBI_ERR_DENIED => c"denied",
        SBI_ERR_INVALID_ADDRESS => c"invalid address",
        SBI_ERR_ALREADY_AVAILABLE => c"already available",
        SBI_ERR_ALREADY_STARTED => c"already started",
        SBI_ERR_ALREADY_STOPPED => c"already stopped",
        _ => c"unknown error",
    })
    .as_ptr()
}

#[no_mangle]
pub extern "C" fn sbi_hart_state_str(state: i64) -> *const c_char {
    (match state {
        SBI_HSM_STATE_STARTED => c"started",
        SBI_HSM_STATE_STOPPED => c"stopped",
        SBI_HSM_STATE_START_PENDING => c"start pending",
        SBI_HSM_STATE_STOP_PENDING => c"stop pending",
        SBI_HSM_STATE_SUSPENDED => c"suspended",
        SBI_HSM_STATE_SUSPEND_PENDING => c"suspend pending",
        SBI_HSM_STATE_RESUME_PENDING => c"resume pending",
        _ => c"unknown state",
    })
    .as_ptr()
}

#[no_mangle]
pub extern "C" fn sbi_print_version() {
    let spec_ver = sbi_get_spec_version();
    let impl_id = sbi_get_impl_id();
    let impl_ver = sbi_get_impl_version();

    let major = (spec_ver >> 24) & 0x7f;
    let minor = spec_ver & 0xffffff;

    let impl_name: &core::ffi::CStr = match impl_id {
        0 => c"Berkeley Boot Loader (BBL)",
        1 => c"OpenSBI",
        2 => c"Xvisor",
        3 => c"KVM",
        4 => c"RustSBI",
        5 => c"Diosix",
        _ => c"Unknown",
    };

    printf(c"SBI specification v%d.%d\n".as_ptr(), major as c_int, minor as c_int);
    printf(
        c"SBI implementation: %s (id=%ld, version=0x%lx)\n".as_ptr(),
        impl_name.as_ptr(),
        impl_id,
        impl_ver,
    );
}

#[no_mangle]
pub extern "C" fn sbi_start_secondary_harts(start_addr: u64) {
    let boot_hart = machine::cpuid();

    printf(c"Starting secondary harts...\n".as_ptr());
    for i in 0..crate::bindings::NCPU as c_int {
        if i == boot_hart {
            continue;
        }

        let status = sbi_hart_get_status(i as u64);
        if status == SBI_HSM_STATE_STOPPED {
            let ret = sbi_hart_start(i as u64, start_addr, 0);
            if ret != SBI_SUCCESS
                && ret != SBI_ERR_ALREADY_AVAILABLE
                && ret != SBI_ERR_ALREADY_STARTED
            {
                printf(
                    c"hart %d: failed to start (%s)\n".as_ptr(),
                    i,
                    sbi_error_str(ret),
                );
            }
        }
    }
}

// ===========================================================================
// Early console (before UART init).
// ===========================================================================

#[no_mangle]
pub extern "C" fn sbi_console_putchar(c: c_int) {
    if SBI_EXT_AVAILABLE[SBI_EXT_ID_DBCN].load(Ordering::Relaxed) {
        // DBCN write byte — more reliable than the legacy call on some
        // platforms.
        sbi_ecall(SBI_EXT_DBCN, SBI_DBCN_WRITE_BYTE, c as u64, 0, 0, 0, 0, 0);
    } else {
        // Legacy console putchar, used for early boot before DBCN is
        // known to be available.
        sbi_ecall(SBI_EXT_LEGACY_CONSOLE_PUTCHAR, 0, c as u64, 0, 0, 0, 0, 0);
    }
}

/// # Safety
/// `s` must point to a valid NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn sbi_console_puts(s: *const c_char) {
    unsafe {
        let mut p = s;
        while *p != 0 {
            sbi_console_putchar(*p as c_int);
            p = p.add(1);
        }
    }
}

#[no_mangle]
pub extern "C" fn sbi_console_getchar() -> c_int {
    let ret = sbi_ecall(SBI_EXT_LEGACY_CONSOLE_GETCHAR, 0, 0, 0, 0, 0, 0, 0);
    if ret.error < 0 {
        return -1;
    }
    // Legacy call returns the character in the `error` field.
    ret.error as c_int
}
