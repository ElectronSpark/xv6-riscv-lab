#!/usr/bin/env bash
# QEMU-driven ctest harness for in-kernel Rust runtime test suites
# (docs/rustify/test_port_plan.md Phase 3/4).
#
# Configures + builds the kernel in an ISOLATED build directory (never the
# main `build/`), with a given set of feature-gate env vars, boots QEMU
# with the project's standard boot-gate flags under a timeout, and greps
# the captured console log for a "must appear" marker and a "must not
# appear" failure marker.
#
# Usage:
#   run_qemu_test.sh <isolated_build_dir> <timeout_seconds> \
#       <must_match_ere> <must_not_match_ere_or_NONE> \
#       [-- NAME=VALUE ...]
#
# `NAME=VALUE` pairs (after `--`) are exported into this script's own
# environment before configuring *and* are what select the cargo feature
# via kernel/CMakeLists.txt's RUST_TEST_FEATURES (e.g. `RWLOCK_TEST=1`,
# `PCACHE_TEST=1`).
#
# Exit code: 0 if the kernel booted (`init: starting sh` appears exactly
# once), the must-match marker was found, and the must-not-match marker was
# absent; 1 otherwise (with a diagnostic dump of the captured log).
set -uo pipefail

if [ "$#" -lt 4 ]; then
    echo "usage: $0 <build_dir> <timeout_s> <must_match_ere> <must_not_match_ere|NONE> [-- NAME=VALUE ...]" >&2
    exit 2
fi

BUILD_DIR="$1"; shift
TIMEOUT_S="$1"; shift
MUST_MATCH="$1"; shift
MUST_NOT_MATCH="$1"; shift
if [ "${1:-}" = "--" ]; then
    shift
    for kv in "$@"; do
        export "${kv?}"
    done
fi

# ---------------------------------------------------------------------------
# MANDATORY build env (RUST_REWRITE.md "Working notes" -- Iteration 23/26
# poisoning lessons). Every script that can invoke `cmake` must set these
# itself, not rely on an inherited shell -- this is exactly the vector that
# poisoned two earlier waves' verification runs. Respect an already-exported
# override (e.g. a CI runner pinning a different but still-correct
# toolchain) but always fall back to the known-good defaults.
# ---------------------------------------------------------------------------
export TOOLPREFIX="${TOOLPREFIX:-/home/es/xv6/toolchain/build/bin/riscv64-unknown-elf-}"
export LAB="${LAB:-fs}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

mkdir -p "${BUILD_DIR}"
BUILD_DIR="$(cd "${BUILD_DIR}" && pwd)"

echo "[run_qemu_test] build_dir=${BUILD_DIR} TOOLPREFIX=${TOOLPREFIX} LAB=${LAB}"

# ---------------------------------------------------------------------------
# Configure + build (isolated dir -- never touches the main build/'s cache).
# ---------------------------------------------------------------------------
if ! cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" > "${BUILD_DIR}/configure.log" 2>&1; then
    echo "[run_qemu_test] FAIL: cmake configure failed" >&2
    tail -n 100 "${BUILD_DIR}/configure.log" >&2
    exit 1
fi

if ! cmake --build "${BUILD_DIR}" -j"$(nproc)" > "${BUILD_DIR}/build.log" 2>&1; then
    echo "[run_qemu_test] FAIL: cmake build failed" >&2
    tail -n 150 "${BUILD_DIR}/build.log" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Cache-poisoning guard (RUST_REWRITE.md: re-check C_COMPILER/BUILD_TYPE
# after every build, not just before it).
# ---------------------------------------------------------------------------
if ! grep -q "CMAKE_C_COMPILER:STRING=.*riscv64-unknown-elf-gcc" "${BUILD_DIR}/CMakeCache.txt"; then
    echo "[run_qemu_test] FAIL: cache poisoned -- wrong C compiler:" >&2
    grep "CMAKE_C_COMPILER:" "${BUILD_DIR}/CMakeCache.txt" >&2
    exit 1
fi
if grep -qE "^CMAKE_BUILD_TYPE:STRING=.+" "${BUILD_DIR}/CMakeCache.txt"; then
    echo "[run_qemu_test] FAIL: cache poisoned -- CMAKE_BUILD_TYPE not empty:" >&2
    grep "CMAKE_BUILD_TYPE:" "${BUILD_DIR}/CMakeCache.txt" >&2
    exit 1
fi

QEMU_BIN="$(command -v qemu-system-riscv64)"
if [ -z "${QEMU_BIN}" ]; then
    echo "[run_qemu_test] FAIL: qemu-system-riscv64 not found" >&2
    exit 1
fi

KERNEL_BIN="${BUILD_DIR}/kernel/xv6.bin"
INITRD_IMG="${BUILD_DIR}/fs.img"
FS0_IMG="${BUILD_DIR}/fs0.img"
if [ ! -f "${KERNEL_BIN}" ] || [ ! -f "${INITRD_IMG}" ] || [ ! -f "${FS0_IMG}" ]; then
    echo "[run_qemu_test] FAIL: expected build artifacts missing (${KERNEL_BIN}, ${INITRD_IMG}, ${FS0_IMG})" >&2
    exit 1
fi

LOG="${BUILD_DIR}/qemu_test.log"
rm -f "${LOG}"

# ---------------------------------------------------------------------------
# Standard boot flags (RUST_REWRITE.md "Working notes" -- Boot test
# invocation), same -machine/-bios/-m/-smp/-nographic/virtio-disk pinning as
# cmake/qemu.cmake's `qemu` target uses for the interactive boot gate. Both
# disks are mandatory: `virtio_disk_init()` crashes the kernel via
# IPI_REASON_CRASH if either virtio-mmio slot 0 or 1 is missing, independent
# of any test feature gate (empirically confirmed -- booting with only the
# first disk attached crashes looking for "virtio disk 1"). The netdev/e1000
# options `qemu.cmake` also passes are omitted here: none of the suites this
# harness drives touch the network, and running the exact same qemu binary
# ourselves (rather than going through `cmake --build --target qemu`, a
# custom target with no OUTPUT that a plain `timeout` cannot reliably signal
# all the way down to the qemu grandchild through `make`) gives direct
# process control for the timeout below.
# ---------------------------------------------------------------------------
timeout --kill-after=5 "${TIMEOUT_S}" "${QEMU_BIN}" \
    -machine virt \
    -bios default \
    -kernel "${KERNEL_BIN}" \
    -initrd "${INITRD_IMG}" \
    -m 1024M \
    -smp 2 \
    -nographic \
    -global virtio-mmio.force-legacy=false \
    -drive file="${INITRD_IMG}",if=none,format=raw,id=x0 \
    -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
    -drive file="${FS0_IMG}",if=none,format=raw,id=x1 \
    -device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1 \
    < /dev/null > "${LOG}" 2>&1
QEMU_RC=$?

# `timeout` killing QEMU once the suite finished printing its marker (we
# don't have an in-band "shut down now" signal from inside the guest) is
# the expected, successful path -- 124/137 from `timeout`/`timeout
# --kill-after` are not failures by themselves; only the log-content checks
# below decide pass/fail.
if [ "${QEMU_RC}" != 0 ] && [ "${QEMU_RC}" != 124 ] && [ "${QEMU_RC}" != 137 ]; then
    echo "[run_qemu_test] FAIL: qemu exited abnormally (rc=${QEMU_RC})" >&2
    tail -n 200 "${LOG}" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Re-check the cache one more time, after the QEMU run (RUST_REWRITE.md:
# "every wave's verification must re-check C_COMPILER/CMAKE_BUILD_TYPE in
# the cache after its last QEMU run, not just before its first").
# ---------------------------------------------------------------------------
if ! grep -q "CMAKE_C_COMPILER:STRING=.*riscv64-unknown-elf-gcc" "${BUILD_DIR}/CMakeCache.txt"; then
    echo "[run_qemu_test] FAIL: cache poisoned after run -- wrong C compiler" >&2
    exit 1
fi

BOOT_COUNT="$(grep -c "init: starting sh" "${LOG}" || true)"
if [ "${BOOT_COUNT}" != "1" ]; then
    echo "[run_qemu_test] FAIL: boot gate -- expected exactly 1 'init: starting sh', got ${BOOT_COUNT}" >&2
    tail -n 200 "${LOG}" >&2
    exit 1
fi

if ! grep -qE "${MUST_MATCH}" "${LOG}"; then
    echo "[run_qemu_test] FAIL: expected marker not found: /${MUST_MATCH}/" >&2
    tail -n 200 "${LOG}" >&2
    exit 1
fi

if [ "${MUST_NOT_MATCH}" != "NONE" ] && grep -qE "${MUST_NOT_MATCH}" "${LOG}"; then
    echo "[run_qemu_test] FAIL: forbidden marker found: /${MUST_NOT_MATCH}/" >&2
    grep -E "${MUST_NOT_MATCH}" "${LOG}" >&2
    exit 1
fi

echo "[run_qemu_test] PASS (log: ${LOG})"
exit 0
