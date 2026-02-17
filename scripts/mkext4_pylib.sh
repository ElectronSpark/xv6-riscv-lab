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
STAGING=$(mktemp -d)
cleanup() {
    rm -rf "$STAGING"
    rmdir "$MOUNTPOINT" 2>/dev/null || true
}
trap cleanup EXIT

# Create the target directory hierarchy in a staging dir
mkdir -p "$STAGING/local/lib/python3.12"

# Build rsync exclude arguments
RSYNC_EXCLUDES=()
for exc in "${EXCLUDES[@]}"; do
    RSYNC_EXCLUDES+=(--exclude="$exc")
done

# Create lib-dynload directory (Python's getpath needs it to resolve exec_prefix)
mkdir -p "$STAGING/local/lib/python3.12/lib-dynload"

# Copy Python stdlib (only .py files and essential data)
rsync -a "${RSYNC_EXCLUDES[@]}" \
    --include='*/' \
    --include='*.py' \
    --include='*.pem' \
    --include='*.txt' \
    --include='*.cfg' \
    --exclude='*.pyc' \
    "$PYLIB_SRC/" "$STAGING/local/lib/python3.12/"

# Disable readline interactive hook at startup — the import chain
# (rlcompleter → inspect → ast → re → enum) is too slow on virtio ext4
# and tab completion doesn't work on the telnet PTY anyway.
cat > "$STAGING/local/lib/python3.12/sitecustomize.py" <<'PYEOF'
import sys
sys.__interactivehook__ = lambda: None
PYEOF

# Report
FILE_COUNT=$(find "$STAGING/local/lib/python3.12/" -type f | wc -l)
USED=$(du -sh "$STAGING/local/lib/python3.12/" | cut -f1)

# Create the ext2 image populated from the staging directory (no root required)
# Use mke2fs -d to populate directly without mounting.
mke2fs -F -t ext2 -O ^dir_index -b 1024 -d "$STAGING" "$OUTPUT" "${SIZE_MB}m" >/dev/null 2>&1

echo "Packed ${FILE_COUNT} files (${USED}) into ${OUTPUT} (${SIZE_MB} MB ext2)"

cleanup
trap - EXIT
