set(PROC_C_SRCS
    # thread.c -- ported to Rust (kernel/proc/thread.rs)
    # thread_group.c -- ported to Rust (kernel/proc/thread_group.rs)
    # thread_queue.c -- ported to Rust (kernel/proc/thread_queue.rs)
    # sched.c -- ported to Rust (kernel/proc/sched.rs)
    # signal.c       -- ported to Rust (kernel/proc/signal.rs)
    # sys_signal.c   -- ported to Rust (kernel/proc/sys_signal.rs)
    # sysproc.c      -- ported to Rust (kernel/proc/sysproc.rs)
    # workqueue.c   -- ported to Rust (kernel/proc/workqueue.rs)
    # clone.c        -- ported to Rust (kernel/proc/clone.rs)
    # exit.c         -- ported to Rust (kernel/proc/exit.rs)
    # pid.c          -- ported to Rust (kernel/proc/pid.rs)
    # rq.c           -- ported to Rust (kernel/proc/rq.rs)
    # sched_idle.c   -- ported to Rust (kernel/proc/sched_idle.rs)
    # sched_fifo.c   -- ported to Rust (kernel/proc/sched_fifo.rs)
    # rq_test.c      -- ported to Rust (kernel/proc/rq_test.rs)
    # pgroup.c       -- ported to Rust (kernel/proc/pgroup.rs)
    # proc_rust_shims.c -- fully replaced by Rust ports
    proc_rust_bridge.c
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
