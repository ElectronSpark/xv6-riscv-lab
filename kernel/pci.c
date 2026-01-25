//
// simple PCI-Express initialization, only
// works for qemu and its e1000 card.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "defs.h"
#include "printf.h"
#include "dev/pci.h"

uint64 __pcie_ecam_mmio_base = 0x30000000L;

void pci_init() {
    // we'll place the e1000 registers at this address.
    // vm.c maps this range.
    uint64 e1000_regs = 0x40000000L;

    // qemu -machine virt puts PCIe config space here.
    // vm.c maps this range.
    uint32 *ecam = (uint32 *)0x30000000L;

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

            e1000_init((uint32 *)e1000_regs);
        }
    }
}
