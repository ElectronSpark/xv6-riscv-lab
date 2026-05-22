//
// PCI device initialization.
// Supports ECAM (RISC-V) and I/O port (x86) config space access.
// Detects e1000 NIC and virtio block device.
//

#include "types.h"
#include "param.h"
#include "errno.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "defs.h"
#include "string.h"
#include "printf.h"
#include "dev/pci.h"
#include "dev/e1000_dev.h"
#include "dev/virtio.h"
#include "dev/fdt.h"
#include "dev/fb.h"

uint64 __pcie_ecam_mmio_base = (uint64)PA2VA(0x30000000L);

// Forward declarations
void e1000_init(uint32 *xregs);

#define PCI_CONFIG_BACKEND_MAX 8
#define PCI_VIRTUAL_CHILD_MAX 64
#define PCI_VIRTUAL_BUS 0xff

struct pci_config_backend {
    int used;
    const char *name;
    const struct pci_config_backend_ops *ops;
    void *ctx;
};

struct pci_virtual_child_entry {
    int used;
    int probed;
    uint8 bus;
    uint8 dev;
    uint8 func;
    struct pci_virtual_child child;
};

static struct pci_config_backend pci_config_backends[PCI_CONFIG_BACKEND_MAX];
static struct pci_virtual_child_entry pci_virtual_children[PCI_VIRTUAL_CHILD_MAX];
static uint32 pci_virtual_child_count;
static uint32 pci_virtual_children_registered;
static uint32 pci_virtual_children_probed;
static uint32 pci_virtual_children_probe_failed;
static int pci_init_done;
static int pci_legacy_scan_active;

static void pci_probe_function(uint8 bus, uint8 dev, uint8 func);

static struct pci_virtual_child_entry *pci_find_virtual_child(uint8 bus,
                                                              uint8 dev,
                                                              uint8 func)
{
    for (uint32 i = 0; i < pci_virtual_child_count; i++) {
        struct pci_virtual_child_entry *entry = &pci_virtual_children[i];

        if (entry->used && entry->bus == bus && entry->dev == dev &&
            entry->func == func)
            return entry;
    }
    return NULL;
}

static uint32 pci_virtual_child_synth_read32(
    const struct pci_virtual_child_entry *entry, uint16 offset)
{
    const struct pci_virtual_child *child = &entry->child;

    switch (offset & 0xffc) {
    case 0x00:
        return ((uint32)child->device_id << 16) | child->vendor_id;
    case 0x08:
        return (child->class_code << 8) | child->revision_id;
    case 0x0c:
        return (uint32)child->header_type << 16;
    case 0x2c:
        return ((uint32)child->subsystem_id << 16) |
               child->subsystem_vendor_id;
    case 0x3c:
        return child->irq_line;
    default:
        return 0;
    }
}

#if defined(__x86_64__) || defined(__i386__)

#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

static void pci_outl(uint16 port, uint32 val)
{
    asm volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static uint32 pci_inl(uint16 port)
{
    uint32 val;

    asm volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static uint32 pci_legacy_config_read32(uint8 bus, uint8 dev, uint8 func,
                                       uint16 offset)
{
    uint32 addr = (1U << 31) | ((uint32)bus << 16) |
                  ((uint32)dev << 11) | ((uint32)func << 8) |
                  (offset & 0xFC);

    pci_outl(PCI_CONFIG_ADDR, addr);
    return pci_inl(PCI_CONFIG_DATA);
}

static void pci_legacy_config_write32(uint8 bus, uint8 dev, uint8 func,
                                      uint16 offset, uint32 val)
{
    uint32 addr = (1U << 31) | ((uint32)bus << 16) |
                  ((uint32)dev << 11) | ((uint32)func << 8) |
                  (offset & 0xFC);

    pci_outl(PCI_CONFIG_ADDR, addr);
    pci_outl(PCI_CONFIG_DATA, val);
}

#else

static uint32 pci_legacy_config_read32(uint8 bus, uint8 dev, uint8 func,
                                       uint16 offset)
{
    uint32 off = ((uint32)bus << 16) | ((uint32)dev << 11) |
                 ((uint32)func << 8) | (offset & 0xFC);
    volatile uint32 *base = (volatile uint32 *)(PCIE_ECAM + off);

    return *base;
}

static void pci_legacy_config_write32(uint8 bus, uint8 dev, uint8 func,
                                      uint16 offset, uint32 val)
{
    uint32 off = ((uint32)bus << 16) | ((uint32)dev << 11) |
                 ((uint32)func << 8) | (offset & 0xFC);
    volatile uint32 *base = (volatile uint32 *)(PCIE_ECAM + off);

    *base = val;
}

#endif

uint32 pci_config_read32(uint8 bus, uint8 dev, uint8 func, uint16 offset)
{
    struct pci_virtual_child_entry *entry =
        pci_find_virtual_child(bus, dev, func);

    if (!pci_legacy_scan_active && entry != NULL) {
        uint32 backend_index = entry->child.backend_index;
        uint32 value = 0xffffffffU;

        if (backend_index < PCI_CONFIG_BACKEND_MAX &&
            pci_config_backends[backend_index].used &&
            pci_config_backends[backend_index].ops != NULL &&
            pci_config_backends[backend_index].ops->read != NULL &&
            pci_config_backends[backend_index].ops->read(
                pci_config_backends[backend_index].ctx,
                entry->child.backend_token, offset & 0xFFC, 4,
                &value) == 0)
            return value;
        return pci_virtual_child_synth_read32(entry, offset);
    }

    return pci_legacy_config_read32(bus, dev, func, offset);
}

int pci_config_try_write32(uint8 bus, uint8 dev, uint8 func, uint16 offset,
                           uint32 val)
{
    struct pci_virtual_child_entry *entry =
        pci_find_virtual_child(bus, dev, func);

    if (!pci_legacy_scan_active && entry != NULL) {
        uint32 backend_index = entry->child.backend_index;

        if (backend_index < PCI_CONFIG_BACKEND_MAX &&
            pci_config_backends[backend_index].used &&
            pci_config_backends[backend_index].ops != NULL &&
            pci_config_backends[backend_index].ops->write != NULL)
            return pci_config_backends[backend_index].ops->write(
                pci_config_backends[backend_index].ctx,
                entry->child.backend_token, offset & 0xFFC, 4, val);
        return -ENOTSUP;
    }

    pci_legacy_config_write32(bus, dev, func, offset, val);
    return 0;
}

void pci_config_write32(uint8 bus, uint8 dev, uint8 func, uint16 offset,
                        uint32 val)
{
    (void)pci_config_try_write32(bus, dev, func, offset, val);
}

uint16 pci_config_read16(uint8 bus, uint8 dev, uint8 func, uint16 offset)
{
    uint32 val = pci_config_read32(bus, dev, func, offset & 0xFC);

    return (val >> ((offset & 2) * 8)) & 0xFFFF;
}

uint8 pci_config_read8(uint8 bus, uint8 dev, uint8 func, uint16 offset)
{
    uint32 val = pci_config_read32(bus, dev, func, offset & 0xFC);

    return (val >> ((offset & 3) * 8)) & 0xFF;
}

void pci_config_write16(uint8 bus, uint8 dev, uint8 func, uint16 offset,
                        uint16 newval)
{
    struct pci_virtual_child_entry *entry =
        pci_find_virtual_child(bus, dev, func);

    if (!pci_legacy_scan_active && entry != NULL) {
        uint32 backend_index = entry->child.backend_index;

        if (backend_index < PCI_CONFIG_BACKEND_MAX &&
            pci_config_backends[backend_index].used &&
            pci_config_backends[backend_index].ops != NULL &&
            pci_config_backends[backend_index].ops->write != NULL &&
            pci_config_backends[backend_index].ops->write(
                pci_config_backends[backend_index].ctx,
                entry->child.backend_token, offset, 2, newval) == 0)
            return;
    }

    uint32 old = pci_config_read32(bus, dev, func, offset & 0xFC);
    int shift = (offset & 2) * 8;
    old &= ~(0xFFFFU << shift);
    old |= ((uint32)newval << shift);
    pci_config_write32(bus, dev, func, offset & 0xFC, old);
}

void pci_config_write8(uint8 bus, uint8 dev, uint8 func, uint16 offset,
                       uint8 newval)
{
    struct pci_virtual_child_entry *entry =
        pci_find_virtual_child(bus, dev, func);

    if (!pci_legacy_scan_active && entry != NULL) {
        uint32 backend_index = entry->child.backend_index;

        if (backend_index < PCI_CONFIG_BACKEND_MAX &&
            pci_config_backends[backend_index].used &&
            pci_config_backends[backend_index].ops != NULL &&
            pci_config_backends[backend_index].ops->write != NULL &&
            pci_config_backends[backend_index].ops->write(
                pci_config_backends[backend_index].ctx,
                entry->child.backend_token, offset, 1, newval) == 0)
            return;
    }

    uint32 old = pci_config_read32(bus, dev, func, offset & 0xFC);
    int shift = (offset & 3) * 8;
    old &= ~(0xFFU << shift);
    old |= ((uint32)newval << shift);
    pci_config_write32(bus, dev, func, offset & 0xFC, old);
}

int pci_register_config_backend(const char *name,
                                const struct pci_config_backend_ops *ops,
                                void *ctx)
{
    if (ops == NULL || ops->read == NULL || ops->write == NULL)
        return -EINVAL;

    for (uint32 i = 0; i < PCI_CONFIG_BACKEND_MAX; i++) {
        if (!pci_config_backends[i].used) {
            pci_config_backends[i].used = 1;
            pci_config_backends[i].name = name;
            pci_config_backends[i].ops = ops;
            pci_config_backends[i].ctx = ctx;
            return (int)i;
        }
    }
    return -ENOSPC;
}

int pci_register_virtual_child(const struct pci_virtual_child *child,
                               uint8 *bus_out, uint8 *dev_out,
                               uint8 *func_out)
{
    struct pci_virtual_child_entry *entry;
    uint32 slot;

    if (child == NULL || child->backend_index >= PCI_CONFIG_BACKEND_MAX ||
        !pci_config_backends[child->backend_index].used)
        return -EINVAL;
    if (pci_virtual_child_count >= PCI_VIRTUAL_CHILD_MAX)
        return -ENOSPC;

    slot = pci_virtual_child_count++;
    entry = &pci_virtual_children[slot];
    entry->used = 1;
    entry->bus = PCI_VIRTUAL_BUS;
    entry->dev = (uint8)(slot / 8);
    entry->func = (uint8)(slot % 8);
    entry->child = *child;
    pci_virtual_children_registered++;

    if (bus_out != NULL)
        *bus_out = entry->bus;
    if (dev_out != NULL)
        *dev_out = entry->dev;
    if (func_out != NULL)
        *func_out = entry->func;
    printf("PCI: virtual child registered %d:%d:%d backend=%u token=0x%lx vendor=0x%x device=0x%x\n",
           entry->bus, entry->dev, entry->func, child->backend_index,
           (uint64)child->backend_token, child->vendor_id,
           child->device_id);
    if (pci_init_done)
        (void)pci_probe_virtual_bdf(entry->bus, entry->dev, entry->func);
    return 0;
}

int pci_probe_virtual_bdf(uint8 bus, uint8 dev, uint8 func)
{
    struct pci_virtual_child_entry *entry =
        pci_find_virtual_child(bus, dev, func);
    uint32 id;

    if (entry == NULL)
        return -ENOENT;
    if (entry->probed)
        return 0;

    id = pci_config_read32(bus, dev, func, 0);
    if (id == 0xffffffffU || id == 0) {
        pci_virtual_children_probe_failed++;
        printf("PCI: virtual child probe failed %d:%d:%d id=0x%lx\n",
               bus, dev, func, (uint64)id);
        return -ENODEV;
    }

    pci_probe_function(bus, dev, func);
    entry->probed = 1;
    pci_virtual_children_probed++;
    printf("PCI: virtual child probed %d:%d:%d id=0x%lx\n",
           bus, dev, func, (uint64)id);
    return 0;
}

void pci_probe_registered_virtual_children(void)
{
    for (uint32 i = 0; i < pci_virtual_child_count; i++) {
        struct pci_virtual_child_entry *entry = &pci_virtual_children[i];

        if (entry->used && !entry->probed)
            (void)pci_probe_virtual_bdf(entry->bus, entry->dev,
                                        entry->func);
    }
}

#if defined(__x86_64__) || defined(__i386__)

// ─── x86 PCI init using I/O-port configuration space access ───

// Read a BAR, determine if it's memory or I/O, return the base address
static uint32 pci_read_bar(uint8 bus, uint8 dev, uint8 func, int bar_idx)
{
    uint8 offset = 0x10 + bar_idx * 4;
    return pci_config_read32(bus, dev, func, offset);
}

// Find a virtio PCI capability of a given type.
// Returns config space offset, or 0 if not found.
static uint8 pci_find_virtio_cap(uint8 bus, uint8 dev, uint8 func,
                                 uint8 cfg_type)
{
    uint16 status = pci_config_read16(bus, dev, func, 0x06);
    if (!(status & PCIE_STATUS_CAPL))
        return 0;

    uint8 pos = pci_config_read8(bus, dev, func, 0x34) & 0xFC;
    while (pos) {
        uint8 id = pci_config_read8(bus, dev, func, pos);
        if (id == PCI_CAP_ID_VENDOR) {
            // Check the cfg_type field at offset +3
            uint8 type = pci_config_read8(bus, dev, func, pos + 3);
            if (type == cfg_type)
                return pos;
        }
        pos = pci_config_read8(bus, dev, func, pos + 1) & 0xFC;
        if (pos == 0xFF)
            break;
    }
    return 0;
}

// Stored info about discovered virtio device for virtio_disk_init_pci()
// (struct defined in pci.h)

static struct virtio_pci_discovery virtio_pci_devs[N_VIRTIO_DISK];
static struct virtio_pci_discovery virtio_gpu_pci_devs[N_VIRTIO_GPU];
static struct virtio_pci_discovery virtio_input_pci_devs[N_VIRTIO_INPUT];
static struct virtio_pci_discovery virtio_net_pci_devs[N_VIRTIO_NET];
static struct pci_device_info nvidia_gpu_pci_devs[4];
static int virtio_pci_count = 0;
static int virtio_gpu_pci_count = 0;
static int virtio_input_pci_count = 0;
static int virtio_net_pci_count = 0;
static int nvidia_gpu_pci_count = 0;
static int pci_diag_microsoft_count = 0;
static int pci_diag_display_count = 0;
static int pci_diag_accelerator_count = 0;
static int pci_diag_microsoft_display_or_accel_count = 0;
static uint32 pci_diag_scanned_functions = 0;
static uint32 pci_diag_present_functions = 0;

struct pci_diag_candidate {
    int valid;
    uint8 bus;
    uint8 dev;
    uint8 func;
    uint8 base_class;
    uint8 subclass;
    uint8 prog_if;
    uint8 revision;
    uint8 header;
    uint16 vendor;
    uint16 device;
    uint32 class_code;
    uint32 bar0;
    uint32 bar1;
};

static struct pci_diag_candidate pci_diag_first_candidate;
static struct pci_diag_candidate pci_diag_last_candidate;

struct virtio_pci_discovery *pci_get_virtio_blk(int index)
{
    if (index < 0 || index >= virtio_pci_count)
        return 0;
    return &virtio_pci_devs[index];
}

struct virtio_pci_discovery *pci_get_virtio_gpu(int index)
{
    if (index < 0 || index >= virtio_gpu_pci_count)
        return 0;
    return &virtio_gpu_pci_devs[index];
}

struct virtio_pci_discovery *pci_get_virtio_input(int index)
{
    if (index < 0 || index >= virtio_input_pci_count)
        return 0;
    return &virtio_input_pci_devs[index];
}

struct virtio_pci_discovery *pci_get_virtio_net(int index)
{
    if (index < 0 || index >= virtio_net_pci_count)
        return 0;
    return &virtio_net_pci_devs[index];
}

struct pci_device_info *pci_get_nvidia_gpu(int index)
{
    if (index < 0 || index >= nvidia_gpu_pci_count)
        return 0;
    return &nvidia_gpu_pci_devs[index];
}

// ─── PIIX3 PIRQ routing ───────────────────────────────────────────────

// IRQ assigned to each PIRQ line (set by piix3_init_irq_routing)
static uint8 pirq_irq_map[4];

// Program the PIIX3 ISA bridge (bus 0, dev 1, func 0) PIRQ routing
// registers so that each PIRQ line is routed to a distinct ISA IRQ.
// This avoids IRQ sharing that QEMU's default firmware may set up.
static void piix3_init_irq_routing(void)
{
    // PIRQ routing registers at config offsets 0x60-0x63 (one byte each)
    // Bit 7: 0 = enabled, bits 3:0 = ISA IRQ number
    static const uint8 routing[4] = {5, 9, 10, 11}; // PIRQA-D
    for (int i = 0; i < 4; i++) {
        pci_config_write8(0, 1, 0, 0x60 + i, routing[i]);
        pirq_irq_map[i] = routing[i];
    }
    printf("PCI: PIIX3 PIRQ routing: A->IRQ%d B->IRQ%d C->IRQ%d D->IRQ%d\n",
           routing[0], routing[1], routing[2], routing[3]);
}

// Compute the ISA IRQ for a PCI device using QEMU i440FX PIIX3 routing.
// Formula: pirq = (intx + slot - 1) & 3, where intx = intpin - 1.
// intpin: value from PCI config register 0x3D (1=INTA..4=INTD).
static uint8 piix3_compute_irq(uint8 slot, uint8 intpin)
{
    if (intpin == 0 || intpin > 4)
        return 0;
    int pirq = ((intpin - 1) + (slot - 1)) & 3;
    return pirq_irq_map[pirq];
}

// Forward declare for PCI IRQ level-trigger programming
extern void plic_enable_irq_level(int irq);

// ─── PCI device init ──────────────────────────────────────────────────

static void pci_init_e1000(uint8 bus, uint8 dev, uint8 func)
{
    printf("PCI: e1000 detected at %d:%d:%d\n", bus, dev, func);

    // Enable memory access and bus mastering
    uint16 cmd = pci_config_read16(bus, dev, func, 0x04);
    cmd |= PCIE_CSCMD_MAE | PCIE_CSCMD_BME;
    pci_config_write16(bus, dev, func, 0x04, cmd);

    // Read BAR0 (memory-mapped registers)
    uint32 bar0 = pci_read_bar(bus, dev, func, 0) & ~0xFU;
    // Compute actual IRQ from PIIX3 routing (register 0x3C is unreliable)
    uint8 irq = piix3_compute_irq(dev, pci_config_read8(bus, dev, func, 0x3D));

    printf("PCI: e1000 BAR0=0x%lx IRQ=%d\n", (uint64)bar0, irq);

    __e1000_pci_mmio_base = (uint64)PA2VA((uint64)bar0);
    __e1000_pci_irqno = (uint64)irq;

    e1000_init((uint32 *)(uint64)PA2VA((uint64)bar0));

    // PCI interrupts are level-triggered, active-low
    plic_enable_irq_level(irq);
}

static void pci_init_virtio_blk(uint8 bus, uint8 dev, uint8 func)
{
    if (virtio_pci_count >= N_VIRTIO_DISK) {
        printf("PCI: too many virtio-blk devices\n");
        return;
    }

    printf("PCI: virtio-blk detected at %d:%d:%d\n", bus, dev, func);

    // Enable memory access and bus mastering
    uint16 cmd = pci_config_read16(bus, dev, func, 0x04);
    cmd |= PCIE_CSCMD_MAE | PCIE_CSCMD_BME;
    pci_config_write16(bus, dev, func, 0x04, cmd);

    struct virtio_pci_discovery *vd = &virtio_pci_devs[virtio_pci_count];
    vd->found = 1;
    vd->bus = bus;
    vd->dev = dev;
    vd->func = func;
    // Compute actual IRQ from PIIX3 routing (register 0x3C is unreliable)
    vd->irq_line = piix3_compute_irq(dev, pci_config_read8(bus, dev, func, 0x3D));

    for (int i = 0; i < 6; i++)
        vd->bar[i] = pci_read_bar(bus, dev, func, i);

    // Find virtio PCI capabilities
    vd->common_cfg_cap = pci_find_virtio_cap(bus, dev, func,
                                              VIRTIO_PCI_CAP_COMMON_CFG);
    vd->notify_cfg_cap = pci_find_virtio_cap(bus, dev, func,
                                              VIRTIO_PCI_CAP_NOTIFY_CFG);
    vd->isr_cfg_cap = pci_find_virtio_cap(bus, dev, func,
                                            VIRTIO_PCI_CAP_ISR_CFG);
    vd->device_cfg_cap = pci_find_virtio_cap(bus, dev, func,
                                              VIRTIO_PCI_CAP_DEVICE_CFG);

    printf("PCI: virtio-blk BAR0=0x%lx BAR4=0x%lx IRQ=%d caps: common=%d notify=%d isr=%d dev=%d\n",
           (uint64)vd->bar[0], (uint64)vd->bar[4], vd->irq_line,
           vd->common_cfg_cap, vd->notify_cfg_cap,
           vd->isr_cfg_cap, vd->device_cfg_cap);

    virtio_pci_count++;

    // Set platform flags so virtio_disk_init() knows devices exist
    extern struct platform_info platform;
    platform.has_virtio = 1;
    platform.virtio_count = virtio_pci_count;
}

static void pci_note_virtio_gpu(uint8 bus, uint8 dev, uint8 func)
{
    if (virtio_gpu_pci_count >= N_VIRTIO_GPU) {
        printf("PCI: too many virtio-gpu devices\n");
        return;
    }

    uint16 cmd = pci_config_read16(bus, dev, func, 0x04);
    cmd |= PCIE_CSCMD_MAE | PCIE_CSCMD_BME;
    pci_config_write16(bus, dev, func, 0x04, cmd);

    struct virtio_pci_discovery *vd =
        &virtio_gpu_pci_devs[virtio_gpu_pci_count];
    vd->found = 1;
    vd->bus = bus;
    vd->dev = dev;
    vd->func = func;
    vd->irq_line = piix3_compute_irq(dev, pci_config_read8(bus, dev, func,
                                                            0x3D));

    for (int i = 0; i < 6; i++)
        vd->bar[i] = pci_read_bar(bus, dev, func, i);

    vd->common_cfg_cap = pci_find_virtio_cap(bus, dev, func,
                                             VIRTIO_PCI_CAP_COMMON_CFG);
    vd->notify_cfg_cap = pci_find_virtio_cap(bus, dev, func,
                                             VIRTIO_PCI_CAP_NOTIFY_CFG);
    vd->isr_cfg_cap = pci_find_virtio_cap(bus, dev, func,
                                          VIRTIO_PCI_CAP_ISR_CFG);
    vd->device_cfg_cap = pci_find_virtio_cap(bus, dev, func,
                                             VIRTIO_PCI_CAP_DEVICE_CFG);

    virtio_gpu_pci_count++;
    printf("PCI: virtio-gpu detected at %d:%d:%d\n", bus, dev, func);
    printf("PCI: virtio-gpu BAR0=0x%lx BAR1=0x%lx BAR2=0x%lx BAR4=0x%lx IRQ=%d caps: common=%d notify=%d isr=%d dev=%d\n",
           (uint64)vd->bar[0], (uint64)vd->bar[1], (uint64)vd->bar[2],
           (uint64)vd->bar[4], vd->irq_line, vd->common_cfg_cap,
           vd->notify_cfg_cap, vd->isr_cfg_cap, vd->device_cfg_cap);
}

static void pci_note_virtio_input(uint8 bus, uint8 dev, uint8 func)
{
    if (virtio_input_pci_count >= N_VIRTIO_INPUT) {
        printf("PCI: too many virtio-input devices\n");
        return;
    }

    uint16 cmd = pci_config_read16(bus, dev, func, 0x04);
    cmd |= PCIE_CSCMD_MAE | PCIE_CSCMD_BME;
    pci_config_write16(bus, dev, func, 0x04, cmd);

    struct virtio_pci_discovery *vd =
        &virtio_input_pci_devs[virtio_input_pci_count];
    vd->found = 1;
    vd->bus = bus;
    vd->dev = dev;
    vd->func = func;
    vd->irq_line = piix3_compute_irq(dev, pci_config_read8(bus, dev, func,
                                                            0x3D));

    for (int i = 0; i < 6; i++)
        vd->bar[i] = pci_read_bar(bus, dev, func, i);

    vd->common_cfg_cap = pci_find_virtio_cap(bus, dev, func,
                                             VIRTIO_PCI_CAP_COMMON_CFG);
    vd->notify_cfg_cap = pci_find_virtio_cap(bus, dev, func,
                                             VIRTIO_PCI_CAP_NOTIFY_CFG);
    vd->isr_cfg_cap = pci_find_virtio_cap(bus, dev, func,
                                          VIRTIO_PCI_CAP_ISR_CFG);
    vd->device_cfg_cap = pci_find_virtio_cap(bus, dev, func,
                                             VIRTIO_PCI_CAP_DEVICE_CFG);

    virtio_input_pci_count++;
    printf("PCI: virtio-input detected at %d:%d:%d\n", bus, dev, func);
    printf("PCI: virtio-input BAR0=0x%lx BAR1=0x%lx BAR2=0x%lx BAR4=0x%lx IRQ=%d caps: common=%d notify=%d isr=%d dev=%d\n",
           (uint64)vd->bar[0], (uint64)vd->bar[1], (uint64)vd->bar[2],
           (uint64)vd->bar[4], vd->irq_line, vd->common_cfg_cap,
           vd->notify_cfg_cap, vd->isr_cfg_cap, vd->device_cfg_cap);
}

static void pci_note_virtio_net(uint8 bus, uint8 dev, uint8 func)
{
    if (virtio_net_pci_count >= N_VIRTIO_NET) {
        printf("PCI: too many virtio-net devices\n");
        return;
    }

    uint16 cmd = pci_config_read16(bus, dev, func, 0x04);
    cmd |= PCIE_CSCMD_MAE | PCIE_CSCMD_BME;
    pci_config_write16(bus, dev, func, 0x04, cmd);

    struct virtio_pci_discovery *vd =
        &virtio_net_pci_devs[virtio_net_pci_count];
    vd->found = 1;
    vd->bus = bus;
    vd->dev = dev;
    vd->func = func;
    vd->irq_line = piix3_compute_irq(dev, pci_config_read8(bus, dev, func,
                                                            0x3D));

    for (int i = 0; i < 6; i++)
        vd->bar[i] = pci_read_bar(bus, dev, func, i);

    vd->common_cfg_cap = pci_find_virtio_cap(bus, dev, func,
                                             VIRTIO_PCI_CAP_COMMON_CFG);
    vd->notify_cfg_cap = pci_find_virtio_cap(bus, dev, func,
                                             VIRTIO_PCI_CAP_NOTIFY_CFG);
    vd->isr_cfg_cap = pci_find_virtio_cap(bus, dev, func,
                                          VIRTIO_PCI_CAP_ISR_CFG);
    vd->device_cfg_cap = pci_find_virtio_cap(bus, dev, func,
                                             VIRTIO_PCI_CAP_DEVICE_CFG);

    virtio_net_pci_count++;
    printf("PCI: virtio-net detected at %d:%d:%d\n", bus, dev, func);
    printf("PCI: virtio-net IRQ=%d caps: common=%d notify=%d isr=%d dev=%d\n",
           vd->irq_line, vd->common_cfg_cap, vd->notify_cfg_cap,
           vd->isr_cfg_cap, vd->device_cfg_cap);
}

static void pci_diag_note_candidate(uint8 bus, uint8 dev, uint8 func,
                                    uint16 vendor, uint16 device)
{
    enum {
        PCI_CLASS_DISPLAY = 0x03,
        PCI_CLASS_PROCESSING_ACCELERATOR = 0x12,
    };
    uint32 class_rev = pci_config_read32(bus, dev, func, 0x08);
    uint32 class_code = class_rev >> 8;
    uint8 base_class = (uint8)((class_code >> 16) & 0xff);
    uint8 subclass = (uint8)((class_code >> 8) & 0xff);
    uint8 prog_if = (uint8)(class_code & 0xff);
    uint8 revision = (uint8)(class_rev & 0xff);
    uint8 header = pci_config_read8(bus, dev, func, 0x0e);
    uint32 bar0 = pci_read_bar(bus, dev, func, 0);
    uint32 bar1 = pci_read_bar(bus, dev, func, 1);
    int is_microsoft = vendor == PCI_VENDOR_MICROSOFT;
    int is_display = base_class == PCI_CLASS_DISPLAY;
    int is_accelerator = base_class == PCI_CLASS_PROCESSING_ACCELERATOR;
    struct pci_diag_candidate candidate;

    if (!is_microsoft && !is_display && !is_accelerator)
        return;

    candidate.valid = 1;
    candidate.bus = bus;
    candidate.dev = dev;
    candidate.func = func;
    candidate.base_class = base_class;
    candidate.subclass = subclass;
    candidate.prog_if = prog_if;
    candidate.revision = revision;
    candidate.header = header;
    candidate.vendor = vendor;
    candidate.device = device;
    candidate.class_code = class_code;
    candidate.bar0 = bar0;
    candidate.bar1 = bar1;
    if (!pci_diag_first_candidate.valid)
        pci_diag_first_candidate = candidate;
    pci_diag_last_candidate = candidate;

    if (is_microsoft)
        pci_diag_microsoft_count++;
    if (is_display)
        pci_diag_display_count++;
    if (is_accelerator)
        pci_diag_accelerator_count++;
    if (is_microsoft && (is_display || is_accelerator))
        pci_diag_microsoft_display_or_accel_count++;

    printf("PCI: candidate %d:%d:%d vendor=0x%x device=0x%x class=0x%lx base=0x%x sub=0x%x prog=0x%x rev=0x%x header=0x%x bar0=0x%lx bar1=0x%lx%s%s%s\n",
           bus, dev, func, vendor, device, (uint64)class_code,
           base_class, subclass, prog_if, revision, header,
           (uint64)bar0, (uint64)bar1,
           is_microsoft ? " microsoft" : "",
           is_display ? " display" : "",
           is_accelerator ? " accelerator" : "");
}

static void pci_note_hyperv_dxg(uint8 bus, uint8 dev, uint8 func,
                                uint16 vendor, uint16 device)
{
    enum {
        DXGK_VMBUS_CHANNEL_ID_OFFSET = 192,
        DXGK_VMBUS_VERSION_OFFSET = 208,
        DXGK_VMBUS_GUESTCAPS_OFFSET = 212,
        DXGK_VMBUS_VGPU_LUID_OFFSET = 212,
        DXGK_VMBUS_GUESTCAPS_WSL2 = 1,
    };
    uint16 cmd = pci_config_read16(bus, dev, func, 0x04);
    cmd |= PCIE_CSCMD_MAE | PCIE_CSCMD_BME;
    pci_config_write16(bus, dev, func, 0x04, cmd);

    uint32 class_rev = pci_config_read32(bus, dev, func, 0x08);
    uint32 class_code = class_rev >> 8;
    uint32 guid0 = pci_config_read32(bus, dev, func,
                                     DXGK_VMBUS_CHANNEL_ID_OFFSET);
    uint32 guid1 = pci_config_read32(bus, dev, func,
                                     DXGK_VMBUS_CHANNEL_ID_OFFSET + 4);
    uint32 guid2 = pci_config_read32(bus, dev, func,
                                     DXGK_VMBUS_CHANNEL_ID_OFFSET + 8);
    uint32 guid3 = pci_config_read32(bus, dev, func,
                                     DXGK_VMBUS_CHANNEL_ID_OFFSET + 12);

    /*
     * WSL reports guest capabilities before reading the negotiated
     * version/channel data.  This config dword later aliases the low
     * host-vGPU LUID, so the immediate readback is diagnostic only.
     */
    pci_config_write32(bus, dev, func, DXGK_VMBUS_GUESTCAPS_OFFSET,
                       DXGK_VMBUS_GUESTCAPS_WSL2);
    uint32 guestcaps_readback =
        pci_config_read32(bus, dev, func, DXGK_VMBUS_GUESTCAPS_OFFSET);

    uint32 vmbus_version = pci_config_read32(bus, dev, func,
                                             DXGK_VMBUS_VERSION_OFFSET);
    uint32 luid0 = pci_config_read32(bus, dev, func,
                                     DXGK_VMBUS_VGPU_LUID_OFFSET);
    uint32 luid1 = pci_config_read32(bus, dev, func,
                                     DXGK_VMBUS_VGPU_LUID_OFFSET + 4);

    printf("PCI: Hyper-V GPU-PV %s detected at %d:%d:%d\n",
        device == PCI_DEVICE_MS_COMPUTE_ACCELERATOR ?
        "compute accelerator" : "virtual render", bus, dev, func);
    printf("PCI: Hyper-V GPU-PV channel-guid=%lx-%lx-%lx-%lx version=%u host-luid=%lx:%lx guestcaps=0x%lx readback=0x%lx\n",
        (uint64)guid0, (uint64)guid1, (uint64)guid2, (uint64)guid3,
        vmbus_version, (uint64)luid0, (uint64)luid1,
        (uint64)DXGK_VMBUS_GUESTCAPS_WSL2,
        (uint64)guestcaps_readback);
    hyperv_dxg_note_pci(0, bus, dev, func, vendor, device, class_code,
                        guid0, guid1, guid2, guid3, vmbus_version,
                        luid0, luid1, DXGK_VMBUS_GUESTCAPS_OFFSET,
                        DXGK_VMBUS_GUESTCAPS_WSL2, guestcaps_readback,
                        0);
}

static void pci_note_nvidia_gpu(uint8 bus, uint8 dev, uint8 func,
                                uint16 vendor, uint16 device)
{
    if (nvidia_gpu_pci_count >= (int)(sizeof(nvidia_gpu_pci_devs) /
                                      sizeof(nvidia_gpu_pci_devs[0]))) {
        printf("PCI: too many NVIDIA GPU devices, ignoring %d:%d:%d\n",
               bus, dev, func);
        return;
    }

    uint16 cmd = pci_config_read16(bus, dev, func, 0x04);
    cmd |= PCIE_CSCMD_MAE | PCIE_CSCMD_BME;
    pci_config_write16(bus, dev, func, 0x04, cmd);

    struct pci_device_info *info = &nvidia_gpu_pci_devs[nvidia_gpu_pci_count];
    memset(info, 0, sizeof(*info));
    info->vendor_id = vendor;
    info->device_id = device;
    info->bus = bus;
    info->dev = dev;
    info->func = func;
    info->irq_line = pci_config_read8(bus, dev, func, 0x3D);
    for (int i = 0; i < 6; i++)
        info->bar[i] = pci_read_bar(bus, dev, func, i);
    nvidia_gpu_pci_count++;

    printf("PCI: NVIDIA GPU candidate at %d:%d:%d device=0x%x irq=%d bar0=0x%lx bar1=0x%lx\n",
           bus, dev, func, device, info->irq_line,
           (uint64)info->bar[0], (uint64)info->bar[1]);
}

static void pci_probe_function(uint8 bus, uint8 dev, uint8 func)
{
    uint32 id = pci_config_read32(bus, dev, func, 0);
    uint16 vendor;
    uint16 device;
    uint32 class_code;
    uint32 base_class;

    if (id == 0xFFFFFFFF || id == 0)
        return;

    vendor = id & 0xFFFF;
    device = (id >> 16) & 0xFFFF;
    class_code = pci_config_read32(bus, dev, func, 0x08) >> 8;
    base_class = (class_code >> 16) & 0xff;

    printf("PCI %d:%d:%d vendor=0x%x device=0x%x\n",
           bus, dev, func, vendor, device);
    pci_diag_note_candidate(bus, dev, func, vendor, device);

    if (vendor == PCI_VENDOR_INTEL && device == PCI_DEVICE_E1000) {
        pci_init_e1000(bus, dev, func);
    } else if (vendor == PCI_VENDOR_VIRTIO &&
               (device == PCI_DEVICE_VIRTIO_BLK_TRANSITIONAL ||
                device == PCI_DEVICE_VIRTIO_BLK_MODERN)) {
        pci_init_virtio_blk(bus, dev, func);
    } else if (vendor == PCI_VENDOR_VIRTIO &&
               (device == PCI_DEVICE_VIRTIO_GPU_TRANSITIONAL ||
                device == PCI_DEVICE_VIRTIO_GPU_MODERN)) {
        pci_note_virtio_gpu(bus, dev, func);
    } else if (vendor == PCI_VENDOR_VIRTIO &&
               (device == PCI_DEVICE_VIRTIO_INPUT_TRANSITIONAL ||
                device == PCI_DEVICE_VIRTIO_INPUT_MODERN)) {
        pci_note_virtio_input(bus, dev, func);
    } else if (vendor == PCI_VENDOR_VIRTIO &&
               (device == PCI_DEVICE_VIRTIO_NET_TRANSITIONAL ||
                device == PCI_DEVICE_VIRTIO_NET_MODERN)) {
        pci_note_virtio_net(bus, dev, func);
    } else if (vendor == PCI_VENDOR_MICROSOFT &&
           (device == PCI_DEVICE_MS_VIRTUAL_RENDER ||
            device == PCI_DEVICE_MS_COMPUTE_ACCELERATOR)) {
        pci_note_hyperv_dxg(bus, dev, func, vendor, device);
    } else if (vendor == PCI_VENDOR_NVIDIA && base_class == 0x03) {
        pci_note_nvidia_gpu(bus, dev, func, vendor, device);
    } else if (vendor == PCI_VENDOR_BOCHS &&
               device == PCI_DEVICE_BOCHS_VGA) {
        fb_pci_init(bus, dev, func);
    }
}

void pci_init(void)
{
    piix3_init_irq_routing();

    printf("PCI: scanning buses 0-255 (x86 I/O port method)\n");
    pci_diag_microsoft_count = 0;
    pci_diag_display_count = 0;
    pci_diag_accelerator_count = 0;
    pci_diag_microsoft_display_or_accel_count = 0;
    pci_diag_scanned_functions = 0;
    pci_diag_present_functions = 0;
    pci_diag_first_candidate.valid = 0;
    pci_diag_last_candidate.valid = 0;

    pci_legacy_scan_active = 1;
    for (int bus = 0; bus < 256; bus++) {
    for (int dev = 0; dev < 32; dev++) {
    for (int func = 0; func < 8; func++) {
        pci_diag_scanned_functions++;
        uint32 id = pci_config_read32(bus, dev, func, 0);
        if (id == 0xFFFFFFFF || id == 0)
            continue;
        pci_diag_present_functions++;
        pci_probe_function(bus, dev, func);
    }
    }
    }
    pci_legacy_scan_active = 0;
    pci_init_done = 1;
    pci_probe_registered_virtual_children();

    printf("PCI: scan complete, %d virtio-blk device(s), %d virtio-gpu device(s), %d virtio-input device(s), %d virtio-net device(s) found\n",
           virtio_pci_count, virtio_gpu_pci_count, virtio_input_pci_count,
           virtio_net_pci_count);
    printf("PCI: diagnostics microsoft=%d display=%d accelerator=%d microsoft_display_or_accel=%d\n",
           pci_diag_microsoft_count, pci_diag_display_count,
           pci_diag_accelerator_count,
           pci_diag_microsoft_display_or_accel_count);
    printf("PCI: config diagnostics source=legacy-cf8 mcfg=not-parsed root_bridge=unknown ecam_base=0x%lx ecam_used=0 scanned=%lx present=%lx\n",
           (uint64)PCIE_ECAM, (uint64)pci_diag_scanned_functions,
           (uint64)pci_diag_present_functions);
    printf("PCI: virtual diagnostics registered=%lx probed=%lx failed=%lx\n",
           (uint64)pci_virtual_children_registered,
           (uint64)pci_virtual_children_probed,
           (uint64)pci_virtual_children_probe_failed);
    if (pci_diag_first_candidate.valid) {
        printf("PCI: candidate diagnostics first=%d:%d:%d vendor=0x%x device=0x%x class=0x%lx bar0=0x%lx bar1=0x%lx last=%d:%d:%d vendor=0x%x device=0x%x class=0x%lx bar0=0x%lx bar1=0x%lx\n",
               pci_diag_first_candidate.bus, pci_diag_first_candidate.dev,
               pci_diag_first_candidate.func, pci_diag_first_candidate.vendor,
               pci_diag_first_candidate.device,
               (uint64)pci_diag_first_candidate.class_code,
               (uint64)pci_diag_first_candidate.bar0,
               (uint64)pci_diag_first_candidate.bar1,
               pci_diag_last_candidate.bus, pci_diag_last_candidate.dev,
               pci_diag_last_candidate.func, pci_diag_last_candidate.vendor,
               pci_diag_last_candidate.device,
               (uint64)pci_diag_last_candidate.class_code,
               (uint64)pci_diag_last_candidate.bar0,
               (uint64)pci_diag_last_candidate.bar1);
    } else {
        printf("PCI: candidate diagnostics first=none last=none\n");
    }
}

#else /* RISC-V / ECAM path */

static void pci_probe_function(uint8 bus, uint8 dev, uint8 func)
{
    (void)bus;
    (void)dev;
    (void)func;
}

void pci_init(void) {
    // we'll place the e1000 registers at this address.
    // vm.c maps this range.
    uint64 e1000_regs = 0x40000000L;

    // qemu -machine virt puts PCIe config space here.
    // vm.c maps this range.
    uint32 *ecam = (uint32 *)PCIE_ECAM;

    if (sizeof(struct pci_common_confspace_header) != 0x40) {
        printf("sizeof pci_common_confspace_header: %lx\n",
               sizeof(struct pci_common_confspace_header));
        panic("The size of PCI-E Common Configuration Space Header Structure "
              "is not 0x40 Bytes!");
    }

    // look at each possible PCI device on bus 0.
    for (int dev = 0; dev < 32; dev++) {
        int bus = 0;
        int func = 0;
        int offset = 0;
        uint32 off = (bus << 16) | (dev << 11) | (func << 8) | (offset);
        volatile uint32 *base = ecam + off;
        volatile struct pci_common_confspace_header *dsc = (void *)base;

        if (dev < 8) {
            printf("PCI device %d:%d:%d - vendor ID: 0x%x, device ID: 0x%x\n",
                   bus, dev, func, dsc->vendor_id, dsc->device_id);
        }

        // 100e:8086 is an e1000
        if (dsc->device_id == 0x100e && dsc->vendor_id == 0x8086) {
            printf("E1000 Ethernet Controller detected.\n");
            // command and status register.
            // bit 0 : I/O access enable
            // bit 1 : memory access enable
            // bit 2 : enable mastering
            dsc->command = PCIE_CSCMD_IAE | PCIE_CSCMD_MAE | PCIE_CSCMD_BME;
            __atomic_thread_fence(__ATOMIC_SEQ_CST);

            for (int i = 0; i < 6; i++) {
                uint32 old = dsc->header_type_0.base_addr[i];

                // writing all 1's to the BAR causes it to be
                // replaced with its size.
                dsc->header_type_0.base_addr[i] = 0xffffffff;
                __atomic_thread_fence(__ATOMIC_SEQ_CST);

                dsc->header_type_0.base_addr[i] = old;
            }

            // tell the e1000 to reveal its registers at
            // physical address 0x40000000.
            dsc->header_type_0.base_addr[0] = e1000_regs;

            e1000_init((uint32 *)PA2VA(e1000_regs));
        }
    }
}

#endif /* arch selection */
