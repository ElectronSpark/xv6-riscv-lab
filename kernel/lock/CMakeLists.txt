add_library(lock OBJECT
    # Lock primitives ported to Rust live alongside this CMakeLists in
    # kernel/lock/*.rs and are linked into the kernel via the xv6_rust
    # staticlib (see kernel/CMakeLists.txt). The C files here are the
    # remaining bits that are still implemented in C.
    semaphore_test.c
    rwsem_test.c
    rcu_test.c
    # Thin C trampoline for the inline atomic CAS primitives still defined
    # in kernel/inc/lock/rwlock.h, consumed by kernel/lock/rwlock.rs.
    rwlock_shim.c
)
