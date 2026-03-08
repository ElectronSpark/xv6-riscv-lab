#include "types.h"
#include "string.h"
#include "param.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "lock/spinlock.h"
#include "defs.h"
#include "proc/thread.h"
#include "arch_thread.h"
#include "printf.h"
#include "proc/sched.h"
#include "signal.h"
#include "syscall.h"
#include <mm/page.h>
#include <mm/vm.h>
#include "trapframe.h"
#include "trap.h"

#define USER_FAULT_AROUND_PAGES 16UL
#define USER_FAULT_AROUND_SIZE  (USER_FAULT_AROUND_PAGES * PGSIZE)

extern char trampoline[], uservec[], userret[], _data_ktlb[];
extern uint64 trampoline_uservec;

static void (*trampoline_userret)(uint64, uint64) = NULL;

// in kernelvec.S
// Recursive kernel trap handler, already on interrupt stack,
// skip getting sscratch.
void kernelvec();

void arch_trap_init(void) {
    trampoline_userret =
        (void *)(TRAMPOLINE + ((uint64)userret - (uint64)trampoline));
    printf("trapinit: trampoline_userret at %p\n", trampoline_userret);
    // send syscalls, interrupts, and exceptions to uservec in trampoline.S
    trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
    printf("trapinit: trampoline_uservec at %p\n", (void *)trampoline_uservec);

    // Allocate and map interrupt stacks for each CPU hart
    pagetable_t kpgtbl = (void *)_data_ktlb;
    for (int i = 0; i < NCPU; i++) {
        void *intr_stacks = page_alloc(INTR_STACK_ORDER, 0);
        assert(intr_stacks != NULL,
               "trapinit: page_alloc for intr_stacks failed");
        memset(intr_stacks, 0, INTR_STACK_SIZE);
        kvmmap(kpgtbl, KIRQSTACK(i), (uint64)intr_stacks, INTR_STACK_SIZE,
               PTE_R | PTE_W);
        cpus[i].intr_stacks = (void *)KIRQSTACK(i);
        cpus[i].intr_sp = (uint64)cpus[i].intr_stacks + INTR_STACK_SIZE;
        printf("trapinit: CPU %d intr_stack at %lx -> %p\n", i, KIRQSTACK(i),
               intr_stacks);
    }
}

// set up to take exceptions and traps while in the kernel.
void arch_trap_init_hart(void) {
    w_sscratch(mycpu()->intr_sp);
    w_stvec((uint64)kernelvec);
}

void kerneltrap_dump_regs(struct trapframe *sp) {
    printf("kerneltrap_dump_regs:\n");
    printf("pc: 0x%lx\n", sp->sepc);
    printf("ra: 0x%lx, sp: 0x%lx, s0: 0x%lx\n", sp->ra, sp->sp, sp->s0);
    printf("tp: 0x%lx, t0: 0x%lx, t1: 0x%lx, t2: 0x%lx\n", r_tp(), sp->t0,
           sp->t1, sp->t2);
    printf("a0: 0x%lx, a1: 0x%lx, a2: 0x%lx, a3: 0x%lx\n", sp->a0, sp->a1,
           sp->a2, sp->a3);
    printf("a4: 0x%lx, a5: 0x%lx, a6: 0x%lx, a7: 0x%lx\n", sp->a4, sp->a5,
           sp->a6, sp->a7);
    printf("t3: 0x%lx, t4: 0x%lx, t5: 0x%lx, t6: 0x%lx\n", sp->t3, sp->t4,
           sp->t5, sp->t6);
    printf("gp: 0x%lx\n", r_gp());
}

void __user_kirq_return(uint64 irq_sp, uint64 s0) { usertrapret(); }

static void __trap_panic(struct trapframe *tf, uint64 s0) {
    // interrupt or trap from an unknown source
    // printf("0x%lx 0x%lx\n", sp, s0);
    printf("scause=0x%lx(%s) sepc=0x%lx stval=0x%lx\n", tf->scause,
           __scause_to_str(tf->scause), tf->sepc, tf->stval);
    tf->ra = tf->sepc;
    // to enconvinient gdb back trace
    *(uint64 *)((uint64)tf - 8) = tf->sepc;
    struct thread *p = current;
    if (p == NULL) {
        printf("kerneltrap: no current thread\n");
        kerneltrap_dump_regs(tf);
        panic_disable_bt();
        panic("kerneltrap");
    }

    size_t kstack_size = (1UL << (PAGE_SHIFT + p->kstack_order));
    /* Use the kernel s0 (passed from assembly stub), not tf->s0 which
     * may be the user's frame pointer if we entered from user mode. */
    print_backtrace(s0, p->kstack, p->kstack + kstack_size);
    kerneltrap_dump_regs(tf);
    panic_disable_bt();
    panic("kerneltrap");
}

void user_kirq_entrance(uint64 ksp, uint64 s0) {
    enter_irq();
    if ((current->trapframe->trapframe.sstatus & SSTATUS_SPP) != 0)
        panic("usertrap: not from user mode");

    // Mark the current CPU as offline for this process's VM
    vm_cpu_offline(current->vm, cpuid());
    // Mark the current CPU as online for the kernel VM (reverse of user VM)
    vm_cpu_online(kernel_vm, cpuid(), current);

    // redirect traps to kerneltrap()
    // Since we are on kernel stack
    arch_trap_init_hart();
    if (do_irq(current->trapframe->trapframe.scause & ~(1UL << 63)) < 0) {
        __trap_panic(&current->trapframe->trapframe, s0);
    }
    exit_irq();

    if (NEEDS_RESCHED()) {
        // If anyone has requested a reschedule, do it now.
        // switch to kernel stack first (so yield() runs on the right stack)
        __switch_noreturn(current->ksp, s0, __user_kirq_return);
    }
    // Otherwise return to user space.
    usertrapret();
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void usertrap(void) {
    uint64 va;
    vma_t *vma = NULL;
    uint64 scause = current->trapframe->trapframe.scause;
    extern int gdbstub_signal_stop(struct thread *, int);

    if ((current->trapframe->trapframe.sstatus & SSTATUS_SPP) != 0)
        panic("usertrap: not from user mode");

    // Mark the current CPU as offline for this process's VM
    vm_cpu_offline(current->vm, cpuid());
    // Mark the current CPU as online for the kernel VM (reverse of user VM)
    vm_cpu_online(kernel_vm, cpuid(), current);

    // redirect traps to kerneltrap()
    // Since we are on kernel stack
    arch_trap_init_hart();

    switch (scause) {
    case RISCV_BREAKPOINT_TRAP: {
        // Software breakpoint (EBREAK instruction).
        // Hand off to the GDB stub if it is attached to this process.
        extern int gdbstub_trap(struct thread *);
        if (gdbstub_trap(current) != 0) {
            // GDB stub didn't handle it — treat as fatal.
            printf("pid %d %s: breakpoint trap sepc=0x%lx\n",
                   current->pid, current->name,
                   current->trapframe->trapframe.sepc);
            kill(current->pid, SIGTRAP);
        }
        break;
    }
    case RISCV_ILLEGAL_INSTRUCTION: {
        /*
         * Lazy FPU switching: if sstatus.FS == Off, the user tried to
         * execute an FP instruction.  Enable FP for this thread and
         * retry the instruction.  If FS was not Off, it's a genuinely
         * illegal instruction — kill the process.
         */
        uint64 saved_sstatus = current->trapframe->trapframe.sstatus;
        if ((saved_sstatus & SSTATUS_FS_MASK) == SSTATUS_FS_OFF) {
            /* Enable kernel FP access so we can save/restore FP regs */
            unsigned long s = r_sstatus();
            s &= ~SSTATUS_FS_MASK;
            s |= SSTATUS_FS_DIRTY;
            w_sstatus(s);

            struct cpu_local *c = mycpu();

            /* First time this thread uses FP — allocate fpu_state */
            if (!THREAD_FPU_USED(current)) {
                if (current->fpu_state == NULL) {
                    current->fpu_state = kvmalloc(sizeof(struct fpu_state));
                    if (current->fpu_state == NULL) {
                        printf("pid %d: failed to allocate fpu_state\n",
                               current->pid);
                        kill(current->pid, SIGKILL);
                        break;
                    }
                    memset(current->fpu_state, 0, sizeof(struct fpu_state));
                }
                fpu_init_state();
                THREAD_SET_FPU_USED(current);
            } else if (c->fpu_owner_tid != current->pid ||
                       current->fpu_seq != c->fpu_seq) {
                /*
                 * Different owner or mismatched seq — restore our
                 * saved FP state and take ownership.  Bump seq so
                 * stale per-CPU entries from other CPUs won't match.
                 */
                fpu_restore_state(current->fpu_state);
                current->fpu_seq++;
            }
            c->fpu_owner_tid = current->pid;
            c->fpu_seq = current->fpu_seq;
            break;
        }
        /* Genuine illegal instruction */
        printf("pid %d %s: illegal instruction sepc=0x%lx stval=0x%lx\n",
               current->pid, current->name,
               current->trapframe->trapframe.sepc,
               current->trapframe->trapframe.stval);
        assert(current->pid != 1, "init exiting");
        extern int gdbstub_signal_stop(struct thread *, int);
        gdbstub_signal_stop(current, SIGILL);
        kill(current->pid, SIGILL);
        break;
    }
    case RISCV_ENV_CALL_FROM_U_MODE:
        // system call

        if (killed(current))
            exit(-1);

        // sepc points to the ecall instruction,
        // but we want to return to the next instruction.
        current->trapframe->trapframe.sepc += 4;

        // an interrupt will change sepc, scause, and sstatus,
        // so enable only now that we're done with those registers.
        intr_on();

        syscall();
        break;
    case RISCV_INSTRUCTION_PAGE_FAULT:
        va = current->trapframe->trapframe.stval;
        vm_rlock(current->vm);
        vma = vm_find_area(current->vm, va);
        uint64 fault_len = USER_FAULT_AROUND_SIZE;
        uint64 fault_base = PGROUNDDOWN(va);
        if (vma != NULL) {
            if (fault_base >= vma->end)
                fault_len = PGSIZE;
            else if (fault_base + fault_len > vma->end)
                fault_len = vma->end - fault_base;
        }
        if (vma != NULL &&
            vma_validate(vma, fault_base, fault_len,
                         VMA_FLAG_USER | PROT_EXEC | PROT_READ) ==
                0) {
            vm_runlock(current->vm);
            // fence.i will be invoked in trampoline return
            break;
        }
        vm_runlock(current->vm);
        printf("pid %d %s: fatal instruction page fault sepc=0x%lx stval=0x%lx\n",
               current->pid, current->name,
               current->trapframe->trapframe.sepc,
               current->trapframe->trapframe.stval);
        printf("  ra=0x%lx sp=0x%lx tp=0x%lx gp=0x%lx\n",
               current->trapframe->trapframe.ra,
               current->trapframe->trapframe.sp,
               current->trapframe->tp,
               current->trapframe->gp);
        printf("  a0=0x%lx a1=0x%lx a2=0x%lx s0=0x%lx s1=0x%lx\n",
               current->trapframe->trapframe.a0,
               current->trapframe->trapframe.a1,
               current->trapframe->trapframe.a2,
               current->trapframe->trapframe.s0,
               current->trapframe->s1);
        assert(current->pid != 1, "init exiting");
        gdbstub_signal_stop(current, SIGSEGV);
        kill(current->pid, SIGSEGV);
        break;
    case RISCV_LOAD_PAGE_FAULT:
        // Load page fault - handle demand paging for read access
        va = current->trapframe->trapframe.stval;
        // First try to grow stack if the address is in stack region
        vm_try_growstack(current->vm, va);
        // Now find the VMA (may be stack that just grew, or existing VMA
        // needing demand paging). Hold vm_rlock to protect VMA tree traversal.
        vm_rlock(current->vm);
        vma = vm_find_area(current->vm, va);
        fault_len = USER_FAULT_AROUND_SIZE;
        fault_base = PGROUNDDOWN(va);
        if (vma != NULL) {
            if (fault_base >= vma->end)
                fault_len = PGSIZE;
            else if (fault_base + fault_len > vma->end)
                fault_len = vma->end - fault_base;
        }
        if (vma != NULL &&
            vma_validate(vma, fault_base, fault_len,
                         VMA_FLAG_USER | PROT_READ) == 0) {
            vm_runlock(current->vm);
            break;
        }
        vm_runlock(current->vm);
        printf("pid %d %s: fatal load page fault sepc=0x%lx stval=0x%lx\n",
               current->pid, current->name,
               current->trapframe->trapframe.sepc,
               current->trapframe->trapframe.stval);
        assert(current->pid != 1, "init exiting");
        gdbstub_signal_stop(current, SIGSEGV);
        kill(current->pid, SIGSEGV);
        break;
    case RISCV_STORE_PAGE_FAULT:
        // Store page fault - handle demand paging for write access
        va = current->trapframe->trapframe.stval;
        // First try to grow stack if the address is in stack region
        vm_try_growstack(current->vm, va);
        // Now find the VMA (may be stack that just grew, or existing VMA
        // needing demand paging). Hold vm_rlock to protect VMA tree traversal.
        vm_rlock(current->vm);
        vma = vm_find_area(current->vm, va);
        if (vma != NULL &&
            vma_validate(vma, va, 1, VMA_FLAG_USER | PROT_WRITE) == 0) {
            vm_runlock(current->vm);
            break;
        }
        vm_runlock(current->vm);
        printf("pid %d %s: fatal store page fault sepc=0x%lx stval=0x%lx\n",
               current->pid, current->name,
               current->trapframe->trapframe.sepc,
               current->trapframe->trapframe.stval);
        assert(current->pid != 1, "init exiting");
        gdbstub_signal_stop(current, SIGSEGV);
        kill(current->pid, SIGSEGV);
        break;
    default:
        assert(current->trapframe->trapframe.scause >> 63 == 0,
               "unexpected interrupt");
        printf("pid %d %s: unexpected scause=0x%lx sepc=0x%lx stval=0x%lx\n",
               current->pid, current->name, scause,
               current->trapframe->trapframe.sepc,
               current->trapframe->trapframe.stval);
        assert(current->pid != 1, "init exiting");
        gdbstub_signal_stop(current, SIGSEGV);
        kill(current->pid, SIGSEGV);
        break;
    }

    usertrapret();
}

extern void sig_trampoline(uint64 arg0, uint64 arg1, uint64 arg2,
                           void *handler);

// Will only modify the user space memory and p->signal.sig_ucontext
// Further modifications to the thread struct need to be done if it succeeds.
int push_sigframe(struct thread *p, int signo, sigaction_t *sa,
                  ksiginfo_t *info) {
    if (sa == NULL || sa->sa_handler == NULL || p == NULL) {
        return -1; // Invalid arguments
    }

    uint64 new_sp = 0;
    if ((sa->sa_flags & SA_ONSTACK) != 0 &&
        (p->signal.sig_stack.ss_flags & (SS_ONSTACK | SS_DISABLE)) == 0) {
        // Use the alternate stack if SA_ONSTACK is set.

        if (p->signal.sig_stack.ss_size < MINSIGSTKSZ) {
            return -1; // Stack too small
        }
        new_sp =
            (uint64)p->signal.sig_stack.ss_sp + p->signal.sig_stack.ss_size;
    } else {
        new_sp = p->trapframe->trapframe.sp;
    }

    new_sp -= 0x10UL;
    new_sp &= ~0xFUL; // align to 16 bytes
    uint64 new_ucontext = new_sp - sizeof(ucontext_t);
    new_ucontext &= ~0xFUL;
    uint64 user_siginfo = 0;
    if (sa->sa_flags & SA_SIGINFO) {
        assert(info != NULL,
               "push_sigframe: info is NULL when SA_SIGINFO is set");
        user_siginfo = new_ucontext - sizeof(siginfo_t);
        user_siginfo &= ~0xFUL;
        new_sp = user_siginfo;
    } else {
        new_sp = new_ucontext;
    }

    if ((sa->sa_flags & SA_ONSTACK) == 0) {
        if (p == NULL || p->vm == NULL) {
            exit(-1); // No stack area available
        }
        if (vm_try_growstack(p->vm, new_sp) != 0) {
            exit(-1); // No stack area available
        }
    }

    ucontext_t uc = {0};
    uc.uc_link = (ucontext_t *)p->signal.sig_ucontext;
    uc.uc_sigmask =
        p->signal.sig_mask; // Save current mask to restore after handler
    memmove(&uc.uc_mcontext, p->trapframe, sizeof(mcontext_t));
    memmove(&uc.uc_stack, &p->signal.sig_stack, sizeof(stack_t));

    // Save FP state into the signal frame if this thread has used FP.
    if (THREAD_FPU_USED(p) && p->fpu_state != NULL) {
        if (mycpu()->fpu_owner_tid == p->pid &&
            p->fpu_seq == mycpu()->fpu_seq) {
            // FP regs are live in hardware — save them first.
            unsigned long s = r_sstatus();
            s &= ~SSTATUS_FS_MASK;
            s |= SSTATUS_FS_DIRTY;
            w_sstatus(s);
            fpu_save_state(p->fpu_state);
        }
        memmove(&uc.uc_fpstate, p->fpu_state, sizeof(struct fpu_state));
        uc.uc_fpflags = 1; // FP state is valid
    }

    // Copy the trap frame to the signal trap frame.
    if (vm_copyout(p->vm, new_ucontext, (void *)&uc, sizeof(ucontext_t)) != 0) {
        return -1; // Copy failed
    }
    if (sa->sa_flags & SA_SIGINFO) {
        if (vm_copyout(p->vm, user_siginfo, &info->info, sizeof(siginfo_t)) !=
            0) {
            return -1; // Copy failed
        }
    }

    p->trapframe->trapframe.sp = new_sp;
    p->trapframe->trapframe.sepc =
        (uint64)SIG_TRAMPOLINE;         // Set the epc to the signal trampoline
    arch_tf_set_arg0(p->trapframe, signo);          // signal number
    arch_tf_set_arg1(p->trapframe, user_siginfo);   // siginfo_t *
    arch_tf_set_arg2(p->trapframe, new_ucontext);   // ucontext_t *
    p->trapframe->trapframe.t0 =
        (uint64)sa->sa_handler; // Set the handler address
    p->signal.sig_ucontext = new_ucontext;

    return 0; // Success
}

int restore_sigframe(struct thread *p, ucontext_t *ret_uc) {
    uint64 sig_ucontext = p->signal.sig_ucontext;

    if (sig_ucontext == 0 || ret_uc == NULL) {
        return -1; // No signal trap frame to restore
    }

    // Copy the signal trap frame back to the user trap frame.
    if (vm_copyin(p->vm, (void *)ret_uc, sig_ucontext, sizeof(ucontext_t)) !=
        0) {
        return -1; // Copy failed
    }

    p->signal.sig_ucontext = (uint64)ret_uc->uc_link;
    memmove(p->trapframe, &ret_uc->uc_mcontext, sizeof(mcontext_t));

    // Restore FP state from the signal frame if it was saved.
    if (ret_uc->uc_fpflags && p->fpu_state != NULL) {
        memmove(p->fpu_state, &ret_uc->uc_fpstate, sizeof(struct fpu_state));
        // Invalidate fpu_owner so the next FP use triggers a lazy reload.
        if (mycpu()->fpu_owner_tid == p->pid)
            mycpu()->fpu_owner_tid = 0;
    }

    return 0; // Success
}

//
// return to user space
//
void usertrapret(void) {
    struct thread *p = current;

    if (killed(p)) {
        // If the thread is terminated, exit it.
        exit(-1);
    }

    handle_signal();

    /* handle_signal() may have marked us killed (e.g. unhandled SIGSEGV). */
    if (killed(p))
        exit(-1);

    /* Check if GDB wants to interrupt this process (Ctrl-C). */
    {
        extern int gdbstub_check_interrupt(struct thread *);
        gdbstub_check_interrupt(p);
    }

    if (NEEDS_RESCHED()) {
        scheduler_yield();
    }

    // we're about to switch the destination of traps from
    // kerneltrap() to usertrap(), so turn off interrupts until
    // we're back in user space, where usertrap() is correct.
    intr_off();
    assert(mycpu()->spin_depth == 0, "usertrapret: spin_depth not zero");

    // set up trapframe values that uservec will need when
    // the thread next traps into the kernel.
    p->trapframe->kernel_sp = p->ksp;
    p->trapframe->irq_sp = mycpu()->intr_sp;

    // set up the registers that trampoline.S's sret will use
    // to get to user space.

    // set S Previous Privilege mode to User.
    unsigned long x = r_sstatus();
    x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
    x |= SSTATUS_SPIE; // enable interrupts in user mode

    // Lazy FPU: set FS field for user mode.
    // If this thread owns the FPU on this CPU (matching TID and seq),
    // allow FP access (Clean).  Otherwise set FS=Off so first FP use
    // triggers a trap.
    x &= ~SSTATUS_FS_MASK;
    if (THREAD_FPU_USED(p) &&
        mycpu()->fpu_owner_tid == p->pid &&
        p->fpu_seq == mycpu()->fpu_seq)
        x |= SSTATUS_FS_CLEAN;
    else
        x |= SSTATUS_FS_OFF;

    w_sstatus(x);

    // printf("user pagetable before usertrapret:\n");
    // dump_pagetable(p->vm.pagetable, 2, 0, 0, 0, false);
    // printf("\n");

    // Mark the current CPU as offline for the kernel VM (reverse of user VM)
    vm_cpu_offline(kernel_vm, cpuid());

    // Before returning, mark the current CPU as online for this process's VM
    // and get the trapframe base virtual address for this CPU
    uint64 trapframe_base = vm_cpu_online(p->vm, cpuid(), p);

    // jump to userret in trampoline.S at the top of memory, which
    // switches to the user page table, restores user registers,
    // and switches to user mode with sret.
    trampoline_userret(trapframe_base, MAKE_SATP(p->vm->pagetable));
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void kerneltrap(struct trapframe *sp, uint64 s0) {
    if (CPU_IN_ITR()) {
        printf("kerneltrap: exception preempted interrupt. level=%d",
               mycpu()->intr_depth);
        __trap_panic(sp, s0);
    }
    if (mycpu()->intr_depth++) {
        printf("kerneltrap: nested interrupts not supported. level=%d",
               mycpu()->intr_depth);
        __trap_panic(sp, s0);
    }
    if (!(sp->sstatus & SSTATUS_SPP)) {
        printf("kerneltrap: not from supervisor mode");
        __trap_panic(sp, s0);
    }
    if (intr_get()) {
        printf("kerneltrap: interrupts enabled");
        __trap_panic(sp, s0);
    }

    // By now there's no valid exception from kernel mode.
    printf("kerneltrap: unexpected scause 0x%lx\n", sp->scause);
    __trap_panic(sp, s0);

    mycpu()->intr_depth--;
}

void kernel_irq(struct trapframe *sp, uint64 s0) {
    enter_irq();
    assert(sp->sstatus & SSTATUS_SPP, "kerneltrap: not from supervisor mode");
    if (do_irq(sp->scause & ~(1UL << 63)) < 0) {
        __trap_panic(sp, s0);
    }
    exit_irq();
}

void enter_irq(void) {
    assert(!CPU_IN_ITR(),
           "enter_irq: nested interrupts not supported. level=%d",
           mycpu()->intr_depth);
    mycpu()->intr_depth++;
    if (mycpu()->intr_depth == 1) {
        CPU_SET_IN_ITR();
    }
    assert(!intr_get(), "kerneltrap: interrupts enabled");
}

void exit_irq(void) {
    mycpu()->intr_depth--;
    if (mycpu()->intr_depth == 0) {
        CPU_CLEAR_IN_ITR();
    }
}
