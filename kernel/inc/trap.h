#ifndef __KERNEL_TRAP_H
#define __KERNEL_TRAP_H

#include "types.h"
#include "lock/rcu_type.h"

// Interrupts are asynchronous events that require the CPU to stop its current
// execution flow and jump to a specific handler function. They can be triggered
// by hardware devices (like timers, keyboards, or network cards).
// Dirrefent from exceptions, interrupts are triggered unexpectedly and can occur at any time.
// Thus, each CPU hart has a dedicated interrupt stack to handle interrupts safely.
// When in IRQ stack, we should:
// - Finish handling the interrupt as soon as possible
// When in IRQ stack, we should NOT:
// - Call functions that may sleep or yield the CPU. IRQ context doesn't belong to any process.
// - Perform long computations
// IRQ numbers will be distributed as the following:
// HW Exeption Codes 0 - 16(not including 9):
// - mapped to 0 - 16 IRQ numbers
// PLINC IRQs:
// - mapped to 1025 - 2047 IRQ numbers (+ 1024 offset, 0 does not exist)

#define CLINT_IRQ_CNT   1024
#define PLIC_IRQ_OFFSET CLINT_IRQ_CNT
#define PLIC_IRQ_CNT    1024
#define IRQCNT          (PLIC_IRQ_OFFSET + PLIC_IRQ_CNT)
#define PLIC_IRQ(hw_irq)    ((hw_irq) + PLIC_IRQ_OFFSET)

#define RISCV_INSTRUCTION_ADDR_MISALIGNED   0
#define RISCV_INSTRUCTION_ACCESS_FAULT      1
#define RISCV_ILLEGAL_INSTRUCTION           2
#define RISCV_BREAKPOINT_TRAP               3
#define RISCV_LOAD_ADDR_MISALIGNED          4
#define RISCV_LOAD_ACCESS_FAULT             5
#define RISCV_STORE_ADDR_MISALIGNED         6
#define RISCV_STORE_ACCESS_FAULT            7
#define RISCV_ENV_CALL_FROM_U_MODE          8
#define RISCV_ENV_CALL_FROM_S_MODE          9
#define RISCV_ENV_CALL_FROM_M_MODE          11
#define RISCV_INSTRUCTION_PAGE_FAULT        12
#define RISCV_LOAD_PAGE_FAULT               13
#define RISCV_STORE_PAGE_FAULT              15
#define RISCV_DOUBLE_TRAP                   16
#define RISCV_SOFTWARE_CHECK_FAIL           18
#define RISCV_HARDWARE_ERROR                19

#define RISCV_S_SOFTWARE_INTERRUPT          1
#define RISCV_M_SOFTWARE_INTERRUPT          3
#define RISCV_S_TIMER_INTERRUPT             5
#define RISCV_M_TIMER_INTERRUPT             7
#define RISCV_S_EXTERNAL_INTERRUPT          9
#define RISCV_M_EXTERNAL_INTERRUPT          11
#define RISCV_COUNTER_INTERRUPT             12

struct trapframe;
typedef struct rcu_head rcu_head_t;
typedef struct device_instance device_t;
typedef void (*irq_handler_t)(int irq, void *data, device_t *dev);

struct irq_desc {
    // User specified data and handler
    irq_handler_t handler;
    void *data;
    device_t *dev;

    // Status info(will be ignored when registering)
    int irq;
    uint64 count; // Number of times this IRQ has been handled
    
    // RCU support for deferred freeing
    rcu_head_t rcu_head;
};

static inline const char *__scause_to_str(uint64 scause)
{
    if (scause & 0x8000000000000000) {
            // Interrupts are negative, exceptions are positive.
            switch (scause & 0x7FFFFFFFFFFFFFFF) {
            case 0: return "User software interrupt";
            case 1: return "Supervisor software interrupt";
            case 4: return "User timer interrupt";
            case 5: return "Supervisor timer interrupt";
            case 8: return "User external interrupt";
            case 9: return "Supervisor external interrupt";
            default: return "Unknown interrupt";
        }
    }
    switch (scause) {
        case 0: return "Instruction address misaligned";
        case 1: return "Instruction access fault";
        case 2: return "Illegal instruction";
        case 3: return "Breakpoint";
        case 5: return "Load access fault";
        case 6: return "Store/AMO address misaligned";
        case 7: return "Store/AMO access fault";
        case 8: return "Environment call from U-mode";
        case 9: return "Environment call from S-mode";
        case 12: return "Instruction page fault";
        case 13: return "Load page fault";
        case 15: return "Store/AMO page fault";
        default: return "Unknown exception";
    }
}

void trapinit(void);
void trapinithart(void);

void irq_desc_init(void);
int register_irq_handler(int irq_num, struct irq_desc *desc);
int unregister_irq_handler(int irq_num);
int do_irq(struct trapframe *tf);

void enter_irq(void);
void exit_irq(void);
void enter_softirq(void);
void exit_softirq(void);

#endif        /* __KERNEL_TRAP_H */
