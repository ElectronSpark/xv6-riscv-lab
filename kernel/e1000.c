#include "compiler.h"
#include "types.h"
#include "string.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc/proc.h"
#include "defs.h"
#include "printf.h"
#include "e1000_dev.h"
#include "net.h"
#include "trap.h"

uint64 __e1000_pci_mmio_base = 0x40000000L;
uint64 __e1000_pci_irqno = 33;

static void e1000_intr(int irq, void *data, device_t *dev);

#define TX_RING_SIZE 16
STATIC struct tx_desc tx_ring[TX_RING_SIZE] __ALIGNED(16);
STATIC struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
STATIC struct rx_desc rx_ring[RX_RING_SIZE] __ALIGNED(16);
STATIC struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
STATIC volatile uint32 *regs;

struct spinlock e1000_lock = SPINLOCK_INITIALIZED("e1000_lock");

// Full reset the device
// Called by e1000_init()
// When calling this function, register base variable should
// be initialized.
void
e1000_dev_reset(void)
{
  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

// Set Receive MAC address
// Pass an array of 6 bytes as MAC address, then add it to the E1000's address 
// filter table
// Args:
//  mac:
//  - MAC address in bytes array
//  as:
//  - Address Type:
//    - 00b: Destination address (required for normal mode)
//    - 01b: Source address
//    - otherwise: invalid
//  valid:
//  - The validity of the address:
//    - true: The MAC address is valid
//    - false: The MAC addres is invalid
// return 0 if success, return -1 if err
int
e1000_set_rcvaddr(const uint8 mac[], uint8 as, int valid, int index)
{
  volatile uint32 l;
  volatile uint32 h;

  if (index >= 16 || index < 0) {
    return -1; // the receive address array of e1000 should be less than 16
  }
  l = *(uint32 *)mac;
  h = (uint32)(*(uint16 *)&mac[4]) | (as << 16);
  if (valid) {
    h |= (1<<31);
  }
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  regs[E1000_RA + 2*index] = l;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  regs[E1000_RA + 2*index + 1] = h;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  return 0;
}

// Initialize the transmit descriptor circular buffer
// Args:
//  virtual_base:
//  - The virtual base address of the transmit descriptor circular buffer
//    virtual_base may be different from physical_base due to the Operating
//    System's virtual memory mapping. But they should point to the same
//    physical address.
//  physical_base:
//  - The physical base address of the transmit descriptor circular buffer
//    Must be 16 Bytes aligned
//  mbufs_ptr_arr_base:
//  - A pointer array pointing to transmission content buffer
//    The length of mbufs must equals to the length of TD circular buffer
//  size:
//  - The size of TD circular buffer in bytes.
//    Must be a multiple of 128 bytes
// return 0 if success, return -1 if err
int
e1000_set_transmission_descriptor_base( \
  struct tx_desc *virtual_base,         \
  uint64 physical_base,                 \
  struct mbuf **mbufs_ptr_arr_base,     \
  uint32 size)
{
  volatile uint32 l;
  volatile uint32 h;
  int rd_arr_size = size / sizeof(struct tx_desc);
  
  if (physical_base == 0 || virtual_base == 0) {
    return -1;  // null address
  }
  if (physical_base & 15) {
    return -1;  // TD base address must be 16 Bytes aligned
  }
  if (size % 128 != 0) {
    return -1;  // The size of TA must be a multiple of 128 bytes
  }

  memset(virtual_base, 0, size);
  for (int i = 0; i < rd_arr_size; i++) {
    virtual_base[i].status = E1000_TXD_STAT_DD;
    mbufs_ptr_arr_base[i] = 0;
  }

  l = (physical_base << 32) >> 32;
  h = physical_base >> 32;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  regs[E1000_TDBAL] = l;
  regs[E1000_TDBAH] = h;
  regs[E1000_TDLEN] = size;
  regs[E1000_TDH] = 0;
  regs[E1000_TDT] = 0;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  return 0;
}

// Initialize the receive descriptor circular buffer
// Args:
//  virtual_base:
//  - The virtual base address of the receive descriptor circular buffer
//    virtual_base may be different from physical_base due to the Operating
//    System's virtual memory mapping. But they should point to the same
//    physical address.
//  physical_base:
//  - The physical base address of the receive descriptor circular buffer
//    Must be 16 Bytes aligned
//  mbufs_ptr_arr_base:
//  - A pointer array pointing to receive content buffer
//    The length of mbufs must equals to the length of RD circular buffer
//  size:
//  - The size of RD circular buffer in bytes.
//    Must be a multiple of 128 bytes
// return 0 if success, return -1 if err
int
e1000_set_receive_descriptor_base(  \
  struct rx_desc *virtual_base,     \
  uint64 physical_base,             \
  struct mbuf **mbufs_ptr_arr_base, \
  uint32 size)
{
  volatile uint32 l;
  volatile uint32 h;
  int rd_arr_size = size / sizeof(struct rx_desc);
  
  if (physical_base == 0 || virtual_base == 0) {
    return -1;  // null address
  }
  if (physical_base & 15) {
    return -1;  // TD base address must be 16 Bytes aligned
  }
  if (size % 128 != 0) {
    return -1;  // The size of TA must be a multiple of 128 bytes
  }

  l = (physical_base << 32) >> 32;
  h = physical_base >> 32;
  memset(virtual_base, 0, size);
  for (int i = 0; i < rd_arr_size; i++) {
    mbufs_ptr_arr_base[i] = mbufalloc(0);
    if (!mbufs_ptr_arr_base[i])
      return -1;
    virtual_base[i].addr = (uint64) mbufs_ptr_arr_base[i]->head;
  }
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  regs[E1000_RDBAL] = l;
  regs[E1000_RDBAH] = h;
  regs[E1000_RDLEN] = size;
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = rd_arr_size - 1;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  return 0;

}

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  uint8 default_mac_address[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

  regs = xregs;
  struct irq_desc e1000_irq_desc = {
    .handler = e1000_intr,
    .data = NULL,
    .dev = NULL,
  };
  int ret = register_irq_handler(PLIC_IRQ(E1000_IRQ), &e1000_irq_desc);
  assert(ret == 0, "e1000_init: failed to register irq handler");

  // Reset the device
  e1000_dev_reset();

  // [E1000 14.5] Transmit initialization
  if (e1000_set_transmission_descriptor_base(tx_ring, \
      (uint64)tx_ring, tx_mbufs, sizeof(tx_ring)) != 0) {
    panic("e1000");
  }

  // [E1000 14.4] Receive initialization
  if (e1000_set_receive_descriptor_base(rx_ring,  \
      (uint64)rx_ring, rx_mbufs, sizeof(rx_ring)) != 0) {
    panic("e1000");
  }

  // filter by qemu's MAC address, 52:54:00:12:34:56
  if (e1000_set_rcvaddr(default_mac_address, 0, 1, 0) != 0) {
    panic("e1000_init: MAC address");
  }
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x0F << E1000_TCTL_CT_SHIFT) |   // maximum number of times of retransmission in collision
    (0x40 << E1000_TCTL_COLD_SHIFT);  // collision distance
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC

  // ask e1000 for receive interrupts.
  // Instead of RDTR and RADV, using the Interrupt Throttling Register (ITR)
  // if needs interrupt delay.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

int
e1000_transmit(struct mbuf *m)
{
  //
  // Your code here.
  //
  // the mbuf contains an ethernet frame; program it into
  // the TX descriptor ring so that the e1000 sends it. Stash
  // a pointer so that it can be freed after sending.
  //
  spin_lock(&e1000_lock);
  // get the current tail pointer of the transmission ring buffer
  uint32 index = regs[E1000_TDT];
  if(index > TX_RING_SIZE) {
    panic("e1000 transmit: ring overflow");
  }
  struct tx_desc *desc = tx_ring + index;
  if(!(desc->status & E1000_TXD_STAT_DD)){
    // if the descriptor is not finished, return error
    spin_unlock(&e1000_lock);
    return -1;
  }
  if(tx_mbufs[index]){
    // free the buffer containing the already transmitted data
    mbuffree(tx_mbufs[index]);
  }
  // pass packet information into transmission discriptor
  desc->addr = (uint64)m->head;
  desc->length = m->len;
  // - let the Ethernet controller report the status information of this
  //   packet, so that the next we could check if the data of this descriptor
  //   had finished transmission.
  // - tell the Ethernet controller that this is the last descriptor making up
  //   this packet.
  desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
  tx_mbufs[index] = m;
  // move forward the tail pointer of the transmission ring buffer
  regs[E1000_TDT] = (regs[E1000_TDT] + 1) % TX_RING_SIZE;
  spin_unlock(&e1000_lock);
  return 0;
}

STATIC void
e1000_recv(void)
{
  //
  // Your code here.
  //
  // Check for packets that have arrived from the e1000
  // Create and deliver an mbuf for each packet (using net_rx()).
  //
  while(1){
    uint32 index = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
    struct rx_desc *desc = rx_ring + index;
    if(!(desc->status & E1000_RXD_STAT_DD)){
      // keep processing RX Descriptor until meeting an unfinished one.
      return;
    }
    struct mbuf *buf = rx_mbufs[index];
    buf->len = desc->length;
    // Start processing the received packet.
    net_rx(buf);
    // allocate a new buffer
    struct mbuf *newbuf = mbufalloc(0);
    // let the current descriptor point to the newly allocated buffer
    rx_mbufs[index] = newbuf;
    desc->addr = (uint64)newbuf->head;
    desc->status = 0;
    // tell the Ethernet controller that we have finished processing
    // this packet.
    regs[E1000_RDT] = index;
  }
}

static void e1000_intr(int irq, void *data, device_t *dev)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
