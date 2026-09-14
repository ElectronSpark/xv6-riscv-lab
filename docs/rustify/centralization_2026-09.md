# Unsafe boundary centralization, September 2026

This continuation starts at `50e725b3`, after the
[previous ownership work](native_safety_2026-09.md). The implementation is
committed in `4ae7c7d8`, `c0e77688`, `5a41d0dd`, and `f4bb8adf`. The final
documentation commit also replaces obsolete port-history comments; it changes
no executable behavior.

The changes strengthen the contracts around unsafe operations: checked byte
views, allocation owners, request tokens, typed descriptor indices, and scoped
locking replace duplicated pointer helpers. Raw-pointer entry points that need
caller guarantees are explicitly unsafe. This continues the Rust ownership and
interface direction described in the previous report's Redox references.

## Ownership and parsing

[`SlabCacheRef<T>`](../../kernel/mm/mm_safe.rs) validates object size, first-slot
offset, stride and alignment before allocating `MaybeUninit<T>`. Its raw
constructor requires an externally pinned cache lifetime and does not create a
shared reference to concurrently mutable allocator metadata. `SlabBox` drops
initialized values before returning storage; unfinished allocations free only
their storage. The unused allocator that promised zeroed values but returned
uninitialized slab bytes, and unsafe shared page-metadata accessors exposed as
safe methods, were removed. Page-cache allocation uses the checked capability.

[`KernelBuffer`](../../kernel/mm/buffer.rs) owns up to one page of initialized
scratch bytes, exposes slices through `Deref`/`DerefMut`, and frees on drop.
Readlink, getdents and poll use it instead of manually paired allocation/free
paths. Getdents serializes directory position updates with the file mutex,
encodes records through byte slices, and initializes the entire aligned record.
The latter fixes disclosure of recycled heap bytes through record padding.
Negative counts, small buffers, failed copyout and cleanup have regressions in
[`rustvfstest`](../../user/rustvfstest.c).

[`FDT wire views`](../../kernel/dev/fdt/wire.rs) parse bounded slices into typed
tokens, nodes, properties, cell sequences and memory regions. The boot adapter
copies the validated blob into permanent early storage, preserving the lifetime
of exported register names. It replaces the allocated raw node tree, string
scans, linked indexes and pointer traversal. Native `Result` methods replace
integer status plus output-pointer APIs. The early allocation arena excludes
the original DTB before any allocation; rejected blobs are not read again after
allocation begins. Full physical memory accounting remains separate from that
temporary arena bound.

FDT fixes include root-child memory discovery, register offsets expressed in
32-bit cells, working phandle lookup, bounds on strings/tokens/reservations and
cell arithmetic, and complete ordering when distinct node names have equal
numeric unit addresses. Device enumeration retains ascending numeric unit
address order. Platform-record layouts remain unchanged.

## Shared block I/O

[`Bio::begin`](../../kernel/dev/bio.rs) checks the request layout and returns an
iterator of `BioPart` tokens. Each token owns a BIO header reference and one
completion obligation. The submitting caller still owns and pins the pages and
device; the BIO does not silently acquire or release their references. No
header-wide shared Rust reference is formed while completion fields can change.

The iterator retains a submission sentinel. Completion cannot publish until
both submission and every issued part finish. Atomic accounting selects one
final publisher for byte count, first error, callback and waiter wakeup. Its
reference pins the header through the callback and wakeup, even if another
owner releases its reference. Interrupted waits still drain I/O before pages
can be reused. A callback must not reset or resubmit the same request while its
old completion is being published.

BIO progress occupies the former 16-byte padding slot at offset 128; the
320-byte header and completion at offset 192 retain their asserted layouts.
Segment replacement validates the entire update before mutation and uses
nonwrapping totals. The pure transfer module rejects invalid block shifts,
including shifts that would turn 512 into zero despite `checked_shl` succeeding.

VirtIO, ramdisk and SDHCI consume the same iterator and completion interface.
This removes duplicate iterator/completion/wait helpers, including an iterator
boundary error, SDHCI's failure to advance beyond its first segment, and VirtIO
signalling the entire BIO complete after an individual segment.

[`VirtIO`](../../kernel/virtio_disk.rs) separates CPU-owned queue state under a
`SpinLock` from DMA storage. `DescriptorIndex` checks device-provided IDs before
indexing; a noncopyable `DescriptorChain` owns descriptor allocation and release.
The transport module contains volatile MMIO/DMA operations and ordering fences.
Three owned queue pages retain the existing allocation count. Completion
consumes its token outside the disk lock. Device status errors return EIO after
cleanup; malformed completion IDs remain fatal. Variable segment lengths that
are valid multiples of 512 bytes are supported.

SDHCI now proves a failed DMA transfer stopped before completing its token; an
unsuccessful command/data reset is fatal while the token remains held. Buffers
outside the full 32-bit DMA range, unsuitable cache-line ranges, or ending at an
SDMA boundary use PIO. PIO handles unaligned RAM addresses. Command/count and
address arithmetic live in a small safe, host-tested module. Hardware register
widths and cache-maintenance ordering remain explicit.

## Writeback correctness

The buffer cache waits only after successful submission, retains failed dirty
buffers, and keeps dirty buffers out of the recycling list on release, unpin
and failed flush. `BufCache::sync` returns `KResult` and uses a sleeping gate
for the entire pass. Without that gate, another mount's sync could observe an
empty dirty list while the first pass still had popped buffers in flight.
Current callers release buffer locks before acquiring the gate.

Journal code checks flush errors before publishing a commit header or clearing
the committed journal. Its existing interface cannot return those errors, so
it stops on failure. Synchronous writes likewise stop before treating failed
data as clean. Journal reads establish a nonnull locked buffer before accessing
its payload. These checks preserve replay information instead of advancing a
transaction after failed I/O; they are not a new crash-consistency proof or a
change to the device-cache flush protocol.

## Validation

| Check | Result and evidence |
| --- | --- |
| RISC-V release kernel and user images | [Build passed](evidence/centralization/kernel-build.log) |
| RISC-V Cargo with all features | [Passed](evidence/centralization/all-features.log) |
| Rust host tests | [109 passed](evidence/centralization/host-tests.log), including 45 added tests |
| Analyzer and regression-runner tests | [20 passed](evidence/centralization/python-tests.log) |
| Default kernel, 2 harts, 1 GiB | [Ten suites passed](evidence/centralization/kernel-regressions.log): VFS, UDP/network, mmap, signals, COW, symlinks, vfork, clone, devices and filesystem stress |
| Default kernel, 2 harts, 512 MiB | [58 quick tests and strict memory test passed](evidence/centralization/usertests-quick-and-memory.log) |
| BIO, RCU and workqueue feature build | [5 BIO, 10 RCU and 8 workqueue checks passed](evidence/centralization/bio-rcu-workqueue.log), followed by VFS and clone suites |

The five BIO checks exercise unequal split reads, one final callback, draining
device EIO, successful reads after EIO, metadata rejection without partial
mutation, and failed-write retention (the split read and callback are one
case). The buffer-cache case targets only block `u32::MAX` on the small QEMU
disk, then removes its synthetic cache entry. It also verifies that an error
releases the flush gate. BIO tests run synchronously in init's schedulable
context before writable user programs begin; other enabled boot tests do not
write the block cache. QEMU uses copied disk images and snapshot writes.

The 45 new host tests cover typed allocation ownership (8), initialized scratch
buffers (3), FDT parsing (17), BIO arithmetic/completion (6), descriptor ownership
(3), and SDHCI arithmetic (8). They compile production algorithms with small
host allocation/lock substitutes, not the entire kernel on the host.

To reproduce from the repository root with the local cross toolchain:

```sh
export TOOLPREFIX=/home/es/xv6/toolchain/build/bin/riscv64-unknown-elf- LAB=fs
cmake -S . -B build
cmake --build build -j8
cargo test --manifest-path kernel/Cargo.toml --target x86_64-unknown-linux-gnu
cargo check --manifest-path kernel/Cargo.toml --target riscv64gc-unknown-none-elf --all-features
python3 -m unittest discover -s scripts -p 'test_*.py'
python3 scripts/run_kernel_regressions.py usertests usermem --memory 512M
python3 scripts/run_kernel_regressions.py rustvfstest rustnettest mmaptest testsig cowtest symlinktest vforktest clonetest devtest stressfs
BIO_TEST=1 RCU_TEST=1 WORKQUEUE_TEST=1 cmake -S . -B build
cmake --build build -j8
python3 scripts/run_kernel_regressions.py rustvfstest clonetest \
  --boot-marker 'BIO TESTS: 5/5 PASSED' \
  --boot-marker 'WORKQUEUE TESTS: 8/8 PASSED' \
  --boot-marker 'RCU synchronization smoke tests: ALL TESTS PASSED'
cmake -S . -B build
cmake --build build -j8
```

The workspace is restored to the default feature configuration. Successful
runtime logs cover `f4bb8adf`'s executable changes; the following documentation
cleanup has no executable changes and is rebuilt separately.

## Measurement and remaining boundaries

[`unsafe-before.json`](evidence/centralization/unsafe-before.json) and
[`unsafe-after.json`](evidence/centralization/unsafe-after.json) use the same
`unsafe_analyzer.py` over all kernel Rust sources and cfg branches, including
host/runtime tests. The analyzer counts physical lines spanned by unsafe bodies
(including their comments and delimiters), recognizes `u!`, and does not expand
other macros or establish soundness.

| Metric | Before | After |
| --- | ---: | ---: |
| Rust source lines | 103,455 | 101,983 |
| Lines in unsafe bodies | 23,835 | 22,037 |
| Unsafe line share | 23.039% | 21.609% |
| Unsafe blocks | 3,556 | 3,311 |
| Unsafe functions | 942 | 893 |
| Unsafe keywords | 4,691 | 4,399 |
| `u!` sites | 198 | 198 |

There are 1,798 fewer lines in unsafe bodies. Most of that reduction comes from
the checked FDT representation; shared BIO code and allocation test backends
have more explicit unsafe scopes than their previous interfaces. Honest raw
contracts can increase a local count while improving safety. Removed stale
comments and added tests also affect the denominator.

SDHCI has compile checks and arithmetic tests, but QEMU does not expose the X1
controller: DMA reset behavior, cache maintenance and card enumeration require
board testing. Ramdisk uses the shared lifecycle but does not receive direct
I/O coverage from the default VirtIO-root regression suites. Firmware
mapping validity, existing raw BIO page/device ownership, allocator internals,
intrusive lists and other legacy pointer interfaces remain unsafe contracts.
The kernel is not fully sanitized, and the measured percentage is not a safety
score.
