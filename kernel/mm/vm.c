#include "param.h"
#include "types.h"
#include "string.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "page.h"
#include "rbtree.h"
#include "list.h"
#include "vm.h"
#include "slab.h"

static slab_cache_t __vma_pool = {0};
static slab_cache_t __vm_pool = {0};

static void __vma_pool_init(void)
{
  slab_cache_init(&__vma_pool, "vm area", sizeof(vma_t), SLAB_FLAG_STATIC);
}

static void __vm_pool_init(void)
{
  slab_cache_init(&__vm_pool, "vm", sizeof(vm_t), SLAB_FLAG_STATIC);
}

static vma_t *__vma_alloc(vm_t *vm)
{
  vma_t *vma = slab_alloc(&__vma_pool);
  if (vma == NULL) {
    return NULL;
  }
  memset(vma, 0, sizeof(vma_t));
  rb_node_init(&vma->rb_entry);
  list_entry_init(&vma->list_entry);
  list_entry_init(&vma->free_list_entry);
  vma->vm = vm;
  return vma;
}

static void __vma_free(vma_t *vma)
{
  if (vma) {
    slab_free((void*)vma);
  }
}


/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

static void *__pgtab_alloc(void)
{
  void *pa = page_alloc(0, PAGE_TYPE_PGTABLE);
  if (pa) {
    memset(pa, 0, PGSIZE);
  }
  return pa;
}

static void __pgtab_free(void *pa)
{
  page_t *page = __pa_to_page((uint64)pa);
  if (page == NULL) {
    panic("__pgtab_free: invalid page table address");
  }
  page_lock_acquire(page);
  if (!PAGE_IS_TYPE(page, PAGE_TYPE_PGTABLE)) {
    panic("__pgtab_free: trying to free a non-pagetable page");
  }
  page_lock_release(page);
  page_free(pa, 0);
}

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) __pgtab_alloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);
  kvmmap(kpgtbl, VIRTIO1, VIRTIO1, PGSIZE, PTE_R | PTE_W);

  // PCI-E ECAM (configuration space), for pci.c
  kvmmap(kpgtbl, PCIE_ECAM, PCIE_ECAM, 0x10000000, PTE_R | PTE_W);

  // pci.c maps the e1000's registers here.
  kvmmap(kpgtbl, E1000_PCI_ADDR, E1000_PCI_ADDR, 0x20000, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
  printf("trampoline 0x%lx -> %p\n", TRAMPOLINE, trampoline);
  printf("signal trampoline would be at 0x%lx\n", SIG_TRAMPOLINE);
  
  // map kernel symbols
  kvmmap(kpgtbl, KERNEL_SYMBOLS_START, KERNEL_SYMBOLS_START, KERNEL_SYMBOLS_SIZE, PTE_R);
  
  // map kernel symbols index
  kvmmap(kpgtbl, KERNEL_SYMBOLS_IDX_START, KERNEL_SYMBOLS_IDX_START, KERNEL_SYMBOLS_IDX_SIZE, PTE_R | PTE_W);
  
  // allocate and map a kernel stack for each process.
  // proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the one kernel_pagetable
void
kvminit(void)
{
  __vma_pool_init(); // Initialize the VMA pool
  __vm_pool_init(); // Initialize the VM pool
  kernel_pagetable = kvmmake();
  
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

void 
dump_pagetable(pagetable_t pagetable, int level, int indent, uint64 va_base, uint64 va_end, bool omit_pa)
{
  if (level < 0 || level > 2) {
    printf("Invalid level %d for pagetable dump\n", level);
    return;
  }

  int idx_start = 0;
  int idx_end = 512;
  idx_start = PX(2, va_base);
  if (level == 2 && va_end != 0){
    idx_end = PX(2, va_end);
  }

  if (level == 0) {
    // Leaf level - print in chunks
    int chunk_start = -1;
    uint64 chunk_va_start = 0;
    uint64 chunk_pa_start = 0;
    uint32 chunk_flags = 0;
    int chunk_count = 0;

    for (int i = idx_start; i <= idx_end; i++) {
      pte_t pte = (i < idx_end) ? pagetable[i] : 0;
      uint64 va = va_base | (((uint64)i) << 12);
      uint64 pa = PTE2PA(pte);
      uint32 flags = PTE_FLAGS(pte) | ((pte & PTE_RSW_w) ? PTE_V : 0);
      
      bool valid_entry = (i < idx_end) && (pte & (PTE_V | PTE_RSW_w)) && 
                        !(omit_pa && va >= KERNBASE && va < PHYSTOP);
      
      if (valid_entry && chunk_start == -1) {
        // Start new chunk
        chunk_start = i;
        chunk_va_start = va;
        chunk_pa_start = pa;
        chunk_flags = flags;
        chunk_count = 1;
      } else if (valid_entry && chunk_start != -1 && 
                 pa == chunk_pa_start + (chunk_count * PGSIZE) &&
                 flags == chunk_flags) {
        // Continue chunk
        chunk_count++;
      } else {
        // End current chunk and print it
        if (chunk_start != -1) {
          const char *str_v = (chunk_flags & PTE_V) ? "V" : " ";
          const char *str_u = (chunk_flags & PTE_U) ? "U" : " ";
          const char *str_w = (chunk_flags & PTE_W) ? "W" : " ";
          const char *str_x = (chunk_flags & PTE_X) ? "X" : " ";
          const char *str_r = (chunk_flags & PTE_R) ? "R" : " ";
          const char *str_rsw = (chunk_flags & PTE_RSW_w) ? "C" : " ";
          
          if (chunk_count == 1) {
            printf("%*sPTE[%d](%p): %lx(%s%s%s%s%s%s), (va, pa): (%p, %p)\n", 
                    indent, "", chunk_start, &pagetable[i], chunk_flags & ~PTE_V,
                    str_v, str_u, str_w, str_x, str_r, str_rsw,
                    (void *)chunk_va_start, (void *)chunk_pa_start);
          } else {
            printf("%*sPTE[%d-%d]: %lx(%s%s%s%s%s%s), (va, pa): (%p-%p, %p-%p) [%d pages]\n", 
                    indent, "", chunk_start, chunk_start + chunk_count - 1, 
                    chunk_flags & ~PTE_V, str_v, str_u, str_w, str_x, str_r, str_rsw,
                    (void *)chunk_va_start, 
                    (void *)(chunk_va_start + (chunk_count - 1) * PGSIZE),
                    (void *)chunk_pa_start, 
                    (void *)(chunk_pa_start + (chunk_count - 1) * PGSIZE),
                    chunk_count);
          }
        }
        
        // Start new chunk if current entry is valid
        if (valid_entry) {
          chunk_start = i;
          chunk_va_start = va;
          chunk_pa_start = pa;
          chunk_flags = flags;
          chunk_count = 1;
        } else {
          chunk_start = -1;
        }
      }
    }
  } else {
    // Non-leaf level - recurse normally
    for (int i = idx_start; i < idx_end; i++) {
      pte_t pte = pagetable[i];
      if (pte & (PTE_V | PTE_RSW_w)) {
        uint64 va = va_base | (((uint64)i) << (12 + 9 * level));
        if (omit_pa && va >= KERNBASE && va < PHYSTOP) {
          continue;
        }
        void *pa = (void *)PTE2PA(pte);
        const char *str_v = (pte & PTE_V) ? "V" : " ";
        const char *str_u = (pte & PTE_U) ? "U" : " ";
        const char *str_w = (pte & PTE_W) ? "W" : " ";
        const char *str_x = (pte & PTE_X) ? "X" : " ";
        const char *str_r = (pte & PTE_R) ? "R" : " ";
        const char *str_rsw = (pte & PTE_RSW_w) ? "C" : " ";
        printf("%*sPTE[%d](%p): %x(%s%s%s%s%s%s), (va, pa): (%p, %p)", 
                indent, "", i, &pagetable[i], (uint32)PTE_FLAGS(pte),
                str_v, str_u, str_w, str_x, str_r, str_rsw,
                (void *)va, pa);
        if (level > 0 && PTE_FLAGS(pte) == PTE_V) {
          // This is a page table pointer.
          printf(":\n");
          dump_pagetable((pagetable_t)pa, level - 1, indent + 2, va, 0, omit_pa);
        } else {
          printf("\n");
        }
      }
    }
  }
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc, pte_t **retl2, pte_t **retl1)
{//TODO: print out page table
  assert(va < MAXVA, "walk: va out of range");
  assert(pagetable != NULL, "walk: pagetable is null");

  pte_t *ret_pte[3] = {NULL, NULL, NULL};

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    ret_pte[level] = pte;
    assert(pte != NULL, "walk: pte is null");
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)__pgtab_alloc()) == 0)
        return NULL;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  if (retl2) {
    *retl2 = ret_pte[2];
  }
  if (retl1) {
    *retl1 = ret_pte[1];
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0, NULL, NULL);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1, NULL, NULL)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0, NULL, NULL)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped, va=%p, pa=%p, flags: %lx", (void *)a, (void*)PTE2PA(*pte), PTE_FLAGS(*pte));
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    uint64 pa = PTE2PA(*pte);
    *pte = 0;
    if(do_free){
      __pgtab_free((void*)pa);
    }
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) __pgtab_alloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_RSW_w|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  __pgtab_free((void*)pagetable);
}

// // Given a parent process's page table, copy
// // its memory into a child's page table.
// // Copies both the page table and the
// // physical memory.
// // returns 0 on success, -1 on failure.
// // frees any allocated pages on failure.
// int
// uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
// {
//   pte_t *pte;
//   uint64 pa, i;
//   uint flags;

//   for(i = 0; i < sz; i += PGSIZE){
//     if((pte = walk(old, i, 0)) == 0)
//       panic("uvmcopy: pte should exist");
//     if((*pte & PTE_V) == 0)
//       panic("uvmcopy: page not present");
//     flags = PTE_FLAGS(*pte);
//     pa = PTE2PA(*pte);
//     if (page_ref_inc((void*)pa) <= 0) {
//       goto err;
//     }
//     if (flags & (PTE_RSW_w | PTE_W)) {
//       // If the page is writable or has COW flag
//       flags &= ~PTE_W; // Do copy when write(COW)
//       flags |= PTE_RSW_w; // Set COW flag
//       *pte = PA2PTE(pa) | flags; // Restore PTE with COW flag
//     }
//     if(mappages(new, i, PGSIZE, pa, flags) != 0){
//       goto err;
//     }
//   }
//   return 0;

//  err:
//   uvmunmap(new, 0, i / PGSIZE, 1);
//   return -1;
// }

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  freewalk(pagetable);
}

int vm_copyout(vm_t *vm, uint64 dstva, const void *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if(va0 >= MAXVA)
      return -1;
    vma_t *vma = vm_find_area(vm, va0);
    if (vma == NULL || vma_validate(vma, va0, PGSIZE, VM_FLAG_USERMAP | VM_FLAG_WRITE) != 0) {
      printf("vma_copyout: invalid vma for va %lx\n", va0);
      return -1;
    }

    pte = walk(vm->pagetable, va0, 0, NULL, NULL);
    assert(pte != NULL, "vma_copyout: pte should not be null");

    pa0 = PTE2PA(*pte);
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

int vm_copyin(vm_t *vm, void *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    vma_t *vma = vm_find_area(vm, va0);
    if (vma == NULL || vma_validate(vma, va0, PGSIZE, VM_FLAG_USERMAP | VM_FLAG_READ) != 0) {
      return -1;
    }
    pa0 = walkaddr(vm->pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

int vm_copyinstr(vm_t *vm, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    vma_t *vma = vm_find_area(vm, va0);
    if (vma == NULL || vma_validate(vma, va0, PGSIZE, VM_FLAG_USERMAP | VM_FLAG_READ) != 0) {
      return -1;
    }
    pa0 = walkaddr(vm->pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

// free the pages and ptes in a vm area, and set the VMA as free.
static void __vma_set_free(vma_t *vma)
{
  if (vma == NULL || vma->vm == NULL) {
    return;
  }
  if (vma->flags == VM_FLAG_NONE) {
    return; // Already free
  }
  assert((vma->start & (PGSIZE - 1)) == 0, "__vma_set_free: vma start not aligned");
  if (vma->vm->pagetable != NULL) {
    pagetable_t pagetable = vma->vm->pagetable;
    for(uint64 a = vma->start; a < vma->end; a += PGSIZE){
      pte_t *pte = walk(pagetable, a, 0, NULL, NULL);
      if(pte == 0)
        continue; // Not mapped, skip
      if(PTE_FLAGS(*pte) == PTE_V)
        panic("__vma_set_free: not a leaf");
      if((*pte & PTE_V) == 0)
        continue;
      uint64 pa = PTE2PA(*pte);
      *pte = 0;
      page_ref_dec((void*)pa);
    }
    // Drop any stale TLB entries covering the unmapped range.
    sfence_vma();
  }

  vma->flags = VM_FLAG_NONE; // Set the VMA as free
  vma->file = NULL; // Clear the file pointer
  vma->pgoff = 0; // Reset the page offset
  assert(LIST_NODE_IS_DETACHED(vma, free_list_entry), "__vma_set_free: vma already in free list");
}

static int __vma_dup(vma_t *dst, vma_t *src)
{
  // @TODO: need to take care of file and pgoff if they are not NULL
  if (dst == NULL || src == NULL) {
    return -1; // Invalid parameters
  }
  if (src->vm == NULL || dst->vm == NULL) {
    return -1; // Source or destination VM is NULL
  }
  if (VMA_SIZE(src) != VMA_SIZE(dst)) {
    return -1; // Source VMA is empty, cannot duplicate
  }
  // @TODO: ?
  if ((src->flags & VM_FLAG_PROT_MASK) != (dst->flags & VM_FLAG_PROT_MASK)) {
    return -1; // Destination VMA is not free, cannot duplicate
  }
  
  dst->flags = src->flags;
  dst->file = src->file; // Shallow copy of file pointer
  dst->pgoff = src->pgoff;
  if (src->flags != VM_FLAG_NONE) {
    pagetable_t pgtb_src = src->vm->pagetable;
    pagetable_t pgtb_dst = dst->vm->pagetable;
    for(uint64 a = src->start; a < src->end; a += PGSIZE){
      pte_t *src_pte = walk(pgtb_src, a, 0, NULL, NULL);
      if(src_pte == NULL || *src_pte == 0)
        continue; // Not mapped, skip
      if(PTE_FLAGS(*src_pte) == PTE_V)
        panic("__vma_dup: not a leaf");
      if((PTE_FLAGS(*src_pte) & PTE_V) == 0)
        continue;
      pte_t *new_pte = walk(pgtb_dst, a, 1, NULL, NULL);
      if (new_pte == NULL) {
        __vma_set_free(dst);
        return -1; // Failed to allocate new PTE
      }
      *src_pte |= PTE_RSW_w; // Set COW flag
      *src_pte &= ~PTE_W; // Clear write flag
      *new_pte = *src_pte; // Copy the PTE
      uint64 pa = PTE2PA(*src_pte);
      assert(page_ref_inc((void*)pa) > 0, "__vma_dup: page refcnt should be greater than 0");
    }
    // Flush TLB so downgraded parent PTEs lose stale writable entries (COW safety).
    sfence_vma();
  }
  return 0;
}

static int __vma_cmp(uint64 a, uint64 b)
{
  if (a == b) {
    return 0; // Equal
  }
  if (a < b) {
    return -1; // a is less than b
  }
  return 1;
}

static uint64 __cma_get_key(struct rb_node *node)
{
  vma_t *vma = container_of(node, vma_t, rb_entry);
  return vma->start;
}

static struct rb_root_opts __vm_tree_opts = {
    .keys_cmp_fun = __vma_cmp,
    .get_key_fun = __cma_get_key,
};

static void __vm_unmap_trapframe(vm_t *vm)
{
  if (vm == NULL || vm->pagetable == NULL) {
    return; // Invalid VM or pagetable
  }
  uvmunmap(vm->pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(vm->pagetable, SIG_TRAMPOLINE, 1, 0);
  uvmunmap(vm->pagetable, TRAPFRAME, 1, 0);
}

void vm_destroy(vm_t *vm)
{
  if (vm == NULL) {
    return; // Nothing to destroy
  }
  vm->valid = false; // Mark the VM as invalid
  vma_t *vma, *tmp;
  list_foreach_node_safe(&vm->vm_list, vma, tmp, list_entry) {
    __vma_set_free(vma);
    __vma_free(vma);
  }
  list_entry_init(&vm->vm_list);
  list_entry_init(&vm->vm_free_list);
  rb_root_init(&vm->vm_tree, &__vm_tree_opts);
  if (vm->trapframe != 0) {
    __vm_unmap_trapframe(vm); // Unmap the trapframe and trampolines
  }
  if (vm->pagetable != NULL) {
    uvmfree(vm->pagetable, 0); // Free the pagetable
    vm->pagetable = NULL; // Clear the pagetable pointer
  }
  slab_free((void*)vm); // Free the VM structure
}

// Duplicate the VM structure from src to dst.
// The destination VM must be initialized as user vm, and empty.
// Files have to be duplicated.
vm_t *vm_dup(vm_t *src, uint64 trapframe)
{
  if (src == NULL) {
    return NULL; // Invalid parameters
  }
  if (src->trapframe != 0 && trapframe == 0) {
    return NULL; // Cannot duplicate if src has a trapframe but dst does not
  }
  vm_t *dst = vm_init(trapframe);
  vma_t *vma, *tmp;
  list_foreach_node_safe(&src->vm_list, vma, tmp, list_entry) {
    if (vma->flags == VM_FLAG_NONE) {
      continue;
    }
    vma_t *new_vma = va_alloc(dst, vma->start, VMA_SIZE(vma), vma->flags);
    if (new_vma == NULL) {
      vm_destroy(dst);
      return NULL; // Allocation failed
    }
    if (vma == src->stack) {
      dst->stack = new_vma; // Set the stack for the new VM
      dst->stack_size = src->stack_size; // Copy the stack size
    } else if (vma == src->heap) {
      dst->heap = new_vma; // Set the heap for the new VM
      dst->heap_size = src->heap_size; // Copy the heap size
    }
    if (vma->flags != VM_FLAG_NONE && __vma_dup(new_vma, vma) != 0) {
      __vma_free(new_vma);
      vm_destroy(dst);
      return NULL; // Duplication failed
    }
  }
  return dst;
}

// map trapframe and trampolines for user processes.
static int __vm_map_trampoline(vm_t *vm, uint64 trapframe)
{
  extern char sig_trampoline[];
  if (vm == NULL || vm->pagetable == NULL) {
    return -1; // Invalid VM or pagetable
  }
  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(vm->pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    return -1;
  }

  // Map the signal trampoline page just below the trampoline page.
  // The user epc will point to this page when a signal is delivered.
  if(mappages(vm->pagetable, SIG_TRAMPOLINE, PGSIZE,
              (uint64)sig_trampoline, PTE_U | PTE_R | PTE_X) < 0){
    uvmunmap(vm->pagetable, TRAMPOLINE, 1, 0);
    return -1;
  }

  // map the trapframe page just below the signal trampoline page, for
  // trampoline.S.
  trapframe = PGROUNDDOWN(trapframe); // Ensure trapframe is page-aligned
  if(mappages(vm->pagetable, TRAPFRAME, PGSIZE,
              trapframe, PTE_R | PTE_W | PTE_RSW_w) < 0){
    uvmunmap(vm->pagetable, TRAMPOLINE, 1, 0);
    uvmunmap(vm->pagetable, SIG_TRAMPOLINE, 1, 0);
    return -1;
  }
  vm->trapframe = trapframe; // Store the trapframe pointer in the VM
  return 0;
}

// Initialize the vm struct of a process.
vm_t *vm_init(uint64 trapframe) {
  vm_t *vm = slab_alloc(&__vm_pool);
  if (vm == NULL) {
    return NULL; // Out of memory
  }
  memset(vm, 0, sizeof(vm_t));
  rb_root_init(&vm->vm_tree, &__vm_tree_opts);
  list_entry_init(&vm->vm_list);
  list_entry_init(&vm->vm_free_list);
  vma_t *vma = __vma_alloc(vm);
  if (vma == NULL) {
    vm_destroy(vm);
    return NULL;
  }

  vm->pagetable = uvmcreate();
  if (vm->pagetable == NULL) {
    vm_destroy(vm);
    return NULL;
  }
  if (trapframe != 0) {
    if (__vm_map_trampoline(vm, trapframe) != 0) {
      vm_destroy(vm);
      return NULL; // Failed to map trampoline
    }
  }

  vma->start = UVMBOTTOM; // Start of user virtual memory
  vma->end = UVMTOP; // End of user virtual memory
  // Add the initial VMA for the process.
  rb_insert_color(&vm->vm_tree, &vma->rb_entry);
  list_node_push(&vm->vm_free_list, vma, list_entry);
  list_node_push(&vm->vm_list, vma, list_entry);
  vm->valid = true; // Mark the VM as valid

  return vm;
}

static inline vma_t *__get_vma_left(vma_t *vma)
{
  if (vma == NULL || vma->vm == NULL) {
    return NULL;
  }
  return rb_prev_entry_safe(vma, rb_entry);
}

static inline vma_t *__get_vma_right(vma_t *vma)
{
  if (vma == NULL || vma->vm == NULL) {
    return NULL;
  }
  return rb_next_entry_safe(vma, rb_entry);
}

vma_t *vm_find_area(vm_t *vm, uint64 va)
{
  if (va >= UVMTOP || va < UVMBOTTOM) {
    return NULL; // Virtual address out of bounds
  }
  struct rb_node *node = rb_find_key_rdown(&vm->vm_tree, va);
  if (node != NULL) {
    vma_t *vma = container_of(node, vma_t, rb_entry);
    assert (VMA_IN_RANGE(vma, va), "vm_find_area: va %lx not in range [%lx, %lx)", va, vma->start, vma->end);
    return vma; // Found the VMA that contains the virtual address
  }
  return NULL;
}

// Split a VMA at the given virtual address.
// Return the later half of the split VMA if successful,
// or NULL if the split fails (e.g., if the VMA cannot be split).
vma_t *vma_split(vma_t *vma, uint64 va)
{
  if (vma == NULL || vma->vm == NULL) {
    return NULL;
  }
  if (va < vma->start || va >= vma->end) {
    return NULL; // Cannot split outside the VMA range
  }

  if (va == vma->start) {
    return vma;
  }

  vma_t *new_vma = __vma_alloc(vma->vm);
  if (new_vma == NULL) {
    return NULL; // Allocation failed
  }

  new_vma->start = va;
  assert(rb_insert_color(&vma->vm->vm_tree, &new_vma->rb_entry) == &new_vma->rb_entry, 
         "vma_split: rb_insert_color failed");

  new_vma->end = vma->end;
  new_vma->flags = vma->flags;
  new_vma->file = vma->file;
  if (vma->file != NULL) {
    new_vma->pgoff = vma->pgoff + (va - vma->start);
  } else {
    new_vma->pgoff = 0; // Reset page offset for anonymous VMA
  }

  vma->end = va; // Adjust the original VMA to the new end

  // Insert the new VMA into the tree and free list
  list_node_insert(vma, new_vma, list_entry);
  if (vma->flags == VM_FLAG_NONE) {
    list_node_insert(vma, new_vma, free_list_entry);
  }

  return new_vma;
}

vma_t *vma_merge(vma_t *vma1, vma_t *vma2)
{
  if (vma1 == NULL || vma2 == NULL || vma1->vm != vma2->vm) {
    return NULL; // Cannot merge VMA from different vm or null VMA
  }
  if (!VM_ADJACENT(vma1, vma2)) {
    return NULL; // VMA do not touch each other
  }
  if ((vma1->flags & VM_FLAG_PROT_MASK) != (vma2->flags & VM_FLAG_PROT_MASK)) {
    return NULL; // Cannot merge VMA with different protection flags
  }
  // Merge the two VMAs
  if (vma1->start > vma2->start) {
    // Ensure vma1 is always the left one
    vma_t *tmp = vma1;
    vma1 = vma2;
    vma2 = tmp;
  }
  if (vma1->file != vma2->file) {
    return NULL; // Cannot merge VMA with different files or offsets
  }
  if (vma1->file != NULL && (vma2->pgoff - vma1->pgoff) != (vma2->start - vma1->start)) {
    return NULL; // Cannot merge VMA with different page offsets
  }

  // Now vma1 starts before or at the same point as vma2
  vma1->end = vma2->end; // Extend the end of vma1 to include vma2
  assert(rb_delete_node_color(&vma2->vm->vm_tree, &vma2->rb_entry) == &vma2->rb_entry,
         "vma_merge: rb_delete_node_color failed"); // Remove from tree
  list_node_detach(vma2, list_entry);
  list_node_detach(vma2, free_list_entry);
  __vma_free(vma2); // Free the merged VMA

  return vma1; // Return the merged VMA
}

vma_t *va_alloc(vm_t *vm, uint64 va, uint64 size, uint64 flags)
{
  if (vm == NULL) {
    return NULL;
  }
  if (size == 0 || (size & (PGSIZE - 1)) != 0) {
    return NULL;
  }
  if ((va & (PGSIZE - 1)) != 0) {
    return NULL; // va must be page-aligned
  }
  if ((flags & VM_FLAG_PROT_MASK) == 0) {
    return NULL; // Invalid protection flags
  }
  // Don't strip non-protection flags like VM_FLAG_USERMAP

  vma_t *free_area = NULL;
  if (va == 0) {
    vma_t *tmp = NULL;
    // If va is 0, then find the last free area with enough size.
    list_foreach_node_inv_safe(&vm->vm_free_list, free_area, tmp, list_entry) {
      if (VMA_SIZE(free_area) >= size) {
        break;
      }
    }
  } else {
    free_area = vm_find_area(vm, va);
  }

  if (free_area == NULL) {
    return NULL; // No free area found
  }

  if (free_area->flags != VM_FLAG_NONE) {
    return NULL; // The free area is not empty
  }

  uint64 va_end = 0;
  if (va == 0) {
    if (VMA_SIZE(free_area) < size) {
      return NULL; // Not enough space in the free area
    }
    va = free_area->start; // Use the start of the free area
  } else {
    if (free_area->end - va < size) {
      return NULL; // Not enough space in the free area
    }
  }
  va_end = va + size;

  vma_t *vma2 = NULL;
  vma_t *vma3 = NULL;
  if (va > free_area->start) {
    // Split the free area if va is not at the start
    vma2 = vma_split(free_area, va);
    assert(vma2 != NULL, "va_alloc: vma_split failed");
  } else {
    vma2 = free_area; // Use the whole free area
  }
  if (va_end < vma2->end) {
    vma3 = vma_split(vma2, va_end);
    assert(vma3 != NULL, "va_alloc: vma_split failed");
  }
  list_node_detach(vma2, free_list_entry);
  vma2->flags = flags; // Set the flags for the new VMA

  return vma2;
}

int va_free(vma_t *vma)
{
  if (vma == NULL || vma->vm == NULL) {
    return -1; // Invalid VMA
  }
  if (vma->flags == VM_FLAG_NONE) {
    return -1; // Cannot free an empty VMA
  }

  vma_t *left = __get_vma_left(vma);
  vma_t *right = __get_vma_right(vma);
  
  if (left == NULL && right == NULL) {
    return -1; // No adjacent VMAs to merge with
  }

  __vma_set_free(vma); // Set the VMA as free
  list_node_push_back(&vma->vm->vm_free_list, vma, free_list_entry);
  if (left != NULL && left->flags == VM_FLAG_NONE) {
    // Merge with the left VMA
    vma = vma_merge(left, vma);
    assert(vma == left, "va_free: vma_merge failed with left VMA");
  }

  if (right != NULL && right->flags == VM_FLAG_NONE) {
    // Merge with the right VMA
    left = vma_merge(vma, right);
    assert(left == vma, "va_free: vma_merge failed with right VMA");
  }

  return 0;
}

static int __vma_validate_pte_rxw(vma_t *vma, pte_t *pte)
{
  pte_t pte_val = *pte;

  if (pte_val & PTE_W) {
    return 0; // Page is already writable
  }
  
  pte_t flags = PTE_FLAGS(pte_val);
  void *addr = (void *)PTE2PA(pte_val);
  void *pa = NULL;
  if (pte_val == 0) {
    // if the pte is not mapped, just map a new page
    pa = page_alloc(0, PAGE_TYPE_ANON);
    if (pa == NULL) {
      return -1; // Page allocation failed
    }
    memset(pa, 0, PGSIZE);
  } else if (pte_val & PTE_V) {
    if (pte_val & PTE_RSW_w) {
      // if the page is marked as COW, we need to handle it
      pa = page_alloc(0, PAGE_TYPE_ANON);
      if (pa == NULL) {
        return -1; // Page allocation failed
      }
      memmove(pa, addr, PGSIZE); // Copy the page content
      flags &= ~PTE_RSW_w; // Clear the COW flag
      assert(page_ref_dec(addr) >= 0, "vma_validate_pte_w: page_ref_dec failed for addr %p", addr);
    } else {
      // Else, just change the page to writable
      pa = addr;
    }
  } else {
    return -1; // Page is already present and writable
  }

  flags |= PTE_V | PTE_W; // Set the flags for writable page
  if (vma->flags & VM_FLAG_READ) {
    flags |= PTE_R; // Set the read permission if VM_FLAG_READ is set
  }
  if (vma->flags & VM_FLAG_EXEC) {
    flags |= PTE_X; // Set the execute permission if VM_FLAG_EXEC is set
  }
  if (vma->flags & VM_FLAG_USERMAP) {
    flags |= PTE_U; // Set the user permission if VM_FLAG_USERMAP is set
  }
  *pte = PA2PTE(pa) | flags; // Update the PTE with the new address and flags

  return 0;
}

static int __vma_validate_pte_rx(vma_t *vma, pte_t *pte)
{
  pte_t pte_val = *pte;
  void *pa = (void *)PTE2PA(pte_val);
  pte_t flags = PTE_FLAGS(pte_val);
  
  if (pte_val == 0) {
    pa = page_alloc(0, PAGE_TYPE_ANON);
    if (pa == NULL) {
      return -1; // Page allocation failed
    }
    memset(pa, 0, PGSIZE); // Initialize the page
    if (vma->flags & VM_FLAG_WRITE) {
      flags |= PTE_W; // Set the write permission if VM_FLAG_WRITE is set
    }
  } else if (!(pte_val & PTE_V)) {
    return -1;
  }

  if (vma->flags & VM_FLAG_READ) {
    flags |= PTE_R; // Set the read permission if VM_FLAG_READ is set
  }
  if (vma->flags & VM_FLAG_EXEC) {
    flags |= PTE_X; // Set the execute permission if VM_FLAG_EXEC is set
  }
  if (vma->flags & VM_FLAG_USERMAP) {
    flags |= PTE_U; // Set the user permission if VM_FLAG_USERMAP is set
  }
  /*
   * FIX: Must set PTE_V (valid bit) when allocating new pages for demand paging.
   * Without this, the hardware considers the PTE invalid even though we allocated
   * a physical page and set other permission bits. This caused repeated page faults
   * on BSS section access (e.g., static variables) because the MMU would not
   * recognize the mapping as valid.
   * 
   * Note: __vma_validate_pte_rxw already sets PTE_V via "flags |= PTE_V | PTE_W",
   * but this function was missing it for read-only/execute pages.
   */
  flags |= PTE_V; // Set the valid bit
  *pte = PA2PTE(pa) | flags; // Update the PTE with the new address and flags

  // Flush TLB so faulting hart sees the new private writable mapping (COW fix).
  sfence_vma();

  return 0;
}

static int __vma_validate_pte(vma_t *vma, pte_t *pte, uint64 flags)
{
  bool pte_user = (*pte & PTE_U) != 0;
  bool vma_user = (flags & VM_FLAG_USERMAP) != 0;

  if (*pte != 0 && (pte_user ^ vma_user)) {
    return -1; // User access permissions do not match
  }

  // @TODO: handle file-backed pages in all the three situations
  assert(vma->file == NULL, "vma_validate_pte: file-backed pages not supported yet");

  if ((flags & VM_FLAG_WRITE) && __vma_validate_pte_rxw(vma, pte) != 0) {
    return -1; // PTE validation failed for writable page
  } else if ((flags & (VM_FLAG_READ | VM_FLAG_EXEC)) && __vma_validate_pte_rx(vma, pte) != 0) {
    return -1; // PTE validation failed for readable page
  }
  return 0;
}

int vma_validate(vma_t *vma, uint64 va, uint64 size, uint64 flags)
{
  if (flags == VM_FLAG_NONE) {
    return -1;
  }
  if (vma == NULL || vma->vm == NULL || vma->vm->pagetable == NULL) {
    return -1; // Invalid vm
  }
  if (flags & ~VM_FLAG_PROT_MASK) {
    return -1; // Invalid flags
  }
  if ((flags & VM_FLAG_EXEC)) {
    if ((flags & VM_FLAG_READ) == 0) {
      return -1; // If executable, must also be readable
    }
    if ((flags & VM_FLAG_WRITE) != 0 && (flags & VM_FLAG_USERMAP) != 0) {
      return -1; // User executable pages cannot be writable
    }
  }
  uint64 va_end = va + size;
  va = PGROUNDDOWN(va);
  if (size == 0) {
    va_end = vma->end; // If size is 0, use the end of the VMA
  } else {
    va_end = PGROUNDUP(va_end);
  }
  if (va < vma->start || va_end > vma->end) {
    return -1; // va is not in the range of the VMA
  }
  if ((flags & vma->flags) != flags) {
    return -1; // Flags do not match
  }
  if (vma->file != NULL && (flags & VM_FLAG_FWRITE)) {
    return -1; // File-backed VMA cannot be writable
  }

  for(uint64 i = va; i < va_end; i += PGSIZE) {
    pte_t *pte = walk(vma->vm->pagetable, va, 1, NULL, NULL);
    assert(pte != NULL, "vma_validate: walk failed for va %lx", va);
    if (__vma_validate_pte(vma, pte, flags) != 0) {
      return -1; // PTE validation failed
    }
  }

  return 0;
}

uint64 vm2pte_flags(uint64 flags)
{
  uint64 pte_flags = 0;
  if (flags & VM_FLAG_READ) {
    pte_flags |= PTE_R;
  }
  if (flags & VM_FLAG_WRITE) {
    pte_flags |= PTE_W;
  }
  if (flags & VM_FLAG_EXEC) {
    pte_flags |= PTE_X;
  }
  if (flags & VM_FLAG_USERMAP) {
    pte_flags |= PTE_U;
  }
  return pte_flags;
}

uint64 pte2vm_flags(uint64 pte_flags)
{
  uint64 flags = 0;
  if (pte_flags & PTE_R) {
    flags |= VM_FLAG_READ;
  }
  if (pte_flags & PTE_W) {
    flags |= VM_FLAG_WRITE;
  }
  if (pte_flags & PTE_X) {
    flags |= VM_FLAG_EXEC;
  }
  if (pte_flags & PTE_U) {
    flags |= VM_FLAG_USERMAP;
  }
  return flags;
}

int vm_createheap(vm_t *vm, uint64 va, uint64 size)
{
  size = PGROUNDUP(size);
  if ((va & (PGSIZE - 1)) != 0) {
    return -1; // va must be page-aligned
  }
  if (va >= UVMTOP || va + size > UVMTOP) {
    return -1; // Invalid heap address
  }
  vma_t *vma = va_alloc(vm, va, size, VM_FLAG_READ | VM_FLAG_WRITE | VM_FLAG_USERMAP | VM_FLAG_GROWSUP);
  if (vma == NULL) {
    return -1; // Allocation failed
  }
  vm->heap = vma; // Set the heap VMA
  vm->heap_size = size; // Set the heap size
  return 0; // Success
}

int vm_createstack(vm_t *vm, uint64 stack_top, uint64 size)
{
  size = PGROUNDUP(size);
  if ((stack_top & (PGSIZE - 1)) != 0) {
    return -1; // stack_top must be page-aligned
  }
  if (stack_top < size || stack_top > UVMTOP) {
    return -1; // Invalid stack address
  }
  vma_t *vma = va_alloc(vm, stack_top - size, size, VM_FLAG_READ | VM_FLAG_WRITE | VM_FLAG_USERMAP | VM_FLAG_GROWSDOWN);
  if (vma == NULL) {
    return -1; // Allocation failed
  }
  vm->stack = vma; // Set the stack VMA
  vm->stack_size = size; // Set the stack size
  return 0; // Success
}

int vm_growstack(vm_t *vm, int change_size)
{
  if (vm == NULL || vm->pagetable == NULL) {
    return -1; // Invalid VM
  }
  if (vm->stack == NULL || vm->stack_size < PGSIZE) {
    return -1; // No stack VMA found
  }
  if ((vm->stack->flags & VM_FLAG_GROWSDOWN) == 0) {
    return -1; // Stack VMA is not growable
  }
  if (change_size == 0) {
    return 0; // No change in stack size
  }
  
  if (change_size < 0 && (uint64)(-change_size) > vm->stack_size - PGSIZE) {
    return -1; // Cannot shrink stack beyond current size
  } else if ((uint64)change_size > (MAXUSTACK << PGSHIFT) - vm->stack_size) {
    return -1; // Cannot grow stack beyond maximum size
  }
  
  uint64 new_size = vm->stack_size + change_size;
  if (new_size < PGSIZE || new_size > (MAXUSTACK << PGSHIFT)) {
    return -1; // Invalid new stack size
  }

  int64 delta = PGROUNDUP(new_size) - PGROUNDUP(vm->stack_size);
  if (delta == 0) {
    vm->stack_size = new_size; // Update the stack size
    return 0; // No change in stack size
  }
  vma_t *left = __get_vma_left(vm->stack);
  uint64 new_start = vm->stack->start - delta;

  if (delta < 0) {
    // Shrinking the stack
    vma_t *splitted = vm->stack;
    vma_t *right = vma_split(vm->stack, new_start);
    assert(right != NULL, "vm_growstack: vma_split failed in shrinking stack");
    vm->stack = right; // Update the stack VMA
    __vma_set_free(splitted); // Set the VMA as free
    if (left != NULL && left->flags == VM_FLAG_NONE) {
      assert(vma_merge(splitted, left) != NULL, 
             "vm_growstack: vma_merge failed in shrinking stack");
    }
  } else {
    if (left == NULL || left->flags != VM_FLAG_NONE) {
      return -1; // No adjacent free VMA to grow the stack
    }
    if (VMA_SIZE(left) < delta) {
      return -1; // Not enough space in the free VMA to grow the stack
    }
    vma_t *grows = vma_split(left, new_start);
    if (grows == NULL) {
      return -1; // Failed to split the free VMA
    }
    list_entry_detach(&grows->free_list_entry);
    grows->flags = vm->stack->flags; // Set the flags for the new VMA
    vma_t *new_stack = vma_merge(grows, vm->stack);
    assert(new_stack == grows, "vm_growstack: vma_merge failed");
    vm->stack = new_stack; // Update the stack VMA
  }
  vm->stack_size = new_size; // Update the stack size

  return 0; // Success
}

int vm_growheap(vm_t *vm, int change_size)
{
  if (vm == NULL || vm->pagetable == NULL) {
    return -1; // Invalid VM
  }
  if (vm->heap == NULL || vm->heap_size < PGSIZE) {
    return -1; // No heap VMA found
  }
  if ((vm->heap->flags & VM_FLAG_GROWSUP) == 0) {
    return -1; // Heap VMA is not growable
  }
  if (change_size == 0) {
    return 0; // No change in heap size
  }

  if (change_size < 0) {
    if ((uint64)(-change_size) > vm->heap_size - PGSIZE) {
      return -1; // Cannot shrink heap beyond current size
    }
  } else if (change_size > UHEAP_MAX_TOP - vm->heap->end) {
    return -1; // Cannot grow heap beyond maximum size
  }

  uint64 new_size = vm->heap_size + change_size;
  int64 delta = PGROUNDUP(new_size) - VMA_SIZE(vm->heap);
  if (delta == 0) {
    vm->heap_size = new_size; // Update the heap size
    return 0; // No change in heap size
  }
  uint64 new_end = vm->heap->end + delta;
  vma_t *right = __get_vma_right(vm->heap);

  if (delta < 0) {
    // Shrinking the heap
    vma_t *splitted = vma_split(vm->heap, new_end);
    assert(splitted != NULL, "vm_growheap: vma_split failed in shrinking heap");
    __vma_set_free(splitted); // Set the VMA as free
    if (right != NULL && right->flags == VM_FLAG_NONE) {
      assert(vma_merge(splitted, right) != NULL, 
             "vm_growheap: vma_merge failed in shrinking heap");
    }
  } else {
    if (right == NULL || right->flags != VM_FLAG_NONE) {
      return -1; // No adjacent free VMA to grow the heap
    }
    if (VMA_SIZE(right) < delta) {
      return -1; // Not enough space in the free VMA to grow the heap
    }
    if (vma_split(right, new_end) == NULL) {
      return -1; // Failed to split the free VMA
    }
    list_entry_detach(&right->free_list_entry);
    right->flags = vm->heap->flags; // Set the flags for the new VMA
    vma_t *new_heap = vma_merge(right, vm->heap);
    assert(new_heap == vm->heap, "vm_growheap: vma_merge failed");
    vm->heap = new_heap; // Update the heap VMA
  }
  
  vm->heap_size = new_size; // Update the heap size
  return 0; // Success
}

int vma_mmap(vm_t *vm, uint64 start, size_t size, uint64 flags, void *file, uint64 pgoff, void *pa)
{
  if (vm == NULL || vm->pagetable == NULL) {
    return -1; // Invalid VM
  }
  uint64 va_end = PGROUNDUP(start + size);
  start = PGROUNDDOWN(start);
  if (va_end <= start || start < UVMBOTTOM || va_end > UVMTOP) {
    return -1; // Invalid address range
  }
  size = va_end - start; // Ensure size is page-aligned

  // @TODO: file no supported
  if (file != NULL && (flags & VM_FLAG_FWRITE)) {
    return -1; // File-backed VMA cannot be writable
  }

  vma_t *vma = va_alloc(vm, start, size, flags);
  if (vma == NULL) {
    return -1; // Allocation failed
  }
  if (pa != NULL) {
    pte_t pte_flags = vm2pte_flags(flags);
    if (mappages(vm->pagetable, vma->start, size, (uint64)pa, pte_flags) != 0) {
      assert(va_free(vma) == 0, "vma_mmap: failed to free vma"); // Free the VMA if mapping fails
      return -1; // Mapping failed
    }
  }

  return 0; // Success
}

int vma_munmap(vm_t *vm, uint64 start, size_t size)
{
  if (vm == NULL || vm->pagetable == NULL) {
    return -1; // Invalid VM
  }
  if (start < UVMBOTTOM || (start + size) > UVMTOP) {
    return -1; // Invalid address range
  }
  if ((size & (PGSIZE - 1)) != 0 || (start & (PGSIZE - 1)) != 0) {
    return -1; // Size and va must be page-aligned and non-zero
  }
  if (size == 0) {
    return 0; // Nothing to unmap
  }

  vma_t *vma = vm_find_area(vm, start);
  if (vma == NULL || vma->start != start || vma->end < start + size) {
    return -1; // No VMA found or VMA does not cover the range
  }

  if (va_free(vma) != 0) {
    return -1; // Failed to free the VMA
  }

  return 0; // Success
}

int vm_dump_flags(uint64 flags, char *buf, size_t buf_size)
{
  if (buf == NULL) {
    return -1; // Invalid buffer
  }
  if (buf_size < 5) {
    return -1; // Buffer too small to hold any flags
  }
  size_t len = 0;
  if (flags & VM_FLAG_READ) {
    buf[len++] = 'R';
  } else {
    buf[len++] = ' '; // Use '-' for no read permission
  }
  if (flags & VM_FLAG_WRITE) {
    buf[len++] = 'W';
  } else {
    buf[len++] = ' '; // Use '-' for no write permission
  }
  if (flags & VM_FLAG_EXEC) {
    buf[len++] = 'X';
  } else {
    buf[len++] = ' '; // Use '-' for no execute permission
  }
  if (flags & VM_FLAG_USERMAP) {
    buf[len++] = 'U';
  } else {
    buf[len++] = ' '; // Use '-' for no user map permission
  }
  buf[len] = '\0'; // Null-terminate the string
  return len; // Return the length of the flags string
}

void dump_vm(vm_t *vm)
{
  if (vm == NULL) {
    return; // Nothing to dump
  }
  printf("VM dump:\n");
  printf("Valid: %d\n", vm->valid);
  printf("Pagetable: %p\n", vm->pagetable);
  printf("VMAs:\n");
  vma_t *vma, *tmp;
  list_foreach_node_safe(&vm->vm_list, vma, tmp, list_entry) {
    char flags_buf[10] = { 0 };
    vm_dump_flags(vma->flags, flags_buf, sizeof(flags_buf));
    printf("VMA: start=%lx, end=%lx, flags=%s, file=%p, pgoff=%lx\n",
           vma->start, vma->end, flags_buf, vma->file, vma->pgoff);
  }
}

int vm_try_growstack(vm_t *vm, uint64 va)
{
  if (vm == NULL || vm->pagetable == NULL) {
    return -1; // Invalid VM
  }
  if (va < USTACK_MAX_BOTTOM || va >= USTACKTOP) {
    // Probably not a stack address, do regular validation
    return 0;
  }

  // Check if the stack can be grown
  if (vm->stack == NULL) {
    return -1; // No stack VMA found
  }

  if (vm->stack->start <= va) {
    return 0; // Stack already covers the address, no need to grow
  }

  // @TODO: potentially overflow
  uint64 ustack_bottom_after = vm->stack->start - (USERSTACK_GROWTH << PAGE_SHIFT);
  if (ustack_bottom_after < USTACK_MAX_BOTTOM) {
    return -1; // Cannot grow the stack below the minimum stack size
  }
  if (ustack_bottom_after > va) {
    // Too far below the stack to grow
    return -1; // Cannot grow the stack to cover the address
  }

  // Grow the stack
  return vm_growstack(vm, USERSTACK_GROWTH << PAGE_SHIFT); // Grow the stack by one page
}
