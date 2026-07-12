#!/usr/bin/env bash
set -euo pipefail

kernel_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
top_root="$(cd -- "${kernel_root}/.." && pwd)"
tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/xv6-console-record-test.XXXXXX")"
trap 'rm -rf "${tmpdir}"' EXIT

cc -std=c11 -Wall -Wextra -Werror -DHOST_LIBC_PROGRAM \
    -iquote "${kernel_root}/kernel/inc" \
    "${kernel_root}/tests/console_record_host_test.c" \
    -o "${tmpdir}/console_record_host_test"
"${tmpdir}/console_record_host_test"

safe_rg="${top_root}/scripts/audit/safe-rg.sh"
cd "${top_root}"

require_source() {
    "${safe_rg}" -q "$1" "$2"
}

require_literal() {
    "${safe_rg}" -F -q "$1" "$2"
}

forbid_source() {
    if "${safe_rg}" -q "$1" "$2"; then
        printf 'forbidden source pattern: %s in %s\n' "$1" "$2" >&2
        exit 1
    fi
}

# Bind the fake model to the actual x86 ioctl, device dispatch/init, and all
# normal wire producers. These are source locks, not a kernel execution test.
require_source 'static int console_record_write_ioctl' kernel/kernel/console.c
require_source 'either_copyin\(&request, 1, \(uint64\)arg, sizeof\(request\)\)' kernel/kernel/console.c
require_source 'either_copyin\(record, 1, request.data_ptr, request.data_len\)' kernel/kernel/console.c
require_source 'mutex_lock_timed\(&console_wire_lock, CONSOLE_RECORD_LOCK_TIMEOUT_MS\)' kernel/kernel/console.c
require_source 'console_wire_emit_text_locked\(record, \(int\)request.data_len\)' kernel/kernel/console.c
require_source 'current->thread_group->euid != 0' kernel/kernel/console.c
require_source '#ifdef __x86_64__' kernel/kernel/console.c
require_source '#endif /\* __x86_64__ \*/' kernel/kernel/console.c
require_source 'mutex_unlock\(&console_wire_lock\)' kernel/kernel/console.c
require_source 'static int console_ioctl_common' kernel/kernel/console.c
require_source 'cmd == CONSOLE_IOC_WRITE_RECORD' kernel/kernel/console.c
require_source 'console_cdev.dev.ops.ioctl = console_dev_ioctl' kernel/kernel/console.c
require_source 'mutex_init\(&console_wire_lock, "console_wire"\)' kernel/kernel/console.c
require_source 'console_wire_emit_raw\(outbuf, olen\)' kernel/kernel/console.c
require_source 'console_wire_emit_text\(s, n\)' kernel/kernel/console.c
require_source 'console_wire_emit_raw\(buf, \(int\)n\)' kernel/kernel/console.c
require_source 'console_wire_note_emergency' kernel/kernel/console.c
require_source 'The timed acquisition above has exactly this one release path' kernel/kernel/console.c

# The actual staging path is host-glibc, but xv6's program convention keeps
# this command underscore-prefixed for the generated guest helper.
require_source 'consolerecord\)' scripts/build/build-linux-host-probes.sh
require_literal 'build_host_user_program "${name}" "_${name}"' scripts/build/build-linux-host-probes.sh
require_source 'open\("/dev/console", O_WRONLY\)' user/programs/consolerecord/consolerecord.c
require_literal "record[input_len++] = '\\n';" user/programs/consolerecord/consolerecord.c
require_source 'ioctl\(fd, CONSOLE_IOC_WRITE_RECORD, &request\)' user/programs/consolerecord/consolerecord.c
forbid_source '(^|[^[:alnum:]_])write[[:space:]]*\(' user/programs/consolerecord/consolerecord.c
forbid_source '(^|[^[:alnum:]_])(printf|fprintf|puts|fputs)[[:space:]]*\(' user/programs/consolerecord/consolerecord.c
