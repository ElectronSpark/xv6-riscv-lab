set(ARCH_C_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/start.c
    ${CMAKE_CURRENT_SOURCE_DIR}/sbi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/irq/trap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/irq/syscall.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mm/vm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/timer/timer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/timer/goldfish_rtc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ipi/ipi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/dev/plic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/dev/fdt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/platform_riscv.c
    ${CMAKE_CURRENT_SOURCE_DIR}/backtrace.c
    ${CMAKE_CURRENT_SOURCE_DIR}/gdbstub_arch.c
)

set(ARCH_S_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/entry.S
    ${CMAKE_CURRENT_SOURCE_DIR}/irq/kernelvec.S
    ${CMAKE_CURRENT_SOURCE_DIR}/irq/trampoline.S
    ${CMAKE_CURRENT_SOURCE_DIR}/proc/swtch.S
    ${CMAKE_CURRENT_SOURCE_DIR}/proc/sig_trampoline.S
    ${CMAKE_CURRENT_SOURCE_DIR}/proc/fpu.S
)

set_source_files_properties(${ARCH_C_FILES} PROPERTIES LANGUAGE C)
set_source_files_properties(${ARCH_S_FILES} PROPERTIES LANGUAGE C)

# fpu.S needs F+D extensions to assemble fsd/fld/fmv.d.x instructions,
# even though the rest of the kernel is compiled with -march=rv64imac.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/proc/fpu.S
    PROPERTIES COMPILE_FLAGS "-march=rv64gc_zicsr_zifencei -mabi=lp64"
)

add_library(arch_riscv OBJECT ${ARCH_C_FILES} ${ARCH_S_FILES})
set_source_files_properties(${ARCH_S_FILES} PROPERTIES LANGUAGE C)
add_dependencies(arch_riscv generate_asm_offsets)
target_include_directories(arch_riscv PRIVATE
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