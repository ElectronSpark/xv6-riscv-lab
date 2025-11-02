# CMake configuration for the test directory

cmake_minimum_required(VERSION 3.10)

# Project name
project(xv6-riscv-unit-test LANGUAGES C ASM)

ENABLE_LANGUAGE(C)

set(TEST_C_FLAGS
    -ggdb
    -Wall
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

set(MOCK_TEST_WRAP_FUNCTIONS
    page_lock_acquire
    page_lock_release
    page_ref_inc
    page_ref_dec
    page_ref_count
    page_refcnt
    page_alloc
    page_free
    __page_alloc
    __page_free
    # page_buddy_init
    panic
    spin_acquire
    spin_release
    spin_init
    spin_holding
    kmm_alloc
    kmm_free
)

set(MOCK_TEST_SRC
    ${CMAKE_SOURCE_DIR}/src/ut_page_main.c
    ${CMAKE_SOURCE_DIR}/src/ut_page_wraps.c
    ${CMAKE_SOURCE_DIR}/src/panic_stubs.c
)

set(MOCK_WRAP_OPTIONS "")
foreach(EACH_WRAP ${MOCK_TEST_WRAP_FUNCTIONS})
    set(MOCK_WRAP_OPTIONS "${MOCK_WRAP_OPTIONS},--wrap=${EACH_WRAP}")
endforeach()

set(MOCK_COMPILE_OPTIONS "-Wl${MOCK_WRAP_OPTIONS}")


include(cmake/FetchCmocka.cmake)
# find_library(cmocka-static)

add_executable(ut_page ${MOCK_TEST_SRC} ${MOCK_TEST_KERNEL_SRC})
target_link_libraries(ut_page PRIVATE cmocka-static)
target_compile_options(ut_page PRIVATE ${TEST_C_FLAGS})
target_compile_definitions(ut_page PRIVATE MOCK_TEST=1)
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
    ${CMAKE_SOURCE_DIR}/src/semaphore_runtime_stubs.c
)
target_link_libraries(ut_semaphore PRIVATE cmocka-static)
target_compile_options(ut_semaphore PRIVATE ${TEST_C_FLAGS})
target_include_directories(ut_semaphore PRIVATE
    ${CMAKE_SOURCE_DIR}/include/semaphore_stubs
    ${UNIT_TEST_INCLUDE_DIRS}
)

add_executable(ut_rwlock
    ${CMAKE_SOURCE_DIR}/src/ut_rwlock_main.c
    ${CMAKE_SOURCE_DIR}/../kernel/lock/rwlock.c
    ${CMAKE_SOURCE_DIR}/src/panic_stubs.c
)
target_link_libraries(ut_rwlock PRIVATE cmocka-static)
target_compile_options(ut_rwlock PRIVATE ${TEST_C_FLAGS})
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
    ${CMAKE_SOURCE_DIR}/src/pcache_test_stubs.c
    ${CMAKE_SOURCE_DIR}/../kernel/mm/pcache.c
    ${CMAKE_SOURCE_DIR}/../kernel/rbtree.c
    ${CMAKE_SOURCE_DIR}/../kernel/bintree.c
)
target_link_libraries(ut_pcache PRIVATE cmocka-static)
target_compile_options(ut_pcache PRIVATE ${TEST_C_FLAGS})
target_include_directories(ut_pcache PRIVATE ${UNIT_TEST_INCLUDE_DIRS})

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
