# Native Rust ownership and safety, September 2026

This wave replaces selected C-shaped Rust internals with owned values, checked
byte slices, enums, traits, and scoped guards. It also fixes kernel bugs exposed
by those changes. The comparison baseline is **`cd4a7e77`**. This is an account
of a bounded implementation and its evidence, not a claim that the entire
kernel is sanitized or that an unsafe-line percentage proves soundness.

## Design direction

Redox's [file context types](https://github.com/redox-os/kernel/blob/master/src/context/file.rs)
and [scheme interface](https://github.com/redox-os/kernel/blob/master/src/scheme/mod.rs)
were design references for expressing kernel resources and operations through
Rust ownership and interfaces. They are inspiration, not copied code, and this
work does not adopt Redox's architecture or make a numerical safety comparison
with it.

The practical boundary is between operations Rust can express safely and
operations that still depend on external invariants. Packet parsing, path
components, bounded name construction, and buffer editing can use slices and
ordinary values. DMA, user-address copies, intrusive lists, scheduling, and
existing raw ABIs still need explicit unsafe contracts. Moving an unsafe block
without strengthening its ownership or validation contract is not counted as a
design improvement.

## Implemented native patterns

| Area | Representation and contract |
| --- | --- |
| Packet storage | `Packet` owns a stable `NonNull<Mbuf>` allocation and a checked byte range. `Drop` frees it; raw driver handoff consumes ownership. |
| Receive queues | `MbufQueue` accepts and returns owned packets, keeps links private, and drains remaining packets on drop. |
| Protocols | `EtherType: TryFrom<u16>` and `ReceivedPacket` represent accepted wire values and parsed packet kinds; payloads are checked ranges into a slice. |
| Locks | `SpinLock<T>` requires `T: Send` for cross-hart sharing. Guards control data access and remain on the acquiring hart. |
| PTY allocation | `PtySlot::{Vacant, Reserved, Occupied(NonNull<PtyPair>)}` replaces a fabricated pointer used to mark reservation. |
| Paths | `KernelPath`, `ParentPath`, and `PathError` carry validated byte paths; traversal uses existing `IRef` ownership and `KResult`. |
| Socket I/O | `SocketFileOps: FileOps` uses native `usize`/`KResult` APIs and declares driver-managed I/O synchronization. |
| Channel waits | `ChannelWait: TqWait` makes the scheduler's lock-reacquisition contract explicit at the waiter-tree callback boundary. |
| Thread diagnostics | `AtomicName` stores atomic bytes; readers receive an owned, bounded `NameSnapshot` instead of borrowing mutable name storage. |
| Work callbacks | Execution snapshots metadata before invoking `WorkHandler`; the initial flags specify whether the handler or executor may reclaim the item. |
| RCU synchronization | Per-hart `Option<u64>` snapshots distinguish participants from excluded harts; completion requires a later quiescent publication from every participant. |
| Common helpers | Slice copying, `CStr`, const-generic name arrays, and Rust integer methods replace selected raw loops and helper calls. |

Small modules separate testable policy from machine access: `net/buffer.rs`,
`net/wire.rs`, `vfs/path.rs`, `sync/spinlock.rs`, and `backtrace/frame.rs`.
Compile-time layout assertions retain the packet and DMA descriptor geometry
required by existing drivers. These choices use Rust features where they encode
an invariant; introducing additional macro syntax is not an objective by itself.

### Packet ownership and protocol parsing

[`Packet`](../../kernel/net/buffer.rs) prevents accidental copying of the
self-referential `Mbuf`, bounds append/prepend/retain operations, and exposes
packet bytes through slices. Allocation initializes the backing storage in
place, avoiding a large temporary on the kernel stack. The unsafe `from_raw`
boundary consumes a driver-owned allocation and rejects invalid head/length
metadata. `into_raw` synchronizes that metadata and transfers ownership back to
the driver. Those functions still require a live allocation and exclusive
ownership; metadata checks cannot establish pointer validity.

Queue insertion consumes a packet, removal returns one, and dropping the queue
frees leftovers. Removing the last packet also clears the tail. The socket
table and receive queues have domain-specific, documented `Send`
implementations rather than making arbitrary raw pointers shareable.

[`wire.rs`](../../kernel/net/wire.rs) decodes network byte order with byte-array
conversions. It checks Ethernet, IPv4, UDP, and ARP sizes before reading fields,
checks IPv4 checksums and declared lengths, rejects unsupported fragmentation,
and separates UDP payload from Ethernet padding. Checksum arithmetic folds
incrementally, including odd-length input, so long input does not overflow an
accumulator. Packet construction uses checked slices and fixed arrays instead
of packed-header casts or C memory helpers.

The network remains a small IPv4/UDP implementation: IPv4 options,
fragment reassembly, a complete ARP cache, and validation of nonzero UDP
checksums are outside this wave. Rejecting an unsupported frame safely does not
implement that protocol feature.

### DMA lifecycle and socket behavior

[`e1000.rs`](../../kernel/e1000.rs) retains raw MMIO and DMA boundaries while
separating CPU ring state behind locks. Descriptor storage uses `UnsafeCell`
with volatile accesses for fields the device can modify. Initialization has an
unsafe contract for the mapped register aperture and publication order.
RISC-V I/O fences order descriptor-memory changes and MMIO publication.

Concrete fixes include clearing an old transmit-complete status on descriptor
reuse, rejecting out-of-range ring indices, validating receive length and
completion/error flags, and discarding unsupported multi-descriptor frames.
Receive allocates a replacement before transferring the old buffer to the
stack. Allocation failure rearms the current buffer; it does not install a null
replacement or free a buffer still available to DMA. Partial initialization
failure frees allocations already acquired. The X1 EMAC receive path receives
the same replacement-before-handoff correction and Ethernet/FCS minimum-size
validation; its hardware coverage is recorded separately below.

[`sysnet.rs`](../../kernel/sysnet.rs) and
[`file.rs`](../../kernel/vfs/file.rs) now share the actual `Socket` and
`MbufQueue` types. Construction initializes real lock and queue values. Endpoint
uniqueness is checked before attaching the file to the global table, fixing the
duplicate-connect cleanup path that could leave a freed file attached. Final
file release unlinks the socket and drops queued packets. Failed descriptor
allocation also releases the constructed file/socket.

Socket reads can sleep while another holder of the same file reference writes.
`IoSynchronization::Driver` avoids holding the file mutex for a blocking socket
operation; the driver protects its own queue. File flags are read or updated
under the file mutex. Dispatch occurs before creating an exclusive reference to
the whole shared file. The same rule applies to inode-less ioctl, stat, seek,
and truncate paths. Their existing errors and stat behavior are preserved.
This addresses a Rust aliasing issue as well as a full-duplex deadlock. Poll
readiness follows receive-queue contents instead of unconditionally reporting
the socket readable.

Read/write syscall lengths use checked signed-to-`usize` conversion. Socket
helpers return `KResult<usize>` and use the existing kernel/user copy routines.
The maximum UDP payload derives from both the active interface MTU and packet
storage capacity. For an MTU of 1500, the IPv4/UDP payload limit is 1472 bytes;
larger datagrams return `EMSGSIZE`. Missing device, allocation failure, and bad
copy addresses retain explicit error paths. Packet ownership cleans up copy
failures.

### Synchronization and scheduler repairs

The extracted [`SpinLock`](../../kernel/sync/spinlock.rs) only implements
`Sync` when the protected `T` is `Send`. This closes the prior generic API hole
that permitted a non-transferable value to cross harts. A guard's marker keeps
it from moving between harts, where releasing the lock would corrupt interrupt
bookkeeping. Constant construction replaces the safe shared-reference API that
could reinitialize a live lock. Raw-pointer state needed by real kernel tables
receives individual `Send` justifications based on allocation lifetimes and
locking; the generic restriction is not bypassed globally.

Real UDP traffic exposed recursive acquisition of the global channel sleep
lock. During the sleep handoff, unlocking the caller's spinlock could restore
interrupts while the global sleep lock was still held. A network interrupt
waking a channel then acquired the same lock. The scheduler now preserves the
caller's interrupt intent while keeping interrupts disabled across that handoff,
and restores the caller lock and saved state in the correct order.

A second issue involved signal-interrupted waits: the old callback could remove
an enqueued waiter before the global sleep lock had been reacquired.
[`ChannelWait`](../../kernel/proc/sched.rs) reacquires that lock before the tree
inspects or removes the waiter, while recognizing the no-context-switch path
where it remains held. The implementation uses the existing `TqWait` trait and
does not create a separate wait mechanism.

Fork pressure also exposed a panic when thread-group allocation failed after
the child had joined the process table and a vfork parent could already be
asleep. `PendingChild` now owns the reserved PID capacity and unfinished thread;
its destructor releases installed VM, filesystem, descriptor-table, and signal
references on failure. All resource allocation, including the thread group,
precedes publication. The remaining publication phase assigns the PID, fixes
group IDs, and links the child under one uninterrupted PID write lock, preserving
uniqueness across PID wraparound. Internal construction returns `KResult`.

Clone also rejects overflowing stack arithmetic before reserving resources and
captures the PID before waking the child. The latter avoids reading a
`CLONE_THREAD` child's storage after it could have exited and been reclaimed.
The PID reservation count's diagnostic load now uses the same atomic access
protocol as its updates. `clonetest` verifies stack-overflow rejection and child
exit statuses; the saved baseline accepts the overflowing stack and fails that
new regression.

### Callback ownership and RCU completion

[`WorkStructRef::execute`](../../kernel/proc/workqueue.rs) previously inspected
work-item flags after invoking its handler. The VFS file-release handler frees
the work item itself, and the timer handler frees an enclosing allocation, so
that inspection read freed storage. Execution now snapshots the pointer, flags,
and handler before invocation. With `FREE_AFTER_RUN`, the handler must leave the
allocation live and the executor reclaims it. Without that flag, the handler
may consume the allocation and the executor performs no subsequent access.
Changing flags inside the callback does not change that execution's ownership.
The production self-free handlers use default flags without `FREE_AFTER_RUN`;
the page-cache handler retains its embedded work item.

[`Rcu::synchronize_impl`](../../kernel/lock/rcu.rs) previously returned after a
fixed retry budget even when a grace period had not completed. A dynamic-head
allocation failure in `Rcu::call` could then invoke a reclamation callback while
earlier readers remained active. The strict waiter captures the active harts'
timestamps while pinned and requires a strictly later publication from each
participant. An active hart whose timestamp is zero must publish its first
quiescent state. The original caller hart is excluded only after confirming
that it has no active read-side section; later waiter migration does not make
an earlier reader appear on that excluded hart.

The waiter requires thread context, enabled interrupts, no spinlocks, and no
enclosing RCU read-side section. Invalid contexts panic instead of reclaiming
early. A stalled participant causes continued waiting with diagnostic warnings,
not successful completion. All production dynamic-head callers are descriptor
close/replacement/unwind paths that release the descriptor-table spinlock before
calling RCU. Other production callers supply embedded heads. Expedited
synchronization uses the same strict completion check and increments its
completion counters only afterward.

Both quiescent-state publishers now use `Riscv::read_time`; the scheduler had
written a jiffies counter into the same timestamp field. Full memory barriers
order unlinking before the updater's participant snapshots, quiescent-state
publication before any subsequent reader loads, and completion before
reclamation. The barrier after publication matters because a release store
alone orders earlier accesses, not a later reader's pointer load.

This repair depends on the kernel's nonpreemptible RCU readers and current
boot-time online-hart model. It does not supply a CPU-hotplug protocol or prove
the separate asynchronous callback-drainer and `barrier_impl` algorithms
correct. The warning followed by direct fd-file release under OOM was observed
during testing, but its presence alone did not establish a panic cause:
ordinary RCU callbacks run in thread context after their queue guards are
released.

### Slab reclamation and comparable memory measurements

The global slab trimmer now keeps the registry lock until it has detached empty
slabs under the cache's free-list lock. It uses raw field projections and narrow
atomic/list borrows instead of an exclusive reference to a concurrently used
cache. Detached slabs no longer reference the cache and are reclaimed outside
both locks, because freeing their bitmaps and descriptors can enter other
allocator caches. The public single-cache trim path shares this helper.

An old assertion compared the slab count before and after detachment and
expected it to decrease. Another hart can allocate a new slab between those
loads, causing a false panic. Counter validation now checks the old values
returned by atomic decrements. This is a bounded trim-path repair; other broad
mutable allocator borrows remain outside its scope.

`MEMSTAT_RECLAIM` explicitly requests empty-slab reclamation before taking a
memory snapshot. Ordinary and default reporting flags retain their read-only
behavior. Reclamation returns actual unused pages to the allocator; it does
not count live objects or partially occupied slabs as free memory. The user-test
supervisor prefaults its executable and stack before the baseline and permits
bounded retries for asynchronous cleanup. The comparison still requires the
original free-page count, without an allowed deficit.

Before this normalization, an isolated recursive-fork run retained 70 pages:
65 were slab capacity and its metadata, two were newly faulted executable cache
pages, and three were outside those two measured pools. All process-resource
active counts returned exactly to baseline. The 15 extra slab descriptors and
14 extra debug bitmaps matched the retained slabs that required them. This
explains why a raw buddy-page difference was insufficient evidence of a leak;
it does not by itself establish the source of the remaining three pages.

### Paths, unmount, and bounded helpers

[`KernelPath`](../../kernel/vfs/path.rs) validates bounded byte slices and
rejects empty, oversized, and embedded-NUL paths. Components remain byte strings,
so non-UTF-8 names remain valid. The iterator handles separators while leaving
dot and parent-directory semantics to traversal. `ParentPath` represents
current-directory, root, and explicit-parent cases without sentinel strings.
Traversal carries existing `IRef` owners across root, cwd, and mounted-root
transitions; typed internal results are converted at retained raw boundaries.
Filesystem registry lookup compares complete `CStr` names, fixing acceptance of
empty names or prefixes such as `tmp` for `tmpfs`.

Cached inode lookup now relies on its existing superblock read/write lock to
keep the hashed allocation alive while acquiring the inode mutex. Removal and
reclamation require the superblock write lock. It checks validity and the
destruction flag before acquiring a lookup reference, so a rejected lookup owns
nothing to release. This removes the deferred-iput allocation and its OOM
fallback, which could call `vfs_iput` while the caller still held the superblock
lock. Linked backendless inodes may still move from zero cached references to
one owned reference under the inode mutex; a saturated positive count is no
longer mistaken for that zero-reference case. The shared VFS workqueue remains
available for fd-file cleanup.

Unmount previously could destroy backend contents before discovering a live
reference. The [`fs.rs`](../../kernel/vfs/fs.rs) preflight checks cached inode
references under the mount/superblock/inode locking contract before destructive
callbacks. It distinguishes internal directory-parent references from external
owners such as open files, cwd, and root. Rejected unmount preserves the mounted
tree. Generic eviction requires zero references, and successful teardown releases
directory parent references before reclaiming the closed tree. The syscall
success path no longer unlocks already released mountpoint/superblock locks.
The inode diagnostic syscall retains its lookup reference while using the
superblock.

Tmpfs inode destruction frees each directory's allocated entry records without
recursively freeing child inodes, which the superblock already owns and visits
separately. Bucket storage is inline and names share their entry allocation.
This also cleans the mounted root and fixes the leak exposed by closed,
nonempty-tree unmount tests. Busy rejection occurs before this cleanup.

[`copy_cstr` and `cstr_array`](../../kernel/string.rs) express bounded,
NUL-terminated name construction using slices and const generics. Selected tty,
device, and process call sites use these helpers. The compatibility `strncpy`
implementation writes exactly the supplied count and retains padding semantics;
canary tests cover short, empty, and truncated buffers. Byte lookup tables use
`count_ones`, `leading_zeros`, `trailing_zeros`, and `reverse_bits`, retaining the
historical zero sentinel where required.

[`AtomicName`](../../kernel/thread_name.rs) replaces the thread's mutable name
array with 16 atomic bytes while retaining its size and alignment. Writers use
`set(&CStr)`; diagnostic readers receive an owned `NameSnapshot`, whose `CStr`
borrow cannot outlive that snapshot. `ThreadAccess` no longer exposes raw name
buffers, and `Thread` no longer implements `Copy` or `Clone`. Relaxed byte
loads/stores remove the rename/read data race; the final byte always remains
NUL. A snapshot can mix bytes from concurrent renames, so it is a diagnostic
label rather than a coherent identity. Reads require no locks, retry loops, or
allocation, including on panic paths. Host tests cover layout, truncation,
snapshot stability, formatting, and concurrent bounded reads/writes.

Backtrace diagnostics also needed repair: checking a frame pointer alone did
not guarantee that both saved words were readable. The safe
[`StackBounds`](../../kernel/backtrace/frame.rs) helper checks order/size
arithmetic, alignment, and the complete frame record before raw reads. The
walker rejects invalid or non-progressing frames. This reduces recursive faults
while diagnosing a damaged stack; bounds validation still depends on the stack
allocation being mapped and live.

## Verification and final evidence

The host seams exercise the actual safe implementations. Packet tests substitute
allocation, and lock tests substitute the raw lock backend; they do not emulate
DMA, interrupts, or the complete kernel allocator. Wire tests cover truncation,
length inconsistencies, checksum cases, accepted and rejected protocol fields,
and ARP parsing. Other tests cover packet ownership/queue cleanup, paths,
bounded strings, frame arithmetic, and exhaustive byte-table results.

The new [`rustnettest`](../../user/rustnettest.c) drives 64 UDP echoes through
the 16-entry rings and covers near-MTU payloads, invalid counts/addresses,
duplicate endpoints, close/rebind, poll, shared-file full-duplex operations,
concurrent metadata operations, and interrupted reads.
[`rustvfstest`](../../user/rustvfstest.c) exercises exact filesystem names,
path boundaries, root/cwd/open-file unmount rejection, preservation after
rejection, and closed nested-tree teardown. A success marker alone does not
replace the runner's panic/failure checks.

The actual `workqueue_test` feature exercises eight in-kernel cases, including
automatic reclamation and a new batch of 64 callback-owned work items. It is
separate from the older `workqueue_smoke_test` stub. The `rcu_test` feature
launches bounded callback/synchronization tests and a held-reader regression:
the reader stays inside RCU on one hart while a coordinator and waiter run on
another. Normal and expedited waits must remain incomplete until the
coordinator releases that reader. These tests exercise real scheduling and
ownership paths; they do not exhaust weak-memory interleavings or validate the
entire legacy RCU stress suite.

The existing `usertests mem` failure also reproduces with the saved
`cd4a7e77` kernel. Its fixture expected the legacy raw status `-1`, whereas
lazy allocation failure occurs during a later page fault and terminates the
child with `SIGSEGV` (signal status 11). Wait-status fixtures now decode normal
exits and signal termination using the POSIX status conventions, including
decoding an exit status before passing it to `exit`. This corrects test
expectations without changing kernel OOM behavior.

The recursive fork stress fixture also needed to supervise its entire process
tree. It previously waited for one child and slept for one second while orphaned
descendants continued to exit or fork. Workers now propagate the stop condition,
close the stop-file descriptor, and recursively reap children with checked exit
statuses. A 16 MiB reserve stops this concurrency test before copy-on-write OOM
kills its supervisors; the separate `mem` test still exhausts allocation.

[`stressfs`](../../user/stressfs.c) now checks read/write results, data contents,
file lengths, and child exit status so filesystem or worker failures cannot
silently satisfy its success marker. The strengthened 33-worker test passed with
the other targeted suites in the evidence table.

[`run_kernel_regressions.py`](../../scripts/run_kernel_regressions.py) copies
kernel and filesystem artifacts into a temporary directory and runs two-hart
QEMU with disposable disk snapshots. It supplies a local UDP echo service,
checks each suite's marker and shell return, and rejects kernel/test failures.
Its alternate-kernel option supports baseline comparison. Repeatable
`--boot-marker` arguments also require completion of asynchronous in-kernel
tests, even when the shell prompt appears first. Failure detection allows
documented recovery warnings but rejects fatal panic preludes, then collects a
short tail so the backtrace cannot hide the actual panic reason.

| Evidence | Final status and artifact |
| --- | --- |
| Final tested revision and build configuration | Kernel `132552fa`, cleaned user-test fixture `9d01ccc2`; QEMU platform, `LAB=fs`, release RISC-V staticlib. Tool/artifact identities are recorded below. |
| Host Rust tests | **64 passed**, `cargo test --manifest-path kernel/Cargo.toml --target x86_64-unknown-linux-gnu`; [log](evidence/host-tests.log). |
| Python verification | **20 passed**: 18 unsafe-analyzer tests and two regression-runner diagnostic tests; [log](evidence/python-tests.log). |
| RISC-V kernel build/link | **Passed**, [default](evidence/kernel-build.log) and `rcu_test,workqueue_test` builds; all-feature Cargo check also [passed](evidence/all-features.log). |
| `rustnettest` and `rustvfstest` final QEMU logs | **Passed**, 64 UDP echoes with boundary/concurrency checks and mount-lifetime regression cases; [log](evidence/kernel-regressions.log). |
| `workqueue_test` eight-case feature run | **8/8 passed**, including 64 callback-owned work items; [log](evidence/rcu-workqueue.log). |
| `rcu_test` bounded SMP feature run | **10 cases passed**, including both held-reader wait variants; [log](evidence/rcu-workqueue.log). |
| Existing mmap/signal/COW/symlink/vfork/clone/device/filesystem suites | **Passed**: mmap 16/16, signals 21/21, COW, symlinks, vfork, clone, devices, and stressfs with 33 workers; [log](evidence/kernel-regressions.log). |
| `usertests -q`, including memory exhaustion | **58/58 passed**, strict final free-page comparison passed with the cleaned fixture; [final log](evidence/usertests-quick.log). Instrumented run restored exactly 66,063 pages after 40 ms, with unchanged slab/page-cache footprints; [accounting log](evidence/quick-accounting.log). Isolated recursive fork also [passed](evidence/fork-accounting.log). |
| X1 EMAC | Compile-only coverage in the RISC-V build/check. No hardware runtime claim. |
| Unexecuted checks | Full legacy RCU stress suite, exhaustive weak-memory interleavings, slow usertests, and physical hardware runs. |

Tool versions: Rust 1.95.0, riscv64-unknown-elf GCC 14.2.0, QEMU 9.0.2. Kernel
binary SHA-256 identities (the binary files themselves are build artifacts):

```text
default  72b22f9a754af59c2f8d4920f25d935bc76c52afa78a5b2255c128d72dfa7c3f
features 835d9e95ee7960e3b7c915365a091cdb5dfabe17d772c0b24fe7073b5cc041e4
baseline f14d1c4fe7da9961097baf8e94e6e1cdac9f296561fda97f8ceab34db1839d1b
```

Stored console logs normalize carriage returns and trailing whitespace; test
messages and ordering are retained.

The saved baseline fails the old memory-status expectation
([log](evidence/baseline-mem.log)) and accepts the overflowing clone stack
([log](evidence/baseline-clone-overflow.log)). A separate baseline full quick
attempt with corrected status fixtures failed during concurrent create/delete
before its final memory check; it is not evidence about that check's final
baseline value. The instrumented accounting logs are from the same final kernel
with temporary user-test diagnostics, removed in `9d01ccc2`.

Reproduction entry points, run from the repository root:

```sh
cargo test --manifest-path kernel/Cargo.toml --target x86_64-unknown-linux-gnu
python3 -m unittest scripts/test_unsafe_analyzer.py scripts/test_kernel_regressions.py
cargo check --manifest-path kernel/Cargo.toml --target riscv64gc-unknown-none-elf --all-features
export TOOLPREFIX=/home/es/xv6/toolchain/build/bin/riscv64-unknown-elf- LAB=fs
cmake -S . -B build
cmake --build build -j8
python3 scripts/run_kernel_regressions.py rustnettest rustvfstest mmaptest testsig cowtest symlinktest vforktest clonetest devtest stressfs
python3 scripts/run_kernel_regressions.py usertests --memory 512M --timeout 600
python3 scripts/run_kernel_regressions.py forkstress --memory 512M
python3 unsafe_analyzer.py --json --baseline docs/rustify/unsafe-baseline-cd4a7e77.json

# The actual in-kernel suites (the separate workqueue_smoke_test is a stub):
RCU_TEST=1 WORKQUEUE_TEST=1 cmake -S . -B build
cmake --build build -j8
python3 scripts/run_kernel_regressions.py clonetest --memory 512M --boot-timeout 180 \
  --boot-marker 'WORKQUEUE TESTS: 8/8 PASSED' \
  --boot-marker 'RCU synchronization smoke tests: ALL TESTS PASSED'

# Restore the default configuration afterward.
cmake -S . -B build
cmake --build build -j8
```

Commands assume the installed cross-toolchain path above; adjust `TOOLPREFIX`
for another installation. QEMU runs use two harts, default 1 GiB for the targeted
battery, and 512 MiB for memory-pressure and in-kernel feature tests. Disk images
are copied before each run. The final build directory is restored to the
default feature configuration.

## Unsafe measurements

Use the same analyzer version and source selection on both revisions. The
[`analyzer documentation`](../../scripts/README_unsafe_analyzer.md) defines the
measurement. It lexes Rust so comments, literal contents, and raw identifiers do
not become fake unsafe sites. Counts distinguish blocks, function bodies,
declarations, impls, traits, extern blocks, and known `u!` invocations.

`unsafe_lines` is the union of physical lines spanned by unsafe block/function
bodies and recognized `u!` arguments; overlapping scopes count once. It includes
comments, whitespace, and delimiters within those bodies. All source cfg
branches, including tests, are counted. Macro expansion is not performed. The
line percentage and unsafe-sites-per-1000-lines describe source shape, not the
number of unsound operations or reachable production instructions.

| Same-analyzer comparison | Baseline `cd4a7e77` | Final revision | Change |
| --- | --- | --- | --- |
| Selected Rust files / total lines | 121 / 103,354 | 127 / 103,455 | +6 / +101 |
| Unsafe lines / percentage | 23,974 / 23.196% | 23,835 / 23.039% | -139 / -0.157 percentage points |
| Unsafe keyword sites / known `u!` calls | 4,674 / 204 | 4,691 / 198 | +17 / -6 |
| Unsafe sites per 1000 lines | 47.197 | 47.257 | +0.060 |
| Network boundary modules: unsafe lines / blocks | 625 / 103 | 409 / 66 | -216 / -37 |

The [baseline JSON](unsafe-baseline-cd4a7e77.json) and
[final JSON](unsafe-final.json) retain file-level measurements. The kernel
measurement corresponds to `132552fa`; later test-fixture and evidence changes
do not alter these Rust sources. The network row compares `net.rs`, `sysnet.rs`,
and `e1000.rs`, including the new `net/` modules in the final selection. It
includes embedded tests and all cfg branches: 1,945 lines before and 1,461 after.
Its unsafe-scope line count fell by 34.6%.

The whole-kernel reduction is modest. Additional explicit unsafe contracts,
scoped raw projections, and justified `Send` implementations increase some site
counts even while removing aliasing and ownership bugs. Test growth also affects
denominators. These numbers do not support a claim that the entire kernel is
now idiomatic or sanitized, or that its remaining unsafe rate is a safety
threshold. The concrete ownership and failure-path changes are the stronger
evidence.

## Remaining boundaries and follow-up scope

Unsafe remains necessary around MMIO and device-owned DMA storage, interrupt
state and context switching, page/slab allocation, user-memory translation and
copying, and raw ABI entry points. Their validity depends on kernel invariants
that slices and local types alone cannot prove. Intrusive queues, reference
counts, mount caches, and manual teardown still contain raw-pointer ownership
relationships. Audited `Send` implementations document those relationships;
they do not make arbitrary dereferences safe.

Large parts of VFS, process, memory-management, and driver code retain C-shaped
entry points, null/error-pointer conventions, broad unsafe functions, and raw
field projections. The socket dispatch audit addresses shared socket files, not
every exclusive-reference conversion in the VFS. Retained string/memory exports
serve compatibility callers and compiler-generated libc calls; replacing their
implementations blindly with intrinsics can introduce recursive libc calls.

Further work should replace individual raw ownership protocols with checked
resource types and then test their failure and concurrency paths. This wave
provides narrower safe interfaces and regression evidence for the paths it
changes. It does not establish complete kernel memory safety, exhaustive race
freedom, or hardware equivalence beyond the recorded tests.
