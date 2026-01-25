//
// 16550A UART driver with PXA UART support (SpacemiT K1 / Orange Pi RV2).
//
// PXA UART differences from standard 16550A:
//   - reg-shift=2, reg-io-width=4 (4-byte spacing, 32-bit access)
//   - 64-byte FIFO (vs 16-byte), requires IER_UUE (0x40) to enable
//   - MCR_OUT2 (0x08) required for interrupt routing to PLIC
//

#include "compiler.h"
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc/proc.h"
#include "defs.h"
#include "printf.h"
#include "proc/sched.h"
#include "trap.h"
#include "uart.h"

uint64 __uart0_mmio_base = 0x10000000L;
uint64 __uart0_irqno = 10;
uint32 __uart0_clock = 0;  // 0 = use default (assume 1.8432 MHz for QEMU)
uint32 __uart0_baud = 0;   // 0 = use default 115200
uint32 __uart0_reg_shift = 0;  // 0 = 1-byte spacing, 2 = 4-byte spacing (common on SoCs)
uint32 __uart0_reg_io_width = 1;  // 1 = 8-bit access, 4 = 32-bit access (PXA UART)

void uartintr(int irq, void *data, device_t *dev);

// Register access macros with configurable spacing and width
#define Reg8(reg) ((volatile unsigned char *)(UART0 + ((reg) << __uart0_reg_shift)))
#define Reg32(reg) ((volatile uint32 *)(UART0 + ((reg) << __uart0_reg_shift)))

// 16550A/PXA UART registers
#define RHR 0                 // receive holding register
#define THR 0                 // transmit holding register
#define IER 1                 // interrupt enable register
#define IER_RX_ENABLE (1<<0)
#define IER_TX_ENABLE (1<<1)
#define IER_RTOIE     (1<<4)  // PXA: receiver timeout
#define IER_UUE       (1<<6)  // PXA: unit enable
#define FCR 2                 // FIFO control register
#define FCR_FIFO_ENABLE (1<<0)
#define FCR_FIFO_CLEAR (3<<1)
#define FCR_TRIGGER_1   (0<<6)
#define FCR_TRIGGER_8   (2<<6)
#define ISR 2
#define IIR 2
#define LCR 3
#define LCR_EIGHT_BITS (3<<0)
#define LCR_BAUD_LATCH (1<<7)
#define MCR 4
#define MCR_DTR  (1<<0)
#define MCR_RTS  (1<<1)
#define MCR_OUT2 (1<<3)       // Required for PLIC interrupt routing
#define LSR 5
#define LSR_RX_READY (1<<0)
#define LSR_TX_IDLE (1<<5)
#define MSR 6

#define UART_FIFO_SIZE ((__uart0_reg_io_width == 4) ? 64 : 16)

static inline uint32 ReadReg(int reg) {
  return (__uart0_reg_io_width == 4) ? *Reg32(reg) : *Reg8(reg);
}

static inline void WriteReg(int reg, uint32 v) {
  if (__uart0_reg_io_width == 4)
    *Reg32(reg) = v;
  else
    *Reg8(reg) = (unsigned char)v;
}

// TX/RX buffers
struct spinlock uart_tx_lock = SPINLOCK_INITIALIZED("uart_tx_lock");
#define UART_TX_BUF_SIZE 128
char uart_tx_buf[UART_TX_BUF_SIZE];
uint64 uart_tx_w;
uint64 uart_tx_r;

static uint32 uart_ier = 0;  // Current IER value for dynamic TX interrupt

struct spinlock uart_rx_lock = SPINLOCK_INITIALIZED("uart_rx_lock");
#define UART_RX_BUF_SIZE 128
char uart_rx_buf[UART_RX_BUF_SIZE];
uint64 uart_rx_w;
uint64 uart_rx_r;

extern volatile int panicked;

void uartstart();
static void uartrecv(void);

// UART bring-up rationale (PXA + 16550) and prior failure modes:
// 1) IER=0: stop IRQs while changing FIFO/LCR/MCR to avoid spurious interrupts.
// 2) FIFO reset (enable->clear->disable): flush stale RX/TX state; mirrors Linux PXA flow.
// 3) Read LSR/RHR/IIR/MSR: drains latched status so later enables do not fire immediately.
// 4) LCR=8N1: console framing expected by boot ROM/host; keeps parity/stop bits default.
// 5) MCR sets DTR/RTS and OUT2: OUT2 is required on PXA to wire the IRQ line into the PLIC; without OUT2 we previously saw no UART interrupts and a stuck TX path.
// 6) FCR trigger: PXA (64-byte FIFO) uses 8-byte to cut interrupt rate; 16550 (16-byte) uses 1-byte for latency. Earlier, forcing 8-byte on 16550 caused sluggish echo.
// 7) Read status again after FIFO re-enable: ensure no pending conditions remain.
// 8) IER: enable RX; on PXA also RTOIE (RX timeout) plus UUE to power the block; TX is toggled dynamically when data exists.
// Historical missteps: using UART5 (unaligned base) broke MMIO mapping; missing reg-shift/reg-io-width led to bad register offsets; omitting UUE left the PXA UART inert; omitting \r before \n caused right-shifted terminal lines.
int
uartinit(void)
{
  // Disable interrupts to avoid spurious IRQs while reprogramming
  WriteReg(IER, 0x00);
  
  // Reset FIFOs: enable, flush both, then disable (Linux PXA sequence)
  WriteReg(FCR, FCR_FIFO_ENABLE);
  WriteReg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);
  WriteReg(FCR, 0);
  
  // Drain latched status so new config starts clean
  (void)ReadReg(LSR);
  (void)ReadReg(RHR);
  (void)ReadReg(IIR);
  (void)ReadReg(MSR);
  
  // 8N1 framing; OUT2 required on PXA for IRQ line to reach PLIC
  WriteReg(LCR, LCR_EIGHT_BITS);
  WriteReg(MCR, MCR_DTR | MCR_RTS | MCR_OUT2);
  
  // RX trigger: 8-byte on PXA (64-byte FIFO to cut IRQ rate), 1-byte on 16550 for responsiveness
  if (__uart0_reg_io_width == 4)
    WriteReg(FCR, FCR_FIFO_ENABLE | FCR_TRIGGER_8);
  else
    WriteReg(FCR, FCR_FIFO_ENABLE | FCR_TRIGGER_1);
  
  // Clear status again after re-enabling FIFO
  (void)ReadReg(LSR);
  (void)ReadReg(RHR);
  (void)ReadReg(IIR);
  (void)ReadReg(MSR);
  
  // Enable RX interrupts; PXA also needs RTOIE for RX timeout and UUE to power the block
  if (__uart0_reg_io_width == 4) {
    uart_ier = IER_RX_ENABLE | IER_RTOIE | IER_UUE;
  } else {
    uart_ier = IER_RX_ENABLE;
  }
  WriteReg(IER, uart_ier);

  return 1;  // Success - UART is now initialized
}

// add a character to the output buffer and tell the
// UART to start sending if it isn't already.
// blocks if the output buffer is full.
// because it may block, it can't be called
// from interrupts; it's only suitable for use
// by write().
void
uartputc(int c)
{
  // TX path is lock-protected and can sleep; not IRQ-safe
  spin_lock(&uart_tx_lock);

  if(panicked){
    for(;;) {
      asm volatile("wfi");
    }
  }
  while(uart_tx_w == uart_tx_r + UART_TX_BUF_SIZE){
    // Buffer full: sleep until uartstart() frees space
    sleep_on_chan(&uart_tx_r, &uart_tx_lock);
  }
  uart_tx_buf[uart_tx_w % UART_TX_BUF_SIZE] = c;
  uart_tx_w += 1;
  uartstart();
  spin_unlock(&uart_tx_lock);
}

// batch version of uartputc() - write multiple characters at once.
// blocks if the output buffer is full.
// because it may block, it can't be called from interrupts.
void
uartputs(const char *s, int n)
{
  // Bulk TX; same locking/sleeping semantics as uartputc
  spin_lock(&uart_tx_lock);

  if(panicked){
    for(;;) {
      asm volatile("wfi");
    }
  }
  
  for(int i = 0; i < n; i++) {
    while(uart_tx_w == uart_tx_r + UART_TX_BUF_SIZE){
      // Buffer full: sleep until uartstart() frees space
      sleep_on_chan(&uart_tx_r, &uart_tx_lock);
    }
    uart_tx_buf[uart_tx_w % UART_TX_BUF_SIZE] = s[i];
    uart_tx_w += 1;
  }
  
  uartstart();
  spin_unlock(&uart_tx_lock);
}

// alternate version of uartputc() that doesn't 
// use interrupts, for use by kernel printf() and
// to echo characters. it spins waiting for the uart's
// output register to be empty.
void
uartputc_sync(int c)
{
  // IRQ-safe, spin-waits on hardware THR empty; used by early printf/echo
  push_off();

  if(panicked){
    for(;;) {
      asm volatile("wfi");
    }
  }

  // wait for Transmit Holding Empty to be set in LSR.
  while((ReadReg(LSR) & LSR_TX_IDLE) == 0)
    ;
  WriteReg(THR, c);

  pop_off();
}

// if the UART is idle, and a character is waiting
// in the transmit buffer, send it.
// caller must hold uart_tx_lock.
// called from both the top- and bottom-half.
void
uartstart()
{
  // Caller holds uart_tx_lock; services TX in top- or bottom-half
  // Check if UART TX FIFO is ready (THR empty)
  if((ReadReg(LSR) & LSR_TX_IDLE) == 0){
    // the UART transmit holding register is full,
    // enable TX interrupt so we get notified when ready
    if(uart_tx_w != uart_tx_r && !(uart_ier & IER_TX_ENABLE)) {
      uart_ier |= IER_TX_ENABLE;
      WriteReg(IER, uart_ier);
    }
    return;
  }

  // Fill TX FIFO with up to half the FIFO size
  int max_batch = UART_FIFO_SIZE / 2;
  int sent = 0;
  
  while(sent < max_batch){
    if(uart_tx_w == uart_tx_r){
      // transmit buffer is empty.
      break;
    }
    
    int c = uart_tx_buf[uart_tx_r % UART_TX_BUF_SIZE];
    uart_tx_r += 1;
    sent++;
    
    WriteReg(THR, c);
  }
  
  // Enable or disable TX interrupt based on whether more data is pending
  if(uart_tx_w != uart_tx_r) {
    // More data to send - enable TX interrupt
    if(!(uart_ier & IER_TX_ENABLE)) {
      uart_ier |= IER_TX_ENABLE;
      WriteReg(IER, uart_ier);
    }
  } else {
    // Buffer empty - disable TX interrupt to avoid spurious interrupts
    if(uart_ier & IER_TX_ENABLE) {
      uart_ier &= ~IER_TX_ENABLE;
      WriteReg(IER, uart_ier);
    }
  }
  
  // maybe uartputc() is waiting for space in the buffer.
  if(sent > 0){
    wakeup_on_chan(&uart_tx_r);
  }
}

// Drain the hardware RX FIFO into the software buffer.
// caller must hold uart_rx_lock.
static void
uartrecv(void)
{
  // Drain hardware RX FIFO into software buffer while space remains
  // Read all available bytes from the hardware RX FIFO
  while(ReadReg(LSR) & LSR_RX_READY){
    if(uart_rx_w == uart_rx_r + UART_RX_BUF_SIZE){
      // SW buffer full: drop byte (console path is best-effort)
      ReadReg(RHR);  // discard to advance HW FIFO
      continue;
    }
    uart_rx_buf[uart_rx_w % UART_RX_BUF_SIZE] = ReadReg(RHR);
    uart_rx_w += 1;
  }
}

// read one input character from the UART.
// return -1 if none is waiting.
int
uartgetc(void)
{
  int c = -1;
  spin_lock(&uart_rx_lock);
  
  // First drain any new data from hardware FIFO
  uartrecv();
  
  if(uart_rx_r != uart_rx_w){
    c = uart_rx_buf[uart_rx_r % UART_RX_BUF_SIZE];
    uart_rx_r += 1;
  }
  
  spin_unlock(&uart_rx_lock);
  return c;
}

// batch read from the UART.
// reads up to n characters into buf.
// returns the number of characters read.
int
uartgets(char *buf, int n)
{
  int i = 0;
  spin_lock(&uart_rx_lock);
  
  // First drain hardware FIFO into software buffer
  uartrecv();
  
  // Then read from software buffer
  while(i < n && uart_rx_r != uart_rx_w){
    buf[i++] = uart_rx_buf[uart_rx_r % UART_RX_BUF_SIZE];
    uart_rx_r += 1;
  }
  
  spin_unlock(&uart_rx_lock);
  return i;
}

// handle a uart interrupt, raised because input has
// arrived, or the uart is ready for more output, or
// both. called from devintr().
void uartintr(int irq, void *data, device_t *dev)
{
  // Drain hardware RX FIFO into software buffer and process
  spin_lock(&uart_rx_lock);
  uartrecv();
  
  // Process all buffered input
  while(uart_rx_r != uart_rx_w){
    int c = uart_rx_buf[uart_rx_r % UART_RX_BUF_SIZE];
    uart_rx_r += 1;
    spin_unlock(&uart_rx_lock);
    consoleintr(c);
    spin_lock(&uart_rx_lock);
  }
  spin_unlock(&uart_rx_lock);

  // send buffered characters.
  spin_lock(&uart_tx_lock);
  uartstart();
  spin_unlock(&uart_tx_lock);
}
