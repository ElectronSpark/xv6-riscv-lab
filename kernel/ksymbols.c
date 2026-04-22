/**
 * @file ksymbols.c
 * @brief Kernel symbol table — parsing, rb-tree storage, and lookup.
 *
 * Architecture-independent.  The backtrace walkers (per-arch) call the
 * lookup helpers defined here.
 */

#include "types.h"
#include "string.h"
#include "param.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "bintree.h"
#include "rbtree.h"
#include "ksymbols.h"

/* ── rb-tree plumbing ────────────────────────────────────────────────── */

static int ksym_keys_cmp(uint64 a, uint64 b)
{
    ksymbols_t *sym_a = (ksymbols_t *)a;
    ksymbols_t *sym_b = (ksymbols_t *)b;

    if ((uint64)sym_a->start_addr < (uint64)sym_b->start_addr)
        return -1;
    if ((uint64)sym_a->start_addr > (uint64)sym_b->start_addr)
        return 1;

    /* Secondary key: node pointer as tie-breaker for duplicates */
    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return 0;
}

static uint64 ksym_get_key(struct rb_node *node)
{
    ksymbols_t *sym = container_of(node, ksymbols_t, rb);
    return (uint64)sym;
}

static struct rb_root_opts __ksym_rb_opts = {
    .keys_cmp_fun = ksym_keys_cmp,
    .get_key_fun  = ksym_get_key,
};

/* No separate rdown comparison needed — ksym_search walks the tree
 * manually using only the start_addr field for round-down lookup. */

static struct rb_root __ksym_rb_root = {
    .node = NULL,
    .opts = &__ksym_rb_opts,
};

static ksymbols_t *__ksymbols;      /* storage pool */
static int __ksymbol_count = -1;

/* ── Initialisation ──────────────────────────────────────────────────── */

void ksymbols_init(void)
{
    __ksymbols      = (void *)KERNEL_SYMBOLS_IDX_START;
    __ksymbol_count = 0;

    if (KERNEL_SYMBOLS_SIZE == 0 ||
        KERNEL_SYMBOLS_START == KERNEL_SYMBOLS_END) {
        printf("ksymbols: no embedded symbols found\n");
        return;
    }

    printf("ksymbols: loading embedded symbols from 0x%lx-0x%lx (%ld bytes)\n",
           KERNEL_SYMBOLS_START, KERNEL_SYMBOLS_END, KERNEL_SYMBOLS_SIZE);

    const char *current_file     = NULL;
    int         current_file_len = 0;
    const char *current_symbol     = NULL;
    int         current_symbol_len = 0;

    const char *line_start = (const char *)KERNEL_SYMBOLS_START;
    const char *end        = (const char *)KERNEL_SYMBOLS_END;

    for (const char *p = line_start; p < end; p++) {
        if (*p == '\n' || *p == '\0') {
            const char *line_end = p;
            int line_len = line_end - line_start;

            if (line_len == 0) {
                /* empty */
            } else if (line_start[line_len - 1] == ':' &&
                       line_start[0] != ':') {
                current_file     = line_start;
                current_file_len = line_len - 1;
            } else if (line_start[0] == ':') {
                current_symbol     = line_start + 1;
                current_symbol_len = line_len - 1;
            } else {
                char *next = NULL;
                uint64 start_addr = strtoul(line_start, &next, 16);
                if (next && *next == ' ') {
                    next++;
                    uint32 line_num = (uint32)strtoul(next, NULL, 10);

                    if (__ksymbol_count <
                        (int)(KERNEL_SYMBOLS_IDX_SIZE / sizeof(ksymbols_t))) {
                        ksymbols_t *entry = &__ksymbols[__ksymbol_count];
                        entry->start_addr   = (void *)start_addr;
                        entry->line         = line_num;
                        entry->symbol       = current_symbol;
                        entry->symbol_len   = current_symbol_len;
                        entry->filename     = current_file;
                        entry->filename_len = current_file_len;

                        rb_node_init(&entry->rb);
                        rb_insert_color(&__ksym_rb_root, &entry->rb);
                        __ksymbol_count++;
                    }
                }
            }

            line_start = p + 1;
            if (*p == '\0')
                break;
        }
    }

    printf("Kernel symbols initialized: %d entries\n", __ksymbol_count);
}

/* ── Lookup helpers ──────────────────────────────────────────────────── */

/**
 * ksym_search — round-down lookup: largest start_addr ≤ addr.
 *
 * Walk the rb-tree directly, comparing only start_addr.  This avoids
 * the pitfall of using rb_find_key_rdown with a comparison function
 * that differs from the one used during insertion (the tree is keyed
 * by (start_addr, pointer) but we only care about start_addr here).
 */
ksymbols_t *ksym_search(uint64 addr)
{
    if (__ksymbol_count <= 0)
        return NULL;

    struct rb_node *n = __ksym_rb_root.node;
    ksymbols_t *best = NULL;

    while (n != NULL) {
        ksymbols_t *sym = container_of(n, ksymbols_t, rb);
        uint64 sym_addr = (uint64)sym->start_addr;

        if (sym_addr <= addr) {
            /* Candidate — remember it and look for a closer (higher) one. */
            if (best == NULL ||
                sym_addr > (uint64)best->start_addr ||
                (sym_addr == (uint64)best->start_addr && sym->line != 0))
                best = sym;
            n = n->right;
        } else {
            n = n->left;
        }
    }

    if (best == NULL || best->line == 0)
        return NULL;

    return best;
}

int ksym_lookup(uint64 addr, char *buf, size_t buflen, void **return_addr)
{
    ksymbols_t *sym = ksym_search(addr);
    if (sym == NULL) {
        buf[0] = '\0';
        return -1;
    }

    if (sym->symbol && sym->symbol_len > 0) {
        size_t copy_len = sym->symbol_len;
        if (copy_len >= buflen)
            copy_len = buflen - 1;
        memmove(buf, sym->symbol, copy_len);
        buf[copy_len] = '\0';
    } else {
        buf[0] = '\0';
    }

    if (return_addr)
        *return_addr = sym->start_addr;

    return (int)(sym - __ksymbols);
}

void ksym_get_location(ksymbols_t *sym, char *filebuf, size_t filebuflen,
                       uint32 *line)
{
    if (sym == NULL) {
        filebuf[0] = '\0';
        *line = 0;
        return;
    }

    if (sym->filename && sym->filename_len > 0) {
        size_t copy_len = sym->filename_len;
        if (copy_len >= filebuflen)
            copy_len = filebuflen - 1;
        memmove(filebuf, sym->filename, copy_len);
        filebuf[copy_len] = '\0';
    } else {
        filebuf[0] = '\0';
    }

    *line = sym->line;
}

int ksym_get_offset(ksymbols_t *sym, uint64 addr)
{
    if (sym == NULL)
        return 0;
    return (int)((void *)addr - sym->start_addr);
}

/* ── Debug breakpoint stub ───────────────────────────────────────────── */

void db_break(void) { return; }
