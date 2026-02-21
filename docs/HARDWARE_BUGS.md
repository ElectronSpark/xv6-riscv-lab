# Hardware-Specific Bugs Found on Orange Pi (Ky X1)

This document records bugs that only manifest on real RISC-V hardware (Orange Pi
with 8-core Ky X1, rv64imafdcv, ky,x60 micro-architecture) but are hidden by
QEMU's more forgiving emulation.

---

## 1. Missing PTE Accessed/Dirty Bits → Infinite Store Page Fault Loop

**Date discovered**: February 2026  
**Symptom**: CPython hangs silently after ~500 successful syscalls. Ctrl-C shows
the process is alive but stuck at a deterministic PC (`0xb3ad6`, a `sd`
instruction in `PyList_Append`). Works perfectly on QEMU.  
**Root cause**: The Ky X1 does **not** implement the Svadu extension (Supervisor
Virtual-Address Data Update). Without Svadu, the hardware does NOT manage the
PTE Accessed (A) and Dirty (D) bits automatically — instead it raises a page
fault when those bits are clear and expects the OS to set them.

Two code paths were affected:

### 1a. `__vma_validate_pte_rxw()` — Write fault handler early-exit bug

**File**: `kernel/mm/vm.c`

The write-fault handler had this early return:

```c
if (pte_val & PTE_W) {
    return 0; // Page is already writable
}
```

When a store page fault occurred because PTE_D (dirty) was not set — even though
PTE_W was set — the handler returned immediately without fixing the missing
PTE_D bit or flushing the TLB. The store would retry, fault again, ad infinitum.

Additionally, after modifying PTE flags in the non-early-return path, this
function was missing `sfence_vma()` (unlike its sister function
`__vma_validate_pte_rx()` which had it).

**Fix**: Changed the early-exit check to require all three bits:

```c
if ((pte_val & (PTE_W | PTE_A | PTE_D)) == (PTE_W | PTE_A | PTE_D)) {
    return 0;
}
```

And added `sfence_vma()` after setting the new PTE value.

### 1b. `vm_mremap()` — Missing A/D bits when moving pages

**File**: `kernel/mm/vm.c`

When mremap moved pages to a new virtual address, it only preserved
`PTE_R | PTE_W | PTE_X | PTE_U` from the source PTE, dropping PTE_A and PTE_D.
On QEMU the hardware silently re-sets them; on Ky X1 this triggers page faults
that feed into bug 1a.

**Fix**: Include PTE_A and PTE_D in the flag mask:

```c
uint64 pte_flags =
    *old_pte & (PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D);
```

### Why QEMU hid this

QEMU's RISC-V MMU emulation manages A/D bits in software internally — it sets
them automatically when a page is accessed or written. Real hardware without
Svadu does not, requiring the OS to handle it via page faults.

---

## 2. Missing `fence.i` After Mapping Executable Pages

**Date discovered**: February 2026  
**Symptom**: Potential for executing stale instruction cache entries after
demand-paging new code pages. Not directly observed as the cause of the Python
hang (that was bug #1), but a correctness issue on real hardware.  
**Root cause**: RISC-V I-cache is not coherent with data writes. After the
kernel writes new code into a page (demand paging, exec, COW), `sfence.vma`
flushes the TLB but NOT the I-cache. On multi-core hardware, `fence.i` must be
issued on all harts that may execute the process.

**Fix**: Added `vm_remote_fence_i(vm)` which calls `sbi_remote_hfence_i()` with
the process's `vm->cpumask` (plus self). Inserted after:
- Instruction page fault resolution (`kernel/irq/trap.c`)
- ELF loading in `exec()` (`kernel/exec.c`)
- `vm_mremap()` when moving executable pages (`kernel/mm/vm.c`)

---

## 3. `sigprocmask` Constant Mismatch

**Date discovered**: February 2026  
**Symptom**: `sigprocmask` syscall returning `-EINVAL` for all calls from
CPython/musl. Python's signal initialization fails silently.  
**Root cause**: xv6 kernel defined `SIG_BLOCK=1, SIG_UNBLOCK=2, SIG_SETMASK=3`
but the Linux ABI (used by musl libc) defines `SIG_BLOCK=0, SIG_UNBLOCK=1,
SIG_SETMASK=2`.

**Fix**: Changed both `kernel/inc/signal.h` and
`user/musl-xv6/arch/riscv64/bits/signal.h` to use the Linux ABI values.

---

## Debugging Methodology

The strace infrastructure (`kernel/irq/syscall.c`, `STRACE_ENABLED`) was
essential for diagnosing these issues. Key observations from strace output:

1. Python's last syscall before hanging was always `mremap(53)` → success
2. When interrupted with Ctrl-C, the PC was always `0xb3ad6` — a `sd s4,0(a0)`
   instruction in `PyList_Append`, storing into a freshly realloc'd buffer
3. The deterministic PC **disproved** the initial I-cache theory (stale I-cache
   would give random garbage instructions, not a real function)
4. The `sd` (store doubleword) instruction pointed to a store page fault loop
5. Disassembly confirmed the store target was a buffer returned by
   `PyMem_Realloc` → `mremap` — the mremap returned pages missing PTE_D

## Audit Results

A full audit of all PTE creation sites in `kernel/mm/vm.c` confirmed that after
the fixes, every leaf PTE write includes PTE_A | PTE_D. Non-leaf (directory)
PTEs and PTE-clearing operations are not affected.

---

## 4. EMAC NIC Driver — Packet Loss on Real Hardware

**Date discovered**: February 2026  
**Symptom**: ~50% packet loss when pinging the board (192.168.0.201). GDB remote
debug gets "timeout" instead of protocol responses. Telnet requires double
connection. Python HTTP demo hangs completely and stops responding to signals.
All networking works perfectly on QEMU (E1000 driver).  
**Affected file**: `kernel/dev/x1_emac.c`  
**Reference**: Linux vendor driver `x1_emac` works correctly on the same
hardware (0 CRC errors, 0 packet errors, 601 interrupts served cleanly).

### Suspected Issues (to investigate)

#### 4a. Interrupt handler runs in hard IRQ context — no NAPI-like deferral

The `x1_emac_intr()` handler calls `x1_emac_recv()` directly, which does
`net_rx()` → `pbuf_alloc` → `pbuf_take` → `e1000_netif.input()` — all inside
the ISR with the spinlock held. This is a lot of work in interrupt context:

1. **Long interrupt hold time**: While processing a burst of RX packets, other
   interrupts are blocked. If new packets arrive faster than they can be
   processed, the DMA ring fills up and packets are dropped.

2. **Lock contention**: `x1_emac_recv()` runs with `sc->lock` NOT held (the
   lock is only in `x1_emac_transmit`), but `net_rx()` eventually calls into
   lwIP's `tcpip_input()` which posts to a mailbox. If the tcpip thread is slow,
   the ISR blocks.

3. **No RX interrupt mitigation**: The `DMA_RECEIVE_IRQ_MITIGATION_CTRL`
   register (0x002C) is never programmed. Without interrupt coalescing, the CPU
   gets one interrupt per received packet, which overwhelms the system under
   load.

The Linux vendor driver uses NAPI polling: the ISR disables RX interrupts and
schedules a poll handler that processes packets in softirq context with a budget
limit. Our driver does none of that.

#### 4b. Missing `__sync_synchronize()` barrier in RX path

In `x1_emac_recv()`, after setting `d->word0 = RX_DESC_OWN` and calling
`dma_cache_clean(d, sizeof(*d))`, the barrier `__sync_synchronize()` is present
before the OWN write. But in the recycle path, there's also a barrier. However,
the key concern is ordering between the cache operations and the DMA engine
reading the descriptor.

#### 4c. TX ring full returns -1 silently

When `x1_emac_transmit()` finds the TX ring full (`d->word0 & TX_DESC_OWN`), it
returns -1. The caller (`e1000_netif_linkoutput`) then frees the mbuf and
returns `ERR_IF` to lwIP. lwIP may retry or drop the packet, but there's no
backpressure mechanism. Under sustained TX load, this silently drops outgoing
packets including TCP retransmissions, ARP replies, etc.

#### 4d. 32-bit DMA address truncation

All buffer addresses are cast to `(uint32)(uint64)addr`. If the kernel allocates
mbufs or descriptor rings from physical memory above 4GB, the upper 32 bits are
silently truncated, causing DMA to read/write garbage memory. The SpacemiT X1
has 8GB RAM — allocations above the 4GB boundary would be corrupted.

Need to verify: Does `kalloc()` / `mbufalloc()` ever return addresses above
0xFFFFFFFF? If the Orange Pi's DRAM starts at a low physical address and we
only use <4GB, this may not be the active bug, but it's a latent correctness
issue.

#### 4e. No link state change handling

After initial PHY auto-negotiation, the driver never re-checks PHY link state.
If the PHY temporarily loses link (cable wiggle, switch reboot), the MAC
continues transmitting into a dead link. The Linux driver uses a PHY state
machine with periodic polling. Adding `yt8531_poll_link()` from a timer or
kthread would detect link changes and call `x1_emac_adjust_link()`.

### Recommended Fixes (priority order)

1. **Enable RX interrupt mitigation** — Program `DMA_RECEIVE_IRQ_MITIGATION_CTRL`
   to coalesce interrupts (e.g., after N packets or T microseconds). This is
   the most likely cause of the 50% packet loss.

2. **Move RX processing out of ISR** — Create a "bottom half" kthread or
   semaphore-based deferred handler. The ISR should just wake it up.

3. **Add TX backpressure** — When TX ring is full, sleep briefly or use a
   semaphore instead of silently dropping the packet.

4. **Verify DMA address range** — Ensure all DMA buffers are allocated from
   physical memory below 4GB, or add IOMMU/bounce buffer support.

5. **Add PHY link state polling** — Periodic kthread to call
   `yt8531_read_link_status()` and adjust MAC if link changes.
