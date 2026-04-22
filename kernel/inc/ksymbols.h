#ifndef __KERNEL_KSYMBOLS_H
#define __KERNEL_KSYMBOLS_H

#include "types.h"
#include "rbtree.h"

/**
 * @brief Kernel symbol entry.
 *
 * Stored in an rb-tree keyed by start_addr.  Each entry maps a code
 * address range to a source file, symbol name, and line number.
 */
typedef struct {
    struct rb_node rb;
    void *start_addr;
    uint32 line;
    const char *symbol;
    uint16 symbol_len;
    const char *filename;
    uint16 filename_len;
} ksymbols_t;

/**
 * Search for the symbol containing @addr (largest start_addr <= addr).
 * Returns NULL if no matching symbol is found or if the match is a
 * guard entry (line == 0).
 */
ksymbols_t *ksym_search(uint64 addr);

/**
 * Copy the symbol name into @buf (at most @buflen-1 chars).
 * Optionally returns the symbol start address via @return_addr.
 * Returns a non-negative index on success, -1 on failure.
 */
int ksym_lookup(uint64 addr, char *buf, size_t buflen, void **return_addr);

/**
 * Fill @filebuf with the source filename and @line with the line number
 * for symbol @sym.
 */
void ksym_get_location(ksymbols_t *sym, char *filebuf, size_t filebuflen,
                       uint32 *line);

/**
 * Return the byte offset of @addr from the start of @sym.
 */
int ksym_get_offset(ksymbols_t *sym, uint64 addr);

#endif /* __KERNEL_KSYMBOLS_H */
