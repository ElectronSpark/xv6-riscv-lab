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

# Detect RISC-V compiler. Honor explicit CC/AR/RANLIB from the caller first.
if [[ -n "${CC:-}" ]]; then
    CC_CMD="${CC}"
    AR_CMD="${AR:-${CC_CMD%gcc}ar}"
    RANLIB_CMD="${RANLIB:-${CC_CMD%gcc}ranlib}"
elif command -v riscv64-xv6-linux-musl-gcc &>/dev/null; then
    CC_CMD="riscv64-xv6-linux-musl-gcc"
    AR_CMD="riscv64-xv6-linux-musl-ar"
    RANLIB_CMD="riscv64-xv6-linux-musl-ranlib"
elif command -v riscv64-linux-gnu-gcc &>/dev/null; then
    CC_CMD="riscv64-linux-gnu-gcc"
    AR_CMD="riscv64-linux-gnu-ar"
    RANLIB_CMD="riscv64-linux-gnu-ranlib"
elif command -v riscv64-unknown-elf-gcc &>/dev/null; then
    CC_CMD="riscv64-unknown-elf-gcc"
    AR_CMD="riscv64-unknown-elf-ar"
    RANLIB_CMD="riscv64-unknown-elf-ranlib"
else
    echo "ERROR: No RISC-V cross compiler found."
    exit 1
fi

echo "=== Building musl libc for xv6 RISC-V ==="
echo "Compiler:       ${CC_CMD}"
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

# Apply xv6 overlay using the shared helper script
source "${SCRIPT_DIR}/apply_xv6_overlay.sh"
apply_xv6_overlay "riscv64" "${MUSL_SRC}" "${SCRIPT_DIR}/arch"

# Configure musl
echo "Configuring musl..."
cd "${MUSL_SRC}"

CC="${CC_CMD}" \
AR="${AR_CMD}" \
RANLIB="${RANLIB_CMD}" \
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
