#!/bin/bash
#
# build_musl.sh — Build musl libc for xv6 RISC-V
#
# This script:
#   1. Downloads musl source (if not already present)
#   2. Overlays MINIMAL xv6-specific arch files (only syscall numbers + clone)
#   3. Configures and builds musl (static + shared, with dynamic linker)
#   4. Installs to a sysroot directory
#
# The key insight: musl's RISC-V ecall convention is identical to xv6's
# (a7=syscall#, a0-a5=args, a0=return, negative=negated errno).
# We only need to change:
#   - bits/syscall.h.in (xv6 uses different syscall numbers than Linux)
#   - kstat.h (xv6 stat layout differs from Linux kstat)
#   - src/thread/riscv64/clone.s (xv6 clone takes a struct pointer)
#   - src/process/riscv64/vfork.s (use clone with CLONE_VM|CLONE_VFORK)
#   - src/stat/fstatat.c (remove zero-initialiser on kstat — xv6 compat)
# All other arch files use upstream musl defaults.
#
# Usage:
#   ./build_musl.sh [--clean] [--prefix=/path/to/sysroot]
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MUSL_VERSION="1.2.5"
MUSL_URL="https://musl.libc.org/releases/musl-${MUSL_VERSION}.tar.gz"
BUILD_DIR=""
PREFIX=""
CLEAN=0

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean)
            CLEAN=1
            shift
            ;;
        --prefix=*)
            PREFIX="${1#--prefix=}"
            shift
            ;;
        --build-dir=*)
            BUILD_DIR="${1#--build-dir=}"
            shift
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Default build dir and prefix if not specified
BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/musl-build}"
PREFIX="${PREFIX:-${SCRIPT_DIR}/sysroot}"
MUSL_SRC="${BUILD_DIR}/musl-${MUSL_VERSION}"

# Detect cross-compiler
# Prefer riscv64-linux-gnu- because its linker supports -shared (needed for
# libc.so and ld-musl dynamic linker).  riscv64-unknown-elf-ld does NOT
# support -shared so musl's shared build fails with the bare-metal toolchain.
if command -v riscv64-linux-gnu-gcc &>/dev/null; then
    CROSS_COMPILE="riscv64-linux-gnu-"
elif command -v riscv64-unknown-elf-gcc &>/dev/null; then
    CROSS_COMPILE="riscv64-unknown-elf-"
else
    echo "ERROR: No RISC-V cross compiler found."
    exit 1
fi

echo "=== Building musl libc for xv6 RISC-V ==="
echo "Cross compiler: ${CROSS_COMPILE}gcc"
echo "Build dir:      ${BUILD_DIR}"
echo "Install prefix: ${PREFIX}"
echo ""

# Clean if requested
if [[ $CLEAN -eq 1 ]]; then
    echo "Cleaning build directory..."
    rm -rf "${BUILD_DIR}"
fi

mkdir -p "${BUILD_DIR}"

# Download musl source
if [[ ! -d "${MUSL_SRC}" ]]; then
    echo "Downloading musl ${MUSL_VERSION}..."
    cd "${BUILD_DIR}"
    if [[ ! -f "musl-${MUSL_VERSION}.tar.gz" ]]; then
        wget -q "${MUSL_URL}" -O "musl-${MUSL_VERSION}.tar.gz"
    fi
    tar xzf "musl-${MUSL_VERSION}.tar.gz"
    echo "Download complete."
fi

# Apply MINIMAL overlay — only files that differ from Linux
echo "Applying xv6 overlay..."
ARCH_DIR="${MUSL_SRC}/arch/riscv64"

# 1. Override syscall numbers (the ONLY bits/ override needed)
cp "${SCRIPT_DIR}/arch/riscv64/bits/syscall.h.in" "${ARCH_DIR}/bits/syscall.h.in"

# 1.5 Override kstat layout (xv6 struct stat is compact, not Linux kstat)
cp "${SCRIPT_DIR}/arch/riscv64/kstat.h" "${ARCH_DIR}/kstat.h"

# 2. Override clone.s (xv6 clone takes a struct pointer, not individual regs)
#    musl's upstream clone.s defines __clone; we replace it with our xv6 version.
#    NOTE: the file must be named clone.s (not __clone.s) because musl's makefile
#    compiles clone.s via REPLACED_OBJS to produce __clone.lo.
mkdir -p "${MUSL_SRC}/src/thread/riscv64"
cp "${SCRIPT_DIR}/arch/riscv64/clone.s" "${MUSL_SRC}/src/thread/riscv64/clone.s"

# 3. Override vfork.s (use clone with CLONE_VM|CLONE_VFORK|SIGCHLD instead
#    of the non-existent SYS_vfork on riscv64)
mkdir -p "${MUSL_SRC}/src/process/riscv64"
cp "${SCRIPT_DIR}/arch/riscv64/vfork.s" "${MUSL_SRC}/src/process/riscv64/vfork.s"

# 4. Override fstatat.c (remove zero-initialiser on struct kstat for xv6 compat)
cp "${SCRIPT_DIR}/arch/riscv64/fstatat.c" "${MUSL_SRC}/src/stat/fstatat.c"

echo "Overlay applied (5 files)."

# 3. Disable brk — xv6 heap growth is unreliable when mmap regions are
#    placed adjacent to the heap VMA.  Force mallocng to use mmap exclusively.
echo "Disabling brk in malloc (mmap-only mode)..."
sed -i 's/#define brk(p) ((uintptr_t)__syscall(SYS_brk, p))/#define brk(p) ((uintptr_t)-1)/' \
    "${MUSL_SRC}/src/malloc/mallocng/glue.h"

# Configure musl
echo "Configuring musl..."
cd "${MUSL_SRC}"

CC="${CROSS_COMPILE}gcc" \
AR="${CROSS_COMPILE}ar" \
RANLIB="${CROSS_COMPILE}ranlib" \
CFLAGS="-march=rv64gc -mabi=lp64d -mcmodel=medany -fPIC -O2 -fno-stack-protector" \
./configure \
    --target=riscv64 \
    --prefix="${PREFIX}" \
    --syslibdir="${PREFIX}/lib" \
    --enable-shared \
    --enable-static \
    --disable-wrapper

echo "Configuration complete."

# Build
echo "Building musl (this may take a few minutes)..."
make -j"$(nproc)" 2>&1 | tail -20
echo "Build complete."

# Install
echo "Installing to ${PREFIX}..."
make install
# musl's make install creates an absolute symlink for the dynamic linker,
# e.g. ld-musl-riscv64.so.1 -> /abs/path/to/sysroot/lib/libc.so
# Fix it to be relative so it works inside the target rootfs.
if [ -L "${PREFIX}/lib/ld-musl-riscv64.so.1" ]; then
    ln -sf libc.so "${PREFIX}/lib/ld-musl-riscv64.so.1"
    echo "Fixed ld-musl-riscv64.so.1 symlink to relative (libc.so)"
fi
echo "Installation complete."

echo ""
echo "=== musl libc for xv6 built successfully ==="
echo "Static library:  ${PREFIX}/lib/libc.a"
echo "Shared library:  ${PREFIX}/lib/libc.so"
echo "Dynamic linker:  ${PREFIX}/lib/ld-musl-riscv64.so.1"
echo "Headers:         ${PREFIX}/include/"
