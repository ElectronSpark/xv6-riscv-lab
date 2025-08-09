#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"
#include "vm.h"

STATIC int loadseg(pagetable_t, uint64, struct xv6_inode*, uint, uint, uint64);

int flags2perm(int flags)
{
    int perm = 0;
    if(flags & 0x1)
      perm = PTE_X;
    if(flags & 0x2)
      perm |= PTE_W;
    if(flags & 0x4)
      perm |= PTE_R;
    return perm;
}

int flags2vmperm(int flags)
{
    int perm = 0;
    if(flags & 0x1)
      perm = VM_FLAG_EXEC;
    if(flags & 0x2)
      perm |= VM_FLAG_WRITE;
    if(flags & 0x4)
      perm |= VM_FLAG_READ;
    return perm;
}

int ustack_alloc(vm_t *vm, uint64 *sp)
{
  uint64 ret_sp = USTACKTOP;
  uint64 stackbase = USTACKTOP - USERSTACK * PGSIZE;
  if (va_alloc(vm, stackbase, USERSTACK * PGSIZE, VM_FLAG_USERMAP | VM_FLAG_WRITE | VM_FLAG_READ | VM_FLAG_GROWSDOWN) == NULL) {
    return -1; // Allocation failed
  }
  for (uint64 i = stackbase; i < USTACKTOP; i += PGSIZE) {
    pte_t *pte = walk(vm->pagetable, i, 1, NULL, NULL);
    if (pte == NULL) {
      return -1; // Walk failed
    }
    uint64 pa = (uint64)kalloc();
    if (pa == 0) {
      return -1; // kalloc failed
    }
    memset((void *)pa, 0, PGSIZE); // Initialize the page
    *pte = PA2PTE(pa) | PTE_V | PTE_U | PTE_W | PTE_W; // Allocate and map the page
  }
  *sp = ret_sp; // Set the stack pointer
  return 0; // Success
}

int
exec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, heap_start = 0, sp, ustack[MAXARG];
  uint64 stackbase = USTACKTOP - USERSTACK * PGSIZE;
  struct elfhdr elf;
  struct xv6_inode *ip;
  struct proghdr ph;
  vm_t *tmp_vm = NULL;
  struct proc *p = myproc();

  begin_op();

  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);

  // Check ELF header
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  if(elf.magic != ELF_MAGIC)
    goto bad;

  if ((tmp_vm = vm_init((uint64)p->trapframe)) == NULL) {
    goto bad;
  }

  // Load program into memory.
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    vma_t *vma = va_alloc(tmp_vm, ph.vaddr, ph.memsz, flags2vmperm(ph.flags) | VM_FLAG_USERMAP);
    if (vma == NULL) {
      goto bad; // Allocation failed
    }
    // @TODO: to locate the start of heap
    uint64 size1 = ph.vaddr + ph.memsz;
    if (heap_start < size1) {
      heap_start = size1; // Update heap start if this segment extends it
    }
    if(loadseg(tmp_vm->pagetable, ph.vaddr, ip, ph.off, ph.filesz, flags2perm(ph.flags)) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  p = myproc();

  // Allocate some pages at the next page boundary.
  // Make the first inaccessible as a stack guard.
  // Use the rest as the user stack.
  // Create heap area and reserve one page for heap space.
  if (vm_createheap(tmp_vm, heap_start, USERSTACK * PGSIZE) != 0) {
    goto bad; // Heap allocation failed
  }
  if (vm_createstack(tmp_vm, USTACKTOP, USERSTACK * PGSIZE) != 0) {
    goto bad; // Stack allocation failed
  }
  sp = USTACKTOP;

  // Push argument strings, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if(sp < stackbase)
      goto bad;
    if(vm_copyout(tmp_vm, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push the array of argv[] pointers.
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(vm_copyout(tmp_vm, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  p->trapframe->a1 = sp;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));
    
  // Commit to the user image.
  vm_destroy(p->vm); // Destroy the old VM
  p->vm = tmp_vm; // Set the new VM
  p->trapframe->epc = elf.entry;  // initial program counter = main
  p->trapframe->sp = sp; // initial stack pointer
  return argc; // this ends up in a0, the first argument to main(argc, argv)

 bad:
  vm_destroy(tmp_vm); // Clean up the temporary VM
  if(ip){
    iunlockput(ip);
    end_op();
  }
  return -1;
}

// Load a program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
STATIC int
loadseg(pagetable_t pagetable, uint64 va, struct xv6_inode *ip, uint offset, uint sz, uint64 pteflags)
{
  uint i, n;
  uint64 pa;

  for(i = 0; i < sz; i += PGSIZE){
    // pte_t *pte = walk(pagetable, va + i, 1, NULL, NULL);
    // if (pte == 0)
    //   return -1; // walk failed
    // if(*pte & PTE_V)
    //   panic("loadseg: remap");
    pa = (uint64)kalloc();
    if(pa == 0)
      return -1; // kalloc failed
    
    if(sz - i < PGSIZE)
    {
      n = sz - i;
      memset((void *)(pa + n), 0, PGSIZE - n);
    }
    else
    {
      n = PGSIZE;
    }
    if(readi(ip, 0, pa, offset+i, n) != n)
    {
      kfree((void *)pa);
      return -1;
    }
    if(mappages(pagetable, va + i, PGSIZE, pa, pteflags | PTE_U | PTE_V) != 0)
    {
      kfree((void *)pa);
      return -1; // mappages failed
    }
  }
  
  return 0;
}
