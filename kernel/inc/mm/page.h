#ifndef __KERNEL_PAGE_H
#define __KERNEL_PAGE_H

#include "compiler.h"
#include <mm/page_type.h>

uint64 managed_page_base();
page_t *__pa_to_page(uint64 physical);
uint64 __page_to_pa(page_t *page);
void page_lock_acquire(page_t *page) __acquires(page);
void page_lock_release(page_t *page) __releases(page);
void page_lock_assert_holding(page_t *page) __must_hold(page);
void page_lock_assert_unholding(page_t *page);
int page_buddy_init(void);

// Allocate pages of order 2^order.
// flags: page type (PAGE_TYPE_*) OR'd with optional GFP flags:
//   - Default (no GFP_HIGHMEM): allocates from low memory (DMA-safe)
//   - GFP_HIGHMEM: allows allocation from high memory zone (prefers highmem,
//     falls back to lowmem). High memory pages are NOT suitable for DMA.
page_t *__page_alloc(uint64 order, uint64 flags);
void __page_free(page_t *page, uint64 order);
void *page_alloc(uint64 order, uint64 flags);
void page_free(void *ptr, uint64 order);
int __page_ref_inc(page_t *page);
int __page_ref_dec(page_t *page);
int __page_ref_add(page_t *page, int n);
int __page_ref_sub(page_t *page, int n);
int page_ref_inc_unlocked(page_t *page) __must_hold(page);
int page_ref_dec_unlocked(page_t *page) __must_hold(page);
int page_refcnt(void *physical);
int page_ref_inc(void *ptr);
int page_ref_dec(void *ptr);
int page_ref_count(page_t *page);
void page_free_anon_batch(page_t **pages, int count);

/* ---- U9b anon-frame recycling diagnostic (default OFF, cmdline-gated) ----
 *
 * anon_free_poison=1  : when an anonymous frame's last reference is dropped
 *                       and it returns to the allocator, canary-fill it.
 * anon_fault_verify=1 : at the anon/.bss demand-fault zero-fill, verify the
 *                       frame is untouched (all-canary/all-zero) before it is
 *                       zeroed; a surviving canary plus a foreign word proves a
 *                       kernel free-then-write (the R5-class residue).
 *
 * The canary is a distinctive high tag OR'd with the frame's pfn, so every
 * frame carries a unique 8-byte word and a chance match is effectively
 * impossible. Encoding the pfn lets the fault-side verify recompute the exact
 * word a given frame was filled with. */
#define ANON_POISON_TAG 0xA5A5A5A5UL
static inline uint64 anon_poison_word_for_pa(uint64 pa)
{
    return (ANON_POISON_TAG << 32) | ((pa >> 12) & 0xFFFFFFFFUL);
}
int anon_free_poison_enabled(void);
int anon_fault_verify_enabled(void);
/* Canary-fill [pa, pa + (PGSIZE<<order)). Caller must own the frame
 * exclusively (ref_count just reached 0) so this never races a live map/DMA. */
void anon_frame_poison(uint64 pa, uint64 order);

/* Free-site attribution for poisoned frames: which sink poisoned this pfn
 * last, from which thread, holding which page type, and when.  Lock-free
 * pfn-indexed table (last-writer-wins; diagnostic only). */
struct anon_poison_free_rec {
    uint64 pfn;      /* pfn recorded (0 = empty slot) */
    uint64 jiffs;    /* get_jiffs() at poison time */
    int pid;         /* freeing thread pid (-1 if none) */
    char name[16];   /* freeing thread name */
    uint8 sink;      /* 1=anon_batch 2=ref_dec 3=page_free */
    uint8 page_type; /* PAGE_FLAG_GET_TYPE at free */
};
void anon_poison_note_free(uint64 pa, uint64 order, uint8 sink,
                           uint8 page_type);
int anon_poison_lookup_free(uint64 pa, struct anon_poison_free_rec *out);

void page_buddy_stat(uint64 *ret_arr, bool *empty_arr, size_t size);
void print_buddy_system_stat(int detailed);

#endif /* __KERNEL_PAGE_H */
