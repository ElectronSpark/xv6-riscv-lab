#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

extern char stack0[];

// Names end with a newline character or a null terminator.
typedef struct {
    void *addr;
    size_t size;
    const char *name;
} __ksymbols_t;

static __ksymbols_t *__ksymbols = (void *)KERNEL_SYMBOLS_IDX_START;

static int __ksymbol_count = -1;

static inline uint64
strtoul(const char *nptr, char **endptr, int base)
{
    uint64 result = 0;
    const char *p = NULL;
    for (p = nptr; *p; p++) {
        int digit = 0;
        if (*p >= '0' && *p <= '9') {
            digit = *p - '0';
        } else if (*p >= 'a' && *p <= 'f') {
            digit = *p - 'a' + 10;
        } else if (*p >= 'A' && *p <= 'F') {
            digit = *p - 'A' + 10;
        } else {
            break;
        }
        if (digit >= base) {
            break;
        }
        result = result * base + digit;
    }
    if (endptr) {
        *endptr = (char *)p;
    }
    return result;
}

static inline void
__ksymbol_parse(int idx,
                const char *saddr_start, 
                const char *saddr_end, 
                const char *sname_start, 
                const char *sname_end)
{
    assert(idx < KERNEL_SYMBOLS_IDX_SIZE / sizeof(*__ksymbols), "Too many symbols %d", idx);
    assert(saddr_end - saddr_start > 0, "Invalid symbol address range [%p, %p)", 
           saddr_start, saddr_end);
    assert(sname_start >= 0 && sname_end > sname_start, "Invalid symbol name range [%p, %p)", 
           sname_start, sname_end);
    assert(saddr_end - saddr_start < 32, "Symbol address too long [%p, %p)", 
           saddr_start, saddr_end);

    char saddr_str[32] = { 0 };
    strncpy(saddr_str, saddr_start, saddr_end - saddr_start);
    __ksymbols[idx].addr = (void *)strtoul(saddr_str, NULL, 16);
    __ksymbols[idx].size = sname_end - sname_start;
    __ksymbols[idx].name = sname_start;
}

void
ksymbols_init(void)
{
    __ksymbol_count = 0;

    int saddr_start = 0;
    int saddr_end = -1;
    int sname_start = -1;
    int sname_end = -1;
    for (const char *p = (const char *)KERNEL_SYMBOLS_START; 
         p < (const char *)KERNEL_SYMBOLS_END; p++) {
        if (*p == '\n' || *p == '\0') {
            sname_end = p - (const char *)KERNEL_SYMBOLS_START;
            // printf("%d %d %d %d\n", saddr_start, saddr_end, sname_start, sname_end);
            if (saddr_end == -1 || sname_start == -1 || sname_end == -1) {
                // Invalid symbol line, skip
                saddr_start = p - (const char *)KERNEL_SYMBOLS_START + 1;
            } else {
                __ksymbol_parse(__ksymbol_count,
                    (const char *)KERNEL_SYMBOLS_START + saddr_start,
                    (const char *)KERNEL_SYMBOLS_START + saddr_end,
                    (const char *)KERNEL_SYMBOLS_START + sname_start,
                    (const char *)KERNEL_SYMBOLS_START + sname_end);
                __ksymbol_count++;
                saddr_start = sname_end + 1;
            }
            saddr_end = -1;
            sname_start = -1;
            sname_end = -1;
            
            if (*p == '\0') {
                // Reached the end of the symbols
                break;
            }
        } else if (*p == ' ') {
            saddr_end = p - (char *)KERNEL_SYMBOLS_START;
            sname_start = saddr_end + 1;
        }
    }

    // Sort symbols by address using bubble sort
    for (int i = 0; i < __ksymbol_count - 1; i++) {
        for (int j = 0; j < __ksymbol_count - 1 - i; j++) {
            if (__ksymbols[j].addr > __ksymbols[j + 1].addr) {
                // Swap symbols
                __ksymbols_t temp = __ksymbols[j];
                __ksymbols[j] = __ksymbols[j + 1];
                __ksymbols[j + 1] = temp;
            }
        }
    }

    printf("Kernel symbols initialized: %d symbols\n", __ksymbol_count);
}

int
bt_search(uint64 addr, char *buf, size_t buflen, void **return_addr)
{
    int found_idx = -1;
    if (__ksymbol_count <= 0) {
        return -1;
    }

    if (__ksymbols[0].addr > (void *)addr) {
        return -1;
    }

    if (__ksymbols[__ksymbol_count - 1].addr <= (void *)addr) {
        found_idx = __ksymbol_count - 1;
        goto found;
    }

    int start = 0;
    int end = __ksymbol_count - 1;
    while (start < end) {
        int mid = (start + end + 1) / 2;
        if (__ksymbols[mid].addr <= (void *)addr) {
            start = mid;
        } else {
            end = mid - 1;
        }
    }
    found_idx = start;
found:
    if (buflen > __ksymbols[found_idx].size) {
        buflen = __ksymbols[found_idx].size + 1;
    }
    memmove(buf, __ksymbols[found_idx].name, buflen - 1);
    buf[buflen - 1] = '\0';
    if (return_addr) {
        *return_addr = __ksymbols[found_idx].addr;
    }
    return (void *)addr - __ksymbols[found_idx].addr;
}

#define BT_FRAME_TOP(__fp)          ((__fp) ? *(uint64 *)((uint64)(__fp) - 16) : 0)
#define BT_RETURN_ADDRESS(__fp)     ((__fp) ? *(uint64 *)((uint64)(__fp) - 8) : 0)
#define BT_IS_TOP_FRAME(__fp)       (!(uint64)(__fp) && (uint64)(__fp) == PGROUNDDOWN((uint64)(__fp)))

void
print_backtrace(uint64 context, uint64 stack_start, uint64 stack_end)
{
    printf("backtrace:\n");
    uint64 last_fp = context;
    for (uint64 fp = BT_FRAME_TOP(context), depth = 0;
         !BT_IS_TOP_FRAME(fp) && depth < BACKTRACE_MAX_DEPTH;
         last_fp = fp, fp = BT_FRAME_TOP(fp), depth++) {

        if (fp < stack_start || fp >= stack_end) {
            printf("* unknown frame: %p\n", (void *)fp);
            break;
        } else if (fp == 0) {
            printf("top frame\n");
            break;
        }

        char buf[64] = { 0 };
        void *return_addr = NULL;
        uint64 return_addr_val = BT_RETURN_ADDRESS(last_fp);
        if (return_addr_val == 0) {
            printf("top frame\n");
            break;
        }
        int offset = bt_search(return_addr_val, buf, sizeof(buf), &return_addr);
        if (offset < 0) {
            printf("* unknown(%p)\n", (void *)return_addr_val);
        } else {
            printf("* %p %s(%p + %d)\n", (void *)return_addr_val, buf, return_addr, offset);
        }
    }
}
