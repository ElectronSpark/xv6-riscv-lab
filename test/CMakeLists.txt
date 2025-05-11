# CMake configuration for the test directory

# use gcc on the host system
set(CMAKE_c_COMPILER /usr/bin/gcc)

ENABLE_LANGUAGE(C)

# Define the executable(s) to be built
set(PROGRAMS list_test)

# Define the source files
set(SRC
    common.h
    ../kernel/list.h
    ../kernel/types.h
)


# Add an executable for each program
foreach(PROGRAM ${PROGRAMS})
    add_executable(${PROGRAM} ${PROGRAM}.c ${SRC})
    target_compile_options(${PROGRAM} PRIVATE -ggdb -Wall)
    target_link_options(${PROGRAM} PRIVATE -ggdb)
endforeach()

# Add a custom target for running tests
set(RUN_TEST_COMMANDS "")
foreach(PROGRAM ${PROGRAMS})
    list(APPEND RUN_TEST_COMMANDS COMMAND echo "Running test ${PROGRAM}")
    list(APPEND RUN_TEST_COMMANDS COMMAND ./${PROGRAM})
endforeach()

add_custom_target(test
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
