//
// PCI device initialization.
// Supports ECAM (RISC-V) and I/O port (x86) config space access.
// Detects e1000 NIC and virtio block device.
//

#include "types.h"
#include "param.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "dev/pci.h"
#include "dev/e1000_dev.h"
#include "dev/virtio.h"
#include "dev/fdt.h"
#include "dev/fb.h"

uint64 __pcie_ecam_mmio_base = (uint64)PA2VA(0x30000000L);

// Forward declarations
void e1000_init(uint32 *xregs);

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
static int virtio_pci_count = 0;
static int virtio_gpu_pci_count = 0;

struct virtio_pci_discovery *pci_get_virtio_blk(int index)
{
    if (index < 0 || index >= virtio_pci_count)
        return 0;
    return &virtio_pci_devs[index];
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
    uint32 bar[6];
    uint8 irq = piix3_compute_irq(dev, pci_config_read8(bus, dev, func, 0x3D));
    uint8 common = pci_find_virtio_cap(bus, dev, func,
                                       VIRTIO_PCI_CAP_COMMON_CFG);
    uint8 notify = pci_find_virtio_cap(bus, dev, func,
                                       VIRTIO_PCI_CAP_NOTIFY_CFG);
    uint8 isr = pci_find_virtio_cap(bus, dev, func,
                                    VIRTIO_PCI_CAP_ISR_CFG);
    uint8 device_cfg = pci_find_virtio_cap(bus, dev, func,
                                           VIRTIO_PCI_CAP_DEVICE_CFG);

    for (int i = 0; i < 6; i++)
        bar[i] = pci_read_bar(bus, dev, func, i);

    virtio_gpu_pci_count++;
    printf("PCI: virtio-gpu detected at %d:%d:%d (driver pending)\n",
           bus, dev, func);
    printf("PCI: virtio-gpu BAR0=0x%lx BAR1=0x%lx BAR2=0x%lx BAR4=0x%lx IRQ=%d caps: common=%d notify=%d isr=%d dev=%d\n",
           (uint64)bar[0], (uint64)bar[1], (uint64)bar[2], (uint64)bar[4],
           irq, common, notify, isr, device_cfg);
}

void pci_init(void)
{
    piix3_init_irq_routing();

    printf("PCI: scanning bus 0 (x86 I/O port method)\n");

    for (int dev = 0; dev < 32; dev++) {
        uint32 id = pci_config_read32(0, dev, 0, 0);
        if (id == 0xFFFFFFFF || id == 0)
            continue;

        uint16 vendor = id & 0xFFFF;
        uint16 device = (id >> 16) & 0xFFFF;

        printf("PCI %d:%d:%d vendor=0x%x device=0x%x\n",
               0, dev, 0, vendor, device);

        if (vendor == PCI_VENDOR_INTEL && device == PCI_DEVICE_E1000) {
            pci_init_e1000(0, dev, 0);
        } else if (vendor == PCI_VENDOR_VIRTIO &&
                   (device == PCI_DEVICE_VIRTIO_BLK_TRANSITIONAL ||
                    device == PCI_DEVICE_VIRTIO_BLK_MODERN)) {
            pci_init_virtio_blk(0, dev, 0);
        } else if (vendor == PCI_VENDOR_VIRTIO &&
                   (device == PCI_DEVICE_VIRTIO_GPU_TRANSITIONAL ||
                    device == PCI_DEVICE_VIRTIO_GPU_MODERN)) {
            pci_note_virtio_gpu(0, dev, 0);
        } else if (vendor == PCI_VENDOR_BOCHS &&
                   device == PCI_DEVICE_BOCHS_VGA) {
            fb_pci_init(0, dev, 0);
        }
    }

    printf("PCI: scan complete, %d virtio-blk device(s), %d virtio-gpu device(s) found\n",
           virtio_pci_count, virtio_gpu_pci_count);
}

#else /* RISC-V / ECAM path */

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
