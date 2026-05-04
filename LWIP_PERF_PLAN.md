# lwIP Aggressive Performance Plan

Status: planning. Baseline numbers below were captured before any of these
changes land, on x86_64 / 6 vCPU / virtio-net-pci / TAP backend.

## Baseline (best of 3, 4 MiB per stream)

| Workload         | Aggregate throughput   | Notes                              |
| ---------------- | ---------------------- | ---------------------------------- |
| P=1 single flow  | ~50 Mbit/s (~700 ms)   | one CPU at 100% in `tcpip_thread`  |
| P=4              | ~50 Mbit/s             | no scaling — same single CPU       |
| P=8 (32 MiB ea)  | timeout                | negative scaling                   |

Bottleneck: single `tcpip_thread`. Confirmed by the `LWIP_TCPIP_CORE_LOCKING`
experiment (recorded in `/memories/repo/xv6-os-runtime.md`) — moving locking
into the calling thread did not raise the ceiling, so the work is genuinely
serial inside the stack core.

## Goal

Lift aggregate throughput at P>=4 to the **virtio/QEMU+memcpy ceiling**,
not the lwIP-thread ceiling, without leaving lwIP. This is the bounded-effort
alternative to porting NetBSD's stack (see prior session research).

Target after these changes:

| Workload | Target aggregate     | Stretch |
| -------- | -------------------- | ------- |
| P=1      | ~80 Mbit/s           | 100     |
| P=4      | ~120 Mbit/s          | 180     |
| P=8      | ~150 Mbit/s          | 250     |

These are honest targets; they will not match a NetBSD/SMP stack (which would
land in 400+ Mbit/s territory) because the lwIP core remains single-threaded.
The wins below come from doing **less work per packet on that one thread**
and giving it a CPU of its own.

## Levers (ordered by expected payoff per LOC)

### 1. PBUF_REF zero-copy on RX
File: `kernel/kernel/lwip_port/lwip_glue.c` (`net_rx`).
Today: `pbuf_alloc(PBUF_POOL) + pbuf_take(p, m->head, m->len) + mbuffree(m)` —
one pool allocation plus a 60–1500 byte memcpy per packet on `tcpip_thread`.
Plan:
  - Embed a `struct pbuf_custom` in our `struct mbuf` (or alongside it).
  - Use `pbuf_alloced_custom(PBUF_RAW, m->len, PBUF_REF, &mc->pc, m->head, m->len)`.
  - Free callback (`pbuf_custom.custom_free_function`) calls `mbuffree(m)` once
    lwIP releases the last reference (which may be much later — TCP holds
    received segments until the application reads them).
  - Requires `LWIP_SUPPORT_CUSTOM_PBUF=1` in `lwipopts.h`.
  - Requires PBUF backing storage to outlive the immediate RX path; our 2 KiB
    page-backed mbuf already satisfies this.
Risk: low. Reference-counting bug would manifest as use-after-free or leak;
catchable with a leak counter on `mbuffree`.
Expected: **~10–25% single-stream**, larger on multi-stream where copy bandwidth
dominates the `tcpip_thread` budget.

### 2. LWIP_CHECKSUM_ON_COPY=1
File: `kernel/kernel/lwip_port/lwipopts.h`.
Folds the TCP/UDP checksum into the data copy that `tcp_write` already does
when the application calls `send()` with `TCP_WRITE_FLAG_COPY`. Today we copy,
*then* walk the bytes again to compute a checksum. This collapses the second
pass into the first.
Risk: very low — well-tested upstream lwIP option.
Expected: **~10% TX throughput**.

### 3. LWIP_NETIF_TX_SINGLE_PBUF=1
File: `kernel/kernel/lwip_port/lwipopts.h`.
Forces TCP segments destined for the netif to be emitted as a single
contiguous pbuf, eliminating chain-walk in `xv6_netif_linkoutput()` and the
`pbuf_copy_partial()` over a chain. After this change `linkoutput` is a single
`memmove` into the mbuf with the virtio header reservation.
Risk: low. Slightly increases pbuf memory pressure (we allocate larger TX
pbufs); covered by our existing 8 MiB MEM_SIZE.
Expected: **~5% TX**, simpler hot path.

### 4. Larger TCP send buffer + matching window
File: `kernel/kernel/lwip_port/lwipopts.h`.
Today: `TCP_SND_BUF = 64 * MSS` (~93 KiB), `TCP_WND = 128 * MSS` (~187 KiB).
The asymmetry caps single-stream BDP. Plan:
  - `TCP_SND_BUF = 256 * MSS` (~373 KiB) so a single stream can keep the wire
    full at 1 ms RTT * 1 Gbit/s.
  - `TCP_SND_QUEUELEN` scales automatically (it's a macro of TCP_SND_BUF).
  - Leave `TCP_WND` at 128 MSS for now (host iperf usually has plenty of buffer).
Risk: memory pressure at high concurrency (256 conns * 373 KiB = ~93 MiB peak,
which exceeds our MEM_SIZE). Accept by also lifting MEM_SIZE to 16 MiB if
we see allocation failures under stress.
Expected: **~30–50% single-stream** when RTT > LAN-trivial.

### 5. Pin tcpip_thread to a dedicated CPU
File: `kernel/kernel/lwip_port/sys_arch.c` (`sys_thread_new`).
Today: scheduler may migrate `tcpip_thread` and the virtio RX workqueue
threads onto the same CPU. Plan: when `name` matches `"lwip_tcpip"`, after
`kthread_create`, set `sched_attr.affinity_mask = (1 << TCPIP_PIN_CPU)` (CPU 2,
away from CPU 0 where the virtio IRQ runs and CPU 1 where the workqueue
threads land first). Same `sched_setattr()` pattern RCU already uses
(`kernel/kernel/lock/rcu.c`).
Risk: very low. Worst case: small regression if CPU 2 was hot.
Expected: **~10–20%** by removing migration cost and improving cache residency.

## Combined target

Levers 1+2+3+4 attack the byte-handling cost on `tcpip_thread`. Lever 5
gives that thread a stable CPU. Together: somewhere between 1.5x and 3x on
the single-stream number. Multi-stream is constrained by Amdahl on the same
serial thread, so don't expect linear scaling — but the per-byte work goes
down, so the thread can sustain more total bytes/sec.

## Implementation order (one commit per lever)

1. Add lwipopts toggles for `LWIP_SUPPORT_CUSTOM_PBUF`, `LWIP_CHECKSUM_ON_COPY`,
   `LWIP_NETIF_TX_SINGLE_PBUF`. Build, smoke (DHCP + ping), tcpstress P=1.
2. Bump `TCP_SND_BUF` to 256 MSS. Build, smoke, tcpstress P=1.
3. Implement PBUF_REF zero-copy in `net_rx`. Build, smoke, tcpstress P=1/4.
4. Pin `tcpip_thread` to CPU 2. Build, smoke, tcpstress P=1/4/8.

After each step: capture aggregate Mbit/s and append to the result table at
the bottom of this file. Revert the step if it's a regression.

## Validation script

```
for P in 1 4 8; do
  BEST=999999
  for i in 1 2 3; do
    MS=$(QEMU_NET_MODEL=virtio-net-pci TCPSTRESS_NET_BACKEND=tap \
         TCPSTRESS_PARALLEL=$P TCPSTRESS_PAYLOAD=4194304 \
         TCPSTRESS_ITERS=1 TCPSTRESS_TIMEOUT=30 TCPSTRESS_BOOT_GRACE=4 \
         bash scripts/run-tcpstress.sh 2>&1 \
         | grep -oP 'elapsed_ms=\K[0-9]+' | tail -1)
    [ -z "$MS" ] && MS=999999
    [ $MS -lt $BEST ] && BEST=$MS
  done
  tb=$((4194304*P))
  printf "P=%d best=%d ms => %.1f Mbit/s\n" $P $BEST \
         $(echo "scale=1; $tb*8/($BEST*1000)" | bc)
done
```

## Results (filled in as we go)

| Step | P=1 ms / Mbit | P=4 ms / Mbit | P=8 ms / Mbit | Notes |
| ---- | ------------- | ------------- | ------------- | ----- |
| baseline (current HEAD) | ~700 / 50 | ~2700 / 50 | timeout / —  | reverted CORE_LOCKING |
| 1 (custom-pbuf opts)    |               |               |               |       |
| 2 (TCP_SND_BUF 256)     |               |               |               |       |
| 3 (PBUF_REF on RX)      |               |               |               |       |
| 4 (CPU pin)             |               |               |               |       |

## Out of scope (but tempting)

- TX zero-copy via `pbuf_alloced_custom` over a slab of mbufs. Possible but
  needs careful free-callback ordering with virtio TX completion IRQ.
- TSO/GSO offload to virtio-net. Requires implementing GSO segmentation
  acceptance on RX (which we deferred — see `virtio_net.c` comment block).
  Real win, but bigger surface than this plan.
- LWIP_TCPIP_CORE_LOCKING revisit. Recorded as a wash. Don't redo.
- Multi-instance lwIP (one stack per CPU + RSS). Architectural rewrite, not
  an "aggressive edit". Falls under the NetBSD-port option.
