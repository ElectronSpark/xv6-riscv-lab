set(INTERRUPTION_C_SRCS
    trap.c
    plic.c
    syscall.c
    irq.c
    softirq.c
)

set(INTERRUPTION_S_SRCS
    kernelvec.S
    trampoline.S
)

add_library(irq OBJECT
    ${INTERRUPTION_C_SRCS}
    ${INTERRUPTION_S_SRCS}
)

# These .S files use the C preprocessor; force C language for them.
set_source_files_properties(${INTERRUPTION_S_SRCS} PROPERTIES LANGUAGE C)

# Ensure asm-offsets.h is generated before building assembly files
add_dependencies(irq generate_asm_offsets)

# Add build directory to include path for generated headers
target_include_directories(irq PRIVATE ${CMAKE_BINARY_DIR}/kernel/inc)