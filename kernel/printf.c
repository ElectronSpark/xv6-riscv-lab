//
// formatted console output -- printf, panic.
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

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
printint(long long xx, int base, int sign)
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
    consputc(buf[i]);
}

STATIC void
printptr(uint64 x)
{
  int i;
  consputc('0');
  consputc('x');
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    consputc(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

STATIC void
print_padding(int len)
{
  for (int i = 0; i < len; i++) {
    consputc(' ');
  }
}

// Print to the console.
int
printf(char *fmt, ...)
{
  va_list ap;
  int i, cx, c0, c1, c2, locking;
  char *s;

  locking = pr.locking;
  if(locking)
    spin_acquire(&pr.lock);

  va_start(ap, fmt);
  for(i = 0; (cx = fmt[i] & 0xff) != 0; i++){
    if(cx != '%'){
      consputc(cx);
      continue;
    }
    i++;
    c0 = fmt[i+0] & 0xff;
    c1 = c2 = 0;
    if(c0) c1 = fmt[i+1] & 0xff;
    if(c0 && c1 && c0 == '*' && c1 == 's'){
      int len = va_arg(ap, int);
      print_padding(len);
      i++;
      c0 = c1;
      c1 = fmt[i+1] & 0xff;
    }
    if(c1) c2 = fmt[i+2] & 0xff;
    if(c0 == 'd'){
      printint(va_arg(ap, int), 10, 1);
    } else if(c0 == 'l' && c1 == 'd'){
      printint(va_arg(ap, uint64), 10, 1);
      i += 1;
    } else if(c0 == 'l' && c1 == 'l' && c2 == 'd'){
      printint(va_arg(ap, uint64), 10, 1);
      i += 2;
    } else if(c0 == 'u'){
      printint(va_arg(ap, int), 10, 0);
    } else if(c0 == 'l' && c1 == 'u'){
      printint(va_arg(ap, uint64), 10, 0);
      i += 1;
    } else if(c0 == 'l' && c1 == 'l' && c2 == 'u'){
      printint(va_arg(ap, uint64), 10, 0);
      i += 2;
    } else if(c0 == 'x'){
      printint(va_arg(ap, int), 16, 0);
    } else if(c0 == 'l' && c1 == 'x'){
      printint(va_arg(ap, uint64), 16, 0);
      i += 1;
    } else if(c0 == 'l' && c1 == 'l' && c2 == 'x'){
      printint(va_arg(ap, uint64), 16, 0);
      i += 2;
    } else if(c0 == 'p'){
      printptr(va_arg(ap, uint64));
    } else if(c0 == 's'){
      if((s = va_arg(ap, char*)) == 0)
        s = "(null)";
      for(; *s; s++)
        consputc(*s);
    } else if(c0 == '%'){
      consputc('%');
    } else if(c0 == 0){
      break;
    } else {
      // Print unknown % sequence to draw attention.
      consputc('%');
      consputc(c0);
    }

#if 0
    switch(c){
    case 'd':
      printint(va_arg(ap, int), 10, 1);
      break;
    case 'x':
      printint(va_arg(ap, int), 16, 1);
      break;
    case 'p':
      printptr(va_arg(ap, uint64));
      break;
    case 's':
      if((s = va_arg(ap, char*)) == 0)
        s = "(null)";
      for(; *s; s++)
        consputc(*s);
      break;
    case '%':
      consputc('%');
      break;
    default:
      // Print unknown % sequence to draw attention.
      consputc('%');
      consputc(c);
      break;
    }
#endif
  }
  va_end(ap);

  if(locking)
    spin_release(&pr.lock);

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
    print_backtrace(fp, p->kstack, p->kstack + KERNEL_STACK_SIZE);
  }
}

void
__panic_end()
{
  panicked = 1; // freeze uart output from other CPUs
  for(;;);
}

void
printfinit(void)
{
  spin_init(&pr.lock, "pr");
  pr.locking = 1;
}
