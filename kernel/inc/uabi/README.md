# UABI Headers

This directory contains **user-kernel ABI** definitions shared by syscall callers (user space) and syscall implementations (kernel).

## Purpose

Keep syscall-facing constants and structures in one place so both sides use identical layouts and values.

## What belongs here

- Syscall numbers (`syscall.h`)
- Syscall argument/result structures (`signal.h`, `stat.h`, `statfs.h`, `utsname.h`, `time.h`)
- Shared syscall flags/options (`mman.h`, `wait.h`, `poll.h`, `fcntl.h`, `termios.h`, `access.h`)

## Rules

- UABI headers define **stable external contracts**.
- Kernel-internal-only fields, helper macros, and private state do **not** belong here.
- If a syscall introduces new user-visible constants or structures, add them in this directory first, then include them from both kernel and user code.

## Include style

- Kernel code: `#include <uabi/...>`
- User code in this tree: `#include "uabi/..."`
