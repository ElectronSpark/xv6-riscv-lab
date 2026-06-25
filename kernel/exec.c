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
#include "arch_thread.h"
#include "defs.h"
#include "printf.h"
#include "elf.h"
#include <mm/vm.h>
#include "errno.h"
#include "vfs/fs.h"
#include "vfs/file.h"
#include "vfs/fcntl.h"
#include "proc/cred.h"
#include "signal.h"
#include "kqueue_types.h"
#include "accounting.h"
#include "kstats.h"
#include "timer/timer.h"
#include "proc/chrome_lifecycle.h"

/* Enable verbose exec debugging — set to 1 to trace ELF loading steps */
#define EXEC_DEBUG 0

#if EXEC_DEBUG
#define exec_dbg(fmt, ...) printf("exec: " fmt, ##__VA_ARGS__)
#else
#define exec_dbg(fmt, ...) ((void)0)
#endif

static uint32 chrome_exec_trace_roles(char **argv, uint64 argc)
{
    uint32 roles = 0;

    for (uint64 i = 0; i < argc; i++) {
        const char *arg = argv[i];
        if (arg == NULL)
            continue;
        if (strstr(arg, "network.mojom.NetworkService") != NULL)
            roles |= TG_CHROME_TRACE_NETWORK_SERVICE;
        if (strstr(arg, "audio.mojom.AudioService") != NULL)
            roles |= TG_CHROME_TRACE_AUDIO_SERVICE;
    }
    return roles;
}

static int chrome_exec_fdtable_trace_enabled(void)
{
    static int initialized;
    static int enabled;

    if (!initialized) {
        enabled = chrome_trace_value_enabled("chrome_exec_fdtable_trace");
        initialized = 1;
    }
    return enabled;
}

static int chrome_exec_phase_trace_enabled(void)
{
    static int initialized;
    static int enabled;

    if (!initialized) {
        enabled = chrome_trace_value_enabled("chrome_exec_phase_trace");
        initialized = 1;
    }
    return enabled;
}

static uint64 exec_ticks_to_us(uint64 ticks)
{
    extern uint64 __timebase_frequency;
    uint64 freq = __timebase_frequency ? __timebase_frequency : 10000000UL;

    return (ticks / freq) * 1000000ULL +
           ((ticks % freq) * 1000000ULL) / freq;
}

static void chrome_exec_phase_trace(const char *path, const char *phase,
                                    uint64 start, uint64 *last)
{
    uint64 now;

    if (last == NULL || current == NULL ||
        !chrome_exec_phase_trace_enabled())
        return;
    if (!chrome_lifecycle_string_match(path) &&
        !chrome_lifecycle_thread_match(current))
        return;

    now = r_time();
    printf("chrome-exec-phase: pid=%d tgid=%d name='%s' path='%s' "
           "phase=%s delta_us=%lu total_us=%lu\n",
           current->pid, current->tgid, current->name, path ? path : "",
           phase, exec_ticks_to_us(now - *last),
           exec_ticks_to_us(now - start));
    *last = now;
}

/*
 * Base address for loading ET_DYN executables (PIE).
 * Chosen to be well above typical text addresses but below the stack.
 * The interpreter is loaded above this.
 */
#define ELF_ET_DYN_BASE  0x40000000UL
#define ELF_INTERP_BASE  0x70000000UL

/* Maximum number of auxiliary vector entries */
#define AT_VECTOR_SIZE 20

/*
 * Linux GUI launchers can legitimately carry more argv/env entries than the
 * original xv6 teaching limits.  Keep the wider exec input capacity local to
 * exec so procfs snapshots and thread-group state do not grow with it.
 */
#define EXEC_MAXARG 128
#define EXEC_MAXENV 256

/*
 * Keep the initial exec stack payload away from the bottom edge of the stack
 * VMA. Some dynamic loaders probe one byte before argv/env strings; if exec
 * places a string at the exact low edge, that benign probe becomes SEGV_MAPERR.
 */
#define EXEC_STACK_BOTTOM_SLACK 16UL

int snprintf(char *buf, size_t size, const char *fmt, ...);

#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif

#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

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

            /*
             * Linux still reports the final partially file-backed LOAD page
             * as part of the mapped file in /proc/<pid>/maps.  The page is
             * populated eagerly so the zero-fill tail is correct, but keep the
             * file identity and page offset for procfs/debugger/module readers
             * such as Crashpad.
             */
            ret = vm_mmap_region_locked(vm, file_pg_end, PGSIZE,
                                        vm_flags | VMA_FLAG_FILE, file,
                                        (uint64)foff, pa);
            if (ret != 0) {
                kfree(pa);
                return ret;
            }
            vma_t *boundary_vma = vm_find_area(vm, file_pg_end);
            if (boundary_vma != NULL && boundary_vma->start == file_pg_end)
                boundary_vma->file_data_end = (uint64)foff + nbytes;
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

static const char *exec_platform_string(void)
{
#if defined(CONFIG_ARCH_X86_64)
    return "x86_64";
#else
    return "riscv64";
#endif
}

static uint64 exec_hwcap(void)
{
    return 0;
}

static uint64 exec_hwcap2(void)
{
    return 0;
}

static int exec_stack_push(uint64 *sp, size_t len, uint64 floor)
{
    uint64 nsp;

    if (sp == NULL || len > *sp)
        return -1;
    nsp = *sp - len;
    nsp -= nsp & 15;
    if (nsp < floor)
        return -1;
    *sp = nsp;
    return 0;
}

static char *exec_flatten_strings(char **strings, uint64 count, size_t *out_len)
{
    size_t len = 0;

    if (out_len != NULL)
        *out_len = 0;
    for (uint64 i = 0; i < count; i++) {
        if (strings[i] == NULL)
            break;
        size_t slen = strlen(strings[i]) + 1;
        if (len + slen < len)
            return NULL;
        len += slen;
        if (len >= TG_EXEC_SNAPSHOT_MAX_BYTES) {
            len = TG_EXEC_SNAPSHOT_MAX_BYTES;
            break;
        }
    }
    if (len == 0)
        return NULL;

    char *buf = kvmalloc(len);
    if (buf == NULL)
        return NULL;

    size_t pos = 0;
    for (uint64 i = 0; i < count && pos < len; i++) {
        if (strings[i] == NULL)
            break;
        size_t slen = strlen(strings[i]) + 1;
        size_t chunk = slen;
        if (chunk > len - pos)
            chunk = len - pos;
        memmove(buf + pos, strings[i], chunk);
        pos += chunk;
    }
    if (pos > 0 && buf[pos - 1] != '\0')
        buf[pos - 1] = '\0';
    if (out_len != NULL)
        *out_len = pos;
    return buf;
}

int exec(char *path, char **argv, char **envp) {
    uint64 exec_start = r_time();
    uint64 exec_phase_last = exec_start;
    g_exec_calls += 1;
    char *s, *last;
    int i;
    /*
     * ustack layout (Linux ELF convention):
     *   argc, argv[0..argc-1], NULL, envp[0..envc-1], NULL, auxv[], AT_NULL
     * We size the array generously: exec input vectors + auxv pairs +
     * terminators.
     */
    uint64 argc, envc, sp;
    uint64 ustack[EXEC_MAXARG + EXEC_MAXENV + AT_VECTOR_SIZE * 2 + 8];
    uint64 stackbase = USTACKTOP - USERSTACK * PGSIZE;
    uint64 stackfloor = stackbase + EXEC_STACK_BOTTOM_SLACK;
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
    int exec_setuid = 0;      /* set if S_ISUID bit on executable */
    int exec_setgid = 0;      /* set if S_ISGID bit on executable */
    uint32 exec_uid = 0;      /* uid to adopt if setuid */
    uint32 exec_gid = 0;      /* gid to adopt if setgid */
    char *snapshot_cmdline = NULL;
    size_t snapshot_cmdline_len = 0;
    uint64 snapshot_cmdline_addrs[MAXARG] = {0};
    size_t snapshot_cmdline_lens[MAXARG] = {0};
    char *snapshot_environ = NULL;
    size_t snapshot_environ_len = 0;
    uint64 snapshot_environ_addrs[MAXENV] = {0};
    size_t snapshot_environ_lens[MAXENV] = {0};
    char old_name[sizeof(p->name)];

    exec_dbg("pid %d: exec(\"%s\")\n", current->pid, path);
    chrome_exec_phase_trace(path, "begin", exec_start, &exec_phase_last);

    // Look up the file using VFS
    struct vfs_inode *inode = vfs_namei(path, strlen(path));
    if (IS_ERR_OR_NULL(inode)) {
        long err = IS_ERR(inode) ? PTR_ERR(inode) : -ENOENT;
        exec_dbg("  FAIL: vfs_namei(\"%s\") failed\n", path);
        chrome_exec_phase_trace(path, "fail-namei", exec_start,
                                &exec_phase_last);
        return (int)err;
    }
    chrome_exec_phase_trace(path, "namei", exec_start, &exec_phase_last);

    // Check execute permission
    int perm_ret = inode_permission(inode, MAY_EXEC);
    if (perm_ret != 0) {
        exec_dbg("  FAIL: no execute permission on \"%s\"\n", path);
        vfs_iput(inode);
        chrome_exec_phase_trace(path, "fail-permission", exec_start,
                                &exec_phase_last);
        return perm_ret;
    }
    chrome_exec_phase_trace(path, "permission", exec_start, &exec_phase_last);

    // Check for setuid/setgid bits
    if (inode->mode & S_ISUID) {
        exec_setuid = 1;
        exec_uid = inode->uid;
    }
    if (inode->mode & S_ISGID) {
        exec_setgid = 1;
        exec_gid = inode->gid;
    }

    // Open the file for reading
    file = vfs_fileopen(inode, O_RDONLY);
    vfs_iput(inode); // vfs_fileopen takes its own reference
    inode = NULL;
    chrome_exec_phase_trace(path, "open", exec_start, &exec_phase_last);

    if (IS_ERR(file)) {
        exec_dbg("  FAIL: vfs_fileopen IS_ERR\n");
        chrome_exec_phase_trace(path, "fail-open", exec_start,
                                &exec_phase_last);
        return -1;
    }
    if (file == NULL) {
        exec_dbg("  FAIL: vfs_fileopen returned NULL\n");
        chrome_exec_phase_trace(path, "fail-open-null", exec_start,
                                &exec_phase_last);
        return -1;
    }

    // Read ELF header
    ssize_t n = vfs_fileread(file, &elf, sizeof(elf), false);
    if (n != sizeof(elf)) {
        exec_dbg("  FAIL: read ELF header: got %ld bytes\n", n);
        goto bad;
    }
    chrome_exec_phase_trace(path, "read-ehdr", exec_start, &exec_phase_last);

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
            {
                goto bad;
            }
            if (vfs_fileread(file, interp_path, ph.filesz, false) !=
                (ssize_t)ph.filesz)
            {
                goto bad;
            }
            interp_path[ph.filesz] = '\0';
            /* Strip trailing newline if any */
            if (interp_path[ph.filesz - 1] == '\n')
                interp_path[ph.filesz - 1] = '\0';
            has_interp = 1;
            exec_dbg("  PT_INTERP: \"%s\"\n", interp_path);
            break;
        }
    }
    chrome_exec_phase_trace(path, "scan-phdr", exec_start, &exec_phase_last);

    if ((tmp_vm = vm_init()) == NULL) {
        goto bad;
    }
    chrome_exec_phase_trace(path, "vm-init", exec_start, &exec_phase_last);

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
    chrome_exec_phase_trace(path, "load-main", exec_start, &exec_phase_last);

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
        chrome_exec_phase_trace(path, "interp-namei", exec_start,
                                &exec_phase_last);

        interp_file = vfs_fileopen(interp_inode, O_RDONLY);
        vfs_iput(interp_inode);
        chrome_exec_phase_trace(path, "interp-open", exec_start,
                                &exec_phase_last);

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
        chrome_exec_phase_trace(path, "interp-ehdr", exec_start,
                                &exec_phase_last);

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
        chrome_exec_phase_trace(path, "load-interp", exec_start,
                                &exec_phase_last);

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
    chrome_exec_phase_trace(path, "close-main-file", exec_start,
                            &exec_phase_last);

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
    chrome_exec_phase_trace(path, "map-heap-stack", exec_start,
                            &exec_phase_last);

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
    chrome_exec_phase_trace(path, "preload-entry", exec_start,
                            &exec_phase_last);

    // Count argument strings before placing the Linux-style argv/env block.
    for (argc = 0; argv[argc]; argc++) {
        if (argc >= EXEC_MAXARG)
        {
            goto bad;
        }
    }

    envc = 0;
    if (envp) {
        for (envc = 0; envp[envc]; envc++) {
            if (envc >= EXEC_MAXENV)
            {
                goto bad;
            }
        }
    }
    chrome_exec_phase_trace(path, "count-argv-env", exec_start,
                            &exec_phase_last);

    /*
     * Linux exposes argv/env as a live ascending memory range through
     * procfs. Copy env first and argv second while the stack grows down so
     * the final user layout is argv[0..n], then env[0..n].
     */
    for (i = (int)envc - 1; i >= 0; i--) {
        size_t len = strlen(envp[i]) + 1;
        if (exec_stack_push(&sp, len, stackfloor) != 0)
        {
            goto bad;
        }
        if (vm_copyout(tmp_vm, sp, envp[i], len) < 0)
        {
            goto bad;
        }
        ustack[argc + 2 + i] = sp;
        if (i < MAXENV) {
            snapshot_environ_addrs[i] = sp;
            snapshot_environ_lens[i] = len;
        }
    }

    // Push argument strings onto the stack.
    for (i = (int)argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        if (exec_stack_push(&sp, len, stackfloor) != 0)
        {
            goto bad;
        }
        if (vm_copyout(tmp_vm, sp, argv[i], len) < 0)
        {
            goto bad;
        }
        ustack[i] = sp;
        if (i < MAXARG) {
            snapshot_cmdline_addrs[i] = sp;
            snapshot_cmdline_lens[i] = len;
        }
    }
    chrome_exec_phase_trace(path, "copy-argv-env", exec_start,
                            &exec_phase_last);

    uint8 random_bytes[16];
    random_fill_bytes(random_bytes, sizeof(random_bytes));
    if (exec_stack_push(&sp, sizeof(random_bytes), stackfloor) != 0)
    {
        goto bad;
    }
    uint64 random_addr = sp;
    if (vm_copyout(tmp_vm, sp, (char *)random_bytes, sizeof(random_bytes)) < 0)
    {
        goto bad;
    }

    const char *platform = exec_platform_string();
    size_t platform_len = strlen(platform) + 1;
    if (exec_stack_push(&sp, platform_len, stackfloor) != 0)
    {
        goto bad;
    }
    uint64 platform_addr = sp;
    if (vm_copyout(tmp_vm, sp, (char *)platform, platform_len) < 0)
    {
        goto bad;
    }

    size_t execfn_len = strlen(path) + 1;
    if (exec_stack_push(&sp, execfn_len, stackfloor) != 0)
    {
        goto bad;
    }
    uint64 execfn_addr = sp;
    if (vm_copyout(tmp_vm, sp, path, execfn_len) < 0)
    {
        goto bad;
    }

    snapshot_cmdline = exec_flatten_strings(argv, argc, &snapshot_cmdline_len);
    snapshot_environ = exec_flatten_strings(envp, envc, &snapshot_environ_len);

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
    int auxv_start_idx = aidx;

    push_auxv(ustack, &aidx, AT_PAGESZ, PGSIZE);
    push_auxv(ustack, &aidx, AT_PHENT, sizeof(struct proghdr));
    push_auxv(ustack, &aidx, AT_PHNUM, elf.phnum);
    push_auxv(ustack, &aidx, AT_ENTRY, elf.entry + load_bias);
    push_auxv(ustack, &aidx, AT_FLAGS, 0);
    push_auxv(ustack, &aidx, AT_HWCAP, exec_hwcap());
    push_auxv(ustack, &aidx, AT_HWCAP2, exec_hwcap2());
    push_auxv(ustack, &aidx, AT_CLKTCK, HZ);
    push_auxv(ustack, &aidx, AT_RANDOM, random_addr);
    push_auxv(ustack, &aidx, AT_EXECFN, execfn_addr);
    push_auxv(ustack, &aidx, AT_PLATFORM, platform_addr);

    if (phdr_addr != 0) {
        push_auxv(ustack, &aidx, AT_PHDR, phdr_addr);
    }

    if (has_interp) {
        push_auxv(ustack, &aidx, AT_BASE, interp_base);
    } else {
        push_auxv(ustack, &aidx, AT_BASE, 0);
    }

    /* Apply setuid/setgid credentials before populating auxv */
    if (exec_setuid) {
        p->thread_group->euid = exec_uid;
        p->thread_group->suid = exec_uid;
    }
    if (exec_setgid) {
        p->thread_group->egid = exec_gid;
        p->thread_group->sgid = exec_gid;
    }

    /* Process credentials for ELF auxiliary vector */
    push_auxv(ustack, &aidx, AT_UID, p->thread_group->uid);
    push_auxv(ustack, &aidx, AT_EUID, p->thread_group->euid);
    push_auxv(ustack, &aidx, AT_GID, p->thread_group->gid);
    push_auxv(ustack, &aidx, AT_EGID, p->thread_group->egid);
    push_auxv(ustack, &aidx, AT_SECURE, (exec_setuid || exec_setgid) ? 1 : 0);

    /* AT_NULL terminates the auxv */
    push_auxv(ustack, &aidx, AT_NULL, 0);
    size_t auxv_len = (size_t)(aidx - auxv_start_idx) * sizeof(uint64);

    uint64 nslots = (uint64)aidx;
    if (exec_stack_push(&sp, nslots * sizeof(uint64), stackfloor) != 0)
    {
        goto bad;
    }
    if (vm_copyout(tmp_vm, sp, (char *)ustack, nslots * sizeof(uint64)) < 0)
    {
        goto bad;
    }
    chrome_exec_phase_trace(path, "build-stack", exec_start,
                            &exec_phase_last);

    // Set up the user registers for main(argc, argv).
    // Architecture-specific: on RISC-V a0 doubles as both syscall return
    // and first argument, so only argv (a1) needs explicit setup.
    // On x86_64 the syscall return (RAX) differs from the first argument
    // register (RDI), so both argc and argv must be set explicitly.
    arch_tf_set_exec_args(p->trapframe, argc, sp + sizeof(uint64));

    safestrcpy(old_name, p->name, sizeof(old_name));

    // Save program name for debugging.
    for (last = s = path; *s; s++)
        if (*s == '/')
            last = s + 1;
    safestrcpy(p->name, last, sizeof(p->name));
    chrome_exec_phase_trace(path, "set-name", exec_start, &exec_phase_last);

    /* kqueue: notify EVFILT_PROC watchers of exec */
    kqueue_proc_notify(p, NOTE_EXEC, 0);
    chrome_exec_phase_trace(path, "kqueue-notify", exec_start,
                            &exec_phase_last);

    // Commit to the user image.
    chrome_exec_phase_trace(path, "before-old-vm-put", exec_start,
                            &exec_phase_last);
    vm_put_owner(p->vm, p->thread_group); // Destroy the old VM
    chrome_exec_phase_trace(path, "old-vm-put", exec_start, &exec_phase_last);
    p->vm = NULL;
    p->vm = tmp_vm;
    chrome_exec_phase_trace(path, "assign-new-vm", exec_start,
                            &exec_phase_last);
    __atomic_store_n(&p->thread_group->acct.mm_rss_pages,
                     vm_resident_pages(tmp_vm), __ATOMIC_RELAXED);
    chrome_exec_phase_trace(path, "rss-account", exec_start,
                            &exec_phase_last);

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
    thread_group_exec_snapshot_set(p->thread_group, snapshot_cmdline,
                                   snapshot_cmdline_len,
                                   snapshot_cmdline_addrs,
                                   snapshot_cmdline_lens, argc,
                                   snapshot_environ, snapshot_environ_len,
                                   snapshot_environ_addrs,
                                   snapshot_environ_lens, envc,
                                   &ustack[auxv_start_idx], auxv_len);
    chrome_exec_phase_trace(path, "snapshot", exec_start, &exec_phase_last);
    uint32 old_chrome_roles =
        __atomic_load_n(&p->thread_group->chrome_trace_roles,
                        __ATOMIC_SEQ_CST);
    uint32 chrome_roles = chrome_exec_trace_roles(argv, argc) |
        (old_chrome_roles & TG_CHROME_TRACE_CHILD_PROCESS);
    __atomic_store_n(&p->thread_group->chrome_trace_roles,
                     chrome_roles, __ATOMIC_SEQ_CST);
    snapshot_cmdline = NULL;
    snapshot_environ = NULL;

    if (chrome_lifecycle_trace_enabled() &&
        (chrome_lifecycle_string_match(path) ||
         chrome_lifecycle_thread_match(p))) {
        printf("chrome-lifecycle: exec pid=%d tgid=%d old_name='%s' "
               "name='%s' path='%s' argc=%ld has_interp=%d interp='%s' "
               "entry=0x%lx chrome_roles=0x%x\n",
               p->pid, p->tgid, old_name, p->name, path, argc,
               has_interp, has_interp ? interp_path : "",
               has_interp ? interp_entry : elf.entry + load_bias,
               __atomic_load_n(&p->thread_group->chrome_trace_roles,
                               __ATOMIC_SEQ_CST));
        uint64 arg_limit = argc < 64 ? argc : 64;
        for (uint64 ai = 0; ai < arg_limit; ai++) {
            printf("chrome-lifecycle: exec-argv pid=%d argv[%ld]='%s'\n",
                   p->pid, ai, argv[ai] ? argv[ai] : "");
        }
    }
    int chrome_exec_trace_match =
        chrome_lifecycle_string_match(path) || chrome_lifecycle_thread_match(p);
    if (chrome_trace_value_enabled("chrome_fd_trace") &&
        chrome_exec_trace_match) {
        vfs_fdtable_debug_dump(p, "exec-before-close", 128);
    }

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
    rseq_clear_thread(p);
    chrome_exec_phase_trace(path, "reset-signals-rseq", exec_start,
                            &exec_phase_last);

    // Reset FPU state — new program starts with no FP context.
    THREAD_CLEAR_FPU_USED(p);
    if (mycpu()->fpu_owner_tid == p->pid)
        mycpu()->fpu_owner_tid = 0;
    if (p->fpu_state != NULL) {
        kvfree(p->fpu_state);
        p->fpu_state = NULL;
    }
    chrome_exec_phase_trace(path, "reset-fpu", exec_start, &exec_phase_last);

    // Close descriptors marked close-on-exec.
    vfs_fdtable_close_on_exec(p->fdtable);
    chrome_exec_phase_trace(path, "close-on-exec", exec_start,
                            &exec_phase_last);
    if (chrome_trace_value_enabled("chrome_fd_trace") &&
        chrome_exec_trace_match) {
        vfs_fdtable_debug_dump(p, "exec-after-close", 128);
    }
    if (chrome_exec_fdtable_trace_enabled() && chrome_exec_trace_match) {
        vfs_fdtable_debug_dump(p, "exec-after-close-lite", 128);
    }

    // Wake vfork parent.
    vfork_done(p);
    chrome_exec_phase_trace(path, "vfork-done", exec_start, &exec_phase_last);

    // Stop for GDB if being debugged.
    {
        extern void gdbstub_exec_stop(struct thread *, uint64, const char *);
        gdbstub_exec_stop(p, elf.entry + load_bias, path);
    }
    chrome_exec_phase_trace(path, "gdbstub-stop", exec_start,
                            &exec_phase_last);

    /*
     * Flush the I-cache on ALL harts.  exec() has loaded new executable
     * code (including eagerly-preloaded pages near the entry point).  On
     * real RISC-V hardware the I-cache may contain stale entries from the
     * previous process at the same virtual addresses.  Use SBI remote
     * fence.i so every hart sees fresh code when this process is scheduled.
     */
    vm_remote_fence_i(p->vm);
    chrome_exec_phase_trace(path, "remote-fence-i", exec_start,
                            &exec_phase_last);

    ACCT_INC(p->thread_group, sched_execs);
    g_exec_ticks += r_time() - exec_start;
    chrome_exec_phase_trace(path, "done", exec_start, &exec_phase_last);
    return argc; // this ends up in a0, the first argument to main(argc, argv)

bad_locked:
    vm_wunlock(tmp_vm);
bad:
    exec_dbg("  FAILED for \"%s\"\n", path);
    chrome_exec_phase_trace(path, "fail", exec_start, &exec_phase_last);
    vm_put(tmp_vm);
    tmp_vm = NULL;
    if (file) {
        vfs_fput(file);
    }
    kvfree(snapshot_cmdline);
    kvfree(snapshot_environ);
    g_exec_ticks += r_time() - exec_start;
    return -1;
}

/*
 * System call handler for exec.
 * Parses user arguments and calls exec().
 */
static void exec_free_user_strings(char **strings, int limit);
static int exec_copy_user_strings(uint64 uvec, char **strings, int limit,
                                  int strict);
static void exec_trace_syscall_result(const char *syscall, const char *path,
                                      int ret);

static int exec_is_current_proc_exe_path(const char *path)
{
    char self_path[32];
    char pid_path[32];

    if (path == NULL || current == NULL)
        return 0;
    if (strcmp(path, "/proc/self/exe") == 0)
        return 1;
    snprintf(self_path, sizeof(self_path), "/proc/%d/exe", current->tgid);
    snprintf(pid_path, sizeof(pid_path), "/proc/%d/exe", current->pid);
    return strcmp(path, self_path) == 0 || strcmp(path, pid_path) == 0;
}

static void exec_resolve_current_proc_exe(char *path, size_t size)
{
    const char *target;

    if (!exec_is_current_proc_exe_path(path) || current->thread_group == NULL)
        return;

    target = current->thread_group->exec_path;
    if (target == NULL || target[0] == '\0' ||
        exec_is_current_proc_exe_path(target))
        return;

    if (chrome_lifecycle_trace_enabled()) {
        printf("chrome-lifecycle: resolve-proc-exe pid=%d tgid=%d name='%s' path='%s' target='%s'\n",
               current->pid, current->tgid, current->name, path, target);
    }
    safestrcpy(path, target, size);
}

uint64 sys_exec(void) {
    char path[MAXPATH], *argv[EXEC_MAXARG + 1], *envp[EXEC_MAXENV + 1];
    int i;
    uint64 uargv, uarg, uenvp, uenv;

    argaddr(1, &uargv);
    argaddr(2, &uenvp);
    if (argstr(0, path, MAXPATH) < 0) {
        return -1;
    }
    exec_resolve_current_proc_exe(path, sizeof(path));
    memset(argv, 0, sizeof(argv));
    memset(envp, 0, sizeof(envp));
    for (i = 0;; i++) {
        if (i >= NELEM(argv) - 1) {
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
            if (i >= NELEM(envp) - 1) {
                goto bad;
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
    exec_trace_syscall_result("execve", path, ret);

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

static void exec_free_user_strings(char **strings, int limit)
{
    for (int i = 0; i < limit && strings[i] != 0; i++)
        kfree(strings[i]);
}

static int exec_copy_user_strings(uint64 uvec, char **strings, int limit,
                                  int strict)
{
    uint64 ustr;

    memset(strings, 0, sizeof(char *) * limit);
    if (uvec == 0)
        return 0;

    for (int i = 0;; i++) {
        if (i >= limit - 1)
            return strict ? -E2BIG : 0;
        if (fetchaddr(uvec + sizeof(uint64) * i, &ustr) < 0)
            return strict ? -EFAULT : 0;
        if (ustr == 0) {
            strings[i] = 0;
            return 0;
        }
        strings[i] = kalloc();
        if (strings[i] == 0)
            return -ENOMEM;
        if (fetchstr(ustr, strings[i], PGSIZE) < 0)
            return strict ? -EFAULT : 0;
    }
}

static void exec_trace_syscall_result(const char *syscall, const char *path,
                                      int ret)
{
    if (!chrome_lifecycle_trace_enabled() &&
        !chrome_trace_value_enabled("chrome_fd_trace"))
        return;
    if (ret >= 0 && !chrome_exec_syscall_trace_enabled())
        return;
    if (!chrome_lifecycle_string_match(path) &&
        !chrome_lifecycle_thread_match(current))
        return;

    printf("chrome-lifecycle: %s pid=%d tgid=%d name='%s' path='%s' ret=%d\n",
           syscall, current->pid, current->tgid, current->name, path, ret);
}

/*
 * Linux execveat(2) compatibility.
 *
 * Chromium's zygote/utility launch path can execute through /proc/self/exe
 * or fd-backed AT_EMPTY_PATH forms while passing startup file descriptors
 * over UNIX sockets.  The ELF loader is path-based today, so fd execution is
 * resolved through the best-effort opened_path stored on regular VFS files.
 */
uint64 sys_execveat(void)
{
    int dirfd, flags;
    uint64 uargv, uenvp;
    char path[MAXPATH], exec_path[MAXPATH];
    char *argv[EXEC_MAXARG + 1], *envp[EXEC_MAXENV + 1];
    int ret = -EINVAL;

    argint(0, &dirfd);
    argaddr(2, &uargv);
    argaddr(3, &uenvp);
    argint(4, &flags);

    memset(argv, 0, sizeof(argv));
    memset(envp, 0, sizeof(envp));

    if (argstr(1, path, MAXPATH) < 0)
        return (uint64)-EFAULT;

    if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
        return (uint64)-EINVAL;

    exec_path[0] = '\0';
    if (path[0] == '\0') {
        if ((flags & AT_EMPTY_PATH) == 0 || dirfd == AT_FDCWD)
            return (uint64)-ENOENT;

        struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, dirfd);
        if (f == NULL)
            return (uint64)-EBADF;
        if (f->opened_path == NULL || f->opened_path[0] == '\0') {
            vfs_fput(f);
            return (uint64)-ENOENT;
        }
        safestrcpy(exec_path, f->opened_path, sizeof(exec_path));
        vfs_fput(f);
    } else if (path[0] == '/' || dirfd == AT_FDCWD) {
        safestrcpy(exec_path, path, sizeof(exec_path));
        exec_resolve_current_proc_exe(exec_path, sizeof(exec_path));
    } else {
        struct vfs_file *dir = vfs_fdtable_get_file(current->fdtable, dirfd);
        if (dir == NULL)
            return (uint64)-EBADF;
        if (dir->opened_path == NULL || dir->opened_path[0] == '\0') {
            vfs_fput(dir);
            return (uint64)-ENOENT;
        }
        if (strlen(dir->opened_path) + 1 + strlen(path) >= sizeof(exec_path)) {
            vfs_fput(dir);
            return (uint64)-ENAMETOOLONG;
        }
        safestrcpy(exec_path, dir->opened_path, sizeof(exec_path));
        int len = strlen(exec_path);
        if (len > 0 && exec_path[len - 1] != '/')
            safestrcpy(exec_path + len, "/", sizeof(exec_path) - len);
        safestrcpy(exec_path + strlen(exec_path), path,
                   sizeof(exec_path) - strlen(exec_path));
        vfs_fput(dir);
    }

    ret = exec_copy_user_strings(uargv, argv, NELEM(argv), 1);
    if (ret < 0)
        goto out_free;
    ret = exec_copy_user_strings(uenvp, envp, NELEM(envp), 0);
    if (ret < 0)
        goto out_free;

    ret = exec(exec_path, argv, envp);
    exec_trace_syscall_result("execveat", exec_path, ret);

out_free:
    exec_free_user_strings(argv, NELEM(argv));
    exec_free_user_strings(envp, NELEM(envp));
    return ret;
}
