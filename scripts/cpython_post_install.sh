#!/bin/bash
# Copy shared libpython artifacts and extension modules into the sysroot.
# Usage: cpython_post_install.sh <cpython_build_dir> <sysroot_dir>

set -euo pipefail

CPYTHON_BUILD_DIR="${1:-}"
SYSROOT_DIR="${2:-}"

if [ -z "$CPYTHON_BUILD_DIR" ] || [ -z "$SYSROOT_DIR" ]; then
    echo "Usage: $0 <cpython_build_dir> <sysroot_dir>" >&2
    exit 1
fi

mkdir -p "$SYSROOT_DIR/lib"
for f in "$CPYTHON_BUILD_DIR"/libpython3.12*.so*; do
    if [ -f "$f" ] || [ -L "$f" ]; then
        cp -P "$f" "$SYSROOT_DIR/lib/"
    fi
done

if [ -f "$SYSROOT_DIR/lib/libpython3.12d.so.1.0" ] && [ ! -e "$SYSROOT_DIR/lib/libpython3.12d.so" ]; then
    ln -sf libpython3.12d.so.1.0 "$SYSROOT_DIR/lib/libpython3.12d.so"
fi

mkdir -p "$SYSROOT_DIR/lib/python3.12/lib-dynload"
for f in "$CPYTHON_BUILD_DIR"/Modules/*.cpython-*.so; do
    if [ -f "$f" ]; then
        cp -P "$f" "$SYSROOT_DIR/lib/python3.12/lib-dynload/"
    fi
done
