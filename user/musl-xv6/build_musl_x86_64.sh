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

# Detect x86_64 compiler. Honor explicit CC/AR/RANLIB from the caller first.
if [[ -n "${CC:-}" ]]; then
    CC_CMD="${CC}"
    AR_CMD="${AR:-${CC_CMD%gcc}ar}"
    RANLIB_CMD="${RANLIB:-${CC_CMD%gcc}ranlib}"
elif command -v x86_64-xv6-linux-musl-gcc &>/dev/null; then
    CC_CMD="x86_64-xv6-linux-musl-gcc"
    AR_CMD="x86_64-xv6-linux-musl-ar"
    RANLIB_CMD="x86_64-xv6-linux-musl-ranlib"
elif command -v x86_64-linux-gnu-gcc &>/dev/null; then
    CC_CMD="x86_64-linux-gnu-gcc"
    AR_CMD="x86_64-linux-gnu-ar"
    RANLIB_CMD="x86_64-linux-gnu-ranlib"
elif command -v x86_64-elf-gcc &>/dev/null; then
    CC_CMD="x86_64-elf-gcc"
    AR_CMD="x86_64-elf-ar"
    RANLIB_CMD="x86_64-elf-ranlib"
elif command -v gcc &>/dev/null && [[ "$(uname -m)" == "x86_64" ]]; then
    CC_CMD="gcc"
    AR_CMD="ar"
    RANLIB_CMD="ranlib"
else
    echo "ERROR: No x86_64 compiler found."
    exit 1
fi

echo "=== Building musl libc for xv6 x86_64 ==="
echo "Compiler:       ${CC_CMD}"
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

# Apply xv6 overlay using the shared helper script
source "${SCRIPT_DIR}/apply_xv6_overlay.sh"
apply_xv6_overlay "x86_64" "${MUSL_SRC}" "${SCRIPT_DIR}/arch"

echo "Configuring musl..."
cd "${MUSL_SRC}"

CC="${CC_CMD}" \
AR="${AR_CMD}" \
RANLIB="${RANLIB_CMD}" \
CFLAGS="-fPIC -O2 -fno-stack-protector -mno-red-zone" \
./configure \
    --target=x86_64 \
    --prefix="${PREFIX}" \
    --syslibdir="${PREFIX}/lib" \
    --enable-shared \
    --enable-static \
    --disable-wrapper

echo "Configuration complete."

echo "Building musl..."
make -j"$(nproc)" 2>&1 | tail -20
echo "Build complete."

echo "Installing to ${PREFIX}..."
make install
# musl's make install creates an absolute symlink for the dynamic linker,
# e.g. ld-musl-x86_64.so.1 -> /abs/path/to/sysroot/lib/libc.so
# Fix it to be relative so it works inside the target rootfs.
if [ -L "${PREFIX}/lib/ld-musl-x86_64.so.1" ]; then
    ln -sf libc.so "${PREFIX}/lib/ld-musl-x86_64.so.1"
    echo "Fixed ld-musl-x86_64.so.1 symlink to relative (libc.so)"
fi
echo "Installation complete."

echo ""
echo "=== musl libc for xv6 x86_64 built successfully ==="
echo "Static library:  ${PREFIX}/lib/libc.a"
echo "Shared library:  ${PREFIX}/lib/libc.so"
echo "Headers:         ${PREFIX}/include/"
