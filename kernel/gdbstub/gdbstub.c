/*
 * gdbstub.c — In-kernel GDB Remote Serial Protocol stub for user-process
 *             debugging over TCP.
 *
 * Usage:
 *   1. Boot xv6.  The stub listens on TCP port 1234.
 *   2. From the host:
 *        $ riscv64-unknown-elf-gdb  path/to/user_program
 *        (gdb) target remote 10.0.2.15:1234
 *        (gdb) attach <pid>            # or use 'monitor attach <pid>'
 *        (gdb) b main
 *        (gdb) continue
 *
 * The stub speaks a useful subset of the GDB RSP:
 *   ?            — stop reason
 *   g / G        — read / write all GP registers + PC
 *   p / P        — read / write single register
 *   m / M        — read / write memory
 *   c / s        — continue / single-step  (step = temp bp at PC+4)
 *   Z0 / z0      — insert / remove software breakpoint
 *   H / qC       — thread-id operations (stub is single-threaded)
 *   qSupported   — feature negotiation
 *   qAttached    — always "1" (attached, not spawned)
 *   qfThreadInfo — list the attached PID
 *   qRcmd        — monitor commands (attach <pid>)
 *   D            — detach
 *   k            — kill target process
 */

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "printf.h"
#include "string.h"
#include "trapframe.h"
#include "trap.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "mm/vm.h"
#include "lock/spinlock.h"
#include "defs.h"
#include "lock/semaphore.h"
#include "lock/rcu.h"
#include "signal.h"
#include "smp/ipi.h"
#include "smp/atomic.h"
#include "smp/percpu.h"

/* lwIP netconn (blocking TCP) API */
#include "lwip/api.h"
#include "lwip/tcp.h"
#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"

/* Forward-declare snprintf (provided by lwip_port/sys_arch.c). */
int snprintf(char *buf, size_t size, const char *fmt, ...);

/* ──────────────────────────────────────────────────────────────────────────── */
/* Configuration                                                               */
/* ──────────────────────────────────────────────────────────────────────────── */

#define GDBSTUB_PORT        2159
#define GDB_BUF_SIZE        4096
#define GDB_MAX_BREAKPOINTS 64

/* Set to 1 to enable verbose per-packet tracing on the console. */
/*
 * Logging levels:
 *   0 — off
 *   1 — important events: attach, detach, trap, stop, resume, errors
 *   2 — debug detail: bp insert/remove, bp list, step, mem-write errors
 *   3 — raw packet I/O (every <- / -> packet)
 */
#define GDB_LOG_LEVEL   1

#if GDB_LOG_LEVEL >= 1
#define GDB_LOG(fmt, ...) printf("gdb: " fmt "\n", ##__VA_ARGS__)
#else
#define GDB_LOG(fmt, ...) ((void)0)
#endif

#if GDB_LOG_LEVEL >= 2
#define GDB_DBG(fmt, ...) printf("gdb: " fmt "\n", ##__VA_ARGS__)
#else
#define GDB_DBG(fmt, ...) ((void)0)
#endif

#if GDB_LOG_LEVEL >= 3
static void gdb_trace_pkt(const char *dir, const char *data, int len)
{
    char tmp[68];
    int n = len < 64 ? len : 64;
    memmove(tmp, data, n);
    tmp[n] = '\0';
    if (len > 64)
        printf("gdb: %s %s... (%d bytes)\n", dir, tmp, len);
    else
        printf("gdb: %s %s\n", dir, tmp);
}
#else
static inline void gdb_trace_pkt(const char *dir, const char *data, int len)
{ (void)dir; (void)data; (void)len; }
#endif

/* RISC-V EBREAK encoding */
#define RV_EBREAK           0x00100073U   /* 32-bit EBREAK */
#define RV_C_EBREAK         0x9002U       /* 16-bit compressed EBREAK */

/* Number of GP registers GDB expects for RISC-V (x0-x31 + pc = 33) */
#define GDB_NUM_REGS        33
#define GDB_REG_BYTES       (GDB_NUM_REGS * 8)

/* ──────────────────────────────────────────────────────────────────────────── */
/* Software-breakpoint table                                                   */
/* ──────────────────────────────────────────────────────────────────────────── */

struct gdb_breakpoint {
    uint64 addr;
    uint32 orig_insn;    /* saved instruction bytes (up to 4) */
    uint8  len;          /* 2 or 4 */
    uint8  active;
};

/* ──────────────────────────────────────────────────────────────────────────── */
/* Per-session state                                                           */
/* ──────────────────────────────────────────────────────────────────────────── */

static struct {
    /* Target process */
    int              target_pid;
    struct thread   *target;          /* valid only while stopped */
    int              attached;        /* 1 if GDB is attached to a process */

    /* Synchronization: target stops on breakpoint, GDB resumes it */
    sem_t            target_stopped;  /* posted when target hits a bp */
    sem_t            target_resume;   /* posted when GDB says continue */

    /* Stop reason communicated from trap handler to GDB thread */
    int              stop_signal;     /* e.g. 5 = SIGTRAP */

    /* Single-step state */
    int              stepping;        /* 1 if single-stepping */
    uint64           step_bp_addr;    /* address of temporary step bp */
    uint32           step_bp_orig;    /* saved insn at step_bp_addr */
    uint8            step_bp_len;     /* 2 or 4 */

    /* Breakpoint table */
    struct gdb_breakpoint bps[GDB_MAX_BREAKPOINTS];
    int              nbps;

    /* Pending attach: a process hit EBREAK with no debugger attached
     * and is waiting for one.  The GDB thread posts debugger_attached
     * when it processes a vAttach / monitor attach for that PID. */
    int              pending_pid;     /* PID waiting for debugger, or 0 */
    sem_t            debugger_attached;

    /* Network I/O */
    struct netconn  *conn;            /* current client connection */

    /* Packet buffers */
    char             txbuf[GDB_BUF_SIZE];
    char             rxbuf[GDB_BUF_SIZE];

    /* Async interrupt: set by GDB thread on Ctrl-C, checked by
     * usertrapret() before returning to user space. */
    volatile int     interrupt_pending;

    /* Non-zero when the target is running (c/s was issued, waiting
     * for the next stop).  While set the GDB thread polls for both
     * network data (Ctrl-C) and target_stopped notifications. */
    int              target_running;

    /* Saved pbuf from last recv — may contain unconsumed bytes from
     * a TCP segment that carried multiple GDB packets. */
    struct pbuf     *rxpbuf;
    int              rxpoff;

    /* Global lock protecting this struct */
    spinlock_t       lock;
} gdb;

/* ──────────────────────────────────────────────────────────────────────────── */
/* Hex conversion helpers                                                      */
/* ──────────────────────────────────────────────────────────────────────────── */

static const char hexchars[] = "0123456789abcdef";

static int hex2nib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Write a 64-bit value as 16 hex chars (little-endian byte order). */
static char *hex_le64(char *p, uint64 val)
{
    for (int i = 0; i < 8; i++) {
        uint8 b = (uint8)(val >> (i * 8));
        *p++ = hexchars[b >> 4];
        *p++ = hexchars[b & 0xf];
    }
    return p;
}

/* Parse a hex string into a uint64 (big-endian / natural order). */
static uint64 hex2u64(const char *p, int nchars)
{
    uint64 val = 0;
    for (int i = 0; i < nchars; i++) {
        int n = hex2nib(p[i]);
        if (n < 0) break;
        val = (val << 4) | n;
    }
    return val;
}

/* Parse a little-endian hex-encoded 64-bit value (16 hex chars). */
static uint64 hexle2u64(const char *p)
{
    uint64 val = 0;
    for (int i = 0; i < 8; i++) {
        int hi = hex2nib(p[i * 2]);
        int lo = hex2nib(p[i * 2 + 1]);
        if (hi < 0 || lo < 0) break;
        val |= (uint64)((hi << 4) | lo) << (i * 8);
    }
    return val;
}

/* Encode a byte buffer to hex. Returns number of hex chars written. */
static int mem2hex(char *dst, const void *src, int len)
{
    const uint8 *s = (const uint8 *)src;
    for (int i = 0; i < len; i++) {
        *dst++ = hexchars[s[i] >> 4];
        *dst++ = hexchars[s[i] & 0xf];
    }
    return len * 2;
}

/* Decode hex to byte buffer. Returns number of bytes written. */
static int hex2mem(void *dst, const char *src, int len)
{
    uint8 *d = (uint8 *)dst;
    for (int i = 0; i < len; i++) {
        int hi = hex2nib(src[i * 2]);
        int lo = hex2nib(src[i * 2 + 1]);
        if (hi < 0 || lo < 0) return i;
        d[i] = (uint8)((hi << 4) | lo);
    }
    return len;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Packet framing: $<data>#<checksum>                                          */
/* ──────────────────────────────────────────────────────────────────────────── */

/* Send a raw byte over the GDB connection. */
static int gdb_putchar(char c)
{
    return netconn_write(gdb.conn, &c, 1, NETCONN_COPY);
}

/* Send a complete RSP packet: $data#xx */
static int gdb_send_packet(const char *data, int len)
{
    gdb_trace_pkt("->", data, len);

    uint8 csum = 0;
    for (int i = 0; i < len; i++)
        csum += (uint8)data[i];

    char hdr = '$';
    char trail[3];
    trail[0] = '#';
    trail[1] = hexchars[csum >> 4];
    trail[2] = hexchars[csum & 0xf];

    err_t err;
    /*
     * Use NETCONN_MORE on the first writes so lwIP coalesces them
     * into a single TCP segment.  The final write (without MORE)
     * triggers tcp_output() inside the tcpip thread.
     */
    err = netconn_write(gdb.conn, &hdr, 1, NETCONN_COPY | NETCONN_MORE);
    if (err != ERR_OK) return -1;
    if (len > 0) {
        err = netconn_write(gdb.conn, data, len, NETCONN_COPY | NETCONN_MORE);
        if (err != ERR_OK) return -1;
    }
    /* Final write without MORE — triggers tcp_output(). */
    err = netconn_write(gdb.conn, trail, 3, NETCONN_COPY);
    if (err != ERR_OK) return -1;
    return 0;
}

/* Send a simple string packet (trace is done by gdb_send_packet). */
static int gdb_send(const char *s)
{
    return gdb_send_packet(s, strlen(s));
}

/* Send "OK". */
static int gdb_ok(void)    { return gdb_send("OK"); }

/* Send empty response (unsupported command). */
static int gdb_empty(void) { return gdb_send_packet("", 0); }

/* Send an error: E xx */
static int gdb_error(int code)
{
    char buf[4];
    buf[0] = 'E';
    buf[1] = hexchars[(code >> 4) & 0xf];
    buf[2] = hexchars[code & 0xf];
    buf[3] = '\0';
    return gdb_send(buf);
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Receive a packet (blocking).                                                */
/* Returns length of data in gdb.rxbuf, or -1 on error/disconnect.             */
/* ──────────────────────────────────────────────────────────────────────────── */

static int gdb_recv_packet(void)
{
    /* State machine: skip until '$', collect until '#', read 2 checksum chars */
    enum { WAIT_START, COLLECT, CSUM1, CSUM2 } state = WAIT_START;
    int pos = 0;
    uint8 running_csum = 0;
    /* We don't strictly validate the checksum — GDB re-sends on NACK. */
    (void)running_csum;

    /*
     * Use saved pbuf from previous call — a single TCP segment may
     * contain multiple GDB packets, so we must not free the pbuf
     * until all its data has been consumed.
     */
    for (;;) {
        /* Get more data if needed */
        if (gdb.rxpbuf == NULL || gdb.rxpoff >= gdb.rxpbuf->tot_len) {
            if (gdb.rxpbuf) pbuf_free(gdb.rxpbuf);
            gdb.rxpbuf = NULL;
            err_t err = netconn_recv_tcp_pbuf(gdb.conn, &gdb.rxpbuf);
            if (err != ERR_OK) {
                gdb.rxpbuf = NULL;
                return -1;
            }
            gdb.rxpoff = 0;
        }

        char c = pbuf_get_at(gdb.rxpbuf, gdb.rxpoff++);

        switch (state) {
        case WAIT_START:
            if (c == '$') {
                state = COLLECT;
                pos = 0;
                running_csum = 0;
            } else if (c == '+' || c == '-') {
                /* ACK/NACK — ignore */
            } else if (c == 0x03) {
                /* Ctrl-C interrupt — synthesize a stop.
                 * Do NOT free pbuf — more data may follow. */
                gdb.rxbuf[0] = 0x03;
                return 1;
            }
            break;
        case COLLECT:
            if (c == '#') {
                state = CSUM1;
            } else if (c == '$') {
                /* Re-sync */
                pos = 0;
                running_csum = 0;
            } else {
                running_csum += (uint8)c;
                if (pos < GDB_BUF_SIZE - 1)
                    gdb.rxbuf[pos++] = c;
            }
            break;
        case CSUM1:
            state = CSUM2;
            break;
        case CSUM2:
            gdb.rxbuf[pos] = '\0';
            /* Do NOT free pbuf — more packets may follow in
             * the same TCP segment.  It will be freed on the
             * next call when all data has been consumed. */
            gdb_putchar('+');
            return pos;
        }
    }
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Register access                                                             */
/*                                                                             */
/* GDB RISC-V register order: x0..x31, pc                                      */
/* Map from utrapframe fields.                                                 */
/* ──────────────────────────────────────────────────────────────────────────── */

static uint64 gdb_get_reg(struct utrapframe *tf, int regnum)
{
    struct trapframe *t = &tf->trapframe;
    switch (regnum) {
    case 0:  return 0;                 /* x0 = zero */
    case 1:  return t->ra;             /* x1 */
    case 2:  return t->sp;             /* x2 */
    case 3:  return tf->gp;            /* x3 */
    case 4:  return tf->tp;            /* x4 */
    case 5:  return t->t0;             /* x5 */
    case 6:  return t->t1;             /* x6 */
    case 7:  return t->t2;             /* x7 */
    case 8:  return t->s0;             /* x8 / fp */
    case 9:  return tf->s1;            /* x9 */
    case 10: return t->a0;             /* x10 */
    case 11: return t->a1;             /* x11 */
    case 12: return t->a2;             /* x12 */
    case 13: return t->a3;             /* x13 */
    case 14: return t->a4;             /* x14 */
    case 15: return t->a5;             /* x15 */
    case 16: return t->a6;             /* x16 */
    case 17: return t->a7;             /* x17 */
    case 18: return tf->s2;            /* x18 */
    case 19: return tf->s3;            /* x19 */
    case 20: return tf->s4;            /* x20 */
    case 21: return tf->s5;            /* x21 */
    case 22: return tf->s6;            /* x22 */
    case 23: return tf->s7;            /* x23 */
    case 24: return tf->s8;            /* x24 */
    case 25: return tf->s9;            /* x25 */
    case 26: return tf->s10;           /* x26 */
    case 27: return tf->s11;           /* x27 */
    case 28: return t->t3;             /* x28 */
    case 29: return t->t4;             /* x29 */
    case 30: return t->t5;             /* x30 */
    case 31: return t->t6;             /* x31 */
    case 32: return t->sepc;           /* pc  */
    default: return 0;
    }
}

static void gdb_set_reg(struct utrapframe *tf, int regnum, uint64 val)
{
    struct trapframe *t = &tf->trapframe;
    switch (regnum) {
    case 0:  break;                    /* x0 is hardwired */
    case 1:  t->ra   = val; break;
    case 2:  t->sp   = val; break;
    case 3:  tf->gp  = val; break;
    case 4:  tf->tp  = val; break;
    case 5:  t->t0   = val; break;
    case 6:  t->t1   = val; break;
    case 7:  t->t2   = val; break;
    case 8:  t->s0   = val; break;
    case 9:  tf->s1  = val; break;
    case 10: t->a0   = val; break;
    case 11: t->a1   = val; break;
    case 12: t->a2   = val; break;
    case 13: t->a3   = val; break;
    case 14: t->a4   = val; break;
    case 15: t->a5   = val; break;
    case 16: t->a6   = val; break;
    case 17: t->a7   = val; break;
    case 18: tf->s2  = val; break;
    case 19: tf->s3  = val; break;
    case 20: tf->s4  = val; break;
    case 21: tf->s5  = val; break;
    case 22: tf->s6  = val; break;
    case 23: tf->s7  = val; break;
    case 24: tf->s8  = val; break;
    case 25: tf->s9  = val; break;
    case 26: tf->s10 = val; break;
    case 27: tf->s11 = val; break;
    case 28: t->t3   = val; break;
    case 29: t->t4   = val; break;
    case 30: t->t5   = val; break;
    case 31: t->t6   = val; break;
    case 32: t->sepc = val; break;
    }
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Software breakpoint management                                              */
/* ──────────────────────────────────────────────────────────────────────────── */

/*
 * Read / write user-space instruction memory.
 * Uses vm_copyin / vm_copyout on the target's address space.
 */
static int gdb_read_user_mem(void *dst, uint64 addr, int len)
{
    if (!gdb.target || !gdb.target->vm)
        return -1;
    return vm_copyin(gdb.target->vm, dst, addr, len);
}

static int gdb_write_user_mem(uint64 addr, const void *src, int len)
{
    if (!gdb.target || !gdb.target->vm)
        return -1;

    /*
     * We cannot use vm_copyout() because it checks PROT_WRITE on the VMA.
     * Text segments are mapped read-only + execute, so vm_copyout fails.
     * We also cannot just use walkaddr() because pages may not be faulted
     * in yet (demand paging / file-backed text).
     *
     * Solution: use vma_validate(PROT_READ) to trigger demand paging
     * (which the VMA *does* allow), then write directly via the PA.
     */
    vm_t *vm = gdb.target->vm;
    const char *s = (const char *)src;
    uint64 dstva = addr;
    int remaining = len;

    vm_rlock(vm);
    while (remaining > 0) {
        uint64 va0 = PGROUNDDOWN(dstva);

        /* Ensure the page is faulted in (demand paging). */
        vma_t *vma = vm_find_area(vm, va0);
        if (vma == NULL) {
            vm_runlock(vm);
            GDB_DBG("write_mem: no vma for va %lx", va0);
            return -1;
        }
        /* Drop rlock — vma_validate may sleep for file I/O. */
        vm_runlock(vm);

        int vr = vma_validate(vma, va0, PGSIZE, VMA_FLAG_USER | PROT_READ);
        if (vr != 0) {
            GDB_DBG("write_mem: vma_validate failed for va %lx, err %d", va0, vr);
            return -1;
        }

        /* Page is now mapped. Walk to get the PA. */
        pte_t *pte = walk(vm->pagetable, va0, 0, NULL, NULL);
        if (pte == NULL || !(*pte & PTE_V)) {
            GDB_DBG("write_mem: walk failed for va %lx pte=%p", va0, pte);
            return -1;
        }
        uint64 pa0 = PTE2PA(*pte);

        uint64 off = dstva - va0;
        uint64 n = PGSIZE - off;
        if (n > (uint64)remaining)
            n = remaining;
        memmove((void *)(pa0 + off), s, n);
        remaining -= n;
        s += n;
        dstva = va0 + PGSIZE;

        vm_rlock(vm);
    }
    vm_runlock(vm);

    /* Flush I-cache so the CPU fetches the updated instruction */
    asm volatile("fence.i");
    return 0;
}

static int gdb_insert_bp(uint64 addr, int len)
{
    GDB_DBG("insert bp addr=0x%lx len=%d", addr, len);
    if (gdb.nbps >= GDB_MAX_BREAKPOINTS)
        return -1;

    /* Check for duplicates */
    for (int i = 0; i < gdb.nbps; i++) {
        if (gdb.bps[i].addr == addr && gdb.bps[i].active)
            return 0;  /* already there */
    }

    /* Read original instruction */
    uint32 orig = 0;
    if (gdb_read_user_mem(&orig, addr, len) != 0)
        return -1;

    /* Write EBREAK */
    uint32 brk = (len == 2) ? RV_C_EBREAK : RV_EBREAK;
    if (gdb_write_user_mem(addr, &brk, len) != 0)
        return -1;

    /* Record it */
    int slot = -1;
    for (int i = 0; i < GDB_MAX_BREAKPOINTS; i++) {
        if (!gdb.bps[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;

    gdb.bps[slot].addr     = addr;
    gdb.bps[slot].orig_insn = orig;
    gdb.bps[slot].len      = (uint8)len;
    gdb.bps[slot].active   = 1;
    if (slot >= gdb.nbps) gdb.nbps = slot + 1;

    return 0;
}

static int gdb_remove_bp(uint64 addr, int len)
{
    GDB_DBG("remove bp addr=0x%lx len=%d", addr, len);
    for (int i = 0; i < gdb.nbps; i++) {
        if (gdb.bps[i].addr == addr && gdb.bps[i].active) {
            /* Restore original instruction */
            gdb_write_user_mem(addr, &gdb.bps[i].orig_insn, gdb.bps[i].len);
            gdb.bps[i].active = 0;
            return 0;
        }
    }
    return -1;  /* not found */
}

/* Remove all breakpoints (on detach / kill). */
static void gdb_remove_all_bps(void)
{
    for (int i = 0; i < gdb.nbps; i++) {
        if (gdb.bps[i].active) {
            gdb_write_user_mem(gdb.bps[i].addr,
                               &gdb.bps[i].orig_insn,
                               gdb.bps[i].len);
            gdb.bps[i].active = 0;
        }
    }
    gdb.nbps = 0;
}

/* Clean up step breakpoint if it was placed. */
static void gdb_clean_step_bp(void)
{
    if (gdb.stepping) {
        gdb_write_user_mem(gdb.step_bp_addr,
                           &gdb.step_bp_orig,
                           gdb.step_bp_len);
        gdb.stepping = 0;
    }
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Instruction decoding for single-step                                        */
/*                                                                             */
/* We decode branches/jumps to find the possible next PC(s) and insert         */
/* temporary breakpoints there.  For non-branches we simply bp at PC+insn_len. */
/* ──────────────────────────────────────────────────────────────────────────── */

/* Sign-extend a value from bit 'bit' (0-indexed) */
static inline int64 sign_extend(uint64 val, int bit)
{
    uint64 mask = 1ULL << bit;
    return (int64)((val ^ mask) - mask);
}

/* Is a 16-bit value a compressed instruction? (bottom 2 bits != 0b11) */
static inline int is_compressed(uint16 insn)
{
    return (insn & 0x3) != 0x3;
}

/*
 * Decode the instruction at 'pc' and return the next PC.
 * For conditional branches, returns the branch-taken target
 * (we will also place a bp at pc + insn_len for the fall-through).
 * Returns 0 if we can't decode — caller should bp at pc + 4 as fallback.
 *
 * Sets *insn_len_out to the length of the current instruction (2 or 4).
 * Sets *is_branch_out to 1 if the instruction is a conditional branch
 * (meaning we need breakpoints at BOTH next_pc AND pc + insn_len).
 */
static uint64 gdb_decode_next_pc(struct utrapframe *tf, uint64 pc,
                                 int *insn_len_out, int *is_branch_out)
{
    uint32 insn = 0;
    *is_branch_out = 0;

    if (gdb_read_user_mem(&insn, pc, 4) != 0) {
        /* Can't read — assume 4-byte, step to pc+4 */
        *insn_len_out = 4;
        return pc + 4;
    }

    uint16 lo16 = (uint16)(insn & 0xFFFF);

    if (is_compressed(lo16)) {
        /* ── Compressed instruction (2 bytes) ── */
        *insn_len_out = 2;
        uint16 ci = lo16;
        uint16 op = ci & 0x3;
        uint16 funct3 = (ci >> 13) & 0x7;

        if (op == 1) {
            if (funct3 == 5) {
                /* C.J: offset in bits [12:2], target = pc + sext(offset) */
                int32 imm = 0;
                imm |= ((ci >> 3) & 0x7) << 1;   /* [3:1] */
                imm |= ((ci >> 11) & 0x1) << 4;   /* [4] */
                imm |= ((ci >> 2) & 0x1) << 5;    /* [5] */
                imm |= ((ci >> 7) & 0x1) << 6;    /* [6] */
                imm |= ((ci >> 6) & 0x1) << 7;    /* [7] */
                imm |= ((ci >> 9) & 0x3) << 8;    /* [9:8] */
                imm |= ((ci >> 8) & 0x1) << 10;   /* [10] */
                imm |= ((ci >> 12) & 0x1) << 11;  /* [11] sign */
                imm = (int32)sign_extend(imm, 11);
                return pc + imm;
            }
            if (funct3 == 6 || funct3 == 7) {
                /* C.BEQZ / C.BNEZ */
                int32 imm = 0;
                imm |= ((ci >> 3) & 0x3) << 1;    /* [2:1] */
                imm |= ((ci >> 10) & 0x3) << 3;   /* [4:3] */
                imm |= ((ci >> 2) & 0x1) << 5;    /* [5] */
                imm |= ((ci >> 5) & 0x3) << 6;    /* [7:6] */
                imm |= ((ci >> 12) & 0x1) << 8;   /* [8] sign */
                imm = (int32)sign_extend(imm, 8);
                *is_branch_out = 1;
                /* Evaluate condition */
                int rs1_idx = 8 + ((ci >> 7) & 0x7);  /* s0..s7  = x8..x15 */
                uint64 rs1_val = gdb_get_reg(tf, rs1_idx);
                if (funct3 == 6) {
                    /* C.BEQZ: branch if rs1 == 0 */
                    return (rs1_val == 0) ? pc + imm : pc + 2;
                } else {
                    /* C.BNEZ: branch if rs1 != 0 */
                    return (rs1_val != 0) ? pc + imm : pc + 2;
                }
            }
        }
        if (op == 2) {
            if (funct3 == 4) {
                uint16 bit12 = (ci >> 12) & 1;
                uint16 rs1 = (ci >> 7) & 0x1f;
                uint16 rs2 = (ci >> 2) & 0x1f;
                if (bit12 == 0 && rs2 == 0 && rs1 != 0) {
                    /* C.JR: jalr x0, rs1, 0 */
                    return gdb_get_reg(tf, rs1) & ~1ULL;
                }
                if (bit12 == 1 && rs2 == 0 && rs1 != 0) {
                    /* C.JALR: jalr ra, rs1, 0 */
                    return gdb_get_reg(tf, rs1) & ~1ULL;
                }
            }
        }
        /* Other compressed instructions: fall through to pc+2 */
        return pc + 2;
    }

    /* ── 32-bit instruction ── */
    *insn_len_out = 4;
    uint32 opcode = insn & 0x7f;

    switch (opcode) {
    case 0x6F: {
        /* JAL: J-type */
        int32 imm = 0;
        imm |= ((insn >> 21) & 0x3FF) << 1;    /* [10:1] */
        imm |= ((insn >> 20) & 0x1) << 11;      /* [11] */
        imm |= ((insn >> 12) & 0xFF) << 12;     /* [19:12] */
        imm |= ((insn >> 31) & 0x1) << 20;      /* [20] sign */
        imm = (int32)sign_extend(imm, 20);
        return pc + imm;
    }
    case 0x67: {
        /* JALR: I-type  (opcode=1100111) */
        int rs1 = (insn >> 15) & 0x1f;
        int32 imm = (int32)(insn >> 20);
        imm = (int32)sign_extend(imm, 11);
        uint64 base = gdb_get_reg(tf, rs1);
        return (base + imm) & ~1ULL;
    }
    case 0x63: {
        /* Branch: B-type */
        int funct3 = (insn >> 12) & 0x7;
        int rs1 = (insn >> 15) & 0x1f;
        int rs2 = (insn >> 20) & 0x1f;
        int32 imm = 0;
        imm |= ((insn >> 8) & 0xF) << 1;       /* [4:1] */
        imm |= ((insn >> 25) & 0x3F) << 5;      /* [10:5] */
        imm |= ((insn >> 7) & 0x1) << 11;       /* [11] */
        imm |= ((insn >> 31) & 0x1) << 12;      /* [12] sign */
        imm = (int32)sign_extend(imm, 12);

        uint64 v1 = gdb_get_reg(tf, rs1);
        uint64 v2 = gdb_get_reg(tf, rs2);
        int taken = 0;

        switch (funct3) {
        case 0: taken = (v1 == v2);                break; /* BEQ  */
        case 1: taken = (v1 != v2);                break; /* BNE  */
        case 4: taken = ((int64)v1 < (int64)v2);  break; /* BLT  */
        case 5: taken = ((int64)v1 >= (int64)v2); break; /* BGE  */
        case 6: taken = (v1 < v2);                 break; /* BLTU */
        case 7: taken = (v1 >= v2);                break; /* BGEU */
        default: break;
        }

        *is_branch_out = 1;
        return taken ? (pc + imm) : (pc + 4);
    }
    default:
        return pc + 4;
    }
}

/*
 * Insert a temporary breakpoint for single-stepping.
 * The instruction at 'addr' is saved and replaced with EBREAK.
 */
static int gdb_place_step_bp(uint64 addr, int len)
{
    uint32 orig = 0;
    if (gdb_read_user_mem(&orig, addr, len) != 0)
        return -1;

    gdb.step_bp_addr = addr;
    gdb.step_bp_orig = orig;
    gdb.step_bp_len  = (uint8)len;

    uint32 brk = (len == 2) ? RV_C_EBREAK : RV_EBREAK;
    if (gdb_write_user_mem(addr, &brk, len) != 0)
        return -1;

    gdb.stepping = 1;
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Target process management                                                   */
/* ──────────────────────────────────────────────────────────────────────────── */

/*
 * Resolve the thread pointer for target_pid.
 * Returns 0 on success with gdb.target filled in, -1 on failure.
 */
static int gdb_resolve_target(void)
{
    if (gdb.target_pid <= 0) return -1;

    struct thread *p = NULL;
    rcu_read_lock();
    get_pid_thread(gdb.target_pid, &p);
    if (p == NULL) {
        rcu_read_unlock();
        gdb.target = NULL;
        return -1;
    }
    gdb.target = p;
    rcu_read_unlock();
    return 0;
}

/* Detach from the target process (remove all BPs, unblock if stopped). */
static void gdb_detach(void)
{
    if (gdb.attached && gdb.target) {
        gdb_clean_step_bp();
        gdb_remove_all_bps();
    }
    gdb.attached   = 0;
    gdb.target_pid = 0;
    gdb.target     = NULL;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* "monitor" command handler (qRcmd)                                           */
/*                                                                             */
/*   monitor attach <pid>                                                      */
/*   monitor detach                                                            */
/* ──────────────────────────────────────────────────────────────────────────── */

/* Decode hex-encoded text to plain text in-place. */
static int decode_hex_str(char *dst, const char *hex, int hexlen)
{
    int n = hexlen / 2;
    for (int i = 0; i < n; i++) {
        int hi = hex2nib(hex[i * 2]);
        int lo = hex2nib(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) { dst[i] = '\0'; return i; }
        dst[i] = (char)((hi << 4) | lo);
    }
    dst[n] = '\0';
    return n;
}

/* Encode plain text to hex-encoded output. */
static int encode_hex_str(char *dst, const char *src, int len)
{
    for (int i = 0; i < len; i++) {
        dst[i * 2]     = hexchars[((uint8)src[i]) >> 4];
        dst[i * 2 + 1] = hexchars[((uint8)src[i]) & 0xf];
    }
    return len * 2;
}

static void gdb_handle_monitor(const char *hex_cmd, int hex_len)
{
    char cmd[256];
    int cmdlen = decode_hex_str(cmd, hex_cmd, hex_len);
    (void)cmdlen;

    /* "attach <pid>" */
    if (strncmp(cmd, "attach ", 7) == 0) {
        int pid = (int)strtoul(cmd + 7, NULL, 10);
        if (pid <= 0) {
            char err[] = "Invalid PID\n";
            char hexout[64];
            int n = encode_hex_str(hexout, err, strlen(err));
            gdb_send_packet(hexout, n);
            return;
        }
        gdb.target_pid = pid;
        if (gdb_resolve_target() != 0) {
            char err[] = "Process not found\n";
            char hexout[64];
            int n = encode_hex_str(hexout, err, strlen(err));
            gdb_send_packet(hexout, n);
            return;
        }
        gdb.attached = 1;

        /* Wake pending process if it was waiting for a debugger */
        if (gdb.pending_pid == pid) {
            gdb.pending_pid = 0;
            sem_post(&gdb.debugger_attached);
        }

        char msg[64];
        int mlen = snprintf(msg, sizeof(msg), "Attached to pid %d\n", pid);
        char hexout[128];
        int n = encode_hex_str(hexout, msg, mlen);
        gdb_send_packet(hexout, n);
        return;
    }

    /* "detach" */
    if (strncmp(cmd, "detach", 6) == 0) {
        gdb_detach();
        char msg[] = "Detached\n";
        char hexout[32];
        int n = encode_hex_str(hexout, msg, strlen(msg));
        gdb_send_packet(hexout, n);
        return;
    }

    /* "help" or unknown */
    {
        char msg[] = "Commands:\n  attach <pid>\n  detach\n";
        char hexout[128];
        int n = encode_hex_str(hexout, msg, strlen(msg));
        gdb_send_packet(hexout, n);
    }
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* vAttach — GDB's native attach-by-PID packet                                 */
/*                                                                             */
/* Sent by GDB when the user types "attach <pid>" at the (gdb) prompt.         */
/* Format: vAttach;<pid-hex>                                                   */
/* ──────────────────────────────────────────────────────────────────────────── */

static void gdb_handle_vattach(const char *args)
{
    /* args points right after "vAttach;" — the PID in hex */
    int pid = (int)hex2u64(args, strlen(args));
    if (pid <= 0) {
        gdb_error(1);
        return;
    }

    gdb.target_pid = pid;
    if (gdb_resolve_target() != 0) {
        gdb_error(1);
        return;
    }
    gdb.attached = 1;

    printf("gdbstub: attached to pid %d\n", pid);

    /*
     * If this PID was waiting for a debugger (waitgdb()), wake it up.
     * The process will then post target_stopped and block on
     * target_resume, so we wait for that stop notification before
     * replying to GDB.
     */
    if (gdb.pending_pid == pid) {
        gdb.pending_pid = 0;
        sem_post(&gdb.debugger_attached);

        /* Now wait for the target to actually report stopped */
        sem_wait(&gdb.target_stopped);
        gdb.stop_signal = SIGTRAP;
        gdb_send("S05");
        return;
    }

    /*
     * Normal attach (process was not waiting).
     * Send a stop reply so GDB knows the target is halted.
     * We report SIGTRAP (signal 5).  The target is not *really* stopped
     * yet — it's still running.  But GDB will immediately send commands
     * (read regs, read mem) which we handle by peeking at the trapframe
     * of a running process.  When the user does "continue", we'll let
     * the process run and wait for a breakpoint.
     */
    gdb.stop_signal = SIGTRAP;
    gdb_send("S05");
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Packet dispatcher                                                           */
/* ──────────────────────────────────────────────────────────────────────────── */

static void gdb_handle_packet(const char *pkt, int len)
{
    char *out = gdb.txbuf;

    /* Trace incoming packet (truncate long ones like memory/register blobs) */
    gdb_trace_pkt("<-", pkt, len);

    switch (pkt[0]) {

    /* ── ? — Stop reason ── */
    case '?': {
        GDB_LOG("? (stop reason, attached=%d pending=%d)", gdb.attached, gdb.pending_pid);
        /*
         * If a process is waiting for a debugger (waitgdb), auto-attach
         * so that `target remote` is all the user needs.
         */
        if (!gdb.attached && gdb.pending_pid != 0) {
            int pid = gdb.pending_pid;
            gdb.target_pid = pid;
            if (gdb_resolve_target() == 0) {
                gdb.attached = 1;
                gdb.pending_pid = 0;
                printf("gdbstub: auto-attached to pid %d\n", pid);
                sem_post(&gdb.debugger_attached);
                sem_wait(&gdb.target_stopped);
                gdb_send("S05");
                break;
            }
        }
        int sig = gdb.stop_signal ? gdb.stop_signal : SIGTRAP;
        char buf[4];
        buf[0] = 'S';
        buf[1] = hexchars[(sig >> 4) & 0xf];
        buf[2] = hexchars[sig & 0xf];
        buf[3] = '\0';
        gdb_send(buf);
        break;
    }

    /* ── g — Read all registers ── */
    case 'g': {
        char *p = out;
        if (!gdb.attached || gdb_resolve_target() != 0) {
            /* No target yet — return all zeroes so GDB can connect */
            for (int i = 0; i < GDB_NUM_REGS; i++)
                p = hex_le64(p, 0);
        } else {
            struct utrapframe *tf = gdb.target->trapframe;
            for (int i = 0; i < GDB_NUM_REGS; i++)
                p = hex_le64(p, gdb_get_reg(tf, i));
        }
        gdb_send_packet(out, (int)(p - out));
        break;
    }

    /* ── G — Write all registers ── */
    case 'G': {
        if (!gdb.attached || gdb_resolve_target() != 0) {
            gdb_error(1);
            break;
        }
        const char *hex = pkt + 1;
        struct utrapframe *tf = gdb.target->trapframe;
        for (int i = 0; i < GDB_NUM_REGS; i++) {
            if (strlen(hex) < 16) break;
            gdb_set_reg(tf, i, hexle2u64(hex));
            hex += 16;
        }
        gdb_ok();
        break;
    }

    /* ── p N — Read single register ── */
    case 'p': {
        int regnum = (int)hex2u64(pkt + 1, strlen(pkt + 1));
        if (regnum < 0 || regnum >= GDB_NUM_REGS) {
            gdb_error(0);
            break;
        }
        char *p = out;
        if (!gdb.attached || gdb_resolve_target() != 0)
            p = hex_le64(p, 0);
        else
            p = hex_le64(p, gdb_get_reg(gdb.target->trapframe, regnum));
        gdb_send_packet(out, (int)(p - out));
        break;
    }

    /* ── P N=V — Write single register ── */
    case 'P': {
        if (!gdb.attached || gdb_resolve_target() != 0) {
            gdb_error(1);
            break;
        }
        const char *eq = NULL;
        for (const char *s = pkt + 1; *s; s++) {
            if (*s == '=') { eq = s; break; }
        }
        if (!eq) { gdb_error(0); break; }
        int regnum = (int)hex2u64(pkt + 1, (int)(eq - pkt - 1));
        uint64 val = hexle2u64(eq + 1);
        if (regnum >= 0 && regnum < GDB_NUM_REGS)
            gdb_set_reg(gdb.target->trapframe, regnum, val);
        gdb_ok();
        break;
    }

    /* ── m addr,len — Read memory ── */
    case 'm': {
        if (!gdb.attached || gdb_resolve_target() != 0) {
            gdb_error(0x0e);  /* no target — graceful error */
            break;
        }
        /* Parse "addr,len" */
        const char *comma = NULL;
        for (const char *s = pkt + 1; *s; s++) {
            if (*s == ',') { comma = s; break; }
        }
        if (!comma) { gdb_error(0); break; }

        uint64 addr = hex2u64(pkt + 1, (int)(comma - pkt - 1));
        uint64 mlen = hex2u64(comma + 1, strlen(comma + 1));
        if (mlen > GDB_BUF_SIZE / 2)
            mlen = GDB_BUF_SIZE / 2;

        uint8 tmp[GDB_BUF_SIZE / 2];
        if (gdb_read_user_mem(tmp, addr, (int)mlen) != 0) {
            gdb_error(0x0e);
            break;
        }
        int n = mem2hex(out, tmp, (int)mlen);
        gdb_send_packet(out, n);
        break;
    }

    /* ── M addr,len:data — Write memory ── */
    case 'M': {
        if (!gdb.attached || gdb_resolve_target() != 0) {
            gdb_error(1);
            break;
        }
        const char *comma = NULL, *colon = NULL;
        for (const char *s = pkt + 1; *s; s++) {
            if (*s == ',' && !comma) comma = s;
            if (*s == ':' && !colon) colon = s;
        }
        if (!comma || !colon) { gdb_error(0); break; }

        uint64 addr = hex2u64(pkt + 1, (int)(comma - pkt - 1));
        uint64 mlen = hex2u64(comma + 1, (int)(colon - comma - 1));
        if (mlen > GDB_BUF_SIZE / 2)
            mlen = GDB_BUF_SIZE / 2;

        uint8 tmp[GDB_BUF_SIZE / 2];
        hex2mem(tmp, colon + 1, (int)mlen);
        if (gdb_write_user_mem(addr, tmp, (int)mlen) != 0) {
            gdb_error(0x0e);
            break;
        }
        gdb_ok();
        break;
    }

    /* ── c [addr] — Continue ── */
    case 'c': {
        GDB_LOG("continue (attached=%d, nbps=%d)", gdb.attached, gdb.nbps);
        if (!gdb.attached) {
            gdb_send("S00");  /* not attached — report as stopped */
            break;
        }

#if GDB_LOG_LEVEL >= 2
        for (int i = 0; i < gdb.nbps; i++) {
            if (gdb.bps[i].active)
                GDB_DBG("  bp[%d] addr=0x%lx len=%d",
                          i, gdb.bps[i].addr, gdb.bps[i].len);
        }
#endif

        /* Optional resume address */
        if (len > 1 && gdb_resolve_target() == 0) {
            uint64 addr = hex2u64(pkt + 1, len - 1);
            gdb.target->trapframe->trapframe.sepc = addr;
        }

        /* Resume the target.  Do NOT block — return to the recv loop
         * so we can handle Ctrl-C while the target runs.  The stop
         * reply will be sent when target_stopped is posted. */
        GDB_LOG("-> resuming target pid %d", gdb.target_pid);
        gdb.target_running = 1;
        sem_post(&gdb.target_resume);
        break;
    }

    /* ── s [addr] — Single step ── */
    case 's': {
        GDB_LOG("step (attached=%d)", gdb.attached);
        if (!gdb.attached || gdb_resolve_target() != 0) {
            gdb_send("S00");  /* not attached — report as stopped */
            break;
        }

        /* Optional resume address */
        if (len > 1) {
            uint64 addr = hex2u64(pkt + 1, len - 1);
            gdb.target->trapframe->trapframe.sepc = addr;
        }

        uint64 pc = gdb.target->trapframe->trapframe.sepc;
        int insn_len = 4, is_branch = 0;
        uint64 next_pc = gdb_decode_next_pc(gdb.target->trapframe, pc,
                                             &insn_len, &is_branch);

        /* For branches, we know the direction based on register values.
         * Place step bp at the computed next_pc. */
        uint16 lo16 = 0;
        if (gdb_read_user_mem(&lo16, next_pc, 2) == 0 && is_compressed(lo16))
            gdb_place_step_bp(next_pc, 2);
        else
            gdb_place_step_bp(next_pc, 4);

        /* Resume the target.  Non-blocking, like 'c'. */
        GDB_DBG("-> stepping target pid %d, next_pc=0x%lx", gdb.target_pid, next_pc);
        gdb.target_running = 1;
        sem_post(&gdb.target_resume);
        break;
    }

    /* ── Z type,addr,kind — Insert breakpoint ── */
    case 'Z': {
        if (!gdb.attached || gdb_resolve_target() != 0) {
            gdb_empty();  /* not attached — unsupported */
            break;
        }
        int bptype = hex2nib(pkt[1]);
        if (bptype != 0) { gdb_empty(); break; }  /* only sw breakpoints */

        const char *comma1 = NULL, *comma2 = NULL;
        for (const char *s = pkt + 2; *s; s++) {
            if (*s == ',' && !comma1) comma1 = s;
            else if (*s == ',' && !comma2) comma2 = s;
        }
        if (!comma1 || !comma2) { gdb_error(0); break; }

        uint64 addr = hex2u64(comma1 + 1, (int)(comma2 - comma1 - 1));
        int bplen = (int)hex2u64(comma2 + 1, strlen(comma2 + 1));
        if (bplen != 2 && bplen != 4) bplen = 4;

        if (gdb_insert_bp(addr, bplen) == 0)
            gdb_ok();
        else
            gdb_error(0x0e);
        break;
    }

    /* ── z type,addr,kind — Remove breakpoint ── */
    case 'z': {
        if (!gdb.attached || gdb_resolve_target() != 0) {
            gdb_empty();  /* not attached — unsupported */
            break;
        }
        int bptype = hex2nib(pkt[1]);
        if (bptype != 0) { gdb_empty(); break; }

        const char *comma1 = NULL, *comma2 = NULL;
        for (const char *s = pkt + 2; *s; s++) {
            if (*s == ',' && !comma1) comma1 = s;
            else if (*s == ',' && !comma2) comma2 = s;
        }
        if (!comma1 || !comma2) { gdb_error(0); break; }

        uint64 addr = hex2u64(comma1 + 1, (int)(comma2 - comma1 - 1));
        int bplen = (int)hex2u64(comma2 + 1, strlen(comma2 + 1));
        if (bplen != 2 && bplen != 4) bplen = 4;

        if (gdb_remove_bp(addr, bplen) == 0)
            gdb_ok();
        else
            gdb_error(0x0e);
        break;
    }

    /* ── H op thread-id — Set thread ── */
    case 'H':
        /* We only support one thread (the target PID). Just ACK. */
        gdb_ok();
        break;

    /* ── D — Detach ── */
    case 'D':
        GDB_LOG("detach");
        gdb_detach();
        gdb_ok();
        break;

    /* ── k — Kill ── */
    case 'k':
        GDB_LOG("kill (pid=%d)", gdb.target_pid);
        if (gdb.attached && gdb.target_pid > 0) {
            gdb_remove_all_bps();
            kill(gdb.target_pid, SIGKILL);
            /* Resume the target so it can process the signal */
            sem_post(&gdb.target_resume);
        }
        gdb.attached = 0;
        gdb.target = NULL;
        /* Don't send reply — GDB closes connection after 'k'. */
        break;

    /* ── q — Query packets ── */
    case 'q': {
        if (strncmp(pkt, "qSupported", 10) == 0) {
            gdb_send("PacketSize=1000;swbreak+;vContSupported+");
        } else if (strcmp(pkt, "qAttached") == 0) {
            gdb_send("1");  /* always "attached" */
        } else if (strcmp(pkt, "qC") == 0) {
            /* Current thread = target PID */
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "QCp%x.%x",
                             gdb.target_pid > 0 ? gdb.target_pid : 1,
                             gdb.target_pid > 0 ? gdb.target_pid : 1);
            gdb_send_packet(buf, n);
        } else if (strcmp(pkt, "qfThreadInfo") == 0) {
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "mp%x.%x",
                             gdb.target_pid > 0 ? gdb.target_pid : 1,
                             gdb.target_pid > 0 ? gdb.target_pid : 1);
            gdb_send_packet(buf, n);
        } else if (strcmp(pkt, "qsThreadInfo") == 0) {
            gdb_send("l");  /* end of thread list */
        } else if (strncmp(pkt, "qRcmd,", 6) == 0) {
            gdb_handle_monitor(pkt + 6, len - 6);
        } else if (strcmp(pkt, "qTStatus") == 0) {
            gdb_send("T0");  /* no trace running */
        } else if (strncmp(pkt, "qXfer", 5) == 0) {
            gdb_empty(); /* not supported */
        } else {
            gdb_empty();
        }
        break;
    }

    /* ── v — Extended packets ── */
    case 'v': {
        if (strncmp(pkt, "vAttach;", 8) == 0) {
            gdb_handle_vattach(pkt + 8);
        } else if (strncmp(pkt, "vCont?", 6) == 0) {
            gdb_send("vCont;c;s");
        } else if (strncmp(pkt, "vCont;", 6) == 0) {
            /* Parse vCont;c or vCont;s */
            const char *act = pkt + 6;
            if (act[0] == 's') {
                /* Reuse 's' handler */
                char fake[2] = {'s', '\0'};
                gdb_handle_packet(fake, 1);
            } else {
                /* Default: continue */
                char fake[2] = {'c', '\0'};
                gdb_handle_packet(fake, 1);
            }
        } else if (strcmp(pkt, "vMustReplyEmpty") == 0) {
            gdb_empty();
        } else {
            gdb_empty();
        }
        break;
    }

    default:
        gdb_empty();
        break;
    }
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Trap hook — called from usertrap() on EBREAK                                */
/*                                                                             */
/* Runs in the context of the target process's thread.  We signal the GDB      */
/* network thread, then block until it says "continue" or "step".              */
/* ──────────────────────────────────────────────────────────────────────────── */

/*
 * gdbstub_trap() — called from usertrap() when scause == RISCV_BREAKPOINT_TRAP.
 *
 * Returns:
 *   0  — breakpoint was handled by GDB stub, process should resume normally.
 *  -1  — not our breakpoint (no debugger attached for this pid), caller
 *         should deliver SIGTRAP/SIGSEGV as usual.
 */
int gdbstub_trap(struct thread *t)
{
    GDB_LOG("trap: pid=%d pc=0x%lx attached=%d target_pid=%d",
              t->pid, t->trapframe->trapframe.sepc, gdb.attached, gdb.target_pid);
    /* Is this the process we're debugging? */
    if (!gdb.attached || t->pid != gdb.target_pid) {
        /*
         * No debugger attached for this PID.  If no other process is
         * already waiting, block here until a GDB client connects and
         * attaches to our PID.  This implements the waitgdb() helper.
         */
        if (gdb.pending_pid != 0) {
            /* Another process is already waiting — can't queue two. */
            return -1;
        }

        gdb.pending_pid = t->pid;
        printf("gdbstub: pid %d (%s) waiting for debugger on port %d\n"
               "  (gdb) target remote <host>:%d\n"
               "  (gdb) attach %d\n",
               t->pid, t->name, GDBSTUB_PORT,
               GDBSTUB_PORT, t->pid);

        /* Block until GDB attaches to us */
        sem_wait(&gdb.debugger_attached);

        /* GDB has attached — fall through to normal stop handling. */
    }

    uint64 pc = t->trapframe->trapframe.sepc;

    /* Check if this is one of our breakpoints or a step bp. */
    int is_our_bp = 0;
    if (gdb.stepping && pc == gdb.step_bp_addr) {
        is_our_bp = 1;
    } else {
        for (int i = 0; i < gdb.nbps; i++) {
            if (gdb.bps[i].active && gdb.bps[i].addr == pc) {
                is_our_bp = 1;
                break;
            }
        }
    }

    /*
     * If this is NOT one of our software breakpoints, it's a user EBREAK
     * (e.g. waitgdb() or __builtin_trap).  Advance sepc past the
     * instruction so we don't re-execute it on continue.
     * Our own breakpoints don't need this because we restore the original
     * instruction before resuming.
     */
    if (!is_our_bp) {
        uint16 lo16 = 0;
        if (gdb_read_user_mem(&lo16, pc, 2) == 0 && is_compressed(lo16)) {
            GDB_DBG("trap: user EBREAK (compressed) at 0x%lx, advancing sepc +2", pc);
            t->trapframe->trapframe.sepc += 2;
        } else {
            GDB_DBG("trap: user EBREAK at 0x%lx, advancing sepc +4", pc);
            t->trapframe->trapframe.sepc += 4;
        }
    } else {
        GDB_DBG("trap: our bp at 0x%lx (step=%d)", pc, gdb.stepping);
    }

    /* Update state for the GDB thread */
    gdb.target = t;
    gdb.stop_signal = SIGTRAP;

    /* Notify the GDB thread that the target has stopped */
    sem_post(&gdb.target_stopped);

    /* Block until GDB says continue or step */
    sem_wait(&gdb.target_resume);

    /*
     * Flush this hart's I-cache.  The GDB stub thread may have
     * written EBREAK instructions (or restored originals) into our
     * text segment from a *different* hart.  RISC-V fence.i is
     * local, so we must execute it here — on the hart that will
     * actually fetch the (possibly modified) instructions.
     */
    asm volatile("fence.i");

    return 0;
}

/*
 * gdbstub_exec_stop() — called from exec() after the new program is loaded.
 *
 * If the current process is being debugged, stop it so GDB sees the new
 * address space and can set breakpoints in the freshly-loaded binary.
 *
 * This runs in syscall (exec) context, not trap context.
 */
void gdbstub_exec_stop(struct thread *t)
{
    if (!gdb.attached || t->pid != gdb.target_pid)
        return;

    /* Remove any stale breakpoints from the old address space */
    gdb.nbps = 0;
    memset(gdb.bps, 0, sizeof(gdb.bps));
    gdb_clean_step_bp();

    gdb.target = t;
    gdb.stop_signal = SIGTRAP;

    printf("gdbstub: pid %d exec -> %s, stopped for debugger\n",
           t->pid, t->name);

    /* Notify the GDB thread that the target has stopped */
    sem_post(&gdb.target_stopped);

    /* Block until GDB says continue or step */
    sem_wait(&gdb.target_resume);

    /* Flush I-cache — GDB may have inserted breakpoints from another hart. */
    asm volatile("fence.i");
}

/*
 * gdbstub_check_interrupt() — called from usertrapret() before returning
 * to user space.
 *
 * If GDB sent Ctrl-C (0x03) while the target was running, this function
 * stops the process so GDB can inspect it.  Returns 0 if no action was
 * taken, 1 if the process was stopped and resumed by GDB.
 */
int gdbstub_check_interrupt(struct thread *t)
{
    if (!gdb.attached || t->pid != gdb.target_pid)
        return 0;

    if (!gdb.interrupt_pending)
        return 0;

    gdb.interrupt_pending = 0;
    __sync_synchronize();

    GDB_LOG("check_interrupt: stopping pid %d", t->pid);

    gdb.target = t;
    gdb.stop_signal = SIGINT;

    /* Notify the GDB thread that the target has stopped */
    sem_post(&gdb.target_stopped);

    /* Block until GDB says continue or step */
    sem_wait(&gdb.target_resume);

    /* Flush I-cache — GDB may have inserted breakpoints from another hart. */
    asm volatile("fence.i");

    return 1;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Session handler — one GDB connection                                        */
/* ──────────────────────────────────────────────────────────────────────────── */

static void gdb_session(struct netconn *conn)
{
    gdb.conn = conn;
    gdb.attached = 0;
    gdb.target_pid = 0;
    gdb.target = NULL;
    gdb.stop_signal = 0;
    gdb.stepping = 0;
    gdb.nbps = 0;
    gdb.interrupt_pending = 0;
    gdb.target_running = 0;
    gdb.rxpbuf = NULL;
    gdb.rxpoff = 0;
    memset(gdb.bps, 0, sizeof(gdb.bps));

    /* Disable Nagle's algorithm so small packets are sent immediately. */
    tcp_nagle_disable(conn->pcb.tcp);

    printf("gdbstub: client connected\n");

    for (;;) {
        /*
         * While the target is running we need to simultaneously:
         *  (a) receive Ctrl-C (0x03) from GDB, and
         *  (b) detect when the target stops (breakpoint / interrupt).
         *
         * The lwIP netconn_set_recvtimeout does NOT work in this
         * port (sys_arch_mbox_fetch ignores the timeout), so we
         * use true non-blocking recv + sleep to poll.
         */

        /* ── Check for target stop ─────────────────────────────── */
        if (gdb.target_running && sem_trywait(&gdb.target_stopped) == 0) {
            gdb.target_running = 0;
            GDB_LOG("target stopped (sig=%d)", gdb.stop_signal);
            gdb_clean_step_bp();
            if (gdb.target && gdb.target->sched_entity) {
                while (smp_load_acquire(&gdb.target->sched_entity->on_cpu))
                    ;
            }
            if (gdb_resolve_target() != 0) {
                gdb_send("X09");
                gdb.attached = 0;
            } else {
                int sig = gdb.stop_signal;
                char buf[4];
                buf[0] = 'S';
                buf[1] = hexchars[(sig >> 4) & 0xf];
                buf[2] = hexchars[sig & 0xf];
                buf[3] = '\0';
                gdb_send(buf);
            }
            continue;
        }

        /* ── Receive data from GDB ─────────────────────────────── */
        int pktlen;

        if (gdb.target_running) {
            /*
             * Target is running — use non-blocking recv so we can
             * poll target_stopped between recv attempts.  If no
             * data is available, sleep briefly to avoid busy-wait.
             */
            netconn_set_nonblocking(conn, 1);
            pktlen = gdb_recv_packet();
            netconn_set_nonblocking(conn, 0);

            if (pktlen < 0) {
                sleep_ms(5);
                continue;   /* back to top to check target_stopped */
            }
        } else {
            /* Target is stopped — block for GDB's next command. */
            pktlen = gdb_recv_packet();
            if (pktlen < 0)
                break;  /* disconnect */
        }

        /* ── Handle Ctrl-C ─────────────────────────────────────── */
        if (pktlen == 1 && gdb.rxbuf[0] == 0x03) {
            GDB_LOG("Ctrl-C received (attached=%d running=%d)",
                      gdb.attached, gdb.target_running);
            if (gdb.attached && gdb.target_running) {
                gdb.interrupt_pending = 1;
                __sync_synchronize();

                struct thread *t = NULL;
                get_pid_thread(gdb.target_pid, &t);
                if (t && t->sched_entity) {
                    int target_cpu = smp_load_acquire(&t->sched_entity->cpu_id);
                    if (smp_load_acquire(&t->sched_entity->on_cpu))
                        ipi_send_single(target_cpu, IPI_REASON_RESCHEDULE);
                }
            }
            continue;
        }

        gdb_handle_packet(gdb.rxbuf, pktlen);
    }

    /* Client disconnected — clean up */
    if (gdb.rxpbuf) {
        pbuf_free(gdb.rxpbuf);
        gdb.rxpbuf = NULL;
    }

    if (gdb.attached) {
        gdb_detach();
        /* Resume the target if it was stopped */
        sem_post(&gdb.target_resume);
    }

    printf("gdbstub: client disconnected\n");
    netconn_close(conn);
    netconn_delete(conn);
    gdb.conn = NULL;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Listener thread — accepts GDB connections on TCP port 1234                  */
/* ──────────────────────────────────────────────────────────────────────────── */

static void gdbstub_listener(uint64 arg1, uint64 arg2)
{
    (void)arg1;
    (void)arg2;

    struct netconn *listener = netconn_new(NETCONN_TCP);
    if (listener == NULL) {
        printf("gdbstub: netconn_new failed\n");
        return;
    }

#if SO_REUSE
    ip_set_option(listener->pcb.ip, SOF_REUSEADDR);
#endif

    err_t err = netconn_bind(listener, IP_ADDR_ANY, GDBSTUB_PORT);
    if (err != ERR_OK) {
        printf("gdbstub: bind to port %d failed: %d\n", GDBSTUB_PORT, err);
        netconn_delete(listener);
        return;
    }

    err = netconn_listen_with_backlog(listener, 1);
    if (err != ERR_OK) {
        printf("gdbstub: listen failed: %d\n", err);
        netconn_delete(listener);
        return;
    }

    printf("gdbstub: listening on port %d\n", GDBSTUB_PORT);

    for (;;) {
        struct netconn *client = NULL;
        err = netconn_accept(listener, &client);
        if (err != ERR_OK) {
            if (err == ERR_ABRT || err == ERR_CLSD)
                break;
            sleep_ms(100);
            continue;
        }

        ip_addr_t raddr;
        u16_t rport;
        netconn_getaddr(client, &raddr, &rport, 0);
        printf("gdbstub: connection from %d.%d.%d.%d:%d\n",
               ip4_addr1_16(ip_2_ip4(&raddr)),
               ip4_addr2_16(ip_2_ip4(&raddr)),
               ip4_addr3_16(ip_2_ip4(&raddr)),
               ip4_addr4_16(ip_2_ip4(&raddr)),
               rport);

        /* Only one GDB session at a time */
        gdb_session(client);
    }
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Module init — called from start_kernel.c                                    */
/* ──────────────────────────────────────────────────────────────────────────── */

void gdbstub_init(void)
{
    spin_init(&gdb.lock, "gdbstub");
    sem_init(&gdb.target_stopped, "gdb_stopped", 0);
    sem_init(&gdb.target_resume, "gdb_resume", 0);
    sem_init(&gdb.debugger_attached, "gdb_dbgattach", 0);
    gdb.pending_pid = 0;

    struct thread *t = kthread_create("gdbstub", gdbstub_listener, 0, 0,
                                       KERNEL_STACK_ORDER);
    if (IS_ERR_OR_NULL(t)) {
        printf("gdbstub: failed to create listener thread\n");
        return;
    }
    wakeup(t);
    printf("gdbstub: daemon started\n");
}
