#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

extern char stack0[];

#define BT_FRAME_TOP(__fp)          (*(uint64 *)((uint64)(__fp) - 16))
#define BT_RETURN_ADDRESS(__fp)     (*(uint64 *)((uint64)(__fp) - 8))
#define BT_IS_TOP_FRAME(__fp)       ((uint64)(__fp) == PGROUNDDOWN((uint64)(__fp)))

void
print_backtrace(uint64 context)
{
    printf("backtrace:\n");
    for (uint64 fp = BT_FRAME_TOP(context);
         !BT_IS_TOP_FRAME(fp); 
         fp = BT_FRAME_TOP(fp)) {
            printf("* %p *\n", (void *)BT_RETURN_ADDRESS(fp));
    }
}
