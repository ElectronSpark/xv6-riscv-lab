//
// low-level driver routines for 16550a UART.
// Uses batched buffering for improved output performance.
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
#include "virtio.h"
#include "string.h"
#include "freelist.h"

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
#define ISR 2                 // interrupt status register
#define LCR 3                 // line control register
#define LCR_EIGHT_BITS (3<<0)
#define LCR_BAUD_LATCH (1<<7) // special mode to set baud rate
#define LSR 5                 // line status register
#define LSR_RX_READY (1<<0)   // input is waiting to be read from RHR
#define LSR_TX_IDLE (1<<5)    // THR can accept another character to send

#define ReadReg(reg) (*(Reg(reg)))
#define WriteReg(reg, v) (*(Reg(reg)) = (v))

// the transmit output buffer.
struct spinlock uart_tx_lock;
#define UART_TX_BUF_SIZE 4096
char uart_tx_buf[UART_TX_BUF_SIZE] __ALIGNED_PAGE;
uint64 uart_tx_w; // write next to uart_tx_buf[uart_tx_w % UART_TX_BUF_SIZE]
uint64 uart_tx_r; // read next from uart_tx_buf[uart_tx_r % UART_TX_BUF_SIZE]

extern volatile int panicked; // from printf.c

void uartstart();

// UART buffering control
static struct {
  char free[NUM];  // is a buffer free?
  uint16 free_list[NUM];  // The index of the free buffers
  struct freelist buf_freelist;  // Freelist manager for buffers
  int virtio_ready;
  int interrupt_ready;  // Set after interrupt handler is registered
  char tx_buffers[NUM][256];  // Transmit buffers for batching
} uart_virtio;

// find a free buffer, mark it non-free, return its index.
static int
alloc_uart_buffer(void)
{
  spin_acquire(&uart_tx_lock);
  int idx = freelist_alloc(&uart_virtio.buf_freelist);
  spin_release(&uart_tx_lock);
  return idx;
}

// mark a buffer as free.
static void
free_uart_buffer(int i)
{
  spin_acquire(&uart_tx_lock);
  if(freelist_free(&uart_virtio.buf_freelist, i) != 0)
    panic("free_uart_buffer: invalid free");
  
  // Wake up any waiting processes
  wakeup_on_chan(&uart_virtio.free[0]);
  spin_release(&uart_tx_lock);
}

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

  // reset and enable FIFOs.
  WriteReg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);

  // enable transmit and receive interrupts.
  WriteReg(IER, IER_TX_ENABLE | IER_RX_ENABLE);

  spin_init(&uart_tx_lock, "uart");
  
  // Initialize buffering system
  uart_virtio.virtio_ready = 1;  // Enable batched buffering
  uart_virtio.interrupt_ready = 0;  // Start in synchronous mode
  
  // Initialize buffer free list
  freelist_init(&uart_virtio.buf_freelist, uart_virtio.free, uart_virtio.free_list, NUM);
}

// Register interrupt handler for async mode
// Call this after interrupt system is initialized
void
uart_register_interrupt(void)
{
  // Enable interrupt-driven mode for batched buffering
  uart_virtio.interrupt_ready = 1;
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
  spin_release(&uart_tx_lock);
  uartstart();
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

// batch version of uartputc() - write multiple characters at once
void
uartputs(const char *s, int n)
{
  spin_acquire(&uart_tx_lock);

  if(panicked){
    for(;;) {
      asm volatile("wfi");
    }
  }
  
  // Add all characters to the buffer
  for(int i = 0; i < n; i++) {
    while(uart_tx_w == uart_tx_r + UART_TX_BUF_SIZE){
      // buffer is full.
      // wait for uartstart() to open up space in the buffer.
      sleep_on_chan(&uart_tx_r, &uart_tx_lock);
    }
    uart_tx_buf[uart_tx_w % UART_TX_BUF_SIZE] = s[i];
    uart_tx_w += 1;
  }
  
  // Transmit all batched characters
  spin_release(&uart_tx_lock);
  uartstart();
}

// if the UART is idle, and a character is waiting
// in the transmit buffer, send it.
// handles its own locking internally.
// called from both the top- and bottom-half.
void
uartstart()
{
  if(!uart_virtio.interrupt_ready) {
    // Synchronous mode: batch characters into buffer, then transmit
    while(1) {
      spin_acquire(&uart_tx_lock);
      if(uart_tx_w == uart_tx_r) {
        spin_release(&uart_tx_lock);
        break;
      }
      
      // Calculate how many characters to batch
      uint64 count = uart_tx_w - uart_tx_r;
      if(count > 256)
        count = 256;  // Limit batch size
      
      // Copy characters to temporary buffer
      char temp_buf[256];
      for(uint64 i = 0; i < count; i++) {
        temp_buf[i] = uart_tx_buf[uart_tx_r % UART_TX_BUF_SIZE];
        uart_tx_r++;
      }
      
      // Wake up any waiting uartputc() calls
      wakeup_on_chan(&uart_tx_r);
      spin_release(&uart_tx_lock);
      
      // Allocate a free buffer (this acquires its own lock)
      int idx = alloc_uart_buffer();
      
      if(idx < 0) {
        // No free buffers, send directly via UART
        for(uint64 i = 0; i < count; i++) {
          while((ReadReg(LSR) & LSR_TX_IDLE) == 0)
            ;
          WriteReg(THR, temp_buf[i]);
        }
        continue;
      }

      // Copy to the buffer
      char *buf = uart_virtio.tx_buffers[idx];
      for(uint64 i = 0; i < count; i++) {
        buf[i] = temp_buf[i];
      }

      // Transmit the batched characters via UART (without holding lock)
      for(uint64 i = 0; i < count; i++) {
        while((ReadReg(LSR) & LSR_TX_IDLE) == 0)
          ;
        WriteReg(THR, buf[i]);
      }
      
      // Mark buffer as free (this acquires its own lock)
      free_uart_buffer(idx);
    }
    return;
  }

  // Asynchronous mode: batch into buffer and use interrupts
  while(1) {
    spin_acquire(&uart_tx_lock);
    if(uart_tx_w == uart_tx_r) {
      spin_release(&uart_tx_lock);
      break;
    }
    
    // Calculate how many characters to send
    uint64 count = uart_tx_w - uart_tx_r;
    if(count > 256)
      count = 256;  // Limit batch size

    // Copy characters to temporary buffer
    char temp_buf[256];
    for(uint64 i = 0; i < count; i++) {
      temp_buf[i] = uart_tx_buf[uart_tx_r % UART_TX_BUF_SIZE];
      uart_tx_r++;
    }

    // Wake up any waiting uartputc() calls
    wakeup_on_chan(&uart_tx_r);
    spin_release(&uart_tx_lock);
    
    // Allocate a free buffer (this acquires its own lock)
    int idx = alloc_uart_buffer();
    
    if(idx < 0) {
      // No free descriptors, will retry on next interrupt
      return;
    }

    // Copy characters to the buffer for batching
    char *buf = uart_virtio.tx_buffers[idx];
    for(uint64 i = 0; i < count; i++) {
      buf[i] = temp_buf[i];
    }

    // Transmit batch via UART (interrupt will process remaining)
    for(uint64 i = 0; i < count; i++) {
      if((ReadReg(LSR) & LSR_TX_IDLE) == 0){
        // UART not ready, will continue in interrupt handler
        free_uart_buffer(idx);  // Return to free list (this acquires its own lock)
        return;
      }
      WriteReg(THR, buf[i]);
    }
    
    // Transmission complete, free the buffer (this acquires its own lock)
    free_uart_buffer(idx);
  }
}

// read one input character from the UART.
// return -1 if none is waiting.
int
uartgetc(void)
{
  // Input comes from legacy UART registers
  if(ReadReg(LSR) & 0x01){
    // input data is ready.
    return ReadReg(RHR);
  } else {
    return -1;
  }
}

// handle a uart interrupt, raised because input has
// arrived, or the uart is ready for more output, or
// both. called from devintr().
void uartintr(int irq, void *data, device_t *dev)
{
  // read and process incoming characters.
  while(1){
    int c = uartgetc();
    if(c == -1)
      break;
    consoleintr(c);
  }

  // Send buffered characters (interrupt-driven mode)
  // uartstart now handles its own locking internally
  if(uart_virtio.interrupt_ready) {
    uartstart();
  }
}
