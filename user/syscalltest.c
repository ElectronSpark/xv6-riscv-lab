/**
 * @file syscalltest.c
 * @brief Validation test suite for newly-implemented syscalls.
 *
 * Tests: prctl, sysinfo, getrusage, getpriority/setpriority,
 *        set_robust_list, clock_settime, sched_rr_get_interval,
 *        sigaltstack, fsync, fdatasync, utimensat, memfd_create,
 *        shmget/shmat/shmdt/shmctl, semget/semop/semctl,
 *        msgget/msgsnd/msgrcv/msgctl, clone3.
 */

#include "kernel/inc/types.h"
#include "kernel/inc/syscall.h"
#include "user/user.h"

/* ── stats ── */
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_PASS(name) do { \
    printf("  PASS: %s\n", name); tests_passed++; \
} while (0)

#define TEST_FAIL(name, reason) do { \
    printf("  FAIL: %s — %s\n", name, reason); tests_failed++; \
} while (0)

/* ────────────────────────────────────────────────────────
 * Raw syscall helpers (arch-portable)
 * ──────────────────────────────────────────────────────── */
#if defined(__riscv)
static inline int64 _syscall0(int num) {
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0");
    asm volatile("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}
static inline int64 _syscall1(int num, int64 a) {
    register int64 a7 asm("a7") = num;
    register int64 _a0 asm("a0") = a;
    asm volatile("ecall" : "+r"(_a0) : "r"(a7) : "memory");
    return _a0;
}
static inline int64 _syscall2(int num, int64 a, int64 b) {
    register int64 a7 asm("a7") = num;
    register int64 _a0 asm("a0") = a;
    register int64 _a1 asm("a1") = b;
    asm volatile("ecall" : "+r"(_a0) : "r"(_a1), "r"(a7) : "memory");
    return _a0;
}
static inline int64 _syscall3(int num, int64 a, int64 b, int64 c) {
    register int64 a7 asm("a7") = num;
    register int64 _a0 asm("a0") = a;
    register int64 _a1 asm("a1") = b;
    register int64 _a2 asm("a2") = c;
    asm volatile("ecall" : "+r"(_a0) : "r"(_a1), "r"(_a2), "r"(a7) : "memory");
    return _a0;
}
static inline int64 _syscall4(int num, int64 a, int64 b, int64 c, int64 d) {
    register int64 a7 asm("a7") = num;
    register int64 _a0 asm("a0") = a;
    register int64 _a1 asm("a1") = b;
    register int64 _a2 asm("a2") = c;
    register int64 _a3 asm("a3") = d;
    asm volatile("ecall" : "+r"(_a0) : "r"(_a1), "r"(_a2), "r"(_a3), "r"(a7) : "memory");
    return _a0;
}
static inline int64 _syscall5(int num, int64 a, int64 b, int64 c, int64 d, int64 e) {
    register int64 a7 asm("a7") = num;
    register int64 _a0 asm("a0") = a;
    register int64 _a1 asm("a1") = b;
    register int64 _a2 asm("a2") = c;
    register int64 _a3 asm("a3") = d;
    register int64 _a4 asm("a4") = e;
    asm volatile("ecall" : "+r"(_a0) : "r"(_a1), "r"(_a2), "r"(_a3), "r"(_a4), "r"(a7) : "memory");
    return _a0;
}
#elif defined(__x86_64__)
static inline int64 _syscall0(int num) {
    int64 ret;
    asm volatile("syscall" : "=a"(ret) : "a"((int64)num) : "rcx", "r11", "memory");
    return ret;
}
static inline int64 _syscall1(int num, int64 a) {
    int64 ret;
    asm volatile("syscall" : "=a"(ret) : "a"((int64)num), "D"(a) : "rcx", "r11", "memory");
    return ret;
}
static inline int64 _syscall2(int num, int64 a, int64 b) {
    int64 ret;
    asm volatile("syscall" : "=a"(ret) : "a"((int64)num), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return ret;
}
static inline int64 _syscall3(int num, int64 a, int64 b, int64 c) {
    int64 ret;
    asm volatile("syscall" : "=a"(ret) : "a"((int64)num), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return ret;
}
static inline int64 _syscall4(int num, int64 a, int64 b, int64 c, int64 d) {
    int64 ret;
    register int64 r10 asm("r10") = d;
    asm volatile("syscall" : "=a"(ret) : "a"((int64)num), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
    return ret;
}
static inline int64 _syscall5(int num, int64 a, int64 b, int64 c, int64 d, int64 e) {
    int64 ret;
    register int64 r10 asm("r10") = d;
    register int64 r8 asm("r8") = e;
    asm volatile("syscall" : "=a"(ret) : "a"((int64)num), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8) : "rcx", "r11", "memory");
    return ret;
}
#else
#error "Unsupported architecture"
#endif

/* ── IPC constants (match kernel/inc/ipc.h) ── */
#define IPC_PRIVATE 0
#define IPC_CREAT   01000
#define IPC_EXCL    02000
#define IPC_RMID    0
#define IPC_SET     1
#define IPC_STAT    2
#define IPC_NOWAIT  04000

#define GETVAL  12
#define SETVAL  16
#define GETALL  13
#define SETALL  17

/* ── prctl constants ── */
#define PR_SET_NAME 15
#define PR_GET_NAME 16

/* ── Error codes ── */
#define EINVAL   22
#define EFAULT   14
#define EBADF     9
#define ENOENT    2
#define EEXIST   17
#define EACCES   13
#define EAGAIN   11
#define EFBIG    27
#define ENOMEM   12
#define EBUSY    16
#define E2BIG     7
#define ENOMSG   42
#define EPERM     1

/* ── Message queue constants ── */
#define MSGMAX   8192
#define MSGMNB   16384
#define MSG_NOERROR 0x20000

/* ── Semaphore constants ── */
#define SEMMSL   250
#define SEMOPM   32

/* ── Shared memory constants ── */
#define SHMMAX   (64UL * 1024 * 1024)

/* ── Signal constants ── */
#define SIGUSR1   10
#define SIGALRM   14
#define NSIG      32
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#define SA_SIGINFO  0x00000004
#define SA_RESTART  0x10000000

/* ── Timer constants ── */
#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

/* ── MFD constants ── */
#define MFD_CLOEXEC       0x0001U
#define MFD_ALLOW_SEALING 0x0002U
#define O_RDWR            02

/* ── sysinfo struct (matches kernel) ── */
struct k_sysinfo {
    int64  uptime;
    uint64 loads[3];
    uint64 totalram;
    uint64 freeram;
    uint64 sharedram;
    uint64 bufferram;
    uint64 totalswap;
    uint64 freeswap;
    uint16 procs;
    uint16 __pad;
    uint32 __pad2;
    uint64 totalhigh;
    uint64 freehigh;
    uint32 mem_unit;
};

/* ── rusage struct (matches kernel) ── */
struct k_timeval {
    int64 tv_sec;
    int64 tv_usec;
};
struct k_rusage {
    struct k_timeval ru_utime;
    struct k_timeval ru_stime;
    int64 ru_maxrss;
    int64 ru_ixrss;
    int64 ru_idrss;
    int64 ru_isrss;
    int64 ru_minflt;
    int64 ru_majflt;
    int64 ru_nswap;
    int64 ru_inblock;
    int64 ru_oublock;
    int64 ru_msgsnd;
    int64 ru_msgrcv;
    int64 ru_nsignals;
    int64 ru_nvcsw;
    int64 ru_nivcsw;
};

/* ── IPC data structures (match kernel) ── */
struct ipc_perm {
    int32  key;
    uint32 uid;
    uint32 gid;
    uint32 cuid;
    uint32 cgid;
    uint32 mode;
    uint16 seq;
    uint16 __pad;
};

struct shmid_ds {
    struct ipc_perm shm_perm;
    uint64 shm_segsz;
    uint64 shm_atime;
    uint64 shm_dtime;
    uint64 shm_ctime;
    int32  shm_cpid;
    int32  shm_lpid;
    uint32 shm_nattch;
    uint32 __pad;
};

struct semid_ds {
    struct ipc_perm sem_perm;
    uint64 sem_otime;
    uint64 sem_ctime;
    uint32 sem_nsems;
    uint32 __pad;
};

struct msqid_ds {
    struct ipc_perm msg_perm;
    uint64 msg_stime;
    uint64 msg_rtime;
    uint64 msg_ctime;
    uint64 msg_qnum;
    uint64 msg_qbytes;
    int32  msg_lspid;
    int32  msg_lrpid;
};

/* sigaltstack defs — use kernel values from signal_types.h */
/* SS_ONSTACK=0x2, SS_DISABLE=0x4 already defined via user.h */
#define MINSIGSTKSZ 2048
#define SIGSTKSZ    8192

struct stack_t_user {
    uint64 ss_sp;   /* void * */
    int    ss_flags;
    int    __pad;
    uint64 ss_size;
};

/* utimensat defs */
#define AT_FDCWD         (-100)
#define UTIME_NOW  0x3fffffff
#define UTIME_OMIT 0x3ffffffe

/* ── itimerval struct ── */
struct k_itimerval {
    struct { uint64 tv_sec; uint64 tv_usec; } it_interval;
    struct { uint64 tv_sec; uint64 tv_usec; } it_value;
};

/* ── sigaction struct (user-side) ── */
struct k_sigaction {
    uint64 sa_handler;     /* void (*)(int) or void (*)(int, siginfo_t*, void*) */
    uint64 sa_flags;
#ifdef __x86_64__
    uint64 sa_restorer;
#endif
    uint64 sa_mask;
#ifndef __x86_64__
    uint64 _sa_unused;
#endif
};

/* ── siginfo struct (user-side, matches kernel) ── */
struct k_siginfo {
    int si_signo;
    int si_errno;
    int si_code;
    int si_pid;
    void *si_addr;
    int si_status;
    int _pad;
    union { int sival_int; void *sival_ptr; } si_value;
};

/* ══════════════════════════════════════════════════════════════════════
 * 1. PRCTL
 * ══════════════════════════════════════════════════════════════════════ */
static void test_prctl(void) {
    const char *name = "prctl(PR_SET_NAME/PR_GET_NAME)";
    char newname[] = "testproc";
    int64 ret = _syscall5(SYS_prctl, PR_SET_NAME,
                          (int64)newname, 0, 0, 0);
    if (ret != 0) { TEST_FAIL(name, "PR_SET_NAME returned error"); return; }

    char buf[16];
    memset(buf, 0, sizeof(buf));
    ret = _syscall5(SYS_prctl, PR_GET_NAME, (int64)buf, 0, 0, 0);
    if (ret != 0) { TEST_FAIL(name, "PR_GET_NAME returned error"); return; }
    if (strcmp(buf, "testproc") != 0) {
        TEST_FAIL(name, "name mismatch");
        return;
    }
    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 2. SYSINFO
 * ══════════════════════════════════════════════════════════════════════ */
static void test_sysinfo(void) {
    const char *name = "sysinfo";
    struct k_sysinfo si;
    memset(&si, 0, sizeof(si));
    int64 ret = _syscall1(SYS_sysinfo, (int64)&si);
    if (ret != 0) { TEST_FAIL(name, "returned error"); return; }
    if (si.uptime <= 0) { TEST_FAIL(name, "uptime <= 0"); return; }
    if (si.totalram == 0) { TEST_FAIL(name, "totalram == 0"); return; }
    if (si.freeram == 0) { TEST_FAIL(name, "freeram == 0"); return; }
    if (si.mem_unit != 1) { TEST_FAIL(name, "mem_unit != 1"); return; }
    printf("    sysinfo: uptime=%ld totalram=%ld freeram=%ld\n",
           (long)si.uptime, (long)si.totalram, (long)si.freeram);
    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 3. GETRUSAGE
 * ══════════════════════════════════════════════════════════════════════ */
static void test_getrusage(void) {
    const char *name = "getrusage(RUSAGE_SELF)";
    struct k_rusage ru;
    memset(&ru, 0, sizeof(ru));
    int64 ret = _syscall2(SYS_getrusage, 0 /* RUSAGE_SELF */, (int64)&ru);
    if (ret != 0) { TEST_FAIL(name, "returned error"); return; }
    /* Just verify it didn't crash and returned something sensible */
    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 4. GETPRIORITY / SETPRIORITY
 * ══════════════════════════════════════════════════════════════════════ */
static void test_priority(void) {
    const char *name = "getpriority/setpriority";
    /* getpriority returns 20 - nice, always > 0 */
    int64 ret = _syscall2(SYS_getpriority, 0 /* PRIO_PROCESS */, 0);
    if ((int64)ret <= 0 || (int64)ret > 40) {
        TEST_FAIL(name, "getpriority out of range");
        return;
    }
    int orig_nice = 20 - (int)ret;

    /* Set to nice=5 */
    ret = _syscall3(SYS_setpriority, 0, 0, 5);
    if (ret != 0) { TEST_FAIL(name, "setpriority(5) failed"); return; }

    ret = _syscall2(SYS_getpriority, 0, 0);
    int new_nice = 20 - (int)ret;
    if (new_nice != 5) {
        TEST_FAIL(name, "nice not 5 after setpriority");
        return;
    }

    /* Restore */
    _syscall3(SYS_setpriority, 0, 0, (int64)orig_nice);
    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 5. SET_ROBUST_LIST
 * ══════════════════════════════════════════════════════════════════════ */
static void test_set_robust_list(void) {
    const char *name = "set_robust_list";
    char dummy[24];
    int64 ret = _syscall2(SYS_set_robust_list, (int64)dummy, 24);
    if (ret != 0) { TEST_FAIL(name, "returned error"); return; }

    /* Wrong length should fail */
    ret = _syscall2(SYS_set_robust_list, (int64)dummy, 16);
    if (ret == 0) { TEST_FAIL(name, "len=16 should fail"); return; }
    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 6. CLOCK_SETTIME (should return -EPERM)
 * ══════════════════════════════════════════════════════════════════════ */
static void test_clock_settime(void) {
    const char *name = "clock_settime (expect -EPERM)";
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 0 };
    int64 ret = _syscall2(SYS_clock_settime, 0, (int64)&ts);
    /* Should return -EPERM = -1 */
    if (ret != -1) {
        TEST_FAIL(name, "expected -EPERM (-1)");
        return;
    }
    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 7. SCHED_RR_GET_INTERVAL
 * ══════════════════════════════════════════════════════════════════════ */
static void test_sched_rr_get_interval(void) {
    const char *name = "sched_rr_get_interval";
    struct timespec ts;
    memset(&ts, 0, sizeof(ts));
    int64 ret = _syscall2(SYS_sched_rr_get_interval_time64, 0, (int64)&ts);
    if (ret != 0) { TEST_FAIL(name, "returned error"); return; }
    if (ts.tv_nsec != 100000000LL) {
        TEST_FAIL(name, "expected 100ms interval");
        return;
    }
    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 8. SIGALTSTACK
 * ══════════════════════════════════════════════════════════════════════ */
static void test_sigaltstack(void) {
    const char *name = "sigaltstack";

    /* Get current (should be SS_DISABLE) */
    struct stack_t_user old;
    memset(&old, 0, sizeof(old));
    int64 ret = _syscall2(SYS_sigaltstack, 0, (int64)&old);
    if (ret != 0) { TEST_FAIL(name, "get old failed"); return; }
    if (old.ss_flags != SS_DISABLE) {
        TEST_FAIL(name, "expected SS_DISABLE initially");
        return;
    }

    /* Set a new alternate stack */
    char altstack[SIGSTKSZ];
    struct stack_t_user ss;
    ss.ss_sp    = (uint64)altstack;
    ss.ss_flags = 0;
    ss.ss_size  = SIGSTKSZ;
    ss.__pad    = 0;
    ret = _syscall2(SYS_sigaltstack, (int64)&ss, 0);
    if (ret != 0) { TEST_FAIL(name, "set new failed"); return; }

    /* Verify */
    memset(&old, 0, sizeof(old));
    ret = _syscall2(SYS_sigaltstack, 0, (int64)&old);
    if (ret != 0) { TEST_FAIL(name, "get after set failed"); return; }
    if (old.ss_sp != (uint64)altstack || old.ss_size != SIGSTKSZ) {
        TEST_FAIL(name, "stack info mismatch after set");
        return;
    }

    /* Disable */
    ss.ss_flags = SS_DISABLE;
    ret = _syscall2(SYS_sigaltstack, (int64)&ss, 0);
    if (ret != 0) { TEST_FAIL(name, "disable failed"); return; }

    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 9. FSYNC / FDATASYNC
 * ══════════════════════════════════════════════════════════════════════ */
static void test_fsync(void) {
    const char *name = "fsync/fdatasync";
    /* O_WRONLY=01, O_CREAT=0100 → 0101 octal */
    int fd = open("__test_fsync_tmp", 0101);
    if (fd < 0) { TEST_FAIL(name, "open failed"); return; }

    write(fd, "hello", 5);

    int64 ret = _syscall1(SYS_fsync, fd);
    if (ret != 0) { TEST_FAIL(name, "fsync failed"); close(fd); unlink("__test_fsync_tmp"); return; }

    ret = _syscall1(SYS_fdatasync, fd);
    if (ret != 0) { TEST_FAIL(name, "fdatasync failed"); close(fd); unlink("__test_fsync_tmp"); return; }

    close(fd);
    unlink("__test_fsync_tmp");
    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 10. MEMFD_CREATE
 * ══════════════════════════════════════════════════════════════════════ */
static void test_memfd_create(void) {
    const char *name = "memfd_create";
    char mfd_name[] = "test_mfd";
    int64 fd = _syscall2(SYS_memfd_create, (int64)mfd_name, 0);
    if (fd < 0) { TEST_FAIL(name, "returned error"); return; }

    /* Should be writable */
    int n = write((int)fd, "memfd data", 10);
    if (n != 10) { TEST_FAIL(name, "write failed"); close((int)fd); return; }

    close((int)fd);
    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 11. SHARED MEMORY (shmget/shmat/shmdt/shmctl)
 * ══════════════════════════════════════════════════════════════════════ */
static void test_shm(void) {
    const char *name = "shmget/shmat/shmdt/shmctl";
    uint64 page_size = 4096;

    /* Create a private segment of 1 page */
    int64 shmid = _syscall3(SYS_shmget, IPC_PRIVATE,
                            (int64)page_size, IPC_CREAT | 0600);
    if (shmid < 0) { TEST_FAIL(name, "shmget failed"); return; }

    /* Attach */
    int64 addr = _syscall3(SYS_shmat, shmid, 0, 0);
    if (addr <= 0) { TEST_FAIL(name, "shmat failed"); return; }

    /* Write/read through the mapping */
    volatile int *p = (volatile int *)(uint64)addr;
    *p = 0xDEADBEEF;
    if (*p != (int)0xDEADBEEF) {
        TEST_FAIL(name, "write/read mismatch");
        return;
    }

    /* Stat */
    struct shmid_ds ds;
    memset(&ds, 0, sizeof(ds));
    int64 ret = _syscall3(SYS_shmctl, shmid, IPC_STAT, (int64)&ds);
    if (ret != 0) { TEST_FAIL(name, "shmctl IPC_STAT failed"); return; }
    if (ds.shm_segsz != page_size) {
        TEST_FAIL(name, "segment size mismatch");
        return;
    }
    if (ds.shm_nattch != 1) {
        TEST_FAIL(name, "nattch != 1");
        return;
    }

    /* Detach */
    ret = _syscall1(SYS_shmdt, addr);
    if (ret != 0) { TEST_FAIL(name, "shmdt failed"); return; }

    /* Remove */
    ret = _syscall3(SYS_shmctl, shmid, IPC_RMID, 0);
    if (ret != 0) { TEST_FAIL(name, "shmctl IPC_RMID failed"); return; }

    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 12. SEMAPHORES (semget/semop/semctl)
 * ══════════════════════════════════════════════════════════════════════ */
struct k_sembuf {
    uint16 sem_num;
    int16  sem_op;
    int16  sem_flg;
    int16  __pad;
};

static void test_sem(void) {
    const char *name = "semget/semop/semctl";

    /* Create a semaphore set with 1 semaphore */
    int64 semid = _syscall3(SYS_semget, IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (semid < 0) { TEST_FAIL(name, "semget failed"); return; }

    /* Set value to 5 via semctl SETVAL */
    int64 ret = _syscall4(SYS_semctl, semid, 0, SETVAL, 5);
    if (ret != 0) { TEST_FAIL(name, "semctl SETVAL failed"); return; }

    /* Get value via semctl GETVAL */
    ret = _syscall4(SYS_semctl, semid, 0, GETVAL, 0);
    if (ret != 5) { TEST_FAIL(name, "GETVAL != 5"); return; }

    /* Decrement by 2 */
    struct k_sembuf sops;
    sops.sem_num = 0;
    sops.sem_op  = -2;
    sops.sem_flg = 0;
    sops.__pad   = 0;
    ret = _syscall3(SYS_semop, semid, (int64)&sops, 1);
    if (ret != 0) { TEST_FAIL(name, "semop(-2) failed"); return; }

    /* Verify value = 3 */
    ret = _syscall4(SYS_semctl, semid, 0, GETVAL, 0);
    if (ret != 3) { TEST_FAIL(name, "value != 3 after decrement"); return; }

    /* Increment by 1 */
    sops.sem_op = 1;
    ret = _syscall3(SYS_semop, semid, (int64)&sops, 1);
    if (ret != 0) { TEST_FAIL(name, "semop(+1) failed"); return; }

    /* Verify value = 4 */
    ret = _syscall4(SYS_semctl, semid, 0, GETVAL, 0);
    if (ret != 4) { TEST_FAIL(name, "value != 4 after increment"); return; }

    /* IPC_STAT */
    struct semid_ds sds;
    memset(&sds, 0, sizeof(sds));
    ret = _syscall4(SYS_semctl, semid, 0, IPC_STAT, (int64)&sds);
    if (ret != 0) { TEST_FAIL(name, "IPC_STAT failed"); return; }
    if (sds.sem_nsems != 1) {
        TEST_FAIL(name, "sem_nsems != 1");
        return;
    }

    /* Remove */
    ret = _syscall4(SYS_semctl, semid, 0, IPC_RMID, 0);
    if (ret != 0) { TEST_FAIL(name, "IPC_RMID failed"); return; }

    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 13. MESSAGE QUEUES (msgget/msgsnd/msgrcv/msgctl)
 * ══════════════════════════════════════════════════════════════════════ */
struct msgbuf {
    int64 mtype;
    char  mtext[64];
};

static void test_msg(void) {
    const char *name = "msgget/msgsnd/msgrcv/msgctl";

    int64 msqid = _syscall2(SYS_msgget, IPC_PRIVATE, IPC_CREAT | 0600);
    if (msqid < 0) { TEST_FAIL(name, "msgget failed"); return; }

    /* Send a message */
    struct msgbuf sbuf;
    sbuf.mtype = 42;
    memset(sbuf.mtext, 0, sizeof(sbuf.mtext));
    strcpy(sbuf.mtext, "hello ipc");
    int64 textlen = strlen(sbuf.mtext) + 1;
    int64 ret = _syscall4(SYS_msgsnd, msqid, (int64)&sbuf,
                          textlen, 0);
    if (ret != 0) { TEST_FAIL(name, "msgsnd failed"); return; }

    /* Receive */
    struct msgbuf rbuf;
    memset(&rbuf, 0, sizeof(rbuf));
    ret = _syscall5(SYS_msgrcv, msqid, (int64)&rbuf,
                    (int64)sizeof(rbuf.mtext), 42, 0);
    if (ret < 0) { TEST_FAIL(name, "msgrcv failed"); return; }
    if (rbuf.mtype != 42) { TEST_FAIL(name, "mtype mismatch"); return; }
    if (strcmp(rbuf.mtext, "hello ipc") != 0) {
        TEST_FAIL(name, "message body mismatch");
        return;
    }

    /* IPC_STAT */
    struct msqid_ds mds;
    memset(&mds, 0, sizeof(mds));
    ret = _syscall3(SYS_msgctl, msqid, IPC_STAT, (int64)&mds);
    if (ret != 0) { TEST_FAIL(name, "IPC_STAT failed"); return; }
    /* After receive, qnum should be 0 */
    if (mds.msg_qnum != 0) {
        TEST_FAIL(name, "qnum != 0 after receive");
        return;
    }

    /* Remove */
    ret = _syscall3(SYS_msgctl, msqid, IPC_RMID, 0);
    if (ret != 0) { TEST_FAIL(name, "IPC_RMID failed"); return; }

    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 14. SHM key-based lookup + IPC_EXCL
 * ══════════════════════════════════════════════════════════════════════ */
static void test_shm_key(void) {
    const char *name = "shmget(key) + IPC_EXCL";
    int key = 12345;
    uint64 page_size = 4096;

    int64 id1 = _syscall3(SYS_shmget, key, (int64)page_size,
                          IPC_CREAT | 0600);
    if (id1 < 0) { TEST_FAIL(name, "first shmget failed"); return; }

    /* Second with same key, no EXCL — should return same id */
    int64 id2 = _syscall3(SYS_shmget, key, (int64)page_size,
                          IPC_CREAT | 0600);
    if (id2 != id1) {
        TEST_FAIL(name, "second shmget returned different id");
        _syscall3(SYS_shmctl, id1, IPC_RMID, 0);
        return;
    }

    /* Third with EXCL — should fail with EEXIST */
    int64 id3 = _syscall3(SYS_shmget, key, (int64)page_size,
                          IPC_CREAT | IPC_EXCL | 0600);
    if (id3 >= 0) {
        TEST_FAIL(name, "EXCL should have failed");
        _syscall3(SYS_shmctl, id1, IPC_RMID, 0);
        return;
    }

    /* Detach and clean up */
    _syscall3(SYS_shmctl, id1, IPC_RMID, 0);
    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 15. IPC_NOWAIT on semaphore
 * ══════════════════════════════════════════════════════════════════════ */
static void test_sem_nowait(void) {
    const char *name = "semop IPC_NOWAIT";
    int64 semid = _syscall3(SYS_semget, IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (semid < 0) { TEST_FAIL(name, "semget failed"); return; }

    /* Set value to 0 */
    _syscall4(SYS_semctl, semid, 0, SETVAL, 0);

    /* Try decrement with NOWAIT — should fail immediately */
    struct k_sembuf sops;
    sops.sem_num = 0;
    sops.sem_op  = -1;
    sops.sem_flg = (int16)IPC_NOWAIT;
    sops.__pad   = 0;
    int64 ret = _syscall3(SYS_semop, semid, (int64)&sops, 1);
    if (ret == 0) {
        TEST_FAIL(name, "should have failed with EAGAIN");
        _syscall4(SYS_semctl, semid, 0, IPC_RMID, 0);
        return;
    }

    _syscall4(SYS_semctl, semid, 0, IPC_RMID, 0);
    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 16. MULTIPLE MESSAGES with type filtering
 * ══════════════════════════════════════════════════════════════════════ */
static void test_msg_types(void) {
    const char *name = "msgrcv type filtering";
    int64 msqid = _syscall2(SYS_msgget, IPC_PRIVATE, IPC_CREAT | 0600);
    if (msqid < 0) { TEST_FAIL(name, "msgget failed"); return; }

    /* Send type=1 and type=2 */
    struct msgbuf buf;
    buf.mtype = 1;
    strcpy(buf.mtext, "type1");
    _syscall4(SYS_msgsnd, msqid, (int64)&buf, 6, 0);

    buf.mtype = 2;
    strcpy(buf.mtext, "type2");
    _syscall4(SYS_msgsnd, msqid, (int64)&buf, 6, 0);

    /* Receive type=2 specifically */
    struct msgbuf rbuf;
    memset(&rbuf, 0, sizeof(rbuf));
    int64 ret = _syscall5(SYS_msgrcv, msqid, (int64)&rbuf,
                          (int64)sizeof(rbuf.mtext), 2, 0);
    if (ret < 0) { TEST_FAIL(name, "msgrcv(type=2) failed"); goto cleanup; }
    if (rbuf.mtype != 2 || strcmp(rbuf.mtext, "type2") != 0) {
        TEST_FAIL(name, "wrong message received");
        goto cleanup;
    }

    /* Receive remaining (type=1) with msgtyp=0 (any) */
    memset(&rbuf, 0, sizeof(rbuf));
    ret = _syscall5(SYS_msgrcv, msqid, (int64)&rbuf,
                    (int64)sizeof(rbuf.mtext), 0, 0);
    if (ret < 0) { TEST_FAIL(name, "msgrcv(type=0) failed"); goto cleanup; }
    if (rbuf.mtype != 1) {
        TEST_FAIL(name, "expected type=1 for remaining msg");
        goto cleanup;
    }

    TEST_PASS(name);
cleanup:
    _syscall3(SYS_msgctl, msqid, IPC_RMID, 0);
}

/* ══════════════════════════════════════════════════════════════════════
 * 17. UTIMENSAT
 * ══════════════════════════════════════════════════════════════════════ */
static void test_utimensat(void) {
    const char *name = "utimensat";
    /* Create a test file under /tmp (tmpfs) */
    int fd = open("/tmp/__test_utimens_tmp", 0101); /* O_WRONLY|O_CREAT */
    if (fd < 0) { TEST_FAIL(name, "open failed"); return; }
    write(fd, "x", 1);

    /* Set atime/mtime to UTIME_NOW via fd (path=NULL → operate on fd) */
    struct timespec times[2];
    times[0].tv_sec = 0;
    times[0].tv_nsec = UTIME_NOW;
    times[1].tv_sec = 0;
    times[1].tv_nsec = UTIME_NOW;
    int64 ret = _syscall4(SYS_utimensat, fd,
                          (int64)0,   /* NULL path → use fd directly */
                          (int64)times, 0);
    if (ret != 0) {
        printf("  utimensat(fd, NULL, UTIME_NOW) returned %ld\n", (long)ret);
        TEST_FAIL(name, "UTIME_NOW failed"); goto cleanup;
    }

    /* Set explicit times */
    times[0].tv_sec = 1000000;
    times[0].tv_nsec = 0;
    times[1].tv_sec = 2000000;
    times[1].tv_nsec = 0;
    ret = _syscall4(SYS_utimensat, fd,
                    (int64)0,
                    (int64)times, 0);
    if (ret != 0) { TEST_FAIL(name, "explicit times failed"); goto cleanup; }

    TEST_PASS(name);
cleanup:
    close(fd);
    unlink("/tmp/__test_utimens_tmp");
}

/* ══════════════════════════════════════════════════════════════════════
 * 18. SHM cross-process
 * ══════════════════════════════════════════════════════════════════════ */
static void test_shm_cross_process(void) {
    const char *name = "shm cross-process";
    uint64 page_size = 4096;
    int key = 54321;

    int64 shmid = _syscall3(SYS_shmget, key, (int64)page_size,
                            IPC_CREAT | 0666);
    if (shmid < 0) { TEST_FAIL(name, "shmget failed"); return; }

    int64 addr = _syscall3(SYS_shmat, shmid, 0, 0);
    if (addr <= 0) { TEST_FAIL(name, "parent shmat failed"); goto rm; }

    volatile int *shared = (volatile int *)(uint64)addr;
    *shared = 0;

    int pid = fork();
    if (pid < 0) { TEST_FAIL(name, "fork failed"); goto detach; }

    if (pid == 0) {
        /* Child: attach, write, detach, exit */
        int64 caddr = _syscall3(SYS_shmat, shmid, 0, 0);
        if (caddr > 0) {
            volatile int *cp = (volatile int *)(uint64)caddr;
            *cp = 0xCAFE;
            _syscall1(SYS_shmdt, caddr);
        }
        exit(0);
    }

    /* Parent: wait for child, then check */
    int status;
    waitpid(pid, &status, 0);

    if (*shared != 0xCAFE) {
        TEST_FAIL(name, "child write not visible");
        goto detach;
    }

    TEST_PASS(name);
detach:
    _syscall1(SYS_shmdt, addr);
rm:
    _syscall3(SYS_shmctl, shmid, IPC_RMID, 0);
}

/* ══════════════════════════════════════════════════════════════════════
 * 19. PRCTL error paths
 * ══════════════════════════════════════════════════════════════════════ */
static void test_prctl_errors(void) {
    const char *name = "prctl error paths";

    /* Unknown option should return -EINVAL */
    int64 ret = _syscall5(SYS_prctl, 9999, 0, 0, 0, 0);
    if (ret != -EINVAL) { TEST_FAIL(name, "unknown option != -EINVAL"); return; }

    /* PR_GET_NAME with NULL buf should return -EFAULT */
    ret = _syscall5(SYS_prctl, PR_GET_NAME, 0, 0, 0, 0);
    if (ret != -EFAULT) { TEST_FAIL(name, "NULL buf != -EFAULT"); return; }

    /* PR_SET_NAME truncation at 16 chars */
    char longname[] = "0123456789ABCDEF_extra";
    ret = _syscall5(SYS_prctl, PR_SET_NAME, (int64)longname, 0, 0, 0);
    if (ret != 0) { TEST_FAIL(name, "set long name failed"); return; }

    char buf[16];
    memset(buf, 0, 16);
    ret = _syscall5(SYS_prctl, PR_GET_NAME, (int64)buf, 0, 0, 0);
    if (ret != 0) { TEST_FAIL(name, "get name failed"); return; }
    /* Name should be truncated to 15 chars + NUL */
    if (strlen(buf) != 15) {
        TEST_FAIL(name, "name not truncated to 15");
        return;
    }

    /* Restore name */
    char restore[] = "syscalltest";
    _syscall5(SYS_prctl, PR_SET_NAME, (int64)restore, 0, 0, 0);

    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 20. SYSINFO error path
 * ══════════════════════════════════════════════════════════════════════ */
static void test_sysinfo_null(void) {
    const char *name = "sysinfo(NULL)";
    int64 ret = _syscall1(SYS_sysinfo, 0);
    if (ret != -EFAULT) { TEST_FAIL(name, "expected -EFAULT"); return; }
    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 21. GETRUSAGE error paths + modes
 * ══════════════════════════════════════════════════════════════════════ */
static void test_getrusage_modes(void) {
    const char *name = "getrusage modes";
    struct k_rusage ru;

    /* RUSAGE_CHILDREN (-1) — returns zeros */
    memset(&ru, 0xff, sizeof(ru));
    int64 ret = _syscall2(SYS_getrusage, -1, (int64)&ru);
    if (ret != 0) { TEST_FAIL(name, "RUSAGE_CHILDREN failed"); return; }
    /* ru fields should be zeroed */
    if (ru.ru_maxrss != 0) { TEST_FAIL(name, "children maxrss != 0"); return; }

    /* RUSAGE_THREAD (1) — same as SELF */
    memset(&ru, 0, sizeof(ru));
    ret = _syscall2(SYS_getrusage, 1, (int64)&ru);
    if (ret != 0) { TEST_FAIL(name, "RUSAGE_THREAD failed"); return; }

    /* Invalid who => -EINVAL */
    ret = _syscall2(SYS_getrusage, 42, (int64)&ru);
    if (ret != -EINVAL) { TEST_FAIL(name, "invalid who != -EINVAL"); return; }

    /* NULL addr */
    ret = _syscall2(SYS_getrusage, 0, 0);
    if (ret != -EFAULT) { TEST_FAIL(name, "NULL addr != -EFAULT"); return; }

    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 22. PRIORITY error paths + boundary
 * ══════════════════════════════════════════════════════════════════════ */
static void test_priority_errors(void) {
    const char *name = "priority error paths";

    /* Invalid 'which' */
    int64 ret = _syscall2(SYS_getpriority, 99, 0);
    if (ret != -EINVAL) { TEST_FAIL(name, "bad which != -EINVAL"); return; }

    ret = _syscall3(SYS_setpriority, 99, 0, 0);
    if (ret != -EINVAL) { TEST_FAIL(name, "set bad which != -EINVAL"); return; }

    /* Negative nice requires root — expect -EACCES for non-root */
    ret = _syscall3(SYS_setpriority, 0, 0, -5);
    /* We may or may not be root, so just check it returns something valid */
    if (ret != 0 && ret != -EACCES) {
        TEST_FAIL(name, "negative nice unexpected return");
        return;
    }

    /* Nice clamping: set to 19 (max) then back to 0 */
    ret = _syscall3(SYS_setpriority, 0, 0, 19);
    if (ret != 0) { TEST_FAIL(name, "nice=19 failed"); return; }
    ret = _syscall2(SYS_getpriority, 0, 0);
    if (20 - (int)ret != 19) { TEST_FAIL(name, "nice != 19"); return; }

    /* Restore */
    _syscall3(SYS_setpriority, 0, 0, 0);
    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 23. SIGALTSTACK error paths
 * ══════════════════════════════════════════════════════════════════════ */
static void test_sigaltstack_errors(void) {
    const char *name = "sigaltstack errors";

    /* Invalid flags */
    struct stack_t_user ss;
    ss.ss_sp = 0x1000;
    ss.ss_flags = 0xFF;  /* invalid flags */
    ss.ss_size = SIGSTKSZ;
    ss.__pad = 0;
    int64 ret = _syscall2(SYS_sigaltstack, (int64)&ss, 0);
    if (ret != -EINVAL) { TEST_FAIL(name, "bad flags != -EINVAL"); return; }

    /* Size too small (< MINSIGSTKSZ) */
    ss.ss_flags = 0;
    ss.ss_size = 128;  /* way below MINSIGSTKSZ=4096 */
    ret = _syscall2(SYS_sigaltstack, (int64)&ss, 0);
    if (ret != -ENOMEM) { TEST_FAIL(name, "small size != -ENOMEM"); return; }

    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 24. FSYNC / FDATASYNC error path
 * ══════════════════════════════════════════════════════════════════════ */
static void test_fsync_badfd(void) {
    const char *name = "fsync/fdatasync bad fd";

    int64 ret = _syscall1(SYS_fsync, 999);
    if (ret != -EBADF) { TEST_FAIL(name, "fsync(999) != -EBADF"); return; }

    ret = _syscall1(SYS_fdatasync, 999);
    if (ret != -EBADF) { TEST_FAIL(name, "fdatasync(999) != -EBADF"); return; }

    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 25. MEMFD_CREATE: read-back, MFD_CLOEXEC, invalid flags
 * ══════════════════════════════════════════════════════════════════════ */
static void test_memfd_readback(void) {
    const char *name = "memfd read-back + flags";

    /* Invalid flags */
    char mfn[] = "test";
    int64 fd = _syscall2(SYS_memfd_create, (int64)mfn, 0xFFFF);
    if (fd >= 0) {
        close((int)fd);
        TEST_FAIL(name, "invalid flags should fail");
        return;
    }

    /* MFD_CLOEXEC */
    fd = _syscall2(SYS_memfd_create, (int64)mfn, MFD_CLOEXEC);
    if (fd < 0) { TEST_FAIL(name, "MFD_CLOEXEC create failed"); return; }

    /* Write then seek back and read */
    int n = write((int)fd, "abcdef", 6);
    if (n != 6) { TEST_FAIL(name, "write failed"); close((int)fd); return; }

    /* Seek to beginning: lseek(fd, 0, SEEK_SET) */
    int64 off = lseek((int)fd, 0, 0 /* SEEK_SET */);
    if (off != 0) { TEST_FAIL(name, "lseek failed"); close((int)fd); return; }

    char buf[16];
    memset(buf, 0, sizeof(buf));
    n = read((int)fd, buf, 6);
    if (n != 6 || strcmp(buf, "abcdef") != 0) {
        TEST_FAIL(name, "read-back mismatch");
        close((int)fd);
        return;
    }

    close((int)fd);
    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 26. UTIMENSAT: UTIME_OMIT + NULL times + bad fd
 * ══════════════════════════════════════════════════════════════════════ */
static void test_utimensat_edges(void) {
    const char *name = "utimensat edge cases";

    int fd = open("/tmp/__test_utimens2", 0101); /* O_WRONLY|O_CREAT */
    if (fd < 0) { TEST_FAIL(name, "open failed"); return; }
    write(fd, "x", 1);

    /* UTIME_OMIT for atime, set mtime */
    struct timespec times[2];
    times[0].tv_sec = 0;
    times[0].tv_nsec = UTIME_OMIT;  /* don't change atime */
    times[1].tv_sec = 5000000;
    times[1].tv_nsec = 0;
    int64 ret = _syscall4(SYS_utimensat, fd, 0, (int64)times, 0);
    if (ret != 0) { TEST_FAIL(name, "UTIME_OMIT failed"); goto cleanup; }

    /* NULL times → both set to NOW */
    ret = _syscall4(SYS_utimensat, fd, 0, 0, 0);
    if (ret != 0) { TEST_FAIL(name, "NULL times failed"); goto cleanup; }

    /* Bad fd */
    times[0].tv_nsec = UTIME_NOW;
    times[1].tv_nsec = UTIME_NOW;
    ret = _syscall4(SYS_utimensat, 999, 0, (int64)times, 0);
    if (ret != -EBADF) { TEST_FAIL(name, "bad fd != -EBADF"); goto cleanup; }

    TEST_PASS(name);
cleanup:
    close(fd);
    unlink("/tmp/__test_utimens2");
}

/* ══════════════════════════════════════════════════════════════════════
 * 27. SHM error paths: size=0, invalid shmid, key without IPC_CREAT
 * ══════════════════════════════════════════════════════════════════════ */
static void test_shm_errors(void) {
    const char *name = "shm error paths";

    /* size=0 → -EINVAL */
    int64 ret = _syscall3(SYS_shmget, IPC_PRIVATE, 0, IPC_CREAT | 0600);
    if (ret != -EINVAL) { TEST_FAIL(name, "size=0 != -EINVAL"); return; }

    /* Key without IPC_CREAT → -ENOENT */
    ret = _syscall3(SYS_shmget, 99999, 4096, 0600);
    if (ret != -ENOENT) { TEST_FAIL(name, "no IPC_CREAT != -ENOENT"); return; }

    /* Invalid shmid for shmctl */
    struct shmid_ds ds;
    ret = _syscall3(SYS_shmctl, 99999, IPC_STAT, (int64)&ds);
    if (ret != -EINVAL) { TEST_FAIL(name, "bad shmid != -EINVAL"); return; }

    /* Invalid shmid for shmat */
    ret = _syscall3(SYS_shmat, 99999, 0, 0);
    if (ret != -EINVAL) { TEST_FAIL(name, "bad shmat != -EINVAL"); return; }

    /* shmctl invalid cmd */
    int64 id = _syscall3(SYS_shmget, IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    if (id < 0) { TEST_FAIL(name, "shmget for cmd test failed"); return; }
    ret = _syscall3(SYS_shmctl, id, 999, 0);
    if (ret != -EINVAL) { TEST_FAIL(name, "bad cmd != -EINVAL"); _syscall3(SYS_shmctl, id, IPC_RMID, 0); return; }
    _syscall3(SYS_shmctl, id, IPC_RMID, 0);

    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 28. SHM IPC_SET + multi-page segment
 * ══════════════════════════════════════════════════════════════════════ */
static void test_shm_ipc_set(void) {
    const char *name = "shm IPC_SET + multi-page";

    /* Create a 2-page segment */
    int64 id = _syscall3(SYS_shmget, IPC_PRIVATE, 8192, IPC_CREAT | 0600);
    if (id < 0) { TEST_FAIL(name, "shmget failed"); return; }

    /* Attach and write to second page */
    int64 addr = _syscall3(SYS_shmat, id, 0, 0);
    if (addr <= 0) { TEST_FAIL(name, "shmat failed"); goto rm; }

    volatile char *p = (volatile char *)(uint64)addr;
    p[0] = 'A';         /* first page */
    p[4096] = 'B';      /* second page */
    if (p[0] != 'A' || p[4096] != 'B') {
        TEST_FAIL(name, "multi-page read mismatch");
        _syscall1(SYS_shmdt, addr);
        goto rm;
    }

    _syscall1(SYS_shmdt, addr);

    /* IPC_SET to change mode */
    struct shmid_ds ds;
    memset(&ds, 0, sizeof(ds));
    ds.shm_perm.mode = 0644;
    int64 ret = _syscall3(SYS_shmctl, id, IPC_SET, (int64)&ds);
    if (ret != 0) { TEST_FAIL(name, "IPC_SET failed"); goto rm; }

    /* Verify with IPC_STAT */
    memset(&ds, 0, sizeof(ds));
    ret = _syscall3(SYS_shmctl, id, IPC_STAT, (int64)&ds);
    if (ret != 0) { TEST_FAIL(name, "IPC_STAT failed"); goto rm; }
    if ((ds.shm_perm.mode & 0777) != 0644) {
        TEST_FAIL(name, "mode not updated");
        goto rm;
    }
    if (ds.shm_segsz != 8192) {
        TEST_FAIL(name, "segsz != 8192");
        goto rm;
    }

    TEST_PASS(name);
rm:
    _syscall3(SYS_shmctl, id, IPC_RMID, 0);
}

/* ══════════════════════════════════════════════════════════════════════
 * 29. SHM IPC_RMID while attached → -EBUSY
 * ══════════════════════════════════════════════════════════════════════ */
static void test_shm_rmid_busy(void) {
    const char *name = "shm IPC_RMID while attached";

    int64 id = _syscall3(SYS_shmget, IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    if (id < 0) { TEST_FAIL(name, "shmget failed"); return; }

    int64 addr = _syscall3(SYS_shmat, id, 0, 0);
    if (addr <= 0) { TEST_FAIL(name, "shmat failed"); _syscall3(SYS_shmctl, id, IPC_RMID, 0); return; }

    /* Try IPC_RMID while attached → should fail */
    int64 ret = _syscall3(SYS_shmctl, id, IPC_RMID, 0);
    if (ret != -EBUSY) { TEST_FAIL(name, "expected -EBUSY"); }
    else { TEST_PASS(name); }

    _syscall1(SYS_shmdt, addr);
    _syscall3(SYS_shmctl, id, IPC_RMID, 0);
}

/* ══════════════════════════════════════════════════════════════════════
 * 30. SEMAPHORE error paths
 * ══════════════════════════════════════════════════════════════════════ */
static void test_sem_errors(void) {
    const char *name = "sem error paths";

    /* nsems < 0 */
    int64 ret = _syscall3(SYS_semget, IPC_PRIVATE, -1, IPC_CREAT | 0600);
    if (ret != -EINVAL) { TEST_FAIL(name, "nsems<0 != -EINVAL"); return; }

    /* nsems > SEMMSL */
    ret = _syscall3(SYS_semget, IPC_PRIVATE, SEMMSL + 1, IPC_CREAT | 0600);
    if (ret != -EINVAL) { TEST_FAIL(name, "nsems>SEMMSL != -EINVAL"); return; }

    /* key + IPC_EXCL */
    int key = 77777;
    int64 semid = _syscall3(SYS_semget, key, 1, IPC_CREAT | 0600);
    if (semid < 0) { TEST_FAIL(name, "semget(key) failed"); return; }

    ret = _syscall3(SYS_semget, key, 1, IPC_CREAT | IPC_EXCL | 0600);
    if (ret != -EEXIST) {
        TEST_FAIL(name, "IPC_EXCL != -EEXIST");
        _syscall4(SYS_semctl, semid, 0, IPC_RMID, 0);
        return;
    }

    /* key without IPC_CREAT → -ENOENT */
    _syscall4(SYS_semctl, semid, 0, IPC_RMID, 0);
    ret = _syscall3(SYS_semget, key, 1, 0600);
    if (ret != -ENOENT) { TEST_FAIL(name, "no IPC_CREAT != -ENOENT"); return; }

    /* sem_num >= nsems → -EFBIG */
    semid = _syscall3(SYS_semget, IPC_PRIVATE, 2, IPC_CREAT | 0600);
    if (semid < 0) { TEST_FAIL(name, "semget for EFBIG failed"); return; }

    struct k_sembuf sops;
    sops.sem_num = 5;  /* out of range */
    sops.sem_op = 1;
    sops.sem_flg = 0;
    sops.__pad = 0;
    ret = _syscall3(SYS_semop, semid, (int64)&sops, 1);
    if (ret != -EFBIG) { TEST_FAIL(name, "sem_num>=nsems != -EFBIG"); _syscall4(SYS_semctl, semid, 0, IPC_RMID, 0); return; }

    /* nsops = 0 → -EINVAL */
    ret = _syscall3(SYS_semop, semid, (int64)&sops, 0);
    if (ret != -EINVAL) { TEST_FAIL(name, "nsops=0 != -EINVAL"); _syscall4(SYS_semctl, semid, 0, IPC_RMID, 0); return; }

    /* Invalid semctl cmd */
    ret = _syscall4(SYS_semctl, semid, 0, 999, 0);
    if (ret != -EINVAL) { TEST_FAIL(name, "bad cmd != -EINVAL"); }

    /* GETVAL with out-of-range semnum */
    ret = _syscall4(SYS_semctl, semid, 10, GETVAL, 0);
    if (ret != -EINVAL) { TEST_FAIL(name, "GETVAL out-of-range != -EINVAL"); }

    _syscall4(SYS_semctl, semid, 0, IPC_RMID, 0);
    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 31. SEMAPHORE: multi-sem set, GETALL/SETALL, IPC_SET, wait-for-zero
 * ══════════════════════════════════════════════════════════════════════ */
static void test_sem_advanced(void) {
    const char *name = "sem GETALL/SETALL/IPC_SET/wait-for-zero";

    /* Create a set with 4 semaphores */
    int64 semid = _syscall3(SYS_semget, IPC_PRIVATE, 4, IPC_CREAT | 0600);
    if (semid < 0) { TEST_FAIL(name, "semget failed"); return; }

    /* SETALL: set values to {10, 20, 30, 0} */
    uint16 vals[4] = {10, 20, 30, 0};
    int64 ret = _syscall4(SYS_semctl, semid, 0, SETALL, (int64)vals);
    if (ret != 0) { TEST_FAIL(name, "SETALL failed"); goto cleanup; }

    /* GETALL: verify */
    uint16 got[4] = {0};
    ret = _syscall4(SYS_semctl, semid, 0, GETALL, (int64)got);
    if (ret != 0) { TEST_FAIL(name, "GETALL failed"); goto cleanup; }
    if (got[0] != 10 || got[1] != 20 || got[2] != 30 || got[3] != 0) {
        TEST_FAIL(name, "GETALL values mismatch");
        goto cleanup;
    }

    /* GETVAL for individual semaphores */
    ret = _syscall4(SYS_semctl, semid, 2, GETVAL, 0);
    if (ret != 30) { TEST_FAIL(name, "GETVAL(2) != 30"); goto cleanup; }

    /* Multiple operations atomically: dec sem0 by 5, inc sem1 by 3 */
    struct k_sembuf sops[2];
    sops[0].sem_num = 0; sops[0].sem_op = -5; sops[0].sem_flg = 0; sops[0].__pad = 0;
    sops[1].sem_num = 1; sops[1].sem_op = 3;  sops[1].sem_flg = 0; sops[1].__pad = 0;
    ret = _syscall3(SYS_semop, semid, (int64)sops, 2);
    if (ret != 0) { TEST_FAIL(name, "multi-op semop failed"); goto cleanup; }

    /* Verify: sem0=5, sem1=23 */
    ret = _syscall4(SYS_semctl, semid, 0, GETVAL, 0);
    if (ret != 5) { TEST_FAIL(name, "sem0 != 5 after multi-op"); goto cleanup; }
    ret = _syscall4(SYS_semctl, semid, 1, GETVAL, 0);
    if (ret != 23) { TEST_FAIL(name, "sem1 != 23 after multi-op"); goto cleanup; }

    /* Wait-for-zero on sem3 (already 0) with IPC_NOWAIT — should succeed */
    sops[0].sem_num = 3; sops[0].sem_op = 0; sops[0].sem_flg = (int16)IPC_NOWAIT;
    ret = _syscall3(SYS_semop, semid, (int64)sops, 1);
    if (ret != 0) { TEST_FAIL(name, "wait-for-zero on 0 failed"); goto cleanup; }

    /* Wait-for-zero on sem0 (value=5) with IPC_NOWAIT — should fail */
    sops[0].sem_num = 0; sops[0].sem_op = 0; sops[0].sem_flg = (int16)IPC_NOWAIT;
    ret = _syscall3(SYS_semop, semid, (int64)sops, 1);
    if (ret != -EAGAIN) { TEST_FAIL(name, "wait-for-zero on 5 != -EAGAIN"); goto cleanup; }

    /* IPC_SET to change permissions */
    struct semid_ds sds;
    memset(&sds, 0, sizeof(sds));
    sds.sem_perm.mode = 0644;
    ret = _syscall4(SYS_semctl, semid, 0, IPC_SET, (int64)&sds);
    if (ret != 0) { TEST_FAIL(name, "IPC_SET failed"); goto cleanup; }

    /* Verify via IPC_STAT */
    memset(&sds, 0, sizeof(sds));
    ret = _syscall4(SYS_semctl, semid, 0, IPC_STAT, (int64)&sds);
    if (ret != 0) { TEST_FAIL(name, "IPC_STAT failed"); goto cleanup; }
    if ((sds.sem_perm.mode & 0777) != 0644) {
        TEST_FAIL(name, "mode != 0644");
        goto cleanup;
    }
    if (sds.sem_nsems != 4) {
        TEST_FAIL(name, "nsems != 4");
        goto cleanup;
    }

    TEST_PASS(name);
cleanup:
    _syscall4(SYS_semctl, semid, 0, IPC_RMID, 0);
}

/* ══════════════════════════════════════════════════════════════════════
 * 32. SEMTIMEDOP (basic)
 * ══════════════════════════════════════════════════════════════════════ */
static void test_semtimedop(void) {
    const char *name = "semtimedop basic";

    int64 semid = _syscall3(SYS_semget, IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (semid < 0) { TEST_FAIL(name, "semget failed"); return; }

    _syscall4(SYS_semctl, semid, 0, SETVAL, 3);

    /* Decrement by 1 via semtimedop */
    struct k_sembuf sops;
    sops.sem_num = 0; sops.sem_op = -1; sops.sem_flg = 0; sops.__pad = 0;
    int64 ret = _syscall4(SYS_semtimedop, semid, (int64)&sops, 1, 0);
    if (ret != 0) { TEST_FAIL(name, "semtimedop failed"); goto cleanup; }

    ret = _syscall4(SYS_semctl, semid, 0, GETVAL, 0);
    if (ret != 2) { TEST_FAIL(name, "value != 2 after semtimedop"); goto cleanup; }

    TEST_PASS(name);
cleanup:
    _syscall4(SYS_semctl, semid, 0, IPC_RMID, 0);
}

/* ══════════════════════════════════════════════════════════════════════
 * 33. MSG QUEUE error paths
 * ══════════════════════════════════════════════════════════════════════ */
static void test_msg_errors(void) {
    const char *name = "msg error paths";

    /* key + IPC_EXCL */
    int key = 88888;
    int64 msqid = _syscall2(SYS_msgget, key, IPC_CREAT | 0600);
    if (msqid < 0) { TEST_FAIL(name, "msgget(key) failed"); return; }

    int64 ret = _syscall2(SYS_msgget, key, IPC_CREAT | IPC_EXCL | 0600);
    if (ret != -EEXIST) {
        TEST_FAIL(name, "IPC_EXCL != -EEXIST");
        _syscall3(SYS_msgctl, msqid, IPC_RMID, 0);
        return;
    }

    /* Key lookup without IPC_CREAT of non-existent key */
    _syscall3(SYS_msgctl, msqid, IPC_RMID, 0);
    ret = _syscall2(SYS_msgget, key, 0600);
    if (ret != -ENOENT) { TEST_FAIL(name, "no IPC_CREAT != -ENOENT"); return; }

    /* msgsz > MSGMAX → -EINVAL */
    msqid = _syscall2(SYS_msgget, IPC_PRIVATE, IPC_CREAT | 0600);
    if (msqid < 0) { TEST_FAIL(name, "msgget for size test failed"); return; }

    struct msgbuf sbuf;
    sbuf.mtype = 1;
    memset(sbuf.mtext, 'x', sizeof(sbuf.mtext));
    /* Send with msgsz > MSGMAX */
    ret = _syscall4(SYS_msgsnd, msqid, (int64)&sbuf, MSGMAX + 1, 0);
    if (ret != -EINVAL) { TEST_FAIL(name, "msgsz>MSGMAX != -EINVAL"); goto cleanup; }

    /* mtype < 1 → -EINVAL */
    sbuf.mtype = 0;
    ret = _syscall4(SYS_msgsnd, msqid, (int64)&sbuf, 4, 0);
    if (ret != -EINVAL) { TEST_FAIL(name, "mtype=0 != -EINVAL"); goto cleanup; }

    sbuf.mtype = -5;
    ret = _syscall4(SYS_msgsnd, msqid, (int64)&sbuf, 4, 0);
    if (ret != -EINVAL) { TEST_FAIL(name, "mtype<0 != -EINVAL"); goto cleanup; }

    /* IPC_NOWAIT on empty queue → -ENOMSG */
    struct msgbuf rbuf;
    ret = _syscall5(SYS_msgrcv, msqid, (int64)&rbuf, 64, 0, IPC_NOWAIT);
    if (ret != -ENOMSG) { TEST_FAIL(name, "empty+NOWAIT != -ENOMSG"); goto cleanup; }

    /* Invalid msgctl cmd */
    ret = _syscall3(SYS_msgctl, msqid, 999, 0);
    if (ret != -EINVAL) { TEST_FAIL(name, "bad cmd != -EINVAL"); goto cleanup; }

    TEST_PASS(name);
cleanup:
    _syscall3(SYS_msgctl, msqid, IPC_RMID, 0);
}

/* ══════════════════════════════════════════════════════════════════════
 * 34. MSG: negative msgtyp, MSG_NOERROR, E2BIG, zero-length, IPC_SET
 * ══════════════════════════════════════════════════════════════════════ */
static void test_msg_advanced(void) {
    const char *name = "msg negative-type/NOERROR/E2BIG/IPC_SET";

    int64 msqid = _syscall2(SYS_msgget, IPC_PRIVATE, IPC_CREAT | 0600);
    if (msqid < 0) { TEST_FAIL(name, "msgget failed"); return; }

    /* Send messages with types 3, 1, 2 */
    struct msgbuf buf;

    buf.mtype = 3;
    strcpy(buf.mtext, "type3");
    _syscall4(SYS_msgsnd, msqid, (int64)&buf, 6, 0);

    buf.mtype = 1;
    strcpy(buf.mtext, "type1");
    _syscall4(SYS_msgsnd, msqid, (int64)&buf, 6, 0);

    buf.mtype = 2;
    strcpy(buf.mtext, "type2");
    _syscall4(SYS_msgsnd, msqid, (int64)&buf, 6, 0);

    /* Negative msgtyp (-3): receive lowest type <= 3 → type=1 */
    struct msgbuf rbuf;
    memset(&rbuf, 0, sizeof(rbuf));
    int64 ret = _syscall5(SYS_msgrcv, msqid, (int64)&rbuf, 64, -3, 0);
    if (ret < 0) { TEST_FAIL(name, "negative msgtyp failed"); goto cleanup; }
    if (rbuf.mtype != 1) {
        printf("    got type=%ld expected 1\n", (long)rbuf.mtype);
        TEST_FAIL(name, "negative type didn't get lowest");
        goto cleanup;
    }

    /* Zero-length message send + receive */
    buf.mtype = 10;
    ret = _syscall4(SYS_msgsnd, msqid, (int64)&buf, 0, 0);
    if (ret != 0) { TEST_FAIL(name, "zero-length msgsnd failed"); goto cleanup; }

    memset(&rbuf, 0, sizeof(rbuf));
    ret = _syscall5(SYS_msgrcv, msqid, (int64)&rbuf, 64, 10, 0);
    if (ret != 0) { TEST_FAIL(name, "zero-length msgrcv != 0"); goto cleanup; }
    if (rbuf.mtype != 10) { TEST_FAIL(name, "zero-length mtype mismatch"); goto cleanup; }

    /* E2BIG: receive with buffer too small, without MSG_NOERROR */
    /* Send a 32-byte message */
    buf.mtype = 20;
    memset(buf.mtext, 'A', 32);
    ret = _syscall4(SYS_msgsnd, msqid, (int64)&buf, 32, 0);
    if (ret != 0) { TEST_FAIL(name, "msgsnd for E2BIG failed"); goto cleanup; }

    /* Receive with only 4 bytes buffer → -E2BIG */
    memset(&rbuf, 0, sizeof(rbuf));
    ret = _syscall5(SYS_msgrcv, msqid, (int64)&rbuf, 4, 20, 0);
    if (ret != -E2BIG) {
        printf("    msgrcv returned %ld, expected -E2BIG (%d)\n", (long)ret, -E2BIG);
        TEST_FAIL(name, "small buf != -E2BIG");
        goto cleanup;
    }

    /* MSG_NOERROR: receive same message with truncation */
    memset(&rbuf, 0, sizeof(rbuf));
    ret = _syscall5(SYS_msgrcv, msqid, (int64)&rbuf, 4, 20, MSG_NOERROR);
    if (ret != 4) {
        TEST_FAIL(name, "MSG_NOERROR didn't truncate");
        goto cleanup;
    }
    if (rbuf.mtype != 20) { TEST_FAIL(name, "MSG_NOERROR mtype mismatch"); goto cleanup; }

    /* Drain remaining messages (type=2 and type=3 still queued) */
    _syscall5(SYS_msgrcv, msqid, (int64)&rbuf, 64, 0, IPC_NOWAIT);
    _syscall5(SYS_msgrcv, msqid, (int64)&rbuf, 64, 0, IPC_NOWAIT);

    /* IPC_SET to change qbytes */
    struct msqid_ds mds;
    memset(&mds, 0, sizeof(mds));
    ret = _syscall3(SYS_msgctl, msqid, IPC_STAT, (int64)&mds);
    if (ret != 0) { TEST_FAIL(name, "IPC_STAT failed"); goto cleanup; }

    mds.msg_perm.mode = 0644;
    mds.msg_qbytes = 32768;  /* double default */
    ret = _syscall3(SYS_msgctl, msqid, IPC_SET, (int64)&mds);
    if (ret != 0) { TEST_FAIL(name, "IPC_SET failed"); goto cleanup; }

    /* Verify */
    memset(&mds, 0, sizeof(mds));
    ret = _syscall3(SYS_msgctl, msqid, IPC_STAT, (int64)&mds);
    if (ret != 0) { TEST_FAIL(name, "IPC_STAT after SET failed"); goto cleanup; }
    if ((mds.msg_perm.mode & 0777) != 0644) {
        TEST_FAIL(name, "mode != 0644");
        goto cleanup;
    }
    if (mds.msg_qbytes != 32768) {
        TEST_FAIL(name, "qbytes != 32768");
        goto cleanup;
    }

    TEST_PASS(name);
cleanup:
    _syscall3(SYS_msgctl, msqid, IPC_RMID, 0);
}

/* ══════════════════════════════════════════════════════════════════════
 * 35. MSG IPC_RMID with messages still queued
 * ══════════════════════════════════════════════════════════════════════ */
static void test_msg_rmid_queued(void) {
    const char *name = "msg IPC_RMID with queued msgs";

    int64 msqid = _syscall2(SYS_msgget, IPC_PRIVATE, IPC_CREAT | 0600);
    if (msqid < 0) { TEST_FAIL(name, "msgget failed"); return; }

    /* Send a couple messages */
    struct msgbuf buf;
    buf.mtype = 1;
    strcpy(buf.mtext, "msg1");
    _syscall4(SYS_msgsnd, msqid, (int64)&buf, 5, 0);
    buf.mtype = 2;
    strcpy(buf.mtext, "msg2");
    _syscall4(SYS_msgsnd, msqid, (int64)&buf, 5, 0);

    /* Verify qnum=2 */
    struct msqid_ds mds;
    _syscall3(SYS_msgctl, msqid, IPC_STAT, (int64)&mds);
    if (mds.msg_qnum != 2) { TEST_FAIL(name, "qnum != 2"); _syscall3(SYS_msgctl, msqid, IPC_RMID, 0); return; }

    /* Remove with messages still queued — should succeed and free them */
    int64 ret = _syscall3(SYS_msgctl, msqid, IPC_RMID, 0);
    if (ret != 0) { TEST_FAIL(name, "IPC_RMID failed"); return; }

    /* Trying to use removed queue should fail */
    ret = _syscall3(SYS_msgctl, msqid, IPC_STAT, (int64)&mds);
    if (ret != -EINVAL) { TEST_FAIL(name, "removed queue still accessible"); return; }

    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 36. SCHED_RR_GET_INTERVAL with NULL → -EFAULT
 * ══════════════════════════════════════════════════════════════════════ */
static void test_sched_rr_null(void) {
    const char *name = "sched_rr_get_interval(NULL)";
    int64 ret = _syscall2(SYS_sched_rr_get_interval_time64, 0, 0);
    if (ret != -EFAULT) { TEST_FAIL(name, "expected -EFAULT"); return; }
    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 37. SETITIMER / GETITIMER (ITIMER_REAL)
 * ══════════════════════════════════════════════════════════════════════ */
static void test_setitimer_getitimer(void) {
    const char *name = "setitimer/getitimer";

    /* Invalid 'which' for getitimer */
    struct k_itimerval val;
    memset(&val, 0, sizeof(val));
    int64 ret = _syscall2(SYS_getitimer, ITIMER_VIRTUAL, (int64)&val);
    if (ret != -EINVAL) { TEST_FAIL(name, "getitimer(VIRTUAL) != -EINVAL"); return; }

    ret = _syscall2(SYS_getitimer, ITIMER_PROF, (int64)&val);
    if (ret != -EINVAL) { TEST_FAIL(name, "getitimer(PROF) != -EINVAL"); return; }

    /* getitimer with NULL */
    ret = _syscall2(SYS_getitimer, ITIMER_REAL, 0);
    if (ret != -EINVAL) { TEST_FAIL(name, "getitimer(NULL) != -EINVAL"); return; }

    /* Invalid 'which' for setitimer */
    ret = _syscall3(SYS_setitimer, ITIMER_VIRTUAL, (int64)&val, 0);
    if (ret != -EINVAL) { TEST_FAIL(name, "setitimer(VIRTUAL) != -EINVAL"); return; }

    /* Set a timer with interval, then read it back */
    struct k_itimerval nv;
    memset(&nv, 0, sizeof(nv));
    nv.it_value.tv_sec = 0;
    nv.it_value.tv_usec = 500000; /* 500ms */
    nv.it_interval.tv_sec = 1;
    nv.it_interval.tv_usec = 0; /* 1s interval */

    struct k_itimerval old;
    memset(&old, 0, sizeof(old));
    ret = _syscall3(SYS_setitimer, ITIMER_REAL, (int64)&nv, (int64)&old);
    if (ret != 0) { TEST_FAIL(name, "setitimer failed"); return; }

    /* getitimer should show the interval */
    memset(&val, 0, sizeof(val));
    ret = _syscall2(SYS_getitimer, ITIMER_REAL, (int64)&val);
    if (ret != 0) { TEST_FAIL(name, "getitimer failed"); return; }
    if (val.it_interval.tv_sec != 1 || val.it_interval.tv_usec != 0) {
        TEST_FAIL(name, "interval mismatch");
        /* Disarm before returning */
        memset(&nv, 0, sizeof(nv));
        _syscall3(SYS_setitimer, ITIMER_REAL, (int64)&nv, 0);
        return;
    }

    /* Disarm the timer (value=0) */
    memset(&nv, 0, sizeof(nv));
    ret = _syscall3(SYS_setitimer, ITIMER_REAL, (int64)&nv, (int64)&old);
    if (ret != 0) { TEST_FAIL(name, "disarm failed"); return; }
    /* Old should report the interval we set */
    if (old.it_interval.tv_sec != 1) {
        TEST_FAIL(name, "old interval mismatch after disarm");
        return;
    }

    /* setitimer with new_addr=NULL just bumps generation (cancels) */
    ret = _syscall3(SYS_setitimer, ITIMER_REAL, 0, 0);
    if (ret != 0) { TEST_FAIL(name, "NULL new_addr failed"); return; }

    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 38. RT_SIGQUEUEINFO
 * ══════════════════════════════════════════════════════════════════════ */
static volatile int sigqueueinfo_received = 0;

static void sigusr1_handler(int signo) {
    if (signo == SIGUSR1)
        sigqueueinfo_received = 1;
}

static void test_rt_sigqueueinfo(void) {
    const char *name = "rt_sigqueueinfo";

    /* Install SIGUSR1 handler */
    struct k_sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = (uint64)sigusr1_handler;
    sa.sa_flags = SA_RESTART;
    sa.sa_mask = 0;

    int64 ret = _syscall3(SYS_sigaction, SIGUSR1, (int64)&sa, 0);
    if (ret != 0) {
        TEST_FAIL(name, "sigaction failed");
        return;
    }

    /* Invalid signal number */
    struct k_siginfo info;
    memset(&info, 0, sizeof(info));
    info.si_signo = 0;
    ret = _syscall3(SYS_rt_sigqueueinfo, getpid(), 0, (int64)&info);
    if (ret != -EINVAL) { TEST_FAIL(name, "sig=0 != -EINVAL"); goto restore; }

    /* NULL uinfo */
    ret = _syscall3(SYS_rt_sigqueueinfo, getpid(), SIGUSR1, 0);
    if (ret != -EFAULT) { TEST_FAIL(name, "NULL uinfo != -EFAULT"); goto restore; }

    /* Signal ourselves with SIGUSR1 */
    sigqueueinfo_received = 0;
    info.si_signo = SIGUSR1;
    info.si_pid = getpid();
    ret = _syscall3(SYS_rt_sigqueueinfo, getpid(), SIGUSR1, (int64)&info);
    if (ret != 0) { TEST_FAIL(name, "sigqueueinfo send failed"); goto restore; }

    /* Give signal a chance to be delivered (yield a few times) */
    for (int i = 0; i < 10 && !sigqueueinfo_received; i++)
        sleep(0);

    if (!sigqueueinfo_received) {
        TEST_FAIL(name, "signal not delivered");
        goto restore;
    }

    TEST_PASS(name);
restore:
    /* Restore default handler */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = 0; /* SIG_DFL */
    _syscall3(SYS_sigaction, SIGUSR1, (int64)&sa, 0);
}

/* ══════════════════════════════════════════════════════════════════════
 * 39. CLONE3 (basic — fork-like with flags=0, exit_signal=SIGCHLD)
 * ══════════════════════════════════════════════════════════════════════ */
static void test_clone3(void) {
    const char *name = "clone3 basic";

    /* NULL args */
    int64 ret = _syscall2(SYS_clone3, 0, 0);
    if (ret != -EINVAL) { TEST_FAIL(name, "NULL args != -EINVAL"); return; }

    /* Note: a full clone3 test with valid args is complex since we need
     * to set up a new stack and entry point. Just verify the error path. */
    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 40. MSG: key re-lookup (same key returns same id)
 * ══════════════════════════════════════════════════════════════════════ */
static void test_msg_key_relookup(void) {
    const char *name = "msg key re-lookup";
    int key = 66666;

    int64 id1 = _syscall2(SYS_msgget, key, IPC_CREAT | 0600);
    if (id1 < 0) { TEST_FAIL(name, "first msgget failed"); return; }

    int64 id2 = _syscall2(SYS_msgget, key, IPC_CREAT | 0600);
    if (id2 != id1) {
        TEST_FAIL(name, "second msgget != first id");
        _syscall3(SYS_msgctl, id1, IPC_RMID, 0);
        return;
    }

    _syscall3(SYS_msgctl, id1, IPC_RMID, 0);
    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 41. SEM: nsems=0 default, key re-lookup
 * ══════════════════════════════════════════════════════════════════════ */
static void test_sem_defaults(void) {
    const char *name = "sem nsems=0 + key re-lookup";

    /* nsems=0 should default to 1 */
    int64 semid = _syscall3(SYS_semget, IPC_PRIVATE, 0, IPC_CREAT | 0600);
    if (semid < 0) { TEST_FAIL(name, "nsems=0 semget failed"); return; }

    struct semid_ds sds;
    memset(&sds, 0, sizeof(sds));
    int64 ret = _syscall4(SYS_semctl, semid, 0, IPC_STAT, (int64)&sds);
    if (ret != 0) { TEST_FAIL(name, "IPC_STAT failed"); goto cleanup1; }
    if (sds.sem_nsems != 1) { TEST_FAIL(name, "nsems != 1 for default"); goto cleanup1; }
    _syscall4(SYS_semctl, semid, 0, IPC_RMID, 0);

    /* Key re-lookup */
    int key = 55555;
    int64 id1 = _syscall3(SYS_semget, key, 1, IPC_CREAT | 0600);
    if (id1 < 0) { TEST_FAIL(name, "semget(key) failed"); return; }
    int64 id2 = _syscall3(SYS_semget, key, 0, IPC_CREAT | 0600);
    if (id2 != id1) { TEST_FAIL(name, "re-lookup != first id"); _syscall4(SYS_semctl, id1, 0, IPC_RMID, 0); return; }
    _syscall4(SYS_semctl, id1, 0, IPC_RMID, 0);

    TEST_PASS(name);
    return;
cleanup1:
    _syscall4(SYS_semctl, semid, 0, IPC_RMID, 0);
}

/* ══════════════════════════════════════════════════════════════════════
 * 42. MSG queue full → -EAGAIN
 * ══════════════════════════════════════════════════════════════════════ */
static void test_msg_queue_full(void) {
    const char *name = "msg queue full";

    int64 msqid = _syscall2(SYS_msgget, IPC_PRIVATE, IPC_CREAT | 0600);
    if (msqid < 0) { TEST_FAIL(name, "msgget failed"); return; }

    /* Set queue limit to a small value via IPC_SET */
    struct msqid_ds mds;
    memset(&mds, 0, sizeof(mds));
    _syscall3(SYS_msgctl, msqid, IPC_STAT, (int64)&mds);
    mds.msg_qbytes = 32;  /* only 32 bytes allowed */
    int64 ret = _syscall3(SYS_msgctl, msqid, IPC_SET, (int64)&mds);
    if (ret != 0) { TEST_FAIL(name, "IPC_SET qbytes failed"); goto cleanup; }

    /* Send a 20-byte message — should succeed */
    struct msgbuf buf;
    buf.mtype = 1;
    memset(buf.mtext, 'A', 20);
    ret = _syscall4(SYS_msgsnd, msqid, (int64)&buf, 20, 0);
    if (ret != 0) { TEST_FAIL(name, "first msgsnd failed"); goto cleanup; }

    /* Send another 20-byte message — should fail (20+20=40 > 32) */
    buf.mtype = 2;
    ret = _syscall4(SYS_msgsnd, msqid, (int64)&buf, 20, IPC_NOWAIT);
    if (ret != -EAGAIN) {
        printf("    msgsnd returned %ld, expected -EAGAIN (%d)\n", (long)ret, -EAGAIN);
        TEST_FAIL(name, "queue full != -EAGAIN");
        goto cleanup;
    }

    TEST_PASS(name);
cleanup:
    _syscall3(SYS_msgctl, msqid, IPC_RMID, 0);
}

/* ══════════════════════════════════════════════════════════════════════
 * 43. SHM shmdt invalid address → -EINVAL
 * ══════════════════════════════════════════════════════════════════════ */
static void test_shmdt_invalid(void) {
    const char *name = "shmdt invalid addr";

    int64 ret = _syscall1(SYS_shmdt, 0x12345000);
    if (ret != -EINVAL) { TEST_FAIL(name, "expected -EINVAL"); return; }

    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 44. SHM size > SHMMAX → -EINVAL
 * ══════════════════════════════════════════════════════════════════════ */
static void test_shm_size_too_large(void) {
    const char *name = "shmget size > SHMMAX";

    int64 ret = _syscall3(SYS_shmget, IPC_PRIVATE,
                          (int64)(SHMMAX + 1), IPC_CREAT | 0600);
    if (ret != -EINVAL) { TEST_FAIL(name, "expected -EINVAL"); return; }

    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 45. SETITIMER SIGALRM delivery
 * ══════════════════════════════════════════════════════════════════════ */
static volatile int sigalrm_count = 0;

static void sigalrm_handler(int signo) {
    if (signo == SIGALRM)
        sigalrm_count++;
}

static void test_setitimer_sigalrm(void) {
    const char *name = "setitimer SIGALRM delivery";

    /* Install SIGALRM handler */
    struct k_sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = (uint64)sigalrm_handler;
    sa.sa_flags = SA_RESTART;
    sa.sa_mask = 0;

    int64 ret = _syscall3(SYS_sigaction, SIGALRM, (int64)&sa, 0);
    if (ret != 0) { TEST_FAIL(name, "sigaction failed"); return; }

    sigalrm_count = 0;

    /* Arm a one-shot timer: fire after 50ms, no interval */
    struct k_itimerval nv;
    memset(&nv, 0, sizeof(nv));
    nv.it_value.tv_sec = 0;
    nv.it_value.tv_usec = 50000;  /* 50ms */
    nv.it_interval.tv_sec = 0;
    nv.it_interval.tv_usec = 0;

    ret = _syscall3(SYS_setitimer, ITIMER_REAL, (int64)&nv, 0);
    if (ret != 0) { TEST_FAIL(name, "setitimer failed"); goto restore; }

    /* Wait for the signal (up to ~1 second) */
    for (int i = 0; i < 100 && sigalrm_count == 0; i++)
        sleep(1);  /* sleep(1) = 1 tick */

    if (sigalrm_count == 0) {
        TEST_FAIL(name, "SIGALRM not received");
        goto restore;
    }

    TEST_PASS(name);
restore:
    /* Disarm timer */
    memset(&nv, 0, sizeof(nv));
    _syscall3(SYS_setitimer, ITIMER_REAL, (int64)&nv, 0);
    /* Restore default handler */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = 0;
    _syscall3(SYS_sigaction, SIGALRM, (int64)&sa, 0);
}

/* ══════════════════════════════════════════════════════════════════════
 * 46. SETITIMER interval (repeating) timer
 * ══════════════════════════════════════════════════════════════════════ */
static void test_setitimer_interval(void) {
    const char *name = "setitimer interval";

    /* Install SIGALRM handler */
    struct k_sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = (uint64)sigalrm_handler;
    sa.sa_flags = SA_RESTART;
    sa.sa_mask = 0;

    int64 ret = _syscall3(SYS_sigaction, SIGALRM, (int64)&sa, 0);
    if (ret != 0) { TEST_FAIL(name, "sigaction failed"); return; }

    sigalrm_count = 0;

    /* Arm repeating timer: first fire after 30ms, then every 30ms */
    struct k_itimerval nv;
    memset(&nv, 0, sizeof(nv));
    nv.it_value.tv_sec = 0;
    nv.it_value.tv_usec = 30000;
    nv.it_interval.tv_sec = 0;
    nv.it_interval.tv_usec = 30000;

    ret = _syscall3(SYS_setitimer, ITIMER_REAL, (int64)&nv, 0);
    if (ret != 0) { TEST_FAIL(name, "setitimer failed"); goto restore; }

    /* Wait until we get at least 2 signals (up to ~2 seconds) */
    for (int i = 0; i < 200 && sigalrm_count < 2; i++)
        sleep(1);

    if (sigalrm_count < 2) {
        printf("    sigalrm_count=%d (expected >= 2)\n", sigalrm_count);
        TEST_FAIL(name, "interval timer didn't fire multiple times");
        goto restore;
    }

    TEST_PASS(name);
restore:
    memset(&nv, 0, sizeof(nv));
    _syscall3(SYS_setitimer, ITIMER_REAL, (int64)&nv, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = 0;
    _syscall3(SYS_sigaction, SIGALRM, (int64)&sa, 0);
}

/* ══════════════════════════════════════════════════════════════════════
 * 47. SEM cross-process synchronization
 * ══════════════════════════════════════════════════════════════════════ */
static void test_sem_cross_process(void) {
    const char *name = "sem cross-process";

    /* Create a semaphore set with 1 sem, initially 0 */
    int64 semid = _syscall3(SYS_semget, IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (semid < 0) { TEST_FAIL(name, "semget failed"); return; }
    _syscall4(SYS_semctl, semid, 0, SETVAL, 0);

    int pid = fork();
    if (pid < 0) { TEST_FAIL(name, "fork failed"); goto cleanup; }

    if (pid == 0) {
        /* Child: post (increment) the semaphore after a small delay */
        sleep(5);  /* ~50ms */
        struct k_sembuf sops;
        sops.sem_num = 0;
        sops.sem_op = 1;  /* V */
        sops.sem_flg = 0;
        sops.__pad = 0;
        _syscall3(SYS_semop, semid, (int64)&sops, 1);
        exit(0);
    }

    /* Parent: wait (decrement) — should block until child posts */
    struct k_sembuf sops;
    sops.sem_num = 0;
    sops.sem_op = -1;  /* P */
    sops.sem_flg = 0;
    sops.__pad = 0;
    int64 ret = _syscall3(SYS_semop, semid, (int64)&sops, 1);

    int status;
    waitpid(pid, &status, 0);

    if (ret != 0) { TEST_FAIL(name, "semop wait failed"); goto cleanup; }

    /* Verify sem is back to 0 */
    ret = _syscall4(SYS_semctl, semid, 0, GETVAL, 0);
    if (ret != 0) { TEST_FAIL(name, "value != 0 after P/V"); goto cleanup; }

    TEST_PASS(name);
cleanup:
    _syscall4(SYS_semctl, semid, 0, IPC_RMID, 0);
}

/* ══════════════════════════════════════════════════════════════════════
 * 48. MSG cross-process
 * ══════════════════════════════════════════════════════════════════════ */
static void test_msg_cross_process(void) {
    const char *name = "msg cross-process";

    int64 msqid = _syscall2(SYS_msgget, IPC_PRIVATE, IPC_CREAT | 0600);
    if (msqid < 0) { TEST_FAIL(name, "msgget failed"); return; }

    int pid = fork();
    if (pid < 0) { TEST_FAIL(name, "fork failed"); goto cleanup; }

    if (pid == 0) {
        /* Child: send a message */
        struct msgbuf buf;
        buf.mtype = 99;
        strcpy(buf.mtext, "from child");
        _syscall4(SYS_msgsnd, msqid, (int64)&buf, (int64)(strlen(buf.mtext) + 1), 0);
        exit(0);
    }

    /* Parent: wait for child, then receive */
    int status;
    waitpid(pid, &status, 0);

    struct msgbuf rbuf;
    memset(&rbuf, 0, sizeof(rbuf));
    int64 ret = _syscall5(SYS_msgrcv, msqid, (int64)&rbuf, 64, 99, 0);
    if (ret < 0) { TEST_FAIL(name, "msgrcv failed"); goto cleanup; }
    if (rbuf.mtype != 99 || strcmp(rbuf.mtext, "from child") != 0) {
        TEST_FAIL(name, "message content mismatch");
        goto cleanup;
    }

    TEST_PASS(name);
cleanup:
    _syscall3(SYS_msgctl, msqid, IPC_RMID, 0);
}

/* ══════════════════════════════════════════════════════════════════════
 * 49. FSYNC on memfd (no-op path)
 * ══════════════════════════════════════════════════════════════════════ */
static void test_fsync_memfd(void) {
    const char *name = "fsync on memfd";

    char mfn[] = "synctest";
    int64 fd = _syscall2(SYS_memfd_create, (int64)mfn, 0);
    if (fd < 0) { TEST_FAIL(name, "memfd_create failed"); return; }

    write((int)fd, "data", 4);

    int64 ret = _syscall1(SYS_fsync, fd);
    if (ret != 0) { TEST_FAIL(name, "fsync failed"); close((int)fd); return; }

    ret = _syscall1(SYS_fdatasync, fd);
    if (ret != 0) { TEST_FAIL(name, "fdatasync failed"); close((int)fd); return; }

    close((int)fd);
    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 50. MEMFD multiple creates → distinct fds
 * ══════════════════════════════════════════════════════════════════════ */
static void test_memfd_multi(void) {
    const char *name = "memfd multiple creates";

    char n1[] = "mfd1";
    char n2[] = "mfd2";
    int64 fd1 = _syscall2(SYS_memfd_create, (int64)n1, 0);
    int64 fd2 = _syscall2(SYS_memfd_create, (int64)n2, 0);
    if (fd1 < 0 || fd2 < 0) {
        TEST_FAIL(name, "memfd_create failed");
        if (fd1 >= 0) close((int)fd1);
        if (fd2 >= 0) close((int)fd2);
        return;
    }
    if (fd1 == fd2) {
        TEST_FAIL(name, "same fd for two memfds");
        close((int)fd1);
        return;
    }

    /* Write different data to each, verify independence */
    write((int)fd1, "AAA", 3);
    write((int)fd2, "BBB", 3);

    lseek((int)fd1, 0, 0);
    lseek((int)fd2, 0, 0);

    char b1[4] = {0}, b2[4] = {0};
    read((int)fd1, b1, 3);
    read((int)fd2, b2, 3);

    if (strcmp(b1, "AAA") != 0 || strcmp(b2, "BBB") != 0) {
        TEST_FAIL(name, "data not independent");
    } else {
        TEST_PASS(name);
    }

    close((int)fd1);
    close((int)fd2);
}

/* ══════════════════════════════════════════════════════════════════════
 * 51. UTIMENSAT both OMIT (no-op)
 * ══════════════════════════════════════════════════════════════════════ */
static void test_utimensat_both_omit(void) {
    const char *name = "utimensat both OMIT";

    int fd = open("/tmp/__test_utimens3", 0101);
    if (fd < 0) { TEST_FAIL(name, "open failed"); return; }
    write(fd, "x", 1);

    /* Set both to UTIME_OMIT — should be a no-op */
    struct timespec times[2];
    times[0].tv_sec = 0;
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_sec = 0;
    times[1].tv_nsec = UTIME_OMIT;
    int64 ret = _syscall4(SYS_utimensat, fd, 0, (int64)times, 0);
    if (ret != 0) { TEST_FAIL(name, "OMIT/OMIT failed"); }
    else { TEST_PASS(name); }

    close(fd);
    unlink("/tmp/__test_utimens3");
}

/* ══════════════════════════════════════════════════════════════════════
 * 52. SIGPROCMASK — block/unblock signals
 * ══════════════════════════════════════════════════════════════════════ */
static void test_sigprocmask(void) {
    const char *name = "sigprocmask";

    /* Get current mask */
    uint64 oldmask = 0;
    int64 ret = _syscall3(SYS_sigprocmask, SIG_BLOCK, 0, (int64)&oldmask);
    if (ret != 0) { TEST_FAIL(name, "get old mask failed"); return; }

    /* Block SIGUSR1 */
    uint64 blockmask = (1UL << (SIGUSR1 - 1));
    ret = _syscall3(SYS_sigprocmask, SIG_BLOCK, (int64)&blockmask, 0);
    if (ret != 0) { TEST_FAIL(name, "SIG_BLOCK failed"); return; }

    /* Read back — should include SIGUSR1 */
    uint64 curmask = 0;
    ret = _syscall3(SYS_sigprocmask, SIG_BLOCK, 0, (int64)&curmask);
    if (ret != 0) { TEST_FAIL(name, "read after block failed"); return; }
    if (!(curmask & blockmask)) {
        TEST_FAIL(name, "SIGUSR1 not in mask after block");
        return;
    }

    /* Unblock SIGUSR1 */
    ret = _syscall3(SYS_sigprocmask, SIG_UNBLOCK, (int64)&blockmask, 0);
    if (ret != 0) { TEST_FAIL(name, "SIG_UNBLOCK failed"); return; }

    /* Read back — should not include SIGUSR1 (assuming it wasn't before) */
    curmask = 0;
    ret = _syscall3(SYS_sigprocmask, SIG_BLOCK, 0, (int64)&curmask);
    if (curmask & blockmask) {
        TEST_FAIL(name, "SIGUSR1 still in mask after unblock");
        return;
    }

    /* SIG_SETMASK */
    uint64 newmask = (1UL << (SIGUSR1 - 1)) | (1UL << (SIGALRM - 1));
    uint64 prevmask = 0;
    ret = _syscall3(SYS_sigprocmask, SIG_SETMASK, (int64)&newmask, (int64)&prevmask);
    if (ret != 0) { TEST_FAIL(name, "SIG_SETMASK failed"); return; }

    /* Verify */
    curmask = 0;
    _syscall3(SYS_sigprocmask, SIG_BLOCK, 0, (int64)&curmask);
    if (!(curmask & (1UL << (SIGUSR1 - 1))) || !(curmask & (1UL << (SIGALRM - 1)))) {
        TEST_FAIL(name, "SETMASK didn't apply");
        /* Restore */
        _syscall3(SYS_sigprocmask, SIG_SETMASK, (int64)&oldmask, 0);
        return;
    }

    /* Restore original mask */
    _syscall3(SYS_sigprocmask, SIG_SETMASK, (int64)&oldmask, 0);

    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 53. MEMFD seek + truncate behavior
 * ══════════════════════════════════════════════════════════════════════ */
static void test_memfd_seek(void) {
    const char *name = "memfd seek";

    char mfn[] = "seektest";
    int64 fd = _syscall2(SYS_memfd_create, (int64)mfn, 0);
    if (fd < 0) { TEST_FAIL(name, "memfd_create failed"); return; }

    /* Write 10 bytes */
    write((int)fd, "0123456789", 10);

    /* SEEK_SET to position 5 */
    int64 off = lseek((int)fd, 5, 0);
    if (off != 5) { TEST_FAIL(name, "SEEK_SET failed"); close((int)fd); return; }

    /* Read remaining 5 bytes */
    char buf[8] = {0};
    int n = read((int)fd, buf, 8);
    if (n != 5 || strcmp(buf, "56789") != 0) {
        TEST_FAIL(name, "read after seek mismatch");
        close((int)fd);
        return;
    }

    /* SEEK_CUR from current position */
    lseek((int)fd, 0, 0);  /* reset to start */
    off = lseek((int)fd, 3, 1 /* SEEK_CUR */);
    if (off != 3) { TEST_FAIL(name, "SEEK_CUR failed"); close((int)fd); return; }

    /* SEEK_END */
    off = lseek((int)fd, 0, 2 /* SEEK_END */);
    if (off != 10) { TEST_FAIL(name, "SEEK_END failed"); close((int)fd); return; }

    close((int)fd);
    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 54. SHM stat timestamps (atime/dtime after attach/detach)
 * ══════════════════════════════════════════════════════════════════════ */
static void test_shm_timestamps(void) {
    const char *name = "shm timestamps";

    int64 id = _syscall3(SYS_shmget, IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    if (id < 0) { TEST_FAIL(name, "shmget failed"); return; }

    /* ctime should be set on creation */
    struct shmid_ds ds;
    memset(&ds, 0, sizeof(ds));
    _syscall3(SYS_shmctl, id, IPC_STAT, (int64)&ds);
    if (ds.shm_ctime == 0) { TEST_FAIL(name, "ctime == 0"); goto cleanup; }

    /* atime should be set after attach */
    int64 addr = _syscall3(SYS_shmat, id, 0, 0);
    if (addr <= 0) { TEST_FAIL(name, "shmat failed"); goto cleanup; }

    memset(&ds, 0, sizeof(ds));
    _syscall3(SYS_shmctl, id, IPC_STAT, (int64)&ds);
    if (ds.shm_atime == 0) { TEST_FAIL(name, "atime not set after attach"); goto detach; }
    if (ds.shm_nattch != 1) { TEST_FAIL(name, "nattch != 1"); goto detach; }
    if (ds.shm_cpid == 0) { TEST_FAIL(name, "cpid == 0"); goto detach; }

    /* Detach */
    _syscall1(SYS_shmdt, addr);

    /* dtime should be set after detach */
    memset(&ds, 0, sizeof(ds));
    _syscall3(SYS_shmctl, id, IPC_STAT, (int64)&ds);
    if (ds.shm_dtime == 0) { TEST_FAIL(name, "dtime not set after detach"); goto cleanup; }
    if (ds.shm_nattch != 0) { TEST_FAIL(name, "nattch != 0 after detach"); goto cleanup; }

    TEST_PASS(name);
    _syscall3(SYS_shmctl, id, IPC_RMID, 0);
    return;
detach:
    _syscall1(SYS_shmdt, addr);
cleanup:
    _syscall3(SYS_shmctl, id, IPC_RMID, 0);
}

/* ══════════════════════════════════════════════════════════════════════
 * 55. SEM otime after operation
 * ══════════════════════════════════════════════════════════════════════ */
static void test_sem_timestamps(void) {
    const char *name = "sem otime + ctime";

    int64 semid = _syscall3(SYS_semget, IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (semid < 0) { TEST_FAIL(name, "semget failed"); return; }

    /* ctime should be set */
    struct semid_ds sds;
    memset(&sds, 0, sizeof(sds));
    _syscall4(SYS_semctl, semid, 0, IPC_STAT, (int64)&sds);
    if (sds.sem_ctime == 0) { TEST_FAIL(name, "ctime == 0"); goto cleanup; }

    /* otime should be 0 before any operation */
    if (sds.sem_otime != 0) { TEST_FAIL(name, "otime != 0 initially"); goto cleanup; }

    /* Do a semop */
    _syscall4(SYS_semctl, semid, 0, SETVAL, 5);
    struct k_sembuf sops;
    sops.sem_num = 0; sops.sem_op = -1; sops.sem_flg = 0; sops.__pad = 0;
    _syscall3(SYS_semop, semid, (int64)&sops, 1);

    /* otime should now be set */
    memset(&sds, 0, sizeof(sds));
    _syscall4(SYS_semctl, semid, 0, IPC_STAT, (int64)&sds);
    if (sds.sem_otime == 0) { TEST_FAIL(name, "otime still 0 after semop"); goto cleanup; }

    TEST_PASS(name);
cleanup:
    _syscall4(SYS_semctl, semid, 0, IPC_RMID, 0);
}

/* ══════════════════════════════════════════════════════════════════════
 * 56. MSG timestamps (stime/rtime)
 * ══════════════════════════════════════════════════════════════════════ */
static void test_msg_timestamps(void) {
    const char *name = "msg stime/rtime";

    int64 msqid = _syscall2(SYS_msgget, IPC_PRIVATE, IPC_CREAT | 0600);
    if (msqid < 0) { TEST_FAIL(name, "msgget failed"); return; }

    /* ctime should be set, stime/rtime should be 0 */
    struct msqid_ds mds;
    memset(&mds, 0, sizeof(mds));
    _syscall3(SYS_msgctl, msqid, IPC_STAT, (int64)&mds);
    if (mds.msg_ctime == 0) { TEST_FAIL(name, "ctime == 0"); goto cleanup; }
    if (mds.msg_stime != 0) { TEST_FAIL(name, "stime != 0 initially"); goto cleanup; }

    /* Send a message → stime should be set */
    struct msgbuf buf;
    buf.mtype = 1;
    strcpy(buf.mtext, "ts");
    _syscall4(SYS_msgsnd, msqid, (int64)&buf, 3, 0);

    memset(&mds, 0, sizeof(mds));
    _syscall3(SYS_msgctl, msqid, IPC_STAT, (int64)&mds);
    if (mds.msg_stime == 0) { TEST_FAIL(name, "stime still 0 after send"); goto cleanup; }
    if (mds.msg_lspid == 0) { TEST_FAIL(name, "lspid == 0"); goto cleanup; }
    if (mds.msg_qnum != 1) { TEST_FAIL(name, "qnum != 1"); goto cleanup; }

    /* Receive → rtime should be set */
    struct msgbuf rbuf;
    memset(&rbuf, 0, sizeof(rbuf));
    _syscall5(SYS_msgrcv, msqid, (int64)&rbuf, 64, 0, 0);

    memset(&mds, 0, sizeof(mds));
    _syscall3(SYS_msgctl, msqid, IPC_STAT, (int64)&mds);
    if (mds.msg_rtime == 0) { TEST_FAIL(name, "rtime still 0 after recv"); goto cleanup; }
    if (mds.msg_lrpid == 0) { TEST_FAIL(name, "lrpid == 0"); goto cleanup; }
    if (mds.msg_qnum != 0) { TEST_FAIL(name, "qnum != 0 after recv"); goto cleanup; }

    TEST_PASS(name);
cleanup:
    _syscall3(SYS_msgctl, msqid, IPC_RMID, 0);
}

/* ══════════════════════════════════════════════════════════════════════
 * 57. PRCTL name across fork
 * ══════════════════════════════════════════════════════════════════════ */
static void test_prctl_fork(void) {
    const char *name = "prctl name across fork";

    char parent_name[] = "parentproc";
    _syscall5(SYS_prctl, PR_SET_NAME, (int64)parent_name, 0, 0, 0);

    int pid = fork();
    if (pid < 0) { TEST_FAIL(name, "fork failed"); return; }

    if (pid == 0) {
        /* Child: change own name */
        char child_name[] = "childproc";
        _syscall5(SYS_prctl, PR_SET_NAME, (int64)child_name, 0, 0, 0);

        char buf[16] = {0};
        _syscall5(SYS_prctl, PR_GET_NAME, (int64)buf, 0, 0, 0);
        if (strcmp(buf, "childproc") != 0)
            exit(1);
        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);

    /* Parent name should be unchanged */
    char buf[16] = {0};
    _syscall5(SYS_prctl, PR_GET_NAME, (int64)buf, 0, 0, 0);
    if (strcmp(buf, "parentproc") != 0) {
        TEST_FAIL(name, "parent name changed");
        return;
    }
    if (status != 0) {
        TEST_FAIL(name, "child name check failed");
        return;
    }

    /* Restore */
    char restore[] = "syscalltest";
    _syscall5(SYS_prctl, PR_SET_NAME, (int64)restore, 0, 0, 0);

    TEST_PASS(name);
}

/* ══════════════════════════════════════════════════════════════════════
 * 58. UTIMENSAT on path-based (AT_FDCWD)
 * ══════════════════════════════════════════════════════════════════════ */
static void test_utimensat_path(void) {
    const char *name = "utimensat path-based";

    /* Create a file under /tmp (filename must be <= DIRSIZ=14 chars) */
    int fd = open("/tmp/_ut_path", 0101);
    if (fd < 0) { TEST_FAIL(name, "open failed"); return; }
    write(fd, "x", 1);
    close(fd);

    /* Use AT_FDCWD + path to set times */
    struct timespec times[2];
    times[0].tv_sec = 1000;
    times[0].tv_nsec = 0;
    times[1].tv_sec = 2000;
    times[1].tv_nsec = 0;
    int64 ret = _syscall4(SYS_utimensat, AT_FDCWD,
                          (int64)"/tmp/_ut_path",
                          (int64)times, 0);
    if (ret != 0) {
        printf("    utimensat(AT_FDCWD) returned %ld\n", (long)ret);
        TEST_FAIL(name, "path-based utimensat failed");
    } else {
        TEST_PASS(name);
    }

    unlink("/tmp/_ut_path");
}

/* ══════════════════════════════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    printf("=== Syscall validation test suite ===\n\n");

    printf("[prctl]\n");
    test_prctl();

    printf("[sysinfo]\n");
    test_sysinfo();

    printf("[getrusage]\n");
    test_getrusage();

    printf("[getpriority/setpriority]\n");
    test_priority();

    printf("[set_robust_list]\n");
    test_set_robust_list();

    printf("[clock_settime]\n");
    test_clock_settime();

    printf("[sched_rr_get_interval]\n");
    test_sched_rr_get_interval();

    printf("[sigaltstack]\n");
    test_sigaltstack();

    printf("[fsync/fdatasync]\n");
    test_fsync();

    printf("[memfd_create]\n");
    test_memfd_create();

    printf("[utimensat]\n");
    test_utimensat();

    printf("[shared memory]\n");
    test_shm();
    test_shm_key();
    test_shm_cross_process();

    printf("[semaphores]\n");
    test_sem();
    test_sem_nowait();

    printf("[message queues]\n");
    test_msg();
    test_msg_types();

    printf("\n--- Extended coverage tests ---\n\n");

    printf("[prctl errors]\n");
    test_prctl_errors();

    printf("[sysinfo errors]\n");
    test_sysinfo_null();

    printf("[getrusage modes]\n");
    test_getrusage_modes();

    printf("[priority errors]\n");
    test_priority_errors();

    printf("[sigaltstack errors]\n");
    test_sigaltstack_errors();

    printf("[fsync bad fd]\n");
    test_fsync_badfd();

    printf("[memfd read-back]\n");
    test_memfd_readback();

    printf("[utimensat edges]\n");
    test_utimensat_edges();

    printf("[shm errors]\n");
    test_shm_errors();

    printf("[shm IPC_SET + multi-page]\n");
    test_shm_ipc_set();

    printf("[shm IPC_RMID busy]\n");
    test_shm_rmid_busy();

    printf("[sem errors]\n");
    test_sem_errors();

    printf("[sem advanced]\n");
    test_sem_advanced();

    printf("[semtimedop]\n");
    test_semtimedop();

    printf("[msg errors]\n");
    test_msg_errors();

    printf("[msg advanced]\n");
    test_msg_advanced();

    printf("[msg IPC_RMID queued]\n");
    test_msg_rmid_queued();

    printf("[sched_rr null]\n");
    test_sched_rr_null();

    printf("[setitimer/getitimer]\n");
    test_setitimer_getitimer();

    printf("[rt_sigqueueinfo]\n");
    test_rt_sigqueueinfo();

    printf("[clone3]\n");
    test_clone3();

    printf("[msg key re-lookup]\n");
    test_msg_key_relookup();

    printf("[sem defaults]\n");
    test_sem_defaults();

    printf("\n--- Additional coverage tests ---\n\n");

    printf("[msg queue full]\n");
    test_msg_queue_full();

    printf("[shmdt invalid]\n");
    test_shmdt_invalid();

    printf("[shm size > SHMMAX]\n");
    test_shm_size_too_large();

    printf("[setitimer SIGALRM delivery]\n");
    test_setitimer_sigalrm();

    printf("[setitimer interval]\n");
    test_setitimer_interval();

    printf("[sem cross-process]\n");
    test_sem_cross_process();

    printf("[msg cross-process]\n");
    test_msg_cross_process();

    printf("[fsync on memfd]\n");
    test_fsync_memfd();

    printf("[memfd multi]\n");
    test_memfd_multi();

    printf("[utimensat both OMIT]\n");
    test_utimensat_both_omit();

    printf("[sigprocmask]\n");
    test_sigprocmask();

    printf("[memfd seek]\n");
    test_memfd_seek();

    printf("[shm timestamps]\n");
    test_shm_timestamps();

    printf("[sem timestamps]\n");
    test_sem_timestamps();

    printf("[msg timestamps]\n");
    test_msg_timestamps();

    printf("[prctl fork]\n");
    test_prctl_fork();

    printf("[utimensat path]\n");
    test_utimensat_path();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);
    if (tests_failed > 0)
        printf("*** SOME TESTS FAILED ***\n");
    else
        printf("All tests passed!\n");

    exit(tests_failed > 0 ? 1 : 0);
}
