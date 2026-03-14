/**
 * @file backtrace.c
 * @brief RISC-V stack-frame walker.
 *
 * RISC-V (with -fno-omit-frame-pointer) stores the return address at
 * (fp - 8) and the previous frame pointer at (fp - 16).
 */

#include "types.h"
#include "string.h"
#include "param.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "proc/thread.h"
#include "defs.h"
#include "printf.h"
#include "ksymbols.h"
#include <mm/pgtable.h>

#define BT_FRAME_TOP(fp)       ((fp) ? *(uint64 *)((uint64)(fp) - 16) : 0)
#define BT_RETURN_ADDRESS(fp)  ((fp) ? *(uint64 *)((uint64)(fp) - 8)  : 0)
#define BT_IS_TOP_FRAME(fp) \
    (!(uint64)(fp) || (uint64)(fp) == PGROUNDDOWN((uint64)(fp)))

void print_backtrace(uint64 context, uint64 stack_start, uint64 stack_end)
{
    printf("backtrace:\n");
    uint64 last_fp = context;

    for (uint64 fp = BT_FRAME_TOP(context), depth = 0;
         !BT_IS_TOP_FRAME(fp) && depth < BACKTRACE_MAX_DEPTH;
         last_fp = fp, fp = BT_FRAME_TOP(fp), depth++) {

        if (fp < stack_start || fp >= stack_end) {
            printf("  * unknown frame: %p\n", (void *)fp);
            break;
        } else if (fp == 0) {
            printf("  top frame\n");
            break;
        }

        char symbuf[64] = {0};
        char filebuf[128] = {0};
        uint32 line = 0;
        void *sym_addr = NULL;
        uint64 return_addr_val = BT_RETURN_ADDRESS(last_fp);
        if (return_addr_val == 0) {
            printf("  top frame\n");
            break;
        }

        /* return_addr is the insn after the call; -1 to land in the caller */
        uint64 lookup_addr = return_addr_val - 1;
        int idx = ksym_lookup(lookup_addr, symbuf, sizeof(symbuf),
                              &sym_addr);
        if (idx < 0) {
            printf("  * %p: unknown\n", (void *)return_addr_val);
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

    uint64 fp = ctx->s0;    /* s0 is the frame pointer on RISC-V */
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
    uint64 last_fp          = fp;
    uint64 last_return_addr = ctx->ra;
    int repeat_count        = 0;
    const int MAX_REPEATS   = 3;

    for (uint64 curr_fp = BT_FRAME_TOP(fp), depth = 0;
         !BT_IS_TOP_FRAME(curr_fp) && depth < BACKTRACE_MAX_DEPTH;
         last_fp = curr_fp, curr_fp = BT_FRAME_TOP(curr_fp), depth++) {

        if (curr_fp < stack_start || curr_fp >= stack_end) {
            printf("  * frame outside stack: %p\n", (void *)curr_fp);
            break;
        }

        uint64 return_addr_val = BT_RETURN_ADDRESS(last_fp);
        if (return_addr_val == 0)
            break;

        if (return_addr_val == last_return_addr) {
            repeat_count++;
            if (repeat_count >= MAX_REPEATS) {
                printf("  * ... (%ld more repeated frames)\n", depth);
                break;
            }
        } else {
            repeat_count = 0;
            last_return_addr = return_addr_val;
        }

        /* return_addr is the insn after the call; -1 to land in the caller */
        uint64 lookup_addr_val = return_addr_val - 1;
        sym = ksym_search(lookup_addr_val);
        if (sym == NULL) {
            printf("  * %p: unknown\n", (void *)return_addr_val);
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
            int offset = ksym_get_offset(sym, lookup_addr_val);
            printf("  * %s:%d: %s+%d [%p]\n", filebuf, line, symbuf, offset,
                   (void *)return_addr_val);
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
    *out = *(uint64 *)((uint64)PA2VA(pa) + (uva & (PGSIZE - 1)));
    return 0;
}

/**
 * Walk the RISC-V frame-pointer chain in user space and print return
 * addresses.  Useful for post-mortem analysis with addr2line.
 * Falls back to a raw stack scan when the fp chain is unusable.
 *
 * @param pgtbl   User page table
 * @param fp      User s0 (frame pointer) at time of trap
 * @param ra      User ra (return address) at time of trap
 * @param sp      User sp (stack pointer) at time of trap
 * @param sepc    User PC at time of trap
 * @param max     Maximum frames to walk
 */
void print_user_backtrace(pagetable_t pgtbl, uint64 fp, uint64 ra,
                          uint64 sp, uint64 sepc, int max)
{
    int frame = 0;
    printf("user backtrace (sepc=0x%lx):\n", sepc);
    printf("  #%d  pc=0x%lx\n", frame++, sepc);

    /* Always print ra from trapframe as frame #1 */
    if (ra != 0 && ra != sepc)
        printf("  #%d  pc=0x%lx  (ra)\n", frame++, ra);

    /* Try fp-chain walk */
    int fp_ok = 0;
    uint64 prev_fp = 0;
    uint64 cur_fp = fp;
    for (; frame < max && cur_fp != 0; frame++) {
        uint64 saved_ra, next_fp;
        if (read_user_u64(pgtbl, cur_fp - 8, &saved_ra) != 0)
            break;
        if (read_user_u64(pgtbl, cur_fp - 16, &next_fp) != 0)
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
            /* Heuristic: looks like a user-space code address if
             * it is page-aligned-ish and in the user VA range.
             * Skip zero/stack-like values. */
            if (val >= 0x1000 && val < UVMTOP &&
                val != sp && val != fp) {
                printf("    [sp+%3d] 0x%lx = 0x%lx\n", si * 8, addr, val);
                found++;
            }
        }
    }
}
