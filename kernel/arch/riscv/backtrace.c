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
