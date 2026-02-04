#!/bin/bash
set -e

IMG=/tmp/test_fs.img
MNT=/tmp/xv6_mnt
EXAMINE=/tmp/examine_xv6fs.py

# Helper functions
mount_fs() {
    mkdir -p $MNT
    /tmp/xv6fuse --image=$IMG $MNT &
    sleep 2
}

unmount_fs() {
    fusermount3 -u $MNT 2>/dev/null || true
    sleep 1
}

examine_fs() {
    echo ""
    python3 $EXAMINE $IMG
}

# Start fresh
cp /home/es/xv6/xv6-tmp/build/fs.img $IMG

echo "========================================"
echo "PHASE 0: INITIAL STATE"
echo "========================================"
examine_fs

echo ""
echo "========================================"
echo "PHASE 1: WRITE 3 BIG FILES"
echo "========================================"
mount_fs

# Create files with known patterns - smaller sizes to avoid timeouts
rm -f /tmp/checksums_all.txt

echo "Creating bigfile1.dat (100KB)..."
dd if=/dev/urandom of=$MNT/tmp/bigfile1.dat bs=1024 count=100 2>/dev/null
md5sum $MNT/tmp/bigfile1.dat | awk '{print "bigfile1.dat " $1}' >> /tmp/checksums_all.txt

echo "Creating bigfile2.dat (150KB)..."
dd if=/dev/urandom of=$MNT/tmp/bigfile2.dat bs=1024 count=150 2>/dev/null
md5sum $MNT/tmp/bigfile2.dat | awk '{print "bigfile2.dat " $1}' >> /tmp/checksums_all.txt

echo "Creating bigfile3.dat (200KB)..."
dd if=/dev/urandom of=$MNT/tmp/bigfile3.dat bs=1024 count=200 2>/dev/null
md5sum $MNT/tmp/bigfile3.dat | awk '{print "bigfile3.dat " $1}' >> /tmp/checksums_all.txt

echo ""
echo "Files created:"
ls -lh $MNT/tmp/bigfile*.dat

echo ""
echo "Checksums after Phase 1:"
cat /tmp/checksums_all.txt

unmount_fs

echo ""
echo "========================================"
echo "PHASE 1 EXAMINATION (after unmount)"
echo "========================================"
examine_fs

echo ""
echo "========================================"
echo "PHASE 2: WRITE 2 MORE BIG FILES"
echo "========================================"
mount_fs

echo "Creating bigfile4.dat (120KB)..."
dd if=/dev/urandom of=$MNT/tmp/bigfile4.dat bs=1024 count=120 2>/dev/null
md5sum $MNT/tmp/bigfile4.dat | awk '{print "bigfile4.dat " $1}' >> /tmp/checksums_all.txt

echo "Creating bigfile5.dat (180KB)..."
dd if=/dev/urandom of=$MNT/tmp/bigfile5.dat bs=1024 count=180 2>/dev/null
md5sum $MNT/tmp/bigfile5.dat | awk '{print "bigfile5.dat " $1}' >> /tmp/checksums_all.txt

echo ""
echo "All files now:"
ls -lh $MNT/tmp/bigfile*.dat

echo ""
echo "All checksums:"
cat /tmp/checksums_all.txt

unmount_fs

echo ""
echo "========================================"
echo "PHASE 2 EXAMINATION (after unmount)"
echo "========================================"
examine_fs

echo ""
echo "========================================"
echo "PHASE 3: READ AND VALIDATE ALL FILES"
echo "========================================"
mount_fs

echo "Validating file integrity..."
PASS=0
FAIL=0

while read name expected; do
    actual=$(md5sum $MNT/tmp/$name 2>/dev/null | awk '{print $1}')
    if [ "$actual" = "$expected" ]; then
        echo "  [OK] $name"
        PASS=$((PASS+1))
    else
        echo "  [FAIL] $name: expected $expected, got $actual"
        FAIL=$((FAIL+1))
    fi
done < /tmp/checksums_all.txt

echo ""
echo "Validation: $PASS passed, $FAIL failed"

echo ""
echo "========================================"
echo "PHASE 4: DELETE SOME FILES"
echo "========================================"
echo "Deleting bigfile1.dat and bigfile3.dat..."
rm $MNT/tmp/bigfile1.dat
rm $MNT/tmp/bigfile3.dat

echo "Remaining files:"
ls -lh $MNT/tmp/bigfile*.dat

# Update checksums (remove deleted files)
grep -v "bigfile1.dat\|bigfile3.dat" /tmp/checksums_all.txt > /tmp/checksums_remaining.txt

unmount_fs

echo ""
echo "========================================"
echo "PHASE 4 EXAMINATION (after deletions)"
echo "========================================"
examine_fs

echo ""
echo "========================================"
echo "PHASE 5: VALIDATE REMAINING FILES"
echo "========================================"
mount_fs

echo "Validating remaining files..."
PASS=0
FAIL=0

while read name expected; do
    actual=$(md5sum $MNT/tmp/$name 2>/dev/null | awk '{print $1}')
    if [ "$actual" = "$expected" ]; then
        echo "  [OK] $name"
        PASS=$((PASS+1))
    else
        echo "  [FAIL] $name: expected $expected, got $actual"
        FAIL=$((FAIL+1))
    fi
done < /tmp/checksums_remaining.txt

echo ""
echo "Validation: $PASS passed, $FAIL failed"

echo ""
echo "========================================"
echo "PHASE 6: DELETE ALL REMAINING FILES"
echo "========================================"
echo "Deleting all remaining bigfiles..."
rm -f $MNT/tmp/bigfile*.dat

echo "Files in /tmp after deletion:"
ls -la $MNT/tmp/

unmount_fs

echo ""
echo "========================================"
echo "FINAL EXAMINATION (all files deleted)"
echo "========================================"
examine_fs

echo ""
echo "========================================"
echo "TEST COMPLETE!"
echo "========================================"
