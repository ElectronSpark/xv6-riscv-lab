//
// Console input and output to UART. Reads are line-buffered.
// Special keys: ^H=backspace, ^U=kill line, ^D=EOF, ^P=process list
// Uses SBI for early boot output before UART init.
// Converts \n to \r\n for proper terminal display.
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "lock/spinlock.h"
#include "lock/mutex_types.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "proc/proc.h"
#include "proc/sched.h"
#include "dev/cdev.h"
#include "trap.h"
#include "dev/uart.h"
#include "sbi.h"

#ifndef CONSOLE_MAJOR
#define CONSOLE_MAJOR  1
#endif
#ifndef CONSOLE_MINOR
#define CONSOLE_MINOR  1
#endif

#define BACKSPACE 0x100
#define C(x)  ((x)-'@')  // Control-x

// Flag to track if UART has been initialized
// Before UART init, we use SBI for console output
static volatile int uart_initialized = 0;

//
// send one character to the uart.
// called by printf(), and to echo input characters,
// but not from write().
//
void
consputc(int c)
{
  if(!uart_initialized) {
    // Use SBI for early console output before UART is ready
    if(c == BACKSPACE){
      sbi_console_putchar('\b'); sbi_console_putchar(' '); sbi_console_putchar('\b');
    } else {
      // Convert \n to \r\n for proper terminal output
      if(c == '\n')
        sbi_console_putchar('\r');
      sbi_console_putchar(c);
    }
    return;
  }
  
  // Use synchronous UART output (safe for interrupt context, like xv6-OrangePi_RV2)
  if(c == BACKSPACE){
    // if the user typed backspace, overwrite with a space.
    uartputc_sync('\b'); uartputc_sync(' '); uartputc_sync('\b');
  } else {
    // Convert \n to \r\n for proper terminal output
    if(c == '\n')
      uartputc_sync('\r');
    uartputc_sync(c);
  }
}

//
// send a string to the console.
// for use by puts() and optimized printing.
//
void
consputs(const char *s, int n)
{
  if(!uart_initialized) {
    // Use SBI for early console output before UART is ready
    for(int i = 0; i < n; i++) {
      if(s[i] == BACKSPACE){
        sbi_console_putchar('\b'); sbi_console_putchar(' '); sbi_console_putchar('\b');
      } else {
        // Convert \n to \r\n for proper terminal output
        if(s[i] == '\n')
          sbi_console_putchar('\r');
        sbi_console_putchar(s[i]);
      }
    }
    return;
  }
  
  for(int i = 0; i < n; i++) {
    if(s[i] == BACKSPACE){
      uartputc_sync('\b'); uartputc_sync(' '); uartputc_sync('\b');
    } else {
      // Convert \n to \r\n for proper terminal output
      if(s[i] == '\n')
        uartputc_sync('\r');
      uartputc_sync(s[i]);
    }
  }
}

struct {
  struct spinlock lock;
  
  // input
#define INPUT_BUF_SIZE 128
  char buf[INPUT_BUF_SIZE];
  uint r;  // Read index
  uint w;  // Write index
  uint e;  // Edit index
} cons;

//
// user write()s to the console go here.
//
int
consolewrite(cdev_t *cdev, bool user_src, const void *buffer, size_t n)
{
  int i;
  uint64 src = (uint64)buffer;
  char kbuf[64];
  int written = 0;

  while(written < n) {
    int batch_size = n - written;
    if(batch_size > 64)
      batch_size = 64;
    
    for(i = 0; i < batch_size; i++){
      if(either_copyin(&kbuf[i], user_src, src + written + i, 1) == -1)
        return written + i;
    }
    
    // Output with \n -> \r\n conversion for proper terminal display
    // Use interrupt-driven uartputc for efficiency
    for(i = 0; i < batch_size; i++) {
      if(kbuf[i] == '\n')
        uartputc('\r');
      uartputc(kbuf[i]);
    }
    written += batch_size;
  }

  return written;
}

//
// user read()s from the console go here.
// copy (up to) a whole input line to dst.
// user_dist indicates whether dst is a user
// or kernel address.
//
int
consoleread(cdev_t *cdev, bool user_dst, void *buffer, size_t n)
{
  uint target;
  int c;
  char cbuf;
  uint64 dst = (uint64)buffer;

  target = n;
  spin_lock(&cons.lock);
  while(n > 0){
    // wait until interrupt handler has put some
    // input into cons.buffer.
    while(cons.r == cons.w){
      if(killed(myproc())){
        spin_unlock(&cons.lock);
        return -1;
      }
      sleep_on_chan(&cons.r, &cons.lock);
    }

    c = cons.buf[cons.r++ % INPUT_BUF_SIZE];

    if(c == C('D')){  // end-of-file
      if(n < target){
        // Save ^D for next time, to make sure
        // caller gets a 0-byte result.
        cons.r--;
      }
      break;
    }

    // copy the input byte to the user-space buffer.
    cbuf = c;
    if(either_copyout(user_dst, dst, &cbuf, 1) == -1)
      break;

    dst++;
    --n;

    if(c == '\n'){
      // a whole line has arrived, return to
      // the user-level read().
      break;
    }
  }
  spin_unlock(&cons.lock);

  return target - n;
}

static int consoleopen(cdev_t *cdev) {
    return 0;
}

static int consoleclose(cdev_t *cdev) {
    return 0;
}

static cdev_ops_t console_cdev_ops = {
    .read = consoleread,
    .write = consolewrite,
    .open = consoleopen,
    .release = consoleclose,
};

static cdev_t console_cdev = {
    .dev = {
        .major = CONSOLE_MAJOR,
        .minor = CONSOLE_MINOR,
    },
    .readable = 1,
    .writable = 1,
};

extern void uartintr(int irq, void *data, device_t *dev);

//
// the console input interrupt handler.
// uartintr() calls this for input character.
// do erase/kill processing, append to cons.buf,
// wake up consoleread() if a whole line has arrived.
//
void
consoleintr(int c)
{
  spin_lock(&cons.lock);

  switch(c){
  case C('P'):  // Print process list.
    procdump();
    break;
  case C('U'):  // Kill line.
    while(cons.e != cons.w &&
          cons.buf[(cons.e-1) % INPUT_BUF_SIZE] != '\n'){
      cons.e--;
      consputc(BACKSPACE);
    }
    break;
  case C('H'): // Backspace
  case '\x7f': // Delete key
    if(cons.e != cons.w){
      cons.e--;
      consputc(BACKSPACE);
    }
    break;
  default:
    if(c != 0 && cons.e-cons.r < INPUT_BUF_SIZE){
      c = (c == '\r') ? '\n' : c;

      // echo back to the user.
      if (c == '\x1b')
        consputc('[');  // Escape sequence start
      else if (c == '\t')
        consputc(' ');  // Convert tab to space for simplicity
      else if ((c < 32 || c > 126) && c != '\n')  // Non-printable characters
        consputc('?');  // Replace with '?'
      else
        consputc(c);

      // store for consumption by consoleread().
      cons.buf[cons.e++ % INPUT_BUF_SIZE] = c;

      if(c == '\n' || c == '\t' || c == C('D') || cons.e-cons.r == INPUT_BUF_SIZE){
        // wake up consoleread() if a whole line (or end-of-file)
        // has arrived.
        cons.w = cons.e;
        wakeup_on_chan(&cons.r);
      }
    }
    break;
  }
  
  spin_unlock(&cons.lock);
}

void
consoleinit(void)
{
  spin_init(&cons.lock, "cons");

  // Try to initialize UART hardware
  // Returns 1 if successful (QEMU), 0 if deferred (real hardware uses SBI)
  if (uartinit()) {
    // Mark UART as initialized - switch from SBI to UART output
    uart_initialized = 1;
  }
  // If uartinit returned 0, keep uart_initialized = 0 to continue using SBI
}

// SBI console input polling thread
// On non-QEMU platforms where UART hardware isn't used, we poll SBI for input
static void
sbi_console_poll_thread(uint64 arg1, uint64 arg2)
{
  (void)arg1;
  (void)arg2;
  for (;;) {
    // Read all available characters in a batch
    int got_input = 0;
    for (int i = 0; i < 32; i++) {  // Read up to 32 chars per cycle
      int c = sbi_console_getchar();
      if (c >= 0) {
        consoleintr(c);
        got_input = 1;
      } else {
        break;  // No more input available
      }
    }
    
    if (!got_input) {
      // No input available, sleep briefly to avoid busy-waiting
      // Use 1ms for responsive interactive typing
      sleep_ms(1);
    }
    // If we got input, immediately check for more without sleeping
  }
}

void
consoledevinit(void)
{
  console_cdev.ops = console_cdev_ops;
  int errno = cdev_register(&console_cdev);
  assert(errno == 0, "consoleinit: cdev_register failed: %d\n", errno);
  struct irq_desc uart_irq_desc = {
    .handler = uartintr,
    .data = NULL,
    .dev = &console_cdev.dev,
  };
  errno = register_irq_handler(PLIC_IRQ(UART0_IRQ), &uart_irq_desc);
  assert(errno == 0, "consoledevinit: register_irq_handler failed, error code: %d\n", errno);
  
  // Start SBI polling thread if UART hardware not available
  if (!uart_initialized) {
    struct proc *p = NULL;
    int pid = kernel_proc_create("sbi_console", &p, sbi_console_poll_thread, 0, 0, 0);
    if (pid >= 0 && p != NULL)
      wakeup_proc(p);
  }
}
