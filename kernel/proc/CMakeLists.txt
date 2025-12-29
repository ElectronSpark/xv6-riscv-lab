set(PROC_C_SRCS
    proc.c
    proc_queue.c
    sched.c
    signal.c
    sys_signal.c
    sysproc.c
    workqueue.c
    timer.c
    sched_timer.c
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
