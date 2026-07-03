/**
 * @file trap.c
 * @brief x86_64 IDT setup, PIC 8259A, and trap handler.
 *
 * arch_trap_init()  — builds IDT, loads GDT, sets SYSCALL MSRs.
 * arch_irq_init()   — initializes PIC 8259A, masks all IRQs except timer.
 * x86_trap_handler  — C-level handler called from trapvec.S.
 */

#include "types.h"
#include "printf.h"
#include "x86.h"
#include "defs.h"
#include "string.h"
#include "arch/trap.h"
#include "proc/thread.h"
#include "arch_thread.h"
#include "signal.h"
#include "seg.h"
#include "trapframe.h"
#include "lapic.h"
#include "ioapic.h"
#include <mm/pgtable.h>
#include <mm/vm.h>
#include "maple_tree.h"
#include <smp/percpu.h>
#include <proc/sched.h>
#include "memlayout.h"
#include "ksymbols.h"
#include "proc/chrome_lifecycle.h"
#include "vfs/file.h"
#include "cmdline.h"
#include "errno.h"
#include <mm/oom_kill.h>
#include <mm/page.h>

#define USER_FAULT_AROUND_PAGES 8UL

static const char *fault_vma_file_name(vma_t *vma)
{
    if (vma == NULL || vma->file == NULL || vma->file->inode.inode == NULL)
        return "-";
    if (vma->file->inode.inode->name != NULL)
        return vma->file->inode.inode->name;
    return "(unnamed)";
}

static struct vfs_inode *fault_vma_inode(vma_t *vma)
{
    if (vma == NULL || vma->file == NULL)
        return NULL;
    return vma->file->inode.inode;
}

static uint64 fault_vma_inode_size(struct vfs_inode *inode)
{
    return inode != NULL ? (uint64)inode->size : 0;
}

static const char *vma_debug_path(vma_t *vma);

static void print_vma_file_data_end(vma_t *vma, struct vfs_inode *inode)
{
    uint64 raw;
    uint64 computed = VMA_FILE_DATA_END_EOF_MARKER;

    if (vma == NULL || vma->file == NULL)
        return;

    raw = vma->file_data_end;
    if (VMA_FILE_DATA_END_IS_LIMIT(raw))
        computed = raw;
    else if (inode != NULL)
        computed = fault_vma_inode_size(inode);

    printf(" file_data_end_raw=0x%lx file_data_end_computed=0x%lx",
           raw, computed);
}

static int fault_vma_entry_valid(vm_t *vm, vma_t *vma)
{
    uint64 ptr = (uint64)vma;
    pte_t *pte;

    if (vm == NULL || vma == NULL)
        return 0;
    if (kernel_vm != NULL && kernel_vm->pagetable != NULL) {
        pte = walk(kernel_vm->pagetable, ptr, 0, NULL, NULL);
        if (pte == NULL || !pte_present(pte))
            return 0;
        pte = walk(kernel_vm->pagetable, ptr + sizeof(vma_t) - 1,
                   0, NULL, NULL);
        if (pte == NULL || !pte_present(pte))
            return 0;
    } else if (!vm->is_kernel && ptr >= vm->vm_bottom && ptr < vm->vm_top) {
        return 0;
    }

    if (vma->vm != vm)
        return 0;
    if ((vma->start & (PGSIZE - 1)) != 0 || vma->start >= vma->end)
        return 0;
    if (vma->start < vm->vm_bottom || vma->end > vm->vm_top)
        return 0;
    return 1;
}

static void print_fault_vma(vm_t *vm, const char *label, uint64 addr)
{
    vma_t *vma = vm_find_area(vm, addr);
    if (vma == NULL) {
        printf("  %s: addr=0x%lx vma=(none)\n", label, addr);
        return;
    }

    uint64 file_off = vma->pgoff + (addr - vma->start);
    struct vfs_inode *inode = fault_vma_inode(vma);
    printf("  %s: addr=0x%lx map=[0x%lx-0x%lx) %c%c%c pgoff=0x%lx file_off=0x%lx file=%p ino=%lu size=%lld",
           label, addr, vma->start, vma->end,
           (vma->flags & PROT_READ) ? 'r' : '-',
           (vma->flags & PROT_WRITE) ? 'w' : '-',
           (vma->flags & PROT_EXEC) ? 'x' : '-',
           vma->pgoff, file_off, (void *)vma->file,
           inode ? inode->ino : 0, inode ? inode->size : 0);
    print_vma_file_data_end(vma, inode);
    printf(" name=%s path=%s\n", fault_vma_file_name(vma),
           vma_debug_path(vma));
}

static int user_int3_diagnostics_thread_match(struct thread *p)
{
    if (p == NULL)
        return 0;
    if (chrome_lifecycle_thread_match(p))
        return 1;
    if (strstr(p->name, "chrome") != NULL ||
        strstr(p->name, "chromium") != NULL ||
        strstr(p->name, "wayland-chromium") != NULL)
        return 1;
    return 0;
}

static int copy_user_mapped_no_fault(vm_t *vm, void *dst, uint64 srcva,
                                     uint64 len)
{
    unsigned char *out = dst;

    if (vm == NULL || dst == NULL)
        return -EFAULT;

    vm_rlock(vm);
    while (len > 0) {
        uint64 va0 = PGROUNDDOWN(srcva);
        uint64 pa0 = walkaddr(vm->pagetable, va0);
        uint64 page_off = srcva - va0;
        uint64 pa;
        uint64 n;

        if (page_off >= PGSIZE) {
            vm_runlock(vm);
            return -EFAULT;
        }

        pa = pa0 + page_off;
        if (pa < KERNBASE || pa >= PHYSTOP) {
            vm_runlock(vm);
            return -EFAULT;
        }

        n = PGSIZE - page_off;
        if (n > len)
            n = len;
        if (PHYSTOP - pa < n)
            n = PHYSTOP - pa;
        if (n == 0) {
            vm_runlock(vm);
            return -EFAULT;
        }

        memmove(out, PA2VA(pa), n);
        out += n;
        srcva += n;
        len -= n;
    }
    vm_runlock(vm);
    return 0;
}

static void dump_user_int3_diagnostics(struct thread *p, struct trapframe *tf)
{
    unsigned char code[32];
    uint64 stack_words[8];
    uint64 rip;
    uint64 code_start;

    if (p == NULL || p->vm == NULL || tf == NULL)
        return;
    if (!user_int3_diagnostics_thread_match(p))
        return;
    static int emitted;
    if (__atomic_exchange_n(&emitted, 1, __ATOMIC_RELAXED) != 0)
        return;

    rip = tf->rip;
    code_start = rip >= sizeof(code) / 2 ? rip - sizeof(code) / 2 : rip;

    printf("  user-int3: pid=%d tgid=%d name=%s rip=0x%lx rsp=0x%lx rbp=0x%lx\n",
           p->pid, thread_tgid(p), p->name, rip, tf->rsp, tf->rbp);
    printf("  user-int3-regs: rax=0x%lx rbx=0x%lx rcx=0x%lx rdx=0x%lx rdi=0x%lx rsi=0x%lx\n",
           tf->rax, tf->rbx, tf->rcx, tf->rdx, tf->rdi, tf->rsi);
    chrome_syscall_tail_dump_current("user-int3");

    vm_rlock(p->vm);
    print_fault_vma(p->vm, "int3-ip", rip);
    print_fault_vma(p->vm, "int3-rsp", tf->rsp);
    vm_runlock(p->vm);

    if (copy_user_mapped_no_fault(p->vm, code, code_start, sizeof(code)) == 0) {
        printf("  user-int3-code @0x%lx int3_index=%lu:",
               code_start, rip - code_start);
        for (size_t i = 0; i < sizeof(code); i++)
            printf(" %02x", code[i]);
        printf("\n");
    } else {
        printf("  user-int3-code @0x%lx: <unreadable>\n", code_start);
    }

    if (copy_user_mapped_no_fault(p->vm, stack_words, tf->rsp,
                                  sizeof(stack_words)) == 0) {
        static const char *stack_labels[] = {
            "int3-stack[0]",
            "int3-stack[1]",
            "int3-stack[2]",
            "int3-stack[3]",
            "int3-stack[4]",
            "int3-stack[5]",
            "int3-stack[6]",
            "int3-stack[7]",
        };

        printf("  user-int3-stack @rsp:");
        for (size_t i = 0; i < sizeof(stack_words) / sizeof(stack_words[0]); i++)
            printf(" 0x%lx", stack_words[i]);
        printf("\n");
        vm_rlock(p->vm);
        for (size_t i = 0; i < sizeof(stack_words) / sizeof(stack_words[0]); i++) {
            if (stack_words[i] == 0)
                continue;
            print_fault_vma(p->vm, stack_labels[i], stack_words[i]);
        }
        vm_runlock(p->vm);
    } else {
        printf("  user-int3-stack @rsp: <unreadable>\n");
    }
}

static const char *vma_debug_path(vma_t *vma)
{
    if (vma == NULL || vma->file == NULL)
        return "-";
    if (vma->file->opened_path != NULL &&
        vma->file->opened_path[0] != '\0')
        return vma->file->opened_path;
    return fault_vma_file_name(vma);
}

static int kde_exception_vma_trace_enabled(struct thread *p)
{
    char value[16];

    if (p == NULL)
        return 0;
    if (strstr(p->name, "kwin") != NULL ||
        strstr(p->name, "plasmashell") != NULL)
        return 1;
    if (cmdline_get_param("kde_fault_maps", value, sizeof(value)) != 0)
        return 0;
    return value[0] != '\0' && value[0] != '0';
}

static void kde_dump_user_bytes(struct thread *p, const char *label,
                                uint64 addr);

#define KDE_EXCEPTION_ICU_SLOT_FILE_OFF 0x20a9c0UL
#define KDE_EXCEPTION_KWINGLUTILS_SLOT_FILE_TAIL 0x26000UL
#define KDE_EXCEPTION_KWINGLUTILS_SLOT_FILE_END 0x26008UL
#define KDE_EXCEPTION_KWINGLUTILS_SLOT_PLATFORM 0x26040UL
#define KDE_EXCEPTION_WIREPLUMBER_LOG_SPECS_FILE_OFF 0x72188UL

static int kde_exception_vma_covers_file_off(vma_t *vma, uint64 file_off)
{
    uint64 span;

    if (vma == NULL || vma->file == NULL)
        return 0;
    if (file_off < vma->pgoff)
        return 0;

    span = vma->end - vma->start;
    return file_off - vma->pgoff < span;
}

static size_t kde_collect_exception_slot_bytes(uint64 slot_va, uint64 pa,
                                               int huge, unsigned char *bytes,
                                               size_t bytes_len)
{
    uint64 page_off;
    uint64 limit;
    size_t n = bytes_len;

    if (bytes == NULL || n == 0)
        return 0;
    if (pa < KERNBASE || pa >= PHYSTOP)
        return 0;

    page_off = huge ? (slot_va & (HUGEPAGE_SIZE - 1)) :
                      (slot_va & (PGSIZE - 1));
    limit = huge ? HUGEPAGE_SIZE : PGSIZE;
    if (limit - page_off < n)
        n = limit - page_off;
    if (PHYSTOP - pa < n)
        n = PHYSTOP - pa;

    memmove(bytes, PA2VA(pa), n);
    return n;
}

static void kde_print_exception_slot_bytes(const char *tag, uint64 slot_va,
                                           const unsigned char *bytes,
                                           size_t n)
{
    char ascii[32 + 1];

    if (bytes == NULL || n == 0) {
        printf("  kde-exception-mem %s @0x%lx: <no-direct-pa>\n",
               tag, slot_va);
        return;
    }
    if (n > sizeof(ascii) - 1)
        n = sizeof(ascii) - 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = bytes[i];
        ascii[i] = (c >= 0x20 && c <= 0x7e) ? (char)c : '.';
    }
    ascii[n] = '\0';

    printf("  kde-exception-mem %s @0x%lx:", tag, slot_va);
    for (size_t i = 0; i < n; i++)
        printf(" %02x", bytes[i]);
    printf("  \"%s\"\n", ascii);
}

static void kde_dump_exception_file_slot(vm_t *vm, vma_t *vma,
                                         uint64 file_off,
                                         const char *marker,
                                         const char *byte_tag)
{
    uint64 slot_va;
    pte_t *pte;
    pte_t pte_val = 0;
    int present = 0;
    int write = 0;
    int user = 0;
    int huge = 0;
    uint64 pa = 0;
    page_t *page = NULL;
    page_t *head = NULL;
    long page_type = -1;
    long head_type = -1;
    int page_ref = -1;
    int head_ref = -1;
    unsigned char slot_bytes[32];
    size_t slot_bytes_len = 0;

    if (vm == NULL || vm->pagetable == NULL || vma == NULL)
        return;
    if (!kde_exception_vma_covers_file_off(vma, file_off))
        return;

    slot_va = vma->start + (file_off - vma->pgoff);

    vm_pgtable_lock(vm);
    pte = walk(vm->pagetable, slot_va, 0, NULL, NULL);
    if (pte != NULL)
        pte_val = *pte;
    present = pte_present(&pte_val);
    write = pte_write(&pte_val);
    user = pte_user(&pte_val);
    huge = pte_is_hugepage(&pte_val);
    if (present) {
        pa = pte_pa(&pte_val);
        pa += huge ? (slot_va & (HUGEPAGE_SIZE - 1)) :
                     (slot_va & (PGSIZE - 1));
        page = __pa_to_page(PGROUNDDOWN(pa));
        if (page != NULL) {
            page_type = (long)PAGE_FLAG_GET_TYPE(page->flags);
            page_ref = page_ref_count(page);
            if (PAGE_IS_TYPE(page, PAGE_TYPE_TAIL) &&
                page->tail.head_page != NULL) {
                head = page->tail.head_page;
                head_type = (long)PAGE_FLAG_GET_TYPE(head->flags);
                head_ref = page_ref_count(head);
            }
        }
        slot_bytes_len = kde_collect_exception_slot_bytes(slot_va, pa, huge,
                                                          slot_bytes,
                                                          sizeof(slot_bytes));
    }
    vm_pgtable_unlock(vm);

    printf("  %s: file_off=0x%lx va=0x%lx pte=0x%lx present=%d write=%d user=%d huge=%d pa=0x%lx page=%p page_type=%ld page_ref=%d head=%p head_type=%ld head_ref=%d\n",
           marker, file_off, slot_va, pte_val, present, write, user, huge,
           pa, page, page_type, page_ref, head, head_type, head_ref);
    if (present)
        kde_print_exception_slot_bytes(byte_tag, slot_va, slot_bytes,
                                       slot_bytes_len);
}

static void kde_dump_exception_icu_vma(vm_t *vm, vma_t *vma,
                                       struct vfs_inode *inode,
                                       const char *path)
{
    if (vm == NULL || vma == NULL || path == NULL)
        return;
    if (strstr(path, "libicuuc.so.74") == NULL)
        return;

    printf("  kde-exception-icu-vma: map=[0x%lx-0x%lx) %c%c%c %s pgoff=0x%lx file=%p inode_ino=%lu inode_size=%lld",
           vma->start, vma->end,
           (vma->flags & PROT_READ) ? 'r' : '-',
           (vma->flags & PROT_WRITE) ? 'w' : '-',
           (vma->flags & PROT_EXEC) ? 'x' : '-',
           (vma->flags & VMA_FLAG_SHARED) ? "shared" : "private",
           vma->pgoff, (void *)vma->file,
           inode ? inode->ino : 0, inode ? inode->size : 0);
    print_vma_file_data_end(vma, inode);
    printf(" path=%s\n", path);

    kde_dump_exception_file_slot(vm, vma, KDE_EXCEPTION_ICU_SLOT_FILE_OFF,
                                 "kde-exception-icu-slot", "icu-slot");
}

static void kde_dump_exception_kwinglutils_vma(vm_t *vm, vma_t *vma,
                                               struct vfs_inode *inode,
                                               const char *path)
{
    if (vm == NULL || vma == NULL || path == NULL)
        return;
    if (strstr(path, "libkwinglutils.so.14") == NULL)
        return;

    printf("  kde-exception-kwinglutils-vma: map=[0x%lx-0x%lx) %c%c%c %s pgoff=0x%lx file=%p inode_ino=%lu inode_size=%lld",
           vma->start, vma->end,
           (vma->flags & PROT_READ) ? 'r' : '-',
           (vma->flags & PROT_WRITE) ? 'w' : '-',
           (vma->flags & PROT_EXEC) ? 'x' : '-',
           (vma->flags & VMA_FLAG_SHARED) ? "shared" : "private",
           vma->pgoff, (void *)vma->file,
           inode ? inode->ino : 0, inode ? inode->size : 0);
    print_vma_file_data_end(vma, inode);
    printf(" path=%s\n", path);

    kde_dump_exception_file_slot(vm, vma,
                                 KDE_EXCEPTION_KWINGLUTILS_SLOT_FILE_TAIL,
                                 "kde-exception-kwinglutils-slot",
                                 "kwinglutils-slot");
    kde_dump_exception_file_slot(vm, vma,
                                 KDE_EXCEPTION_KWINGLUTILS_SLOT_FILE_END,
                                 "kde-exception-kwinglutils-slot",
                                 "kwinglutils-slot");
    kde_dump_exception_file_slot(vm, vma,
                                 KDE_EXCEPTION_KWINGLUTILS_SLOT_PLATFORM,
                                 "kde-exception-kwinglutils-slot",
                                 "kwinglutils-slot");
}

static void kde_dump_exception_wireplumber_vma(vm_t *vm, vma_t *vma,
                                               struct vfs_inode *inode,
                                               const char *path)
{
    if (vm == NULL || vma == NULL || path == NULL)
        return;
    if (strstr(path, "libwireplumber-0.4.so.0") == NULL)
        return;

    printf("  kde-exception-wireplumber-vma: map=[0x%lx-0x%lx) %c%c%c %s pgoff=0x%lx file=%p inode_ino=%lu inode_size=%lld",
           vma->start, vma->end,
           (vma->flags & PROT_READ) ? 'r' : '-',
           (vma->flags & PROT_WRITE) ? 'w' : '-',
           (vma->flags & PROT_EXEC) ? 'x' : '-',
           (vma->flags & VMA_FLAG_SHARED) ? "shared" : "private",
           vma->pgoff, (void *)vma->file,
           inode ? inode->ino : 0, inode ? inode->size : 0);
    print_vma_file_data_end(vma, inode);
    printf(" path=%s\n", path);

    kde_dump_exception_file_slot(vm, vma,
                                 KDE_EXCEPTION_WIREPLUMBER_LOG_SPECS_FILE_OFF,
                                 "kde-exception-wireplumber-log-specs",
                                 "wireplumber-log-specs");
}

static void kde_dump_exception_vmas(struct thread *p, struct trapframe *tf,
                                    uint64 vec, const char *name)
{
    vm_t *vm;
    void *mt_entry;
    unsigned long mt_idx;
    int found_ip = 0;

    if (p == NULL || p->vm == NULL || tf == NULL)
        return;
    if (!kde_exception_vma_trace_enabled(p))
        return;

    vm = p->vm;
    vm_rlock(vm);
    printf("kde-exception-vmas: pid=%d tgid=%d name=%s vec=%lu exception=%s rip=0x%lx rsp=0x%lx rbp=0x%lx\n",
           p->pid, thread_tgid(p), p->name, vec, name ? name : "???",
           tf->rip, tf->rsp, tf->rbp);
    print_fault_vma(vm, "exception-ip", tf->rip);
    print_fault_vma(vm, "exception-rsp", tf->rsp);
    printf("  --- KDE exception VMA map (exec+file) ---\n");
    mt_for_each(&vm->vm_mt, mt_entry, mt_idx, MAPLE_MAX) {
        vma_t *v = (vma_t *)mt_entry;
        struct vfs_inode *inode;
        const char *path;
        uint64 rip_file_off = 0;
        int has_rip = 0;

        if (!fault_vma_entry_valid(vm, v)) {
            printf("  kde-exception-vma: invalid entry=%p\n", v);
            continue;
        }
        if ((v->flags & PROT_EXEC) == 0 && v->file == NULL)
            continue;

        inode = fault_vma_inode(v);
        path = vma_debug_path(v);
        if (tf->rip >= v->start && tf->rip < v->end) {
            has_rip = 1;
            found_ip = 1;
            rip_file_off = v->pgoff + (tf->rip - v->start);
        }
        printf("  kde-exception-vma:%s [0x%lx-0x%lx) %c%c%c %s pgoff=0x%lx file=%p ino=%lu size=%lld",
               has_rip ? " rip" : "", v->start, v->end,
               (v->flags & PROT_READ) ? 'r' : '-',
               (v->flags & PROT_WRITE) ? 'w' : '-',
               (v->flags & PROT_EXEC) ? 'x' : '-',
               (v->flags & VMA_FLAG_SHARED) ? "shared" : "private",
               v->pgoff, (void *)v->file,
               inode ? inode->ino : 0, inode ? inode->size : 0);
        print_vma_file_data_end(v, inode);
        printf(" path=%s", path);
        if (has_rip)
            printf(" rip_file_off=0x%lx", rip_file_off);
        printf("\n");
        kde_dump_exception_icu_vma(vm, v, inode, path);
        kde_dump_exception_kwinglutils_vma(vm, v, inode, path);
        kde_dump_exception_wireplumber_vma(vm, v, inode, path);
    }
    if (!found_ip)
        printf("  kde-exception-vma: rip mapping not found\n");
    printf("  --- end KDE exception VMA map ---\n");
    vm_runlock(vm);
}

static void kde_dump_user_bytes(struct thread *p, const char *label, uint64 addr)
{
    unsigned char bytes[32];
    char ascii[sizeof(bytes) + 1];

    if (p == NULL || p->vm == NULL || addr == 0)
        return;
    if (!kde_exception_vma_trace_enabled(p))
        return;
    if (vm_copyin(p->vm, bytes, addr, sizeof(bytes)) != 0) {
        printf("  kde-exception-mem %s @0x%lx: <unreadable>\n", label, addr);
        return;
    }
    for (size_t i = 0; i < sizeof(bytes); i++) {
        unsigned char c = bytes[i];
        ascii[i] = (c >= 0x20 && c <= 0x7e) ? (char)c : '.';
    }
    ascii[sizeof(bytes)] = '\0';
    printf("  kde-exception-mem %s @0x%lx:", label, addr);
    for (size_t i = 0; i < sizeof(bytes); i++)
        printf(" %02x", bytes[i]);
    printf("  \"%s\"\n", ascii);
}

static void kde_dump_exception_register_memory(struct thread *p,
                                               struct trapframe *tf)
{
    if (p == NULL || tf == NULL || !kde_exception_vma_trace_enabled(p))
        return;

    kde_dump_user_bytes(p, "rax", tf->rax);
    kde_dump_user_bytes(p, "rbx", tf->rbx);
    kde_dump_user_bytes(p, "rdi", tf->rdi);
    kde_dump_user_bytes(p, "rsi", tf->rsi);
    kde_dump_user_bytes(p, "rcx", tf->rcx);
    kde_dump_user_bytes(p, "r12", tf->r12);
    kde_dump_user_bytes(p, "r13", tf->r13);
    kde_dump_user_bytes(p, "r14", tf->r14);
    kde_dump_user_bytes(p, "r15", tf->r15);
    kde_dump_user_bytes(p, "rsp", tf->rsp);
    kde_dump_user_bytes(p, "rbp", tf->rbp);
}

static void kde_dump_fatal_user_qword(struct thread *p, const char *label,
                                      uint64 addr)
{
    uint64 value;

    if (p == NULL || p->vm == NULL || addr == 0)
        return;
    if (!kde_exception_vma_trace_enabled(p))
        return;

    if (vm_copyin(p->vm, &value, addr, sizeof(value)) != 0) {
        printf("  %s: addr=0x%lx qword=<unreadable>\n", label, addr);
        return;
    }

    printf("  %s: addr=0x%lx qword=0x%lx\n", label, addr, value);
}

static void kde_dump_fatal_pointer_slot(struct thread *p, const char *label,
                                        uint64 base, uint64 offset)
{
    uint64 addr;

    if (p == NULL || p->vm == NULL || base == 0)
        return;
    if ((uint64)-1 - base < offset)
        return;

    addr = base + offset;
    kde_dump_fatal_user_qword(p, label, addr);
    vm_rlock(p->vm);
    print_fault_vma(p->vm, label, addr);
    vm_runlock(p->vm);
    kde_dump_user_bytes(p, label, addr);
}

static void kde_dump_fatal_null_call_slots(struct thread *p,
                                           struct trapframe *tf,
                                           uint64 cr2)
{
    if (p == NULL || tf == NULL)
        return;
    if (tf->rip != 0 || cr2 != 0 || tf->r13 == 0)
        return;

    printf("  fatal-null-call-slots: base=r13=0x%lx\n", tf->r13);
    kde_dump_fatal_pointer_slot(p, "fatal-r13+0x5c0", tf->r13, 0x5c0);
    kde_dump_fatal_pointer_slot(p, "fatal-r13+0x5d0", tf->r13, 0x5d0);
    kde_dump_fatal_pointer_slot(p, "fatal-r13+0x5e0", tf->r13, 0x5e0);
}

static void kde_dump_fatal_lowptr_object_slots(struct thread *p,
                                               struct trapframe *tf,
                                               uint64 cr2)
{
    if (p == NULL || tf == NULL)
        return;
    if (cr2 >= PGSIZE || tf->rdi == 0)
        return;

    printf("  fatal-lowptr-object-slots: base=rdi=0x%lx cr2=0x%lx\n",
           tf->rdi, cr2);
    kde_dump_fatal_pointer_slot(p, "fatal-rdi+0x80", tf->rdi, 0x80);
    kde_dump_fatal_pointer_slot(p, "fatal-rdi+0x88", tf->rdi, 0x88);
    kde_dump_fatal_pointer_slot(p, "fatal-rdi+0x90", tf->rdi, 0x90);
    kde_dump_fatal_pointer_slot(p, "fatal-rdi+0x98", tf->rdi, 0x98);
}

static void kde_dump_fatal_page_fault_summary(struct thread *p,
                                              struct trapframe *tf,
                                              uint64 cr2)
{
    struct fatal_reg_dump {
        const char *name;
        uint64 addr;
    } regs[12];
    uint64 stack_words[8];
    int stack_ok;

    if (p == NULL || p->vm == NULL || tf == NULL)
        return;
    if (!kde_exception_vma_trace_enabled(p))
        return;

    printf("kde-exception-fatal-summary: pid=%d name=%s cr2=0x%lx err=0x%lx rip=0x%lx rsp=0x%lx rbp=0x%lx\n",
           p->pid, p->name, cr2, tf->err, tf->rip, tf->rsp, tf->rbp);
    printf("  regs: rax=0x%lx rbx=0x%lx rcx=0x%lx rdx=0x%lx\n",
           tf->rax, tf->rbx, tf->rcx, tf->rdx);
    printf("        rdi=0x%lx rsi=0x%lx rbp=0x%lx rsp=0x%lx\n",
           tf->rdi, tf->rsi, tf->rbp, tf->rsp);
    printf("        r8 =0x%lx r9 =0x%lx r10=0x%lx r11=0x%lx\n",
           tf->r8, tf->r9, tf->r10, tf->r11);
    printf("        r12=0x%lx r13=0x%lx r14=0x%lx r15=0x%lx\n",
           tf->r12, tf->r13, tf->r14, tf->r15);

    stack_ok = vm_copyin(p->vm, stack_words, tf->rsp, sizeof(stack_words));
    if (stack_ok == 0) {
        printf("  fatal stack @rsp:");
        for (size_t i = 0; i < sizeof(stack_words) / sizeof(stack_words[0]);
             i++)
            printf(" 0x%lx", stack_words[i]);
        printf("\n");

        if (stack_words[0] != 0) {
            vm_rlock(p->vm);
            print_fault_vma(p->vm, "fatal-ret", stack_words[0]);
            vm_runlock(p->vm);
            if (stack_words[0] >= 16)
                kde_dump_user_bytes(p, "fatal-ret-16", stack_words[0] - 16);
        }
    } else {
        printf("  fatal stack @rsp: <unreadable>\n");
    }

    regs[0] = (struct fatal_reg_dump){"fatal-rax", tf->rax};
    regs[1] = (struct fatal_reg_dump){"fatal-rbx", tf->rbx};
    regs[2] = (struct fatal_reg_dump){"fatal-rcx", tf->rcx};
    regs[3] = (struct fatal_reg_dump){"fatal-rdx", tf->rdx};
    regs[4] = (struct fatal_reg_dump){"fatal-rdi", tf->rdi};
    regs[5] = (struct fatal_reg_dump){"fatal-rsi", tf->rsi};
    regs[6] = (struct fatal_reg_dump){"fatal-r12", tf->r12};
    regs[7] = (struct fatal_reg_dump){"fatal-r13", tf->r13};
    regs[8] = (struct fatal_reg_dump){"fatal-r14", tf->r14};
    regs[9] = (struct fatal_reg_dump){"fatal-r15", tf->r15};
    regs[10] = (struct fatal_reg_dump){"fatal-rbp", tf->rbp};
    regs[11] = (struct fatal_reg_dump){"fatal-rsp", tf->rsp};

    vm_rlock(p->vm);
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        if (regs[i].addr == 0)
            continue;
        print_fault_vma(p->vm, regs[i].name, regs[i].addr);
    }
    vm_runlock(p->vm);

    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        if (regs[i].addr == 0)
            continue;
        kde_dump_user_bytes(p, regs[i].name, regs[i].addr);
    }

    kde_dump_fatal_null_call_slots(p, tf, cr2);
    kde_dump_fatal_lowptr_object_slots(p, tf, cr2);
}

static void chrome_dump_icu_vmas(struct thread *p, const char *reason)
{
    if (p == NULL || p->vm == NULL)
        return;
    if (!chrome_lifecycle_thread_match(p) && strncmp(p->name, "exe", 4) != 0)
        return;

    vm_t *vm = p->vm;
    int found = 0;

    vm_rlock(vm);
    vma_t *vma;
    uint64 index = 0;
    mt_for_each(&vm->vm_mt, vma, index, (uint64)(-1ULL)) {
        const char *path = vma_debug_path(vma);
        if (strstr(path, "icudtl.dat") == NULL)
            continue;

        found++;
        printf("chrome-icu-vma: reason=%s pid=%d tgid=%d name=%s map=[0x%lx-0x%lx) %c%c%c %s pgoff=0x%lx file=%p path=%s\n",
               reason, p->pid, thread_tgid(p), p->name, vma->start, vma->end,
               (vma->flags & PROT_READ) ? 'r' : '-',
               (vma->flags & PROT_WRITE) ? 'w' : '-',
               (vma->flags & PROT_EXEC) ? 'x' : '-',
               (vma->flags & VMA_FLAG_SHARED) ? "shared" : "private",
               vma->pgoff, (void *)vma->file, path);
    }
    vm_runlock(vm);

    if (found == 0) {
        printf("chrome-icu-vma: reason=%s pid=%d tgid=%d name=%s found=0\n",
               reason, p->pid, thread_tgid(p), p->name);
    }
}

static int trap_is_fd(struct vfs_file *file)
{
    return (uint64)file > NOFILE;
}

static void chrome_dump_asset_fds(struct thread *p, const char *reason)
{
    struct vfs_fdtable *fdt;
    int found = 0;

    if (p == NULL || p->fdtable == NULL)
        return;
    if (!chrome_lifecycle_thread_match(p) && strncmp(p->name, "exe", 4) != 0)
        return;

    fdt = p->fdtable;
    spin_lock(&fdt->lock);
    for (int fd = 0; fd < NOFILE; fd++) {
        struct vfs_file *file = fdt->files[fd];
        const char *path;
        if (!trap_is_fd(file))
            continue;

        path = file->opened_path;
        if (path == NULL || path[0] == '\0')
            path = "(no-path)";

        if (fd != 3 && strstr(path, "icudtl.dat") == NULL &&
            strstr(path, "v8_context_snapshot.bin") == NULL)
            continue;

        found++;
        printf("chrome-asset-fd: reason=%s pid=%d tgid=%d name=%s fd=%d cloexec=%d kind=%d flags=0x%x file=%p path=%s\n",
               reason, p->pid, thread_tgid(p), p->name, fd,
               !!(fdt->cloexec_bitmap[fd >> 6] & (1ULL << (fd & 63))),
               file->f_kind, file->f_flags, (void *)file, path);
    }
    spin_unlock(&fdt->lock);

    if (found == 0) {
        printf("chrome-asset-fd: reason=%s pid=%d tgid=%d name=%s found=0\n",
               reason, p->pid, thread_tgid(p), p->name);
    }
}

/* ──────────────────────────────────────────────────────────────
 *  User-ABI signal types — exact binary layout that musl (and
 *  Linux) expects on the user stack.  The kernel's own types
 *  (signal_types.h) differ in size, field order and register
 *  layout, so we translate when pushing / restoring signal frames.
 * ────────────────────────────────────────────────────────────── */

typedef long long user_gregset_t[23];

/* gregset_t register indices (must match musl arch/x86_64/bits/signal.h) */
#define UREG_R8      0
#define UREG_R9      1
#define UREG_R10     2
#define UREG_R11     3
#define UREG_R12     4
#define UREG_R13     5
#define UREG_R14     6
#define UREG_R15     7
#define UREG_RDI     8
#define UREG_RSI     9
#define UREG_RBP    10
#define UREG_RBX    11
#define UREG_RDX    12
#define UREG_RAX    13
#define UREG_RCX    14
#define UREG_RSP    15
#define UREG_RIP    16
#define UREG_EFL    17
#define UREG_CSGSFS 18
#define UREG_ERR    19
#define UREG_TRAPNO 20
#define UREG_OLDMASK 21
#define UREG_CR2    22

/* musl: mcontext_t = { gregset_t gregs; fpregset_t fpregs; ull __reserved1[8]; }
 *       sizeof = 23*8 + 8 + 8*8 = 256 bytes */
typedef struct {
    user_gregset_t gregs;          /* 184 bytes */
    void          *fpregs;         /*   8 bytes (pointer to fpstate) */
    uint64         __reserved1[8]; /*  64 bytes */
} user_mcontext_t;

_Static_assert(sizeof(user_mcontext_t) == 256,
               "user_mcontext_t must be 256 bytes (musl ABI)");

/* musl: sigset_t = { unsigned long __bits[128/sizeof(long)]; } = 128 bytes */
typedef struct {
    unsigned long __bits[128 / sizeof(unsigned long)];
} user_sigset_t;

_Static_assert(sizeof(user_sigset_t) == 128,
               "user_sigset_t must be 128 bytes (musl ABI)");

/* musl: ucontext_t */
typedef struct user_ucontext {
    unsigned long            uc_flags;
    struct user_ucontext    *uc_link;
    stack_t                  uc_stack;           /*  24 bytes */
    user_mcontext_t          uc_mcontext;        /* 256 bytes */
    user_sigset_t            uc_sigmask;          /* 128 bytes */
    unsigned long            __fpregs_mem[64];   /* 512 bytes */
} user_ucontext_t;
#define USER_FAULT_AROUND_SIZE  (USER_FAULT_AROUND_PAGES * PGSIZE)
#define USER_WRITE_FAULT_AROUND_PAGES 4UL
#define USER_WRITE_FAULT_AROUND_SIZE  (USER_WRITE_FAULT_AROUND_PAGES * PGSIZE)

extern pagetable_t kernel_pagetable;

/* ── Provided by trapvec.S — table of 256 stub addresses ── */
extern uint64 vectors[];

/* ── Trampoline stubs (linker compatibility) ── */
extern uint64 trampoline_ksatp; /* defined in trapvec.S */
extern char trampoline[];
extern void sig_trampoline(void); /* defined in sig_trampoline.S */

extern char userret[];
static void (*trampoline_userret)(uint64 tf, uint64 user_cr3,
                                  uint64 user_gs, uint64 kernel_gs,
                                  uint64 invalidate_trapframe) = NULL;
static int x86_cr3_noflush_enabled;

static int x86_cr3_noflush_enabled_by_cmdline(void)
{
    char value[16];

    /*
     * OPT-IN ONLY (see x86_pcid_enabled_by_cmdline): the 2026-07-02
     * default-on attempt reproduced the KWin corruption signature.
     */
    if (cmdline_get_param("x86_cr3_noflush", value, sizeof(value)) != 0)
        return 0;
    return value[0] == '1';
}

/* ── Debugcon helpers (forward declarations for use in usertrapret) ── */
static inline void dbg_outb(uint16 port, uint8 val);
static void dbg_puts(const char *s);
static void dbg_hex(uint64 v);

/*
 * usertrapret — return to user mode.
 *
 * Sets up kernel state in the utrapframe, maps the trapframe page,
 * prepares the trapframe and kernel stack metadata, then lets the final
 * assembly trampoline install the user/kernel GS bases immediately before
 * iretq.  Keeping GS_BASE as per-CPU throughout this C function avoids
 * kernel-mode exception paths observing a user GS base.
 */
void usertrapret(void) {
    struct thread *p = current;
    struct trapframe *tf = &p->trapframe->trapframe;
    uint32 rseq_events = 0;

    oom_process_deferred_kill();

    if (killed(p))
        exit(-1);

    if (THREAD_SIGPENDING(p))
        rseq_events |= RSEQ_EVENT_SIGNAL;
    rseq_user_return(p, rseq_events);
    if (killed(p))
        exit(-1);

    handle_signal();

    /* handle_signal() may have marked us killed (e.g. unhandled SIGSEGV). */
    if (killed(p))
        exit(-1);

    /* Check if GDB wants to interrupt this process (Ctrl-C). */
    {
        extern int gdbstub_check_interrupt(struct thread *);
        gdbstub_check_interrupt(p);
    }

    if (NEEDS_RESCHED()) {
        scheduler_yield();
        rseq_user_return(p, RSEQ_EVENT_PREEMPT);
        if (killed(p))
            exit(-1);
    }

    /* Disable interrupts while setting up the return path. */
    intr_off();

    struct cpu_local *my_cpu = mycpu();
    int cpu = cpuid();

    /* Restore user FS base only when this CPU is not already current. */
    if (!my_cpu->user_fs_base_valid ||
        my_cpu->user_fs_base != p->trapframe->tp) {
        wrmsr(MSR_FS_BASE, p->trapframe->tp);
        my_cpu->user_fs_base = p->trapframe->tp;
        my_cpu->user_fs_base_valid = 1;
    }

    /*
     * TSS.RSP0 = higher-half VA of the thread kstack.
     * With IST=0 for most vectors, the CPU loads RSP from RSP0 on
     * user→kernel transitions.  Must be a VA accessible from user
     * page tables (PML4[256+] shared higher-half).
     */
    x86_tss_set_rsp0_cpu(cpu, (uint64)PA2VA(p->ksp));

    /* Store kernel state in utrapframe for next trap entry. */
    p->trapframe->kernel_sp = p->ksp;
    p->trapframe->kernel_satp = (uint64)kernel_pagetable; /* RISC-V compat */

    /*
     * Tell alltraps where to pivot RSP for user->kernel transitions.
     * Most IDT vectors use IST=0, so the CPU loads RSP from TSS.RSP0
     * on user→kernel transitions.  alltraps reads intr_kstack_top
     * (which equals RSP0) and copies the 7-qword interrupt frame —
     * a no-op for non-IST vectors since source==destination — then
     * pivots RSP so all subsequent C code runs on the per-thread
     * kernel stack.  For IST vectors (NMI, #DF) the copy actually
     * moves the frame from the IST stack to kstack.
     *
     * Store as higher-half VA (PA2VA) so alltraps can access the
     * kstack via PML4[256] without a CR3 switch to the kernel page
     * table.  The C handler performs the CR3 switch later for
     * identity-map access.
     *
     * SMP-safe: stored in per-CPU struct via saved my_cpu pointer.
     */
    my_cpu->intr_kstack_top = (uint64)PA2VA(p->ksp);

    /* Guard: if vm was freed (process being torn down), don't proceed. */
    if (p->vm == NULL || p->vm->pagetable == NULL) {
        printf("usertrapret: pid %d '%s' has no VM, exiting\n", p->pid,
               p->name);
        intr_on();
        exit(-1);
    }

    /* Validate trapframe_pte before vm_cpu_online */
    if (p->vm->trapframe_pte) {
        uint64 pte_val = (uint64)p->vm->trapframe_pte;
        if (pte_val < 0x100000UL) {
            panic("usertrapret: BAD trapframe_pte");
        }
    }
    /* Mark the current CPU offline for the kernel VM (reverse of user VM) */
    vm_cpu_offline(kernel_vm, cpu);

    bool trapframe_pte_changed = false;
    uint64 trapframe_base =
        vm_cpu_online_trapframe(p->vm, cpu, p, &trapframe_pte_changed);

    /*
     * Store the kernel stack top for the next SYSCALL entry.
     * syscall_entry reads these per-CPU fields via %gs after swapgs.
     * Stored as higher-half VA (PA2VA) so syscall_entry can access
     * them via PML4[256] without a CR3 switch.
     */
    my_cpu->syscall_kstack_top = (uint64)PA2VA(p->ksp);
    my_cpu->syscall_trapframe = (uint64)PA2VA((uint64)p->trapframe);

    /* Ensure user RFLAGS is valid and has IF set.
     * Bit 1 in RFLAGS is architecturally fixed to 1.
     */
    tf->rflags |= (0x200 | 0x2);

    if (trampoline_userret == NULL)
        panic("usertrapret: trampoline_userret not initialized");

    /* Lazy FPU: if this thread owns the FPU on this CPU, clear TS so
     * FP/SSE works in user mode.  Otherwise set TS so the first FP
     * instruction triggers #NM for lazy switching. */
    {
        uint64 cr0;
        asm volatile("movq %%cr0, %0" : "=r"(cr0));
        if (THREAD_FPU_USED(p) &&
            my_cpu->fpu_owner_tid == p->pid &&
            p->fpu_seq == my_cpu->fpu_seq)
            cr0 &= ~(1ULL << 3); /* clear CR0.TS */
        else
            cr0 |= (1ULL << 3); /* set CR0.TS   */
        asm volatile("movq %0, %%cr0" : : "r"(cr0));
    }

    /* Final sanity check — should not happen after the earlier guard,
     * but if it does, kill the process instead of panicking. */
    if (p->vm == NULL || p->vm->pagetable == NULL) {
        printf("usertrapret: pid %d '%s' lost VM before iretq\n", p->pid,
               p->name);
        intr_on();
        exit(-1);
    }

    /* Per-CPU ASID/PCID generation check: if the global generation has
     * advanced past what this CPU last saw, flush the entire TLB so stale
     * entries from a recycled PCID cannot be served. */
    uint16 cur_gen = vm_asid_gen();
    if (my_cpu->asid_gen != cur_gen) {
        sfence_vma_global();
        my_cpu->asid_gen = cur_gen;
    }

    /*
     * x86_cr3_noflush=1 is opt-in and only active with PCID.  When the
     * TRAPFRAME slot changed, userret invalidates it after loading the user
     * CR3 so invlpg targets the user PCID, not the kernel PCID.
     */
    int noflush_enabled = x86_cr3_noflush_enabled && vm_asid_max() > 0;
    uint64 invalidate_trapframe = noflush_enabled && trapframe_pte_changed;
    uint64 user_cr3;
    if (vm_asid_max() > 0) {
        user_cr3 = MAKE_SATP_PCID((uint64)p->vm->pagetable,
                                   p->vm->asid, noflush_enabled);
    } else {
        user_cr3 = (uint64)p->vm->pagetable;
    }

    trampoline_userret(trapframe_base, user_cr3,
                        p->trapframe->user_gs_base, (uint64)my_cpu,
                        invalidate_trapframe);

    __builtin_unreachable();
}

int push_sigframe(struct thread *p, int signo, sigaction_t *sa,
                  ksiginfo_t *info) {
    if (sa == NULL || sa->sa_handler == NULL || p == NULL)
        return -1;

    /*
     * Determine the user stack to push the signal frame onto.
     * SA_ONSTACK: use the alternate signal stack if available.
     */
    uint64 new_sp;
    if ((sa->sa_flags & SA_ONSTACK) != 0 &&
        (p->signal.sig_stack.ss_flags & (SS_ONSTACK | SS_DISABLE)) == 0) {
        if (p->signal.sig_stack.ss_size < MINSIGSTKSZ)
            return -1;
        new_sp =
            (uint64)p->signal.sig_stack.ss_sp + p->signal.sig_stack.ss_size;
    } else {
        new_sp = p->trapframe->trapframe.rsp;
    }

    /* 128-byte red zone — the System V AMD64 ABI reserves 128 bytes
     * below RSP that must not be clobbered by signal delivery. */
    new_sp -= 128;
    new_sp &= ~0xFUL;

    /* Reserve space for the user-ABI ucontext_t on the user stack. */
    uint64 uc_addr = (new_sp - sizeof(user_ucontext_t)) & ~0xFUL;

    /* Reserve space for siginfo_t if SA_SIGINFO. */
    uint64 si_addr = 0;
    if (sa->sa_flags & SA_SIGINFO) {
        assert(info != NULL,
               "push_sigframe: info is NULL when SA_SIGINFO is set");
        si_addr = (uc_addr - sizeof(siginfo_t)) & ~0xFUL;
        new_sp = si_addr;
    } else {
        new_sp = uc_addr;
    }

    /*
     * Push the return address (SIG_TRAMPOLINE) so that when the
     * signal handler executes RET it lands in the trampoline which
     * calls sys_sigreturn.
     *
     * System V AMD64 ABI requires RSP to be 16-byte aligned BEFORE
     * the CALL instruction, i.e. RSP % 16 == 0 *before* the 8-byte
     * return address is pushed, so RSP % 16 == 8 on function entry.
     */
    new_sp = (new_sp & ~0xFUL) - 8;
    uint64 ret_addr = SIG_TRAMPOLINE;

    /* Grow the stack VMA if necessary. */
    if ((sa->sa_flags & SA_ONSTACK) == 0) {
        if (p->vm == NULL)
            exit(-1);
        if (vm_try_growstack(p->vm, new_sp) != 0)
            exit(-1);
    }

    /* Build the user-ABI ucontext (matches musl's layout). */
    user_ucontext_t uc;
    memset(&uc, 0, sizeof(uc));
    uc.uc_flags = 0;
    uc.uc_link  = (struct user_ucontext *)p->signal.sig_ucontext;
    memmove(&uc.uc_stack, &p->signal.sig_stack, sizeof(stack_t));

    /* ── Translate kernel trapframe → musl gregset_t ── */
    struct trapframe *tf = &p->trapframe->trapframe;
    user_gregset_t *gregs = &uc.uc_mcontext.gregs;
    (*gregs)[UREG_R8]      = tf->r8;
    (*gregs)[UREG_R9]      = tf->r9;
    (*gregs)[UREG_R10]     = tf->r10;
    (*gregs)[UREG_R11]     = tf->r11;
    (*gregs)[UREG_R12]     = tf->r12;
    (*gregs)[UREG_R13]     = tf->r13;
    (*gregs)[UREG_R14]     = tf->r14;
    (*gregs)[UREG_R15]     = tf->r15;
    (*gregs)[UREG_RDI]     = tf->rdi;
    (*gregs)[UREG_RSI]     = tf->rsi;
    (*gregs)[UREG_RBP]     = tf->rbp;
    (*gregs)[UREG_RBX]     = tf->rbx;
    (*gregs)[UREG_RDX]     = tf->rdx;
    (*gregs)[UREG_RAX]     = tf->rax;
    (*gregs)[UREG_RCX]     = tf->rcx;
    (*gregs)[UREG_RSP]     = tf->rsp;
    (*gregs)[UREG_RIP]     = tf->rip;
    (*gregs)[UREG_EFL]     = tf->rflags;
    (*gregs)[UREG_CSGSFS]  = tf->cs;  /* cs (gs/fs not tracked separately) */
    (*gregs)[UREG_ERR]     = tf->err;
    (*gregs)[UREG_TRAPNO]  = tf->trapno;
    (*gregs)[UREG_OLDMASK] = 0;
    /*
     * Linux exposes the faulting linear address in both siginfo.si_addr and
     * mcontext.gregs[REG_CR2] for synchronous SIGSEGV/SIGBUS delivery.  Some
     * runtimes inspect the ucontext directly when implementing guarded-memory
     * or JIT fault handlers, so preserve the address instead of zeroing it.
     */
    if (info != NULL && (signo == SIGSEGV || signo == SIGBUS))
        (*gregs)[UREG_CR2] = (uint64)info->info.si_addr;
    else
        (*gregs)[UREG_CR2] = 0;

    /* ── Signal mask: kernel sigset_t (8 bytes) → user sigset_t (128 bytes) ── */
    uc.uc_sigmask.__bits[0] = p->signal.sig_mask;

    /* ── Save FP state into __fpregs_mem if this thread has used FP ── */
    if (THREAD_FPU_USED(p) && p->fpu_state != NULL) {
        if (mycpu()->fpu_owner_tid == p->pid &&
            p->fpu_seq == mycpu()->fpu_seq) {
            asm volatile("clts");
            fpu_save_state(p->fpu_state);
        }
        memmove(&uc.__fpregs_mem, p->fpu_state, X86_FPU_LEGACY_SIZE);
        /* fpregs pointer → user-space address of __fpregs_mem area */
        uc.uc_mcontext.fpregs =
            (void *)(uc_addr + __builtin_offsetof(user_ucontext_t, __fpregs_mem));
    }

    /* Copy ucontext to user stack. */
    if (vm_copyout(p->vm, uc_addr, (void *)&uc, sizeof(user_ucontext_t)) != 0)
        return -1;

    /* Copy siginfo to user stack. */
    if (sa->sa_flags & SA_SIGINFO) {
        if (vm_copyout(p->vm, si_addr, &info->info, sizeof(siginfo_t)) != 0)
            return -1;
    }

    /* Write the return address (trampoline) below the frame. */
    if (vm_copyout(p->vm, new_sp, (void *)&ret_addr, sizeof(ret_addr)) != 0)
        return -1;

    /*
     * Redirect execution to the signal handler.
     * x86_64 System V calling convention: RDI, RSI, RDX, …
     */
    p->trapframe->trapframe.rip = (uint64)sa->sa_handler;
    p->trapframe->trapframe.rsp = new_sp;
    arch_tf_set_arg0(p->trapframe, (uint64)signo); /* arg1: signal number  */
    arch_tf_set_arg1(p->trapframe, si_addr);       /* arg2: siginfo_t *    */
    arch_tf_set_arg2(p->trapframe, uc_addr);       /* arg3: ucontext_t *   */

    p->signal.sig_ucontext = uc_addr;

    return 0;
}

int restore_sigframe(struct thread *p, ucontext_t *ret_uc) {
    uint64 sig_ucontext = p->signal.sig_ucontext;

    if (sig_ucontext == 0 || ret_uc == NULL)
        return -1;

    /* Copy the user-ABI ucontext back from user memory. */
    user_ucontext_t user_uc;
    if (vm_copyin(p->vm, (void *)&user_uc, sig_ucontext,
                  sizeof(user_ucontext_t)) != 0)
        return -1;

    /* ── Translate musl gregset_t → kernel trapframe ── */
    struct trapframe *tf = &p->trapframe->trapframe;
    user_gregset_t *gregs = &user_uc.uc_mcontext.gregs;
    tf->r8     = (*gregs)[UREG_R8];
    tf->r9     = (*gregs)[UREG_R9];
    tf->r10    = (*gregs)[UREG_R10];
    tf->r11    = (*gregs)[UREG_R11];
    tf->r12    = (*gregs)[UREG_R12];
    tf->r13    = (*gregs)[UREG_R13];
    tf->r14    = (*gregs)[UREG_R14];
    tf->r15    = (*gregs)[UREG_R15];
    tf->rdi    = (*gregs)[UREG_RDI];
    tf->rsi    = (*gregs)[UREG_RSI];
    tf->rbp    = (*gregs)[UREG_RBP];
    tf->rbx    = (*gregs)[UREG_RBX];
    tf->rdx    = (*gregs)[UREG_RDX];
    tf->rax    = (*gregs)[UREG_RAX];
    tf->rcx    = (*gregs)[UREG_RCX];
    tf->rsp    = (*gregs)[UREG_RSP];
    tf->rip    = (*gregs)[UREG_RIP];
    tf->rflags = (*gregs)[UREG_EFL];
    tf->cs     = (*gregs)[UREG_CSGSFS];
    tf->err    = (*gregs)[UREG_ERR];
    tf->trapno = (*gregs)[UREG_TRAPNO];

    /* Sanitise CS/SS — only allow user-mode segments. */
    tf->cs = SEG_UCODE | DPL_USER;
    tf->ss = SEG_UDATA | DPL_USER;

    /* Sanitise RFLAGS — preserve user-safe bits only. */
    tf->rflags = (tf->rflags & 0x3C0FD5UL) | 0x200; /* keep safe bits, force IF */

    /* ── Populate the kernel ucontext for signal_restore() ── */
    ret_uc->uc_link    = (ucontext_t *)(unsigned long)user_uc.uc_link;
    memmove(&ret_uc->uc_stack, &user_uc.uc_stack, sizeof(stack_t));
    ret_uc->uc_sigmask = user_uc.uc_sigmask.__bits[0]; /* first 64 signals */

    /* ── Restore FP state from __fpregs_mem if it was saved ── */
    if (user_uc.uc_mcontext.fpregs != NULL && p->fpu_state != NULL) {
        memmove(p->fpu_state, &user_uc.__fpregs_mem, X86_FPU_LEGACY_SIZE);
        if (mycpu()->fpu_owner_tid == p->pid)
            mycpu()->fpu_owner_tid = 0;
    }

    return 0;
}

/* ─────────────────── I/O helpers ─────────────────── */

static inline void outb(uint16 port, uint8 val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8 inb(uint16 port) {
    uint8 val;
    asm volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* ─────────────────── PIC 8259A ─────────────────── */

#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1

#define PIC_EOI 0x20

/* IRQ vector base: IRQ 0 → vector 32 */
#define IRQ_BASE 32

static void pic_init(void) {
    /* ICW1: begin init + ICW4 needed */
    outb(PIC1_CMD, 0x11);
    outb(PIC2_CMD, 0x11);

    /* ICW2: vector offsets */
    outb(PIC1_DATA, IRQ_BASE);     /* master: IRQ 0-7  → vectors 32-39 */
    outb(PIC2_DATA, IRQ_BASE + 8); /* slave:  IRQ 8-15 → vectors 40-47 */

    /* ICW3: cascade wiring */
    outb(PIC1_DATA, 0x04); /* master: slave on IRQ 2 */
    outb(PIC2_DATA, 0x02); /* slave:  cascade identity 2 */

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);

    /* Mask ALL IRQs — we use the I/O APIC now */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

/* ── IDT ── */
static struct idt_gate idt[IDT_ENTRIES] __attribute__((aligned(4096)));
static struct x86_desc_ptr idtr;

void x86_idt_init(void) {
    extern void vector0(void);
    uint64 trapvec_page0 = PGROUNDDOWN((uint64)vector0);

    /*
     * Most vectors use IST=0: user→kernel transitions load RSP from
     * TSS.RSP0 (per-thread kstack); kernel→kernel traps stay on the
     * current stack.  This avoids the IST reentrancy problem where a
     * nested kernel exception (e.g. #PF for lazy PML4 sync) reloads
     * the same IST pointer and overwrites the outer handler's frame.
     *
     * Only a few critical vectors keep IST to guarantee a known-good
     * stack even when the current stack is corrupt or unavailable:
     *   NMI  (vec 2)  → IST=1   (can arrive at any time)
     *   #DF  (vec 8)  → IST=2   (fires when stack is corrupt)
     */
    for (int i = 0; i < IDT_ENTRIES; i++)
        idt_set_gate(&idt[i], TRAPVEC_ALIAS_BASE + (vectors[i] - trapvec_page0),
                     SEG_KCODE, GATE_INTERRUPT, 0);

    /* NMI → IST=1 */
    idt_set_gate(&idt[2], TRAPVEC_ALIAS_BASE + (vectors[2] - trapvec_page0),
                 SEG_KCODE, GATE_INTERRUPT, 1);

    /* #DF → IST=2 */
    idt_set_gate(&idt[8], TRAPVEC_ALIAS_BASE + (vectors[8] - trapvec_page0),
                 SEG_KCODE, GATE_INTERRUPT, 2);

    /* Vector 3 (INT3 / breakpoint) must be DPL=3 so user-mode
     * processes can trigger it for GDB breakpoints / waitgdb. */
    idt_set_gate(&idt[3], TRAPVEC_ALIAS_BASE + (vectors[3] - trapvec_page0),
                 SEG_KCODE, GATE_USER_INT, 0);

    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64)idt;
    asm volatile("lidt %0" : : "m"(idtr));
}

void x86_idt_remap(uint64 idt_va) {
    idtr.base = idt_va;
    asm volatile("lidt %0" : : "m"(idtr));
}

uint64 x86_idt_page_pa(void) { return (uint64)idt; }

/* ── arch_trap_init / arch_trap_init_hart ── */

void arch_trap_init(void) {
    trampoline_userret =
        (void *)(TRAMPOLINE + ((uint64)userret - (uint64)trampoline));
    x86_cr3_noflush_enabled =
        x86_cr3_noflush_enabled_by_cmdline() && vm_asid_max() > 0;
    x86_gdt_init();
    x86_idt_init();
    x86_syscall_init();

    /*
     * Remap GDT+TSS and IDT to high-canonical addresses (CPU_ENTRY_AREA)
     * so they are accessible when running under user CR3.
     * iretq reads the GDT to validate CS/SS, and the IDT/TSS must be
     * reachable for fault delivery during ring transitions.
     * The physical pages were already mapped in arch_vm_init().
     */
    x86_gdt_remap(CPU_ENTRY_GDT);
    x86_idt_remap(CPU_ENTRY_IDT);
}

void arch_trap_init_hart(void) {
    static int bsp_trap_done = 0;
    if (!bsp_trap_done) {
        /* BSP: GDT/TSS/SYSCALL already set up by arch_trap_init() */
        bsp_trap_done = 1;
    } else {
        /* AP: load GDT, TSS, and SYSCALL MSRs */
        x86_gdt_init_ap();
    }
    x86_tss_set_ist1(mycpu()->intr_sp);

    /*
     * IST2 (#DF) stack: use the lower half of the per-CPU interrupt
     * stack.  IST1 (NMI) uses the top half (intr_sp points to the top
     * of the full 16KB stack; IST2 gets the midpoint).
     */
    x86_tss_set_ist2(mycpu()->intr_sp - INTR_STACK_SIZE / 2);
    asm volatile("lidt %0" : : "m"(idtr));
}

/* ── Interrupt controller init ── */

/* Defined in timer/timer.c */
extern void arch_timer_init(void);

/* Forward declarations for debugcon helpers defined later */
static inline void dbg_outb(uint16 port, uint8 val);
static void dbg_puts(const char *s);
static void dbg_hex(uint64 v);

void arch_irq_init(void) {
    dbg_puts("[x86] arch_irq_init: begin\n");

    /* Remap and mask the legacy PIC so it doesn't interfere */
    pic_init();
    dbg_puts("[x86] arch_irq_init: PIC masked\n");

    /* Initialize the Local APIC on this CPU */
    lapic_init();
    dbg_puts("[x86] arch_irq_init: LAPIC done\n");

    /* Initialize the I/O APIC — masks all redirection entries */
    ioapic_init();
    dbg_puts("[x86] arch_irq_init: IOAPIC done\n");

    /* Select and start the tick timer (LAPIC/HPET/PIT) */
    arch_timer_init();

    /* If the chosen timer uses IRQ 0 (PIT or HPET), route it via IOAPIC */
    extern int arch_timer_needs_ioapic_irq0(void);
    int bsp_id = lapic_id();
    if (arch_timer_needs_ioapic_irq0())
        ioapic_enable(0, T_IRQ0 + 0, bsp_id); /* IRQ 0 → vector 32 → BSP */

    dbg_puts("[x86] arch_irq_init: APIC initialized, bsp_id=");
    dbg_hex((uint64)bsp_id);
    dbg_puts("\n");
}

void arch_irq_init_hart(void) {
    /* Per-CPU LAPIC init for secondary CPUs.
     * Skip for BSP — arch_irq_init() already set up the LAPIC and timer.
     * Calling lapic_init() again would re-mask the LVT timer entry. */
    static int bsp_done = 0;
    if (!bsp_done) {
        bsp_done = 1;
        return; /* BSP: already initialized in arch_irq_init() */
    }
    lapic_init();
    extern void arch_timer_init_hart(void);
    arch_timer_init_hart();
}

/*
 * plic_enable_irq — called by register_irq_handler() for device IRQs.
 *
 * On x86, this programs the I/O APIC to route the given ISA IRQ
 * to the BSP's LAPIC at vector T_IRQ0 + irq.
 */
void plic_enable_irq(int irq) {
    ioapic_enable(irq, T_IRQ0 + irq, 0 /* BSP LAPIC ID */);
}

/*
 * plic_enable_irq_level - enable a PCI device IRQ with level-trigger.
 *
 * PCI interrupts are level-triggered, active-low.  Call this after
 * register_irq_handler() for PCI devices to reprogram the IOAPIC entry.
 */
void plic_enable_irq_level(int irq) {
    ioapic_enable_level(irq, T_IRQ0 + irq, 0 /* BSP LAPIC ID */);
}

/* These are only used on RISC-V; on x86 the trap handler dispatches
 * directly and sends LAPIC EOI. */
int plic_claim(void) { return 0; }
void plic_complete(int irq) { (void)irq; }

/* ── Exception name table ── */

static const char *exception_names[32] = {
    [0] = "#DE Divide Error",
    [1] = "#DB Debug",
    [2] = "NMI",
    [3] = "#BP Breakpoint",
    [4] = "#OF Overflow",
    [5] = "#BR Bound Range",
    [6] = "#UD Invalid Opcode",
    [7] = "#NM Device Not Available",
    [8] = "#DF Double Fault",
    [9] = "(reserved)",
    [10] = "#TS Invalid TSS",
    [11] = "#NP Segment Not Present",
    [12] = "#SS Stack-Segment Fault",
    [13] = "#GP General Protection",
    [14] = "#PF Page Fault",
    [15] = "(reserved)",
    [16] = "#MF x87 FP Exception",
    [17] = "#AC Alignment Check",
    [18] = "#MC Machine Check",
    [19] = "#XM SIMD FP Exception",
    [20] = "#VE Virtualization",
    [21] = "#CP Control Protection",
    [22] = "(reserved)",
    [23] = "(reserved)",
    [24] = "(reserved)",
    [25] = "(reserved)",
    [26] = "(reserved)",
    [27] = "(reserved)",
    [28] = "(reserved)",
    [29] = "#SX Security Exception",
    [30] = "#HV VMM Communication",
    [31] = "(reserved)",
};

/* ── Debugcon helpers ── */
static inline void dbg_outb(uint16 port, uint8 val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static void dbg_puts(const char *s) {
    while (*s)
        dbg_outb(0xE9, (uint8)*s++);
}
static void dbg_hex(uint64 v) {
    static const char h[] = "0123456789abcdef";
    for (int i = 15; i >= 0; i--)
        dbg_outb(0xE9, (uint8)h[(v >> (i * 4)) & 0xF]);
}

static inline uint64 read_cr2(void) {
    uint64 v;
    asm volatile("movq %%cr2, %0" : "=r"(v));
    return v;
}

static int kde_xwayland_fault_trace_enabled(void)
{
    char value[16];

    if (cmdline_get_param("kde_xwayland_fault_trace", value,
                          sizeof(value)) != 0)
        return 0;
    return value[0] != '\0' && value[0] != '0';
}

static void kde_xwayland_fault_trace(struct thread *p, struct trapframe *tf,
                                     uint64 cr2, vma_t *known_vma)
{
    uint8 code[16];
    uint64 stack_words[6];
    int code_ok = -1;
    int stack_ok = -1;

    if (p == NULL || p->vm == NULL || tf == NULL)
        return;
    if (!kde_xwayland_fault_trace_enabled())
        return;
    if (strstr(p->name, "Xwayland") == NULL)
        return;

    if (tf->rip >= sizeof(code) / 2)
        code_ok = vm_copyin(p->vm, code, tf->rip - sizeof(code) / 2,
                            sizeof(code));
    stack_ok = vm_copyin(p->vm, stack_words, tf->rsp, sizeof(stack_words));

    printf("kde-xwayland-fault: pid=%d tgid=%d name='%s' cr2=0x%lx err=0x%lx rip=0x%lx rsp=0x%lx rbp=0x%lx handler=yes vma=%p\n",
           p->pid, thread_tgid(p), p->name, cr2, tf->err, tf->rip, tf->rsp,
           tf->rbp, known_vma);
    printf("  regs: rax=0x%lx rbx=0x%lx rcx=0x%lx rdx=0x%lx\n",
           tf->rax, tf->rbx, tf->rcx, tf->rdx);
    printf("        rdi=0x%lx rsi=0x%lx r8=0x%lx r9=0x%lx\n",
           tf->rdi, tf->rsi, tf->r8, tf->r9);
    printf("        r10=0x%lx r11=0x%lx r12=0x%lx r13=0x%lx r14=0x%lx r15=0x%lx\n",
           tf->r10, tf->r11, tf->r12, tf->r13, tf->r14, tf->r15);

    if (code_ok == 0) {
        printf("  code @rip-8:");
        for (size_t i = 0; i < sizeof(code); i++)
            printf(" %02x", code[i]);
        printf("\n");
    } else {
        printf("  code @rip-8: <unreadable>\n");
    }

    if (stack_ok == 0) {
        printf("  stack @rsp:");
        for (size_t i = 0; i < sizeof(stack_words) / sizeof(stack_words[0]);
             i++)
            printf(" 0x%lx", stack_words[i]);
        printf("\n");
    } else {
        printf("  stack @rsp: <unreadable>\n");
    }

    vm_rlock(p->vm);
    print_fault_vma(p->vm, "kde-xwayland-ip", tf->rip);
    print_fault_vma(p->vm, "kde-xwayland-va", cr2);
    vm_runlock(p->vm);

    print_user_backtrace(p->vm->pagetable, tf->rbp, tf->rip, tf->rsp,
                         tf->rip, 16);
}

/* ── Timer tick advance (defined in timer.c) ── */
extern void timer_tick_advance(void);

/* ── Device IRQ dispatch (defined in kernel/irq/irq.c) ── */
extern int do_device_irq(int hw_irq);

/* ── x86_trap_handler ── */

void x86_trap_handler(struct trapframe *tf) {
    uint64 vec = tf->trapno;
    int from_user = ((tf->cs & 3) == 3);

    /*
     * Switch to the kernel page table to gain identity-map access
     * (PML4[0]).  User page tables share PML4[256-511] (kernel text,
     * data, trampoline) but not PML4[0], which is needed for
     * PA-as-pointer dereferences (page_alloc results, kstack, etc.).
     *
     * This replaces the assembly-level CR3 switch that was previously
     * in alltraps.  Conditional to avoid an unnecessary TLB flush
     * when handling kernel-mode traps (CR3 is already correct).
     */
    {
        uint64 __cr3;
        asm volatile("movq %%cr3, %0" : "=r"(__cr3));
        if (__cr3 != trampoline_ksatp)
            asm volatile("movq %0, %%cr3" : : "r"(trampoline_ksatp) : "memory");
    }

    /*
     * For user-mode traps, copy the stack-based trapframe into the thread's
     * utrapframe so that syscall arg fetching and other code that reads
     * p->trapframe->trapframe works correctly.
     *
     * Also save user GS base. User FS base lives in trapframe->tp:
     * FSGSBASE is not enabled, so only arch_prctl()/clone TLS changes it.
     */
    if (from_user && current && current->trapframe) {
        current->trapframe->trapframe = *tf;
        current->trapframe->user_gs_base = rdmsr(MSR_KERNEL_GS_BASE);
    }

    /* Mark the current CPU offline for the user VM, online for the kernel VM */
    if (from_user && current && current->vm) {
        vm_cpu_offline(current->vm, cpuid());
        vm_cpu_online(kernel_vm, cpuid(), current);
    }

    /*
     * Page fault (#PF): demand paging for user-mode faults.
     * Error code bits: [0]=P (present), [1]=W (write), [2]=U (user).
     */
    if (vec == 14) {
        uint64 cr2 = read_cr2();

        if (from_user && current && current->vm) {
            /*
             * User-mode page fault — attempt demand paging.
             * x86 PF error code bit 1 = write fault.
             */
            int is_write = (tf->err & 0x2);
            uint64 prot = VMA_FLAG_USER | (is_write ? PROT_WRITE : PROT_READ);
            uint64 fault_base = PGROUNDDOWN(cr2);
            uint64 fault_len = is_write ? USER_WRITE_FAULT_AROUND_SIZE
                                         : USER_FAULT_AROUND_SIZE;

            /* Try growing main and MAP_GROWSDOWN stacks first. */
            vm_try_growstack(current->vm, cr2);
            vm_try_growdown(current->vm, cr2);

            if (!is_write &&
                vm_file_read_fault_unlocked(current->vm, fault_base, prot) == 0) {
                if (vm_asid_max() > 0)
                    sfence_vma_global();
                goto user_return;
            }

            vm_rlock(current->vm);
            vma_t *vma = vm_find_area(current->vm, cr2);
            if (vma != NULL) {
                if (fault_base >= vma->end)
                    fault_len = PGSIZE;
                else if (fault_base + fault_len > vma->end)
                    fault_len = vma->end - fault_base;
            }
            int fault_ret = -EFAULT;
            if (vma != NULL)
                fault_ret = vma_validate(vma, fault_base, fault_len, prot);
            if (fault_ret == 0) {
                vm_runlock(current->vm);
                /*
                 * With PCID enabled, vma_validate()'s sfence_vma() only
                 * flushed the kernel PCID (PCID 0, the current CR3).
                 * The user PCID may retain stale TLB entries — either
                 * negative-cached not-present results or read-only COW
                 * entries that were upgraded to writable.  Flush all
                 * PCIDs so the CPU re-walks the updated page table on
                 * return to user mode.
                 *
                 * TCG masks this bug because its softmmu ignores the
                 * noflush bit and flushes the entire TLB on every CR3
                 * write.  KVM with hardware PCID exposes it.
                 */
                if (vm_asid_max() > 0)
                    sfence_vma_global();
                goto user_return; /* fault resolved */
            }
            vm_runlock(current->vm);

            if (fault_ret == -ENOMEM)
                goto user_return;

            /*
             * Check whether the process has a user-installed SIGSEGV
             * handler.  If so, deliver the signal (with proper
             * siginfo) instead of treating it as unconditionally
             * fatal.  JSC JIT (and other runtimes) rely on SIGSEGV
             * delivery for speculative null-pointer checks.
             */
            {
                int has_handler = 0;
                void *handler_addr = NULL;
                sigacts_t *sa = current->sigacts;
                if (sa) {
                    sigacts_lock(sa);
                    sigaction_t *act = &sa->sa[SIGSEGV];
                    handler_addr = (void *)act->sa_handler;
                    if (act->sa_handler != (void (*)(int))0 &&
                        act->sa_handler != (void (*)(int))1) /* !SIG_DFL && !SIG_IGN */
                        has_handler = 1;
                    sigacts_unlock(sa);
                }
                if (has_handler) {
                    kde_xwayland_fault_trace(current, tf, cr2, vma);
                    /* Deliver SIGSEGV with fault address and code. */
                    ksiginfo_t si = {0};
                    si.signo = SIGSEGV;
                    si.sender = NULL;
                    si.info.si_signo = SIGSEGV;
                    si.info.si_code = (vma != NULL) ? 2 : 1; /* SEGV_ACCERR : SEGV_MAPERR */
                    si.info.si_addr = (void *)cr2;
                    __signal_send(current, &si);
                    goto user_return;
                }

                /* No handler — fatal fault. */
                kde_dump_fatal_page_fault_summary(current, tf, cr2);
                kde_dump_exception_vmas(current, tf, vec, "#PF Page Fault");
                printf(
                    "pid %d %s: fatal page fault cr2=0x%lx err=0x%lx rip=0x%lx"
                    " (SIGSEGV handler=%p sigacts=%p)\n",
                    current->pid, current->name, cr2, tf->err, tf->rip,
                    handler_addr, (void *)sa);
                printf("  regs: rax=0x%lx rbx=0x%lx rcx=0x%lx rdx=0x%lx\n",
                       tf->rax, tf->rbx, tf->rcx, tf->rdx);
                printf("        rdi=0x%lx rsi=0x%lx rbp=0x%lx rsp=0x%lx\n",
                       tf->rdi, tf->rsi, tf->rbp, tf->rsp);
                printf("        r8 =0x%lx r9 =0x%lx r10=0x%lx r11=0x%lx\n",
                       tf->r8, tf->r9, tf->r10, tf->r11);
                printf("        r12=0x%lx r13=0x%lx r14=0x%lx r15=0x%lx\n",
                       tf->r12, tf->r13, tf->r14, tf->r15);
                {
                    uint64 stack_words[8];
                    if (vm_copyin(current->vm, stack_words, tf->rsp,
                                  sizeof(stack_words)) == 0) {
                        printf("  user stack @rsp:");
                        for (size_t i = 0; i < sizeof(stack_words) /
                                               sizeof(stack_words[0]); i++)
                            printf(" 0x%lx", stack_words[i]);
                        printf("\n");
                    } else {
                        printf("  user stack @rsp: <unreadable>\n");
                    }
                }
                {
                    struct fault_reg_dump {
                        const char *name;
                        uint64 addr;
                    } dumps[] = {
                        {"rbx", tf->rbx},
                        {"r12", tf->r12},
                        {"r12+0x100", tf->r12 + 0x100},
                        {"r13", tf->r13},
                        {"r14", tf->r14},
                    };
                    for (size_t d = 0; d < sizeof(dumps) / sizeof(dumps[0]);
                         d++) {
                        uint64 words[8];

                        if (dumps[d].addr == 0)
                            continue;
                        if (vm_copyin(current->vm, words, dumps[d].addr,
                                      sizeof(words)) != 0) {
                            printf("  user mem %s @0x%lx: <unreadable>\n",
                                   dumps[d].name, dumps[d].addr);
                            continue;
                        }
                        printf("  user mem %s @0x%lx:",
                               dumps[d].name, dumps[d].addr);
                        for (size_t i = 0;
                             i < sizeof(words) / sizeof(words[0]); i++)
                            printf(" 0x%lx", words[i]);
                        printf("\n");
                        if (dumps[d].addr == tf->r12 + 0x100) {
                            for (size_t pidx = 0; pidx < 2; pidx++) {
                                uint64 pointed[8];

                                if (words[pidx] == 0)
                                    continue;
                                if (vm_copyin(current->vm, pointed,
                                              words[pidx],
                                              sizeof(pointed)) != 0) {
                                    printf("  user mem *(%s+0x%lx)=0x%lx: <unreadable>\n",
                                           dumps[d].name, pidx * 8,
                                           words[pidx]);
                                    continue;
                                }
                                printf("  user mem *(%s+0x%lx)=0x%lx:",
                                       dumps[d].name, pidx * 8,
                                       words[pidx]);
                                for (size_t i = 0;
                                     i < sizeof(pointed) / sizeof(pointed[0]);
                                     i++)
                                    printf(" 0x%lx", pointed[i]);
                                printf("\n");
                            }
                        }
                    }
                }
                vm_rlock(current->vm);
                print_fault_vma(current->vm, "fault-ip", tf->rip);
                print_fault_vma(current->vm, "fault-va", cr2);
                vm_runlock(current->vm);
                /* Dump the full VMA map.  Allocator/JIT crashes often hinge
                 * on anonymous mappings next to file-backed runtime state. */
                {
                    void *mt_entry;
                    unsigned long mt_idx;
                    printf("  --- VMA map ---\n");
                    mt_for_each(&current->vm->vm_mt, mt_entry, mt_idx, MAPLE_MAX) {
                        vma_t *v = (vma_t *)mt_entry;
                        if (!fault_vma_entry_valid(current->vm, v)) {
                            printf("  <invalid VMA entry %p>\n", v);
                            continue;
                        }
                        struct vfs_inode *inode = fault_vma_inode(v);
                        printf("  [0x%lx-0x%lx) %c%c%c %s pgoff=0x%lx file=%p ino=%lu size=%lld path=%s\n",
                               v->start, v->end,
                               (v->flags & PROT_READ) ? 'r' : '-',
                               (v->flags & PROT_WRITE) ? 'w' : '-',
                               (v->flags & PROT_EXEC) ? 'x' : '-',
                               (v->flags & VMA_FLAG_SHARED) ? "shared" : "private",
                               v->pgoff, (void *)v->file,
                               inode ? inode->ino : 0,
                               inode ? inode->size : 0,
                               vma_debug_path(v));
                    }
                    printf("  --- end VMA map ---\n");
                }
            }
            assert(current->pid != 1, "init exiting");
            print_user_backtrace(current->vm->pagetable,
                                 tf->rbp, tf->rip, tf->rsp, tf->rip, 20);
            do_coredump(current);
            {
                extern int gdbstub_signal_stop(struct thread *, int);
                gdbstub_signal_stop(current, SIGSEGV);
            }
            kill(current->pid, SIGSEGV);
            goto user_return;
        }

        if (from_user && current) {
            /* User-mode page fault but VM is NULL — process is being
             * torn down.  Just kill it; don't fall through to kernel
             * panic. */
            printf("pid %d %s: page fault with NULL vm cr2=0x%lx rip=0x%lx\n",
                   current->pid, current->name, cr2, tf->rip);
            assert(current->pid != 1, "init exiting");
            kill(current->pid, SIGSEGV);
            goto user_return;
        }

        /* Kernel-mode page fault — attempt vmalloc-area lazy validation.
         *
         * Process page tables copy kpml4[256-511] at creation time
         * (uvmcreate).  If the kernel later populates a new PML4
         * entry (e.g. kvm_mmap allocating beyond the initial
         * mappings), existing process page tables won't have it.
         * A kernel-mode fault can occur when the trap entry path
         * runs briefly with the process's CR3 before the C-level
         * CR3 switch, or during any window where a stale page
         * table is active.
         *
         * Fix: sync the PML4 entry from kernel_pagetable to the
         * faulting process's page table and retry.
         */
        {
            int pml4_idx = PX(3, cr2);

            /* Only handle higher-half kernel addresses (PML4[256-511]). */
            if (pml4_idx >= 256) {
                pte_t kpml4_entry = kernel_pagetable[pml4_idx];

                if (kpml4_entry & PTE_V) {
                    /*
                     * The reference kernel page table has this entry.
                     * Sync it to the current process's page table.
                     */
                    pagetable_t proc_pt = NULL;
                    if (current && current->vm)
                        proc_pt = current->vm->pagetable;

                    if (proc_pt != NULL &&
                        proc_pt[pml4_idx] != kpml4_entry) {
                        proc_pt[pml4_idx] = kpml4_entry;
                        /* Flush TLB for the faulting address. */
                        asm volatile("invlpg (%0)" : : "r"(cr2) : "memory");
                        return; /* retry the faulting instruction */
                    }
                }
            }
        }

        /* Truly unrecoverable kernel page fault */
        dbg_puts("\n*** KERNEL PAGE FAULT\n");
        dbg_puts("  cr2=0x");
        dbg_hex(cr2);
        dbg_puts(" err=0x");
        dbg_hex(tf->err);
        dbg_puts(" rip=0x");
        dbg_hex(tf->rip);
        dbg_puts("\n");
        printf("\n*** KERNEL PAGE FAULT: cr2=0x%lx err=0x%lx rip=0x%lx\n", cr2,
               tf->err, tf->rip);
        *(uint64 *)((uint64)tf - 8) = tf->rip;
        {
            /* Print crash site as first backtrace entry */
            char _sym[64] = {0}; char _file[128] = {0};
            uint32 _line = 0; void *_addr = NULL;
            printf("backtrace:\n");
            if (ksym_lookup(tf->rip, _sym, sizeof(_sym), &_addr) >= 0) {
                ksymbols_t *s = ksym_search(tf->rip);
                ksym_get_location(s, _file, sizeof(_file), &_line);
                printf("  > %s:%d: %s+%d [crash]\n", _file, _line,
                       _sym, ksym_get_offset(s, tf->rip));
            } else {
                printf("  > %p [crash]\n", (void *)tf->rip);
            }
        }
        if (current) {
            size_t kstack_size = (1UL << (PAGE_SHIFT + current->kstack_order));
            uint64 kstack_va = (uint64)PA2VA(current->kstack);
            print_backtrace(tf->rbp, kstack_va, kstack_va + kstack_size);
        } else {
            print_backtrace(tf->rbp, KERNBASE, PHYSTOP);
        }
        panic_disable_bt();
        panic("kernel page fault");
    }

    if (vec == 3 && from_user && current) {
        /* INT3 breakpoint from user mode — hand off to gdbstub.
         * Used by waitgdb() and GDB software breakpoints.
         *
         * x86 INT3 pushes the address AFTER the 1-byte 0xCC onto the
         * stack, so RIP in the trapframe points one byte past the INT3.
         * Adjust it back so RIP points AT the INT3, matching RISC-V
         * behavior (sepc points at EBREAK).  This lets gdbstub_trap()
         * match breakpoints by address uniformly on both architectures.
         *
         * IMPORTANT: modify the utrapframe (not the stack tf), because
         * gdbstub_trap reads/writes p->trapframe->trapframe, and we
         * return via usertrapret() which also uses the utrapframe.
         * The stack-based tf is NOT used on the return path. */
        current->trapframe->trapframe.rip -= 1;
        extern int gdbstub_trap(struct thread *);
        if (gdbstub_trap(current) != 0) {
            /* GDB stub didn't handle it — deliver SIGTRAP */
            printf("pid %d %s: breakpoint trap rip=0x%lx\n", current->pid,
                   current->name, current->trapframe->trapframe.rip);
            dump_user_int3_diagnostics(current,
                                       &current->trapframe->trapframe);
            if (chrome_trace_value_enabled("chrome_fd_trace")) {
                chrome_dump_icu_vmas(current, "trap-int3");
                chrome_dump_asset_fds(current, "trap-int3");
            }
            if (chrome_trace_value_enabled("chrome_fd_trace") &&
                (chrome_lifecycle_thread_match(current) ||
                 strncmp(current->name, "exe", 4) == 0)) {
                vfs_fdtable_debug_dump(current, "trap-int3", 128);
            }
            kill(current->pid, SIGTRAP);
        }
        usertrapret(); /* noreturn — returns via utrapframe */
    }

    if (vec == 1 && from_user && current) {
        /* #DB (Debug exception) from user mode — triggered by RFLAGS.TF
         * hardware single-step.  Hand off to gdbstub.
         *
         * Modify the utrapframe (not stack tf) because gdbstub_trap
         * reads/writes the utrapframe, and usertrapret uses it too. */
        current->trapframe->trapframe.rflags &= ~(1ULL << 8);
        extern int gdbstub_trap(struct thread *);
        if (gdbstub_trap(current) != 0) {
            /* GDB stub didn't handle it — deliver SIGTRAP */
            printf("pid %d %s: debug trap rip=0x%lx\n", current->pid,
                   current->name, current->trapframe->trapframe.rip);
            kill(current->pid, SIGTRAP);
        }
        usertrapret(); /* noreturn — returns via utrapframe */
    }

    if (vec < 32) {
        /* #NM (Device Not Available, vector 7) — lazy FPU switching.
         * CR0.TS was set, user code executed an FP/SSE instruction. */
        if (vec == 7 && from_user && current) {
            /* Clear TS so kernel FP save/restore routines work. */
            asm volatile("clts");

            struct cpu_local *c = mycpu();

            /* First time this thread uses FP — allocate fpu_state */
            if (!THREAD_FPU_USED(current)) {
                if (current->fpu_state == NULL) {
                    current->fpu_state = kvmalloc(sizeof(struct fpu_state));
                    if (current->fpu_state == NULL) {
                        printf("pid %d: failed to allocate fpu_state\n",
                               current->pid);
                        kill(current->pid, SIGKILL);
                        goto user_return;
                    }
                    memset(current->fpu_state, 0, sizeof(struct fpu_state));
                }
                fpu_init_state();
                THREAD_SET_FPU_USED(current);
            } else if (c->fpu_owner_tid != current->pid ||
                       current->fpu_seq != c->fpu_seq) {
                /*
                 * Different owner or mismatched seq — restore our
                 * saved FP state and take ownership.  Bump seq so
                 * stale per-CPU entries from other CPUs won't match.
                 */
                fpu_restore_state(current->fpu_state);
                current->fpu_seq++;
            }
            c->fpu_owner_tid = current->pid;
            c->fpu_seq = current->fpu_seq;
            /* TS is clear — FP/SSE will work when we return to user */
            goto user_return;
        }

        /* CPU exception (not #PF, not #BP, not #NM) */
        const char *name = exception_names[vec];

        if (from_user && current) {
            /* User-mode exception — deliver appropriate signal */
            int sig = SIGSEGV;
            if (vec == 0 || vec == 16 || vec == 19)
                sig = SIGFPE; /* #DE, #MF, #XM */
            if (vec == 6)
                sig = SIGILL; /* #UD */
            printf("pid %d %s: exception %ld (%s) rip=0x%lx err=0x%lx\n",
                   current->pid, current->name, vec, name ? name : "???",
                   tf->rip, tf->err);
            printf("  rsp=0x%lx rbp=0x%lx rax=0x%lx rbx=0x%lx\n",
                   tf->rsp, tf->rbp, tf->rax, tf->rbx);
            printf("  rdi=0x%lx rsi=0x%lx rdx=0x%lx rcx=0x%lx\n",
                   tf->rdi, tf->rsi, tf->rdx, tf->rcx);
            printf("  r8=0x%lx r9=0x%lx r10=0x%lx r11=0x%lx\n",
                   tf->r8, tf->r9, tf->r10, tf->r11);
            printf("  r12=0x%lx r13=0x%lx r14=0x%lx r15=0x%lx\n",
                   tf->r12, tf->r13, tf->r14, tf->r15);
            printf("  fs_base(tp)=0x%lx\n",
                   current->trapframe ? current->trapframe->tp : 0);
            kde_dump_exception_register_memory(current, tf);
            kde_dump_exception_vmas(current, tf, vec, name);
            assert(current->pid != 1, "init exiting");
            print_user_backtrace(current->vm->pagetable,
                                 tf->rbp, tf->rip, tf->rsp, tf->rip, 20);
            do_coredump(current);
            {
                extern int gdbstub_signal_stop(struct thread *, int);
                gdbstub_signal_stop(current, sig);
            }
            kill(current->pid, sig);
            goto user_return;
        }

        /* Kernel exception */
        dbg_puts("\n*** EXCEPTION: ");
        dbg_puts(name ? name : "???");
        dbg_puts("\n  vector=0x");
        dbg_hex(vec);
        dbg_puts(" err=0x");
        dbg_hex(tf->err);
        dbg_puts("\n  rip=0x");
        dbg_hex(tf->rip);
        dbg_puts(" cs=0x");
        dbg_hex(tf->cs);
        dbg_puts("\n  rflags=0x");
        dbg_hex(tf->rflags);
        dbg_puts(" rsp=0x");
        dbg_hex(tf->rsp);
        dbg_puts(" ss=0x");
        dbg_hex(tf->ss);
        dbg_puts("\n  rax=0x");
        dbg_hex(tf->rax);
        dbg_puts(" rbx=0x");
        dbg_hex(tf->rbx);
        dbg_puts(" rcx=0x");
        dbg_hex(tf->rcx);
        dbg_puts(" rdx=0x");
        dbg_hex(tf->rdx);
        dbg_puts("\n  rdi=0x");
        dbg_hex(tf->rdi);
        dbg_puts(" rsi=0x");
        dbg_hex(tf->rsi);
        dbg_puts(" rbp=0x");
        dbg_hex(tf->rbp);
        dbg_puts("\n  r8 =0x");
        dbg_hex(tf->r8);
        dbg_puts(" r9 =0x");
        dbg_hex(tf->r9);
        dbg_puts(" r10=0x");
        dbg_hex(tf->r10);
        dbg_puts(" r11=0x");
        dbg_hex(tf->r11);
        dbg_puts("\n  r12=0x");
        dbg_hex(tf->r12);
        dbg_puts(" r13=0x");
        dbg_hex(tf->r13);
        dbg_puts(" r14=0x");
        dbg_hex(tf->r14);
        dbg_puts(" r15=0x");
        dbg_hex(tf->r15);
        dbg_puts("\n");
        printf("\n*** EXCEPTION %ld: %s\n", vec, name ? name : "???");
        printf("  err=0x%lx  rip=0x%lx  cs=0x%lx\n", tf->err, tf->rip, tf->cs);
        printf("  rsp=0x%lx  rbp=0x%lx  rflags=0x%lx\n", tf->rsp, tf->rbp,
               tf->rflags);
        printf("  rax=0x%lx  rbx=0x%lx  rcx=0x%lx  rdx=0x%lx\n",
               tf->rax, tf->rbx, tf->rcx, tf->rdx);
        printf("  rdi=0x%lx  rsi=0x%lx  r8=0x%lx  r9=0x%lx\n",
               tf->rdi, tf->rsi, tf->r8, tf->r9);
        printf("  r10=0x%lx  r11=0x%lx  r12=0x%lx  r13=0x%lx\n",
               tf->r10, tf->r11, tf->r12, tf->r13);
        printf("  r14=0x%lx  r15=0x%lx  ss=0x%lx\n",
               tf->r14, tf->r15, tf->ss);
        if (current)
            printf("  current: pid=%d name=%s ksp=0x%lx\n",
                   current->pid, current->name, current->ksp);
        /* GS_BASE (should be per-CPU in kernel mode) */
        {
            uint64 gs_base = rdmsr(MSR_GS_BASE);
            uint64 kgs_base = rdmsr(MSR_KERNEL_GS_BASE);
            printf("  MSR_GS_BASE=0x%lx  MSR_KERNEL_GS_BASE=0x%lx\n",
                   gs_base, kgs_base);
        }
        /* Dump instruction bytes at crash RIP (may fault; guard access) */
        {
            uint64 rip = tf->rip;
            printf("  insn@rip:");
            for (int _i = 0; _i < 16; _i++)
                printf(" %02x", *(volatile uint8 *)(rip + _i));
            printf("\n");
        }
        /* Raw stack dump around RSP (16 qwords) */
        {
            uint64 *sp = (uint64 *)tf->rsp;
            printf("  stack dump @ rsp=0x%lx:\n", tf->rsp);
            for (int _i = 0; _i < 16; _i++) {
                uint64 val = sp[_i];
                printf("    [rsp+0x%02x] = 0x%lx", _i * 8, val);
                /* Try to resolve as symbol */
                char _sym[64] = {0};
                void *_addr = NULL;
                if (ksym_lookup(val, _sym, sizeof(_sym), &_addr) >= 0)
                    printf("  <%s+%ld>", _sym,
                           (long)(val - (uint64)_addr));
                printf("\n");
            }
        }

        *(uint64 *)((uint64)tf - 8) = tf->rip;
        {
            /* Print crash site as first backtrace entry */
            char _sym[64] = {0}; char _file[128] = {0};
            uint32 _line = 0; void *_addr = NULL;
            printf("backtrace:\n");
            if (ksym_lookup(tf->rip, _sym, sizeof(_sym), &_addr) >= 0) {
                ksymbols_t *s = ksym_search(tf->rip);
                ksym_get_location(s, _file, sizeof(_file), &_line);
                printf("  > %s:%d: %s+%d [crash]\n", _file, _line,
                       _sym, ksym_get_offset(s, tf->rip));
            } else {
                printf("  > %p [crash]\n", (void *)tf->rip);
            }
        }
        if (current) {
            size_t kstack_size = (1UL << (PAGE_SHIFT + current->kstack_order));
            uint64 kstack_va = (uint64)PA2VA(current->kstack);
            print_backtrace(tf->rbp, kstack_va, kstack_va + kstack_size);
        } else {
            print_backtrace(tf->rbp, KERNBASE, PHYSTOP);
        }
        panic_disable_bt();
        panic("exception");

    } else if (vec >= T_IRQ0 && vec < T_IRQ0 + 24) {
        /* Hardware IRQ via I/O APIC (ISA IRQ 0-23) */
        int irq = vec - T_IRQ0;

        if (irq == 0) {
            /* PIT timer tick */
            timer_tick_advance();
        } else {
            /* Dispatch through irq_descs[] for registered device handlers */
            do_device_irq(irq);
        }

        lapic_eoi();

    } else if (vec == LAPIC_TIMER_VEC) {
        /* LAPIC timer interrupt (periodic or TSC-deadline) */
        timer_tick_advance();
        extern void lapic_timer_rearm(void);
        lapic_timer_rearm(); /* no-op for periodic mode */
        lapic_eoi();

    } else if (vec == LAPIC_IPI_VEC) {
        /* Inter-processor interrupt */
        extern void x86_ipi_handler(void);
        lapic_eoi(); /* EOI before handler so nested IPIs can arrive */
        x86_ipi_handler();

    } else if (vec == LAPIC_ERROR_VEC) {
        /* LAPIC error interrupt */
        printf("[x86] LAPIC error: ESR=0x%x\n", lapic_read(LAPIC_ESR));
        lapic_eoi();

    } else if (vec == 0xF1) {
        /* Hyper-V SynIC message interrupt.  The handler is a no-op unless the
         * Hyper-V input backend initialized successfully. */
        hyperv_input_intr();
        lapic_eoi();

    } else if (vec == LAPIC_SPURIOUS_VEC) {
        /* Spurious interrupt — do NOT send EOI */
    }

    /*
     * User-mode traps: return through usertrapret so that
     * the trapframe page, GS scratch, and TSS are set up
     * for the next entry.  All user-mode code paths above
     * must reach here (via goto user_return) rather than
     * using plain return, because alltraps did swapgs on
     * entry and trapret does NOT swapgs on exit.
     */
user_return:
    if (from_user && current) {
        usertrapret(); /* noreturn */
    }
    /* Kernel traps return here and fall back to trapret/iretq. */
}

/*
 * usertrap_syscall — C-level SYSCALL handler.
 *
 * Called from syscall_entry (trapvec.S) with RDI = pointer to
 * the current thread's authoritative utrapframe.  The SYSCALL entry path
 * stores registers there directly so the hot path avoids a stack-frame copy.
 */
void usertrap_syscall(struct trapframe *tf) {
    /*
     * Switch to kernel page table for identity-map access (PML4[0]).
     * syscall_entry no longer switches CR3 in assembly.
     */
    asm volatile("movq %0, %%cr3" : : "r"(trampoline_ksatp) : "memory");

    struct thread *p = current;
    (void)tf;

    /*
     * syscall_entry saved the register frame directly into p->trapframe.
     * Save user GS base here; FS base is cached in trapframe->tp because
     * FSGSBASE is disabled and arch_prctl()/clone TLS own updates.
     */
    if (p && p->trapframe) {
        p->trapframe->user_gs_base = rdmsr(MSR_KERNEL_GS_BASE);
    }

    /* Mark the current CPU offline for the user VM, online for the kernel VM */
    if (p && p->vm) {
        vm_cpu_offline(p->vm, cpuid());
        vm_cpu_online(kernel_vm, cpuid(), p);
    }

    intr_on();
    syscall();
    usertrapret(); /* noreturn */
}
