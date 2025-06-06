#ifndef __UT_MOCK_WRAPS_H__
#define __UT_MOCK_WRAPS_H__

#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include "cmocka.h"

#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "page.h"
#include "page_private.h"

// Mock function declarations to prevent implicit function declaration warnings
void *__wrap_page_alloc(uint64 order, uint64 flags);
void __wrap_page_free(void *ptr, uint64 order);

#endif // __UT_MOCK_WRAPS_H__
