# xv6 Development Notes

## Hardware: Orange Pi RV2

| Property | Value |
|---|---|
| Board | Orange Pi RV2 |
| SoC | SpacemiT X1 (Ky X60 cores), `ky,x1-emac` compatible |
| ISA | rv64imafdcv + Zicbom, Zicboz, Zba/Zbb/Zbc/Zbs, Svpbmt, Svinval |
| CPU | 8 cores, 1 thread/core, max 1.6 GHz, SV39 MMU |
| RAM | 8 GB (7.7 GiB usable) |
| L1 D/I | 32 KiB each per core, 4-way, 64-byte lines |
| L2 | 512 KiB per cluster (2 clusters), 16-way, 64-byte lines |
| CBOM block | 64 bytes (from DT: `riscv,cbom-block-size`) |
| Storage | 469 GB NVMe SSD |
| Linux kernel | 6.6.63-ky (vendor) |
| SSH access | `sshpass -p orangepi ssh root@192.168.0.201` |

### Network (EMAC)

| Property | Value |
|---|---|
| Primary interface | `end0`, IP 192.168.0.201/24 (static) + DHCP secondary |
| MAC | `c0:74:2b:f9:16:a8` |
| Speed | 1 Gbps Full, flow control rx/tx |
| Driver | `x1_emac` (built-in, `drivers/net/ethernet/ky/x1-emac`) |
| MMIO base | `0xcac80000`, size `0x420` |
| Second EMAC | `end1` at `0xcac81000` (link DOWN) |
| PHY UID | `0x4f51e91b` (likely Motorcomm YT8531) |
| WiFi | `wlan0` at 192.168.0.110 |

### Vendor DT EMAC Configuration

| Property | DT Value | Meaning |
|---|---|---|
| `rx-ring-num` | 1024 | Vendor uses 1024 RX descriptors (16-byte each) |
| `tx-ring-num` | 1024 | Vendor uses 1024 TX descriptors (16-byte each) |
| `dma-burst-len` | 5 | bit 5 → `BURST_16WORD` |
| `rx-threshold` | 0x0c (12) | RX packet start threshold |
| `tx-threshold` | 0x05ee (1518) | TX packet start threshold (store-forward) |

### Vendor DMA_CONFIGURATION Register (0x0000)

Read from `ethtool -d end0`: register offset 0x0000 = `0x00060020`

Bits set: `STRICT_BURST` (bit 17) | `64BIT_MODE` (bit 18) | `BURST_16WORD` (bit 5)

**`WAIT_FOR_DONE` (bit 16) is NOT set** in the vendor driver.

---

## Bug Fixes Applied

### 1. TCP Timed-Wait Bug (sys_arch.c)
- **Problem**: Decrement-before-sleep in timed mbox/sem waits could lose wakeups
- **Fix**: Refactored to use `sem_wait_timed()` which handles the protocol internally

### 2. sys_now() Returns Zero (sys_arch.c)
- **Problem**: `goldfish_rtc_read_ns()` returns 0 on SpacemiT X1 (no goldfish RTC)
- **Fix**: `sys_now()` now uses `r_time() / TICK_MS` — same clock as scheduler timers
- **Verification**: Timing measurements now correct: "mbox_fetch TIMEOUT after 100/100 ms"

### 3. DMA Cache Coherency Bug — Descriptor Cache Line Sharing (x1_emac)

- **Problem**: Each RX/TX descriptor was 16 bytes. Four descriptors shared one 64-byte
  cache line (`CBOM_BLOCK_SIZE=64`). When `dma_cache_clean(d, sizeof(*d))` wrote back
  a recycled descriptor (setting OWN=1), `cbo.clean` wrote back the **entire 64-byte
  cache line**, clobbering DMA engine's OWN=0 updates on adjacent descriptors that
  had received packets.
- **Symptoms**: ~50% packet loss initially, 100% "Destination host unreachable" after
  telnet. ISRs fired with `RX_DONE` but `rx_this_burst == 0` — invisible on QEMU
  because memory is coherent there.
- **Root cause**: Non-coherent DMA on RISC-V with Zicbom. `cbo.clean` operates on
  full cache lines, not individual bytes.

#### Fix: Chained Descriptor Mode (64-byte padding)

**`kernel/inc/dev/x1_emac.h`:**
- Padded `struct x1_rx_desc` and `struct x1_tx_desc` from 16 → 64 bytes
  (added `uint32 __pad[12]`)
- Added `#define DESC_CHAINED (1U << 25)` for `SecondAddressChained` bit
- Reduced ring sizes: 256 → 64 entries (64 × 64 = 4096 = 1 page, same memory)

**`kernel/dev/x1_emac.c`:**
- Converted from ring mode (`END_RING` on last descriptor) to **chained descriptor
  mode**: `buf_addr2` = next descriptor physical address, `DESC_CHAINED` set in word1.
- DMA engine follows chain pointers → correctly strides 64 bytes between descriptors.
- Updated functions: `x1_emac_tx_ring_init`, `x1_emac_rx_ring_init`,
  `x1_emac_transmit`, `x1_emac_recv` (normal + recycle paths).
- `x1_emac_tx_reclaim` needed NO changes (read-only: inval + OWN check).
- No `END_RING` references remain in the `.c` file (defines kept in header but unused).

**Alternatives considered and rejected:**
- DSL (Descriptor Skip Length): Not available — bits 8-15 of DMA_CONFIGURATION
  are undocumented/unused in this variant.
- Struct padding without chaining: DMA hardware uses fixed 16-byte stride internally
  for ring mode, would read padding as next descriptor.
- Uncached mappings: Would require Svpbmt page table attribute support, too complex.

### 4. DMA Configuration Mismatch (x1_emac)

- **Problem**: Our driver set `DMA_CFG_WAIT_FOR_DONE` (bit 16), but the vendor Linux
  driver does NOT set it (register value `0x00060020`).
- **Fix**: Removed `DMA_CFG_WAIT_FOR_DONE` from DMA configuration in `x1_emac.c`.
  Now matches vendor: `DMA_CFG_STRICT_BURST | DMA_CFG_64BIT_MODE | DMA_CFG_BURST_16WORD`.

### 5. Uninterruptible Socket Blocking (sys_arch.c, sys_socket.c)

- **Problem**: Python httpd demo blocks in `accept()` and cannot respond to Ctrl+C
  (SIGINT) until a browser refresh triggers a new connection.
- **Root cause chain**:
  1. `sys_accept()` → `netconn_accept()` → `sys_arch_mbox_fetch(mbox, msg, 0)`
  2. timeout=0 (wait forever) → `sem_wait(ne)` → **THREAD_UNINTERRUPTIBLE**
  3. SIGINT delivered but thread doesn't wake; signal only runs when connection
     arrives and thread returns to userspace.
  4. Same issue for `sys_recvfrom()` → `netconn_recv*()` → `sys_arch_mbox_fetch`.

#### Fix: Interruptible mbox fetch for user-space threads

**`kernel/lwip_port/sys_arch.c`:**
- Added `#include "signal.h"` for `signal_pending()`.
- In `sys_arch_mbox_fetch` (timeout=0 path): user-space threads
  (`THREAD_USER_SPACE(current)`) now use `sem_wait_interruptible()`.
  If interrupted (`-EINTR`), returns `SYS_ARCH_TIMEOUT`.
- Kernel threads (e.g. lwIP tcpip thread) still use uninterruptible `sem_wait()`.

**`kernel/lwip_port/sys_socket.c`:**
- Added `#include "signal.h"`.
- `sys_accept`: when `netconn_accept` returns `ERR_TIMEOUT` and
  `signal_pending(current)`, returns `-EINTR` instead of `-ETIMEDOUT`.
- `sys_recvfrom` (TCP path): same `ERR_TIMEOUT` + `signal_pending` → `-EINTR`.
- `sys_recvfrom` (UDP path): same treatment for `netconn_recv`.

**NOT changed**: `sys_arch_sem_wait` remains uninterruptible — it's used by
`tcpip_send_msg_wait_sem()` where a message is already posted to the tcpip thread.
Interrupting mid-roundtrip would leave lwIP state inconsistent.

**lwIP path**: `SYS_ARCH_TIMEOUT` → lwIP checks it (since `LWIP_SO_RCVTIMEO=1`)
→ returns `ERR_TIMEOUT` → syscall wrapper converts to `-EINTR` if signal pending.

---

## Sleep Lock Interruptible Primitives (added earlier)

- `sem_wait_interruptible()` — `kernel/lock/semaphore.c`
- `sem_wait_timed()` — `kernel/lock/semaphore.c`
- `mutex_lock_interruptible()` — `kernel/lock/mutex.c`
- `mutex_lock_timed()` — `kernel/lock/mutex.c`
- `rwsem_acquire_read_interruptible()` — `kernel/lock/rwsem.c`
- `rwsem_acquire_write_interruptible()` — `kernel/lock/rwsem.c`
- `wait_for_completion_timeout()` — `kernel/lock/completion.c`
- `wait_for_completion_interruptible_timeout()` — `kernel/lock/completion.c`

All use `tq_wait_in_state(&queue, &lock, NULL, THREAD_INTERRUPTIBLE)` and
check `signal_pending(current)` to return `-EINTR` on signal delivery.

---

## Key Architecture Notes

### DMA Cache Management (RISC-V Zicbom)
- `dma_cache_clean(addr, size)` → `cbo.clean` — write-back dirty cache lines
- `dma_cache_inval(addr, size)` → `cbo.inval` — invalidate (discard dirty data!)
- `dma_cache_flush(addr, size)` → `cbo.flush` — clean + invalidate
- All operate on **full 64-byte cache lines** — cannot target sub-line ranges
- No IOMMU on this SoC — DMA addresses are physical addresses

### Memory Allocation
- `kalloc()` — single 4096-byte page
- `page_alloc(order, flags)` — buddy allocator, 2^order contiguous pages

### Thread Types
- `THREAD_USER_SPACE(p)` — checks flag bit 5, true for user processes
- `THREAD_SET_USER_SPACE(p)` / `THREAD_CLEAR_USER_SPACE(p)` — set/clear
- Kernel threads (lwIP tcpip, gdbstub, etc.) do NOT have this flag
- `signal_pending(current)` — declared in `kernel/inc/signal.h`

### lwIP Configuration (kernel/lwip_port/lwipopts.h)
- `LWIP_DHCP 1` — DHCP enabled (could conflict with static IP)
- `LWIP_SO_RCVTIMEO 1` — receive timeout support enabled
- `PBUF_POOL_SIZE 1024`, `MEMP_NUM_TCPIP_MSG_INPKT 128`, `SYS_MBOX_SIZE 128`
- `TCPIP_THREAD_NAME "lwip_tcpip"`

### Build
- Workspace: `/home/es/xv6/xv6-tmp/`
- Build dir: `build/`
- Build command: `cd build && make -j16`
- Compiler flags include `-Werror`
- CMake-based build system

---

## Pending / Not Yet Tested

- [ ] DMA cache coherency fix (chained descriptors) — not yet tested on hardware
- [ ] DMA configuration alignment (remove WAIT_FOR_DONE) — not yet tested
- [ ] Interruptible socket blocking — not yet tested (on QEMU or hardware)
- [ ] `LWIP_DHCP 1` may cause IP address conflicts if DHCP server assigns
      different IP than the static 192.168.0.201 configured in lwip_glue.c
