/*
 * Framebuffer/GPU implementation root.
 *
 * This unity-style root keeps the current static helper relationships intact
 * while the implementation is split by subsystem under dev/fb/.
 */

#include "fb_common.c"

#if defined(__x86_64__) || defined(__i386__)
#include "dma_fence.c"
#include "fb_internal.c"
#include "fb_scanout.c"
#include "fb_bo_shmem_dmabuf.c"
#include "fb_dxg_present.c"
#include "fb_fd_sync.c"
#include "fb_device_ioctl.c"
#include "fb_drm_core_kms.c"
#include "fb_kms_atomic.c"
#include "fb_syncobj_prime_virtgpu.c"
#include "fb_nouveau.c"
#include "fb_drm_dispatch.c"
#include "fb_init_panic.c"
#else
#include "fb_non_x86.c"
#endif
