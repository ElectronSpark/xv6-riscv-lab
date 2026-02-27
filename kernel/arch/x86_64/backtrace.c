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

#define BT_PREV_FP(fp)        ((fp) ? *(uint64 *)(uint64)(fp) : 0)
#define BT_RETURN_ADDRESS(fp)  ((fp) ? *(uint64 *)((uint64)(fp) + 8) : 0)
#define BT_IS_TOP_FRAME(fp) \
    (!(uint64)(fp) || (uint64)(fp) == PGROUNDDOWN((uint64)(fp)))

void print_backtrace(uint64 context, uint64 stack_start, uint64 stack_end)
{
    printf("backtrace:\n");

    for (uint64 fp = context, depth = 0;
         !BT_IS_TOP_FRAME(fp) && depth < BACKTRACE_MAX_DEPTH;
         fp = BT_PREV_FP(fp), depth++) {

        if (fp < stack_start || fp >= stack_end) {
            printf("  * unknown frame: %p\n", (void *)fp);
            break;
        }

        uint64 return_addr = BT_RETURN_ADDRESS(fp);
        if (return_addr == 0) {
            printf("  top frame\n");
            break;
        }

        /* return_addr points to the instruction AFTER the call;
         * subtract 1 so we look up an address inside the call insn. */
        uint64 lookup_addr = return_addr - 1;

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

        /* return_addr is the insn after the call; -1 to land in the caller */
        uint64 lookup_addr = return_addr - 1;
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
