/*
 * sysfs/superblock.c - Read-only sysfs superblock and inode factory.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "errno.h"
#include "vfs/stat.h"
#include "lock/rwsem.h"
#include "vfs/fs.h"
#include "../vfs_private.h"
#include "list.h"
#include "hlist.h"
#include <mm/slab.h>
#include "smp/percpu.h"
#include "printf.h"
#include "sysfs_private.h"

static slab_cache_t __sysfs_inode_cache = {0};
static slab_cache_t __sysfs_sb_cache = {0};

struct sysfs_superblock {
    struct vfs_superblock vfs_sb;
};

static int sysfs_init_caches(void)
{
    int ret;

    ret = slab_cache_init(&__sysfs_inode_cache, "sysfs_inode_cache",
                          sizeof(struct sysfs_inode),
                          SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
    if (ret != 0)
        return ret;
    return slab_cache_init(&__sysfs_sb_cache, "sysfs_sb_cache",
                           sizeof(struct sysfs_superblock),
                           SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
}

struct vfs_inode *sysfs_alloc_inode(struct vfs_superblock *sb)
{
    struct sysfs_inode *si;

    (void)sb;
    si = slab_alloc(&__sysfs_inode_cache);
    if (si == NULL)
        return ERR_PTR(-ENOMEM);
    memset(si, 0, sizeof(*si));
    si->vfs_inode.ops = &sysfs_inode_ops;
    return &si->vfs_inode;
}

void sysfs_free_inode(struct vfs_inode *inode)
{
    slab_free(sysfs_i(inode));
}

static void sysfs_fill_inode(struct sysfs_inode *si, uint64 ino,
                             enum sysfs_entry_type type,
                             enum sysfs_device_kind dev_kind,
                             enum sysfs_attr_type attr)
{
    si->vfs_inode.ino = ino;
    si->vfs_inode.ops = &sysfs_inode_ops;
    si->vfs_inode.ref_count = 1;
    si->vfs_inode.uid = 0;
    si->vfs_inode.gid = 0;
    si->type = type;
    si->dev_kind = dev_kind;
    si->attr = attr;

    if (type == SYSFS_FILE) {
        si->vfs_inode.mode = S_IFREG | 0444;
        si->vfs_inode.n_links = 1;
        si->vfs_inode.size = 128;
    } else if (type == SYSFS_SYMLINK) {
        si->vfs_inode.mode = S_IFLNK | 0777;
        si->vfs_inode.n_links = 1;
        si->vfs_inode.size = 128;
    } else {
        si->vfs_inode.mode = S_IFDIR | 0555;
        si->vfs_inode.n_links = 2;
    }
}

static int sysfs_decode_device_attr(uint64 ino, uint64 base,
                                    enum sysfs_device_kind kind,
                                    struct sysfs_inode *si)
{
    enum sysfs_attr_type attr;

    if (ino < base || ino >= base + 32)
        return -ENOENT;

    switch (ino - base) {
    case 0: attr = SYSFS_ATTR_VENDOR; break;
    case 1: attr = SYSFS_ATTR_DEVICE; break;
    case 2: attr = SYSFS_ATTR_SUBSYSTEM_VENDOR; break;
    case 3: attr = SYSFS_ATTR_SUBSYSTEM_DEVICE; break;
    case 4: attr = SYSFS_ATTR_REVISION; break;
    case 5: attr = SYSFS_ATTR_UEVENT; break;
    case 6: attr = SYSFS_ATTR_SUBSYSTEM; break;
    case 7: attr = SYSFS_ATTR_MODALIAS; break;
    case 8: attr = SYSFS_ATTR_CLASS; break;
    case 9: attr = SYSFS_ATTR_DRIVER_LINK; break;
    case 10: attr = SYSFS_ATTR_BOOT_VGA; break;
    default: return -ENOENT;
    }

    sysfs_fill_inode(si, ino,
                     (attr == SYSFS_ATTR_SUBSYSTEM ||
                      attr == SYSFS_ATTR_DRIVER_LINK) ?
                     SYSFS_SYMLINK : SYSFS_FILE,
                     kind, attr);
    return 0;
}

static int sysfs_decode_class_attr(uint64 ino, uint64 base,
                                   enum sysfs_device_kind kind,
                                   struct sysfs_inode *si)
{
    enum sysfs_attr_type attr;

    if (ino < base || ino >= base + 32)
        return -ENOENT;

    if (kind == SYSFS_DEV_SOUND_CARD) {
        switch (ino - base) {
        case 0: attr = SYSFS_ATTR_DEVICE_LINK; break;
        case 1: attr = SYSFS_ATTR_SUBSYSTEM; break;
        case 2: attr = SYSFS_ATTR_UEVENT; break;
        case 3: attr = SYSFS_ATTR_ID; break;
        default: return -ENOENT;
        }
    } else {
        switch (ino - base) {
        case 0: attr = SYSFS_ATTR_DEV; break;
        case 1: attr = SYSFS_ATTR_DEVICE_LINK; break;
        case 2: attr = SYSFS_ATTR_SUBSYSTEM; break;
        case 3: attr = SYSFS_ATTR_UEVENT; break;
        default: return -ENOENT;
        }
    }

    sysfs_fill_inode(si, ino,
                     (attr == SYSFS_ATTR_DEVICE_LINK ||
                      attr == SYSFS_ATTR_SUBSYSTEM) ? SYSFS_SYMLINK : SYSFS_FILE,
                     kind, attr);
    return 0;
}

struct vfs_inode *sysfs_get_inode(struct vfs_superblock *sb, uint64 ino)
{
    struct sysfs_inode *si;
    int ret = 0;

    if (sb == NULL)
        return ERR_PTR(-EINVAL);
    si = slab_alloc(&__sysfs_inode_cache);
    if (si == NULL)
        return ERR_PTR(-ENOMEM);
    memset(si, 0, sizeof(*si));

    switch (ino) {
    case SYSFS_INO_ROOT:
        sysfs_fill_inode(si, ino, SYSFS_ROOT, SYSFS_DEV_NONE, SYSFS_ATTR_NONE);
        si->vfs_inode.parent = &si->vfs_inode;
        break;
    case SYSFS_INO_DEV:
    case SYSFS_INO_DEV_CHAR:
    case SYSFS_INO_BUS:
    case SYSFS_INO_BUS_PCI:
    case SYSFS_INO_BUS_PCI_DEVICES:
    case SYSFS_INO_BUS_PCI_DRIVERS:
    case SYSFS_INO_BUS_PCI_DRIVER_VIRTIO:
    case SYSFS_INO_CLASS:
    case SYSFS_INO_CLASS_DRM:
    case SYSFS_INO_CLASS_SOUND:
    case SYSFS_INO_DEVICES:
    case SYSFS_INO_DEVICES_PCI_ROOT:
    case SYSFS_INO_DEVICES_SYSTEM:
    case SYSFS_INO_DEVICES_SYSTEM_CPU:
    case SYSFS_INO_PCI_DEVICE:
    case SYSFS_INO_PCI_DRM:
        sysfs_fill_inode(si, ino, SYSFS_DIR, SYSFS_DEV_NONE, SYSFS_ATTR_NONE);
        break;
    case SYSFS_INO_CPU_ONLINE:
        sysfs_fill_inode(si, ino, SYSFS_FILE, SYSFS_DEV_NONE,
                         SYSFS_ATTR_CPU_ONLINE);
        break;
    case SYSFS_INO_CPU_PRESENT:
        sysfs_fill_inode(si, ino, SYSFS_FILE, SYSFS_DEV_NONE,
                         SYSFS_ATTR_CPU_PRESENT);
        break;
    case SYSFS_INO_CPU_POSSIBLE:
        sysfs_fill_inode(si, ino, SYSFS_FILE, SYSFS_DEV_NONE,
                         SYSFS_ATTR_CPU_POSSIBLE);
        break;
    case SYSFS_INO_CPU_KERNEL_MAX:
        sysfs_fill_inode(si, ino, SYSFS_FILE, SYSFS_DEV_NONE,
                         SYSFS_ATTR_CPU_KERNEL_MAX);
        break;
    case SYSFS_INO_BUS_PCI_DEVICE_LINK:
    case SYSFS_INO_BUS_PCI_DRIVER_DEVICE_LINK:
        sysfs_fill_inode(si, ino, SYSFS_SYMLINK, SYSFS_DEV_DRM_PRIMARY,
                         SYSFS_ATTR_NONE);
        break;
    case SYSFS_INO_DRM_PRIMARY:
        sysfs_fill_inode(si, ino, SYSFS_DIR, SYSFS_DEV_DRM_PRIMARY,
                         SYSFS_ATTR_NONE);
        break;
    case SYSFS_INO_DRM_PRIMARY_DEVICE:
    case SYSFS_INO_DRM_PRIMARY_DEVICE_DRM:
        sysfs_fill_inode(si, ino, SYSFS_DIR, SYSFS_DEV_DRM_PRIMARY,
                         SYSFS_ATTR_NONE);
        break;
    case SYSFS_INO_DRM_RENDER:
        sysfs_fill_inode(si, ino, SYSFS_DIR, SYSFS_DEV_DRM_RENDER,
                         SYSFS_ATTR_NONE);
        break;
    case SYSFS_INO_DRM_RENDER_DEVICE:
    case SYSFS_INO_DRM_RENDER_DEVICE_DRM:
        sysfs_fill_inode(si, ino, SYSFS_DIR, SYSFS_DEV_DRM_RENDER,
                         SYSFS_ATTR_NONE);
        break;
    case SYSFS_INO_CLASS_DRM_CARD0:
        sysfs_fill_inode(si, ino, SYSFS_DIR, SYSFS_DEV_DRM_PRIMARY,
                         SYSFS_ATTR_NONE);
        break;
    case SYSFS_INO_CLASS_DRM_RENDER:
        sysfs_fill_inode(si, ino, SYSFS_DIR, SYSFS_DEV_DRM_RENDER,
                         SYSFS_ATTR_NONE);
        break;
    case SYSFS_INO_CLASS_SOUND_CARD0:
        sysfs_fill_inode(si, ino, SYSFS_DIR, SYSFS_DEV_SOUND_CARD,
                         SYSFS_ATTR_NONE);
        break;
    case SYSFS_INO_CLASS_SOUND_CONTROL0:
        sysfs_fill_inode(si, ino, SYSFS_DIR, SYSFS_DEV_SOUND_CONTROL,
                         SYSFS_ATTR_NONE);
        break;
    case SYSFS_INO_CLASS_SOUND_PCM0P:
        sysfs_fill_inode(si, ino, SYSFS_DIR, SYSFS_DEV_SOUND_PCM,
                         SYSFS_ATTR_NONE);
        break;
    default:
    {
        uint64 cpu_rel = ino >= SYSFS_INO_CPU_BASE ?
            ino - SYSFS_INO_CPU_BASE : (uint64)-1;
        int cpu = cpu_rel == (uint64)-1 ? -1 :
            (int)(cpu_rel / SYSFS_INO_CPU_STRIDE);
        uint64 cpu_kind = cpu_rel == (uint64)-1 ? (uint64)-1 :
            cpu_rel % SYSFS_INO_CPU_STRIDE;

        if (cpu >= 0 && cpu < cpu_possible_count()) {
            if (cpu_kind == 0 || cpu_kind == 1) {
                sysfs_fill_inode(si, ino, SYSFS_DIR, SYSFS_DEV_NONE,
                                 SYSFS_ATTR_NONE);
                break;
            }
            if (cpu_kind == 2) {
                sysfs_fill_inode(si, ino, SYSFS_FILE, SYSFS_DEV_NONE,
                                 SYSFS_ATTR_CPUINFO_MAX_FREQ);
                break;
            }
        }
    }
        ret = sysfs_decode_device_attr(ino, SYSFS_INO_PRIMARY_ATTR_BASE,
                                       SYSFS_DEV_DRM_PRIMARY, si);
        if (ret != 0)
            ret = sysfs_decode_device_attr(ino, SYSFS_INO_RENDER_ATTR_BASE,
                                           SYSFS_DEV_DRM_RENDER, si);
        if (ret != 0)
            ret = sysfs_decode_class_attr(ino, SYSFS_INO_CLASS_CARD0_ATTR_BASE,
                                          SYSFS_DEV_DRM_PRIMARY, si);
        if (ret != 0)
            ret = sysfs_decode_class_attr(ino, SYSFS_INO_CLASS_RENDER_ATTR_BASE,
                                          SYSFS_DEV_DRM_RENDER, si);
        if (ret != 0)
            ret = sysfs_decode_class_attr(ino,
                                          SYSFS_INO_CLASS_SOUND_CARD_ATTR_BASE,
                                          SYSFS_DEV_SOUND_CARD, si);
        if (ret != 0)
            ret = sysfs_decode_class_attr(ino,
                                          SYSFS_INO_CLASS_SOUND_CONTROL_ATTR_BASE,
                                          SYSFS_DEV_SOUND_CONTROL, si);
        if (ret != 0)
            ret = sysfs_decode_class_attr(ino,
                                          SYSFS_INO_CLASS_SOUND_PCM_ATTR_BASE,
                                          SYSFS_DEV_SOUND_PCM, si);
        if (ret != 0) {
            slab_free(si);
            return ERR_PTR(ret);
        }
        break;
    }

    return &si->vfs_inode;
}

static int sysfs_sync_fs(struct vfs_superblock *sb, int wait)
{
    (void)sb;
    (void)wait;
    return 0;
}

static void sysfs_unmount_begin(struct vfs_superblock *sb)
{
    (void)sb;
}

struct vfs_superblock_ops sysfs_sb_ops = {
    .alloc_inode = sysfs_alloc_inode,
    .get_inode = sysfs_get_inode,
    .sync_fs = sysfs_sync_fs,
    .unmount_begin = sysfs_unmount_begin,
};

static int sysfs_mount(struct vfs_inode *mountpoint, struct vfs_inode *device,
                       int flags, const char *data,
                       struct vfs_superblock **ret_sb)
{
    struct sysfs_superblock *ssb;
    struct sysfs_inode *rooti;
    struct vfs_superblock *sb;

    (void)flags;
    (void)data;
    if (mountpoint == NULL || ret_sb == NULL)
        return -EINVAL;
    if (device != NULL)
        return -EINVAL;

    ssb = slab_alloc(&__sysfs_sb_cache);
    if (ssb == NULL)
        return -ENOMEM;
    memset(ssb, 0, sizeof(*ssb));

    sb = &ssb->vfs_sb;
    sb->backendless = 1;
    sb->no_neg_dcache = 1; /* dynamic names — no vfs_create bump_dir_seq */
    sb->block_size = 512;
    sb->ops = &sysfs_sb_ops;

    rooti = slab_alloc(&__sysfs_inode_cache);
    if (rooti == NULL) {
        slab_free(ssb);
        return -ENOMEM;
    }
    memset(rooti, 0, sizeof(*rooti));
    sysfs_fill_inode(rooti, SYSFS_INO_ROOT, SYSFS_ROOT, SYSFS_DEV_NONE,
                     SYSFS_ATTR_NONE);
    rooti->vfs_inode.parent = &rooti->vfs_inode;
    sb->root_inode = &rooti->vfs_inode;

    *ret_sb = sb;
    return 0;
}

static void sysfs_free(struct vfs_superblock *sb)
{
    struct sysfs_superblock *ssb =
        container_of(sb, struct sysfs_superblock, vfs_sb);
    slab_free(ssb);
}

struct vfs_fs_type_ops sysfs_fs_type_ops = {
    .mount = sysfs_mount,
    .free = sysfs_free,
};

void sysfs_init(void)
{
    struct vfs_fs_type *fs_type;
    int ret;

    ret = sysfs_init_caches();
    assert(ret == 0, "sysfs_init: cache init failed, errno=%d", ret);

    fs_type = vfs_fs_type_allocate();
    assert(fs_type != NULL, "sysfs_init: vfs_fs_type_allocate failed");
    fs_type->name = "sysfs";
    fs_type->ops = &sysfs_fs_type_ops;

    vfs_mount_lock();
    ret = vfs_register_fs_type(fs_type);
    assert(ret == 0, "sysfs_init: vfs_register_fs_type failed, errno=%d", ret);
    vfs_mount_unlock();

    printf("sysfs: initialized\n");
}
