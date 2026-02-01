#include "types.h"
#include "string.h"
#include "param.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "lock/spinlock.h"
#include "defs.h"
#include "proc/proc.h"
#include "printf.h"
#include "proc/sched.h"
#include "signal.h"
#include <mm/page.h>
#include <mm/vm.h>
#include "trap.h"

extern char trampoline[], uservec[], userret[], _data_ktlb[];
extern uint64 trampoline_uservec;

static void (*trampoline_userret)(uint64) = NULL;

// in kernelvec.S
// Recursive kernel trap handler, already on interrupt stack,
// skip getting sscratch.
void kernelvec();

void trapinit(void) {
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
void trapinithart(void) {
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
    struct proc *p = myproc();
    if (p == NULL) {
        printf("kerneltrap: no current process\n");
        kerneltrap_dump_regs(tf);
        panic_disable_bt();
        panic("kerneltrap");
    }

    size_t kstack_size = (1UL << (PAGE_SHIFT + p->kstack_order));
    print_backtrace(tf->s0, p->kstack, p->kstack + kstack_size);
    kerneltrap_dump_regs(tf);
    panic_disable_bt();
    panic("kerneltrap");
}

void user_kirq_entrance(uint64 ksp, uint64 s0) {
    enter_irq();
    if ((myproc()->trapframe->trapframe.sstatus & SSTATUS_SPP) != 0)
        panic("usertrap: not from user mode");

    // Mark the current CPU as offline for this process's VM
    vm_cpu_offline(myproc()->vm, cpuid());

    // redirect traps to kerneltrap()
    // Since we are on kernel stack
    trapinithart();
    if (do_irq(&myproc()->trapframe->trapframe) < 0) {
        __trap_panic(&myproc()->trapframe->trapframe, s0);
    }
    exit_irq();
    
    if (NEEDS_RESCHED()) {
        // If anyone has requested a reschedule, do it now.
        // switch to kernel stack first (so yield() runs on the right stack)
        __switch_noreturn(myproc()->ksp, s0, __user_kirq_return);
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
    uint64 scause = myproc()->trapframe->trapframe.scause;

    if ((myproc()->trapframe->trapframe.sstatus & SSTATUS_SPP) != 0)
        panic("usertrap: not from user mode");

    // Mark the current CPU as offline for this process's VM
    vm_cpu_offline(myproc()->vm, cpuid());

    // redirect traps to kerneltrap()
    // Since we are on kernel stack
    trapinithart();

    switch (scause) {
    case RISCV_ENV_CALL_FROM_U_MODE:
        // system call

        if (killed(myproc()))
            exit(-1);

        // sepc points to the ecall instruction,
        // but we want to return to the next instruction.
        myproc()->trapframe->trapframe.sepc += 4;

        // an interrupt will change sepc, scause, and sstatus,
        // so enable only now that we're done with those registers.
        intr_on();

        syscall();
        break;
    case RISCV_INSTRUCTION_PAGE_FAULT:
        va = myproc()->trapframe->trapframe.stval;
        vm_rlock(myproc()->vm);
        vma = vm_find_area(myproc()->vm, va);
        if (vma != NULL &&
            vma_validate(vma, va, 1, VM_FLAG_USERMAP | VM_FLAG_EXEC) == 0) {
            vm_runlock(myproc()->vm);
            break;
        }
        vm_runlock(myproc()->vm);
        assert(myproc()->pid != 1, "init exiting");
        kill(myproc()->pid, SIGSEGV);
        break;
    case RISCV_LOAD_PAGE_FAULT:
        // Load page fault - handle demand paging for read access
        va = myproc()->trapframe->trapframe.stval;
        // First try to grow stack if the address is in stack region
        vm_try_growstack(myproc()->vm, va);
        // Now find the VMA (may be stack that just grew, or existing VMA
        // needing demand paging). Hold vm_rlock to protect VMA tree traversal.
        vm_rlock(myproc()->vm);
        vma = vm_find_area(myproc()->vm, va);
        if (vma != NULL &&
            vma_validate(vma, va, 1, VM_FLAG_USERMAP | VM_FLAG_READ) == 0) {
            vm_runlock(myproc()->vm);
            break;
        }
        vm_runlock(myproc()->vm);
        // printf("usertrap(): page fault on read 0x%lx pid=%d\n", r_scause(),
        // p->pid); printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(),
        // r_stval()); printf("            pgtbl=0x%lx\n",
        // (uint64)myproc()->vm->pagetable);
        assert(myproc()->pid != 1, "init exiting");
        kill(myproc()->pid, SIGSEGV);
        break;
    case RISCV_STORE_PAGE_FAULT:
        // Store page fault - handle demand paging for write access
        va = myproc()->trapframe->trapframe.stval;
        // First try to grow stack if the address is in stack region
        vm_try_growstack(myproc()->vm, va);
        // Now find the VMA (may be stack that just grew, or existing VMA
        // needing demand paging). Hold vm_rlock to protect VMA tree traversal.
        vm_rlock(myproc()->vm);
        vma = vm_find_area(myproc()->vm, va);
        if (vma != NULL &&
            vma_validate(vma, va, 1, VM_FLAG_USERMAP | VM_FLAG_WRITE) == 0) {
            vm_runlock(myproc()->vm);
            break;
        }
        vm_runlock(myproc()->vm);
        // printf("usertrap(): page fault on write 0x%lx pid=%d\n", r_scause(),
        // p->pid); printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(),
        // r_stval()); printf("            pgtbl=0x%lx\n",
        // (uint64)myproc()->vm->pagetable);
        assert(myproc()->pid != 1, "init exiting");
        kill(myproc()->pid, SIGSEGV);
        break;
    default:
        assert(myproc()->trapframe->trapframe.scause >> 63 == 0,
               "unexpected interrupt");
        // printf("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(),
        // myproc()->pid); printf("            sepc=0x%lx stval=0x%lx\n",
        // r_sepc(), r_stval());
        assert(myproc()->pid != 1, "init exiting");
        kill(myproc()->pid, SIGSEGV);
        break;
    }

    usertrapret();
}

extern void sig_trampoline(uint64 arg0, uint64 arg1, uint64 arg2,
                           void *handler);

// Will only modify the user space memory and p->sig_ucontext
// Further modifications to the process struct need to be done if it succeeds.
int push_sigframe(struct proc *p, int signo, sigaction_t *sa,
                  ksiginfo_t *info) {
    if (sa == NULL || sa->sa_handler == NULL || p == NULL) {
        return -1; // Invalid arguments
    }

    uint64 new_sp = 0;
    if ((sa->sa_flags & SA_ONSTACK) != 0 &&
        (p->sig_stack.ss_flags & (SS_ONSTACK | SS_DISABLE)) == 0) {
        // Use the alternate stack if SA_ONSTACK is set.

        if (p->sig_stack.ss_size < MINSIGSTKSZ) {
            return -1; // Stack too small
        }
        new_sp = (uint64)p->sig_stack.ss_sp + p->sig_stack.ss_size;
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
    uc.uc_link = (ucontext_t *)p->sig_ucontext;
    uc.uc_sigmask =
        p->sigacts->sa_sigmask; // Save current mask to restore after handler
    memmove(&uc.uc_mcontext, p->trapframe, sizeof(mcontext_t));
    memmove(&uc.uc_stack, &p->sig_stack, sizeof(stack_t));

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
    p->trapframe->trapframe.a0 = signo; // Set the first argument
    p->trapframe->trapframe.a1 = user_siginfo; // Set the second argument
    p->trapframe->trapframe.a2 = new_ucontext; // Set the third argument
    p->trapframe->trapframe.t0 =
        (uint64)sa->sa_handler; // Set the handler address
    p->sig_ucontext = new_ucontext;

    return 0; // Success
}

int restore_sigframe(struct proc *p, ucontext_t *ret_uc) {
    uint64 sig_ucontext = p->sig_ucontext;

    if (sig_ucontext == 0 || ret_uc == NULL) {
        return -1; // No signal trap frame to restore
    }

    // Copy the signal trap frame back to the user trap frame.
    if (vm_copyin(p->vm, (void *)ret_uc, sig_ucontext, sizeof(ucontext_t)) !=
        0) {
        return -1; // Copy failed
    }

    p->sig_ucontext = (uint64)ret_uc->uc_link;
    memmove(p->trapframe, &ret_uc->uc_mcontext, sizeof(mcontext_t));

    return 0; // Success
}

//
// return to user space
//
void usertrapret(void) {
    struct proc *p = myproc();

    if (killed(p)) {
        // If the process is terminated, exit it.
        exit(-1);
    }

    handle_signal();

    if (NEEDS_RESCHED()) {
        yield();
    }

    // we're about to switch the destination of traps from
    // kerneltrap() to usertrap(), so turn off interrupts until
    // we're back in user space, where usertrap() is correct.
    intr_off();
    assert(mycpu()->spin_depth == 0, "usertrapret: spin_depth not zero");

    // set up trapframe values that uservec will need when
    // the process next traps into the kernel.
    p->trapframe->kernel_sp = p->ksp;
    p->trapframe->irq_sp = mycpu()->intr_sp;

    // set up the registers that trampoline.S's sret will use
    // to get to user space.

    // set S Previous Privilege mode to User.
    unsigned long x = r_sstatus();
    x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
    x |= SSTATUS_SPIE; // enable interrupts in user mode
    w_sstatus(x);

    // printf("user pagetable before usertrapret:\n");
    // dump_pagetable(p->vm.pagetable, 2, 0, 0, 0, false);
    // printf("\n");

    // Before returning, mark the current CPU as online for this process's VM
    vm_cpu_online(p->vm, cpuid());

    // jump to userret in trampoline.S at the top of memory, which
    // switches to the user page table, restores user registers,
    // and switches to user mode with sret.
    trampoline_userret(MAKE_SATP(p->vm->pagetable));
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
    if (do_irq(sp) < 0) {
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
