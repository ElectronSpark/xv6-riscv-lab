#!/bin/bash
#
# build_musl.sh — Build musl libc for xv6 RISC-V
#
# This script:
#   1. Downloads musl source (if not already present)
#   2. Overlays MINIMAL xv6-specific arch files (only syscall numbers + clone)
#   3. Configures and builds musl (static only)
#   4. Installs to a sysroot directory
#
# The key insight: musl's RISC-V ecall convention is identical to xv6's
# (a7=syscall#, a0-a5=args, a0=return, negative=negated errno).
# We only need to change:
#   - bits/syscall.h.in (xv6 uses different syscall numbers than Linux)
#   - kstat.h (xv6 stat layout differs from Linux kstat)
#   - src/thread/riscv64/__clone.s (xv6 clone takes a struct pointer)
# All other arch files use upstream musl defaults.
#
# Usage:
#   ./build_musl.sh [--clean] [--prefix=/path/to/sysroot]
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MUSL_VERSION="1.2.5"
MUSL_URL="https://musl.libc.org/releases/musl-${MUSL_VERSION}.tar.gz"
BUILD_DIR="${SCRIPT_DIR}/musl-build"
MUSL_SRC="${BUILD_DIR}/musl-${MUSL_VERSION}"
PREFIX="${SCRIPT_DIR}/musl-sysroot"
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
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Detect cross-compiler
if command -v riscv64-unknown-elf-gcc &>/dev/null; then
    CROSS_COMPILE="riscv64-unknown-elf-"
elif command -v riscv64-linux-gnu-gcc &>/dev/null; then
    CROSS_COMPILE="riscv64-linux-gnu-"
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

# 2. Override __clone.s (xv6 clone takes a struct pointer, not individual regs)
mkdir -p "${MUSL_SRC}/src/thread/riscv64"
cp "${SCRIPT_DIR}/arch/riscv64/__clone.s" "${MUSL_SRC}/src/thread/riscv64/__clone.s"

echo "Overlay applied (3 files)."

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
    --disable-shared \
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
echo "Installation complete."

echo ""
echo "=== musl libc for xv6 built successfully ==="
echo "Static library: ${PREFIX}/lib/libc.a"
echo "Headers:        ${PREFIX}/include/"
