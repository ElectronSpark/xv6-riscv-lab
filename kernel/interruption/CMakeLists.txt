set(INTERRUPTION_C_SRCS
    trap.c
    plic.c
    syscall.c
    irq.c
)

set(INTERRUPTION_S_SRCS
    kernelvec.S
    trampoline.S
)

add_library(interruption OBJECT
    ${INTERRUPTION_C_SRCS}
    ${INTERRUPTION_S_SRCS}
)

# These .S files use the C preprocessor; force C language for them.
set_source_files_properties(${INTERRUPTION_S_SRCS} PROPERTIES LANGUAGE C)
