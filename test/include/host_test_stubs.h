#ifndef __HOST_TEST_STUBS_H
#define __HOST_TEST_STUBS_H

// Provide missing definitions for host tests that include kernel headers

#ifndef PGSIZE
#define PGSIZE 4096
#endif

#ifndef NCPU
#define NCPU 8
#endif

#ifndef NOFILE
#define NOFILE 64
#endif

// pagetable_t is now defined in riscv.h (which works on host with ON_HOST_OS
// guard) No need to duplicate the definition here

#endif // __HOST_TEST_STUBS_H