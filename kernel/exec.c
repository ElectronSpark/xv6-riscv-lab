/*
 * exec.c - Program execution
 *
 * Loads and executes ELF binaries using VFS file operations.
 * This replaces the legacy xv6 fs-based implementation with VFS interfaces.
 */

#include "types.h"
#include "string.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "mutex_types.h"
#include "proc/proc.h"
#include "defs.h"
#include "printf.h"
#include "elf.h"
#include "vm.h"
#include "errno.h"
#include "vfs/fs.h"
#include "vfs/file.h"
#include "vfs/fcntl.h"

STATIC int loadseg(pagetable_t pagetable, uint64 va, struct vfs_file *file,
                   uint offset, uint sz, uint64 pteflags);

int flags2perm(int flags) {
    int perm = 0;
    if (flags & 0x1)
        perm = PTE_X;
    if (flags & 0x2)
        perm |= PTE_W;
    if (flags & 0x4)
        perm |= PTE_R;
    return perm;
}

int flags2vmperm(int flags) {
    int perm = 0;
    if (flags & 0x1)
        perm = VM_FLAG_EXEC;
    if (flags & 0x2)
        perm |= VM_FLAG_WRITE;
    if (flags & 0x4)
        perm |= VM_FLAG_READ;
    return perm;
}

int ustack_alloc(vm_t *vm, uint64 *sp) {
    uint64 ret_sp = USTACKTOP;
    uint64 stackbase = USTACKTOP - USERSTACK * PGSIZE;
    if (va_alloc(vm, stackbase, USERSTACK * PGSIZE,
                 VM_FLAG_USERMAP | VM_FLAG_WRITE | VM_FLAG_READ |
                     VM_FLAG_GROWSDOWN) == NULL) {
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
        *pte = PA2PTE(pa) | PTE_V | PTE_U | PTE_W |
               PTE_R; // Allocate and map the page
    }
    *sp = ret_sp; // Set the stack pointer
    return 0;     // Success
}

int exec(char *path, char **argv) {
    char *s, *last;
    int i, off;
    uint64 argc, heap_start = 0, sp, ustack[MAXARG];
    uint64 stackbase = USTACKTOP - USERSTACK * PGSIZE;
    struct elfhdr elf;
    struct vfs_file *file = NULL;
    struct proghdr ph;
    vm_t *tmp_vm = NULL;
    struct proc *p = myproc();

    // Look up the file using VFS
    struct vfs_inode *inode = vfs_namei(path, strlen(path));
    if (IS_ERR_OR_NULL(inode) && path[0] != '/') {
        // In case of failing to find the inode in cwd, try absolute path
        size_t path_len = strlen(path);
        char *path1 = kmm_alloc(path_len + 2);
        if (path1 == NULL) {
            return -1;
        }
        path1[0] = '/';
        memmove(path1 + 1, path, path_len + 1);
        inode = vfs_namei(path1, path_len + 1);
        kmm_free(path1);
        if (IS_ERR_OR_NULL(inode)) {
            return -1;
        }
    }

    // Open the file for reading
    file = vfs_fileopen(inode, O_RDONLY);
    vfs_iput(inode); // vfs_fileopen takes its own reference
    inode = NULL;

    if (IS_ERR(file)) {
        return -1;
    }
    if (file == NULL) {
        return -1;
    }

    // Read ELF header
    ssize_t n = vfs_fileread(file, &elf, sizeof(elf));
    if (n != sizeof(elf))
        goto bad;

    if (elf.magic != ELF_MAGIC)
        goto bad;

    if ((tmp_vm = vm_init((uint64)p->trapframe)) == NULL) {
        goto bad;
    }

    // Because by this time no one else can see tmp_vm, we don't need to worry about
    // lock contention. But we still need to hold write lock to supress assertions.
    vm_wlock(tmp_vm);

    // Load program into memory.
    for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
        // Seek to program header
        if (vfs_filelseek(file, off, SEEK_SET) != off)
            goto bad_locked;

        // Read program header
        if (vfs_fileread(file, &ph, sizeof(ph)) != sizeof(ph))
            goto bad_locked;

        if (ph.type != ELF_PROG_LOAD)
            continue;
        if (ph.memsz < ph.filesz)
            goto bad_locked;
        if (ph.vaddr + ph.memsz < ph.vaddr)
            goto bad_locked;
        if (ph.vaddr % PGSIZE != 0)
            goto bad_locked;

        vma_t *vma = va_alloc(tmp_vm, ph.vaddr, ph.memsz,
                              flags2vmperm(ph.flags) | VM_FLAG_USERMAP);
        if (vma == NULL) {
            goto bad_locked; // Allocation failed
        }

        // Track the end of loaded segments for heap start
        uint64 size1 = ph.vaddr + ph.memsz;
        if (heap_start < size1) {
            heap_start = size1;
        }

        if (loadseg(tmp_vm->pagetable, ph.vaddr, file, ph.off, ph.filesz,
                    flags2perm(ph.flags)) < 0)
            goto bad_locked;
    }

    // Done with the file
    vfs_fput(file);
    file = NULL;

    p = myproc();

    // Allocate some pages at the next page boundary.
    // Make the first inaccessible as a stack guard.
    // Use the rest as the user stack.
    // Create heap area and reserve one page for heap space.
    if (vm_createheap(tmp_vm, heap_start, USERSTACK * PGSIZE) != 0) {
        goto bad_locked; // Heap allocation failed
    }
    if (vm_createstack(tmp_vm, USTACKTOP, USERSTACK * PGSIZE) != 0) {
        goto bad_locked; // Stack allocation failed
    }
    vm_wunlock(tmp_vm);
    sp = USTACKTOP;

    // Push argument strings, prepare rest of stack in ustack.
    for (argc = 0; argv[argc]; argc++) {
        if (argc >= MAXARG)
            goto bad;
        sp -= strlen(argv[argc]) + 1;
        sp -= sp % 16; // riscv sp must be 16-byte aligned
        if (sp < stackbase)
            goto bad;
        if (vm_copyout(tmp_vm, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
            goto bad;
        ustack[argc] = sp;
    }
    ustack[argc] = 0;

    // push the array of argv[] pointers.
    sp -= (argc + 1) * sizeof(uint64);
    sp -= sp % 16;
    if (sp < stackbase)
        goto bad;
    if (vm_copyout(tmp_vm, sp, (char *)ustack, (argc + 1) * sizeof(uint64)) < 0)
        goto bad;

    // arguments to user main(argc, argv)
    // argc is returned via the system call return
    // value, which goes in a0.
    p->trapframe->trapframe.a1 = sp;

    // Save program name for debugging.
    for (last = s = path; *s; s++)
        if (*s == '/')
            last = s + 1;
    safestrcpy(p->name, last, sizeof(p->name));

    // Commit to the user image.
    vm_put(p->vm);                        // Destroy the old VM
    p->vm = NULL;
    p->vm = tmp_vm;                           // Set the new VM
    p->trapframe->trapframe.sepc = elf.entry; // initial program counter = main
    p->trapframe->trapframe.sp = sp;          // initial stack pointer
    return argc; // this ends up in a0, the first argument to main(argc, argv)

bad_locked:
    vm_wunlock(tmp_vm);
bad:
    vm_put(tmp_vm); // Clean up the temporary VM
    tmp_vm = NULL;
    if (file) {
        vfs_fput(file);
    }
    return -1;
}

/*
 * Load a program segment into pagetable at virtual address va.
 * va must be page-aligned and the pages from va to va+sz must already be
 * mapped. Uses VFS file operations instead of legacy inode readi(). Returns 0
 * on success, -1 on failure.
 */
STATIC int loadseg(pagetable_t pagetable, uint64 va, struct vfs_file *file,
                   uint offset, uint sz, uint64 pteflags) {
    uint i, n;
    uint64 pa;

    for (i = 0; i < sz; i += PGSIZE) {
        // Allocate a physical page
        pa = (uint64)kalloc();
        if (pa == 0)
            return -1; // kalloc failed

        // Calculate how many bytes to read for this page
        if (sz - i < PGSIZE) {
            n = sz - i;
            memset((void *)(pa + n), 0,
                   PGSIZE - n); // Zero the rest of the page
        } else {
            n = PGSIZE;
        }

        // Seek to the file offset for this segment portion
        if (vfs_filelseek(file, offset + i, SEEK_SET) != (loff_t)(offset + i)) {
            kfree((void *)pa);
            return -1;
        }

        // Read directly into the physical page (kernel address)
        ssize_t bytes_read = vfs_fileread(file, (void *)pa, n);
        if (bytes_read != (ssize_t)n) {
            kfree((void *)pa);
            return -1;
        }

        // Map the page into the process's page table
        if (mappages(pagetable, va + i, PGSIZE, pa, pteflags | PTE_U | PTE_V) !=
            0) {
            kfree((void *)pa);
            return -1; // mappages failed
        }
    }

    return 0;
}

/*
 * System call handler for exec.
 * Parses user arguments and calls exec().
 */
uint64 sys_exec(void) {
    char path[MAXPATH], *argv[MAXARG];
    int i;
    uint64 uargv, uarg;

    argaddr(1, &uargv);
    if (argstr(0, path, MAXPATH) < 0) {
        return -1;
    }
    memset(argv, 0, sizeof(argv));
    for (i = 0;; i++) {
        if (i >= NELEM(argv)) {
            goto bad;
        }
        if (fetchaddr(uargv + sizeof(uint64) * i, (uint64 *)&uarg) < 0) {
            goto bad;
        }
        if (uarg == 0) {
            argv[i] = 0;
            break;
        }
        argv[i] = kalloc();
        if (argv[i] == 0)
            goto bad;
        if (fetchstr(uarg, argv[i], PGSIZE) < 0)
            goto bad;
    }

    int ret = exec(path, argv);

    for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
        kfree(argv[i]);

    return ret;

bad:
    for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
        kfree(argv[i]);
    return -1;
}
