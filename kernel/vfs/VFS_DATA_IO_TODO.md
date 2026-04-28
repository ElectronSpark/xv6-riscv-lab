# VFS Data I/O Separation TODO

This plan separates regular-file data transfer from filesystem drivers without
making the VFS core itself heavy. The VFS core should stay thin: syscall
semantics, file position, permissions, inode and file lifetime, mount
boundaries, and lock coordination remain its job.

Generic VFS/page-cache code should own regular-file data transfer: user and
kernel copying, page-cache population, BIO construction, BIO batching,
readahead, writeback, mmap faults, prefault, writepage, EOF clamping, and
zero-fill for holes. Filesystem drivers should provide logical-to-physical
block mapping, block allocation, metadata transactions, truncate, and exact
size commit. Block drivers remain BIO execution layers only.

## Architecture Boundaries

- [ ] Keep `vfs_fileread()`, `vfs_filewrite()`, vectored I/O syscalls, and mmap paths responsible for dispatch, access checks, file-position semantics, and calling generic helpers.
- [ ] Move regular-file byte transfer out of filesystem file operations into generic VFS/page-cache helpers.
- [ ] Keep directory, symlink, special-file, pipe, socket, device, and tmpfs-specific behavior on their existing specialized paths unless a generic helper naturally applies.
- [ ] Keep metadata I/O filesystem-owned: inode tables, bitmaps, directories, journals/logs, superblocks, orphan recovery, and filesystem-private caches.
- [ ] Keep block devices as pure BIO executors; do not push filesystem mapping, EOF, sparse-file, or user-copy behavior into `blkdev` or hardware drivers.
- [ ] Document the new layering in `VFS_DESIGN.md` after the implementation is stable.

## Generic Interfaces

- [x] Add a regular-file address-space interface to VFS-visible inode state or a pointed-to mapping object.
- [x] Define a mapped-extent type with logical file block, physical device sector, block count, device pointer, flags for hole/unwritten/allocated, and filesystem block size.
- [x] Add `map_blocks(inode, logical_block, max_blocks, flags, extents, max_extents)` for read-only logical-to-physical mapping.
- [x] Add `allocate_blocks(inode, logical_block, max_blocks, flags, extents, max_extents)` for write paths that must allocate missing blocks.
- [x] Add write lifecycle hooks: `begin_write`, `end_write`, and `commit_size`, with explicit ordering that allows xv6fs to begin a transaction before taking the inode lock.
- [x] Add optional hooks for `invalidate_mapping`, `truncate_mapping`, `sync_mapping_metadata`, and `flush_device_cache` where filesystem-specific metadata ordering requires them.
- [x] Record per-filesystem limits in the interface: maximum file size, maximum allocation bytes per transaction, supported sparse behavior, preferred folio order, and whether writeback may allocate blocks.
- [x] Return normal negative errno values from all mapping hooks and define partial-success rules for multi-extent mapping.
- [x] Add checked VFS wrapper helpers for address-space mapping, allocation, write lifecycle, metadata sync, truncate, and device-cache flush hooks.

## Generic Data I/O Implementation

- [ ] Add generic `read`, `write`, `readv`, and `writev` helpers for regular files backed by `pcache`.
- [ ] Make generic reads snapshot inode size safely, clamp to EOF, populate pcache folios, copy data to user/kernel buffers, and advance file position only on successful bytes.
- [ ] Make generic writes copy user/kernel data into pcache, allocate blocks before dirtying data, update inode size only after copied bytes are valid, and return correct partial-write counts.
- [ ] Add generic mmap `fault`, `prefault`, and `writepage` helpers that use the same page-cache and mapping hooks as normal file I/O.
- [ ] Implement sparse reads in generic code by zero-filling holes without submitting BIOs.
- [ ] Implement partial-block and partial-folio writes by reading existing data first unless the write fully covers the preserved range.
- [ ] Ensure `RWF_NOWAIT` uses pcache nowait helpers and returns `-EAGAIN` whenever allocation, blocking I/O, transaction start, or lock waiting would be required.
- [ ] Ensure generic helpers do not hold VFS inode locks across long BIO waits except where a filesystem explicitly requires it and documents why.
- [ ] Ensure close-time flush, `fflush`, `fsync`, `msync`, `munmap`, and shared mmap writeback all use the same dirty-page writeback path.

## xv6fs Conversion

- [ ] Keep `xv6fs_bmap_read()` as the read-only mapping primitive behind `map_blocks`.
- [ ] Keep `xv6fs_bmap()` as the allocation primitive behind `allocate_blocks`.
- [ ] Move xv6fs regular-file read/write/readv/writev byte loops to generic helpers.
- [ ] Move xv6fs pcache read/write/readahead/writeback BIO construction to generic mapping-based pcache code.
- [ ] Preserve xv6fs transaction order: begin transaction before inode locking for writes that may allocate or commit size.
- [ ] Preserve xv6fs log semantics for metadata allocation and inode size updates while keeping data blocks on the writeback path.
- [ ] Preserve xv6fs file-format limits such as maximum representable block count without reintroducing original xv6 limits that this OS has already outgrown.
- [ ] Keep xv6fs directory, inode, truncate, log, and metadata buffer-head paths filesystem-specific for this phase.

## ext4fs Conversion

- [ ] Implement ext4 `map_blocks` using lwext4 extent/block lookup only; do not copy regular-file data through lwext4 block-cache byte loops.
- [ ] Implement ext4 `allocate_blocks` using lwext4 allocation APIs while returning physical mappings for generic data I/O.
- [ ] Keep `ext4fs_lock` around lwext4 metadata operations and avoid holding it across unrelated user-copy work.
- [ ] Move ext4 regular-file read/write/readv/writev byte loops to generic helpers.
- [ ] Move ext4 pcache read/write/readahead/writeback BIO construction to generic mapping-based pcache code.
- [ ] Preserve exact ext4 inode size commits, including cases where lwext4 allocation temporarily rounds on-disk size to block boundaries.
- [ ] Preserve ext4 sparse-file and unwritten-block behavior by reporting holes/unwritten ranges through mapping flags.
- [ ] Keep ext4 directory, symlink, xattr stubs, mount, unmount, truncate, and metadata paths filesystem-specific.

## Performance Work

- [ ] Batch mapping before I/O so contiguous logical and physical extents can be merged into larger BIOs.
- [ ] Merge contiguous folios into one multi-vector BIO up to `BIO_MAX_VECS` and `BIO_MAX_SIZE`.
- [ ] Use `bio->batch` and one `blkdev_kick()` per readahead or writeback batch.
- [ ] Preserve full-folio overwrite optimization so generic writes skip read-before-write when all bytes in a folio are replaced.
- [ ] Add per-inode recent extent caching for sequential reads, writes, readahead, and mmap fault-around.
- [ ] Invalidate extent caches on truncate, block allocation, hole creation, inode eviction, and filesystem-specific metadata changes.
- [ ] Route large readahead and writeback batches through `iosched` where queue sorting helps, while keeping single synchronous metadata I/O direct.
- [ ] Keep folio sizing adaptive: use current disk-backed defaults, fall back on memory pressure, and avoid I/O amplification for sparse or fragmented files.
- [ ] Track performance counters for mapped extents, merged BIOs, hole zero-fills, read-before-write skips, writeback errors, and readahead hits.

## Correctness Work

- [ ] Keep lock ordering explicit: filesystem transaction, superblock where needed, inode, file lock, pcache/page locks, then BIO wait.
- [ ] Never mark a pcache folio uptodate until every required read BIO for that folio has completed successfully.
- [ ] Never mark a dirty folio clean until every write BIO covering its dirty data has completed successfully.
- [ ] On write failure, return bytes successfully copied and mapped before the failure; commit file size only for the successful range.
- [ ] Preserve EOF behavior for reads, writes, mmap faults, and writeback beyond current inode size.
- [ ] Preserve sparse zero-fill semantics for holes and beyond-EOF folio tails.
- [ ] Preserve partial-write semantics when user-copy fails midway through a block or folio.
- [ ] Ensure `fsync` writes dirty data first, then filesystem metadata, then block-device cache flush when available.
- [ ] Ensure truncate invalidates or clips cached folios before freeing blocks that could be reallocated elsewhere.
- [ ] Ensure concurrent read, write, truncate, mmap fault, writeback, and close paths cannot observe stale block mappings or freed inodes.

## Tests and Validation

- [x] Build the kernel after interface changes and keep warnings actionable.
- [ ] Run existing file tests: `bigfile`, `stressfs`, `grind`, `symlinktest`, `mmaptest`, `mmapbigfile`, `iovectest`, `dd`, `cp`, and `find`.
- [ ] Add tests for reads across holes, EOF, partial first blocks, partial last blocks, and multi-folio contiguous files.
- [ ] Add tests for writes crossing block, page, folio, indirect, double-indirect, and ext4 extent boundaries.
- [ ] Add tests proving full-folio overwrite does not issue a read BIO.
- [ ] Add tests proving partial overwrite preserves untouched bytes.
- [ ] Add mmap tests for shared writes through `msync`, `munmap`, close, remount, and reboot.
- [ ] Add `fsync` persistence tests that remount and verify exact data and exact file size.
- [ ] Add concurrent read/write/truncate/fsync stress tests using separate file descriptors.
- [ ] Run `iobench` before and after conversion; sequential read/write throughput should be no worse than the current optimized filesystem-local paths.
- [ ] Compare BIO counts before and after conversion; contiguous large reads and writes should issue equal or fewer BIOs.

## Validation Log

- [x] 2026-04-28: Booted `build-x86_64` in QEMU with `USE_KVM=1`, `DISPLAY_MODE=nographic`, `QEMU_NET=0`, 4 CPUs, and 2G memory; guest reached root shell, mounted ext4 root, and powered off cleanly.
- [x] 2026-04-28: Ran `iovectest` under KVM; all tests passed.
- [x] 2026-04-28: Ran `mmaptest` under KVM; all tests passed, including expected `mprotect` SIGSEGV subtest.
- [x] 2026-04-28: Ran `mmapbigfile` under KVM; all tests passed.
- [x] 2026-04-28: Ran `stressfs` under KVM; completed and returned to shell.
- [ ] 2026-04-28: Started `bigfile` under KVM and observed extended write progress, but interrupted it after it stopped producing output for about a minute; rerun to completion before marking the full existing-file-test suite done.
