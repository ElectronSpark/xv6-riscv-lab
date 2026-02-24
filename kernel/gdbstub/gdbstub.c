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
 *   H / qC       — thread-id operations (multi-threaded, thread groups)
 *   qSupported   — feature negotiation (multiprocess+, qXfer:threads:read+)
 *   qAttached    — always "1" (attached, not spawned)
 *   qfThreadInfo — list all threads in the attached process (thread group)
 *   qXfer:threads:read — XML thread listing with core affinity
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
#include "gdbstub_arch.h"
#include "trap.h"
#include "proc/thread.h"
#include "arch_thread.h"
#include "proc/thread_group.h"
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

/* Number of GP registers is architecture-dependent (see gdbstub_arch.h).
 * gdb_arch_num_regs is defined in the per-arch gdbstub_arch.c. */

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
    int              target_pid;      /* TGID of the debugged process */
    struct thread   *target;          /* thread that stopped (trap ctx) */
    int              attached;        /* 1 if GDB is attached to a process */

    /* Thread selection (Hg / Hc packets).
     * g_tid  — TID selected for register/memory reads (Hg).
     * c_tid  — TID selected for continue/step (Hc).
     * Value of 0  means "any thread" (use target that last stopped).
     * Value of -1 means "all threads". */
    int              g_tid;
    int              c_tid;

    /* Synchronization: target stops on breakpoint, GDB resumes it */
    sem_t            target_stopped;  /* posted when target hits a bp */
    sem_t            target_resume;   /* posted when GDB says continue */

    /* Stop reason communicated from trap handler to GDB thread */
    int              stop_signal;     /* e.g. 5 = SIGTRAP */
    int              stop_tid;        /* TID of thread that stopped */
    int              stop_is_swbreak; /* 1 if stopped at our sw bp */

    /* Single-step state */
    int              stepping;        /* 1 if sw single-stepping */
    int              hw_stepping;     /* 1 if hw single-stepping (x86 TF) */
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

    /* Non-zero when the target was stopped directly by the gdbstub
     * thread (e.g. Ctrl-C while the thread was sleeping in a syscall).
     * In this state the target thread never blocked on target_resume,
     * so 'c'/'s' must NOT post target_resume. */
    int              direct_stop;

    /* Set by gdbstub_exit_stop() when a debugged thread/process exits.
     * The GDB session loop checks this after target_stopped fires. */
    volatile int     target_exited;    /* 1 = process exited */
    int              exit_code;        /* exit status (for W packet) */
    int              exit_tid;         /* TID of thread that exited */
    volatile int     thread_exited;    /* 1 = individual thread exit */
    int              thread_exit_tid;  /* TID of exited thread */
    int              thread_exit_code; /* exit code for thread */
    volatile int     process_killed;   /* 1 = killed by signal */
    int              kill_signal;      /* signal that killed it */

    /* When set, gdbstub_exec_stop() will stop the process at the
     * entry point of the new program after exec().  Defaults to 1
     * on attach; waitgdb() clears it (a0==0), waitgdb -e keeps it
     * (a0==1).  This avoids an unwanted extra stop when the user
     * only wants to set breakpoints and continue past exec. */
    int              stop_on_exec;

    /* Exec event: set by gdbstub_exec_stop() with the new binary name.
     * The session loop includes exec:<hexpath> in the stop reply so
     * GDB reloads symbols and discards stale breakpoints. */
    volatile int     exec_event;       /* 1 = exec happened */
    char             exec_name[128];   /* name of the new binary */

    /* Auto-inserted entry-point breakpoint after exec.  GDB auto-
     * continues after receiving an exec event, so we insert a one-shot
     * breakpoint at the program's actual entry point (_start) to force
     * a stop.  gdbstub_trap() auto-removes it when hit. */
    int              exec_entry_bp_active;
    uint64           exec_entry_bp_addr;

    /* Set by gdbstub_signal_stop() when the debugger already inspected
     * the process at the fault point.  gdbstub_exit_stop() checks this
     * to skip its own Phase-1 inspection stop (the trapframe has been
     * modified by signal delivery by then, so stopping again would
     * show the wrong PC). */
    volatile int     signal_stopped;

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
/* Encode a value as little-endian hex with variable byte width.
 * For nbytes > 8, bytes beyond the uint64 range are emitted as 0x00. */
static char *hex_le(char *p, uint64 val, int nbytes)
{
    for (int i = 0; i < nbytes; i++) {
        uint8 b = (i < 8) ? (uint8)(val >> (i * 8)) : 0;
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

/* Parse a little-endian hex-encoded value with variable byte width.
 * For nbytes > 8, only the first 8 bytes are decoded into the uint64. */
static uint64 hexle2val(const char *p, int nbytes)
{
    uint64 val = 0;
    int max = nbytes < 8 ? nbytes : 8;
    for (int i = 0; i < max; i++) {
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
/* Register access — delegated to arch-specific gdbstub_arch.c                 */
/*                                                                             */
/* gdb_arch_get_reg() / gdb_arch_set_reg() in gdbstub_arch.h                   */
/* ──────────────────────────────────────────────────────────────────────────── */

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

        /* Page is now mapped. Resolve VA->PA through arch VM helpers. */
        uint64 pa0 = walkaddr(vm->pagetable, va0);
        if (pa0 == 0) {
            GDB_DBG("write_mem: walkaddr failed for va %lx", va0);
            return -1;
        }

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

    /*
     * Flush I-cache on ALL harts that may run this process.
     * The target thread could be scheduled on any hart after
     * resuming, and gdb_write_user_mem runs on the gdbstub hart.
     * RISC-V fence.i is local — we need SBI remote fence.i to
     * cover all harts.  The target also does fence.i before
     * resuming (belt-and-suspenders).
     */
    vm_remote_fence_i(vm);
    return 0;
}

static int gdb_insert_bp(uint64 addr, int len)
{
    GDB_LOG("insert bp addr=0x%lx len=%d", addr, len);
    if (gdb.nbps >= GDB_MAX_BREAKPOINTS)
        return -1;

    /* Check for duplicates */
    for (int i = 0; i < gdb.nbps; i++) {
        if (gdb.bps[i].addr == addr && gdb.bps[i].active) {
            GDB_LOG("  -> duplicate (slot %d)", i);
            return 0;  /* already there */
        }
    }

    /* Read original instruction */
    uint32 orig = 0;
    if (gdb_read_user_mem(&orig, addr, len) != 0) {
        GDB_LOG("  -> read orig FAILED");
        return -1;
    }

    /* Write breakpoint instruction */
    uint32 brk = 0;
    int brk_len = gdb_arch_brk_encode(&brk, len);
    if (gdb_write_user_mem(addr, &brk, brk_len) != 0) {
        GDB_LOG("  -> write breakpoint FAILED");
        return -1;
    }

    /* Read-back verification: ensure breakpoint was actually written */
    uint32 verify = 0;
    if (gdb_read_user_mem(&verify, addr, brk_len) != 0) {
        GDB_LOG("  -> verify read-back FAILED");
        return -1;
    }
    uint32 mask = (brk_len == 1) ? 0xFFU
               : (brk_len == 2) ? 0xFFFFU : 0xFFFFFFFFU;
    if ((verify & mask) != (brk & mask)) {
        GDB_LOG("  -> VERIFY MISMATCH at 0x%lx: wrote 0x%x read 0x%x",
                addr, brk & mask, verify & mask);
        return -1;
    }

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

    GDB_LOG("  -> ok slot=%d orig=0x%x", slot, orig);
    return 0;
}

static int gdb_remove_bp(uint64 addr, int len)
{
    GDB_LOG("remove bp addr=0x%lx len=%d", addr, len);
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
/* Instruction decoding for single-step — delegated to arch-specific code.     */
/*                                                                             */
/* gdb_arch_decode_next_pc() in gdbstub_arch.h / gdbstub_arch.c.               */
/* On x86_64, hardware single-step (RFLAGS.TF) is used instead.               */
/* ──────────────────────────────────────────────────────────────────────────── */

/*
 * Insert a temporary breakpoint for single-stepping (software step).
 * The instruction at 'addr' is saved and replaced with a breakpoint.
 * Only used when gdb_arch_has_hw_step() returns 0.
 */
static int gdb_place_step_bp(uint64 addr, int len)
{
    uint32 orig = 0;
    if (gdb_read_user_mem(&orig, addr, len) != 0)
        return -1;

    gdb.step_bp_addr = addr;
    gdb.step_bp_orig = orig;
    gdb.step_bp_len  = (uint8)len;

    uint32 brk = 0;
    gdb_arch_brk_encode(&brk, len);
    if (gdb_write_user_mem(addr, &brk, len) != 0)
        return -1;

    gdb.stepping = 1;
    return 0;
}

/*
 * gdb_step_over_bp() — Step over a software breakpoint at the current PC.
 *
 * When `swbreak+` is advertised, the stub is responsible for transparently
 * stepping past a breakpoint on continue/step.  GDB does NOT send z0/Z0
 * packets around continues.
 *
 * If the target's PC matches an active breakpoint, this function:
 *   1. Restores the original instruction at the breakpoint address
 *   2. Single-steps one instruction (hw TF or software decode+bp)
 *   3. Waits for the target to stop
 *   4. Re-inserts the breakpoint at the original address
 *
 * After return the target is stopped (waiting on target_resume) at the
 * instruction after the breakpoint.
 *
 * Returns:
 *   1 — step-over was performed
 *   0 — no step-over needed (PC not at a breakpoint)
 *  -1 — target exited/died during the step-over
 */
static int gdb_step_over_bp(void)
{
    if (!gdb.target || !gdb.target->trapframe)
        return 0;

    uint64 pc = gdb_arch_get_pc(gdb.target->trapframe);

    /* Find the breakpoint at the current PC */
    struct gdb_breakpoint *bp_at_pc = NULL;
    for (int i = 0; i < gdb.nbps; i++) {
        if (gdb.bps[i].active && gdb.bps[i].addr == pc) {
            bp_at_pc = &gdb.bps[i];
            break;
        }
    }
    if (!bp_at_pc)
        return 0;  /* PC is not at a breakpoint — nothing to do */

    GDB_DBG("step-over bp at 0x%lx (len=%d)", pc, bp_at_pc->len);

    /* 1. Restore the original instruction */
    gdb_write_user_mem(pc, &bp_at_pc->orig_insn, bp_at_pc->len);

    /* 2. Set up single-step: hardware TF or software decode+bp */
    if (gdb_arch_has_hw_step()) {
        gdb.hw_stepping = 1;
        gdb_arch_set_hw_step(gdb.target->trapframe, 1);
    } else {
        int insn_len = 4, is_branch = 0;
        uint64 next_pc = gdb_arch_decode_next_pc(gdb.target->trapframe, pc,
                                                  &insn_len, &is_branch,
                                                  gdb_read_user_mem);
        uint16 lo16 = 0;
        int bp_len = 4;
        if (gdb_read_user_mem(&lo16, next_pc, 2) == 0)
            bp_len = gdb_arch_brk_len(next_pc, &lo16, 2);
        gdb_place_step_bp(next_pc, bp_len);
    }

    /* 3. Resume the target for one instruction */
    sem_post(&gdb.target_resume);

    /* 4. Wait for the target to stop */
    sem_wait(&gdb.target_stopped);

    /* Check for abnormal stops (exit / kill / thread exit) */
    if (gdb.target_exited || gdb.process_killed || gdb.thread_exited) {
        if (gdb_arch_has_hw_step()) {
            gdb_arch_set_hw_step(gdb.target->trapframe, 0);
            gdb.hw_stepping = 0;
        } else
            gdb_clean_step_bp();
        /* Re-post so the main loop can handle the event */
        sem_post(&gdb.target_stopped);
        return -1;
    }

    /* 5. Clean up single-step state */
    if (gdb_arch_has_hw_step()) {
        gdb_arch_set_hw_step(gdb.target->trapframe, 0);
        gdb.hw_stepping = 0;
    } else
        gdb_clean_step_bp();

    /* 6. Re-insert breakpoint at the original address */
    uint32 brk = 0;
    int brk_len = gdb_arch_brk_encode(&brk, bp_at_pc->len);
    gdb_write_user_mem(bp_at_pc->addr, &brk, brk_len);

    GDB_DBG("step-over complete, target now at 0x%lx",
              gdb_arch_get_pc(gdb.target->trapframe));
    return 1;
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

/*
 * Resolve the thread selected by Hg (for register / memory reads).
 * Falls back to gdb.target (the thread that last stopped).
 */
static struct thread *gdb_selected_thread(void)
{
    int tid = gdb.g_tid;
    if (tid <= 0) {
        /* 0 = "any", -1 = "all" — just use last-stopped thread */
        return gdb.target;
    }
    struct thread *p = NULL;
    rcu_read_lock();
    get_pid_thread(tid, &p);
    rcu_read_unlock();
    if (p && thread_tgid(p) == gdb.target_pid)
        return p;
    /* TID not in our thread group — fall back */
    return gdb.target;
}

/*
 * Send a T-stop reply:  T05thread:p<tgid>.<tid>;[swbreak:;]
 * This is the multi-process-aware stop reply that includes the
 * thread that stopped and the stop reason.
 */
static void gdb_send_stop_reply(int signal, int tid)
{
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "T%02x",
                     signal & 0xff);

    /* Include swbreak reason when we stopped at our software breakpoint.
     * Required by the RSP when swbreak+ is advertised in qSupported. */
    if (gdb.stop_is_swbreak) {
        n += snprintf(buf + n, sizeof(buf) - n, "swbreak:;");
        gdb.stop_is_swbreak = 0;
    }

    n += snprintf(buf + n, sizeof(buf) - n, "thread:p%x.%x;",
                  gdb.target_pid > 0 ? gdb.target_pid : 1,
                  tid > 0 ? tid : 1);
    gdb_send_packet(buf, n);
}

/*
 * Parse a GDB thread-id token.  Handles:
 *   0        -> *pid_out = 0, *tid_out = 0   ("any")
 *  -1        -> *pid_out = -1, *tid_out = -1 ("all")
 *  <tid>     -> *pid_out = 0, *tid_out = tid
 *  p<pid>.<tid> -> both filled in
 * Returns pointer past consumed chars.
 */
static const char *gdb_parse_thread_id(const char *p, int *pid_out, int *tid_out)
{
    *pid_out = 0;
    *tid_out = 0;
    if (*p == '0') {
        *tid_out = 0;
        return p + 1;
    }
    if (*p == '-' && *(p + 1) == '1') {
        *pid_out = -1;
        *tid_out = -1;
        return p + 2;
    }
    if (*p == 'p') {
        p++;
        /* Parse pid (hex) up to '.' */
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        *pid_out = (int)hex2u64(p, (int)(dot - p));
        if (*dot == '.') {
            dot++;
            if (*dot == '-' && *(dot + 1) == '1') {
                *tid_out = -1;
                return dot + 2;
            }
            const char *end = dot;
            while (*end && *end != ';' && *end != '#') end++;
            *tid_out = (int)hex2u64(dot, (int)(end - dot));
            return end;
        }
        return dot;
    }
    /* Plain hex TID */
    {
        const char *end = p;
        while (*end && *end != ';' && *end != '#') end++;
        *tid_out = (int)hex2u64(p, (int)(end - p));
        return end;
    }
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
        /* Canonicalize to TGID */
        gdb.target_pid = thread_tgid(gdb.target);
        gdb.attached = 1;
        gdb.stop_on_exec = 1;

        /* Wake pending process if it was waiting for a debugger */
        if (gdb.pending_pid == pid || gdb.pending_pid == gdb.target_pid) {
            gdb.pending_pid = 0;
            sem_post(&gdb.debugger_attached);
        }

        char msg[64];
        int mlen = snprintf(msg, sizeof(msg), "Attached to pid %d (tgid %d)\n",
                            pid, gdb.target_pid);
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

    /*
     * Resolve the thread.  The user may supply either a TID or a TGID.
     * We always store the TGID as target_pid so that all threads in the
     * process are recognized by the debugger.
     */
    gdb.target_pid = pid;
    if (gdb_resolve_target() != 0) {
        gdb_error(1);
        return;
    }
    /* Canonicalize to TGID */
    gdb.target_pid = thread_tgid(gdb.target);
    gdb.attached = 1;
    gdb.stop_on_exec = 1;
    safestrcpy(gdb.exec_name, gdb.target->name, sizeof(gdb.exec_name));

    printf("gdbstub: attached to pid %d (tgid %d)\n", pid, gdb.target_pid);

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
        gdb.stop_tid = gdb.target ? gdb.target->pid : pid;
        gdb_send_stop_reply(SIGTRAP, gdb.stop_tid);
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
    gdb.stop_tid = gdb.target ? gdb.target->pid : pid;
    gdb_send_stop_reply(SIGTRAP, gdb.stop_tid);
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
                gdb.stop_on_exec = 1;
                gdb.pending_pid = 0;
                safestrcpy(gdb.exec_name, gdb.target->name,
                           sizeof(gdb.exec_name));
                printf("gdbstub: auto-attached to pid %d\n", pid);
                sem_post(&gdb.debugger_attached);
                sem_wait(&gdb.target_stopped);
                gdb.stop_tid = gdb.target ? gdb.target->pid : pid;
                gdb_send_stop_reply(SIGTRAP, gdb.stop_tid);
                break;
            }
        }
        int sig = gdb.stop_signal ? gdb.stop_signal : SIGTRAP;
        int tid = gdb.stop_tid > 0 ? gdb.stop_tid
                : gdb.target_pid > 0 ? gdb.target_pid : 1;
        gdb_send_stop_reply(sig, tid);
        break;
    }

    /* ── g — Read all registers ── */
    case 'g': {
        char *p = out;
        struct thread *sel = gdb_selected_thread();
        if (!gdb.attached || !sel) {
            /* No target yet — return all zeroes so GDB can connect */
            for (int i = 0; i < gdb_arch_num_regs; i++)
                p = hex_le(p, 0, gdb_arch_reg_size(i));
        } else {
            struct utrapframe *tf = sel->trapframe;
            for (int i = 0; i < gdb_arch_num_regs; i++)
                p = hex_le(p, gdb_arch_get_reg(tf, i), gdb_arch_reg_size(i));
        }
        gdb_send_packet(out, (int)(p - out));
        break;
    }

    /* ── G — Write all registers ── */
    case 'G': {
        struct thread *sel = gdb_selected_thread();
        if (!gdb.attached || !sel) {
            gdb_error(1);
            break;
        }
        const char *hex = pkt + 1;
        struct utrapframe *tf = sel->trapframe;
        for (int i = 0; i < gdb_arch_num_regs; i++) {
            int sz = gdb_arch_reg_size(i);
            if (strlen(hex) < (unsigned)(sz * 2)) break;
            gdb_arch_set_reg(tf, i, hexle2val(hex, sz));
            hex += sz * 2;
        }
        gdb_ok();
        break;
    }

    /* ── p N — Read single register ── */
    case 'p': {
        int regnum = (int)hex2u64(pkt + 1, strlen(pkt + 1));
        if (regnum < 0 || regnum >= gdb_arch_num_regs) {
            gdb_error(0);
            break;
        }
        char *p = out;
        int sz = gdb_arch_reg_size(regnum);
        struct thread *sel = gdb_selected_thread();
        if (!gdb.attached || !sel)
            p = hex_le(p, 0, sz);
        else
            p = hex_le(p, gdb_arch_get_reg(sel->trapframe, regnum), sz);
        gdb_send_packet(out, (int)(p - out));
        break;
    }

    /* ── P N=V — Write single register ── */
    case 'P': {
        struct thread *sel = gdb_selected_thread();
        if (!gdb.attached || !sel) {
            gdb_error(1);
            break;
        }
        const char *eq = NULL;
        for (const char *s = pkt + 1; *s; s++) {
            if (*s == '=') { eq = s; break; }
        }
        if (!eq) { gdb_error(0); break; }
        int regnum = (int)hex2u64(pkt + 1, (int)(eq - pkt - 1));
        if (regnum >= 0 && regnum < gdb_arch_num_regs) {
            int sz = gdb_arch_reg_size(regnum);
            uint64 val = hexle2val(eq + 1, sz);
            gdb_arch_set_reg(sel->trapframe, regnum, val);
        }
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

        for (int i = 0; i < gdb.nbps; i++) {
            if (gdb.bps[i].active)
                GDB_LOG("  bp[%d] addr=0x%lx len=%d orig=0x%x",
                          i, gdb.bps[i].addr, gdb.bps[i].len,
                          gdb.bps[i].orig_insn);
        }

        /* Optional resume address */
        if (len > 1 && gdb_resolve_target() == 0) {
            uint64 addr = hex2u64(pkt + 1, len - 1);
            gdb_arch_set_pc(gdb.target->trapframe, addr);
        }

        /*
         * Direct-stop resume: the target was stopped while sleeping
         * in a kernel syscall — it never blocked on target_resume.
         *
         * However, the thread might have naturally woken up since
         * the direct-stop (e.g. UART data arrived) and reached
         * gdbstub_check_interrupt() or hit a breakpoint.  In that
         * case target_stopped was posted and the thread is blocked
         * on target_resume.  Drain that and unblock it.
         *
         * Then force-wake the thread so it can process any buffered
         * input that arrived while GDB had it "stopped."
         */
        if (gdb.direct_stop) {
            /*
             * Clear gdb_stopped on all threads and release
             * direct_stop so any threads spin-waiting in
             * gdbstub_check_interrupt() can proceed.
             */
            if (gdb.target && gdb.target->thread_group) {
                struct thread_group *tg = gdb.target->thread_group;
                pid_rlock();
                struct thread *th, *tmp;
                list_foreach_node_safe(&tg->thread_list, th, tmp, tg_entry) {
                    __atomic_store_n(&th->gdb_stopped, 0, __ATOMIC_RELEASE);
                }
                pid_runlock();
            } else if (gdb.target) {
                __atomic_store_n(&gdb.target->gdb_stopped, 0, __ATOMIC_RELEASE);
            }
            __atomic_store_n(&gdb.direct_stop, 0, __ATOMIC_RELEASE);
            GDB_LOG("-> resuming from direct-stop, pid %d", gdb.target_pid);
            gdb.target_running = 1;
            break;
        }

        /*
         * Step-over-breakpoint: if the current PC sits on one of our
         * software breakpoints, we must transparently single-step past
         * the original instruction before resuming.  This is required
         * when swbreak+ is advertised — GDB does NOT send z0/Z0 around
         * continues.
         */
        if (gdb_resolve_target() == 0) {
            int so = gdb_step_over_bp();
            if (so < 0) {
                /* Target exited during step-over; main loop handles */
                gdb.target_running = 0;
                break;
            }
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
            gdb_arch_set_pc(gdb.target->trapframe, addr);
        }

        /*
         * Direct-stop step: the target is sleeping in a syscall.
         * Set up single-step so the thread stops on its first
         * instruction when the syscall eventually returns.
         */
        if (gdb.direct_stop) {
            if (gdb_arch_has_hw_step()) {
                gdb.hw_stepping = 1;
                gdb_arch_set_hw_step(gdb.target->trapframe, 1);
            } else {
                uint64 pc = gdb_arch_get_pc(gdb.target->trapframe);
                int insn_len = 4, is_branch = 0;
                uint64 next_pc = gdb_arch_decode_next_pc(gdb.target->trapframe, pc,
                                                          &insn_len, &is_branch,
                                                          gdb_read_user_mem);
                uint16 lo16 = 0;
                int bp_len = 4;
                if (gdb_read_user_mem(&lo16, next_pc, 2) == 0)
                    bp_len = gdb_arch_brk_len(next_pc, &lo16, 2);
                gdb_place_step_bp(next_pc, bp_len);
            }
            /* Clear gdb_stopped on all threads and release direct_stop */
            if (gdb.target->thread_group) {
                struct thread_group *tg = gdb.target->thread_group;
                pid_rlock();
                struct thread *th, *tmp;
                list_foreach_node_safe(&tg->thread_list, th, tmp, tg_entry) {
                    __atomic_store_n(&th->gdb_stopped, 0, __ATOMIC_RELEASE);
                }
                pid_runlock();
            } else {
                __atomic_store_n(&gdb.target->gdb_stopped, 0, __ATOMIC_RELEASE);
            }
            __atomic_store_n(&gdb.direct_stop, 0, __ATOMIC_RELEASE);
            GDB_DBG("-> stepping from direct-stop, pid %d",
                      gdb.target_pid);
            gdb.target_running = 1;
            break;
        }

        /*
         * If PC is at an active breakpoint, step over it first.
         * gdb_step_over_bp() transparently executes the original
         * instruction and re-arms the breakpoint.  Since one
         * instruction was executed, this counts as the single step.
         */
        {
            int so = gdb_step_over_bp();
            if (so < 0) {
                /* Target exited during step-over */
                gdb.target_running = 0;
                break;
            }
            if (so == 1) {
                /* Step-over completed — one instruction was executed.
                 * Report the stop directly; target is still waiting
                 * on target_resume. */
                gdb_send_stop_reply(SIGTRAP, gdb.stop_tid);
                break;
            }
        }

        /* Normal single-step: hardware TF or software decode+bp */
        if (gdb_arch_has_hw_step()) {
            gdb.hw_stepping = 1;
            gdb_arch_set_hw_step(gdb.target->trapframe, 1);
            GDB_DBG("-> hw-stepping target pid %d", gdb.target_pid);
        } else {
            uint64 pc = gdb_arch_get_pc(gdb.target->trapframe);
            int insn_len = 4, is_branch = 0;
            uint64 next_pc = gdb_arch_decode_next_pc(gdb.target->trapframe, pc,
                                                      &insn_len, &is_branch,
                                                      gdb_read_user_mem);
            uint16 lo16 = 0;
            int bp_len = 4;
            if (gdb_read_user_mem(&lo16, next_pc, 2) == 0)
                bp_len = gdb_arch_brk_len(next_pc, &lo16, 2);
            gdb_place_step_bp(next_pc, bp_len);
            GDB_DBG("-> sw-stepping target pid %d, next_pc=0x%lx",
                      gdb.target_pid, next_pc);
        }

        /* Resume the target.  Non-blocking, like 'c'. */
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
        /* Use arch-specific breakpoint length if GDB's 'kind' doesn't
         * match what the architecture expects. */
        if (bplen <= 0) bplen = gdb_arch_brk_len(addr, NULL, 0);

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
        if (bplen <= 0) bplen = gdb_arch_brk_len(addr, NULL, 0);

        if (gdb_remove_bp(addr, bplen) == 0)
            gdb_ok();
        else
            gdb_error(0x0e);
        break;
    }

    /* ── H op thread-id — Set thread for subsequent operations ── */
    case 'H': {
        if (len < 2) { gdb_ok(); break; }
        char op = pkt[1];       /* 'g' = register ops, 'c' = continue/step */
        int pid_parsed, tid_parsed;
        gdb_parse_thread_id(pkt + 2, &pid_parsed, &tid_parsed);

        GDB_DBG("H%c pid=%d tid=%d", op, pid_parsed, tid_parsed);

        if (op == 'g') {
            gdb.g_tid = tid_parsed;
        } else if (op == 'c') {
            gdb.c_tid = tid_parsed;
        }
        gdb_ok();
        break;
    }

    /* ── D — Detach ── */
    case 'D':
        GDB_LOG("detach");
        if (gdb.attached && gdb.direct_stop) {
            /* Thread was direct-stopped — clear all threads and release */
            if (gdb.target && gdb.target->thread_group) {
                struct thread_group *tg = gdb.target->thread_group;
                pid_rlock();
                struct thread *th, *tmp;
                list_foreach_node_safe(&tg->thread_list, th, tmp, tg_entry) {
                    __atomic_store_n(&th->gdb_stopped, 0, __ATOMIC_RELEASE);
                }
                pid_runlock();
            } else if (gdb.target) {
                __atomic_store_n(&gdb.target->gdb_stopped, 0, __ATOMIC_RELEASE);
            }
            __atomic_store_n(&gdb.direct_stop, 0, __ATOMIC_RELEASE);
            gdb_detach();
        } else if (gdb.attached && !gdb.target_running) {
            /* Target is stopped (waiting on target_resume) — let it go */
            gdb_detach();
            sem_post(&gdb.target_resume);
        } else {
            gdb_detach();
        }
        gdb.target_running = 0;
        gdb.direct_stop = 0;
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
            gdb_send("PacketSize=1000;swbreak+;vContSupported+"
                     ";multiprocess+;qXfer:threads:read+"
                     ";qXfer:features:read+"
                     ";qXfer:exec-file:read+"
                     ";qXfer:libraries-svr4:read+"
                     ";ThreadEvents+;exec-events+");
        } else if (strcmp(pkt, "qAttached") == 0) {
            gdb_send("1");  /* always "attached" */
        } else if (strcmp(pkt, "qC") == 0) {
            /* Current thread — use the thread that last stopped. */
            int tid = gdb.stop_tid > 0 ? gdb.stop_tid
                    : gdb.target_pid > 0 ? gdb.target_pid : 1;
            int pid = gdb.target_pid > 0 ? gdb.target_pid : 1;
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "QCp%x.%x", pid, tid);
            gdb_send_packet(buf, n);
        } else if (strcmp(pkt, "qfThreadInfo") == 0) {
            /*
             * List all threads in the attached process's thread group.
             * Format: m p<pid>.<tid1>,p<pid>.<tid2>,...
             */
            char buf[GDB_BUF_SIZE];
            int pos = 0;
            buf[pos++] = 'm';
            int tgid = gdb.target_pid > 0 ? gdb.target_pid : 1;

            if (gdb.attached && gdb.target) {
                struct thread_group *tg = gdb.target->thread_group;
                if (tg) {
                    pid_rlock();
                    struct thread *t, *tmp;
                    int first = 1;
                    list_foreach_node_safe(&tg->thread_list, t, tmp, tg_entry) {
                        if (THREAD_ZOMBIE(t))
                            continue;
                        if (!first && pos < (int)sizeof(buf) - 20)
                            buf[pos++] = ',';
                        first = 0;
                        pos += snprintf(buf + pos, sizeof(buf) - pos,
                                        "p%x.%x", tgid, t->pid);
                    }
                    pid_runlock();
                } else {
                    /* No thread group — single thread */
                    pos += snprintf(buf + pos, sizeof(buf) - pos,
                                    "p%x.%x", tgid, gdb.target->pid);
                }
            } else {
                /* Not attached — report a placeholder */
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                                "p%x.%x", tgid, tgid);
            }
            gdb_send_packet(buf, pos);
        } else if (strcmp(pkt, "qsThreadInfo") == 0) {
            gdb_send("l");  /* end of thread list */
        } else if (strncmp(pkt, "qRcmd,", 6) == 0) {
            gdb_handle_monitor(pkt + 6, len - 6);
        } else if (strcmp(pkt, "qTStatus") == 0) {
            gdb_send("T0");  /* no trace running */
        } else if (strncmp(pkt, "qXfer:features:read:", 20) == 0) {
            /*
             * qXfer:features:read:annex:offset,length
             * Return the target description XML so GDB knows our
             * register set (avoids "Truncated register" errors on
             * architectures where the default includes FPU/SSE).
             */
            const char *annex_start = pkt + 20;
            const char *colon = NULL;
            for (const char *s = annex_start; *s; s++) {
                if (*s == ':') { colon = s; break; }
            }
            if (!colon) { gdb_error(0); break; }

            /* Parse offset,length after the annex */
            const char *params = colon + 1;
            const char *comma = NULL;
            for (const char *s = params; *s; s++) {
                if (*s == ',') { comma = s; break; }
            }
            if (!comma) { gdb_error(0); break; }

            uint64 offset = hex2u64(params, (int)(comma - params));
            uint64 length = hex2u64(comma + 1, strlen(comma + 1));

            /* We only support "target.xml" */
            int annex_len = (int)(colon - annex_start);
            if (annex_len != 10 ||
                strncmp(annex_start, "target.xml", 10) != 0) {
                gdb_error(0);
                break;
            }

            int xml_len = 0;
            const char *xml = gdb_arch_target_xml(&xml_len);

            if ((int)offset >= xml_len) {
                gdb_send("l");  /* done, no more data */
                break;
            }

            int remaining = xml_len - (int)offset;
            int chunk = remaining;
            if (chunk > (int)length)
                chunk = (int)length;
            if (chunk > GDB_BUF_SIZE - 2)
                chunk = GDB_BUF_SIZE - 2;

            char *p = out;
            if ((int)offset + chunk >= xml_len)
                *p++ = 'l';  /* last chunk */
            else
                *p++ = 'm';  /* more data follows */
            memcpy(p, xml + offset, chunk);
            p += chunk;
            gdb_send_packet(out, (int)(p - out));
        } else if (strncmp(pkt, "qXfer:threads:read:", 19) == 0) {
            /*
             * qXfer:threads:read::<offset>,<length>
             * Return an XML document listing threads & thread groups.
             */
            char xml[GDB_BUF_SIZE];
            int xpos = 0;
            xpos += snprintf(xml + xpos, sizeof(xml) - xpos,
                             "l<?xml version=\"1.0\"?>\n"
                             "<threads>\n");

            if (gdb.attached && gdb.target) {
                int tgid = gdb.target_pid;
                struct thread_group *tg = gdb.target->thread_group;
                if (tg) {
                    pid_rlock();
                    struct thread *t, *tmp;
                    list_foreach_node_safe(&tg->thread_list, t, tmp, tg_entry) {
                        if (THREAD_ZOMBIE(t))
                            continue;
                        const char *core = "";
                        char corebuf[32] = "";
                        if (t->sched_entity && smp_load_acquire(&t->sched_entity->on_cpu))
                            snprintf(corebuf, sizeof(corebuf),
                                     " core=\"%d\"",
                                     smp_load_acquire(&t->sched_entity->cpu_id));
                        core = corebuf;
                        xpos += snprintf(xml + xpos, sizeof(xml) - xpos,
                                         "  <thread id=\"p%x.%x\" name=\"%s\"%s/>"
                                         "\n",
                                         tgid, t->pid, t->name, core);
                    }
                    pid_runlock();
                } else {
                    xpos += snprintf(xml + xpos, sizeof(xml) - xpos,
                                     "  <thread id=\"p%x.%x\" name=\"%s\"/>"
                                     "\n",
                                     tgid, gdb.target->pid,
                                     gdb.target->name);
                }
            }

            xpos += snprintf(xml + xpos, sizeof(xml) - xpos,
                             "</threads>\n");
            gdb_send_packet(xml, xpos);
        } else if (strncmp(pkt, "qXfer:exec-file:read:", 21) == 0) {
            /*
             * qXfer:exec-file:read:annex:offset,length
             * Return the executable pathname for the target process.
             * annex is the PID in hex (or empty for the current target).
             */
            const char *name = gdb.exec_name;
            if (name[0] == '\0' && gdb.target &&
                gdb.target->thread_group &&
                gdb.target->thread_group->exec_path[0] != '\0')
                name = gdb.target->thread_group->exec_path;
            if (name[0] == '\0' && gdb.target)
                name = gdb.target->name;
            if (name[0] == '\0')
                name = "unknown";
            char buf[256];
            int n = snprintf(buf, sizeof(buf), "l%s", name);
            gdb_send_packet(buf, n);
        } else if (strncmp(pkt, "qXfer:libraries-svr4:read:", 25) == 0) {
            /*
             * qXfer:libraries-svr4:read::offset,length
             * Return XML listing shared libraries and their load addresses.
             * GDB uses l_addr (load bias) to offset symbols from the
             * library's ELF, enabling source-level debugging of dynamically
             * linked programs.
             */
            char xml[GDB_BUF_SIZE];
            int xpos = 0;
            xpos += snprintf(xml + xpos, sizeof(xml) - xpos,
                             "l<?xml version=\"1.0\"?>\n"
                             "<library-list-svr4 version=\"1.0\">");

            if (gdb.attached && gdb.target) {
                struct thread_group *tg = gdb.target->thread_group;
                /* Report the interpreter (dynamic linker / libc) if present */
                if (tg && tg->interp_base != 0 && tg->interp_path[0] != '\0') {
                    xpos += snprintf(xml + xpos, sizeof(xml) - xpos,
                                     "\n  <library name=\"%s\""
                                     " lm=\"0x%lx\""
                                     " l_addr=\"0x%lx\""
                                     " l_ld=\"0x%lx\" lmid=\"0x0\"/>",
                                     tg->interp_path,
                                     tg->interp_base,
                                     tg->interp_base,
                                     tg->interp_ld);
                }

                /* Scan VMAs for additional file-backed executable mappings
                 * that aren't the main executable or interpreter. */
                vm_t *vm = gdb.target->vm;
                if (vm) {
                    vm_rlock(vm);
                    vma_t *vma, *tmp;
                    /* Track files we've already reported (simple dedup) */
                    struct vfs_file *seen[16];
                    int nseen = 0;
                    /* Mark the interpreter file as already seen */
                    list_foreach_node_safe(&vm->vm_list, vma, tmp, list_entry) {
                        if (!(vma->flags & VMA_FLAG_FILE) || !vma->file)
                            continue;
                        if (!(vma->flags & PROT_EXEC))
                            continue;
                        /* Skip if we've already reported this file */
                        int dup = 0;
                        for (int j = 0; j < nseen; j++) {
                            if (seen[j] == vma->file) { dup = 1; break; }
                        }
                        if (dup)
                            continue;
                        if (nseen < 16)
                            seen[nseen++] = vma->file;

                        /* Build path from inode parent chain */
                        struct vfs_inode *ip = vma->file->inode.inode;
                        if (!ip || !ip->name)
                            continue;

                        char pathbuf[256];
                        pathbuf[0] = '\0';
                        /* Walk up the parent chain to build the path */
                        struct vfs_inode *chain[32];
                        int depth = 0;
                        for (struct vfs_inode *p = ip;
                             p && p->name && depth < 32;
                             p = p->parent) {
                            chain[depth++] = p;
                            if (p->parent == p) break; /* root */
                        }
                        int ppos = 0;
                        for (int k = depth - 1; k >= 0; k--) {
                            ppos += snprintf(pathbuf + ppos,
                                             sizeof(pathbuf) - ppos,
                                             "/%s", chain[k]->name);
                        }
                        if (pathbuf[0] == '\0')
                            continue;

                        /* Skip the main executable */
                        const char *leaf = pathbuf;
                        const char *s;
                        for (s = pathbuf; *s; s++)
                            if (*s == '/') leaf = s + 1;
                        if (gdb.exec_name[0] != '\0') {
                            const char *exec_leaf = gdb.exec_name;
                            for (s = gdb.exec_name; *s; s++)
                                if (*s == '/') exec_leaf = s + 1;
                            if (strcmp(leaf, exec_leaf) == 0)
                                continue;
                        }

                        /* Skip if it matches the interpreter path */
                        if (tg && tg->interp_path[0] != '\0') {
                            const char *interp_leaf = tg->interp_path;
                            for (s = tg->interp_path; *s; s++)
                                if (*s == '/') interp_leaf = s + 1;
                            if (strcmp(leaf, interp_leaf) == 0)
                                continue;
                        }

                        xpos += snprintf(xml + xpos, sizeof(xml) - xpos,
                                         "\n  <library name=\"%s\""
                                         " lm=\"0x%lx\""
                                         " l_addr=\"0x%lx\""
                                         " l_ld=\"0\" lmid=\"0x0\"/>",
                                         pathbuf,
                                         vma->start,
                                         vma->start);
                    }
                    vm_runlock(vm);
                }
            }

            xpos += snprintf(xml + xpos, sizeof(xml) - xpos,
                             "\n</library-list-svr4>\n");
            gdb_send_packet(xml, xpos);
        } else if (strncmp(pkt, "qXfer", 5) == 0) {
            gdb_empty(); /* other qXfer not supported */
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
            gdb_send("vCont;c;s;t");
        } else if (strncmp(pkt, "vCont;", 6) == 0) {
            /*
             * Parse vCont actions.  Format:
             *   vCont;action[:thread-id][;action[:thread-id]...]
             *
             * We scan for 's' (step) first; if none, default to 'c'.
             * Thread IDs in p<pid>.<tid> form are parsed but we
             * currently apply the action to the whole process.
             */
            const char *act = pkt + 6;
            int do_step = 0;
            /* Scan for any 's' action */
            const char *scan = act;
            while (*scan) {
                if (*scan == 's') { do_step = 1; break; }
                /* Skip to next ';' */
                while (*scan && *scan != ';') scan++;
                if (*scan == ';') scan++;
            }
            if (do_step) {
                char fake[2] = {'s', '\0'};
                gdb_handle_packet(fake, 1);
            } else {
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

    /* ── Q — Set packets ── */
    case 'Q': {
        if (strncmp(pkt, "QThreadEvents:", 14) == 0) {
            /* QThreadEvents:1 — enable thread exit events
             * QThreadEvents:0 — disable */
            gdb_ok();
        } else if (strncmp(pkt, "QCatchExec:", 11) == 0) {
            /* QCatchExec:1 — enable exec stop events
             * Always succeed — we always report exec events. */
            gdb_ok();
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
 * gdbstub_signal_stop() — called from usertrap() when a fatal signal
 * (SIGSEGV, SIGBUS, SIGFPE, SIGILL, etc.) is about to be delivered to
 * a debugged process.
 *
 * This stops the process BEFORE signal delivery so GDB can inspect the
 * exact state at the point of the fault (registers, backtrace, memory).
 * The trapframe still contains the faulting PC and all registers at the
 * time of the fault.
 *
 * @param t     The faulting thread
 * @param signo The signal number (e.g., SIGSEGV)
 *
 * Returns:
 *   0  — process was stopped and GDB resumed it; caller should proceed
 *         with signal delivery / kill.
 *  -1  — process is not being debugged; caller should handle normally.
 */
int gdbstub_signal_stop(struct thread *t, int signo)
{
    int tgid = thread_tgid(t);
    if (!gdb.attached || tgid != gdb.target_pid)
        return -1;

    GDB_LOG("signal_stop: tid=%d tgid=%d signo=%d pc=0x%lx stval=0x%lx",
              t->pid, tgid, signo,
              gdb_arch_get_pc(t->trapframe),
              t->trapframe->trapframe.stval);

    gdb.target      = t;
    gdb.stop_signal = signo;
    gdb.stop_tid    = t->pid;

    /* Notify the GDB thread that the target has stopped */
    sem_post(&gdb.target_stopped);

    /* Block until GDB says continue or step */
    sem_wait(&gdb.target_resume);

    /* Mark that the debugger already inspected at the fault point.
     * gdbstub_exit_stop() will skip its Phase-1 stop. */
    gdb.signal_stopped = 1;
    __sync_synchronize();

    /* Flush I-cache in case GDB modified code */
    if (t->vm)
        vm_remote_fence_i(t->vm);

    return 0;
}

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
    int tgid = thread_tgid(t);
    GDB_LOG("trap: tid=%d tgid=%d pc=0x%lx attached=%d target_pid=%d",
              t->pid, tgid, gdb_arch_get_pc(t->trapframe), gdb.attached, gdb.target_pid);
    /* Is this a thread in the process we're debugging? */
    if (!gdb.attached || tgid != gdb.target_pid) {
        /*
         * No debugger attached for this PID.  If no other process is
         * already waiting, block here until a GDB client connects and
         * attaches to our PID.  This implements the waitgdb() helper.
         */
        if (gdb.pending_pid != 0) {
            /* Another process is already waiting — can't queue two. */
            return -1;
        }

        gdb.pending_pid = tgid;
        printf("gdbstub: pid %d (%s) waiting for debugger on port %d\n"
               "  (gdb) target remote <host>:%d\n"
               "  (gdb) attach %d\n",
               tgid, t->name, GDBSTUB_PORT,
               GDBSTUB_PORT, tgid);

        /* Block until GDB attaches to us */
        sem_wait(&gdb.debugger_attached);

        /* GDB has attached — fall through to normal stop handling.
         * Check a0: waitgdb() sets a0=0 (don't stop on exec),
         * waitgdb -e sets a0=1 (stop at entry point after exec). */
        gdb.stop_on_exec = (arch_tf_get_arg0(t->trapframe) != 0) ? 1 : 0;
    }

    uint64 pc = gdb_arch_get_pc(t->trapframe);

    /* Hardware single-step completion (#DB on x86).  The PC is already
     * past the stepped instruction — treat it as a step hit. */
    int is_hw_step = 0;
    if (gdb.hw_stepping) {
        is_hw_step = 1;
        gdb.hw_stepping = 0;
    }

    /* Check if this is one of our breakpoints or a step bp. */
    int is_our_bp = 0;
    int is_entry_bp = 0;
    if (is_hw_step) {
        /* Hardware step: always ours, PC is at the next instruction */
        is_our_bp = 1;
    } else if (gdb.stepping && pc == gdb.step_bp_addr) {
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
     * Check for auto-inserted exec entry-point breakpoint.
     * This is a one-shot breakpoint: remove it now so the user can
     * proceed normally.  Don't report swbreak — GDB didn't set it.
     */
    if (gdb.exec_entry_bp_active && pc == gdb.exec_entry_bp_addr) {
        is_entry_bp = 1;
        is_our_bp = 1;  /* it IS in bps[] */
        gdb_remove_bp(pc, 0);
        gdb.exec_entry_bp_active = 0;
        GDB_LOG("trap: auto entry bp at 0x%lx removed (one-shot)", pc);
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
        gdb_read_user_mem(&lo16, pc, 2);
        int advance = gdb_arch_user_brk_len(pc, &lo16, 2);
        GDB_DBG("trap: user breakpoint at 0x%lx, advancing pc +%d", pc, advance);
        gdb_arch_set_pc(t->trapframe, pc + advance);
    } else {
        GDB_DBG("trap: our bp at 0x%lx (step=%d entry=%d)", pc, gdb.stepping, is_entry_bp);
    }

    /* Update state for the GDB thread */
    gdb.target = t;
    gdb.stop_signal = SIGTRAP;
    gdb.stop_tid = t->pid;
    /* Mark swbreak reason when stopped at one of our software breakpoints
     * (not a step bp, not an entry bp, and not a user EBREAK). */
    gdb.stop_is_swbreak = (is_our_bp && !gdb.stepping && !is_entry_bp) ? 1 : 0;

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
    if (t->vm)
        vm_remote_fence_i(t->vm);

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
void gdbstub_exec_stop(struct thread *t, uint64 entry_pc, const char *path)
{
    if (!gdb.attached || thread_tgid(t) != gdb.target_pid)
        return;

    /* Remove any stale breakpoints from the old address space */
    gdb.nbps = 0;
    memset(gdb.bps, 0, sizeof(gdb.bps));
    gdb_clean_step_bp();

    /*
     * Only stop at the entry point if stop_on_exec was requested
     * (e.g. via `waitgdb -e`).  Otherwise just clear stale breakpoints
     * and let the process continue into the new binary.
     */
    if (!gdb.stop_on_exec)
        return;
    gdb.stop_on_exec = 0; /* one-shot */

    gdb.target = t;
    gdb.stop_signal = SIGTRAP;
    gdb.stop_tid = t->pid;

    /* Record exec event so the session loop includes exec:<path> in
     * the stop reply.  GDB uses this to reload symbols and discard
     * stale breakpoints from the old address space. */
    gdb.exec_event = 1;
    snprintf(gdb.exec_name, sizeof(gdb.exec_name), "%s", path);
    __sync_synchronize();

    printf("gdbstub: pid %d exec -> %s, stopped at entry point\n",
           t->pid, path);

    /* Notify the GDB thread that the target has stopped */
    sem_post(&gdb.target_stopped);

    /* Block until GDB says continue or step */
    sem_wait(&gdb.target_resume);

    /*
     * Insert a one-shot breakpoint at the program's actual entry point.
     * GDB auto-continues after processing an exec event (unless the
     * user has 'catch exec' set), so without this the process would
     * run unimpeded.  The breakpoint ensures the process stops at
     * _start, giving the user a chance to set breakpoints.
     *
     * gdbstub_trap() auto-removes this when hit (exec_entry_bp_active).
     */
    if (entry_pc != 0) {
        uint16 lo16 = 0;
        gdb_read_user_mem(&lo16, entry_pc, 2);
        int bp_len = gdb_arch_brk_len(entry_pc, &lo16, 2);
        if (gdb_insert_bp(entry_pc, bp_len) == 0) {
            gdb.exec_entry_bp_active = 1;
            gdb.exec_entry_bp_addr = entry_pc;
            GDB_LOG("exec: auto entry bp at 0x%lx (len=%d)", entry_pc, bp_len);
        } else {
            GDB_LOG("exec: FAILED to insert entry bp at 0x%lx", entry_pc);
        }
    }

    /* Flush I-cache — GDB may have inserted breakpoints from another hart. */
    if (t->vm)
        vm_remote_fence_i(t->vm);
}

/*
 * gdbstub_exit_stop() — called from exit() when a debugged thread/process
 * is about to exit.
 *
 * For the last thread (or single-threaded process) this sends a W (exited)
 * notification so the remote GDB client sees a clean exit.  For individual
 * thread exits in a multi-threaded process we send a thread-exit event.
 *
 * Must be called while the thread's VM is still valid (before resource
 * teardown).
 *
 * @param t       The exiting thread
 * @param status  The exit code
 * @param last    true if this is the last thread in the process (or
 *                single-threaded)
 */
void gdbstub_exit_stop(struct thread *t, int status, int last)
{
    if (!gdb.attached || thread_tgid(t) != gdb.target_pid)
        return;

    GDB_LOG("exit_stop: tid=%d tgid=%d status=%d last=%d",
              t->pid, thread_tgid(t), status, last);

    if (last) {
        /*
         * Whole process is about to exit.
         *
         * If gdbstub_signal_stop() already stopped the process at the
         * fault point (SIGSEGV, etc.), the debugger has had its chance
         * to inspect.  Skip Phase 1 — the trapframe has been modified
         * by signal delivery by now, so stopping again would show the
         * wrong PC (e.g. _start instead of the faulting instruction).
         *
         * If signal_stopped is NOT set (e.g. normal exit()), do the
         * Phase-1 inspection stop so GDB can look around.
         */
        if (!gdb.signal_stopped) {
            /* Phase 1: stop for inspection */
            gdb.target       = t;
            gdb.stop_signal  = SIGTRAP;
            gdb.stop_tid     = t->pid;

            sem_post(&gdb.target_stopped);
            sem_wait(&gdb.target_resume);
        }
        gdb.signal_stopped = 0;

        /*
         * Phase 2: GDB said "continue" — now send the exit event.
         *          Clean up breakpoints while the VM is still live.
         */
        gdb_clean_step_bp();
        gdb_remove_all_bps();

        gdb.target        = t;
        gdb.exit_code     = status;
        gdb.exit_tid      = t->pid;
        gdb.target_exited = 1;
        __sync_synchronize();

        sem_post(&gdb.target_stopped);

        /*
         * Do NOT block again — the session loop will send the W
         * packet and detach.  The process proceeds to tear down.
         */
    } else {
        /*
         * Individual thread exit in a multi-threaded process.
         * Notify GDB so it can update its thread list.
         * This is a non-blocking notification — the thread is going
         * away, we just inform GDB.
         */
        gdb.thread_exit_tid  = t->pid;
        gdb.thread_exit_code = status;
        gdb.thread_exited    = 1;
        __sync_synchronize();

        /* Wake the GDB session loop */
        sem_post(&gdb.target_stopped);

        /* Don't block — this thread is self-reaping. */
    }
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
    if (!gdb.attached || thread_tgid(t) != gdb.target_pid)
        return 0;

    /*
     * Direct-stop: the gdbstub thread already sent the stop reply
     * to GDB while one or more threads were sleeping in syscalls.
     * The per-thread gdb_stopped flag is set on ALL threads of the
     * process.  Each thread that wakes up atomically clears its own
     * flag (1→0).  If it succeeds, it must wait here.
     *
     * Only the "reporting" thread (gdb.stop_tid) interacts with
     * the target_resume semaphore.  Other threads simply spin‐wait
     * until gdb.direct_stop is cleared by the GDB 'c'/'s' handler.
     * This avoids semaphore count mismatches.
     */
    if (__atomic_exchange_n(&t->gdb_stopped, 0, __ATOMIC_ACQ_REL)) {
        GDB_LOG("check_interrupt: direct-stop hold pid %d tid %d",
                  gdb.target_pid, t->pid);
        while (__atomic_load_n(&gdb.direct_stop, __ATOMIC_ACQUIRE))
            sleep_ms(1);
        if (t->vm)
            vm_remote_fence_i(t->vm);
        return 1;
    }

    /*
     * Normal Ctrl-C: interrupt_pending was set and an IPI sent to
     * the target CPU.  Use atomic exchange so that only ONE thread
     * wins the clear — prevents duplicate sem_post(&target_stopped)
     * in multi-threaded processes.
     */
    if (!__atomic_exchange_n(&gdb.interrupt_pending, 0, __ATOMIC_ACQ_REL))
        return 0;

    GDB_LOG("check_interrupt: stopping pid %d tid %d", gdb.target_pid, t->pid);

    gdb.target = t;
    gdb.stop_signal = SIGINT;
    gdb.stop_tid = t->pid;

    /* Notify the GDB thread that the target has stopped */
    sem_post(&gdb.target_stopped);

    /* Block until GDB says continue or step */
    sem_wait(&gdb.target_resume);

    /* Flush I-cache — GDB may have inserted breakpoints from another hart. */
    if (t->vm)
        vm_remote_fence_i(t->vm);

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
    gdb.stop_tid = 0;
    gdb.stop_is_swbreak = 0;
    gdb.g_tid = 0;
    gdb.c_tid = 0;
    gdb.stepping = 0;
    gdb.nbps = 0;
    gdb.interrupt_pending = 0;
    gdb.target_running = 0;
    gdb.direct_stop = 0;
    gdb.target_exited = 0;
    gdb.exit_code = 0;
    gdb.exit_tid = 0;
    gdb.thread_exited = 0;
    gdb.thread_exit_tid = 0;
    gdb.thread_exit_code = 0;
    gdb.process_killed = 0;
    gdb.kill_signal = 0;
    gdb.exec_event = 0;
    gdb.exec_name[0] = '\0';
    gdb.signal_stopped = 0;
    gdb.rxpbuf = NULL;
    gdb.rxpoff = 0;
    memset(gdb.bps, 0, sizeof(gdb.bps));

    /*
     * Drain stale semaphore counts from previous sessions.
     * If the previous session disconnected while a target_stopped was
     * pending (e.g. breakpoint hit during cleanup), or if target_resume
     * leaked an extra post, those stale counts would confuse this session.
     */
    while (sem_trywait(&gdb.target_stopped) == 0)
        ;
    while (sem_trywait(&gdb.target_resume) == 0)
        ;

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

            /* ── Process exit ──────────────────────────────────── */
            if (gdb.target_exited) {
                int code = (gdb.exit_code >> 8) & 0xff;  /* W uses raw byte */
                char buf[16];
                int n = snprintf(buf, sizeof(buf), "W%02x", code);
                GDB_LOG("target exited (pid=%d code=%d)", gdb.target_pid, gdb.exit_code);
                gdb_send_packet(buf, n);
                gdb.target_exited = 0;
                gdb.attached = 0;
                gdb.target = NULL;
                gdb.target_pid = 0;
                continue;
            }

            /* ── Process killed by signal ──────────────────────── */
            if (gdb.process_killed) {
                char buf[16];
                int n = snprintf(buf, sizeof(buf), "X%02x", gdb.kill_signal & 0xff);
                GDB_LOG("target killed by signal %d (pid=%d)", gdb.kill_signal, gdb.target_pid);
                gdb_send_packet(buf, n);
                gdb.process_killed = 0;
                gdb.attached = 0;
                gdb.target = NULL;
                gdb.target_pid = 0;
                continue;
            }

            /* ── Individual thread exit ────────────────────────── */
            if (gdb.thread_exited) {
                char buf[64];
                int teid = gdb.thread_exit_tid;
                int tecode = (gdb.thread_exit_code >> 8) & 0xff;
                int n = snprintf(buf, sizeof(buf), "w%02x;p%x.%x",
                                 tecode, gdb.target_pid, teid);
                GDB_LOG("thread exited (tid=%d code=%d)", teid, gdb.thread_exit_code);
                gdb_send_packet(buf, n);
                gdb.thread_exited = 0;
                /* Stay attached — other threads still running */
                continue;
            }

            /* ── Normal stop (breakpoint / signal) ─────────────── */
            GDB_LOG("target stopped (sig=%d exec=%d)", gdb.stop_signal, gdb.exec_event);
            gdb_clean_step_bp();
            if (gdb.target && gdb.target->sched_entity) {
                while (smp_load_acquire(&gdb.target->sched_entity->on_cpu))
                    ;
            }
            if (gdb_resolve_target() != 0) {
                gdb_send("X09");
                gdb.attached = 0;
            } else if (gdb.exec_event) {
                /*
                 * Exec event: send T05exec:<hex-pathname>;thread:...;
                 * so GDB discards stale breakpoints and reloads symbols
                 * for the new binary.
                 */
                gdb.exec_event = 0;
                char buf[512];
                int n = snprintf(buf, sizeof(buf), "T%02x", SIGTRAP);
                /* Hex-encode the exec pathname */
                n += snprintf(buf + n, sizeof(buf) - n, "exec:");
                const char *name = gdb.exec_name;
                for (int i = 0; name[i] && n + 2 < (int)sizeof(buf); i++) {
                    buf[n++] = hexchars[((uint8)name[i]) >> 4];
                    buf[n++] = hexchars[((uint8)name[i]) & 0xf];
                }
                n += snprintf(buf + n, sizeof(buf) - n, ";thread:p%x.%x;",
                              gdb.target_pid > 0 ? gdb.target_pid : 1,
                              gdb.stop_tid > 0 ? gdb.stop_tid : 1);
                gdb_send_packet(buf, n);
            } else {
                gdb_send_stop_reply(gdb.stop_signal, gdb.stop_tid);
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
                struct thread *t = NULL;
                rcu_read_lock();
                get_pid_thread(gdb.target_pid, &t);
                if (t && t->sched_entity) {
                    if (smp_load_acquire(&t->sched_entity->on_cpu)) {
                        /*
                         * Thread is running on a CPU — set the
                         * interrupt_pending flag and send an IPI to
                         * force a trap.  The thread will stop in
                         * gdbstub_check_interrupt() on its way back
                         * to user space.
                         */
                        gdb.interrupt_pending = 1;
                        __sync_synchronize();
                        int target_cpu = smp_load_acquire(
                            &t->sched_entity->cpu_id);
                        ipi_send_single(target_cpu, IPI_REASON_RESCHEDULE);
                    } else {
                        /*
                         * Thread is off-cpu (sleeping in a syscall).
                         * It may never reach usertrapret() until the
                         * blocking condition is satisfied (e.g. console
                         * data for read()).  Stop it directly: report
                         * the stop to GDB now using the trapframe state
                         * saved at syscall entry.  The target thread
                         * stays asleep — when GDB continues, we just
                         * mark target_running again.
                         */
                        gdb.target = t;
                        gdb.stop_signal = SIGINT;
                        gdb.stop_tid = t->pid;
                        gdb.direct_stop = 1;
                        gdb.target_running = 0;
                        /* Mark ALL threads in the process so they
                         * block in gdbstub_check_interrupt() if they
                         * wake before GDB continues. */
                        __atomic_store_n(&t->gdb_stopped, 1,
                                         __ATOMIC_RELEASE);
                        struct thread_group *tg = t->thread_group;
                        if (tg) {
                            pid_rlock();
                            struct thread *th, *tmp;
                            list_foreach_node_safe(&tg->thread_list, th, tmp, tg_entry) {
                                if (th != t && !THREAD_ZOMBIE(th))
                                    __atomic_store_n(&th->gdb_stopped, 1,
                                                     __ATOMIC_RELEASE);
                            }
                            pid_runlock();
                        }
                    }
                }
                rcu_read_unlock();
                if (gdb.direct_stop) {
                    GDB_LOG("direct stop: pid %d tid %d (off-cpu)",
                              gdb.target_pid, gdb.stop_tid);
                    gdb_send_stop_reply(SIGINT, gdb.stop_tid);
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
        if (gdb.direct_stop) {
            /* Thread was direct-stopped — clear all threads and release */
            if (gdb.target && gdb.target->thread_group) {
                struct thread_group *tg = gdb.target->thread_group;
                pid_rlock();
                struct thread *th, *tmp;
                list_foreach_node_safe(&tg->thread_list, th, tmp, tg_entry) {
                    __atomic_store_n(&th->gdb_stopped, 0, __ATOMIC_RELEASE);
                }
                pid_runlock();
            } else if (gdb.target) {
                __atomic_store_n(&gdb.target->gdb_stopped, 0, __ATOMIC_RELEASE);
            }
            __atomic_store_n(&gdb.direct_stop, 0, __ATOMIC_RELEASE);
            gdb_detach();
        } else {
            gdb_detach();
            /* Resume the target if it was stopped */
            sem_post(&gdb.target_resume);
        }
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
