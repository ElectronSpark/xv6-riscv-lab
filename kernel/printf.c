//
// formatted console output -- printf, panic.
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

volatile int panicked = 0;

int
panic_state(void)
{
  return panicked;
}

// lock to avoid interleaving concurrent printf's.
STATIC struct {
  struct spinlock lock;
  int locking;
} pr;

STATIC char digits[] = "0123456789abcdef";

STATIC void
printint(long long xx, int base, int sign, char *outbuf, int *outlen)
{
  char buf[16];
  int i;
  unsigned long long x;

  if(sign && (sign = (xx < 0)))
    x = -xx;
  else
    x = xx;

  i = 0;
  do {
    buf[i++] = digits[x % base];
  } while((x /= base) != 0);

  if(sign)
    buf[i++] = '-';

  while(--i >= 0)
    outbuf[(*outlen)++] = buf[i];
}

STATIC void
printptr(uint64 x, char *outbuf, int *outlen)
{
  int i;
  outbuf[(*outlen)++] = '0';
  outbuf[(*outlen)++] = 'x';
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    outbuf[(*outlen)++] = digits[x >> (sizeof(uint64) * 8 - 4)];
}

STATIC void
print_padding(int len, char *outbuf, int *outlen)
{
  for (int i = 0; i < len; i++) {
    outbuf[(*outlen)++] = ' ';
  }
}

STATIC void
print_timestamp(char *outbuf, int *outlen)
{
  uint64 stime = r_time();
  outbuf[(*outlen)++] = '[';
  printint(stime, 10, 0, outbuf, outlen);
  outbuf[(*outlen)++] = ']';
  outbuf[(*outlen)++] = ' ';
}

// Print to the console.
int
printf(char *fmt, ...)
{
  va_list ap;
  int i, cx, c0, c1, c2, locking;
  char *s;
  static int nnewline = false;
  char outbuf[512];  // Buffer for batched output
  int outlen = 0;

  locking = pr.locking;
  if(locking)
    spin_lock(&pr.lock);

  if (!__atomic_test_and_set(&nnewline, __ATOMIC_ACQUIRE)) {
    print_timestamp(outbuf, &outlen);
  }

  va_start(ap, fmt);
  for(i = 0; (cx = fmt[i] & 0xff) != 0; i++){
    if(cx != '%'){
      outbuf[outlen++] = cx;
      if (cx == '\n') {
        __atomic_clear(&nnewline, __ATOMIC_RELEASE);
      }
      // Flush if buffer is nearly full
      if(outlen >= 500) {
        consputs(outbuf, outlen);
        outlen = 0;
      }
      continue;
    }
    i++;
    c0 = fmt[i+0] & 0xff;
    c1 = c2 = 0;
    if(c0) c1 = fmt[i+1] & 0xff;
    if(c0 && c1 && c0 == '*' && c1 == 's'){
      int len = va_arg(ap, int);
      print_padding(len, outbuf, &outlen);
      i++;
      c0 = c1;
      c1 = fmt[i+1] & 0xff;
    }
    if(c1) c2 = fmt[i+2] & 0xff;
    if(c0 == 'd'){
      printint(va_arg(ap, int), 10, 1, outbuf, &outlen);
    } else if(c0 == 'l' && c1 == 'd'){
      printint(va_arg(ap, uint64), 10, 1, outbuf, &outlen);
      i += 1;
    } else if(c0 == 'l' && c1 == 'l' && c2 == 'd'){
      printint(va_arg(ap, uint64), 10, 1, outbuf, &outlen);
      i += 2;
    } else if(c0 == 'u'){
      printint(va_arg(ap, int), 10, 0, outbuf, &outlen);
    } else if(c0 == 'l' && c1 == 'u'){
      printint(va_arg(ap, uint64), 10, 0, outbuf, &outlen);
      i += 1;
    } else if(c0 == 'l' && c1 == 'l' && c2 == 'u'){
      printint(va_arg(ap, uint64), 10, 0, outbuf, &outlen);
      i += 2;
    } else if(c0 == 'x'){
      printint(va_arg(ap, int), 16, 0, outbuf, &outlen);
    } else if(c0 == 'l' && c1 == 'x'){
      printint(va_arg(ap, uint64), 16, 0, outbuf, &outlen);
      i += 1;
    } else if(c0 == 'l' && c1 == 'l' && c2 == 'x'){
      printint(va_arg(ap, uint64), 16, 0, outbuf, &outlen);
      i += 2;
    } else if(c0 == 'p'){
      printptr(va_arg(ap, uint64), outbuf, &outlen);
    } else if(c0 == 's'){
      if((s = va_arg(ap, char*)) == 0)
        s = "(null)";
      for(; *s; s++) {
        outbuf[outlen++] = *s;
        // Flush if buffer is nearly full
        if(outlen >= 500) {
          consputs(outbuf, outlen);
          outlen = 0;
        }
      }
    } else if(c0 == '%'){
      outbuf[outlen++] = '%';
    } else if(c0 == 0){
      break;
    } else {
      // Print unknown % sequence to draw attention.
      outbuf[outlen++] = '%';
      outbuf[outlen++] = c0;
    }
    // Flush if buffer is nearly full
    if(outlen >= 500) {
      consputs(outbuf, outlen);
      outlen = 0;
    }
  }
  va_end(ap);

  // Flush remaining buffer
  if(outlen > 0) {
    consputs(outbuf, outlen);
  }

  if(locking)
    spin_unlock(&pr.lock);

  return 0;
}

static int __bt_enabled = 1;

void
panic_disable_bt(void)
{
  __bt_enabled = 0;
}

void
__panic_start()
{
  pr.locking = 0;
  uint64 fp = r_fp();
  struct proc *p = myproc();
  if (p == NULL || p->kstack == 0) {
    printf("unknown back trace context\n");
    return;
  }
  printf("In process %d (%s) at %p\n", p->pid, p->name, (void *)fp);
  if (__bt_enabled) {
    size_t kstack_size = (1UL << (PAGE_SHIFT + p->kstack_order));
    print_backtrace(fp, p->kstack, p->kstack + kstack_size);
  }
}

void 
__panic_end()
{
  panicked = 1; // freeze uart output from other CPUs
  intr_off();
  for(;;) {
    asm volatile("wfi");
  }
}

void
printfinit(void)
{
  spin_init(&pr.lock, "pr");
  pr.locking = 1;
}
