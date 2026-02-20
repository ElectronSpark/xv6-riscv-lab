# ncurses Port Dependency Checklist (xv6 + local newlib)

This is the current dependency matrix for porting `ncurses` to xv6/newlib, updated after recent kernel/newlib integration work.

## Status Snapshot

- ✅ Kernel-side simple `poll(2)` syscall implemented and wired.
- ✅ xv6 header overlays now exported into local newlib install (`sys/dirent.h`, `sys/random.h`, `sys/utsname.h`, `sys/termios.h`, `sys/ioctl.h`).
- ✅ Compile probes for `<termios.h>` and `<sys/ioctl.h>` now pass against `build/newlib/install/.../include`.
- ✅ `SIGWINCH` confirmed present in installed `sys/signal.h`.
- ✅ Dedicated CMake compile-check target added: `newlib_header_compile_check`.
- ✅ Runtime `newlibtest` passes poll/select and TTY ioctl checks.
- ⚠️ `poll` is intentionally simple (good enough for bring-up, not full Linux semantics).

## 1) Hard blockers

### 1.1 Header blockers for termios/ioctl
- Previous issue:
  - Missing `sys/termios.h` and `sys/ioctl.h` in installed local newlib headers.
- Current state:
  - **Resolved**. Both headers are now installed in:
    - `build/newlib/install/riscv64-unknown-elf/include/sys/termios.h`
    - `build/newlib/install/riscv64-unknown-elf/include/sys/ioctl.h`
- Residual risk:
  - Keep these overlays synced with kernel constants/structures when TTY ABI evolves.

### 1.2 No remaining compile-time hard blocker identified
- Header-level blockers for basic ncurses configure/compile are currently cleared.
- Next likely blockers are feature/semantics gaps discovered during actual ncurses configure/build.

---

## 2) Runtime/kernel interfaces ncurses needs

## 2.1 TTY control (required)
- Needed by ncurses:
  - `tcgetattr`, `tcsetattr`, `cfmakeraw`/termios flags
  - `isatty`
  - `ioctl(TIOCGWINSZ)` and often `TIOCSWINSZ`
- Current status:
  - Kernel has TTY + ioctl support (`TCGETS/TCSETS*`, `TIOCGWINSZ`, `TIOCSWINSZ`, `TIOCGPGRP`, `TIOCSPGRP`).
  - Newlib syscall layer has `_isatty` and generic `ioctl` syscall bridge.
- Gap:
  - None for baseline termios/ioctl compile path.

## 2.2 Input waiting / timing (required)
- Needed by ncurses:
  - `select()` and/or `poll()` for non-blocking input and timeout behavior.
- Current status:
  - `select()` wrapper exists in newlib layer.
  - `poll()` now routes to kernel syscall (`SYS_poll`) instead of pure userspace emulation.
- Notes:
  - Implementation is a **simple event poll** suitable for interactive TTY bring-up.
  - Semantics are intentionally minimal and may differ from full POSIX/Linux in edge cases.

## 2.3 Signals (recommended)
- Needed by ncurses:
  - `sigaction`, `sigprocmask`; often `SIGWINCH` handling for resize.
- Current status:
  - Signal syscalls/wrappers exist in `newlib_syscalls.c`.
- Check:
  - Confirm `SIGWINCH` and related constants are available in active signal headers.

---

## 3) Filesystem + terminfo dependencies

## 3.1 Terminfo DB access (required unless hardcoding terminal)
- Needed by ncurses:
  - `open/read/close/lseek/stat/access`
  - Directory traversal (`opendir/readdir/closedir`) depending on database layout lookups.
- Current status:
  - Core file syscalls and `stat/lstat/fstat` are implemented.
  - `dirent` overlay is now installed in local newlib include/sys.
  - First ncurses cross probe succeeded in generating static libraries in:
    - `build/ncurses-probe/ncurses-6.5/build-xv6-nosgtty/lib/libncurses.a`
    - `build/ncurses-probe/ncurses-6.5/build-xv6-nosgtty/lib/libpanel.a`
    - `build/ncurses-probe/ncurses-6.5/build-xv6-nosgtty/lib/libmenu.a`
    - `build/ncurses-probe/ncurses-6.5/build-xv6-nosgtty/lib/libform.a`

## 3.2 Environment variables (required)
- Needed by ncurses:
  - `getenv("TERM")`, and environment interaction.
- Current status:
  - `getenv/setenv/unsetenv/putenv` implemented in xv6 newlib syscalls.

---

## 4) C library features

## 4.1 Required C/POSIX subset
- `malloc/free`, `stdio`, `string`, `ctype`, `errno`, `time/gettimeofday`, `stdarg`.
- Current status:
  - Available in your newlib + xv6 shim.

## 4.2 Wide-char / locale (for `ncursesw`)
- Needed by `ncursesw`:
  - `wchar.h`, `mb/wc` conversion, `wcwidth`, locale support.
- Current status:
  - Newlib includes these APIs.
- Note:
  - For first bring-up, prefer non-wide build (`--without-widec`) to reduce risk.

---

## 5) Build-time host dependencies (outside xv6 target)

When building ncurses source, host tools are usually required:
- `sh`, `make`, `sed`, `awk`, `grep`, `cmp`, `mkdir`, `install`
- Optional docs/tools: `tic`, `infocmp`, `toe` generation pipeline components

For cross build to xv6 target, plan to:
- Build only target libraries first (`libtinfo`/`libncurses`),
- Skip docs/manpages/tests,
- Avoid building host-running target executables in early stage.

---

## 6) Suggested first iteration strategy

1. **Run minimal ncurses configure (cross)**
   - Start with narrow, static, reduced feature set:
   - Disable extras not needed for first bring-up (Ada/C++ bindings, tests, manpages, db install tools for target runtime).
2. **Compile/link only core libs first**
  - Prioritize `libtinfo` + `libncurses` generation.
3. **Runtime smoke app**
   - Build a tiny `initscr()/printw()/getch()/endwin()` program.
4. **Then widen scope**
   - Add resize handling, color, and (later) wide-char support.

---

## 7) High-priority TODOs for this repo

- [x] Provide/install `sys/termios.h` in local newlib output.
- [x] Provide/install `sys/ioctl.h` in local newlib output.
- [x] Implement simple kernel-backed `poll` path and wire newlib wrapper to syscall.
- [x] Verify `SIGWINCH` macro availability for ncurses resize path.
- [x] Add a dedicated compile check target for `termios` + `ioctl` headers (currently done ad-hoc via shell probes).
- [x] Attempt first cross-configure/build of `ncurses` and record exact missing probes.
- [x] Add a minimal `ncurses` smoke test user program once library is built.

### Probe notes (first ncurses bring-up)

- Initial configure/build probe exposed these key gaps:
  - missing `<sgtty.h>` in target headers,
  - missing legacy baud constants (`B50..B4800`) in `sys/termios.h`,
  - missing `TIOCFLUSH` ioctl macro,
  - missing sgtty compatibility flags/macros (`RAW`, `CBREAK`, `XTABS`).
- Compatibility additions made for probe bring-up:
  - added `user/v6-newlib/newlib/libc/sys/xv6/sgtty.h`,
  - expanded `user/v6-newlib/newlib/libc/sys/xv6/sys/termios.h` speeds,
  - expanded `user/v6-newlib/newlib/libc/sys/xv6/sys/ioctl.h` flush macros.
- Minimal smoke source added:
  - `user/v6-newlib/newlib/libc/sys/xv6/src/misc/newlib_ncurses_smoke.c`


