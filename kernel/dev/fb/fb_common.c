/*
 * fb.c — Bochs VGA (BGA) framebuffer driver for x86_64 QEMU.
 *
 * Exposes /dev/fb0 as a character device.  Userspace can:
 *   - write()  pixel data (BGRA8888 format, row-major from top-left)
 *   - ioctl()  FBIOGET_VSCREENINFO / FBIOGET_FSCREENINFO for screen parameters
 *   - lseek()  is implicit via the write offset
 *
 * The Bochs Display Adapter uses VBE dispi I/O ports (0x01CE/0x01CF) for
 * mode setting and exposes the linear framebuffer through PCI BAR0.
 */

#include <types.h>
#include <param.h>
#include <riscv.h>
#include <defs.h>
#include <errno.h>
#include <string.h>
#include <dev/cdev.h>
#include <dev/drm_core.h>
#include <dev/fb.h>
#include <dev/fdt.h>
#include <dev/pci.h>
#include <timer/timer.h>
#include <trap.h>
#include <mm/page.h>
#include <mm/pgtable.h>
#include <mm/memlayout.h>
#include <mm/rmap.h>
#include <mm/vm.h>
#include <proc/sched.h>
#include <proc/thread.h>
#include <printf.h>
#include <lock/spinlock.h>
#include <cmdline.h>
#include <vfs/file.h>
#include <vfs/poll.h>
#include <vfs/vfs_types.h>
#include <kqueue.h>
#include <kqueue_types.h>
#include <dev/dma_fence.h>
#include <uabi/drm.h>

void fb_shmem_release_pages(page_t **pages, uint32 npages);

int fb_shmem_alloc_pages(uint32 npages, page_t ***pages_out)
{
    page_t **pages;

    if (npages == 0 || pages_out == NULL)
        return -EINVAL;
    *pages_out = NULL;
    pages = kvmalloc((size_t)npages * sizeof(*pages));
    if (pages == NULL)
        return -ENOMEM;
    memset(pages, 0, (size_t)npages * sizeof(*pages));

    for (uint32 i = 0; i < npages; i++) {
        pages[i] = __page_alloc(0, PAGE_TYPE_ANON);
        if (pages[i] == NULL) {
            fb_shmem_release_pages(pages, npages);
            return -ENOMEM;
        }
        memset((void *)PA2VA(__page_to_pa(pages[i])), 0, PGSIZE);
    }

    *pages_out = pages;
    return 0;
}

void fb_shmem_release_pages(page_t **pages, uint32 npages)
{
    if (pages == NULL)
        return;
    for (uint32 i = 0; i < npages; i++) {
        if (pages[i] != NULL)
            page_ref_dec((void *)__page_to_pa(pages[i]));
    }
    kvfree(pages);
}
