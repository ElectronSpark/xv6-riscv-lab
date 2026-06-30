#include "types.h"
#include "printf.h"
#include "compiler.h"
#include "riscv.h"
#include <mm/kasan.h>
#include <mm/memlayout.h>
#include <mm/page.h>
#include <mm/page_type.h>

#ifdef XV6_KASAN

#define KASAN_PAGE_POISON 0xff

static volatile int kasan_ready;
static volatile int kasan_reporting;
static volatile uint64 kasan_reports;
static uint8 *kasan_page_shadow;
static uint64 kasan_page_shadow_len;
static uint64 kasan_page_shadow_bytes;

static __no_sanitize_address int kasan_addr_to_pa(uint64 addr, uint64 *pa)
{
    uint64 candidate;

    if (pa == NULL)
        return 0;

    if (addr >= KERNBASE && addr < PHYSTOP) {
        *pa = addr;
        return 1;
    }

#if defined(__x86_64__) || defined(__riscv)
    candidate = VA2PA(addr);
    if (candidate >= KERNBASE && candidate < PHYSTOP) {
        *pa = candidate;
        return 1;
    }
#else
    (void)candidate;
#endif

    return 0;
}

static __no_sanitize_address int kasan_pa_to_index(uint64 pa, uint64 *index)
{
    if (index == NULL)
        return 0;

    pa = PGROUNDDOWN(pa);
    if (pa < KERNBASE || pa >= PHYSTOP)
        return 0;

    *index = (pa - KERNBASE) >> PAGE_SHIFT;
    if (*index >= kasan_page_shadow_len)
        return 0;
    return 1;
}

static __no_sanitize_address int kasan_shadow_test(uint64 index)
{
    uint64 byte = index >> 3;
    uint8 bit = 1U << (index & 7);

    if (kasan_page_shadow == NULL || index >= kasan_page_shadow_len ||
        byte >= kasan_page_shadow_bytes)
        return 0;
    return (kasan_page_shadow[byte] & bit) != 0;
}

static __no_sanitize_address void kasan_shadow_mark(uint64 index, int poisoned)
{
    uint64 byte = index >> 3;
    uint8 bit = 1U << (index & 7);

    if (kasan_page_shadow == NULL || index >= kasan_page_shadow_len ||
        byte >= kasan_page_shadow_bytes)
        return;

    if (poisoned)
        kasan_page_shadow[byte] |= bit;
    else
        kasan_page_shadow[byte] &= (uint8)~bit;
}

static __no_sanitize_address void
kasan_shadow_set_pa(uint64 start_pa, uint64 size, int poisoned)
{
    uint64 start;
    uint64 end;

    if (kasan_page_shadow == NULL || size == 0)
        return;
    if (start_pa + size < start_pa)
        return;

    start = PGROUNDDOWN(start_pa);
    end = PGROUNDDOWN(start_pa + size - 1);

    for (uint64 pa = start; pa <= end;) {
        uint64 index;

        if (kasan_pa_to_index(pa, &index))
            kasan_shadow_mark(index, poisoned);

        if (pa + PGSIZE <= pa || pa == end)
            break;
        pa += PGSIZE;
    }
}

static __no_sanitize_address void
kasan_report(uint64 addr, size_t size, int write, const char *reason,
             unsigned long ret_ip)
{
    if (__atomic_test_and_set(&kasan_reporting, __ATOMIC_ACQUIRE))
        return;

    kasan_reports++;
    printf("kasan: %s addr=0x%lx size=%lu access=%s caller=0x%lx "
           "reports=%lu\n",
           reason, addr, (uint64)size, write ? "write" : "read", ret_ip,
           kasan_reports);

    __atomic_clear(&kasan_reporting, __ATOMIC_RELEASE);
}

void __no_sanitize_address kasan_poison(const void *addr, size_t size,
                                        uint8 tag)
{
    uint64 start = (uint64)addr;
    uint64 end;

    (void)tag;
    if (size == 0 || start + size < start)
        return;

    end = start + size - 1;
    for (uint64 cur = start; cur <= end;) {
        uint64 pa;
        uint64 next = (cur + PGSIZE) & ~(PGSIZE - 1);

        if (kasan_addr_to_pa(cur, &pa))
            kasan_shadow_set_pa(pa, 1, 1);

        if (next <= cur || next > end)
            break;
        cur = next;
    }
}

void __no_sanitize_address kasan_unpoison(const void *addr, size_t size)
{
    uint64 start = (uint64)addr;
    uint64 end;

    if (size == 0 || start + size < start)
        return;

    end = start + size - 1;
    for (uint64 cur = start; cur <= end;) {
        uint64 pa;
        uint64 next = (cur + PGSIZE) & ~(PGSIZE - 1);

        if (kasan_addr_to_pa(cur, &pa))
            kasan_shadow_set_pa(pa, 1, 0);

        if (next <= cur || next > end)
            break;
        cur = next;
    }
}

void __no_sanitize_address kasan_page_alloc(const void *pa, uint64 order)
{
    kasan_unpoison(pa, PGSIZE << order);
}

void __no_sanitize_address kasan_page_free(const void *pa, uint64 order)
{
    kasan_poison(pa, PGSIZE << order, KASAN_PAGE_POISON);
}

int __no_sanitize_address kasan_check_range(const void *addr, size_t size,
                                            int write, unsigned long ret_ip)
{
    uint64 start = (uint64)addr;
    uint64 end;
    uint64 pa;

    if (!__atomic_load_n(&kasan_ready, __ATOMIC_ACQUIRE) || size == 0)
        return 0;
    if (start < PGSIZE) {
        kasan_report(start, size, write, "low-address-access", ret_ip);
        return -1;
    }
    if (start + size < start) {
        kasan_report(start, size, write, "range-overflow", ret_ip);
        return -1;
    }

    end = start + size - 1;
    if (!kasan_addr_to_pa(start, &pa) || !kasan_addr_to_pa(end, &pa))
        return 0;

    for (uint64 cur = start; cur <= end;) {
        uint64 cur_pa;
        page_t *page;
        uint64 type;
        int ref;
        uint64 next = (cur + PGSIZE) & ~(PGSIZE - 1);

        if (!kasan_addr_to_pa(cur, &cur_pa))
            return 0;

        if (kasan_page_shadow != NULL) {
            uint64 index;

            if (kasan_pa_to_index(cur_pa, &index) &&
                kasan_shadow_test(index)) {
                kasan_report(cur, size, write, "poisoned-page-access", ret_ip);
                return -1;
            }
        }

        page = __pa_to_page(PGROUNDDOWN(cur_pa));
        if (page == NULL)
            return 0;
        type = PAGE_FLAG_GET_TYPE(page->flags);
        ref = __atomic_load_n(&page->ref_count, __ATOMIC_ACQUIRE);

        if (type == PAGE_TYPE_BUDDY || ref < 0) {
            kasan_report(cur, size, write, "freed-page-access", ret_ip);
            return -1;
        }

        if (next <= cur || next > end)
            break;
        cur = next;
    }

    return 0;
}

static __no_sanitize_address uint64 kasan_shadow_order(uint64 bytes)
{
    uint64 pages = (bytes + PGSIZE - 1) >> PAGE_SHIFT;
    uint64 order = 0;
    uint64 capacity = 1;

    while (capacity < pages) {
        capacity <<= 1;
        order++;
    }
    return order;
}

static __no_sanitize_address int kasan_page_is_free(uint64 pa)
{
    page_t *page = __pa_to_page(pa);
    uint64 type;

    if (page == NULL)
        return 0;

    type = PAGE_FLAG_GET_TYPE(page->flags);
    if (type == PAGE_TYPE_BUDDY)
        return 1;

    if (type == PAGE_TYPE_TAIL && page->tail.head_page != NULL &&
        PAGE_FLAG_GET_TYPE(page->tail.head_page->flags) == PAGE_TYPE_BUDDY)
        return 1;

    return 0;
}

static __no_sanitize_address int kasan_shadow_init(void)
{
    uint64 pages = TOTALPAGES;
    uint64 bytes = (pages + 7) >> 3;
    uint64 order;
    uint64 managed_base;

    if (pages == 0 || bytes == 0)
        return -1;

    order = kasan_shadow_order(bytes);
    if (order > PAGE_BUDDY_MAX_ORDER)
        return -1;

    kasan_page_shadow = (uint8 *)page_alloc(order, PAGE_TYPE_ANON);
    if (kasan_page_shadow == NULL)
        return -1;

    kasan_page_shadow_len = pages;
    kasan_page_shadow_bytes = bytes;
    managed_base = managed_page_base();

    for (uint64 i = 0; i < kasan_page_shadow_bytes; i++)
        kasan_page_shadow[i] = 0;

    for (uint64 pa = KERNBASE; pa < PHYSTOP;) {
        uint64 index;

        if (pa >= managed_base && kasan_pa_to_index(pa, &index) &&
            kasan_page_is_free(pa))
            kasan_shadow_mark(index, 1);

        if (pa + PGSIZE <= pa)
            break;
        pa += PGSIZE;
    }

    return 0;
}

void __no_sanitize_address kasan_enable(void)
{
    if (kasan_shadow_init() != 0) {
        printf("kasan: disabled, failed to initialize page shadow\n");
        return;
    }

    __atomic_store_n(&kasan_ready, 1, __ATOMIC_RELEASE);
    printf("kasan: enabled page-shadow callback checks (%lu pages, %lu bytes)\n",
           kasan_page_shadow_len, kasan_page_shadow_bytes);
}

void __no_sanitize_address kasan_disable(void)
{
    __atomic_store_n(&kasan_ready, 0, __ATOMIC_RELEASE);
}

static __no_sanitize_address void kasan_load(uint64 addr, size_t size,
                                             unsigned long ret_ip)
{
    kasan_check_range((const void *)addr, size, 0, ret_ip);
}

static __no_sanitize_address void kasan_store(uint64 addr, size_t size,
                                              unsigned long ret_ip)
{
    kasan_check_range((const void *)addr, size, 1, ret_ip);
}

#define DEFINE_ASAN_LOAD(size)                                                \
    void __no_sanitize_address __asan_load##size##_noabort(uint64 addr)       \
    {                                                                         \
        kasan_load(addr, size, (unsigned long)__builtin_return_address(0));    \
    }

#define DEFINE_ASAN_STORE(size)                                               \
    void __no_sanitize_address __asan_store##size##_noabort(uint64 addr)      \
    {                                                                         \
        kasan_store(addr, size, (unsigned long)__builtin_return_address(0));   \
    }

DEFINE_ASAN_LOAD(1)
DEFINE_ASAN_LOAD(2)
DEFINE_ASAN_LOAD(4)
DEFINE_ASAN_LOAD(8)
DEFINE_ASAN_LOAD(16)
DEFINE_ASAN_STORE(1)
DEFINE_ASAN_STORE(2)
DEFINE_ASAN_STORE(4)
DEFINE_ASAN_STORE(8)
DEFINE_ASAN_STORE(16)

void __no_sanitize_address __asan_loadN_noabort(uint64 addr, size_t size)
{
    kasan_load(addr, size, (unsigned long)__builtin_return_address(0));
}

void __no_sanitize_address __asan_storeN_noabort(uint64 addr, size_t size)
{
    kasan_store(addr, size, (unsigned long)__builtin_return_address(0));
}

void __no_sanitize_address __asan_handle_no_return(void) {}

#endif /* XV6_KASAN */
