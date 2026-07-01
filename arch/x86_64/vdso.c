#include "types.h"
#include "string.h"
#include "errno.h"
#include "elf.h"
#include "printf.h"
#include "vdso.h"
#include <mm/memlayout.h>
#include <mm/page.h>
#include <mm/page_type.h>
#include <mm/vm.h>
#include "timer/timer.h"

extern const uchar x86_vdso_image_start[];
extern const uchar x86_vdso_image_end[];

#define XV6_VDSO_MARKER0 0x317664736f367678ULL
#define XV6_VDSO_MARKER1 0xce899b8c90c98987ULL

static uint64
x86_vdso_image_size(void)
{
    return (uint64)(x86_vdso_image_end - x86_vdso_image_start);
}

static int
x86_vdso_load_size(uint64 *size_out)
{
    uint64 size = x86_vdso_image_size();
    const struct elfhdr *eh = (const struct elfhdr *)x86_vdso_image_start;
    uint64 max_end = 0;

    if (size_out == NULL || size < sizeof(*eh))
        return -EINVAL;
    if (eh->magic != ELF_MAGIC || eh->type != ET_DYN ||
        eh->phentsize != sizeof(struct proghdr) || eh->phnum == 0)
        return -ENOEXEC;
    if (eh->phoff > size ||
        (uint64)eh->phnum * sizeof(struct proghdr) > size - eh->phoff)
        return -ENOEXEC;

    for (uint i = 0; i < eh->phnum; i++) {
        const struct proghdr *ph =
            (const struct proghdr *)(x86_vdso_image_start + eh->phoff +
                                     (uint64)i * sizeof(*ph));
        uint64 end;

        if (ph->type != ELF_PROG_LOAD)
            continue;
        if (ph->off > size || ph->filesz > size - ph->off)
            return -ENOEXEC;
        if (ph->vaddr > (uint64)-1 - ph->memsz)
            return -ENOEXEC;
        end = ph->vaddr + ph->memsz;
        if (end > max_end)
            max_end = end;
    }

    if (max_end == 0 || max_end > (UVDSO_MAX_PAGES << PAGE_SHIFT))
        return -E2BIG;
    *size_out = PGROUNDUP(max_end);
    return 0;
}

static int
x86_vdso_copy_page(void *dst, uint64 page_vaddr)
{
    uint64 size = x86_vdso_image_size();
    const struct elfhdr *eh = (const struct elfhdr *)x86_vdso_image_start;

    for (uint i = 0; i < eh->phnum; i++) {
        const struct proghdr *ph =
            (const struct proghdr *)(x86_vdso_image_start + eh->phoff +
                                     (uint64)i * sizeof(*ph));
        uint64 seg_start;
        uint64 seg_end;
        uint64 copy_start;
        uint64 copy_end;
        uint64 len;

        if (ph->type != ELF_PROG_LOAD || ph->filesz == 0)
            continue;
        seg_start = ph->vaddr;
        seg_end = ph->vaddr + ph->filesz;
        copy_start = page_vaddr > seg_start ? page_vaddr : seg_start;
        copy_end = page_vaddr + PGSIZE < seg_end ? page_vaddr + PGSIZE :
                   seg_end;
        if (copy_end <= copy_start)
            continue;
        if (ph->off > size || ph->filesz > size - ph->off)
            return -ENOEXEC;
        len = copy_end - copy_start;
        memmove((char *)dst + (copy_start - page_vaddr),
                x86_vdso_image_start + ph->off + (copy_start - ph->vaddr),
                len);
    }

    return 0;
}

static int
x86_vdso_patch_timebase_page(void *page)
{
    uint64 *words = page;
    uint64 nwords = PGSIZE / sizeof(uint64);

    for (uint64 i = 0; i + 2 < nwords; i++) {
        if (words[i] == XV6_VDSO_MARKER0 &&
            words[i + 1] == XV6_VDSO_MARKER1) {
            words[i + 2] = __timebase_frequency ? __timebase_frequency :
                           10000000UL;
            return 0;
        }
    }

    return -EINVAL;
}

int
x86_vdso_map(struct vm *vm, uint64 *ehdr)
{
    uint64 image_size = x86_vdso_image_size();
    uint64 map_size;
    uint64 pages;
    int patched_timebase = 0;
    int ret;

    if (vm == NULL || ehdr == NULL)
        return -EINVAL;
    *ehdr = 0;
    if (image_size == 0 || image_size > (UVDSO_MAX_PAGES << PAGE_SHIFT))
        return -E2BIG;
    ret = x86_vdso_load_size(&map_size);
    if (ret != 0)
        return ret;

    pages = map_size / PGSIZE;

    for (uint64 i = 0; i < pages; i++) {
        void *pa = page_alloc(0, PAGE_TYPE_ANON);
        uint64 off = i << PAGE_SHIFT;

        if (pa == NULL)
            return -ENOMEM;
        memset(pa, 0, PGSIZE);
        ret = x86_vdso_copy_page(pa, off);
        if (ret != 0) {
            page_free(pa, 0);
            return ret;
        }
        if (x86_vdso_patch_timebase_page(pa) == 0)
            patched_timebase = 1;

        ret = vm_mmap_region_locked(vm, UVDSO + off, PGSIZE,
                                    PROT_READ | PROT_EXEC | VMA_FLAG_USER |
                                        VMA_FLAG_DONTDUMP,
                                    NULL, 0, pa);
        if (ret != 0) {
            page_free(pa, 0);
            return ret;
        }
    }
    if (!patched_timebase)
        return -ENOEXEC;

    *ehdr = UVDSO;
    return 0;
}
