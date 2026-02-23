#!/bin/bash
#
# build_musl_x86_64.sh — Build musl libc for xv6 x86_64
#
# Analogous to build_musl.sh but for x86_64 target.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MUSL_VERSION="1.2.5"
MUSL_URL="https://musl.libc.org/releases/musl-${MUSL_VERSION}.tar.gz"
BUILD_DIR=""
PREFIX=""
CLEAN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean) CLEAN=1; shift ;;
        --prefix=*) PREFIX="${1#--prefix=}"; shift ;;
        --build-dir=*) BUILD_DIR="${1#--build-dir=}"; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/musl-build-x86_64}"
PREFIX="${PREFIX:-${SCRIPT_DIR}/sysroot}"
MUSL_SRC="${BUILD_DIR}/musl-${MUSL_VERSION}"

# Detect x86_64 cross-compiler
if command -v x86_64-linux-gnu-gcc &>/dev/null; then
    CROSS_COMPILE="x86_64-linux-gnu-"
elif command -v x86_64-elf-gcc &>/dev/null; then
    CROSS_COMPILE="x86_64-elf-"
elif command -v gcc &>/dev/null && [[ "$(uname -m)" == "x86_64" ]]; then
    CROSS_COMPILE=""
else
    echo "ERROR: No x86_64 compiler found."
    exit 1
fi

echo "=== Building musl libc for xv6 x86_64 ==="
echo "Cross compiler: ${CROSS_COMPILE}gcc"
echo "Build dir:      ${BUILD_DIR}"
echo "Install prefix: ${PREFIX}"
echo ""

if [[ $CLEAN -eq 1 ]]; then
    echo "Cleaning build directory..."
    rm -rf "${BUILD_DIR}"
fi

mkdir -p "${BUILD_DIR}"

if [[ ! -d "${MUSL_SRC}" ]]; then
    echo "Downloading musl ${MUSL_VERSION}..."
    cd "${BUILD_DIR}"
    if [[ ! -f "musl-${MUSL_VERSION}.tar.gz" ]]; then
        wget -q "${MUSL_URL}" -O "musl-${MUSL_VERSION}.tar.gz"
    fi
    tar xzf "musl-${MUSL_VERSION}.tar.gz"
    echo "Download complete."
fi

echo "Applying xv6 x86_64 overlay..."
ARCH_DIR="${MUSL_SRC}/arch/x86_64"

# Override syscall numbers
cp "${SCRIPT_DIR}/arch/x86_64/bits/syscall.h.in" "${ARCH_DIR}/bits/syscall.h.in"

# Override kstat layout
cp "${SCRIPT_DIR}/arch/x86_64/kstat.h" "${ARCH_DIR}/kstat.h"

# Override syscall_arch.h
cp "${SCRIPT_DIR}/arch/x86_64/syscall_arch.h" "${ARCH_DIR}/syscall_arch.h"

# Override crt_arch.h
cp "${SCRIPT_DIR}/arch/x86_64/crt_arch.h" "${ARCH_DIR}/crt_arch.h"

# Override clone.s — overwrite upstream __clone.s
mkdir -p "${MUSL_SRC}/src/thread/x86_64"
cp "${SCRIPT_DIR}/arch/x86_64/clone.s" "${MUSL_SRC}/src/thread/x86_64/__clone.s"

# Remove any leftover clone.s that might conflict
rm -f "${MUSL_SRC}/src/thread/x86_64/clone.s"

# Override vfork.s
mkdir -p "${MUSL_SRC}/src/process/x86_64"
cp "${SCRIPT_DIR}/arch/x86_64/vfork.s" "${MUSL_SRC}/src/process/x86_64/vfork.s"

# Override fstatat.c (same fix as riscv)
cp "${SCRIPT_DIR}/arch/riscv64/fstatat.c" "${MUSL_SRC}/src/stat/fstatat.c"

echo "Overlay applied."

# Disable brk — force mmap-only malloc
echo "Disabling brk in malloc (mmap-only mode)..."
sed -i 's/#define brk(p) ((uintptr_t)__syscall(SYS_brk, p))/#define brk(p) ((uintptr_t)-1)/' \
    "${MUSL_SRC}/src/malloc/mallocng/glue.h" 2>/dev/null || true

echo "Configuring musl..."
cd "${MUSL_SRC}"

CC="${CROSS_COMPILE}gcc" \
AR="${CROSS_COMPILE}ar" \
RANLIB="${CROSS_COMPILE}ranlib" \
CFLAGS="-fPIC -O2 -fno-stack-protector -mno-red-zone" \
./configure \
    --target=x86_64 \
    --prefix="${PREFIX}" \
    --syslibdir="${PREFIX}/lib" \
    --disable-shared \
    --enable-static \
    --disable-wrapper

echo "Configuration complete."

echo "Building musl..."
make -j"$(nproc)" 2>&1 | tail -20
echo "Build complete."

echo "Installing to ${PREFIX}..."
make install
echo "Installation complete."

echo ""
echo "=== musl libc for xv6 x86_64 built successfully ==="
echo "Static library:  ${PREFIX}/lib/libc.a"
echo "Headers:         ${PREFIX}/include/"
