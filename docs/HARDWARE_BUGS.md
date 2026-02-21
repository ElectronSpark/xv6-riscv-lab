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
