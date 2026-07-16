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
    # proc_rust_shims.c  -- deleted; every function had a live Rust port
    #                       already (proc_shims.rs, access.rs, signal.rs,
    #                       sys_signal.rs, thread_group.rs, thread.rs,
    #                       sched.rs, rq.rs, rq_test.rs, workqueue.rs,
    #                       thread_queue.rs).
    # proc_rust_bridge.c -- deleted; its 7 functions ported to Rust
    #                       (access.rs::{PgroupAccess,SessionAccess}::mark_kernel,
    #                       thread.rs::install_user_root_finish,
    #                       proc_shims.rs::{xv6_panic,for_each_proctab_thread}).
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

# P3-5: asm-offsets.h is a static checked-in header now
# (kernel/inc/asm-offsets.h, found via the global -I${KERNEL_DIR}/inc);
# the generate_asm_offsets custom target and the
# ${CMAKE_BINARY_DIR}/kernel/inc include dir it wrote into are gone.
# (swtch.S never consumed it anyway — it hardcodes the context offsets,
# pinned by kernel/proc/sched.rs's const asserts.)
