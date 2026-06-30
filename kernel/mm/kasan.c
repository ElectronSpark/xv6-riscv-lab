#include "types.h"
#include "printf.h"
#include "compiler.h"
#include "riscv.h"
#include <mm/kasan.h>
#include <mm/memlayout.h>
#include <mm/page.h>
#include <mm/page_type.h>

#ifdef XV6_KASAN

static volatile int kasan_ready;
static volatile int kasan_reporting;
static volatile uint64 kasan_reports;

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

        page = __pa_to_page(cur_pa);
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

void __no_sanitize_address kasan_enable(void)
{
    __atomic_store_n(&kasan_ready, 1, __ATOMIC_RELEASE);
    printf("kasan: enabled page-state callback checks\n");
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
