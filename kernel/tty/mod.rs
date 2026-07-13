//! TTY subsystem.
//!
//! Phase 2 Wave 10 (see `docs/rustify/phase2_plan.md`) ported three
//! smaller tty pieces to Rust: `termios.c` -> [`termios`], `tty_dev.c`
//! -> [`tty_dev`], `pty.c` -> [`pty`]. Phase 2 Wave 11 ports the core
//! line discipline: `tty.c` -> [`tty`], `ptmx.c` (`/dev/ptmx` VFS glue)
//! -> [`ptmx`]. Phase 2 Wave 12 ports `session.c` (job control) ->
//! [`session`], completing the module -- `kernel/tty/` has no C sources
//! left (see `kernel/tty/CMakeLists.txt`, now deleted).

#[path = "termios.rs"]
pub mod termios;

#[path = "tty_dev.rs"]
pub mod tty_dev;

#[path = "pty.rs"]
pub mod pty;

#[path = "tty.rs"]
pub mod tty;

#[path = "ptmx.rs"]
pub mod ptmx;

#[path = "session.rs"]
pub mod session;
