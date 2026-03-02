#!/bin/bash
# mk_gpt_disk.sh - Create a GPT-partitioned disk image with ext4 + xv6fs partitions
#
# Usage: mk_gpt_disk.sh <output.img> <mkfs_tool> [size_mb]
#
# Creates a GPT disk with:
#   Partition 1: ext4  (half the usable space)
#   Partition 2: xv6fs (remaining space)
#
# The ext4 partition is formatted with mkfs.ext4.
# The xv6fs partition is formatted with the xv6 mkfs tool.

set -e

OUTPUT="$1"
MKFS_XV6="$2"
SIZE_MB="${3:-512}"

if [ -z "$OUTPUT" ] || [ -z "$MKFS_XV6" ]; then
    echo "Usage: $0 <output.img> <mkfs_tool> [size_mb]" >&2
    exit 1
fi

# Validate tools
for tool in sgdisk mkfs.ext4 dd; do
    if ! command -v "$tool" &>/dev/null; then
        echo "Error: $tool not found" >&2
        exit 1
    fi
done

if [ ! -x "$MKFS_XV6" ]; then
    echo "Error: mkfs tool not found or not executable: $MKFS_XV6" >&2
    exit 1
fi

TOTAL_SECTORS=$((SIZE_MB * 1024 * 1024 / 512))

echo "mk_gpt_disk: creating ${SIZE_MB}MB GPT image -> $OUTPUT"

# Create empty image
dd if=/dev/zero of="$OUTPUT" bs=512 count="$TOTAL_SECTORS" status=none

# Create GPT partition table using sgdisk
# GPT uses 34 sectors at start (protective MBR + GPT header + 128 entries)
# and 33 sectors at end (backup GPT header + entries)
# Usable range: sector 2048 .. (TOTAL_SECTORS - 34)
#
# We use sector 2048 as the first partition start (standard 1MB alignment)

FIRST_USABLE=2048
LAST_USABLE=$((TOTAL_SECTORS - 34))
USABLE_SECTORS=$((LAST_USABLE - FIRST_USABLE + 1))

# Split roughly in half
HALF=$((USABLE_SECTORS / 2))
# Align to 2048-sector boundaries
HALF=$(( (HALF / 2048) * 2048 ))

PART1_START=$FIRST_USABLE
PART1_END=$((PART1_START + HALF - 1))
PART2_START=$((PART1_END + 1))
PART2_END=$LAST_USABLE

PART1_SECTORS=$((PART1_END - PART1_START + 1))
PART2_SECTORS=$((PART2_END - PART2_START + 1))

echo "mk_gpt_disk: partition 1 (ext4):  sectors ${PART1_START}-${PART1_END} (${PART1_SECTORS} sectors, $((PART1_SECTORS * 512 / 1024 / 1024))MB)"
echo "mk_gpt_disk: partition 2 (xv6fs): sectors ${PART2_START}-${PART2_END} (${PART2_SECTORS} sectors, $((PART2_SECTORS * 512 / 1024 / 1024))MB)"

# Create GPT with two partitions
# Type 0FC63DAF-... = Linux filesystem (used for ext4)
# Type EBD0A0A2-... = Microsoft Basic Data (we'll use a custom type for xv6fs)
# Actually, let's use Linux filesystem for both — type doesn't matter for xv6
sgdisk \
    --clear \
    --new=1:${PART1_START}:${PART1_END} --typecode=1:8300 --change-name=1:"ext4" \
    --new=2:${PART2_START}:${PART2_END} --typecode=2:8300 --change-name=2:"xv6fs" \
    "$OUTPUT" >/dev/null 2>&1

echo "mk_gpt_disk: GPT partition table created"

# Format partition 1 as ext4
# Extract partition region, format it, write back
PART1_IMG=$(mktemp)
dd if=/dev/zero of="$PART1_IMG" bs=512 count="$PART1_SECTORS" status=none

mkfs.ext4 -q -b 4096 -L "data" "$PART1_IMG" >/dev/null 2>&1

# Write ext4 partition into the image at the correct offset
dd if="$PART1_IMG" of="$OUTPUT" bs=512 seek="$PART1_START" count="$PART1_SECTORS" conv=notrunc status=none
rm -f "$PART1_IMG"

echo "mk_gpt_disk: partition 1 formatted as ext4"

# Format partition 2 as xv6fs
# The xv6 mkfs creates a standalone image - we create it then dd into place
# xv6fs FSSIZE is in 1024-byte blocks (BSIZE=1024)
# We need to tell mkfs the size, but mkfs uses hardcoded FSSIZE from param.h
# So we create the image with mkfs, then truncate/pad to partition size and dd in

PART2_IMG=$(mktemp)
"$MKFS_XV6" "$PART2_IMG"

PART2_BYTES=$((PART2_SECTORS * 512))
MKFS_SIZE=$(stat -c%s "$PART2_IMG" 2>/dev/null || stat -f%z "$PART2_IMG")

if [ "$MKFS_SIZE" -gt "$PART2_BYTES" ]; then
    echo "mk_gpt_disk: WARNING: xv6fs image (${MKFS_SIZE} bytes) larger than partition ($PART2_BYTES bytes), truncating"
    truncate -s "$PART2_BYTES" "$PART2_IMG"
fi

# Write xv6fs into partition 2
dd if="$PART2_IMG" of="$OUTPUT" bs=512 seek="$PART2_START" conv=notrunc status=none
rm -f "$PART2_IMG"

echo "mk_gpt_disk: partition 2 formatted as xv6fs"

# Verify with sgdisk
echo "mk_gpt_disk: verifying GPT..."
sgdisk --verify "$OUTPUT" 2>&1 | head -5 || true

echo "mk_gpt_disk: done -> $OUTPUT ($SIZE_MB MB)"
