#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "sched.h"
#include "signal.h"

struct spinlock tickslock;
uint ticks;

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
  spin_init(&tickslock, "time");
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
  pagetable_t pagetable;
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
  case 15:
    va = r_stval();
    pagetable = p->pagetable;
    if (uvmvalidate_w(pagetable, va) != 0) {
      printf("usertrap(): page fault on write 0x%lx pid=%d\n", r_scause(), p->pid);
      printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
      printf("            pgtbl=0x%lx\n", (uint64)pagetable);
      setkilled(p);
    }
    break;
  default:
    if((which_dev = devintr()) != 0){
      // ok
    } else {
      printf("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
      printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
      assert(p->pid != 1, "init exiting");
      setkilled(p);
    }
    break;
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    set_needs_resched(p);
  
  usertrapret();
}

extern void sig_trampoline(uint64 arg0, uint64 arg1, uint64 arg2, void *handler);

int push_sigtrapframe(struct proc *p, void *handler, uint64 arg0, uint64 arg1, uint64 arg2)
{
  // @TODO: Need to take care of stack overflow.
  // ucontext_t uc = {0};
  uint64 new_sigtrap = p->trapframe->sp - sizeof(struct trapframe);
  p->trapframe->prev = p->sigtrapframe;

  // Copy the trap frame to the signal trap frame.
  if (copyout(p->pagetable, new_sigtrap, (void *)p->trapframe, sizeof(struct trapframe)) != 0) {
    return -1; // Copy failed
  }

  p->sigtrapframe = new_sigtrap;
  p->trapframe->sp = new_sigtrap - 16;
  p->trapframe->epc = (uint64)SIG_TRAMPOLINE; // Set the epc to the signal trampoline
  p->trapframe->a0 = arg0; // Set the first argument
  p->trapframe->a1 = arg1; // Set the second argument
  p->trapframe->a2 = arg2; // Set the third argument
  p->trapframe->t0 = (uint64)handler; // Set the handler address

  return 0; // Success
}

int restore_sigtrapframe(struct proc *p)
{
  if (p->sigtrapframe == 0) {
    return -1; // No signal trap frame to restore
  }

  uint64 sigtrapframe = p->sigtrapframe;

  // Copy the signal trap frame back to the user trap frame.
  if (copyin(p->pagetable, (void *)p->trapframe, sigtrapframe, sizeof(struct trapframe)) != 0) {
    return -1; // Copy failed
  }

  p->sigtrapframe = p->trapframe->prev; // Restore the previous signal trap frame

  return 0; // Success
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  spin_acquire(&p->lock);
  int terminated = signal_terminated(p->sigacts);
  spin_release(&p->lock);

  if (terminated || killed(p)) {
    // If the process is terminated, exit it.
    setkilled(p);
    exit(-1);
  }

  if (needs_resched(p)) {
    yield();
  }
  
  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  int signo;
  sigaction_t *sa = signal_take(p->sigacts, &signo);
  if(sa){
    if (push_sigtrapframe(p, sa->handler, signo, 0, 0) != 0) {
      exit(-1); // Failed to push the signal trap frame
    }
  }

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  // p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  uint64 ksp = p->kstack + KERNEL_STACK_SIZE;
  ksp -= sizeof(struct trapframe) + 8;
  ksp &= ~0x7UL; // align to 8 bytes
  p->trapframe->kernel_sp = ksp;
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
  uint64 satp = MAKE_SATP(p->pagetable);

  // printf("user pagetable before usertrapret:\n");
  // dump_pagetable(p->pagetable, 2, 0, 0, 0, true);
  // printf("\n");

  // jump to userret in trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

void
kerneltrap_dump_regs(struct ktrapframe *sp)
{
  printf("kerneltrap_dump_regs:\n");
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
    if (myproc() == NULL)
      printf("kerneltrap: no current process\n");
    else
      print_backtrace((uint64)sp, myproc()->kstack, myproc()->kstack + KERNEL_STACK_SIZE);
    kerneltrap_dump_regs(sp);
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
    spin_acquire(&tickslock);
    ticks++;
    if (!sched_holding())
      wakeup(&ticks);
    spin_release(&tickslock);
  }

  // ask for the next timer interrupt. this also clears
  // the interrupt request. 1000000 is about a tenth
  // of a second.
  w_stimecmp(r_time() + 1000000);
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

