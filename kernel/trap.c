#include "types.h"
#include "string.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "printf.h"
#include "sched.h"
#include "signal.h"
#include "vm.h"

uint64 ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

static const char *__scause_to_str(uint64 scause)
{
  if (scause & 0x8000000000000000) {
    // Interrupts are negative, exceptions are positive.
    switch (scause & 0x7FFFFFFFFFFFFFFF) {
      case 0: return "User software interrupt";
      case 1: return "Supervisor software interrupt";
      case 4: return "User timer interrupt";
      case 5: return "Supervisor timer interrupt";
      case 8: return "User external interrupt";
      case 9: return "Supervisor external interrupt";
      default: return "Unknown interrupt";
    }
  }
  switch (scause) {
    case 0: return "Instruction address misaligned";
    case 1: return "Instruction access fault";
    case 2: return "Illegal instruction";
    case 3: return "Breakpoint";
    case 5: return "Load access fault";
    case 6: return "Store/AMO address misaligned";
    case 7: return "Store/AMO access fault";
    case 8: return "Environment call from U-mode";
    case 9: return "Environment call from S-mode";
    case 12: return "Instruction page fault";
    case 13: return "Load page fault";
    case 15: return "Store/AMO page fault";
    default: return "Unknown exception";
  }
}

void
trapinit(void)
{
  ;
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;
  uint64 fault_no, va;
  vma_t *vma = NULL;
  // printf("usertrap: scause=0x%lx (%s), sepc=0x%lx, stval=0x%lx\n",
  //        r_scause(), __scause_to_str(r_scause()), r_sepc(), r_stval());

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  fault_no = r_scause();
  switch (fault_no)
  {
  case 8:
    // system call

    if(killed(p))
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    intr_on();

    syscall();
    break;
  case 13:
    va = r_stval();
    if (vm_try_growstack(p->vm, va) == 0) {
      vma = vm_find_area(p->vm, va);
      if (vma != NULL && vma_validate(vma, va, 1, VM_FLAG_USERMAP | VM_FLAG_READ) == 0) {
        break;
      }
    }
    printf("usertrap(): page fault on read 0x%lx pid=%d\n", r_scause(), p->pid);
    printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    printf("            pgtbl=0x%lx\n", (uint64)p->vm->pagetable);
    kill(p->pid, SIGSEGV);
    break;
  case 15:
    va = r_stval();
    if (vm_try_growstack(p->vm, va) == 0) {
      vma = vm_find_area(p->vm, va);
      if (vma != NULL && vma_validate(vma, va, 1, VM_FLAG_USERMAP | VM_FLAG_WRITE) == 0) {
        break;
      }
    }
    printf("usertrap(): page fault on write 0x%lx pid=%d\n", r_scause(), p->pid);
    printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    printf("            pgtbl=0x%lx\n", (uint64)p->vm->pagetable);
    kill(p->pid, SIGSEGV);
    break;
  default:
    if((which_dev = devintr()) != 0){
      // ok
    } else {
      printf("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
      printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
      assert(p->pid != 1, "init exiting");
      kill(p->pid, SIGSEGV);
    }
    break;
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    PROC_SET_NEEDS_RESCHED(p);
  
  usertrapret();
}

extern void sig_trampoline(uint64 arg0, uint64 arg1, uint64 arg2, void *handler);

// Will only modify the user space memory and p->sig_ucontext
// Further modifications to the process struct need to be done if it succeeds.
int push_sigframe(struct proc *p, 
                  int signo,
                  sigaction_t *sa,
                  ksiginfo_t *info)
{
  if (sa == NULL || sa->sa_handler == NULL || p == NULL) {
    return -1; // Invalid arguments
  }

  uint64 new_sp = 0;
  if ((sa->sa_flags & SA_ONSTACK) != 0 && \
      (p->sig_stack.ss_flags & (SS_ONSTACK | SS_DISABLE)) == 0) {
    // Use the alternate stack if SA_ONSTACK is set.

    if (p->sig_stack.ss_size < MINSIGSTKSZ) {
      return -1; // Stack too small
    }
    new_sp = (uint64)p->sig_stack.ss_sp + p->sig_stack.ss_size;
  } else {
    new_sp = p->trapframe->sp;
  }

  new_sp -= 0x10UL;
  new_sp &= ~0xFUL; // align to 16 bytes
  uint64 new_ucontext = new_sp - sizeof(ucontext_t);
  new_ucontext &= ~0xFUL;
  uint64 user_siginfo = 0;
  if (sa->sa_flags & SA_SIGINFO) {
    assert(info != NULL, "push_sigframe: info is NULL when SA_SIGINFO is set");
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
  uc.uc_link = (ucontext_t*)p->sig_ucontext;
  uc.uc_sigmask = sa->sa_mask;
  memmove(&uc.uc_mcontext, p->trapframe, sizeof(mcontext_t));
  memmove(&uc.uc_stack, &p->sig_stack, sizeof(stack_t));

  // Copy the trap frame to the signal trap frame.
  if (vm_copyout(p->vm, new_ucontext, (void *)&uc, sizeof(ucontext_t)) != 0) {
    return -1; // Copy failed
  }
  if (sa->sa_flags & SA_SIGINFO) {
    if (vm_copyout(p->vm, user_siginfo, &info->info, sizeof(siginfo_t)) != 0) {
      return -1; // Copy failed
    }
  }

  p->trapframe->sp = new_sp;
  p->trapframe->epc = (uint64)SIG_TRAMPOLINE; // Set the epc to the signal trampoline
  p->trapframe->a0 = signo; // Set the first argument
  p->trapframe->a1 = user_siginfo; // Set the second argument
  p->trapframe->a2 = new_ucontext; // Set the third argument
  p->trapframe->t0 = (uint64)sa->sa_handler; // Set the handler address
  p->sig_ucontext = new_ucontext;

  return 0; // Success
}

int restore_sigframe(struct proc *p, ucontext_t *ret_uc)
{
  uint64 sig_ucontext = p->sig_ucontext;
  
  if (sig_ucontext == 0 || ret_uc == NULL) {
    return -1; // No signal trap frame to restore
  }
  
  // Copy the signal trap frame back to the user trap frame.
  if (vm_copyin(p->vm, (void *)ret_uc, sig_ucontext, sizeof(ucontext_t)) != 0) {
    return -1; // Copy failed
  }

  p->sig_ucontext = (uint64)ret_uc->uc_link;
  memmove(p->trapframe, &ret_uc->uc_mcontext, sizeof(mcontext_t));

  return 0; // Success
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  if (killed(p)) {
    // If the process is terminated, exit it.
    exit(-1);
  }
  
  handle_signal();

  if (PROC_NEEDS_RESCHED(p)) {
    yield();
  }
  
  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->ksp;
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->vm->pagetable);

  // printf("user pagetable before usertrapret:\n");
  // dump_pagetable(p->vm.pagetable, 2, 0, 0, 0, false);
  // printf("\n");

  // jump to userret in trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  uint64 trapframe_base = TRAPFRAME;
  trapframe_base += (uint64)p->trapframe - PGROUNDDOWN((uint64)p->trapframe);
  ((void (*)(uint64, uint64))trampoline_userret)(trapframe_base, satp);
}

void
kerneltrap_dump_regs(struct ktrapframe *sp, uint64 spc)
{
  printf("kerneltrap_dump_regs:\n");
  printf("pc: 0x%lx\n", spc);
  printf("ra: 0x%lx, sp: 0x%lx, s0: 0x%lx\n", 
         sp->ra, sp->sp, sp->s0);
  printf("tp: 0x%lx, t0: 0x%lx, t1: 0x%lx, t2: 0x%lx\n",
         sp->tp, sp->t0, sp->t1, sp->t2);
  printf("a0: 0x%lx, a1: 0x%lx, a2: 0x%lx, a3: 0x%lx\n",
         sp->a0, sp->a1, sp->a2, sp->a3);
  printf("a4: 0x%lx, a5: 0x%lx, a6: 0x%lx, a7: 0x%lx\n",
         sp->a4, sp->a5, sp->a6, sp->a7);
  printf("t3: 0x%lx, t4: 0x%lx, t5: 0x%lx, t6: 0x%lx\n",
         sp->t3, sp->t4, sp->t5, sp->t6);
  printf("gp: 0x%lx\n", sp->gp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap(struct ktrapframe *sp, uint64 s0)
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    // interrupt or trap from an unknown source
    // printf("0x%lx 0x%lx\n", sp, s0);
    printf("scause=0x%lx(%s) sepc=0x%lx stval=0x%lx\n", scause, __scause_to_str(scause), r_sepc(), r_stval());
    sp->ra = r_sepc();
    // to enconvinient gdb back trace
    *(uint64 *)((uint64)sp - 8) = r_sepc();
    if (myproc() == NULL) {
      printf("kerneltrap: no current process\n");
    }
    size_t kstack_size = (1UL << (PAGE_SHIFT + myproc()->kstack_order));
    print_backtrace(sp->s0, myproc()->kstack, myproc()->kstack + kstack_size);
    kerneltrap_dump_regs(sp, r_sepc());
    panic_disable_bt();
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  // Also not to yield if it's scheduling
  if(which_dev == 2 && myproc() != 0 && !sched_holding())
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  if(cpuid() == 0){
    __atomic_fetch_add(&ticks, 1, __ATOMIC_SEQ_CST);
    sched_timer_tick();
  }

  // ask for the next timer interrupt. this also clears
  // the interrupt request. 1000000 is about a tenth
  // of a second.
  w_stimecmp(r_time() + JIFF_TICKS);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if(scause == 0x8000000000000009L){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq == E1000_IRQ){
      e1000_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000005L){
    // timer interrupt.
    clockintr();
    return 2;
  } else {
    return 0;
  }
}

