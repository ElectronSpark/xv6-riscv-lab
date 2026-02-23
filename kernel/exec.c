/*
 * exec.c - Program execution with dynamic linking support
 *
 * Loads and executes ELF binaries using file-backed mmap for demand paging.
 * ELF LOAD segment data is loaded lazily from the page cache on first access
 * rather than being eagerly read into memory at exec time.
 *
 * Supports:
 *   - ET_EXEC (statically linked executables)
 *   - ET_DYN  (position-independent executables / shared libraries)
 *   - PT_INTERP (dynamic linker / interpreter loading)
 *   - Auxiliary vector (auxv) for ELF loader communication
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
#include "signal.h"
#include "kqueue_types.h"

/* Enable verbose exec debugging — set to 1 to trace ELF loading steps */
#define EXEC_DEBUG 0

#if EXEC_DEBUG
#define exec_dbg(fmt, ...) printf("exec: " fmt, ##__VA_ARGS__)
#else
#define exec_dbg(fmt, ...) ((void)0)
#endif

/*
 * Base address for loading ET_DYN executables (PIE).
 * Chosen to be well above typical text addresses but below the stack.
 * The interpreter is loaded above this.
 */
#define ELF_ET_DYN_BASE  0x40000000UL
#define ELF_INTERP_BASE  0x70000000UL

/* Maximum number of auxiliary vector entries */
#define AT_VECTOR_SIZE 20

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

/*
 * load_elf_segments - Load PT_LOAD segments from an ELF file into a VM.
 *
 * @vm:         Target virtual memory (must be write-locked by caller)
 * @file:       VFS file handle for the ELF binary
 * @elf:        Parsed ELF header
 * @load_bias:  Offset added to each segment's vaddr (0 for ET_EXEC,
 *              nonzero for ET_DYN)
 * @out_brk:    Output: highest mapped address (for heap placement)
 * @out_phdr_addr: Output: virtual address of the program header table
 *                 in memory (for AT_PHDR), or 0 if not mapped.
 *
 * Returns 0 on success, negative errno on failure.
 *
 * For each LOAD segment we create up to three regions:
 *   1. File-backed mmap  [vaddr, file_pg_end)       – demand-paged from pcache
 *   2. Boundary page     [file_pg_end, file_pg_end+PGSIZE) – eagerly populated
 *   3. Anonymous mmap    [anon_start, total_end)     – lazy zero-fill (BSS)
 */
static int load_elf_segments(vm_t *vm, struct vfs_file *file,
                             struct elfhdr *elf, uint64 load_bias,
                             uint64 *out_brk, uint64 *out_phdr_addr) {
    struct proghdr ph;
    int i, off;
    uint64 brk = 0;
    uint64 phdr_addr = 0;

    for (i = 0, off = elf->phoff; i < elf->phnum; i++, off += sizeof(ph)) {
        if (vfs_filelseek(file, off, SEEK_SET) != off)
            return -EIO;
        if (vfs_fileread(file, &ph, sizeof(ph), false) != sizeof(ph))
            return -EIO;

        if (ph.type != ELF_PROG_LOAD)
            continue;
        if (ph.memsz < ph.filesz)
            return -ENOEXEC;
        if (ph.vaddr + ph.memsz < ph.vaddr)
            return -ENOEXEC;

        /* Check if the program headers are within this segment */
        if (elf->phoff >= ph.off &&
            elf->phoff < ph.off + ph.filesz) {
            phdr_addr = ph.vaddr + load_bias + (elf->phoff - ph.off);
        }

        uint64 va = ph.vaddr + load_bias;
        uint64 filesz = ph.filesz;
        uint64 memsz = ph.memsz;
        uint64 file_off = ph.off;
        uint64 vm_flags = flags2vmperm(ph.flags) | VMA_FLAG_USER;

        exec_dbg("LOAD[%d] va=%p filesz=%p memsz=%p off=%p flags=%x\n",
                 i, (void *)va, (void *)filesz, (void *)memsz,
                 (void *)file_off, ph.flags);
        int ret;

        /*
         * Handle non-page-aligned segments.
         * Many shared libraries have segments that are not page-aligned.
         * We align down the vaddr and adjust offsets accordingly.
         */
        uint64 va_page = ELF_PAGESTART(va);
        uint64 va_offset = ELF_PAGEOFFSET(va);
        uint64 file_off_page = file_off - va_offset;
        uint64 filesz_adj = filesz + va_offset;
        uint64 memsz_adj = memsz + va_offset;

        uint64 total_end = ELF_PAGEALIGN(va_page + memsz_adj);
        uint64 file_pg_end =
            (filesz_adj > 0) ? PGROUNDDOWN(va_page + filesz_adj) : va_page;

        exec_dbg("va_page=%p file_pg_end=%p total_end=%p anon_start(pre)=%p\n",
                 (void *)va_page, (void *)file_pg_end, (void *)total_end,
                 (void *)(((filesz_adj > 0 && (filesz_adj & (PGSIZE - 1)) != 0)
                           ? file_pg_end + PGSIZE : file_pg_end)));

        /* Region 1: file-backed mmap */
        if (file_pg_end > va_page) {
            exec_dbg("R1 file-backed [%p, %p)\n",
                     (void *)va_page, (void *)file_pg_end);
            ret = vm_mmap_region_locked(vm, va_page, file_pg_end - va_page,
                                        vm_flags | VMA_FLAG_FILE, file,
                                        file_off_page, NULL);
            if (ret != 0)
                return ret;
        }

        /* Region 2: boundary page (partial file data + zero fill) */
        int has_boundary =
            (filesz_adj > 0 && (filesz_adj & (PGSIZE - 1)) != 0);
        uint64 anon_start = has_boundary ? file_pg_end + PGSIZE : file_pg_end;

        if (has_boundary) {
            exec_dbg("R2 boundary  [%p, %p)\n",
                     (void *)file_pg_end, (void *)(file_pg_end + PGSIZE));
            uint32 nbytes = (uint32)((va_page + filesz_adj) - file_pg_end);
            void *pa = kalloc();
            if (pa == NULL)
                return -ENOMEM;
            memset(pa, 0, PGSIZE);

            loff_t foff = (loff_t)(file_off_page + (file_pg_end - va_page));
            if (vfs_filelseek(file, foff, SEEK_SET) != foff) {
                kfree(pa);
                return -EIO;
            }
            if (vfs_fileread(file, pa, nbytes, false) != (ssize_t)nbytes) {
                kfree(pa);
                return -EIO;
            }

            ret = vm_mmap_region_locked(vm, file_pg_end, PGSIZE, vm_flags,
                                        NULL, 0, pa);
            if (ret != 0) {
                kfree(pa);
                return ret;
            }
        }

        /* Region 3: anonymous BSS pages */
        if (total_end > anon_start) {
            exec_dbg("R3 anon BSS  [%p, %p)\n",
                     (void *)anon_start, (void *)total_end);
            ret = vm_mmap_region_locked(vm, anon_start,
                                        total_end - anon_start, vm_flags, NULL,
                                        0, NULL);
            if (ret != 0)
                return ret;
        }

        /* Track highest mapped address */
        uint64 seg_end = ph.vaddr + load_bias + ph.memsz;
        if (brk < seg_end)
            brk = seg_end;
    }

    if (out_brk)
        *out_brk = brk;
    if (out_phdr_addr)
        *out_phdr_addr = phdr_addr;
    return 0;
}

/*
 * push_auxv - Push a single auxiliary vector entry onto ustack.
 */
static inline void push_auxv(uint64 *ustack, int *idx, uint64 type,
                              uint64 val) {
    ustack[(*idx)++] = type;
    ustack[(*idx)++] = val;
}

int exec(char *path, char **argv, char **envp) {
    char *s, *last;
    int i;
    /*
     * ustack layout (Linux ELF convention):
     *   argc, argv[0..argc-1], NULL, envp[0..envc-1], NULL, auxv[], AT_NULL
     * We size the array generously: MAXARG + MAXENV + auxv pairs + terminators.
     */
    uint64 argc, envc, sp;
    uint64 ustack[MAXARG + MAXENV + AT_VECTOR_SIZE * 2 + 8];
    uint64 stackbase = USTACKTOP - USERSTACK * PGSIZE;
    struct elfhdr elf;
    struct vfs_file *file = NULL;
    struct proghdr ph;
    vm_t *tmp_vm = NULL;
    struct thread *p = current;
    uint64 heap_start = 0;
    uint64 phdr_addr = 0;
    char interp_path[ELF_INTERP_MAXLEN];
    int has_interp = 0;
    uint64 interp_base = 0;   /* load bias of interpreter */
    uint64 interp_entry = 0;  /* entry point of interpreter */
    uint64 interp_ld = 0;     /* loaded address of interpreter's .dynamic */

    exec_dbg("pid %d: exec(\"%s\")\n", current->pid, path);

    // Look up the file using VFS
    struct vfs_inode *inode = vfs_namei(path, strlen(path));
    if (IS_ERR_OR_NULL(inode)) {
        exec_dbg("  FAIL: vfs_namei(\"%s\") failed\n", path);
        return -1;
    }

    // Open the file for reading
    file = vfs_fileopen(inode, O_RDONLY);
    vfs_iput(inode); // vfs_fileopen takes its own reference
    inode = NULL;

    if (IS_ERR(file)) {
        exec_dbg("  FAIL: vfs_fileopen IS_ERR\n");
        return -1;
    }
    if (file == NULL) {
        exec_dbg("  FAIL: vfs_fileopen returned NULL\n");
        return -1;
    }

    // Read ELF header
    ssize_t n = vfs_fileread(file, &elf, sizeof(elf), false);
    if (n != sizeof(elf)) {
        exec_dbg("  FAIL: read ELF header: got %ld bytes\n", n);
        goto bad;
    }

    if (elf.magic != ELF_MAGIC) {
        exec_dbg("  FAIL: bad ELF magic 0x%x\n", elf.magic);
        goto bad;
    }

    /* Only ET_EXEC and ET_DYN are loadable */
    if (elf.type != ET_EXEC && elf.type != ET_DYN) {
        exec_dbg("  FAIL: unsupported ELF type %d\n", elf.type);
        goto bad;
    }
    exec_dbg("  ELF type=%s entry=0x%lx phnum=%d phoff=0x%lx\n",
             elf.type == ET_EXEC ? "EXEC" : "DYN",
             (uint64)elf.entry, (int)elf.phnum, (uint64)elf.phoff);

    /*
     * Scan program headers for PT_INTERP first (before creating VM).
     * If present, we'll need to load the dynamic linker too.
     */
    for (i = 0; i < elf.phnum; i++) {
        int off = elf.phoff + i * sizeof(ph);
        if (vfs_filelseek(file, off, SEEK_SET) != off)
            goto bad;
        if (vfs_fileread(file, &ph, sizeof(ph), false) != sizeof(ph))
            goto bad;
        if (ph.type == ELF_PROG_INTERP) {
            if (ph.filesz >= ELF_INTERP_MAXLEN || ph.filesz < 2) {
                exec_dbg("  FAIL: PT_INTERP filesz=%ld out of range\n", (long)ph.filesz);
                goto bad;
            }
            if (vfs_filelseek(file, ph.off, SEEK_SET) != (loff_t)ph.off)
                goto bad;
            if (vfs_fileread(file, interp_path, ph.filesz, false) !=
                (ssize_t)ph.filesz)
                goto bad;
            interp_path[ph.filesz] = '\0';
            /* Strip trailing newline if any */
            if (interp_path[ph.filesz - 1] == '\n')
                interp_path[ph.filesz - 1] = '\0';
            has_interp = 1;
            exec_dbg("  PT_INTERP: \"%s\"\n", interp_path);
            break;
        }
    }

    if ((tmp_vm = vm_init()) == NULL) {
        goto bad;
    }

    // We hold the write lock on tmp_vm for the duration of segment loading.
    vm_wlock(tmp_vm);

    /*
     * Determine load bias:
     *   - ET_EXEC: load at the addresses specified in the ELF (bias = 0)
     *   - ET_DYN:  load at ELF_ET_DYN_BASE offset (PIE executable)
     */
    uint64 load_bias = 0;
    if (elf.type == ET_DYN) {
        load_bias = ELF_ET_DYN_BASE;
    }

    /* Load the main executable's PT_LOAD segments */
    exec_dbg("  loading main ELF segments (bias=0x%lx)\n", load_bias);
    int ret = load_elf_segments(tmp_vm, file, &elf, load_bias, &heap_start,
                                &phdr_addr);
    if (ret != 0) {
        exec_dbg("  FAIL: load_elf_segments(main) returned %d\n", ret);
        goto bad_locked;
    }
    exec_dbg("  main segments loaded: brk=0x%lx phdr_addr=0x%lx\n",
             heap_start, phdr_addr);

    /*
     * Load the interpreter (dynamic linker) if PT_INTERP was found.
     * The interpreter is always ET_DYN and loaded at ELF_INTERP_BASE.
     */
    struct vfs_file *interp_file = NULL;
    if (has_interp) {
        struct elfhdr interp_elf;

        exec_dbg("  loading interpreter \"%s\"\n", interp_path);
        struct vfs_inode *interp_inode =
            vfs_namei(interp_path, strlen(interp_path));
        if (IS_ERR_OR_NULL(interp_inode)) {
            exec_dbg("  FAIL: interpreter vfs_namei(\"%s\") failed\n", interp_path);
            goto bad_locked;
        }

        interp_file = vfs_fileopen(interp_inode, O_RDONLY);
        vfs_iput(interp_inode);

        if (IS_ERR_OR_NULL(interp_file)) {
            exec_dbg("  FAIL: interpreter vfs_fileopen failed\n");
            goto bad_locked;
        }

        n = vfs_fileread(interp_file, &interp_elf, sizeof(interp_elf), false);
        if (n != sizeof(interp_elf) || interp_elf.magic != ELF_MAGIC ||
            interp_elf.type != ET_DYN) {
            exec_dbg("  FAIL: interpreter ELF validation failed "
                     "(n=%ld magic=0x%x type=%d)\n",
                     n, interp_elf.magic, interp_elf.type);
            vfs_fput(interp_file);
            goto bad_locked;
        }

        interp_base = ELF_INTERP_BASE;
        uint64 interp_brk = 0;
        exec_dbg("  loading interpreter segments at base=0x%lx\n", interp_base);
        ret = load_elf_segments(tmp_vm, interp_file, &interp_elf, interp_base,
                                &interp_brk, NULL);

        if (ret != 0) {
            vfs_fput(interp_file);
            exec_dbg("  FAIL: load_elf_segments(interp) returned %d\n", ret);
            goto bad_locked;
        }

        interp_entry = interp_elf.entry + interp_base;
        exec_dbg("  interpreter loaded: entry=0x%lx brk=0x%lx\n",
                 interp_entry, interp_brk);

        /* Find PT_DYNAMIC in the interpreter so GDB can locate .dynamic */
        interp_ld = 0;
        for (i = 0; i < interp_elf.phnum; i++) {
            int off = interp_elf.phoff + i * sizeof(ph);
            if (vfs_filelseek(interp_file, off, SEEK_SET) != off)
                break;
            if (vfs_fileread(interp_file, &ph, sizeof(ph), false) != sizeof(ph))
                break;
            if (ph.type == ELF_PROG_DYNAMIC) {
                interp_ld = interp_base + ph.vaddr;
                exec_dbg("  interpreter PT_DYNAMIC at 0x%lx\n", interp_ld);
                break;
            }
        }

        vfs_fput(interp_file);
        interp_file = NULL;
    }

    // Done with the file
    vfs_fput(file);
    file = NULL;

    p = current;

    // Create heap via mmap.
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
    tmp_vm->heap_reserve_end = heap_start +
                               (HEAP_RESERVE_PAGES << PAGE_SHIFT);

    // Create user stack via mmap.
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

    // Preload pages near the entry point.
    {
#define EXEC_PRELOAD_PAGES 4
        uint64 entry_addr = has_interp ? interp_entry : (elf.entry + load_bias);
        uint64 entry_start = PGROUNDDOWN(entry_addr);
        uint64 preload_size = EXEC_PRELOAD_PAGES * PGSIZE;

        vm_rlock(tmp_vm);
        vma_t *entry_vma = vm_find_area(tmp_vm, entry_start);
        if (entry_vma != NULL) {
            uint64 preload_end = entry_start + preload_size;
            if (preload_end > entry_vma->end)
                preload_end = entry_vma->end;
            vma_validate(entry_vma, entry_start, preload_end - entry_start,
                         PROT_READ | VMA_FLAG_USER);
        }
        vm_runlock(tmp_vm);
    }

    // Push argument strings onto the stack.
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

    // Push environment strings.
    envc = 0;
    if (envp) {
        for (envc = 0; envp[envc]; envc++) {
            if (envc >= MAXENV)
                goto bad;
            sp -= strlen(envp[envc]) + 1;
            sp -= sp % 16;
            if (sp < stackbase)
                goto bad;
            if (vm_copyout(tmp_vm, sp, envp[envc], strlen(envp[envc]) + 1) < 0)
                goto bad;
            ustack[argc + 2 + envc] = sp;
        }
    }

    /*
     * Build the startup stack frame following the Linux ELF ABI convention
     * expected by musl/glibc crt1 and ld.so:
     *
     *   sp[0]                = argc
     *   sp[1 .. argc]        = argv[0] .. argv[argc-1]
     *   sp[argc+1]           = 0  (NULL terminator for argv)
     *   sp[argc+2 .. +1+envc]= envp[0] .. envp[envc-1]
     *   sp[argc+2+envc]      = 0  (NULL terminator for envp)
     *   sp[...]              = auxv pairs (type, value) ...
     *   sp[...]              = AT_NULL, 0
     */
    for (i = (int)argc - 1; i >= 0; i--) {
        ustack[i + 1] = ustack[i];
    }
    ustack[0] = argc;
    ustack[argc + 1] = 0;
    // envp pointers are already at ustack[argc+2 .. argc+1+envc]
    ustack[argc + 2 + envc] = 0;

    /* Build auxiliary vector right after the envp NULL terminator */
    int aidx = (int)(argc + envc + 3); /* index into ustack[] */

    push_auxv(ustack, &aidx, AT_PAGESZ, PGSIZE);
    push_auxv(ustack, &aidx, AT_PHENT, sizeof(struct proghdr));
    push_auxv(ustack, &aidx, AT_PHNUM, elf.phnum);
    push_auxv(ustack, &aidx, AT_ENTRY, elf.entry + load_bias);
    push_auxv(ustack, &aidx, AT_FLAGS, 0);

    if (phdr_addr != 0) {
        push_auxv(ustack, &aidx, AT_PHDR, phdr_addr);
    }

    if (has_interp) {
        push_auxv(ustack, &aidx, AT_BASE, interp_base);
    } else {
        push_auxv(ustack, &aidx, AT_BASE, 0);
    }

    /* Fake UID/GID — xv6 has no notion of users */
    push_auxv(ustack, &aidx, AT_UID, 0);
    push_auxv(ustack, &aidx, AT_EUID, 0);
    push_auxv(ustack, &aidx, AT_GID, 0);
    push_auxv(ustack, &aidx, AT_EGID, 0);
    push_auxv(ustack, &aidx, AT_SECURE, 0);

    /* AT_NULL terminates the auxv */
    push_auxv(ustack, &aidx, AT_NULL, 0);

    uint64 nslots = (uint64)aidx;
    sp -= nslots * sizeof(uint64);
    sp -= sp % 16;
    if (sp < stackbase)
        goto bad;
    if (vm_copyout(tmp_vm, sp, (char *)ustack, nslots * sizeof(uint64)) < 0)
        goto bad;

    // arguments to user main(argc, argv)
    // argc is returned via the system call return value.
    // argv goes in the second argument register.
#ifdef __x86_64__
    p->trapframe->trapframe.rsi = sp + sizeof(uint64);  /* argv in RSI */
#else
    p->trapframe->trapframe.a1 = sp + sizeof(uint64);    /* argv in a1 */
#endif

    // Save program name for debugging.
    for (last = s = path; *s; s++)
        if (*s == '/')
            last = s + 1;
    safestrcpy(p->name, last, sizeof(p->name));

    /* kqueue: notify EVFILT_PROC watchers of exec */
    kqueue_proc_notify(p, NOTE_EXEC, 0);

    // Commit to the user image.
    vm_put(p->vm); // Destroy the old VM
    p->vm = NULL;
    p->vm = tmp_vm;

    /* Store interpreter info in thread_group for the gdbstub.
     * This lets qXfer:libraries-svr4:read report the dynamic
     * linker/libc to GDB so it can resolve shared library symbols. */
    if (has_interp) {
        p->thread_group->interp_base = interp_base;
        p->thread_group->interp_ld = interp_ld;
        safestrcpy(p->thread_group->interp_path, interp_path,
                   sizeof(p->thread_group->interp_path));
    } else {
        p->thread_group->interp_base = 0;
        p->thread_group->interp_ld = 0;
        p->thread_group->interp_path[0] = '\0';
    }
    safestrcpy(p->thread_group->exec_path, path,
               sizeof(p->thread_group->exec_path));

    /*
     * Entry point:
     *   - If we have an interpreter, jump to the interpreter's entry.
     *     The interpreter will read AT_ENTRY from auxv to find the
     *     actual program entry point.
     *   - Otherwise, jump directly to the executable's entry.
     */
    if (has_interp) {
        p->trapframe->trapframe.sepc = interp_entry;
    } else {
        p->trapframe->trapframe.sepc = elf.entry + load_bias;
    }
    p->trapframe->trapframe.sp = sp;
    exec_dbg("  entry=0x%lx sp=0x%lx heap=0x%lx has_interp=%d\n",
             p->trapframe->trapframe.sepc, sp, heap_start, has_interp);
    exec_dbg("  auxv: AT_ENTRY=0x%lx AT_BASE=0x%lx AT_PHDR=0x%lx AT_PHNUM=%d\n",
             elf.entry + load_bias, has_interp ? interp_base : 0UL,
             phdr_addr, (int)elf.phnum);

    // Reset caught signal handlers to SIG_DFL.
    sigacts_exec(p->sigacts);

    // Close descriptors marked close-on-exec.
    vfs_fdtable_close_on_exec(p->fdtable);

    // Wake vfork parent.
    vfork_done(p);

    // Stop for GDB if being debugged.
    {
        extern void gdbstub_exec_stop(struct thread *, uint64, const char *);
        gdbstub_exec_stop(p, elf.entry + load_bias, path);
    }

    /*
     * Flush the I-cache on ALL harts.  exec() has loaded new executable
     * code (including eagerly-preloaded pages near the entry point).  On
     * real RISC-V hardware the I-cache may contain stale entries from the
     * previous process at the same virtual addresses.  Use SBI remote
     * fence.i so every hart sees fresh code when this process is scheduled.
     */
    vm_remote_fence_i(p->vm);

    return argc; // this ends up in a0, the first argument to main(argc, argv)

bad_locked:
    vm_wunlock(tmp_vm);
bad:
    exec_dbg("  FAILED for \"%s\"\n", path);
    vm_put(tmp_vm);
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
    char path[MAXPATH], *argv[MAXARG], *envp[MAXENV];
    int i;
    uint64 uargv, uarg, uenvp, uenv;

    argaddr(1, &uargv);
    argaddr(2, &uenvp);
    if (argstr(0, path, MAXPATH) < 0) {
        return -1;
    }
    memset(argv, 0, sizeof(argv));
    memset(envp, 0, sizeof(envp));
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

    // Parse envp from userspace (may be NULL or garbage from old 2-arg exec)
    int envc = 0;
    if (uenvp != 0) {
        for (i = 0;; i++) {
            if (i >= NELEM(envp)) {
                break; // too many env vars, just truncate
            }
            if (fetchaddr(uenvp + sizeof(uint64) * i, (uint64 *)&uenv) < 0) {
                break; // invalid pointer, treat as no envp
            }
            if (uenv == 0) {
                envp[i] = 0;
                break;
            }
            envp[i] = kalloc();
            if (envp[i] == 0)
                goto bad;
            if (fetchstr(uenv, envp[i], PGSIZE) < 0) {
                kfree(envp[i]);
                envp[i] = 0;
                break; // invalid string, stop here
            }
            envc++;
        }
    }

    int ret = exec(path, argv, envp);

    for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
        kfree(argv[i]);
    for (i = 0; i < NELEM(envp) && envp[i] != 0; i++)
        kfree(envp[i]);

    return ret;

bad:
    for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
        kfree(argv[i]);
    for (i = 0; i < NELEM(envp) && envp[i] != 0; i++)
        kfree(envp[i]);
    return -1;
}
