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

#ifndef pagetable_t
typedef void* pagetable_t;
#endif

#endif // __HOST_TEST_STUBS_H