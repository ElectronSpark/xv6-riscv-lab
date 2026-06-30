#include "types.h"
#include "printf.h"
#include "defs.h"
#include "compiler.h"
#include "riscv.h"
#include "cmdline.h"
#include "string.h"
#include <mm/kasan.h>
#include <mm/memlayout.h>
#include <mm/page.h>
#include <mm/page_type.h>
#include <mm/slab.h>
#include <mm/vm.h>

#ifdef XV6_KASAN

#define KASAN_PAGE_POISON 0xff
#define KASAN_VMALLOC_TABLE_SIZE 4096

struct kasan_vmalloc_entry {
    uint64 start;
    uint64 requested;
    uint64 allocated;
    uint64 caller;
    const char *tag;
    uint8 active;
};

static volatile int kasan_ready;
static volatile int kasan_config_checked;
static volatile int kasan_config_enable = 1;
static volatile int kasan_reporting;
static volatile uint64 kasan_reports;
static uint8 *kasan_page_shadow;
static uint64 kasan_page_shadow_len;
static uint64 kasan_page_shadow_bytes;
static struct kasan_vmalloc_entry kasan_vmalloc_table[KASAN_VMALLOC_TABLE_SIZE];
static volatile uint64 kasan_vmalloc_evict_cursor;

void __no_sanitize_address __asan_store1_noabort(uint64 addr);

static __no_sanitize_address int kasan_streq_ignore_case(const char *value,
                                                         const char *word)
{
    while (*word != '\0') {
        char c = *value++;
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if (c != *word++)
            return 0;
    }
    return *value == '\0';
}

static __no_sanitize_address int kasan_value_is_false(const char *value)
{
    if (value == NULL || value[0] == '\0')
        return 0;
    return kasan_streq_ignore_case(value, "0") ||
           kasan_streq_ignore_case(value, "off") ||
           kasan_streq_ignore_case(value, "false") ||
           kasan_streq_ignore_case(value, "no");
}

static __no_sanitize_address int kasan_value_is_true(const char *value)
{
    if (value == NULL || value[0] == '\0')
        return 0;
    return kasan_streq_ignore_case(value, "1") ||
           kasan_streq_ignore_case(value, "on") ||
           kasan_streq_ignore_case(value, "true") ||
           kasan_streq_ignore_case(value, "yes");
}

int __no_sanitize_address kasan_config_enabled(void)
{
    char value[16];
    int enabled = 1;

    if (__atomic_load_n(&kasan_config_checked, __ATOMIC_ACQUIRE))
        return __atomic_load_n(&kasan_config_enable, __ATOMIC_ACQUIRE);

    if (cmdline_get_param("kasan", value, sizeof(value)) == 0 &&
        kasan_value_is_false(value))
        enabled = 0;

    __atomic_store_n(&kasan_config_enable, enabled, __ATOMIC_RELEASE);
    __atomic_store_n(&kasan_config_checked, 1, __ATOMIC_RELEASE);
    return enabled;
}

int __no_sanitize_address kasan_enabled(void)
{
    return __atomic_load_n(&kasan_ready, __ATOMIC_ACQUIRE);
}

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

static __no_sanitize_address uint64 kasan_vmalloc_hash(uint64 addr)
{
    addr >>= PAGE_SHIFT;
    addr ^= addr >> 33;
    addr *= 0xff51afd7ed558ccdULL;
    addr ^= addr >> 33;
    return addr;
}

static __no_sanitize_address int kasan_range_end(uint64 start, size_t size,
                                                 uint64 *end)
{
    if (end == NULL || size == 0 || start + size < start)
        return 0;
    *end = start + size;
    return 1;
}

static __no_sanitize_address int
kasan_range_overlaps(uint64 a_start, uint64 a_size, uint64 b_start,
                     uint64 b_size)
{
    uint64 a_end;
    uint64 b_end;

    if (!kasan_range_end(a_start, a_size, &a_end) ||
        !kasan_range_end(b_start, b_size, &b_end))
        return 0;
    return a_start < b_end && b_start < a_end;
}

static __no_sanitize_address struct kasan_vmalloc_entry *
kasan_vmalloc_find_start(uint64 start)
{
    uint64 slot = kasan_vmalloc_hash(start) & (KASAN_VMALLOC_TABLE_SIZE - 1);

    for (uint64 i = 0; i < KASAN_VMALLOC_TABLE_SIZE; i++) {
        struct kasan_vmalloc_entry *entry =
            &kasan_vmalloc_table[(slot + i) & (KASAN_VMALLOC_TABLE_SIZE - 1)];
        uint64 seen = __atomic_load_n(&entry->start, __ATOMIC_ACQUIRE);

        if (seen == start)
            return entry;
        if (seen == 0)
            return NULL;
    }
    return NULL;
}

static __no_sanitize_address struct kasan_vmalloc_entry *
kasan_vmalloc_claim_slot(uint64 start)
{
    uint64 slot = kasan_vmalloc_hash(start) & (KASAN_VMALLOC_TABLE_SIZE - 1);

    for (uint64 i = 0; i < KASAN_VMALLOC_TABLE_SIZE; i++) {
        struct kasan_vmalloc_entry *entry =
            &kasan_vmalloc_table[(slot + i) & (KASAN_VMALLOC_TABLE_SIZE - 1)];
        uint64 seen = __atomic_load_n(&entry->start, __ATOMIC_ACQUIRE);

        if (seen == start)
            return entry;
        if (seen == 0) {
            uint64 expected = 0;

            if (__atomic_compare_exchange_n(&entry->start, &expected, start,
                                            false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE))
                return entry;
        }
    }

    slot = __atomic_fetch_add(&kasan_vmalloc_evict_cursor, 1,
                              __ATOMIC_RELAXED) &
           (KASAN_VMALLOC_TABLE_SIZE - 1);
    kasan_vmalloc_table[slot].active = 0;
    __atomic_store_n(&kasan_vmalloc_table[slot].start, start,
                     __ATOMIC_RELEASE);
    return &kasan_vmalloc_table[slot];
}

void __no_sanitize_address
kasan_vmalloc_alloc(const void *addr, size_t requested, size_t allocated,
                    const char *tag, unsigned long caller)
{
    uint64 start = (uint64)addr;
    struct kasan_vmalloc_entry *entry;

    if (!__atomic_load_n(&kasan_ready, __ATOMIC_ACQUIRE) || start == 0 ||
        requested == 0 || allocated == 0)
        return;
    if (requested > allocated)
        requested = allocated;
    if (start + allocated < start)
        return;

    entry = kasan_vmalloc_claim_slot(start);
    if (entry == NULL)
        return;

    __atomic_store_n(&entry->active, 0, __ATOMIC_RELEASE);
    entry->requested = requested;
    entry->allocated = allocated;
    entry->caller = caller;
    entry->tag = tag != NULL ? tag : "?";
    __atomic_store_n(&entry->active, 1, __ATOMIC_RELEASE);
}

void __no_sanitize_address kasan_vmalloc_free(const void *addr, size_t size)
{
    uint64 start = (uint64)addr;
    struct kasan_vmalloc_entry *entry;

    if (!__atomic_load_n(&kasan_ready, __ATOMIC_ACQUIRE) || start == 0)
        return;

    entry = kasan_vmalloc_find_start(start);
    if (entry == NULL)
        return;
    if (size != 0 && entry->allocated != 0 && size != entry->allocated)
        entry->allocated = size;
    __atomic_store_n(&entry->active, 0, __ATOMIC_RELEASE);
}

static __no_sanitize_address int
kasan_vmalloc_check_range(const void *addr, size_t size, int write,
                          unsigned long ret_ip)
{
    uint64 start = (uint64)addr;
    uint64 end;
    int inactive_overlap = 0;

    if (!kasan_range_end(start, size, &end))
        return 0;

    for (uint64 i = 0; i < KASAN_VMALLOC_TABLE_SIZE; i++) {
        struct kasan_vmalloc_entry *entry = &kasan_vmalloc_table[i];
        uint64 entry_start = __atomic_load_n(&entry->start, __ATOMIC_ACQUIRE);
        uint64 allocated = entry->allocated;
        uint64 requested = entry->requested;

        if (entry_start == 0 || allocated == 0 ||
            !__atomic_load_n(&entry->active, __ATOMIC_ACQUIRE))
            continue;
        if (start >= entry_start && end <= entry_start + allocated) {
            if (end > entry_start + requested) {
                kasan_report_access(start, size, write,
                                    "kvmalloc-object-overflow", ret_ip);
                return -1;
            }
            return 1;
        }
        if (kasan_range_overlaps(start, size, entry_start, allocated)) {
            kasan_report_access(start, size, write, "kvmalloc-out-of-range",
                                ret_ip);
            return -1;
        }
    }

    for (uint64 i = 0; i < KASAN_VMALLOC_TABLE_SIZE; i++) {
        struct kasan_vmalloc_entry *entry = &kasan_vmalloc_table[i];
        uint64 entry_start = __atomic_load_n(&entry->start, __ATOMIC_ACQUIRE);
        uint64 allocated = entry->allocated;

        if (entry_start == 0 || allocated == 0 ||
            __atomic_load_n(&entry->active, __ATOMIC_ACQUIRE))
            continue;
        if (kasan_range_overlaps(start, size, entry_start, allocated)) {
            inactive_overlap = 1;
            break;
        }
    }

    if (inactive_overlap) {
        kasan_report_access(start, size, write, "kvmalloc-free-access",
                            ret_ip);
        return -1;
    }
    return 0;
}

void __no_sanitize_address
kasan_report_access(uint64 addr, size_t size, int write, const char *reason,
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
    int vmalloc_check;

    if (!__atomic_load_n(&kasan_ready, __ATOMIC_ACQUIRE) || size == 0)
        return 0;
    if (start < PGSIZE) {
        kasan_report_access(start, size, write, "low-address-access", ret_ip);
        return -1;
    }
    if (start + size < start) {
        kasan_report_access(start, size, write, "range-overflow", ret_ip);
        return -1;
    }

    end = start + size - 1;
    vmalloc_check = kasan_vmalloc_check_range(addr, size, write, ret_ip);
    if (vmalloc_check < 0)
        return -1;
    if (vmalloc_check > 0)
        return 0;

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
                kasan_report_access(cur, size, write, "poisoned-page-access",
                                    ret_ip);
                return -1;
            }
        }

        page = __pa_to_page(PGROUNDDOWN(cur_pa));
        if (page == NULL)
            return 0;
        type = PAGE_FLAG_GET_TYPE(page->flags);
        ref = __atomic_load_n(&page->ref_count, __ATOMIC_ACQUIRE);

        if (type == PAGE_TYPE_BUDDY || ref < 0) {
            kasan_report_access(cur, size, write, "freed-page-access",
                                ret_ip);
            return -1;
        }
        if ((type == PAGE_TYPE_SLAB || type == PAGE_TYPE_TAIL) &&
            slab_kasan_check_range((const void *)cur, size, write, ret_ip) <
                0)
            return -1;

        if (next <= cur || next > end)
            break;
        cur = next;
    }

    return 0;
}

static __no_sanitize_address void kasan_maybe_selftest(void)
{
    char value[16];
    int page_pass = 0;
    int callback_pass = 0;
    int slab_inbounds_pass = 0;
    int slab_bounds_pass = 0;
    int slab_requested_bounds_pass = 0;
    int slab_uaf_pass = 0;
    int string_pass = 0;

    if (cmdline_get_param("kasan_selftest", value, sizeof(value)) != 0 ||
        !kasan_value_is_true(value))
        return;

    void *page = page_alloc(0, PAGE_TYPE_ANON);
    if (page != NULL) {
        uint64 reports_before;

        page_free(page, 0);
        page_pass = kasan_check_range(page, 1, 1, 0) < 0;
        reports_before = __atomic_load_n(&kasan_reports, __ATOMIC_ACQUIRE);
        __asan_store1_noabort((uint64)page);
        callback_pass =
            __atomic_load_n(&kasan_reports, __ATOMIC_ACQUIRE) >
            reports_before;
    }

    void *obj = kmm_alloc(64);
    if (obj != NULL) {
        char *bytes = obj;

        slab_inbounds_pass = kasan_check_range(obj, 64, 0, 0) == 0;
        slab_bounds_pass = kasan_check_range(bytes + 63, 2, 1, 0) < 0;
        kmm_free(obj);
        slab_uaf_pass = kasan_check_range(obj, 1, 0, 0) < 0;
    }

    void *requested_obj = kmm_alloc(33);
    if (requested_obj != NULL) {
        char *bytes = requested_obj;

        slab_requested_bounds_pass =
            kasan_check_range(bytes + 32, 1, 1, 0) == 0 &&
            kasan_check_range(bytes + 33, 1, 1, 0) < 0;
        kmm_free(requested_obj);
    }

    void *string_page = page_alloc(0, PAGE_TYPE_ANON);
    if (string_page != NULL) {
        uint64 reports_before;
        char *bytes = string_page;

        kasan_poison(string_page, PGSIZE, KASAN_PAGE_POISON);
        reports_before = __atomic_load_n(&kasan_reports, __ATOMIC_ACQUIRE);
        memset(string_page, 0, 1);
        memmove(bytes + 8, bytes, 1);
        (void)memcmp(bytes, bytes + 8, 1);
        string_pass =
            __atomic_load_n(&kasan_reports, __ATOMIC_ACQUIRE) >
            reports_before;
        kasan_unpoison(string_page, PGSIZE);
        page_free(string_page, 0);
    }

    printf("kasan: selftest page_uaf=%s callback=%s slab_inbounds=%s "
           "slab_bounds=%s slab_requested_bounds=%s slab_uaf=%s "
           "string=%s status=%s\n",
           page_pass ? "PASS" : "FAIL", callback_pass ? "PASS" : "FAIL",
           slab_inbounds_pass ? "PASS" : "FAIL",
           slab_bounds_pass ? "PASS" : "FAIL",
           slab_requested_bounds_pass ? "PASS" : "FAIL",
           slab_uaf_pass ? "PASS" : "FAIL",
           string_pass ? "PASS" : "FAIL",
           (page_pass && callback_pass && slab_inbounds_pass &&
            slab_bounds_pass && slab_requested_bounds_pass && slab_uaf_pass &&
            string_pass) ?
               "PASS" :
               "FAIL");
}

void __no_sanitize_address kasan_vmalloc_selftest(void)
{
    char value[16];
    int inbounds_pass = 0;
    int overflow_pass = 0;
    int uaf_pass = 0;
    size_t requested = PGSIZE + 128;

    if (!__atomic_load_n(&kasan_ready, __ATOMIC_ACQUIRE))
        return;
    if (cmdline_get_param("kasan_selftest", value, sizeof(value)) != 0 ||
        !kasan_value_is_true(value))
        return;

    char *ptr = kvmalloc(requested);
    if (ptr != NULL) {
        inbounds_pass = kasan_check_range(ptr + requested - 1, 1, 1, 0) == 0;
        overflow_pass = kasan_check_range(ptr + requested, 1, 1, 0) < 0;
        kvfree(ptr);
        uaf_pass = kasan_check_range(ptr, 1, 0, 0) < 0;
    }

    printf("kasan: vmalloc_selftest inbounds=%s overflow=%s uaf=%s "
           "status=%s\n",
           inbounds_pass ? "PASS" : "FAIL",
           overflow_pass ? "PASS" : "FAIL", uaf_pass ? "PASS" : "FAIL",
           (inbounds_pass && overflow_pass && uaf_pass) ? "PASS" : "FAIL");
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

    void *shadow_pa = page_alloc(order, PAGE_TYPE_ANON);
    if (shadow_pa == NULL)
        return -1;
    kasan_page_shadow = (uint8 *)PA2VA(shadow_pa);

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
    if (!kasan_config_enabled()) {
        printf("kasan: disabled by cmdline\n");
        return;
    }

    if (kasan_shadow_init() != 0) {
        printf("kasan: disabled, failed to initialize page shadow\n");
        return;
    }

    __atomic_store_n(&kasan_ready, 1, __ATOMIC_RELEASE);
    printf("kasan: enabled page-shadow callback checks (%lu pages, %lu bytes)\n",
           kasan_page_shadow_len, kasan_page_shadow_bytes);
    kasan_maybe_selftest();
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
