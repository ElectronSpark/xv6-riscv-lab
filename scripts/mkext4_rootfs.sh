#!/bin/bash
# mkext4_rootfs.sh - Create an ext4 root filesystem image
#
# Usage: mkext4_rootfs.sh <output.img> <readme> <cpython_src_dir> [size_mb] [terminfo_dir] [files...]
#
# Creates an ext4 image containing:
#   /              - root directory with README.md
#   /bin/          - user programs (leading '_' stripped from filenames)
#   /dev/          - empty (populated at runtime)
#   /proc/         - empty (populated at runtime)
#   /tmp/          - empty
#   /sys/          - empty
#   /usr/local/lib/python3.12/ - Python standard library
#   /usr/share/terminfo/       - terminfo database (if provided)

set -e

OUTPUT="$1"
README="$2"
CPYTHON_SRC="$3"
SIZE_MB="${4:-64}"
TERMINFO_DIR="$5"
shift 5 || true  # remaining args are user program binaries

if [ -z "$OUTPUT" ] || [ -z "$README" ] || [ -z "$CPYTHON_SRC" ]; then
    echo "Usage: $0 <output.img> <readme> <cpython_src_dir> [size_mb] [terminfo_dir] [files...]" >&2
    exit 1
fi

STAGING=$(mktemp -d)
cleanup() {
    rm -rf "$STAGING"
}
trap cleanup EXIT

echo "mkext4_rootfs: staging directory: $STAGING"

# Create standard directory structure
mkdir -p "$STAGING/bin"
mkdir -p "$STAGING/dev"
mkdir -p "$STAGING/proc"
mkdir -p "$STAGING/tmp"
mkdir -p "$STAGING/sys"
mkdir -p "$STAGING/usr/local/lib/python3.12/lib-dynload"
mkdir -p "$STAGING/usr/share"

# Copy README
if [ -f "$README" ]; then
    cp "$README" "$STAGING/README.md"
fi

# Copy user programs (strip leading '_' from names)
for src in "$@"; do
    if [ ! -e "$src" ]; then
        echo "mkext4_rootfs: skipping missing: $src" >&2
        continue
    fi

    base="$(basename "$src")"
    name="${base#_}"

    cp "$src" "$STAGING/bin/$name"
    chmod 0755 "$STAGING/bin/$name" 2>/dev/null || true
done

# Copy Python standard library
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

# Copy terminfo database if provided
if [ -n "$TERMINFO_DIR" ] && [ -d "$TERMINFO_DIR" ]; then
    cp -r "$TERMINFO_DIR" "$STAGING/usr/share/terminfo"
    echo "mkext4_rootfs: terminfo database included"
fi

BIN_COUNT=$(find "$STAGING/bin/" -type f | wc -l)
TOTAL_USED=$(du -sh "$STAGING" | cut -f1)
echo "mkext4_rootfs: ${BIN_COUNT} programs, total ${TOTAL_USED}"

# Create the ext2 image populated from staging (no root/mount required)
mke2fs -F -t ext2 -O ^dir_index -b 1024 -d "$STAGING" "$OUTPUT" "${SIZE_MB}m" >/dev/null 2>&1

echo "mkext4_rootfs: created ${OUTPUT} (${SIZE_MB} MB ext2)"

cleanup
trap - EXIT
