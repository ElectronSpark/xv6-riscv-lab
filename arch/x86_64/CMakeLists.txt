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
    ${CMAKE_CURRENT_SOURCE_DIR}/mm/pgtable.c
    ${CMAKE_CURRENT_SOURCE_DIR}/proc/fpu.c
    ${CMAKE_CURRENT_SOURCE_DIR}/platform_x86.c
    ${CMAKE_CURRENT_SOURCE_DIR}/backtrace.c
    ${CMAKE_CURRENT_SOURCE_DIR}/gdbstub_arch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/vdso.c
)

set(X86_VDSO_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/vdso)
set(X86_VDSO_SO ${X86_VDSO_BUILD_DIR}/linux-vdso.so.1)
set(X86_VDSO_BLOB_S ${X86_VDSO_BUILD_DIR}/vdso_blob.S)
set(X86_VDSO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/vdso/vdso.S)
set(X86_VDSO_VERSION ${CMAKE_CURRENT_SOURCE_DIR}/vdso/vdso.version)
set(X86_VDSO_LDS ${CMAKE_CURRENT_SOURCE_DIR}/vdso/vdso.lds)
set(X86_VDSO_BLOB_GEN ${CMAKE_CURRENT_SOURCE_DIR}/vdso/gen_vdso_blob.py)

add_custom_command(
    OUTPUT ${X86_VDSO_SO}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${X86_VDSO_BUILD_DIR}
    COMMAND ${CMAKE_C_COMPILER}
            -nostdlib -shared -fPIC
            -Wl,-soname,linux-vdso.so.1
            -Wl,--hash-style=both
            -Wl,--build-id=none
            -Wl,-z,norelro
            -Wl,-z,max-page-size=4096
            -Wl,--version-script=${X86_VDSO_VERSION}
            -Wl,-T,${X86_VDSO_LDS}
            ${X86_VDSO_SRC}
            -o ${X86_VDSO_SO}
    DEPENDS ${X86_VDSO_SRC} ${X86_VDSO_VERSION} ${X86_VDSO_LDS}
    COMMENT "Building x86_64 vDSO ELF"
    VERBATIM
)

add_custom_command(
    OUTPUT ${X86_VDSO_BLOB_S}
    COMMAND python3 ${X86_VDSO_BLOB_GEN} ${X86_VDSO_SO} ${X86_VDSO_BLOB_S}
    DEPENDS ${X86_VDSO_SO} ${X86_VDSO_BLOB_GEN}
    COMMENT "Embedding x86_64 vDSO ELF"
    VERBATIM
)

set(ARCH_S_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/entry.S
    ${CMAKE_CURRENT_SOURCE_DIR}/ap_trampoline.S
    ${CMAKE_CURRENT_SOURCE_DIR}/proc/swtch.S
    ${CMAKE_CURRENT_SOURCE_DIR}/proc/sig_trampoline.S
    ${CMAKE_CURRENT_SOURCE_DIR}/proc/fpu.S
    ${CMAKE_CURRENT_SOURCE_DIR}/irq/trapvec.S
    ${CMAKE_CURRENT_SOURCE_DIR}/irq/trampoline.S
    ${X86_VDSO_BLOB_S}
)

set_source_files_properties(${ARCH_C_FILES} PROPERTIES LANGUAGE C)
set_source_files_properties(${ARCH_S_FILES} PROPERTIES LANGUAGE C)
set_source_files_properties(${X86_VDSO_BLOB_S} PROPERTIES GENERATED TRUE LANGUAGE C)

add_library(arch_x86_64 OBJECT ${ARCH_C_FILES} ${ARCH_S_FILES})
add_dependencies(arch_x86_64 generate_asm_offsets)
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
