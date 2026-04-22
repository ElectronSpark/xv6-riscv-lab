# TTY Subsystem Design and Status

## Overview

The TTY subsystem provides terminal semantics on top of character devices, including line discipline behavior, termios-style mode handling, and session/process-group aware signal generation.

This document tracks the **current** state of the subsystem in this repository (as of Feb 2026).

## Scope

- Terminal core data-path (`kernel/tty/tty.c`)
- Termios behavior and mode flags (`kernel/tty/termios.c`)
- Session and process-group integration (`kernel/tty/session.c`)
- Pseudo-terminal support (`kernel/tty/pty.c`)

## Current Status (Feb 2026)

### Implemented

- TTY core under `kernel/tty/`
- PTY support in-tree (`kernel/tty/pty.c`)
- Session-related helpers (`kernel/tty/session.c`)
- Termios-related handling (`kernel/tty/termios.c`)

### Partially Implemented / Evolving

- Full POSIX job-control behavior across all edge cases
- Complete signal delivery parity with Linux TTY semantics
- Broader compatibility coverage for uncommon ioctl combinations

### Planned

- Expanded conformance tests for canonical/raw transitions and PTY behavior
- Additional documentation of lock ordering and wakeup semantics
- Tightening behavior around process-group foreground control flows

## Architecture Notes

- **TTY core** manages input/output queues and dispatches line-discipline behavior.
- **Termios layer** interprets mode bits (canonical/raw-like behavior, echo, signal generation toggles).
- **Session/PGID layer** coordinates controlling-terminal ownership and foreground group checks.
- **PTY layer** bridges master/slave endpoints for shell-like and multiplexer workflows.

## Concurrency and Locking

- Data-path operations are serialized by TTY-internal synchronization primitives.
- Sleep/wakeup paths should avoid lock inversion with scheduler and VFS locks.
- Signal-related transitions should preserve consistent foreground-group checks.

## File Map

- `kernel/tty/tty.c`
- `kernel/tty/termios.c`
- `kernel/tty/session.c`
- `kernel/tty/pty.c`
- `kernel/tty/CMakeLists.txt`

## Notes

This is a living design/status document. When behavior changes, update this file to describe the **current** repository state rather than historical progression.
