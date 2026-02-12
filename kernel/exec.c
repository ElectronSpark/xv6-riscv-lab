/*
 * exec.c - Program execution
 *
 * Loads and executes ELF binaries using file-backed mmap for demand paging.
 * ELF LOAD segment data is loaded lazily from the page cache on first access
 * rather than being eagerly read into memory at exec time.
 */

#include "types.h"
#include "string.h"
#include "param.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "lock/spinlock.h"
#include "lock/mutex_types.h"
#include "proc/thread.h"
#include "defs.h"
#include "printf.h"
#include "elf.h"
#include <mm/vm.h>
#include "errno.h"
#include "vfs/fs.h"
#include "vfs/file.h"
#include "vfs/fcntl.h"

int flags2vmperm(int flags) {
    int perm = 0;
    if (flags & 0x1)
        perm = PROT_EXEC;
    if (flags & 0x2)
        perm |= PROT_WRITE;
    if (flags & 0x4)
        perm |= PROT_READ;
    return perm;
}

int ustack_alloc(vm_t *vm, uint64 *sp) {
    uint64 ret_sp = USTACKTOP;
    uint64 stackbase = USTACKTOP - USERSTACK * PGSIZE;
    if (vma_alloc(vm, stackbase, USERSTACK * PGSIZE,
                  VMA_FLAG_USER | PROT_WRITE | PROT_READ |
                      VMA_FLAG_GROWSDOWN) == NULL) {
        return -1; // Allocation failed
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
    struct thread *p = current;

    // Look up the file using VFS
    struct vfs_inode *inode = vfs_namei(path, strlen(path));
    if (IS_ERR_OR_NULL(inode) && strncmp(path, "/bin/", 5) != 0) {
        // In case of failing to find the inode in cwd, try absolute path
        size_t path_len = strlen(path);
        char *path1 = kmm_alloc(path_len + 5);
        if (path1 == NULL) {
            return -1;
        }
        strncpy(path1, "/bin/", 5);
        memmove(path1 + 5, path, path_len + 1);
        inode = vfs_namei(path1, path_len + 5);
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
    ssize_t n = vfs_fileread(file, &elf, sizeof(elf), false);
    if (n != sizeof(elf))
        goto bad;

    if (elf.magic != ELF_MAGIC)
        goto bad;

    if ((tmp_vm = vm_init()) == NULL) {
        goto bad;
    }

    // Because by this time no one else can see tmp_vm, we don't need to worry
    // about lock contention. But we still need to hold write lock to suppress
    // assertions in vma_alloc() called by vm_mmap_region_locked().
    vm_wlock(tmp_vm);

    // Load program into memory using mmap for lazy demand paging.
    //
    // For each LOAD segment we create up to three regions:
    //   1. File-backed mmap  [vaddr, file_pg_end)       – demand-paged from
    //   pcache
    //   2. Boundary page     [file_pg_end, file_pg_end+PGSIZE) – eagerly
    //   populated
    //   3. Anonymous mmap    [anon_start, total_end)     – lazy zero-fill (BSS)
    //
    // Linux's load_elf_binary() follows the same pattern: elf_map() for the
    // file portion, padzero() for the partial page, set_brk() for BSS.
    for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
        // Seek to program header
        if (vfs_filelseek(file, off, SEEK_SET) != off)
            goto bad_locked;

        // Read program header
        if (vfs_fileread(file, &ph, sizeof(ph), false) != sizeof(ph))
            goto bad_locked;

        if (ph.type != ELF_PROG_LOAD)
            continue;
        if (ph.memsz < ph.filesz)
            goto bad_locked;
        if (ph.vaddr + ph.memsz < ph.vaddr)
            goto bad_locked;
        if (ph.vaddr % PGSIZE != 0)
            goto bad_locked;
        if (ph.filesz > 0 && ph.off % PGSIZE != 0)
            goto bad_locked;

        uint64 va = ph.vaddr;
        uint64 filesz = ph.filesz;
        uint64 memsz = ph.memsz;
        uint64 vm_flags = flags2vmperm(ph.flags) | VMA_FLAG_USER;
        uint64 total_end = PGROUNDUP(va + memsz);
        // End of full pages entirely covered by file data
        uint64 file_pg_end = (filesz > 0) ? PGROUNDDOWN(va + filesz) : va;
        int ret;

        // Region 1: file-backed mmap – pages are demand-paged from the
        // file's page cache on the first instruction/load/store fault.
        if (file_pg_end > va) {
            ret = vm_mmap_region_locked(tmp_vm, va, file_pg_end - va,
                                        vm_flags | VMA_FLAG_FILE, file, ph.off,
                                        NULL);
            if (ret != 0)
                goto bad_locked;
        }

        // Has a partial (boundary) page at the end of the file data?
        int has_boundary = (filesz > 0 && (filesz & (PGSIZE - 1)) != 0);
        uint64 anon_start = has_boundary ? file_pg_end + PGSIZE : file_pg_end;

        // Region 2: boundary page – the single page that straddles the
        // file-data / BSS boundary.  We allocate it eagerly, copy the
        // partial file data, and zero-fill the rest (same as Linux padzero).
        if (has_boundary) {
            uint32 nbytes = (uint32)((va + filesz) - file_pg_end);
            void *pa = kalloc();
            if (pa == NULL)
                goto bad_locked;
            memset(pa, 0, PGSIZE);

            loff_t foff = (loff_t)(ph.off + (file_pg_end - va));
            if (vfs_filelseek(file, foff, SEEK_SET) != foff) {
                kfree(pa);
                goto bad_locked;
            }
            if (vfs_fileread(file, pa, nbytes, false) != (ssize_t)nbytes) {
                kfree(pa);
                goto bad_locked;
            }

            ret = vm_mmap_region_locked(tmp_vm, file_pg_end, PGSIZE, vm_flags,
                                        NULL, 0, pa);
            if (ret != 0) {
                kfree(pa);
                goto bad_locked;
            }
        }

        // Region 3: anonymous mmap for the remaining BSS pages.
        // Faulted in lazily as zero-filled pages.
        if (total_end > anon_start) {
            ret = vm_mmap_region_locked(tmp_vm, anon_start,
                                        total_end - anon_start, vm_flags, NULL,
                                        0, NULL);
            if (ret != 0)
                goto bad_locked;
        }

        // Track the end of loaded segments for heap start
        uint64 size1 = ph.vaddr + ph.memsz;
        if (heap_start < size1) {
            heap_start = size1;
        }
    }

    // Done with the file
    vfs_fput(file);
    file = NULL;

    p = current;

    // Create heap via mmap (grows-up, demand-paged zero-fill).
    uint64 heap_sz = PGROUNDUP(USERSTACK * PGSIZE);
    heap_start = PGROUNDUP(heap_start);
    if (vm_mmap_region_locked(tmp_vm, heap_start, heap_sz,
                              PROT_READ | PROT_WRITE | VMA_FLAG_USER |
                                  VMA_FLAG_GROWSUP,
                              NULL, 0, NULL) != 0) {
        goto bad_locked;
    }
    tmp_vm->heap = vm_find_area(tmp_vm, heap_start);
    tmp_vm->heap_size = heap_sz;

    // Create user stack via mmap (grows-down, demand-paged zero-fill).
    uint64 stack_sz = PGROUNDUP(USERSTACK * PGSIZE);
    if (vm_mmap_region_locked(tmp_vm, USTACKTOP - stack_sz, stack_sz,
                              PROT_READ | PROT_WRITE | VMA_FLAG_USER |
                                  VMA_FLAG_GROWSDOWN,
                              NULL, 0, NULL) != 0) {
        goto bad_locked;
    }
    tmp_vm->stack = vm_find_area(tmp_vm, USTACKTOP - stack_sz);
    tmp_vm->stack_size = stack_sz;

    vm_wunlock(tmp_vm);
    sp = USTACKTOP;

    // Preload pages near the entry point to avoid initial page faults.
    // This is similar to Linux's fault-ahead in load_elf_binary().
    {
#define EXEC_PRELOAD_PAGES 4
        uint64 entry_start = PGROUNDDOWN(elf.entry);
        uint64 preload_size = EXEC_PRELOAD_PAGES * PGSIZE;

        vm_rlock(tmp_vm);
        vma_t *entry_vma = vm_find_area(tmp_vm, entry_start);
        if (entry_vma != NULL) {
            // Clamp to VMA bounds
            uint64 preload_end = entry_start + preload_size;
            if (preload_end > entry_vma->end)
                preload_end = entry_vma->end;
            vma_validate(entry_vma, entry_start, preload_end - entry_start,
                         PROT_READ | VMA_FLAG_USER);
        }
        vm_runlock(tmp_vm);
    }

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
    vm_put(p->vm); // Destroy the old VM
    p->vm = NULL;
    p->vm = tmp_vm;                           // Set the new VM
    p->trapframe->trapframe.sepc = elf.entry; // initial program counter = main
    p->trapframe->trapframe.sp = sp;          // initial stack pointer

    // Wake vfork parent - we've replaced our address space so parent can resume
    vfork_done(p);

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
