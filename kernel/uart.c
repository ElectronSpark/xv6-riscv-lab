//
// low-level driver routines for 16550a UART.
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
// uint64 __uart0_mmio_base = 0xd4017000UL;
// uint64 __uart0_irqno = 42;

void uartintr(int irq, void *data, device_t *dev);

// the UART control registers are memory-mapped
// at address UART0. this macro returns the
// address of one of the registers.
#define Reg(reg) ((volatile unsigned char *)(UART0 + (reg)))

// the UART control registers.
// some have different meanings for
// read vs write.
// see http://byterunner.com/16550.html
#define RHR 0                 // receive holding register (for input bytes)
#define THR 0                 // transmit holding register (for output bytes)
#define IER 1                 // interrupt enable register
#define IER_RX_ENABLE (1<<0)
#define IER_TX_ENABLE (1<<1)
#define FCR 2                 // FIFO control register
#define FCR_FIFO_ENABLE (1<<0)
#define FCR_FIFO_CLEAR (3<<1) // clear the content of the two FIFOs
#define FCR_TRIGGER_1   (0<<6) // RX FIFO trigger level: 1 byte
#define FCR_TRIGGER_4   (1<<6) // RX FIFO trigger level: 4 bytes
#define FCR_TRIGGER_8   (2<<6) // RX FIFO trigger level: 8 bytes
#define FCR_TRIGGER_14  (3<<6) // RX FIFO trigger level: 14 bytes
#define ISR 2                 // interrupt status register
#define LCR 3                 // line control register
#define LCR_EIGHT_BITS (3<<0)
#define LCR_BAUD_LATCH (1<<7) // special mode to set baud rate
#define LSR 5                 // line status register
#define LSR_RX_READY (1<<0)   // input is waiting to be read from RHR
#define LSR_TX_IDLE (1<<5)    // THR can accept another character to send

// 16550A FIFO size
#define UART_FIFO_SIZE 16

#define ReadReg(reg) (*(Reg(reg)))
#define WriteReg(reg, v) (*(Reg(reg)) = (v))

// the transmit output buffer.
struct spinlock uart_tx_lock = SPINLOCK_INITIALIZED("uart_tx_lock");
#define UART_TX_BUF_SIZE 128
char uart_tx_buf[UART_TX_BUF_SIZE];
uint64 uart_tx_w; // write next to uart_tx_buf[uart_tx_w % UART_TX_BUF_SIZE]
uint64 uart_tx_r; // read next from uart_tx_buf[uart_tx_r % UART_TX_BUF_SIZE]

// the receive input buffer.
struct spinlock uart_rx_lock = SPINLOCK_INITIALIZED("uart_rx_lock");
#define UART_RX_BUF_SIZE 128
char uart_rx_buf[UART_RX_BUF_SIZE];
uint64 uart_rx_w; // write next to uart_rx_buf[uart_rx_w % UART_RX_BUF_SIZE]
uint64 uart_rx_r; // read next from uart_rx_buf[uart_rx_r % UART_RX_BUF_SIZE]

extern volatile int panicked; // from printf.c

void uartstart();
static void uartrecv(void);

void
uartinit(void)
{
  // disable interrupts.
  WriteReg(IER, 0x00);

  // special mode to set baud rate.
  WriteReg(LCR, LCR_BAUD_LATCH);

  // LSB for baud rate of 38.4K.
  WriteReg(0, 0x03);

  // MSB for baud rate of 38.4K.
  WriteReg(1, 0x00);

  // leave set-baud mode,
  // and set word length to 8 bits, no parity.
  WriteReg(LCR, LCR_EIGHT_BITS);

  // reset and enable FIFOs with RX trigger level at 8 bytes.
  // This reduces interrupt frequency while maintaining responsiveness.
  WriteReg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR | FCR_TRIGGER_8);

  // enable transmit and receive interrupts.
  WriteReg(IER, IER_TX_ENABLE | IER_RX_ENABLE);
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
  spin_acquire(&uart_tx_lock);

  if(panicked){
    for(;;) {
      asm volatile("wfi");
    }
  }
  while(uart_tx_w == uart_tx_r + UART_TX_BUF_SIZE){
    // buffer is full.
    // wait for uartstart() to open up space in the buffer.
    sleep_on_chan(&uart_tx_r, &uart_tx_lock);
  }
  uart_tx_buf[uart_tx_w % UART_TX_BUF_SIZE] = c;
  uart_tx_w += 1;
  uartstart();
  spin_release(&uart_tx_lock);
}

// batch version of uartputc() - write multiple characters at once.
// blocks if the output buffer is full.
// because it may block, it can't be called from interrupts.
void
uartputs(const char *s, int n)
{
  spin_acquire(&uart_tx_lock);

  if(panicked){
    for(;;) {
      asm volatile("wfi");
    }
  }
  
  for(int i = 0; i < n; i++) {
    while(uart_tx_w == uart_tx_r + UART_TX_BUF_SIZE){
      // buffer is full.
      // wait for uartstart() to open up space in the buffer.
      sleep_on_chan(&uart_tx_r, &uart_tx_lock);
    }
    uart_tx_buf[uart_tx_w % UART_TX_BUF_SIZE] = s[i];
    uart_tx_w += 1;
  }
  
  uartstart();
  spin_release(&uart_tx_lock);
}

// alternate version of uartputc() that doesn't 
// use interrupts, for use by kernel printf() and
// to echo characters. it spins waiting for the uart's
// output register to be empty.
void
uartputc_sync(int c)
{
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
  // Check if UART TX FIFO is ready (THR empty)
  if((ReadReg(LSR) & LSR_TX_IDLE) == 0){
    // the UART transmit holding register is full,
    // it will interrupt when it's ready for more bytes.
    return;
  }

  // Fill the TX FIFO with up to UART_FIFO_SIZE bytes
  int sent = 0;
  while(sent < UART_FIFO_SIZE){
    if(uart_tx_w == uart_tx_r){
      // transmit buffer is empty.
      break;
    }
    
    int c = uart_tx_buf[uart_tx_r % UART_TX_BUF_SIZE];
    uart_tx_r += 1;
    sent++;
    
    WriteReg(THR, c);
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
  // Read all available bytes from the hardware RX FIFO
  while(ReadReg(LSR) & LSR_RX_READY){
    if(uart_rx_w == uart_rx_r + UART_RX_BUF_SIZE){
      // software buffer is full, discard incoming data
      ReadReg(RHR);  // discard
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
  spin_acquire(&uart_rx_lock);
  
  // First drain any new data from hardware FIFO
  uartrecv();
  
  if(uart_rx_r != uart_rx_w){
    c = uart_rx_buf[uart_rx_r % UART_RX_BUF_SIZE];
    uart_rx_r += 1;
  }
  
  spin_release(&uart_rx_lock);
  return c;
}

// batch read from the UART.
// reads up to n characters into buf.
// returns the number of characters read.
int
uartgets(char *buf, int n)
{
  int i = 0;
  spin_acquire(&uart_rx_lock);
  
  // First drain hardware FIFO into software buffer
  uartrecv();
  
  // Then read from software buffer
  while(i < n && uart_rx_r != uart_rx_w){
    buf[i++] = uart_rx_buf[uart_rx_r % UART_RX_BUF_SIZE];
    uart_rx_r += 1;
  }
  
  spin_release(&uart_rx_lock);
  return i;
}

// handle a uart interrupt, raised because input has
// arrived, or the uart is ready for more output, or
// both. called from devintr().
void uartintr(int irq, void *data, device_t *dev)
{
  // Drain hardware RX FIFO into software buffer and process
  spin_acquire(&uart_rx_lock);
  uartrecv();
  
  // Process all buffered input
  while(uart_rx_r != uart_rx_w){
    int c = uart_rx_buf[uart_rx_r % UART_RX_BUF_SIZE];
    uart_rx_r += 1;
    spin_release(&uart_rx_lock);
    consoleintr(c);
    spin_acquire(&uart_rx_lock);
  }
  spin_release(&uart_rx_lock);

  // send buffered characters.
  spin_acquire(&uart_tx_lock);
  uartstart();
  spin_release(&uart_tx_lock);
}
