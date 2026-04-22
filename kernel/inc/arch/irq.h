#ifndef _ARCH_IRQ_H_
#define _ARCH_IRQ_H_

// Enable interrupts on the current CPU
void arch_irq_enable(void);

// Disable interrupts on the current CPU
void arch_irq_disable(void);

// Check if interrupts are enabled on the current CPU
int arch_irq_enabled(void);

// Initialize architecture-specific interrupt controller
void arch_irq_init(void);

// Initialize interrupt controller for the current CPU/hart
void arch_irq_init_hart(void);

#endif // _ARCH_IRQ_H_