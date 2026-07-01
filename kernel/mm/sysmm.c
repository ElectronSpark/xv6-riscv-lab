/**
 * sysmm.c - Memory management syscall handlers
 *
 * System call implementations for mmap, munmap, mprotect, mremap,
 * msync, mincore, and madvise.
 * These are thin wrappers that extract arguments from the trapframe
 * and dispatch to the corresponding vm.c functions.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "errno.h"
#include "proc/thread.h"
#include <mm/vm.h>
#include "vfs/file.h"
#include "vfs/stat.h"
#include "cmdline.h"
#include "accounting.h"
#include "kstats.h"
#include "maple_tree.h"
#include "proc/chrome_lifecycle.h"

#define SYSCALL_PROFILE_BEGIN(call_ctr)                                     \
    int __sys_profile = kstats_profile_enabled();                           \
    uint64 __sys_start = __sys_profile ? r_time() : 0;                      \
    do {                                                                    \
        if (__sys_profile)                                                  \
            __atomic_add_fetch(&(call_ctr), 1, __ATOMIC_RELAXED);           \
    } while (0)

#define SYSCALL_PROFILE_RETURN(ret_expr, tick_ctr)                          \
    do {                                                                    \
        uint64 __sys_ret = (uint64)(ret_expr);                              \
        if (__sys_profile)                                                  \
            __atomic_add_fetch(&(tick_ctr), r_time() - __sys_start,         \
                               __ATOMIC_RELAXED);                           \
        return __sys_ret;                                                   \
    } while (0)

static int chrome_mmap_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("chrome_fd_trace", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static int chrome_mmap_trace_process(void)
{
    return chrome_lifecycle_kernel_trace_process_match(current, 0, 1);
}

static const char *chrome_mmap_trace_path(struct vfs_file *f)
{
    if (f == NULL)
        return "(null)";
    if (f->opened_path != NULL)
        return f->opened_path;
    if (f->f_kind == VFS_FILE_KIND_PIPE)
        return "pipe";
    if (f->f_kind == VFS_FILE_KIND_LEGACY_SOCKET)
        return "socket";
    if (f->f_kind == VFS_FILE_KIND_CUSTOM)
        return "custom";
    if (f->f_kind == VFS_FILE_KIND_CDEV)
        return "cdev";
    if (f->f_kind == VFS_FILE_KIND_BDEV)
        return "bdev";
    return "(unknown)";
}

static int chrome_asset_trace_path_match(const char *path)
{
    if (path == NULL)
        return 0;
    return strstr(path, "icudtl.dat") != NULL ||
           strstr(path, "v8_context_snapshot.bin") != NULL;
}

static const char *chrome_asset_trace_vma_path(vma_t *vma)
{
    if (vma == NULL || vma->file == NULL)
        return "-";
    if (vma->file->opened_path != NULL &&
        vma->file->opened_path[0] != '\0')
        return vma->file->opened_path;
    if (vma->file->inode.inode != NULL &&
        vma->file->inode.inode->name != NULL)
        return vma->file->inode.inode->name;
    return "(unnamed)";
}

static int chrome_asset_trace_range_overlaps(vma_t *vma, uint64 start,
                                             uint64 end)
{
    return vma != NULL && start < vma->end && end > vma->start;
}

static void chrome_asset_trace_vm_range(const char *op, uint64 addr,
                                        uint64 length, int arg, int ret)
{
    uint64 end = addr + length;
    int found = 0;

    if (!chrome_mmap_trace_enabled() || !chrome_lifecycle_thread_match(current) ||
        current->vm == NULL ||
        length == 0 || end < addr)
        return;

    vm_rlock(current->vm);
    vma_t *vma;
    uint64 index = 0;
    mt_for_each(&current->vm->vm_mt, vma, index, (uint64)(-1ULL)) {
        const char *path;

        if (!chrome_asset_trace_range_overlaps(vma, addr, end))
            continue;
        path = chrome_asset_trace_vma_path(vma);
        if (!chrome_asset_trace_path_match(path))
            continue;

        found++;
        printf("chrome-asset-vma-op: op=%s pid=%d tgid=%d name=%s addr=0x%lx len=%lu arg=%d ret=%d map=[0x%lx-0x%lx) %c%c%c %s flags=0x%lx pgoff=0x%lx file=%p path=%s\n",
               op, current->pid, current->tgid, current->name, addr, length,
               arg, ret, vma->start, vma->end,
               (vma->flags & PROT_READ) ? 'r' : '-',
               (vma->flags & PROT_WRITE) ? 'w' : '-',
               (vma->flags & PROT_EXEC) ? 'x' : '-',
               (vma->flags & VMA_FLAG_SHARED) ? "shared" : "private",
               vma->flags, vma->pgoff, (void *)vma->file, path);
    }
    vm_runlock(current->vm);

    if (found == 0 && strcmp(op, "munmap-after") == 0) {
        printf("chrome-asset-vma-op: op=%s pid=%d tgid=%d name=%s addr=0x%lx len=%lu arg=%d ret=%d found=0\n",
               op, current->pid, current->tgid, current->name, addr, length,
               arg, ret);
    }
}

// mmap(addr, length, prot, flags, fd, offset)
uint64 sys_mmap(void) {
    SYSCALL_PROFILE_BEGIN(g_sys_mmap_calls);
    uint64 addr, offset, length;
    int prot, flags, fd;

    argaddr(0, &addr);
    argaddr(1, &length);  // size_t is 64-bit on rv64
    argint(2, &prot);
    argint(3, &flags);
    argint(4, &fd);
    argaddr(5, &offset);

    struct vfs_file *trace_file = NULL;
    struct stat trace_st;
    int trace_stat_ret = -EBADF;
    memset(&trace_st, 0, sizeof(trace_st));
    int trace_chrome = chrome_mmap_trace_enabled() &&
        chrome_mmap_trace_process() && !(flags & MAP_ANONYMOUS) && fd >= 0;
    if (trace_chrome) {
        trace_file = vfs_fdtable_get_file(current->fdtable, fd);
        if (trace_file != NULL)
            trace_stat_ret = vfs_filestat(trace_file, &trace_st);
    }

    uint64 ret = vm_mmap(current->vm, addr, (size_t)length, prot, flags, fd, offset);
    if ((int64)ret >= 0) {
        ACCT_INC(current->thread_group, mm_mmap_count);
    }
    if (trace_chrome) {
        printf("chrome-fd-trace: mmap pid=%d tgid=%d name=%s fd=%d "
               "addr=0x%lx len=%lu prot=0x%x flags=0x%x type=0x%x "
               "off=%lu ret=0x%lx stat_ret=%d mode=0x%x size=%ld path=%s\n",
               current->pid, current->tgid, current->name, fd, addr, length,
               prot, flags, flags & MAP_TYPE, offset, ret, trace_stat_ret,
               trace_st.st_mode, trace_st.st_size,
               chrome_mmap_trace_path(trace_file));
    }
    if (trace_file != NULL)
        vfs_fput(trace_file);
    SYSCALL_PROFILE_RETURN(ret, g_sys_mmap_ticks);
}

// munmap(addr, length)
uint64 sys_munmap(void) {
    SYSCALL_PROFILE_BEGIN(g_sys_munmap_calls);
    uint64 addr, length;

    argaddr(0, &addr);
    argaddr(1, &length);

    if (length == 0)
        SYSCALL_PROFILE_RETURN(-EINVAL, g_sys_munmap_ticks);

    chrome_asset_trace_vm_range("munmap-before", addr, length, 0, 0);
    int ret = vm_munmap(current->vm, addr, (size_t)length);
    chrome_asset_trace_vm_range("munmap-after", addr, length, 0, ret);
    if (ret == 0)
        ACCT_INC(current->thread_group, mm_munmap_count);
    SYSCALL_PROFILE_RETURN(ret, g_sys_munmap_ticks);
}

// mprotect(addr, length, prot)
uint64 sys_mprotect(void) {
    SYSCALL_PROFILE_BEGIN(g_sys_mprotect_calls);
    uint64 addr, length;
    int prot;

    argaddr(0, &addr);
    argaddr(1, &length);
    argint(2, &prot);

    if (length == 0)
        SYSCALL_PROFILE_RETURN(-EINVAL, g_sys_mprotect_ticks);

    int ret = vm_mprotect(current->vm, addr, (size_t)length, prot);
    SYSCALL_PROFILE_RETURN(ret, g_sys_mprotect_ticks);
}

// mremap(old_addr, old_size, new_size, flags, new_addr)
uint64 sys_mremap(void) {
    uint64 old_addr, new_addr, old_size, new_size;
    int flags;

    argaddr(0, &old_addr);
    argaddr(1, &old_size);
    argaddr(2, &new_size);
    argint(3, &flags);
    argaddr(4, &new_addr);

    if (new_size == 0)
        return -EINVAL;

    uint64 mremap_ret = vm_mremap(current->vm, old_addr, (size_t)old_size,
                     (size_t)new_size, flags, new_addr);
    return mremap_ret;
}

// msync(addr, length, flags)
uint64 sys_msync(void) {
    uint64 addr, length;
    int flags;

    argaddr(0, &addr);
    argaddr(1, &length);
    argint(2, &flags);

    if (length == 0)
        return -EINVAL;

    return (uint64)vm_msync(current->vm, addr, (size_t)length, flags);
}

// mincore(addr, length, vec)
uint64 sys_mincore(void) {
    uint64 addr, vec_uaddr, length;

    argaddr(0, &addr);
    argaddr(1, &length);
    argaddr(2, &vec_uaddr);

    if (length == 0)
        return -EINVAL;

    size_t sz = (size_t)length;
    size_t num_pages = (PGROUNDUP(sz)) / PGSIZE;

    // Use a stack buffer, process in chunks of 256 pages
    unsigned char kbuf[256];
    size_t done = 0;
    while (done < num_pages) {
        size_t chunk = num_pages - done;
        if (chunk > sizeof(kbuf))
            chunk = sizeof(kbuf);

        int ret = vm_mincore(current->vm, addr + done * PGSIZE,
                             chunk * PGSIZE, kbuf);
        if (ret < 0)
            return (uint64)ret;

        if (vm_copyout(current->vm, vec_uaddr + done,
                       (void *)kbuf, chunk) < 0)
            return (uint64)(-EFAULT);

        done += chunk;
    }
    return 0;
}

// madvise(addr, length, advice)
uint64 sys_madvise(void) {
    uint64 addr, length;
    int advice;

    argaddr(0, &addr);
    argaddr(1, &length);
    argint(2, &advice);

    if (length == 0)
        return -EINVAL;

    chrome_asset_trace_vm_range("madvise-before", addr, length, advice, 0);
    int ret = vm_madvise(current->vm, addr, (size_t)length, advice);
    chrome_asset_trace_vm_range("madvise-after", addr, length, advice, ret);
    return (uint64)ret;
}
