# CMake configuration for the test directory

cmake_minimum_required(VERSION 3.10)

# Project name
project(xv6-riscv-unit-test LANGUAGES C ASM)

ENABLE_LANGUAGE(C)

# Option to enable AddressSanitizer
option(ENABLE_ASAN "Enable AddressSanitizer for detecting memory errors" ON)

set(TEST_C_FLAGS
    -ggdb
    -Wall
    -DON_HOST_OS=1
    -DHOST_TEST=1
    -D__KERNEL_SKIP_STRTOUL=1
    # -DUSE_SOFTWARE_FFS=1
)

# AddressSanitizer flags
if(ENABLE_ASAN)
    message(STATUS "AddressSanitizer enabled")
    set(ASAN_COMPILE_FLAGS -fsanitize=address -fno-omit-frame-pointer)
    set(ASAN_LINK_FLAGS -fsanitize=address)
else()
    message(STATUS "AddressSanitizer disabled")
    set(ASAN_COMPILE_FLAGS "")
    set(ASAN_LINK_FLAGS "")
endif()

# Define the executable(s) to be built
set(PROGRAMS list_test)

set(UNIT_TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/../kernel/inc
    ${CMAKE_SOURCE_DIR}/../kernel/inc/lock
    ${CMAKE_SOURCE_DIR}/../kernel
)

# Add an executable for each program
foreach(PROGRAM ${PROGRAMS})
    add_executable(${PROGRAM} ${CMAKE_SOURCE_DIR}/src/${PROGRAM}.c)
    target_compile_options(${PROGRAM} PRIVATE ${TEST_C_FLAGS} ${ASAN_COMPILE_FLAGS})
    target_include_directories(${PROGRAM} PRIVATE ${UNIT_TEST_INCLUDE_DIRS})
    target_link_options(${PROGRAM} PRIVATE -ggdb ${ASAN_LINK_FLAGS})
endforeach()

# Add a custom target for running tests
set(RUN_TEST_COMMANDS "")
foreach(PROGRAM ${PROGRAMS})
    list(APPEND RUN_TEST_COMMANDS COMMAND echo "Running test ${PROGRAM}")
    list(APPEND RUN_TEST_COMMANDS COMMAND ./${PROGRAM})
endforeach()

add_custom_target(test-list
    COMMENT "Running tests..."
    ${RUN_TEST_COMMANDS}
    DEPENDS ${PROGRAMS}
)

# Add a custom target for running valgrind
set(RUN_VALGRIND_COMMANDS "")
foreach(PROGRAM ${PROGRAMS})
    list(APPEND RUN_VALGRIND_COMMANDS COMMAND echo "Running valgrind on ${PROGRAM}")
    list(APPEND RUN_VALGRIND_COMMANDS COMMAND valgrind ./${PROGRAM})
endforeach()

add_custom_target(valgrind
    COMMENT "Running valgrind..."
    ${RUN_VALGRIND_COMMANDS}
)

# Add a custom target for running valgrind with full leak check
set(RUN_VALGRIND_FULL_COMMANDS "")
foreach(PROGRAM ${PROGRAMS})
    list(APPEND RUN_VALGRIND_FULL_COMMANDS COMMAND echo "Running valgrind full on ${PROGRAM}")
    list(APPEND RUN_VALGRIND_FULL_COMMANDS COMMAND valgrind --leak-check=full ./${PROGRAM})
endforeach()

add_custom_target(valgrind-full
    COMMENT "Running valgrind full..."
    ${RUN_VALGRIND_FULL_COMMANDS}
)


# Kernel source files to do unit tests
# NOTE: page.c and slab.c have been ported to Rust (kernel/rust/src/page.rs
# and slab.rs). Host-side unit tests that previously linked these C sources
# now rely on the mock implementations in src/wrappers/ instead.
set(MOCK_TEST_KERNEL_SRC
)

# All wrapper source files
set(WRAPPER_SRC
    ${CMAKE_SOURCE_DIR}/src/wrappers/panic_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/spinlock_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/rwlock_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/page_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/slab_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/proc_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/workqueue_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/completion_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/timer_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/kmm_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/cpu_wrappers.c
)

# Wrapper sources for ut_page
set(UT_PAGE_WRAPPER_SRC
    ${CMAKE_SOURCE_DIR}/src/wrappers/panic_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/spinlock_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/page_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/proc_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/kmm_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/slab_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/workqueue_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/completion_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/timer_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/cpu_wrappers.c
)

# Functions to wrap for all tests
set(MOCK_TEST_WRAP_FUNCTIONS
    # Page functions
    page_lock_acquire
    page_lock_release
    page_lock_assert_holding
    page_lock_assert_unholding
    page_ref_inc
    page_ref_dec
    page_ref_count
    page_ref_inc_unlocked
    page_ref_dec_unlocked
    __page_ref_inc
    __page_ref_dec
    page_refcnt
    page_alloc
    page_free
    __page_alloc
    __page_free
    __page_to_pa
    __pa_to_page
    __page_init
    # Slab functions
    slab_cache_init
    slab_cache_create
    slab_cache_destroy
    slab_cache_shrink
    slab_alloc
    slab_free
    # Spinlock functions
    spin_lock
    spin_unlock
    spin_init
    spin_holding
    spin_lock
    spin_unlock
    push_off
    pop_off
    # Process functions
    cpuid
    mycpu
    myproc
    __current_thread
    proc_lock
    proc_unlock
    proc_assert_holding
    sched_lock
    sched_unlock
    scheduler_wakeup
    scheduler_sleep
    kernel_proc_create
    wakeup_proc
    wakeup_on_chan
    sleep_on_chan
    # Proc queue functions
    proc_queue_init
    proc_queue_size
    proc_queue_wait
    proc_queue_wakeup
    proc_queue_wakeup_all
    # Thread queue functions (tq_*)
    tq_init
    tq_size
    tq_wait
    tq_wait_cb
    tq_wait_in_state
    tq_wakeup
    tq_wakeup_all
    signal_pending
    wakeup
    kthread_create
    rwlock_init
    rwlock_rlock
    rwlock_runlock
    rwlock_wlock
    rwlock_wunlock
    rwlock_r_sleep_cb
    rwlock_r_wake_cb
    # Workqueue functions
    workqueue_create
    queue_work
    init_work_struct
    create_work_struct
    free_work_struct
    # Completion functions
    completion_init
    completion_reinit
    try_wait_for_completion
    wait_for_completion
    complete
    complete_all
    completion_done
    # Timer functions
    get_jiffs
    sleep_ms
    # KMM functions
    kmm_alloc
    kmm_free
    # Panic functions
    __panic_start
    __panic_end
    panic_state
    panic_disable_bt
    printfinit
    argint
)

set(MOCK_TEST_SRC
    ${CMAKE_SOURCE_DIR}/src/ut_page_main.c
)

set(MOCK_WRAP_OPTIONS "")
foreach(EACH_WRAP ${MOCK_TEST_WRAP_FUNCTIONS})
    set(MOCK_WRAP_OPTIONS "${MOCK_WRAP_OPTIONS},--wrap=${EACH_WRAP}")
endforeach()

set(MOCK_COMPILE_OPTIONS "-Wl${MOCK_WRAP_OPTIONS}")


include(cmake/FetchCmocka.cmake)
# find_library(cmocka-static)

# ut_page is temporarily disabled - needs update for refactored page_buddy_init API
# add_executable(ut_page ${MOCK_TEST_SRC} ${MOCK_TEST_KERNEL_SRC} ${UT_PAGE_WRAPPER_SRC})
# target_link_libraries(ut_page PRIVATE cmocka-static)
# target_compile_options(ut_page PRIVATE ${TEST_C_FLAGS} ${ASAN_COMPILE_FLAGS})
# target_compile_definitions(ut_page PRIVATE MOCK_TEST=1 UT_PAGE_TEST_BUILD=1)
# target_link_options(ut_page PRIVATE ${MOCK_COMPILE_OPTIONS} ${ASAN_LINK_FLAGS})
# target_include_directories(ut_page PRIVATE ${UNIT_TEST_INCLUDE_DIRS})

add_executable(ut_list ${CMAKE_SOURCE_DIR}/src/ut_list_main.c)
target_link_libraries(ut_list PRIVATE cmocka-static)
target_compile_options(ut_list PRIVATE ${TEST_C_FLAGS} ${ASAN_COMPILE_FLAGS})
target_link_options(ut_list PRIVATE ${ASAN_LINK_FLAGS})
target_include_directories(ut_list PRIVATE ${UNIT_TEST_INCLUDE_DIRS})

# ut_hlist / ut_rbtree RETIRED (2026-07-11): their kernel C sources
# (kernel/hlist.c, kernel/rbtree.c, kernel/bintree.c) were rewritten in Rust
# (kernel/hlist.rs, kernel/rbtree.rs, kernel/bintree.rs — Phase 2 Wave 1).
# Coverage is to be re-homed as in-kernel Rust runtime tests or cargo-test
# host suites per docs/rustify/test_port_plan.md. Original targets in git
# history.

# ut_semaphore / ut_rwlock RETIRED (2026-07-11): their kernel C sources
# (lock/semaphore.c, lock/rwsem.c) were rewritten in Rust, and equivalent
# in-kernel Rust test suites already exist (kernel/lock/semaphore_test.rs,
# kernel/lock/rwsem_test.rs, run via SEMAPHORE_TEST=1 / RWLOCK_TEST=1
# configure-time gates + QEMU boot). See docs/rustify/test_port_plan.md.

add_executable(ut_bits
    ${CMAKE_SOURCE_DIR}/src/ut_bits_main.c
    # kernel/bits.c was deleted (Rust port); never actually needed here —
    # bits.h uses compiler builtins unless USE_SOFTWARE_FFS=1 (off).
)
target_link_libraries(ut_bits PRIVATE cmocka-static)
target_compile_options(ut_bits PRIVATE ${TEST_C_FLAGS} ${ASAN_COMPILE_FLAGS})
target_link_options(ut_bits PRIVATE ${ASAN_LINK_FLAGS})
target_include_directories(ut_bits PRIVATE ${UNIT_TEST_INCLUDE_DIRS})

# ut_pcache DISABLED (2026-07-11): kernel/mm/pcache.c was rewritten in Rust
# (kernel/mm/pcache.rs). Its 35 tests (incl. 4 concurrency stress tests) are
# to be re-homed as an in-kernel Rust runtime suite (mm/pcache_test.rs) per
# docs/rustify/test_port_plan.md. Original target in git history.

# ut_tmpfs_truncate DISABLED (2026-07-12): its kernel C source
# (kernel/vfs/tmpfs/truncate.c) was rewritten in Rust
# (kernel/vfs/tmpfs/truncate.rs — Phase 2 Wave 18); the test `#include`d
# truncate.c directly, so that link-in strategy no longer applies to a
# Rust file. Coverage is to be re-homed as an in-kernel Rust runtime
# test or a cargo-test host suite per docs/rustify/test_port_plan.md.
# Original target in git history.

# ut_early_allocator DISABLED (2026-07-11): its main.c #includes
# kernel/mm/early_allocator.c which was rewritten in Rust
# (kernel/mm/early_allocator.rs). To be replaced by host-side cargo tests
# per docs/rustify/test_port_plan.md.

# ut_workqueue RETIRED (2026-07-13, docs/rustify/test_port_plan.md Phase 4):
# it never tested the real `kernel/proc/workqueue.c`/`workqueue.rs` at all
# -- every entry point it linked against was `--wrap`'d to a host mock (see
# the `-Wl,--wrap=...` list this target used to pass, and
# src/wrappers/workqueue_wrappers.c), so this suite validated its own mock,
# not the kernel. Re-homed as kernel/proc/workqueue_test.rs, a genuine
# in-kernel Rust runtime suite driving the real implementation's real
# manager/worker kthreads (wired below as the `qemu_workqueue` ctest via
# test/cmake/QemuTests.cmake, WORKQUEUE_TEST=1). Original target/source in
# git history (test/src/ut_workqueue_main.c still present on disk, unused).

enable_testing()
# ut_mock_test disabled - ut_page needs API updates
# add_test(NAME ut_mock_test
#     COMMAND ut_page
#     WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
# )
add_test(NAME ut_list
    COMMAND ut_list
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
# add_test ut_hlist removed (target disabled/retired 2026-07-11)
# add_test ut_rbtree removed (target disabled/retired 2026-07-11)
# add_test ut_semaphore removed (target disabled/retired 2026-07-11)
# add_test ut_rwlock removed (target disabled/retired 2026-07-11)
add_test(NAME ut_bits
    COMMAND ut_bits
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
# add_test ut_pcache removed (target disabled/retired 2026-07-11)
# add_test ut_tmpfs_truncate removed (target disabled/retired 2026-07-12)
# add_test ut_early_allocator removed (target disabled/retired 2026-07-11)
# add_test ut_workqueue removed (target retired 2026-07-13, see the
# add_executable comment above -- superseded by the qemu_workqueue ctest
# below, which drives the real implementation instead of a mock of it).

# ------------------------------------------------------------------------------
# Rust host-test suite (docs/rustify/test_port_plan.md Phase 1/2 -- the
# host-test seam + the `#[cfg(test)]` suites in kernel/mm/bits.rs,
# kernel/list.rs, kernel/mm/early_allocator.rs; see kernel/lib.rs's
# "Host-test seam" doc comment for the full design). Unlike every ut_*
# target above, this isn't a compiled cmocka executable added via
# `add_executable` -- `cargo test` builds and runs its own test harness
# binary, so the ctest entry just shells out to `cargo` directly.
#
# `--target x86_64-unknown-linux-gnu` is mandatory and explicit:
# kernel/.cargo/config.toml pins `[build] target = riscv64gc-unknown-none-elf`
# for the real kernel build (a `#![no_std]` staticlib full of riscv-only
# `core::arch::asm!`), and only an explicit `--target` on the command line
# overrides that pinned default -- an environment variable would not.
# `WORKING_DIRECTORY` is `kernel/` so cargo's own config-file discovery
# (which starts from the process's current directory, not `--manifest-path`)
# still finds that same `.cargo/config.toml` for its `target-dir` redirect
# (`../build/kernel/rust`), keeping host-test artifacts out of the
# CMake-driven riscv build's target dir.
#
# `LAB=fs` mirrors this repo's one supported/verified configuration (see
# RUST_REWRITE.md's "Build env" working note) -- hardcoded rather than
# forwarded from whatever `$ENV{LAB}` this `cmake` invocation happened to
# see, so this ctest entry can't be silently poisoned by an unset/different
# LAB the way the main kernel build historically was (see RUST_REWRITE.md's
# "NEW VECTOR" note). None of the three suites under test are LAB-gated
# themselves; this only affects which conditional kernel-header defines
# bindgen's libclang parse sees while generating `kernel_bindings.rs`.
find_program(CARGO_EXECUTABLE_FOR_RUST_TESTS cargo)
if(CARGO_EXECUTABLE_FOR_RUST_TESTS)
    add_test(NAME rust_host_unit_tests
        COMMAND ${CARGO_EXECUTABLE_FOR_RUST_TESTS} test --target x86_64-unknown-linux-gnu
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/../kernel
    )
    set_tests_properties(rust_host_unit_tests PROPERTIES
        ENVIRONMENT "LAB=fs"
    )
else()
    message(STATUS "cargo not found -- rust_host_unit_tests ctest entry skipped")
endif()

# ------------------------------------------------------------------------------
# QEMU-driven ctest entries for the in-kernel Rust runtime test suites
# (docs/rustify/test_port_plan.md Phase 3/4: rwsem/semaphore -- pre-existing
# suites now with QEMU ctest wiring for the first time -- and the two new
# suites, pcache/workqueue). See test/cmake/QemuTests.cmake for the full
# design (isolated per-suite build dirs, RESOURCE_LOCK serialization,
# scripts/run_qemu_test.sh harness).
# ------------------------------------------------------------------------------
include(cmake/QemuTests.cmake)
