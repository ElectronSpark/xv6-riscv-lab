//! Terminal I/O settings management.
//!
//! Rust port of `kernel/tty/termios.c` (Phase 2 Wave 10, see
//! `docs/rustify/phase2_plan.md`). Provides the default `termios`
//! initialization used by `tty_alloc()` (`kernel/tty/tty.c`, still C).
//!
//! # ABI note
//!
//! `struct termios` and every flag/index constant below mirror
//! `kernel/inc/uabi/termios.h` **exactly** — that header is userspace
//! ABI (user programs `#include` it directly, e.g. to build their own
//! `termios` and call `tcgetattr`/`tcsetattr`). The values here are not
//! free to drift from the C header; they were copied field-for-field
//! and cross-checked against it (see the module doc for the full
//! layout verification the porting worker ran).

use crate::bindings::termios;

// ===========================================================================
// `c_cc` indices (`kernel/inc/uabi/termios.h`).
// ===========================================================================

const VINTR: usize = 0;
const VQUIT: usize = 1;
const VERASE: usize = 2;
const VKILL: usize = 3;
const VEOF: usize = 4;
const VTIME: usize = 5;
const VMIN: usize = 6;
const VSTART: usize = 7;
const VSTOP: usize = 8;
const VSUSP: usize = 9;
const VEOL: usize = 10;

// ===========================================================================
// Flag bits (`kernel/inc/uabi/termios.h`).
// ===========================================================================

// c_iflag
const ICRNL: u32 = 0x0100;
const IXON: u32 = 0x0200;

// c_oflag
const OPOST: u32 = 0x0001;
const ONLCR: u32 = 0x0002;

// c_cflag
const CS8: u32 = 0x0030;
const CREAD: u32 = 0x0080;
const CLOCAL: u32 = 0x0800;

// c_lflag
const ISIG: u32 = 0x0001;
const ICANON: u32 = 0x0002;
const ECHO: u32 = 0x0008;
const ECHOE: u32 = 0x0010;
const ECHOK: u32 = 0x0020;

// Speed (baud rate).
const B115200: u32 = 115200;

/// Fill a `termios` struct with sane defaults.
///
/// Sets up canonical mode with echo, CR-to-NL input mapping, NL-to-CRNL
/// output mapping, standard control characters, and 115200 baud. This
/// matches the typical Linux console defaults.
///
/// # Safety
/// `t` must point to a valid, writable `termios`.
pub(crate) unsafe extern "C" fn termios_init_default(t: *mut termios) {
    // SAFETY: caller guarantees `t` is valid and writable (see fn doc).
    // `termios` is a plain-old-data struct (no padding-sensitive
    // invariants), so a field-by-field write fully initializes it.
    unsafe {
        // Input: map CR -> NL, enable XON/XOFF.
        (*t).c_iflag = ICRNL | IXON;

        // Output: post-process, map NL -> CRNL.
        (*t).c_oflag = OPOST | ONLCR;

        // Control: 8-bit chars, receiver on, local line.
        (*t).c_cflag = CS8 | CREAD | CLOCAL;

        // Local: canonical, echo, signals, erase echo.
        (*t).c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK;

        // Control characters.
        (*t).c_cc[VINTR] = 0x03; // ^C
        (*t).c_cc[VQUIT] = 0x1C; // ^\
        (*t).c_cc[VERASE] = 0x7F; // DEL
        (*t).c_cc[VKILL] = 0x15; // ^U
        (*t).c_cc[VEOF] = 0x04; // ^D
        (*t).c_cc[VTIME] = 0;
        (*t).c_cc[VMIN] = 1;
        (*t).c_cc[VSTART] = 0x11; // ^Q
        (*t).c_cc[VSTOP] = 0x13; // ^S
        (*t).c_cc[VSUSP] = 0x1A; // ^Z
        (*t).c_cc[VEOL] = 0x00;

        // Default baud rate.
        (*t).c_ispeed = B115200;
        (*t).c_ospeed = B115200;
    }
}
