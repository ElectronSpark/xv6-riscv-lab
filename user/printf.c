#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"

#include <stdarg.h>

static char digits[] = "0123456789ABCDEF";

// static void
// putc(int fd, char c)
// {
//   write(fd, &c, 1);
// }

static void
puts_n(int fd, const char *s, size_t n)
{
  int idx = 0;
  while (idx < n) {
    int ret = write(fd, &s[idx], n - idx);
    if (ret <= 0) {
      break;
    }
    idx += ret;
  }
}

static void
printint(int fd, int xx, int base, int sgn)
{
  char buf[16];
  int i, neg;
  uint x;

  neg = 0;
  if(sgn && xx < 0){
    neg = 1;
    x = -xx;
  } else {
    x = xx;
  }

  i = 0;
  do{
    buf[i++] = digits[x % base];
  }while((x /= base) != 0);
  if(neg)
    buf[i++] = '-';

  int left = 0;
  int right = i - 1;
  while (left < right) {
    buf[left] ^= buf[right];
    buf[right] ^= buf[left];
    buf[left] ^= buf[right];
    left++;
    right--;
  }
  puts_n(fd, buf, i);
}

static void
printptr(int fd, uint64 x) {
  int i;
  char buf[3 + sizeof(uint64) * 2] = { 0 };
  int idx = 0;
  buf[idx++] = '0';
  buf[idx++] = 'x';
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    buf[idx++] = digits[x >> (sizeof(uint64) * 8 - 4)];
  puts_n(fd, buf, idx);
}

// Print to the given fd. Only understands %d, %x, %p, %s.
void
vprintf(int fd, const char *fmt, va_list ap)
{
  char *s;
  int c0, c1, c2, i, state;
  char buf[128] = { 0 };
  int idx = 0;

  state = 0;
  for(i = 0; fmt[i]; i++){
    if (idx >= 120) {
      puts_n(fd, buf, idx);
      idx = 0;
    }
    c0 = fmt[i] & 0xff;
    if(state == 0){
      if(c0 == '%'){
        state = '%';
      } else {
        buf[idx++] = c0;
      }
    } else if(state == '%'){
      // Flush buffer before printing formatted value
      if (idx > 0) {
        puts_n(fd, buf, idx);
        idx = 0;
      }
      c1 = c2 = 0;
      if(c0) c1 = fmt[i+1] & 0xff;
      if(c1) c2 = fmt[i+2] & 0xff;
      if(c0 == 'd'){
        printint(fd, va_arg(ap, int), 10, 1);
      } else if(c0 == 'l' && c1 == 'd'){
        printint(fd, va_arg(ap, uint64), 10, 1);
        i += 1;
      } else if(c0 == 'l' && c1 == 'l' && c2 == 'd'){
        printint(fd, va_arg(ap, uint64), 10, 1);
        i += 2;
      } else if(c0 == 'o') {
        printint(fd, va_arg(ap, int), 8, 0);
      } else if(c0 == 'l' && c1 == 'o'){
        printint(fd, va_arg(ap, uint64), 8, 0);
        i += 1;
      } else if(c0 == 'l' && c1 == 'l' && c2 == 'o'){
        printint(fd, va_arg(ap, uint64), 8, 0);
        i += 2;
      } else if(c0 == 'u'){
        printint(fd, va_arg(ap, int), 10, 0);
      } else if(c0 == 'l' && c1 == 'u'){
        printint(fd, va_arg(ap, uint64), 10, 0);
        i += 1;
      } else if(c0 == 'l' && c1 == 'l' && c2 == 'u'){
        printint(fd, va_arg(ap, uint64), 10, 0);
        i += 2;
      } else if(c0 == 'x'){
        printint(fd, va_arg(ap, int), 16, 0);
      } else if(c0 == 'l' && c1 == 'x'){
        printint(fd, va_arg(ap, uint64), 16, 0);
        i += 1;
      } else if(c0 == 'l' && c1 == 'l' && c2 == 'x'){
        printint(fd, va_arg(ap, uint64), 16, 0);
        i += 2;
      } else if(c0 == 'p'){
        printptr(fd, va_arg(ap, uint64));
      } else if(c0 == 's'){
        if((s = va_arg(ap, char*)) == 0) {
          if (idx) {
            puts_n(fd, buf, idx);
            idx = 0;
          }
          puts_n(fd, "(null)", 6);
        } else {
          size_t len = strlen(s);
          puts_n(fd, s, len);
        }
      } else if(c0 == '%'){
        buf[idx++] = '%';
      } else {
        // Unknown % sequence.  Print it to draw attention.
        buf[idx++] = '%';
        buf[idx++] = c0;
      }

#if 0
      if(c == 'd'){
        printint(fd, va_arg(ap, int), 10, 1);
      } else if(c == 'l') {
        printint(fd, va_arg(ap, uint64), 10, 0);
      } else if(c == 'x') {
        printint(fd, va_arg(ap, int), 16, 0);
      } else if(c == 'p') {
        printptr(fd, va_arg(ap, uint64));
      } else if(c == 's'){
        s = va_arg(ap, char*);
        if(s == 0)
          s = "(null)";
        while(*s != 0){
          buf[idx++] = *s;
          s++;
        }
      } else if(c == 'c'){
        buf[idx++] = va_arg(ap, uint);
      } else if(c == '%'){
        buf[idx++] = c;
      } else {
        // Unknown % sequence.  Print it to draw attention.
        buf[idx++] = '%';
        buf[idx++] = c;
      }
#endif
      state = 0;
    }
  }
  if (idx) {
    puts_n(fd, buf, idx);
    idx = 0;
  }
}

void
fprintf(int fd, const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vprintf(fd, fmt, ap);
}

void
printf(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vprintf(1, fmt, ap);
}
