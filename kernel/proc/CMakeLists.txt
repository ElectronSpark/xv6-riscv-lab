set(PROC_C_SRCS
    thread.c
    thread_queue.c
    sched.c
    signal.c
    sys_signal.c
    sysproc.c
    workqueue.c
    clone.c
    exit.c
    pid.c
    rq.c
    sched_idle.c
    sched_fifo.c
    rq_test.c
)

set(PROC_S_SRCS
    swtch.S
    sig_trampoline.S
)

add_library(proc OBJECT
    ${PROC_C_SRCS}
    ${PROC_S_SRCS}
)

# These .S files use the C preprocessor; force C language for them.
set_source_files_properties(${PROC_S_SRCS} PROPERTIES LANGUAGE C)

# Ensure asm-offsets.h is generated before building assembly files
add_dependencies(proc generate_asm_offsets)

# Add build directory to include path for generated headers
target_include_directories(proc PRIVATE ${CMAKE_BINARY_DIR}/kernel/inc)
