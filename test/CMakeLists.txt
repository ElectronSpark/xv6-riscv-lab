# CMake configuration for the test directory

cmake_minimum_required(VERSION 3.10)

# Project name
project(xv6-riscv-unit-test LANGUAGES C ASM)

ENABLE_LANGUAGE(C)

set(TEST_C_FLAGS
    -ggdb
    -Wall
    -D HOST_TEST=1
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

function(add_cmocka_executable target)
    cmake_parse_arguments(CMOCKA "" "" "SOURCES;EXTRA_INCLUDES;EXTRA_LIBS" ${ARGN})
    add_executable(${target} ${CMOCKA_SOURCES})
    target_link_libraries(${target} PRIVATE cmocka-static ${CMOCKA_EXTRA_LIBS})
    target_compile_options(${target} PRIVATE ${TEST_C_FLAGS})
    target_include_directories(${target} PRIVATE ${CMOCKA_EXTRA_INCLUDES} ${UNIT_TEST_INCLUDE_DIRS})
endfunction()

add_cmocka_executable(ut_page
    SOURCES ${MOCK_TEST_SRC} ${MOCK_TEST_KERNEL_SRC}
)
target_compile_definitions(ut_page PRIVATE MOCK_TEST=1)
target_link_options(ut_page PRIVATE ${MOCK_COMPILE_OPTIONS})

add_cmocka_executable(ut_list
    SOURCES ${CMAKE_SOURCE_DIR}/src/ut_list_main.c
)

add_cmocka_executable(ut_hlist
    SOURCES ${CMAKE_SOURCE_DIR}/src/ut_hlist_main.c ${CMAKE_SOURCE_DIR}/../kernel/hlist.c
)

add_cmocka_executable(ut_rbtree
    SOURCES
        ${CMAKE_SOURCE_DIR}/src/ut_rbtree_main.c
        ${CMAKE_SOURCE_DIR}/../kernel/rbtree.c
        ${CMAKE_SOURCE_DIR}/../kernel/bintree.c
)

add_library(ut_threads STATIC ${CMAKE_SOURCE_DIR}/src/ut_thread.c)
target_include_directories(ut_threads PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ut_threads PUBLIC pthread)

add_cmocka_executable(ut_pcache
    SOURCES
        ${CMAKE_SOURCE_DIR}/src/ut_pcache_main.c
        ${CMAKE_SOURCE_DIR}/src/ut_pcache_stubs.c
        ${CMAKE_SOURCE_DIR}/src/panic_stubs.c
        ${CMAKE_SOURCE_DIR}/../kernel/mm/pcache.c
        ${CMAKE_SOURCE_DIR}/../kernel/rbtree.c
        ${CMAKE_SOURCE_DIR}/../kernel/bintree.c
    EXTRA_LIBS ut_threads
)

add_cmocka_executable(ut_semaphore
    SOURCES
        ${CMAKE_SOURCE_DIR}/src/ut_semaphore_main.c
        ${CMAKE_SOURCE_DIR}/../kernel/lock/semaphore.c
        ${CMAKE_SOURCE_DIR}/src/semaphore_runtime_stubs.c
    EXTRA_INCLUDES ${CMAKE_SOURCE_DIR}/include/semaphore_stubs
)

add_cmocka_executable(ut_rwlock
    SOURCES
        ${CMAKE_SOURCE_DIR}/src/ut_rwlock_main.c
        ${CMAKE_SOURCE_DIR}/../kernel/lock/rwlock.c
        ${CMAKE_SOURCE_DIR}/src/panic_stubs.c
    EXTRA_INCLUDES ${CMAKE_SOURCE_DIR}/include/semaphore_stubs
)

enable_testing()
add_test(NAME ut_mock_test
    COMMAND ut_page
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
foreach(CMOCKA_TARGET IN ITEMS ut_list ut_hlist ut_rbtree ut_pcache ut_semaphore ut_rwlock)
    add_test(NAME ${CMOCKA_TARGET}
        COMMAND ${CMOCKA_TARGET}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
endforeach()

set_tests_properties(ut_pcache PROPERTIES TIMEOUT 30)
