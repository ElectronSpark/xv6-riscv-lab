#ifndef __KERNEL_PIPE_H
#define __KERNEL_PIPE_H

#include "spinlock.h"

#define PIPESIZE 512

struct pipe {
  struct spinlock lock;
  uint nread;     // number of bytes read
  uint nwrite;    // number of bytes written
  int readopen: 1;   // read fd is still open
  int writeopen: 1;  // write fd is still open
  char data[PIPESIZE];
};

#endif // __KERNEL_PIPE_H
