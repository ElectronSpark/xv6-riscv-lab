# CMake configuration for the test directory

cmake_minimum_required(VERSION 3.10)

# Project name
project(xv6-riscv-unit-test LANGUAGES C ASM)

ENABLE_LANGUAGE(C)

set(TEST_C_FLAGS
    -ggdb
    -Wall
    -DON_HOST_OS=1
    -DHOST_TEST=1
    # -DUSE_SOFTWARE_FFS=1
)

# Define the executable(s) to be built
set(PROGRAMS list_test)

set(UNIT_TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/../kernel/inc
)

# Add an executable for each program
foreach(PROGRAM ${PROGRAMS})
    add_executable(${PROGRAM} ${CMAKE_SOURCE_DIR}/src/${PROGRAM}.c)
    target_compile_options(${PROGRAM} PRIVATE ${TEST_C_FLAGS})
    target_include_directories(${PROGRAM} PRIVATE ${UNIT_TEST_INCLUDE_DIRS})
    target_link_options(${PROGRAM} PRIVATE -ggdb)
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
set(MOCK_TEST_KERNEL_SRC
    # We're providing mock implementations for page.c
    ${CMAKE_SOURCE_DIR}/../kernel/mm/page.c
    ${CMAKE_SOURCE_DIR}/../kernel/mm/slab.c
)

# All wrapper source files
set(WRAPPER_SRC
    ${CMAKE_SOURCE_DIR}/src/wrappers/panic_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/spinlock_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/page_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/slab_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/proc_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/workqueue_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/completion_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/timer_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/kmm_wrappers.c
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
    spin_acquire
    spin_release
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

add_executable(ut_page ${MOCK_TEST_SRC} ${MOCK_TEST_KERNEL_SRC} ${UT_PAGE_WRAPPER_SRC})
target_link_libraries(ut_page PRIVATE cmocka-static)
target_compile_options(ut_page PRIVATE ${TEST_C_FLAGS})
target_compile_definitions(ut_page PRIVATE MOCK_TEST=1 UT_PAGE_TEST_BUILD=1)
target_link_options(ut_page PRIVATE ${MOCK_COMPILE_OPTIONS})
target_include_directories(ut_page PRIVATE ${UNIT_TEST_INCLUDE_DIRS})

add_executable(ut_list ${CMAKE_SOURCE_DIR}/src/ut_list_main.c)
target_link_libraries(ut_list PRIVATE cmocka-static)
target_compile_options(ut_list PRIVATE ${TEST_C_FLAGS})
target_include_directories(ut_list PRIVATE ${UNIT_TEST_INCLUDE_DIRS})

add_executable(ut_hlist ${CMAKE_SOURCE_DIR}/src/ut_hlist_main.c ${CMAKE_SOURCE_DIR}/../kernel/hlist.c)
target_link_libraries(ut_hlist PRIVATE cmocka-static)
target_compile_options(ut_hlist PRIVATE ${TEST_C_FLAGS})
target_include_directories(ut_hlist PRIVATE ${UNIT_TEST_INCLUDE_DIRS})

add_executable(ut_rbtree
    ${CMAKE_SOURCE_DIR}/src/ut_rbtree_main.c
    ${CMAKE_SOURCE_DIR}/../kernel/rbtree.c
    ${CMAKE_SOURCE_DIR}/../kernel/bintree.c
)
target_link_libraries(ut_rbtree PRIVATE cmocka-static)
target_compile_options(ut_rbtree PRIVATE ${TEST_C_FLAGS})
target_include_directories(ut_rbtree PRIVATE ${UNIT_TEST_INCLUDE_DIRS})

add_executable(ut_semaphore
    ${CMAKE_SOURCE_DIR}/src/ut_semaphore_main.c
    ${CMAKE_SOURCE_DIR}/../kernel/lock/semaphore.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/spinlock_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/proc_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/panic_wrappers.c
)
target_link_libraries(ut_semaphore PRIVATE cmocka-static)
target_compile_options(ut_semaphore PRIVATE ${TEST_C_FLAGS})
target_link_options(ut_semaphore PRIVATE 
    -Wl,--wrap=spin_init,--wrap=spin_acquire,--wrap=spin_release,--wrap=spin_holding
    -Wl,--wrap=push_off,--wrap=pop_off
    -Wl,--wrap=proc_queue_init,--wrap=proc_queue_wait,--wrap=proc_queue_wakeup
    -Wl,--wrap=mycpu,--wrap=myproc,--wrap=proc_lock,--wrap=proc_unlock,--wrap=proc_assert_holding
    -Wl,--wrap=sched_lock,--wrap=sched_unlock,--wrap=scheduler_wakeup,--wrap=scheduler_sleep
    -Wl,--wrap=__panic_start,--wrap=__panic_end,--wrap=panic_state,--wrap=panic_disable_bt
)
target_include_directories(ut_semaphore PRIVATE
    ${CMAKE_SOURCE_DIR}/include/semaphore_stubs
    ${UNIT_TEST_INCLUDE_DIRS}
)

add_executable(ut_rwlock
    ${CMAKE_SOURCE_DIR}/src/ut_rwlock_main.c
    ${CMAKE_SOURCE_DIR}/../kernel/lock/rwlock.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/spinlock_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/proc_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/panic_wrappers.c
)
target_link_libraries(ut_rwlock PRIVATE cmocka-static)
target_compile_options(ut_rwlock PRIVATE ${TEST_C_FLAGS})
target_link_options(ut_rwlock PRIVATE     -Wl,--wrap=spin_init,--wrap=spin_acquire,--wrap=spin_release,--wrap=spin_holding
    -Wl,--wrap=push_off,--wrap=pop_off
    -Wl,--wrap=proc_queue_init,--wrap=proc_queue_size,--wrap=proc_queue_wait
    -Wl,--wrap=proc_queue_wakeup,--wrap=proc_queue_wakeup_all
    -Wl,--wrap=mycpu,--wrap=myproc,--wrap=proc_lock,--wrap=proc_unlock,--wrap=proc_assert_holding
    -Wl,--wrap=sched_lock,--wrap=sched_unlock,--wrap=scheduler_wakeup,--wrap=scheduler_sleep    -Wl,--wrap=__panic_start,--wrap=__panic_end,--wrap=panic_state,--wrap=panic_disable_bt
)
target_include_directories(ut_rwlock PRIVATE
    ${CMAKE_SOURCE_DIR}/include/semaphore_stubs
    ${UNIT_TEST_INCLUDE_DIRS}
)

add_executable(ut_bits
    ${CMAKE_SOURCE_DIR}/src/ut_bits_main.c
    ${CMAKE_SOURCE_DIR}/../kernel/bits.c
)
target_link_libraries(ut_bits PRIVATE cmocka-static)
target_compile_options(ut_bits PRIVATE ${TEST_C_FLAGS})
target_include_directories(ut_bits PRIVATE ${UNIT_TEST_INCLUDE_DIRS})

add_executable(ut_pcache
    ${CMAKE_SOURCE_DIR}/src/ut_pcache_main.c
    ${CMAKE_SOURCE_DIR}/../kernel/mm/pcache.c
    ${CMAKE_SOURCE_DIR}/../kernel/rbtree.c
    ${CMAKE_SOURCE_DIR}/../kernel/bintree.c
    ${WRAPPER_SRC}
)
target_link_libraries(ut_pcache PRIVATE cmocka-static)
target_compile_options(ut_pcache PRIVATE ${TEST_C_FLAGS})
target_link_options(ut_pcache PRIVATE ${MOCK_COMPILE_OPTIONS})
target_include_directories(ut_pcache PRIVATE ${UNIT_TEST_INCLUDE_DIRS})

# Tmpfs truncate test (linked version)
# This test links directly to kernel/vfs/tmpfs/truncate.c using header guards
# to block kernel headers and provide minimal test stubs.
add_executable(ut_tmpfs_truncate
    ${CMAKE_SOURCE_DIR}/src/ut_tmpfs_truncate_linked.c
)
target_link_libraries(ut_tmpfs_truncate PRIVATE cmocka-static)
target_compile_options(ut_tmpfs_truncate PRIVATE ${TEST_C_FLAGS})
target_include_directories(ut_tmpfs_truncate PRIVATE ${UNIT_TEST_INCLUDE_DIRS})

add_executable(ut_early_allocator
    ${CMAKE_SOURCE_DIR}/src/ut_early_allocator_main.c
)
target_link_libraries(ut_early_allocator PRIVATE cmocka-static)
target_compile_options(ut_early_allocator PRIVATE ${TEST_C_FLAGS})
target_include_directories(ut_early_allocator PRIVATE ${UNIT_TEST_INCLUDE_DIRS})

enable_testing()
add_test(NAME ut_mock_test
    COMMAND ut_page
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
add_test(NAME ut_list
    COMMAND ut_list
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
add_test(NAME ut_hlist
    COMMAND ut_hlist
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
add_test(NAME ut_rbtree
    COMMAND ut_rbtree
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
add_test(NAME ut_semaphore
    COMMAND ut_semaphore
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
add_test(NAME ut_rwlock
    COMMAND ut_rwlock
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
add_test(NAME ut_bits
    COMMAND ut_bits
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
add_test(NAME ut_pcache
    COMMAND ut_pcache
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
add_test(NAME ut_tmpfs_truncate
    COMMAND ut_tmpfs_truncate
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
add_test(NAME ut_early_allocator
    COMMAND ut_early_allocator
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
