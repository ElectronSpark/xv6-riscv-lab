#!/bin/bash
# mkext4_pylib.sh - Create an ext4 image containing the Python standard library
#
# Usage: mkext4_pylib.sh <output.img> <cpython_src_dir> [size_mb]
#
# The image is laid out so that when mounted at /usr, Python finds its
# stdlib at /usr/local/lib/python3.12/ (matching prefix=/usr/local).

set -e

OUTPUT="$1"
CPYTHON_SRC="$2"
SIZE_MB="${3:-32}"  # default 32 MB

if [ -z "$OUTPUT" ] || [ -z "$CPYTHON_SRC" ]; then
    echo "Usage: $0 <output.img> <cpython_src_dir> [size_mb]" >&2
    exit 1
fi

PYLIB_SRC="${CPYTHON_SRC}/Lib"
if [ ! -d "$PYLIB_SRC" ]; then
    echo "Error: Python Lib directory not found at ${PYLIB_SRC}" >&2
    exit 1
fi

# Directories to exclude (saves ~35 MB)
EXCLUDES=(
    test
    tests
    idlelib
    turtledemo
    ensurepip
    lib2to3
    tkinter
    __pycache__
    distutils
    msilib
)

MOUNTPOINT=$(mktemp -d)
cleanup() {
    # Ensure unmounted even on error
    sudo umount "$MOUNTPOINT" 2>/dev/null || true
    rmdir "$MOUNTPOINT" 2>/dev/null || true
}
trap cleanup EXIT

# Create a sparse image file
dd if=/dev/zero of="$OUTPUT" bs=1M count=0 seek="$SIZE_MB" 2>/dev/null

# Format as ext2 (no journal, no metadata_csum, no dir_index/htree, no extents)
# This is the most compatible format for lwext4. Features like metadata_csum
# require >128-byte inodes, and dir_index can confuse linear directory iteration.
mke2fs -F -t ext2 -O ^dir_index -b 1024 "$OUTPUT" >/dev/null 2>&1

# Mount and populate
sudo mount -o loop "$OUTPUT" "$MOUNTPOINT"

# Create the target directory hierarchy
sudo mkdir -p "$MOUNTPOINT/local/lib/python3.12"

# Build rsync exclude arguments
RSYNC_EXCLUDES=()
for exc in "${EXCLUDES[@]}"; do
    RSYNC_EXCLUDES+=(--exclude="$exc")
done

# Copy Python stdlib (only .py files and essential data)
sudo rsync -a "${RSYNC_EXCLUDES[@]}" \
    --include='*/' \
    --include='*.py' \
    --include='*.pem' \
    --include='*.txt' \
    --include='*.cfg' \
    --exclude='*.pyc' \
    "$PYLIB_SRC/" "$MOUNTPOINT/local/lib/python3.12/"

# Report
FILE_COUNT=$(find "$MOUNTPOINT/local/lib/python3.12/" -type f | wc -l)
USED=$(du -sh "$MOUNTPOINT/local/lib/python3.12/" | cut -f1)
echo "Packed ${FILE_COUNT} files (${USED}) into ${OUTPUT} (${SIZE_MB} MB ext4)"

sudo umount "$MOUNTPOINT"
trap - EXIT
rmdir "$MOUNTPOINT"
