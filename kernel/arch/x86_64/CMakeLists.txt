set(ARCH_C_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/start.c
    ${CMAKE_CURRENT_SOURCE_DIR}/seg.c
    ${CMAKE_CURRENT_SOURCE_DIR}/irq/trap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/irq/lapic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/irq/ioapic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/irq/syscall.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ipi/ipi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/timer/timer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mm/vm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/platform_x86.c
    ${CMAKE_CURRENT_SOURCE_DIR}/backtrace.c
    ${CMAKE_CURRENT_SOURCE_DIR}/gdbstub_arch.c
)

set(ARCH_S_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/entry.S
    ${CMAKE_CURRENT_SOURCE_DIR}/ap_trampoline.S
    ${CMAKE_CURRENT_SOURCE_DIR}/proc/swtch.S
    ${CMAKE_CURRENT_SOURCE_DIR}/proc/sig_trampoline.S
    ${CMAKE_CURRENT_SOURCE_DIR}/proc/fpu.S
    ${CMAKE_CURRENT_SOURCE_DIR}/irq/trapvec.S
    ${CMAKE_CURRENT_SOURCE_DIR}/irq/trampoline.S
)

set_source_files_properties(${ARCH_C_FILES} PROPERTIES LANGUAGE C)
set_source_files_properties(${ARCH_S_FILES} PROPERTIES LANGUAGE C)

add_library(arch_x86_64 OBJECT ${ARCH_C_FILES} ${ARCH_S_FILES})
target_include_directories(arch_x86_64 PRIVATE
    ${KERNEL_DIR}/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ${CMAKE_BINARY_DIR}/kernel/inc
    ${KERNEL_DIR}/proc
    ${KERNEL_DIR}/mm
    ${KERNEL_DIR}/irq
    ${KERNEL_DIR}/dev
    ${KERNEL_DIR}/timer
    ${KERNEL_DIR}/ipi
)