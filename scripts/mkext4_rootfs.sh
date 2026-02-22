#!/bin/bash
# mkext4_rootfs.sh - Create an ext4 root filesystem image from sysroot
#
# Usage: mkext4_rootfs.sh <output.img> <readme> <cpython_src_dir> <size_mb> <sysroot_dir>
#
# The entire sysroot tree is copied as the filesystem base.
# Additional runtime directories (dev, proc, tmp, sys) are created.
# Python standard library is copied from the cpython source tree.

set -e

OUTPUT="$1"
README="$2"
CPYTHON_SRC="$3"
SIZE_MB="${4:-64}"
SYSROOT_DIR="$5"

if [ -z "$OUTPUT" ] || [ -z "$README" ] || [ -z "$CPYTHON_SRC" ] || [ -z "$SYSROOT_DIR" ]; then
    echo "Usage: $0 <output.img> <readme> <cpython_src_dir> <size_mb> <sysroot_dir>" >&2
    exit 1
fi

STAGING=$(mktemp -d)
cleanup() {
    rm -rf "$STAGING"
}
trap cleanup EXIT

echo "mkext4_rootfs: staging directory: $STAGING"

# Copy the entire sysroot as the filesystem base, excluding static libraries
# and development headers (not needed at runtime, saves ~240MB)
echo "mkext4_rootfs: copying sysroot from $SYSROOT_DIR ..."
rsync -a --exclude='*.a' --exclude='*.o' --exclude='*.old' \
         --exclude='include/' --exclude='pkgconfig/' \
    "$SYSROOT_DIR/" "$STAGING/"
SYSROOT_SIZE=$(du -sh "$SYSROOT_DIR" | cut -f1)
STAGING_SIZE=$(du -sh "$STAGING" | cut -f1)
echo "mkext4_rootfs: sysroot: ${SYSROOT_SIZE} -> staging: ${STAGING_SIZE} (excludes .a/.o/include)"

# Ensure runtime directories exist (not part of sysroot)
mkdir -p "$STAGING/dev"
mkdir -p "$STAGING/proc"
mkdir -p "$STAGING/tmp"
mkdir -p "$STAGING/sys"

# Copy README to root
if [ -f "$README" ]; then
    cp "$README" "$STAGING/README.md"
fi

# Ensure ld-musl dynamic linker symlink is a *relative* link to libc.so.
# The sysroot may contain an absolute symlink (pointing to the host build path)
# which won't resolve inside the target rootfs.
if [ -f "$STAGING/lib/libc.so" ]; then
    ln -sf libc.so "$STAGING/lib/ld-musl-riscv64.so.1"
    echo "mkext4_rootfs: ensured ld-musl-riscv64.so.1 -> libc.so (relative)"
fi

# Copy Python standard library
mkdir -p "$STAGING/usr/local/lib/python3.12/lib-dynload"
PYLIB_SRC="${CPYTHON_SRC}/Lib"
if [ -d "$PYLIB_SRC" ]; then
    # Directories to exclude (saves ~35 MB)
    EXCLUDES=(
        test tests idlelib turtledemo ensurepip lib2to3
        tkinter __pycache__ distutils msilib
    )

    RSYNC_EXCLUDES=()
    for exc in "${EXCLUDES[@]}"; do
        RSYNC_EXCLUDES+=(--exclude="$exc")
    done

    rsync -a "${RSYNC_EXCLUDES[@]}" \
        --include='*/' \
        --include='*.py' \
        --include='*.pem' \
        --include='*.txt' \
        --include='*.cfg' \
        --exclude='*.pyc' \
        "$PYLIB_SRC/" "$STAGING/usr/local/lib/python3.12/"

    PYFILE_COUNT=$(find "$STAGING/usr/local/lib/python3.12/" -type f | wc -l)
    PYUSED=$(du -sh "$STAGING/usr/local/lib/python3.12/" | cut -f1)
    echo "mkext4_rootfs: Python stdlib: ${PYFILE_COUNT} files (${PYUSED})"
else
    echo "mkext4_rootfs: warning: Python Lib not found at ${PYLIB_SRC}" >&2
fi

BIN_COUNT=$(find "$STAGING/bin/" -type f 2>/dev/null | wc -l)
TOTAL_USED=$(du -sh "$STAGING" | cut -f1)
echo "mkext4_rootfs: ${BIN_COUNT} programs, total ${TOTAL_USED}"

# Create the ext2 image populated from staging (no root/mount required)
mke2fs -F -t ext2 -O ^dir_index -b 1024 -d "$STAGING" "$OUTPUT" "${SIZE_MB}m" >/dev/null 2>&1

echo "mkext4_rootfs: created ${OUTPUT} (${SIZE_MB} MB ext2)"

cleanup
trap - EXIT
