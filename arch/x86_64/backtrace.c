/**
 * @file backtrace.c
 * @brief x86_64 stack-frame walker.
 *
 * With -fno-omit-frame-pointer the x86_64 SysV ABI stores the previous
 * frame pointer at *(rbp) and the return address at *(rbp + 8).
 */

#include "types.h"
#include "string.h"
#include "param.h"
#include <mm/memlayout.h>
#include "x86.h"
#include "defs.h"
#include "proc/thread.h"
#include "printf.h"
#include "ksymbols.h"
#include <mm/pgtable.h>

#define BT_PREV_FP(fp)        ((fp) ? *(uint64 *)(uint64)(fp) : 0)
#define BT_RETURN_ADDRESS(fp)  ((fp) ? *(uint64 *)((uint64)(fp) + 8) : 0)
#define BT_IS_TOP_FRAME(fp) \
    (!(uint64)(fp) || (uint64)(fp) == PGROUNDDOWN((uint64)(fp)))

/*
 * Translate a trapvec high-canonical alias address back to the
 * link-time (physical / identity-mapped) address so ksym_lookup
 * can find the right symbol.  IDT vectors, alltraps, syscall_entry,
 * and trampoline data live in the alias window:
 *   [TRAPVEC_ALIAS_BASE .. TRAPVEC_ALIAS_BASE + npages*PGSIZE)
 * mapped from physical pages starting at PGROUNDDOWN(&vector0).
 */
static uint64 bt_dealias(uint64 addr)
{
    extern void vector0(void);
    extern char trapvec_end[];
    uint64 phys_base  = PGROUNDDOWN((uint64)vector0);
    uint64 alias_base = TRAPVEC_ALIAS_BASE;
    uint64 alias_end  = alias_base +
        (PGROUNDDOWN((uint64)trapvec_end) - phys_base) + PGSIZE;

    if (addr >= alias_base && addr < alias_end)
        return addr - alias_base + phys_base;
    return addr;
}

void print_backtrace(uint64 context, uint64 stack_start, uint64 stack_end)
{
    /* Validate the initial frame pointer before walking.
     * If it's outside the kernel stack bounds (e.g. a user-space fp),
     * we must not dereference it — user pages live in a different
     * address space and the pointer may be unmapped or stale. */
    if (context < stack_start || context >= stack_end) {
        printf("  * initial frame outside stack: %p\n", (void *)context);
        return;
    }

    for (uint64 fp = context, depth = 0;
         !BT_IS_TOP_FRAME(fp) && depth < BACKTRACE_MAX_DEPTH;
         fp = BT_PREV_FP(fp), depth++) {

        if (fp < stack_start || fp >= stack_end) {
            printf("  * frame outside stack: %p\n", (void *)fp);
            break;
        }

        uint64 return_addr = BT_RETURN_ADDRESS(fp);
        if (return_addr == 0) {
            printf("  top frame\n");
            break;
        }

        /* return_addr points to the instruction AFTER the call;
         * subtract 1 so we look up an address inside the call insn.
         * Also translate trapvec alias addresses back to link-time
         * addresses so ksym_lookup resolves the right symbol. */
        uint64 lookup_addr = bt_dealias(return_addr - 1);

        char symbuf[64]   = {0};
        char filebuf[128] = {0};
        uint32 line = 0;
        void *sym_addr = NULL;

        int idx = ksym_lookup(lookup_addr, symbuf, sizeof(symbuf), &sym_addr);
        if (idx < 0) {
            printf("  * %p: unknown\n", (void *)return_addr);
        } else {
            ksymbols_t *sym = ksym_search(lookup_addr);
            ksym_get_location(sym, filebuf, sizeof(filebuf), &line);
            int offset = ksym_get_offset(sym, lookup_addr);
            printf("  * %s:%d: %s+%d\n", filebuf, line, symbuf, offset);
        }
    }
}

void print_thread_backtrace(struct context *ctx, uint64 kstack,
                            int kstack_order)
{
    if (ctx == NULL || kstack == 0) {
        printf("backtrace: invalid context or stack\n");
        return;
    }

    uint64 fp = ctx->rbp;      /* frame pointer on x86_64 */
    uint64 stack_size  = (1UL << (PAGE_SHIFT + kstack_order));
    uint64 stack_start = kstack;
    uint64 stack_end   = kstack + stack_size;

    printf("backtrace:\n");

    /* Print the resume-point address from the saved context */
    char symbuf[64]   = {0};
    char filebuf[128] = {0};
    uint32 line = 0;

    ksymbols_t *sym = ksym_search(ctx->ra);
    if (sym == NULL) {
        printf("  > %p: unknown (resume point)\n", (void *)ctx->ra);
    } else {
        if (sym->symbol && sym->symbol_len > 0) {
            size_t copy_len = sym->symbol_len < sizeof(symbuf) - 1
                                  ? sym->symbol_len
                                  : sizeof(symbuf) - 1;
            memmove(symbuf, sym->symbol, copy_len);
            symbuf[copy_len] = '\0';
        }
        ksym_get_location(sym, filebuf, sizeof(filebuf), &line);
        int offset = ksym_get_offset(sym, ctx->ra);
        printf("  > %s:%d: %s+%d (resume point) [%p]\n", filebuf, line, symbuf,
               offset, (void *)ctx->ra);
    }

    /* Walk the stack frames */
    uint64 last_return_addr = ctx->ra;
    int repeat_count        = 0;
    const int MAX_REPEATS   = 3;

    for (uint64 curr_fp = fp, depth = 0;
         !BT_IS_TOP_FRAME(curr_fp) && depth < BACKTRACE_MAX_DEPTH;
         curr_fp = BT_PREV_FP(curr_fp), depth++) {

        if (curr_fp < stack_start || curr_fp >= stack_end) {
            printf("  * frame outside stack: %p\n", (void *)curr_fp);
            break;
        }

        uint64 return_addr = BT_RETURN_ADDRESS(curr_fp);
        if (return_addr == 0)
            break;

        if (return_addr == last_return_addr) {
            repeat_count++;
            if (repeat_count >= MAX_REPEATS) {
                printf("  * ... (%ld more repeated frames)\n", depth);
                break;
            }
        } else {
            repeat_count = 0;
            last_return_addr = return_addr;
        }

        /* return_addr is the insn after the call; -1 to land in the caller.
         * Translate trapvec alias addresses for correct symbol lookup. */
        uint64 lookup_addr = bt_dealias(return_addr - 1);
        sym = ksym_search(lookup_addr);
        if (sym == NULL) {
            printf("  * %p: unknown\n", (void *)return_addr);
        } else {
            if (sym->symbol && sym->symbol_len > 0) {
                size_t copy_len = sym->symbol_len < sizeof(symbuf) - 1
                                      ? sym->symbol_len
                                      : sizeof(symbuf) - 1;
                memmove(symbuf, sym->symbol, copy_len);
                symbuf[copy_len] = '\0';
            } else {
                symbuf[0] = '\0';
            }
            ksym_get_location(sym, filebuf, sizeof(filebuf), &line);
            int offset = ksym_get_offset(sym, lookup_addr);
            printf("  * %s:%d: %s+%d [%p]\n", filebuf, line, symbuf, offset,
                   (void *)return_addr);
        }
    }
}

/**
 * Read a uint64 from user virtual address space via the page table.
 * Returns 0 on success, -1 if the page is not mapped.
 */
static int read_user_u64(pagetable_t pgtbl, uint64 uva, uint64 *out)
{
    uint64 pa = walkaddr(pgtbl, PGROUNDDOWN(uva));
    if (pa == 0)
        return -1;
    /* x86_64 uses identity mapping: PA is directly accessible */
    *out = *(uint64 *)(pa + (uva & (PGSIZE - 1)));
    return 0;
}

/**
 * Walk the x86_64 frame-pointer chain in user space and print return
 * addresses.  Useful for post-mortem analysis with addr2line.
 * Falls back to a raw stack scan when the fp chain is unusable.
 *
 * x86_64 SysV ABI frame layout:
 *   *(rbp)     = previous rbp (caller's frame pointer)
 *   *(rbp + 8) = return address
 *
 * @param pgtbl   User page table
 * @param fp      User rbp (frame pointer) at time of trap
 * @param ra      User rip (return address / program counter) at time of trap
 * @param sp      User rsp (stack pointer) at time of trap
 * @param sepc    User rip at time of trap (same as ra on x86)
 * @param max     Maximum frames to walk
 */
void print_user_backtrace(pagetable_t pgtbl, uint64 fp, uint64 ra,
                          uint64 sp, uint64 sepc, int max)
{
    int frame = 0;
    printf("user backtrace (rip=0x%lx):\n", sepc);
    printf("  #%d  pc=0x%lx\n", frame++, sepc);

    /* Try fp-chain walk (x86_64: *(rbp) = prev_rbp, *(rbp+8) = ret_addr) */
    int fp_ok = 0;
    uint64 prev_fp = 0;
    uint64 cur_fp = fp;
    for (; frame < max && cur_fp != 0; frame++) {
        uint64 saved_ra, next_fp;
        if (read_user_u64(pgtbl, cur_fp + 8, &saved_ra) != 0)
            break;
        if (read_user_u64(pgtbl, cur_fp, &next_fp) != 0)
            break;

        fp_ok++;
        printf("  #%d  pc=0x%lx  (fp=0x%lx)\n", frame, saved_ra, cur_fp);

        if (saved_ra == 0)
            break;
        if (next_fp != 0 && next_fp <= prev_fp && prev_fp != 0)
            break;
        prev_fp = cur_fp;
        cur_fp = next_fp;
    }

    /* Fall back to raw stack scan if fp chain gave < 2 frames */
    if (fp_ok < 2 && sp != 0) {
        printf("  -- stack scan from sp=0x%lx --\n", sp);
        int found = 0;
        for (int si = 0; si < 64 && found < max; si++) {
            uint64 addr = sp + si * 8;
            uint64 val;
            if (read_user_u64(pgtbl, addr, &val) != 0)
                break;
            if (val >= 0x1000 && val < UVMTOP &&
                val != sp && val != fp) {
                printf("    [sp+%3d] 0x%lx = 0x%lx\n", si * 8, addr, val);
                found++;
            }
        }
    }
}
