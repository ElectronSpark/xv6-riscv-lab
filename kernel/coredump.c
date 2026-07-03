/**
 * @file coredump.c
 * @brief ELF core dump generation for crashed user-space processes
 *
 * Writes a minimal ELF core file (/core.PID) containing:
 *  – NT_PRSTATUS note with RISC-V register state
 *  – PT_LOAD segments for all mapped user pages
 */

#include "types.h"
#include "param.h"
#ifdef __riscv
#include "riscv.h"
#else
#include "x86.h"
#endif
#include "printf.h"
#include "string.h"
#include "elf.h"
#include "lock/spinlock.h"
#include "defs.h"
#include "proc/thread.h"
#include "resource.h"
#include "trapframe.h"
#include <mm/vm.h>
#include <mm/vm_types.h>
#include <mm/pgtable.h>
#include <mm/memlayout.h>
#include <vfs/fs.h>
#include <vfs/file.h>
#include <vfs/fcntl.h>
#include <vfs/stat.h>
#include "maple_tree.h"
#include "defs.h"
#include "errno.h"

/* Provided by accounting.c / sys_arch.c */
int snprintf(char *buf, size_t size, const char *fmt, ...);

/* ELF constants not in kernel elf.h */
#define ET_CORE     4
#ifdef __riscv
#define EM_SELF     243   /* EM_RISCV */
#else
#define EM_SELF     62    /* EM_X86_64 */
#endif
#define EV_CURRENT  1
#define PT_NOTE     4
#define PT_LOAD     1
#define PF_X        1
#define PF_W        2
#define PF_R        4
#define NT_PRSTATUS 1

#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define EV_CURRENT_IDENT 1
#define ELFOSABI_NONE 0

/* Align to 4-byte boundary (ELF note alignment) */
#define NOTE_ALIGN(x) (((x) + 3) & ~3)

#ifdef __riscv
/*
 * RISC-V 64-bit prstatus register layout.
 * Matches Linux struct user_regs_struct:
 *   pc, ra, sp, gp, tp, t0-t2, s0-s1, a0-a7, s2-s11, t3-t6
 * = 32 uint64 values.
 */
#define ELF_NGREG 32
#else
/*
 * x86_64 Linux struct user_regs_struct has 27 unsigned long registers.
 * GDB checks the NT_PRSTATUS descriptor size before decoding pr_reg.
 */
#define ELF_NGREG 27
#endif

/*
 * Minimal elf_prstatus structure matching Linux layout.
 * GDB reads pr_pid and pr_reg from this.
 */
struct elf_prstatus {
    /* Signal info (12 bytes) */
    int32 pr_si_signo;
    int32 pr_si_code;
    int32 pr_si_errno;
    /* Current signal */
    int16 pr_cursig;
    int16 __pad0;
    /* Pending/held signals */
    uint64 pr_sigpend;
    uint64 pr_sighold;
    /* IDs */
    int32 pr_pid;
    int32 pr_ppid;
    int32 pr_pgrp;
    int32 pr_sid;
    /* Times (4 × struct timeval = 4 × 16 bytes) */
    uint64 pr_utime[2];
    uint64 pr_stime[2];
    uint64 pr_cutime[2];
    uint64 pr_cstime[2];
    /* Registers */
    uint64 pr_reg[ELF_NGREG];
    /* FP valid flag */
    int32 pr_fpvalid;
    int32 __pad1;
};

/*
 * Count the number of VMAs in the process address space.
 * Caller must hold vm rlock.
 */
static int count_vmas(vm_t *vm)
{
    int n = 0;
    uint64 idx = 0;
    void *entry;
    mt_for_each(&vm->vm_mt, entry, idx, MAPLE_MAX) {
        n++;
    }
    return n;
}

/*
 * Fill pr_reg from the utrapframe in Linux register order.
 */
static void fill_prstatus_regs(struct elf_prstatus *pr,
                               struct utrapframe *utf)
{
    struct trapframe *tf = &utf->trapframe;
    uint64 *r = pr->pr_reg;
#ifdef __riscv
    /* RISC-V: pc, ra, sp, gp, tp, t0-t2, s0-s1, a0-a7, s2-s11, t3-t6 */
    r[0]  = tf->sepc;   /* pc */
    r[1]  = tf->ra;
    r[2]  = tf->sp;
    r[3]  = utf->gp;
    r[4]  = utf->tp;
    r[5]  = tf->t0;
    r[6]  = tf->t1;
    r[7]  = tf->t2;
    r[8]  = tf->s0;     /* s0 / fp */
    r[9]  = utf->s1;
    r[10] = tf->a0;
    r[11] = tf->a1;
    r[12] = tf->a2;
    r[13] = tf->a3;
    r[14] = tf->a4;
    r[15] = tf->a5;
    r[16] = tf->a6;
    r[17] = tf->a7;
    r[18] = utf->s2;
    r[19] = utf->s3;
    r[20] = utf->s4;
    r[21] = utf->s5;
    r[22] = utf->s6;
    r[23] = utf->s7;
    r[24] = utf->s8;
    r[25] = utf->s9;
    r[26] = utf->s10;
    r[27] = utf->s11;
    r[28] = tf->t3;
    r[29] = tf->t4;
    r[30] = tf->t5;
    r[31] = tf->t6;
#else
    /* x86_64: Linux user_regs_struct order (see sys/user.h) */
    r[0]  = tf->r15;
    r[1]  = tf->r14;
    r[2]  = tf->r13;
    r[3]  = tf->r12;
    r[4]  = tf->rbp;
    r[5]  = tf->rbx;
    r[6]  = tf->r11;
    r[7]  = tf->r10;
    r[8]  = tf->r9;
    r[9]  = tf->r8;
    r[10] = tf->rax;
    r[11] = tf->rcx;
    r[12] = tf->rdx;
    r[13] = tf->rsi;
    r[14] = tf->rdi;
    r[15] = 0;          /* orig_rax */
    r[16] = tf->rip;
    r[17] = tf->cs;
    r[18] = tf->rflags;
    r[19] = tf->rsp;
    r[20] = tf->ss;
    /* fs_base, gs_base, ds, es, fs, gs */
    r[21] = utf->tp;
    r[22] = utf->user_gs_base;
    r[23] = 0;
    r[24] = 0;
    r[25] = 0;
    r[26] = 0;
#endif
}

/*
 * Convert VMA flags to ELF p_flags.
 */
static uint32 vma_to_elf_flags(uint64 vma_flags)
{
    uint32 f = 0;
    if (vma_flags & PROT_READ)  f |= PF_R;
    if (vma_flags & PROT_WRITE) f |= PF_W;
    if (vma_flags & PROT_EXEC)  f |= PF_X;
    return f;
}

/*
 * Helper: write kernel buffer to file, return bytes written or -1.
 */
static ssize_t core_write(struct vfs_file *f, const void *buf, size_t n)
{
    return vfs_filewrite(f, buf, n, false);
}

/*
 * Helper: write a page of zeros.
 */
static char zero_page[PGSIZE] __attribute__((aligned(PGSIZE)));

static void *core_page_kva(uint64 pa)
{
#ifndef __riscv
    if (pa >= PHYSTOP || pa + PGSIZE < pa || pa + PGSIZE > PHYSTOP)
        return NULL;
#endif
    return PA2VA(pa);
}

/*
 * do_coredump — Generate an ELF core file for the current thread.
 *
 * Called from the fatal fault handler after diagnostics have been printed.
 * The process is about to be killed, so we dump its state first.
 */
void do_coredump(struct thread *t)
{
    vm_t *vm = t->vm;
    struct utrapframe *utf = t->trapframe;
    char path[32];
    char name[32];
    struct vfs_file *f = NULL;
    uint64 core_limit = RLIM_INFINITY;

    if (t->thread_group)
        core_limit = t->thread_group->rlim[RLIMIT_CORE].rlim_cur;
    if (core_limit == 0) {
        printf("coredump: RLIMIT_CORE=0 for pid %d (%s), skipping\n",
               t->pid, t->name);
        return;
    }

    /* Build filename: /core.PID */
    int len = snprintf(path, sizeof(path), "/core.%d", t->pid);
    if (len < 0 || len >= (int)sizeof(path)) {
        printf("coredump: path too long for pid %d\n", t->pid);
        return;
    }

    printf("coredump: generating %s for pid %d (%s)...\n",
           path, t->pid, t->name);

    /* Count VMAs (need vm_rlock for tree traversal) */
    vm_rlock(vm);
    int nvma = count_vmas(vm);
    vm_runlock(vm);

    if (nvma == 0) {
        printf("coredump: no VMAs, skipping\n");
        return;
    }

    /* Number of program headers: 1 (PT_NOTE) + nvma (PT_LOAD each) */
    int nphdr = 1 + nvma;

    /* ELF header */
    struct elfhdr ehdr;
    memset(&ehdr, 0, sizeof(ehdr));
    ehdr.magic = ELF_MAGIC;
    ehdr.elf[0] = ELFCLASS64;        /* EI_CLASS */
    ehdr.elf[1] = ELFDATA2LSB;       /* EI_DATA */
    ehdr.elf[2] = EV_CURRENT_IDENT;  /* EI_VERSION */
    ehdr.elf[3] = ELFOSABI_NONE;     /* EI_OSABI */
    ehdr.type = ET_CORE;
    ehdr.machine = EM_SELF;
    ehdr.version = EV_CURRENT;
    ehdr.entry = 0;
    ehdr.phoff = sizeof(ehdr);
    ehdr.shoff = 0;
    ehdr.flags = 0;
    ehdr.ehsize = sizeof(ehdr);
    ehdr.phentsize = sizeof(struct proghdr);
    ehdr.phnum = nphdr;
    ehdr.shentsize = 0;
    ehdr.shnum = 0;
    ehdr.shstrndx = 0;

    /* Build the NT_PRSTATUS note */
    struct elf_prstatus prstatus;
    memset(&prstatus, 0, sizeof(prstatus));
    prstatus.pr_si_signo = 11; /* SIGSEGV */
    prstatus.pr_cursig = 11;
    prstatus.pr_pid = t->pid;
    fill_prstatus_regs(&prstatus, utf);

    /* Note header: name = "CORE\0" (5 bytes, padded to 8) */
    const char note_name[] = "CORE";
    uint32 namesz = sizeof(note_name); /* 5 including NUL */
    uint32 descsz = sizeof(prstatus);
    uint32 note_type = NT_PRSTATUS;

    /* Total note size: header(12) + aligned_name + aligned_desc */
    size_t note_total = 12 + NOTE_ALIGN(namesz) + NOTE_ALIGN(descsz);

    /* Calculate file offsets */
    uint64 phdr_offset = sizeof(ehdr);
    uint64 note_offset = phdr_offset + nphdr * sizeof(struct proghdr);
    uint64 data_offset = note_offset + note_total;
    /* Align data start to page boundary */
    data_offset = (data_offset + PGSIZE - 1) & ~((uint64)PGSIZE - 1);

    /* Create the core file */
    struct vfs_inode *dir = vfs_nameiparent(path, strlen(path),
                                            name, sizeof(name));
    if (IS_ERR_OR_NULL(dir)) {
        printf("coredump: cannot resolve parent dir for %s\n", path);
        return;
    }

    /* Try to look up existing file first */
    struct vfs_inode *inode = vfs_namei(path, strlen(path));
    if (!IS_ERR_OR_NULL(inode)) {
        /* File exists — open and truncate */
        f = vfs_fileopen(inode, O_WRONLY | O_TRUNC);
        vfs_iput(inode);
        vfs_iput(dir);
        if (IS_ERR_OR_NULL(f)) {
            printf("coredump: cannot open existing %s\n", path);
            return;
        }
    } else {
        /* Create new file */
        inode = vfs_create(dir, S_IFREG | 0644, name, strlen(name));
        vfs_iput(dir);
        if (IS_ERR_OR_NULL(inode)) {
            printf("coredump: cannot create %s\n", path);
            return;
        }
        f = vfs_fileopen(inode, O_WRONLY);
        vfs_iput(inode);
        if (IS_ERR_OR_NULL(f)) {
            printf("coredump: cannot open new %s\n", path);
            return;
        }
    }

    /* Write ELF header */
    if (core_write(f, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) {
        printf("coredump: failed writing ELF header\n");
        goto out;
    }

    /*
     * Build and write program headers.
     * We need to scan VMAs twice: once for headers, once for data.
     * Allocate phdr array in kernel memory.
     */
    struct proghdr *phdrs = kvmalloc(nphdr * sizeof(struct proghdr));
    if (phdrs == NULL) {
        printf("coredump: cannot allocate phdr array\n");
        goto out;
    }
    memset(phdrs, 0, nphdr * sizeof(struct proghdr));

    /* First phdr: PT_NOTE */
    phdrs[0].type = PT_NOTE;
    phdrs[0].flags = 0;
    phdrs[0].off = note_offset;
    phdrs[0].vaddr = 0;
    phdrs[0].paddr = 0;
    phdrs[0].filesz = note_total;
    phdrs[0].memsz = note_total;
    phdrs[0].align = 4;

    /* Build PT_LOAD entries for each VMA */
    vm_rlock(vm);
    {
        uint64 idx = 0;
        void *entry;
        int i = 1;
        uint64 cur_data_off = data_offset;

        mt_for_each(&vm->vm_mt, entry, idx, MAPLE_MAX) {
            vma_t *vma = (vma_t *)entry;
            uint64 vma_size = vma->end - vma->start;

            phdrs[i].type = PT_LOAD;
            phdrs[i].flags = vma_to_elf_flags(vma->flags);
            phdrs[i].off = cur_data_off;
            phdrs[i].vaddr = vma->start;
            phdrs[i].paddr = 0;
            phdrs[i].filesz = vma_size;
            phdrs[i].memsz = vma_size;
            phdrs[i].align = PGSIZE;

            cur_data_off += vma_size;
            i++;
            if (i > nphdr)
                break;
        }
    }
    vm_runlock(vm);

    /* Write all program headers */
    size_t phdrs_size = nphdr * sizeof(struct proghdr);
    if (core_write(f, phdrs, phdrs_size) != (ssize_t)phdrs_size) {
        printf("coredump: failed writing program headers\n");
        kvfree(phdrs);
        goto out;
    }
    kvfree(phdrs);

    /* Write NT_PRSTATUS note */
    /* Note header */
    uint32 nhdr[3];
    nhdr[0] = namesz;
    nhdr[1] = descsz;
    nhdr[2] = note_type;
    if (core_write(f, nhdr, 12) != 12) {
        printf("coredump: failed writing note header\n");
        goto out;
    }

    /* Note name "CORE\0" + padding */
    char note_name_pad[8];
    memset(note_name_pad, 0, sizeof(note_name_pad));
    memcpy(note_name_pad, note_name, namesz);
    if (core_write(f, note_name_pad, NOTE_ALIGN(namesz)) !=
        (ssize_t)NOTE_ALIGN(namesz)) {
        printf("coredump: failed writing note name\n");
        goto out;
    }

    /* Note descriptor (prstatus) */
    if (core_write(f, &prstatus, descsz) != (ssize_t)descsz) {
        printf("coredump: failed writing prstatus\n");
        goto out;
    }

    /* Pad to aligned descriptor size */
    size_t desc_pad = NOTE_ALIGN(descsz) - descsz;
    if (desc_pad > 0) {
        char pad[4] = {0};
        if (core_write(f, pad, desc_pad) != (ssize_t)desc_pad) {
            printf("coredump: failed writing note padding\n");
            goto out;
        }
    }

    /* Pad from end of note to data_offset (page-aligned) */
    uint64 cur_pos = note_offset + note_total;
    if (cur_pos < data_offset) {
        /* Write zeros to fill the gap */
        uint64 gap = data_offset - cur_pos;
        while (gap > 0) {
            size_t chunk = gap > PGSIZE ? PGSIZE : gap;
            if (core_write(f, zero_page, chunk) != (ssize_t)chunk) {
                printf("coredump: failed writing padding\n");
                goto out;
            }
            gap -= chunk;
        }
    }

    /* Write memory contents for each VMA */
    uint64 total_written = 0;
    vm_rlock(vm);
    {
        uint64 idx = 0;
        void *entry;

        mt_for_each(&vm->vm_mt, entry, idx, MAPLE_MAX) {
            vma_t *vma = (vma_t *)entry;

            for (uint64 va = vma->start; va < vma->end; va += PGSIZE) {
                uint64 pa = walkaddr(vm->pagetable, va);
                if (pa != 0) {
                    void *kva = core_page_kva(pa);
                    vm_runlock(vm);
                    ssize_t n = core_write(f, kva ? kva : zero_page, PGSIZE);
                    vm_rlock(vm);
                    if (n != PGSIZE) {
                        printf("coredump: write error at VA 0x%lx\n", va);
                        goto out_unlock;
                    }
                } else {
                    /* Page not present — write zeros */
                    vm_runlock(vm);
                    ssize_t n = core_write(f, zero_page, PGSIZE);
                    vm_rlock(vm);
                    if (n != PGSIZE) {
                        printf("coredump: write error (zero) at VA 0x%lx\n",
                               va);
                        goto out_unlock;
                    }
                }
                total_written += PGSIZE;
            }
        }
    }
out_unlock:
    vm_runlock(vm);

    printf("coredump: wrote %s (%ld bytes of memory, %d VMAs)\n",
           path, total_written, nvma);

out:
    vfs_fput(f);
}
