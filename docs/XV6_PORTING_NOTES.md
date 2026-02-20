# xv6 Porting Notes — Lessons Learned

This document captures hard-won knowledge about porting user-space programs
(especially interactive / readline-based ones) to xv6.  Consult it **before**
porting new software — the same issues will almost certainly recur.

---

## 1. stdio Buffering vs. UART Console

**Symptom**: Program appears to hang — no prompt, no command output, Ctrl-C is
the only way out.

**Root cause**: glibc / musl default to _line-buffered_ (`_IOLBF`) or
_fully-buffered_ (`_IOFBF`) stdout/stderr when the file descriptor is **not**
a terminal (`isatty()` returns 0).  On xv6 the console device does not
implement the `TIOCGWINSZ` / termios ioctls that `isatty()` checks, so libc
falls back to full buffering.  Characters written by readline (prompt echo,
completion candidates, etc.) pile up in the buffer and never reach the UART.

**Fix**: Force unbuffered I/O at initialisation time:

```c
setvbuf(stdout, NULL, _IONBF, 0);
setvbuf(stderr, NULL, _IONBF, 0);
```

**Where it's applied today**:

| Program | File | Notes |
|---------|------|-------|
| CPython | `user/v6-cpython/Modules/readline.c` ~L1442 | `setvbuf(sys_stdout, NULL, _IONBF, 0)` |
| dash    | `user/musl-xv6/compat/histedit_shim.c` `el_init()` | stdout **and** stderr |

> **Rule of thumb**: any program that does character-at-a-time terminal I/O on
> xv6 needs `_IONBF` on stdout (and usually stderr).

---

## 2. Custom `rl_getc_function` — raw `read()` instead of stdio

**Symptom**: readline echoes nothing or double-echoes, or blocks in unexpected
ways.

**Root cause**: readline's default `rl_getc()` uses `fread()` / `getc()`,
which again goes through stdio buffering.  On xv6 this interacts badly with
the UART driver.

**Fix**: Supply a custom `rl_getc_function` that calls `read(fd, &c, 1)`
directly:

```c
static int
xv6_rl_getc(FILE *fp)
{
    unsigned char c;
    for (;;) {
        int n = read(fileno(fp), &c, 1);
        if (n == 1) return (int)c;
        if (n == 0) return EOF;
        if (errno == EINTR) continue;
        return EOF;
    }
}
```

Then during init: `rl_getc_function = xv6_rl_getc;`

**Where it's applied today**:

| Program | File |
|---------|------|
| CPython | `user/v6-cpython/Modules/readline.c` — `xv6_readline_getc()` |
| dash    | `user/musl-xv6/compat/histedit_shim.c` — `el_rl_getc()` |

---

## 3. SIGINT Handling with readline

**Symptom**: Ctrl-C kills the shell (or the program) instead of cancelling the
current input line.

**Root cause**: By default readline installs its own SIGINT handler that
longjmps.  If the host program (e.g. dash) also manages signals, the two
conflict, often resulting in the process dying.

**Fix**: Disable readline's built-in signal handling and manage SIGINT
manually:

```c
rl_catch_signals  = 0;
rl_catch_sigwinch = 0;
```

Then install a custom SIGINT handler around the `readline()` call that sets a
flag and returns an empty line rather than calling `exit()` or `longjmp()`.

**Where it's applied today**:
- dash shim: `user/musl-xv6/compat/histedit_shim.c` — `el_sigint_handler()`

---

## 4. Kernel TTY Canonical Mode (ICANON) — Line Buffering

**Symptom**: Shell reads one character per `read()` call in canonical mode.
Parser does lookahead after the newline and blocks forever — the command is
never evaluated.

**Root cause**: The original `tty_input()` in `kernel/tty/tty.c` pushed each
character into the input pipe *immediately* — even in canonical mode.  POSIX
canonical mode requires characters to be accumulated in a line editing buffer
and only made available to `read()` as a complete line when a line terminator
(newline, VEOL, VEOF) arrives.

**Fix** (committed): Added `canon_buf[256]` / `canon_len` to `struct tty`
(`kernel/inc/tty/tty_types.h`).  `tty_input()` now:

1. Buffers characters in `canon_buf` instead of writing to the pipe.
2. VERASE removes the last character from `canon_buf`.
3. VKILL clears `canon_buf`.
4. VEOF flushes `canon_buf` to the pipe (zero-length write if empty → EOF).
5. Newline / VEOL flushes the complete line (including the terminator) to the
   pipe.  `read()` then returns the whole line at once.

Mode switches (TCSETS/TCSETSW/TCSETSF) flush any partial `canon_buf` before
applying the new termios.

**Impact**: Every program that uses canonical-mode terminal I/O (not just
dash) — e.g. `cat`, interactive scripts, any program that doesn't do its own
raw-mode line editing.

---

## 5. xv6fs File Permissions

**Symptom**: `command not found` for every binary, even though the file exists.

**Root cause**: xv6fs has no on-disk permission bits.  The VFS layer
synthesises permissions from the inode type via `xv6fs_type_to_mode()`.  If
regular files get mode `0644` (no execute bit), shells that check
`(st_mode & 0111)` before exec will reject them.

**Fix**: Return `S_IFREG | 0755` for `XV6FS_T_FILE` in
`kernel/vfs/xv6fs/xv6fs_private.h` → `xv6fs_type_to_mode()`.

---

## 6. FLUSHERR in dash

**Symptom**: Error messages from dash (e.g. "command not found") never appear;
program seems to silently fail.

**Root cause**: dash has a compile-time option `FLUSHERR` that, when enabled,
flushes stderr after writing error messages.  Without it, error output sits in
a buffer.

**Fix**: Add `-DFLUSHERR=1` to dash's CFLAGS in `user/CMakeLists.txt`.

---

## Quick Checklist for Porting a New Interactive Program

- [ ] Add `setvbuf(stdout, NULL, _IONBF, 0)` (and stderr) early in init
- [ ] If using readline: provide a custom `rl_getc_function` using raw `read()`
- [ ] If using readline: set `rl_catch_signals = 0` and handle SIGINT yourself
- [ ] Verify xv6fs returns appropriate permission bits for the binary type
- [ ] Ensure error-output flushing is enabled (program-specific flags)
