/*
 * sysfs/inode.c - Read-only sysfs inode operations.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "errno.h"
#include "bits.h"
#include "vfs/stat.h"
#include "vfs/fs.h"
#include "vfs/file.h"
#include "../vfs_private.h"
#include <mm/vm.h>
#include <dev/pci.h>
#include "smp/percpu.h"
#include "sysfs_private.h"

int snprintf(char *buf, size_t size, const char *fmt, ...);

#define SYSFS_CHILDREN_START 3

struct sysfs_entry {
    const char *name;
    uint64 ino;
};

static const struct sysfs_entry root_entries[] = {
    {"dev", SYSFS_INO_DEV},
    {"bus", SYSFS_INO_BUS},
    {"class", SYSFS_INO_CLASS},
    {"devices", SYSFS_INO_DEVICES},
};

static const struct sysfs_entry dev_entries[] = {
    {"char", SYSFS_INO_DEV_CHAR},
};

static const struct sysfs_entry char_entries[] = {
    {"226:0", SYSFS_INO_DRM_PRIMARY},
    {"226:128", SYSFS_INO_DRM_RENDER},
};

static const struct sysfs_entry bus_entries[] = {
    {"pci", SYSFS_INO_BUS_PCI},
};

static const struct sysfs_entry bus_pci_entries[] = {
    {"devices", SYSFS_INO_BUS_PCI_DEVICES},
    {"drivers", SYSFS_INO_BUS_PCI_DRIVERS},
};

static const struct sysfs_entry bus_pci_drivers_entries[] = {
    {"virtio-pci", SYSFS_INO_BUS_PCI_DRIVER_VIRTIO},
};

static const struct sysfs_entry class_entries[] = {
    {"drm", SYSFS_INO_CLASS_DRM},
};

static const struct sysfs_entry class_drm_entries[] = {
    {"card0", SYSFS_INO_CLASS_DRM_CARD0},
    {"renderD128", SYSFS_INO_CLASS_DRM_RENDER},
};

static const struct sysfs_entry device_entries[] = {
    {"vendor", 0},
    {"device", 0},
    {"subsystem_vendor", 0},
    {"subsystem_device", 0},
    {"revision", 0},
    {"uevent", 0},
    {"subsystem", 0},
    {"modalias", 0},
    {"class", 0},
    {"driver", 0},
    {"boot_vga", 0},
    {"drm", 0},
};

static const struct sysfs_entry class_node_entries[] = {
    {"dev", 0},
    {"device", 0},
    {"subsystem", 0},
    {"uevent", 0},
};

static const struct sysfs_entry devices_entries[] = {
    {"pci0000:00", SYSFS_INO_DEVICES_PCI_ROOT},
    {"system", SYSFS_INO_DEVICES_SYSTEM},
};

static const struct sysfs_entry system_entries[] = {
    {"cpu", SYSFS_INO_DEVICES_SYSTEM_CPU},
};

static const struct sysfs_entry system_cpu_entries[] = {
    {"online", SYSFS_INO_CPU_ONLINE},
    {"present", SYSFS_INO_CPU_PRESENT},
    {"possible", SYSFS_INO_CPU_POSSIBLE},
    {"kernel_max", SYSFS_INO_CPU_KERNEL_MAX},
};

static const struct sysfs_entry drm_device_entries[] = {
    {"card0", SYSFS_INO_CLASS_DRM_CARD0},
    {"renderD128", SYSFS_INO_CLASS_DRM_RENDER},
};

static int sysfs_lookup_static(const struct sysfs_entry *entries, int nentries,
                               const char *name, size_t name_len, uint64 *ino)
{
    for (int i = 0; i < nentries; i++) {
        size_t len = strlen(entries[i].name);
        if (len == name_len && memcmp(entries[i].name, name, len) == 0) {
            *ino = entries[i].ino;
            return 0;
        }
    }
    return -ENOENT;
}

static void sysfs_gpu_pci_bdf(char *buf, size_t size)
{
    struct virtio_pci_discovery *vd = pci_get_virtio_gpu(0);

    if (vd != NULL && vd->found) {
        snprintf(buf, size, "0000:%02x:%02x.%u", vd->bus, vd->dev,
                 vd->func & 0x7);
        return;
    }
    snprintf(buf, size, "0000:00:04.0");
}

static uint32 sysfs_gpu_pci_class_code(void)
{
    struct virtio_pci_discovery *vd = pci_get_virtio_gpu(0);

    if (vd != NULL && vd->found && vd->class_code != 0)
        return vd->class_code & 0xffffffU;
    return 0x030000U;
}

static void sysfs_gpu_pci_path(char *buf, size_t size)
{
    char bdf[16];

    sysfs_gpu_pci_bdf(bdf, sizeof(bdf));
    snprintf(buf, size, "/sys/devices/pci0000:00/%s", bdf);
}

static int sysfs_lookup_gpu_pci_bdf(const char *name, size_t name_len,
                                    uint64 ino, uint64 *out_ino)
{
    char bdf[16];
    size_t len;

    sysfs_gpu_pci_bdf(bdf, sizeof(bdf));
    len = strlen(bdf);
    if (len == name_len && memcmp(name, bdf, len) == 0) {
        *out_ino = ino;
        return 0;
    }
    return -ENOENT;
}

static int sysfs_parse_cpu_name(const char *name, size_t name_len)
{
    int cpu = 0;

    if (name_len < 4 || memcmp(name, "cpu", 3) != 0)
        return -1;
    for (size_t i = 3; i < name_len; i++) {
        if (name[i] < '0' || name[i] > '9')
            return -1;
        cpu = cpu * 10 + (name[i] - '0');
        if (cpu >= cpu_possible_count())
            return -1;
    }
    return cpu;
}

static int sysfs_cpu_from_ino(uint64 ino, uint64 *kind)
{
    uint64 rel;
    int cpu;

    if (ino < SYSFS_INO_CPU_BASE)
        return -1;
    rel = ino - SYSFS_INO_CPU_BASE;
    cpu = (int)(rel / SYSFS_INO_CPU_STRIDE);
    if (cpu < 0 || cpu >= cpu_possible_count())
        return -1;
    if (kind != NULL)
        *kind = rel % SYSFS_INO_CPU_STRIDE;
    return cpu;
}

static uint64 device_attr_ino(enum sysfs_device_kind kind, int child_idx)
{
    uint64 base = kind == SYSFS_DEV_DRM_PRIMARY ?
        SYSFS_INO_PRIMARY_ATTR_BASE : SYSFS_INO_RENDER_ATTR_BASE;

    if (child_idx < 0 || child_idx >= NELEM(device_entries))
        return 0;
    return base + child_idx;
}

static uint64 class_attr_ino(enum sysfs_device_kind kind, int child_idx)
{
    uint64 base = kind == SYSFS_DEV_DRM_PRIMARY ?
        SYSFS_INO_CLASS_CARD0_ATTR_BASE : SYSFS_INO_CLASS_RENDER_ATTR_BASE;

    if (child_idx < 0 || child_idx >= NELEM(class_node_entries))
        return 0;
    return base + child_idx;
}

static uint64 device_drm_ino(enum sysfs_device_kind kind)
{
    return kind == SYSFS_DEV_DRM_RENDER ?
        SYSFS_INO_DRM_RENDER_DEVICE_DRM : SYSFS_INO_DRM_PRIMARY_DEVICE_DRM;
}

static enum sysfs_device_kind sysfs_device_kind_for_ino(uint64 ino)
{
    switch (ino) {
    case SYSFS_INO_DRM_RENDER:
    case SYSFS_INO_DRM_RENDER_DEVICE:
    case SYSFS_INO_DRM_RENDER_DEVICE_DRM:
    case SYSFS_INO_CLASS_DRM_RENDER:
        return SYSFS_DEV_DRM_RENDER;
    default:
        return SYSFS_DEV_DRM_PRIMARY;
    }
}

static int sysfs_lookup_device_entry(struct sysfs_inode *si,
                                     const char *name, size_t name_len,
                                     uint64 *ino)
{
    for (int i = 0; i < NELEM(device_entries); i++) {
        size_t len = strlen(device_entries[i].name);
        if (len != name_len || memcmp(device_entries[i].name, name, len) != 0)
            continue;

        if (len == 3 && memcmp(device_entries[i].name, "drm", 3) == 0) {
            *ino = si->vfs_inode.ino == SYSFS_INO_PCI_DEVICE ?
                SYSFS_INO_PCI_DRM : device_drm_ino(si->dev_kind);
        } else {
            *ino = device_attr_ino(sysfs_device_kind_for_ino(si->vfs_inode.ino),
                                   i);
        }
        return *ino == 0 ? -ENOENT : 0;
    }
    return -ENOENT;
}

static int sysfs_emit(struct vfs_dir_iter *iter, struct vfs_dentry *ret,
                      const char *name, uint64 ino)
{
    vfs_release_dentry(ret);
    ret->name = strdup(name);
    if (ret->name == NULL)
        return -ENOMEM;
    ret->name_len = (uint16)strlen(name);
    ret->ino = ino;
    ret->cookies = iter->index;
    return 0;
}

static int sysfs_emit_static(struct vfs_dir_iter *iter, struct vfs_dentry *ret,
                             const struct sysfs_entry *entries, int nentries,
                             int child_idx)
{
    if (child_idx < 0 || child_idx >= nentries) {
        vfs_release_dentry(ret);
        ret->name = NULL;
        return 0;
    }
    return sysfs_emit(iter, ret, entries[child_idx].name,
                      entries[child_idx].ino);
}

static int sysfs_lookup(struct vfs_inode *dir, struct vfs_dentry *dentry,
                        const char *name, size_t name_len)
{
    struct sysfs_inode *si = sysfs_i(dir);
    uint64 ino = 0;

    dentry->sb = dir->sb;
    dentry->parent = dir;
    dentry->name = strndup(name, name_len);
    if (dentry->name == NULL)
        return -ENOMEM;
    dentry->name_len = name_len;

    switch (dir->ino) {
    case SYSFS_INO_ROOT:
        if (sysfs_lookup_static(root_entries, NELEM(root_entries), name,
                                name_len, &ino) == 0)
            break;
        return -ENOENT;
    case SYSFS_INO_DEV:
        if (sysfs_lookup_static(dev_entries, NELEM(dev_entries), name,
                                name_len, &ino) == 0)
            break;
        return -ENOENT;
    case SYSFS_INO_DEV_CHAR:
        if (sysfs_lookup_static(char_entries, NELEM(char_entries), name,
                                name_len, &ino) == 0)
            break;
        return -ENOENT;
    case SYSFS_INO_DRM_PRIMARY:
        if (name_len == 6 && memcmp(name, "device", 6) == 0) {
            ino = SYSFS_INO_DRM_PRIMARY_DEVICE;
            break;
        }
        return -ENOENT;
    case SYSFS_INO_DRM_RENDER:
        if (name_len == 6 && memcmp(name, "device", 6) == 0) {
            ino = SYSFS_INO_DRM_RENDER_DEVICE;
            break;
        }
        return -ENOENT;
    case SYSFS_INO_DRM_PRIMARY_DEVICE:
    case SYSFS_INO_DRM_RENDER_DEVICE:
    case SYSFS_INO_PCI_DEVICE:
        if (sysfs_lookup_device_entry(si, name, name_len, &ino) != 0)
            return -ENOENT;
        break;
    case SYSFS_INO_BUS:
        if (sysfs_lookup_static(bus_entries, NELEM(bus_entries), name,
                                name_len, &ino) == 0)
            break;
        return -ENOENT;
    case SYSFS_INO_BUS_PCI:
        if (sysfs_lookup_static(bus_pci_entries, NELEM(bus_pci_entries),
                                name, name_len, &ino) == 0)
            break;
        return -ENOENT;
    case SYSFS_INO_BUS_PCI_DEVICES:
        if (sysfs_lookup_gpu_pci_bdf(name, name_len,
                                     SYSFS_INO_BUS_PCI_DEVICE_LINK,
                                     &ino) == 0)
            break;
        return -ENOENT;
    case SYSFS_INO_BUS_PCI_DRIVERS:
        if (sysfs_lookup_static(bus_pci_drivers_entries,
                                NELEM(bus_pci_drivers_entries), name,
                                name_len, &ino) == 0)
            break;
        return -ENOENT;
    case SYSFS_INO_BUS_PCI_DRIVER_VIRTIO:
        if (sysfs_lookup_gpu_pci_bdf(name, name_len,
                                     SYSFS_INO_BUS_PCI_DRIVER_DEVICE_LINK,
                                     &ino) == 0)
            break;
        return -ENOENT;
    case SYSFS_INO_CLASS:
        if (sysfs_lookup_static(class_entries, NELEM(class_entries), name,
                                name_len, &ino) == 0)
            break;
        return -ENOENT;
    case SYSFS_INO_CLASS_DRM:
        if (sysfs_lookup_static(class_drm_entries, NELEM(class_drm_entries),
                                name, name_len, &ino) == 0)
            break;
        return -ENOENT;
    case SYSFS_INO_CLASS_DRM_CARD0:
    case SYSFS_INO_CLASS_DRM_RENDER:
        for (int i = 0; i < NELEM(class_node_entries); i++) {
            if (strlen(class_node_entries[i].name) == name_len &&
                memcmp(class_node_entries[i].name, name, name_len) == 0) {
                ino = class_attr_ino(si->dev_kind, i);
                break;
            }
        }
        if (ino == 0)
            return -ENOENT;
        break;
    case SYSFS_INO_DEVICES:
        if (sysfs_lookup_static(devices_entries, NELEM(devices_entries), name,
                                name_len, &ino) == 0)
            break;
        return -ENOENT;
    case SYSFS_INO_DEVICES_PCI_ROOT:
        if (sysfs_lookup_gpu_pci_bdf(name, name_len, SYSFS_INO_PCI_DEVICE,
                                     &ino) == 0)
            break;
        return -ENOENT;
    case SYSFS_INO_DEVICES_SYSTEM:
        if (sysfs_lookup_static(system_entries, NELEM(system_entries), name,
                                name_len, &ino) == 0)
            break;
        return -ENOENT;
    case SYSFS_INO_DEVICES_SYSTEM_CPU:
        if (sysfs_lookup_static(system_cpu_entries,
                                NELEM(system_cpu_entries), name, name_len,
                                &ino) == 0)
            break;
        {
            int cpu = sysfs_parse_cpu_name(name, name_len);
            if (cpu >= 0) {
                ino = SYSFS_CPU_DIR_INO(cpu);
                break;
            }
        }
        return -ENOENT;
    default:
    {
        uint64 kind;
        int cpu = sysfs_cpu_from_ino(dir->ino, &kind);
        if (cpu >= 0 && kind == 0 &&
            name_len == 7 && memcmp(name, "cpufreq", 7) == 0) {
            ino = SYSFS_CPU_CPUFREQ_INO(cpu);
            break;
        }
        if (cpu >= 0 && kind == 1 &&
            name_len == 16 && memcmp(name, "cpuinfo_max_freq", 16) == 0) {
            ino = SYSFS_CPU_CPUFREQ_MAX_INO(cpu);
            break;
        }
        return -ENOENT;
    }
    case SYSFS_INO_PCI_DRM:
    case SYSFS_INO_DRM_PRIMARY_DEVICE_DRM:
    case SYSFS_INO_DRM_RENDER_DEVICE_DRM:
        if (sysfs_lookup_static(drm_device_entries,
                                NELEM(drm_device_entries), name, name_len,
                                &ino) == 0)
            break;
        return -ENOENT;
    }

    dentry->ino = ino;
    return 0;
}

static int sysfs_parent_ino(uint64 ino, enum sysfs_device_kind kind)
{
    if (ino == SYSFS_INO_DEV)
        return SYSFS_INO_ROOT;
    if (ino == SYSFS_INO_DEV_CHAR)
        return SYSFS_INO_DEV;
    if (ino == SYSFS_INO_DRM_PRIMARY || ino == SYSFS_INO_DRM_RENDER)
        return SYSFS_INO_DEV_CHAR;
    if (ino == SYSFS_INO_DRM_PRIMARY_DEVICE)
        return SYSFS_INO_DRM_PRIMARY;
    if (ino == SYSFS_INO_DRM_RENDER_DEVICE)
        return SYSFS_INO_DRM_RENDER;
    if (ino == SYSFS_INO_BUS)
        return SYSFS_INO_ROOT;
    if (ino == SYSFS_INO_BUS_PCI)
        return SYSFS_INO_BUS;
    if (ino == SYSFS_INO_BUS_PCI_DEVICES ||
        ino == SYSFS_INO_BUS_PCI_DRIVERS)
        return SYSFS_INO_BUS_PCI;
    if (ino == SYSFS_INO_BUS_PCI_DRIVER_VIRTIO)
        return SYSFS_INO_BUS_PCI_DRIVERS;
    if (ino == SYSFS_INO_CLASS)
        return SYSFS_INO_ROOT;
    if (ino == SYSFS_INO_CLASS_DRM)
        return SYSFS_INO_CLASS;
    if (ino == SYSFS_INO_CLASS_DRM_CARD0 ||
        ino == SYSFS_INO_CLASS_DRM_RENDER)
        return SYSFS_INO_CLASS_DRM;
    if (ino == SYSFS_INO_DEVICES)
        return SYSFS_INO_ROOT;
    if (ino == SYSFS_INO_DEVICES_PCI_ROOT)
        return SYSFS_INO_DEVICES;
    if (ino == SYSFS_INO_DEVICES_SYSTEM)
        return SYSFS_INO_DEVICES;
    if (ino == SYSFS_INO_DEVICES_SYSTEM_CPU)
        return SYSFS_INO_DEVICES_SYSTEM;
    if (ino == SYSFS_INO_CPU_ONLINE ||
        ino == SYSFS_INO_CPU_PRESENT ||
        ino == SYSFS_INO_CPU_POSSIBLE ||
        ino == SYSFS_INO_CPU_KERNEL_MAX)
        return SYSFS_INO_DEVICES_SYSTEM_CPU;
    {
        uint64 cpu_kind;
        int cpu = sysfs_cpu_from_ino(ino, &cpu_kind);
        if (cpu >= 0) {
        if (cpu_kind == 0)
            return SYSFS_INO_DEVICES_SYSTEM_CPU;
        if (cpu_kind == 1)
            return SYSFS_CPU_DIR_INO(cpu);
        return SYSFS_CPU_CPUFREQ_INO(cpu);
        }
    }
    if (ino == SYSFS_INO_PCI_DEVICE)
        return SYSFS_INO_DEVICES_PCI_ROOT;
    if (ino == SYSFS_INO_PCI_DRM)
        return SYSFS_INO_PCI_DEVICE;
    if (ino == SYSFS_INO_DRM_PRIMARY_DEVICE_DRM)
        return SYSFS_INO_DRM_PRIMARY_DEVICE;
    if (ino == SYSFS_INO_DRM_RENDER_DEVICE_DRM)
        return SYSFS_INO_DRM_RENDER_DEVICE;
    (void)kind;
    return SYSFS_INO_ROOT;
}

static int sysfs_dir_iter(struct vfs_inode *dir, struct vfs_dir_iter *iter,
                          struct vfs_dentry *ret)
{
    struct sysfs_inode *si = sysfs_i(dir);
    int child_idx;

    if (iter->index == 1) {
        return sysfs_emit(iter, ret, "..",
                          sysfs_parent_ino(dir->ino, si->dev_kind));
    }

    child_idx = (int)iter->index - SYSFS_CHILDREN_START;
    switch (dir->ino) {
    case SYSFS_INO_ROOT:
        return sysfs_emit_static(iter, ret, root_entries,
                                 NELEM(root_entries), child_idx);
    case SYSFS_INO_DEV:
        return sysfs_emit_static(iter, ret, dev_entries,
                                 NELEM(dev_entries), child_idx);
    case SYSFS_INO_DEV_CHAR:
        return sysfs_emit_static(iter, ret, char_entries,
                                 NELEM(char_entries), child_idx);
    case SYSFS_INO_DRM_PRIMARY:
        return child_idx == 0 ? sysfs_emit(iter, ret, "device",
                                           SYSFS_INO_DRM_PRIMARY_DEVICE) :
                                sysfs_emit_static(iter, ret, NULL, 0, 0);
    case SYSFS_INO_DRM_RENDER:
        return child_idx == 0 ? sysfs_emit(iter, ret, "device",
                                           SYSFS_INO_DRM_RENDER_DEVICE) :
                                sysfs_emit_static(iter, ret, NULL, 0, 0);
    case SYSFS_INO_DRM_PRIMARY_DEVICE:
    case SYSFS_INO_DRM_RENDER_DEVICE:
    case SYSFS_INO_PCI_DEVICE:
        if (child_idx < 0 || child_idx >= NELEM(device_entries)) {
            vfs_release_dentry(ret);
            ret->name = NULL;
            return 0;
        }
        if (strlen(device_entries[child_idx].name) == 3 &&
            memcmp(device_entries[child_idx].name, "drm", 3) == 0) {
            uint64 drm_ino = dir->ino == SYSFS_INO_PCI_DEVICE ?
                SYSFS_INO_PCI_DRM : device_drm_ino(si->dev_kind);
            return sysfs_emit(iter, ret, device_entries[child_idx].name,
                              drm_ino);
        }
        return sysfs_emit(iter, ret, device_entries[child_idx].name,
                          device_attr_ino(sysfs_device_kind_for_ino(dir->ino),
                                          child_idx));
    case SYSFS_INO_BUS:
        return sysfs_emit_static(iter, ret, bus_entries,
                                 NELEM(bus_entries), child_idx);
    case SYSFS_INO_BUS_PCI:
        return sysfs_emit_static(iter, ret, bus_pci_entries,
                                 NELEM(bus_pci_entries), child_idx);
    case SYSFS_INO_BUS_PCI_DEVICES:
        if (child_idx == 0) {
            char bdf[16];
            sysfs_gpu_pci_bdf(bdf, sizeof(bdf));
            return sysfs_emit(iter, ret, bdf, SYSFS_INO_BUS_PCI_DEVICE_LINK);
        }
        return sysfs_emit_static(iter, ret, NULL, 0, 0);
    case SYSFS_INO_BUS_PCI_DRIVERS:
        return sysfs_emit_static(iter, ret, bus_pci_drivers_entries,
                                 NELEM(bus_pci_drivers_entries), child_idx);
    case SYSFS_INO_BUS_PCI_DRIVER_VIRTIO:
        if (child_idx == 0) {
            char bdf[16];
            sysfs_gpu_pci_bdf(bdf, sizeof(bdf));
            return sysfs_emit(iter, ret, bdf,
                              SYSFS_INO_BUS_PCI_DRIVER_DEVICE_LINK);
        }
        return sysfs_emit_static(iter, ret, NULL, 0, 0);
    case SYSFS_INO_CLASS:
        return sysfs_emit_static(iter, ret, class_entries,
                                 NELEM(class_entries), child_idx);
    case SYSFS_INO_CLASS_DRM:
        return sysfs_emit_static(iter, ret, class_drm_entries,
                                 NELEM(class_drm_entries), child_idx);
    case SYSFS_INO_CLASS_DRM_CARD0:
    case SYSFS_INO_CLASS_DRM_RENDER:
        if (child_idx < 0 || child_idx >= NELEM(class_node_entries)) {
            vfs_release_dentry(ret);
            ret->name = NULL;
            return 0;
        }
        return sysfs_emit(iter, ret, class_node_entries[child_idx].name,
                          class_attr_ino(si->dev_kind, child_idx));
    case SYSFS_INO_DEVICES:
        return sysfs_emit_static(iter, ret, devices_entries,
                                 NELEM(devices_entries), child_idx);
    case SYSFS_INO_DEVICES_PCI_ROOT:
        if (child_idx == 0) {
            char bdf[16];
            sysfs_gpu_pci_bdf(bdf, sizeof(bdf));
            return sysfs_emit(iter, ret, bdf, SYSFS_INO_PCI_DEVICE);
        }
        return sysfs_emit_static(iter, ret, NULL, 0, 0);
    case SYSFS_INO_DEVICES_SYSTEM:
        return sysfs_emit_static(iter, ret, system_entries,
                                 NELEM(system_entries), child_idx);
    case SYSFS_INO_DEVICES_SYSTEM_CPU:
        if (child_idx < NELEM(system_cpu_entries)) {
            return sysfs_emit_static(iter, ret, system_cpu_entries,
                                     NELEM(system_cpu_entries), child_idx);
        } else {
            int cpu = child_idx - NELEM(system_cpu_entries);
            char name[16];
            if (cpu < 0 || cpu >= cpu_possible_count()) {
                vfs_release_dentry(ret);
                ret->name = NULL;
                return 0;
            }
            snprintf(name, sizeof(name), "cpu%d", cpu);
            return sysfs_emit(iter, ret, name, SYSFS_CPU_DIR_INO(cpu));
        }
    default:
    {
        uint64 kind;
        int cpu = sysfs_cpu_from_ino(dir->ino, &kind);
        if (cpu >= 0 && kind == 0)
            return child_idx == 0 ?
                sysfs_emit(iter, ret, "cpufreq",
                           SYSFS_CPU_CPUFREQ_INO(cpu)) :
                sysfs_emit_static(iter, ret, NULL, 0, 0);
        if (cpu >= 0 && kind == 1)
            return child_idx == 0 ?
                sysfs_emit(iter, ret, "cpuinfo_max_freq",
                           SYSFS_CPU_CPUFREQ_MAX_INO(cpu)) :
                sysfs_emit_static(iter, ret, NULL, 0, 0);
        return -ENOTDIR;
    }
    case SYSFS_INO_PCI_DRM:
    case SYSFS_INO_DRM_PRIMARY_DEVICE_DRM:
    case SYSFS_INO_DRM_RENDER_DEVICE_DRM:
        return sysfs_emit_static(iter, ret, drm_device_entries,
                                 NELEM(drm_device_entries), child_idx);
    }
}

static const char *sysfs_dev_name(enum sysfs_device_kind kind)
{
    return kind == SYSFS_DEV_DRM_RENDER ? "renderD128" : "card0";
}

static const char *sysfs_dev_number(enum sysfs_device_kind kind)
{
    return kind == SYSFS_DEV_DRM_RENDER ? "226:128" : "226:0";
}

static ssize_t sysfs_readlink(struct vfs_inode *inode, char *buf,
                              size_t buflen)
{
    struct sysfs_inode *si = sysfs_i(inode);
    const char *target = NULL;
    char local[96];

    if (si->attr == SYSFS_ATTR_SUBSYSTEM) {
        target = "/sys/bus/pci";
    } else if (si->attr == SYSFS_ATTR_DEVICE_LINK) {
        snprintf(local, sizeof(local), "/sys/dev/char/%s/device",
                 sysfs_dev_number(si->dev_kind));
        target = local;
    } else if (si->attr == SYSFS_ATTR_DRIVER_LINK) {
        target = "/sys/bus/pci/drivers/virtio-pci";
    } else if (inode->ino == SYSFS_INO_BUS_PCI_DEVICE_LINK ||
               inode->ino == SYSFS_INO_BUS_PCI_DRIVER_DEVICE_LINK) {
        sysfs_gpu_pci_path(local, sizeof(local));
        target = local;
    } else {
        return -EINVAL;
    }

    size_t len = strlen(target);
    if (len + 1 > buflen)
        return -ENAMETOOLONG;
    memmove(buf, target, len + 1);
    return (ssize_t)len;
}

static int sysfs_getattr(struct vfs_inode *inode, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_ino = inode->ino;
    st->st_mode = inode->mode;
    st->st_nlink = inode->n_links;
    st->st_size = inode->size;
    st->st_blksize = 4096;
    return 0;
}

static char *sysfs_gen_file(struct sysfs_inode *si)
{
    char *buf = kvmalloc(512);
    const char *node = sysfs_dev_name(si->dev_kind);
    const char *devnum = sysfs_dev_number(si->dev_kind);
    int n = 0;
    int last_cpu = cpu_possible_count() - 1;

    if (buf == NULL)
        return ERR_PTR(-ENOMEM);
    if (last_cpu < 0)
        last_cpu = 0;

    switch (si->attr) {
    case SYSFS_ATTR_VENDOR:
        n = snprintf(buf, 512, "0x1af4\n");
        break;
    case SYSFS_ATTR_DEVICE:
        n = snprintf(buf, 512, "0x1050\n");
        break;
    case SYSFS_ATTR_SUBSYSTEM_VENDOR:
        n = snprintf(buf, 512, "0x1af4\n");
        break;
    case SYSFS_ATTR_SUBSYSTEM_DEVICE:
        n = snprintf(buf, 512, "0x1100\n");
        break;
    case SYSFS_ATTR_REVISION:
        n = snprintf(buf, 512, "0x01\n");
        break;
    case SYSFS_ATTR_DEV:
        n = snprintf(buf, 512, "%s\n", devnum);
        break;
    case SYSFS_ATTR_UEVENT:
    {
        char bdf[16];
        uint32 class_code;

        sysfs_gpu_pci_bdf(bdf, sizeof(bdf));
        class_code = sysfs_gpu_pci_class_code();
        n = snprintf(buf, 512,
                     "DRIVER=virtio-pci\n"
                     "PCI_CLASS=%lX\n"
                     "PCI_ID=1AF4:1050\n"
                     "PCI_SUBSYS_ID=1AF4:1100\n"
                     "PCI_SLOT_NAME=%s\n"
                     "MODALIAS=pci:v00001AF4d00001050sv00001AF4sd00001100bc%02lXsc%02lXi%02lX\n"
                     "DEVNAME=dri/%s\n"
                     "MAJOR=226\n"
                     "MINOR=%s\n",
                     (uint64)class_code,
                     bdf,
                     (uint64)((class_code >> 16) & 0xffU),
                     (uint64)((class_code >> 8) & 0xffU),
                     (uint64)(class_code & 0xffU),
                     node,
                     si->dev_kind == SYSFS_DEV_DRM_RENDER ? "128" : "0");
        break;
    }
    case SYSFS_ATTR_MODALIAS:
    {
        uint32 class_code = sysfs_gpu_pci_class_code();

        n = snprintf(buf, 512,
                     "pci:v00001AF4d00001050sv00001AF4sd00001100bc%02lXsc%02lXi%02lX\n",
                     (uint64)((class_code >> 16) & 0xffU),
                     (uint64)((class_code >> 8) & 0xffU),
                     (uint64)(class_code & 0xffU));
        break;
    }
    case SYSFS_ATTR_CLASS:
        n = snprintf(buf, 512, "0x%06lX\n",
                     (uint64)sysfs_gpu_pci_class_code());
        break;
    case SYSFS_ATTR_BOOT_VGA:
        n = snprintf(buf, 512, "1\n");
        break;
    case SYSFS_ATTR_CPU_ONLINE:
    case SYSFS_ATTR_CPU_PRESENT:
    case SYSFS_ATTR_CPU_POSSIBLE:
        n = snprintf(buf, 512, "0-%d\n", last_cpu);
        break;
    case SYSFS_ATTR_CPU_KERNEL_MAX:
        n = snprintf(buf, 512, "%d\n", last_cpu);
        break;
    case SYSFS_ATTR_CPUINFO_MAX_FREQ:
        n = snprintf(buf, 512, "2400000\n");
        break;
    default:
        kvfree(buf);
        return ERR_PTR(-EINVAL);
    }

    if (n < 0 || n >= 512) {
        kvfree(buf);
        return ERR_PTR(-EOVERFLOW);
    }
    return buf;
}

static int sysfs_open(struct vfs_inode *inode, struct vfs_file *file,
                      int f_flags)
{
    struct sysfs_inode *si = sysfs_i(inode);
    char *data;

    (void)f_flags;
    if (S_ISDIR(inode->mode)) {
        file->ops = &sysfs_dir_file_ops;
        return 0;
    }
    if (S_ISLNK(inode->mode))
        return -EINVAL;

    data = sysfs_gen_file(si);
    if (IS_ERR(data))
        return PTR_ERR(data);
    file->private_data = data;
    file->ops = &sysfs_reg_file_ops;
    file->f_pos = 0;
    return 0;
}

static void sysfs_destroy_inode(struct vfs_inode *inode)
{
    (void)inode;
}

struct vfs_inode_ops sysfs_inode_ops = {
    .lookup = sysfs_lookup,
    .dir_iter = sysfs_dir_iter,
    .readlink = sysfs_readlink,
    .getattr = sysfs_getattr,
    .open = sysfs_open,
    .destroy_inode = sysfs_destroy_inode,
    .free_inode = sysfs_free_inode,
};
