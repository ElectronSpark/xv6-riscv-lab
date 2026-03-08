# ==============================================================================
# arch/riscv/cmake/musl.cmake — Build musl libc for xv6 RISC-V user programs
# ==============================================================================
#
# This module:
#   1. Builds musl from source with xv6 RISC-V arch overlay
#   2. Provides add_musl_program() to compile/link a static musl program
#   3. Provides add_musl_dynamic_program() to compile/link a dynamic musl program
#   4. Creates musl_sysroot target as a dependency for all musl-based builds
#
# Usage:
#   include(${CMAKE_SOURCE_DIR}/arch/riscv/cmake/musl.cmake)
#

# ==============================================================================
# Paths and toolchain discovery
# ==============================================================================
set(MUSL_XV6_DIR "${CMAKE_SOURCE_DIR}/user/musl-xv6")
set(MUSL_BUILD_SCRIPT "${MUSL_XV6_DIR}/build_musl.sh")
set(MUSL_SYSROOT "${CMAKE_BINARY_DIR}/sysroot")
set(MUSL_BUILD_DIR "${CMAKE_BINARY_DIR}/musl-build")
set(MUSL_LIBC_A "${MUSL_SYSROOT}/lib/libc.a")
set(MUSL_CRT1_O "${MUSL_SYSROOT}/lib/crt1.o")
set(MUSL_CRTI_O "${MUSL_SYSROOT}/lib/crti.o")
set(MUSL_CRTN_O "${MUSL_SYSROOT}/lib/crtn.o")
set(MUSL_RCRT1_O "${MUSL_SYSROOT}/lib/rcrt1.o")
set(MUSL_SCRT1_O "${MUSL_SYSROOT}/lib/Scrt1.o")
set(MUSL_INCLUDE_DIR "${MUSL_SYSROOT}/include")
set(MUSL_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/user/musl.ld")
set(MUSL_DYNAMIC_LINKER_PATH "/lib/ld-musl-riscv64.so.1")

# Get GCC include dir for stdarg.h etc
execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-file-name=include
    OUTPUT_VARIABLE GCC_INCLUDE_DIR_MUSL
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Get libgcc.a path
execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-libgcc-file-name
    OUTPUT_VARIABLE LIBGCC_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

message(STATUS "musl libc: sysroot = ${MUSL_SYSROOT}")
message(STATUS "musl libc: GCC includes = ${GCC_INCLUDE_DIR_MUSL}")
message(STATUS "musl libc: libgcc = ${LIBGCC_PATH}")

# ==============================================================================
# Build musl from source using the xv6 overlay.
#
# Rebuilding into the workspace sysroot keeps the runtime loader and headers in
# sync with local ABI fixes instead of relying on a prebuilt toolchain sysroot.
# ==============================================================================
set(MUSL_ARCH_DEPS
    ${MUSL_XV6_DIR}/arch/riscv64/clone.s
    ${MUSL_XV6_DIR}/arch/riscv64/bits/syscall.h.in
    ${MUSL_XV6_DIR}/arch/riscv64/bits/stat.h
    ${MUSL_XV6_DIR}/arch/riscv64/kstat.h
)
set(MUSL_BUILD_COMMENT "Building musl libc for xv6 RISC-V (this may take a few minutes)...")

add_custom_command(
    OUTPUT ${MUSL_LIBC_A} ${MUSL_CRT1_O} ${MUSL_CRTI_O} ${MUSL_CRTN_O}
    COMMAND ${CMAKE_COMMAND} -E env
            CC=${CMAKE_C_COMPILER}
            AR=${CMAKE_AR}
            RANLIB=${CMAKE_RANLIB}
            bash ${MUSL_BUILD_SCRIPT} --prefix=${MUSL_SYSROOT} --build-dir=${MUSL_BUILD_DIR}
    DEPENDS
        ${MUSL_BUILD_SCRIPT}
        ${MUSL_ARCH_DEPS}
    COMMENT "${MUSL_BUILD_COMMENT}"
)

add_custom_target(musl_sysroot DEPENDS ${MUSL_LIBC_A} ${MUSL_CRT1_O})

# ==============================================================================
# Common compiler flags for musl-linked programs
# ==============================================================================
set(MUSL_COMMON_CFLAGS
    --sysroot=${MUSL_SYSROOT}
    -march=rv64gc -mabi=lp64d
    -mcmodel=medany -fno-pie -no-pie
    -nostdinc
    -isystem ${MUSL_INCLUDE_DIR}
    -isystem ${GCC_INCLUDE_DIR_MUSL}
)
set(MUSL_C_CFLAGS
    ${MUSL_COMMON_CFLAGS}
    -Wall -O${OPT_LEVEL} -fno-omit-frame-pointer -ggdb -gdwarf-2
    -fno-common -ffreestanding
)

# ==============================================================================
# add_musl_program(name source_file [EXTRA_SOURCES src1 src2 ...])
#
# Compile and link a user program against musl libc.
# The resulting executable is placed in ${CMAKE_BINARY_DIR}/user/_${name}
# ==============================================================================
function(add_musl_program PROGRAM_NAME SOURCE_FILE)
    cmake_parse_arguments(MUSL_PROG "" "" "EXTRA_SOURCES" ${ARGN})

    set(ALL_SOURCES ${SOURCE_FILE} ${MUSL_PROG_EXTRA_SOURCES})
    set(ALL_OBJECTS "")
    set(TARGET_NAME _${PROGRAM_NAME})
    set(PROGRAM_ELF ${CMAKE_BINARY_DIR}/user/${TARGET_NAME})

    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${TARGET_NAME}_musl.dir)

    foreach(SRC ${ALL_SOURCES})
        get_filename_component(SRC_BASENAME ${SRC} NAME_WE)
        get_filename_component(SRC_EXT ${SRC} EXT)
        set(OBJ_FILE ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${TARGET_NAME}_musl.dir/${SRC_BASENAME}${SRC_EXT}.o)
        list(APPEND ALL_OBJECTS ${OBJ_FILE})

        # Determine if this is assembly
        if(SRC_EXT STREQUAL ".S" OR SRC_EXT STREQUAL ".s")
            # Assembly source — minimal flags
            add_custom_command(
                OUTPUT ${OBJ_FILE}
                COMMAND ${CMAKE_C_COMPILER}
                        ${MUSL_COMMON_CFLAGS}
                        -o ${OBJ_FILE}
                        -c ${SRC}
                DEPENDS ${SRC} musl_sysroot
                COMMENT "Building musl ASM object ${SRC_BASENAME}${SRC_EXT}"
            )
        else()
            # C source — full debug + dependency tracking
            add_custom_command(
                OUTPUT ${OBJ_FILE}
                COMMAND ${CMAKE_C_COMPILER}
                        ${MUSL_C_CFLAGS}
                        -MD -MF ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${TARGET_NAME}_musl.dir/${SRC_BASENAME}${SRC_EXT}.d
                        -o ${OBJ_FILE}
                        -c ${SRC}
                DEPENDS ${SRC} musl_sysroot
                COMMENT "Building musl C object ${SRC_BASENAME}${SRC_EXT}"
            )
        endif()
    endforeach()

    # Link the program
    add_custom_command(
        OUTPUT ${PROGRAM_ELF}
        COMMAND ${CMAKE_C_COMPILER}
                -nostdlib -static
                -Wl,-z,max-page-size=4096
                -Wl,-z,common-page-size=4096
                -Wl,--build-id=none
                -T ${MUSL_LINKER_SCRIPT}
                -o ${PROGRAM_ELF}
                ${MUSL_CRT1_O}
                ${MUSL_CRTI_O}
                ${ALL_OBJECTS}
                ${MUSL_LIBC_A}
                ${LIBGCC_PATH}
                ${MUSL_CRTN_O}
        DEPENDS ${ALL_OBJECTS} musl_sysroot ${MUSL_LINKER_SCRIPT}
        COMMENT "Linking musl program ${TARGET_NAME}"
    )

    add_custom_target(${TARGET_NAME}_musl ALL DEPENDS ${PROGRAM_ELF})
    add_dependencies(user_programs ${TARGET_NAME}_musl)

    # Add to USER_PROGRAMS list in parent scope
    list(APPEND USER_PROGRAMS ${TARGET_NAME})
    set(USER_PROGRAMS ${USER_PROGRAMS} PARENT_SCOPE)
endfunction()

# ==============================================================================
# add_musl_dynamic_program(name source_file)
#
# Like add_musl_program() but links DYNAMICALLY against musl libc.so.
# The resulting executable uses ${MUSL_DYNAMIC_LINKER_PATH} as interpreter.
# ==============================================================================
function(add_musl_dynamic_program PROGRAM_NAME SOURCE_FILE)
    set(MUSL_ARCH "riscv64")
    set(TARGET_NAME _${PROGRAM_NAME})
    set(PROGRAM_ELF ${CMAKE_BINARY_DIR}/user/${TARGET_NAME})

    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${TARGET_NAME}_dymusl.dir)

    get_filename_component(SRC_BASENAME ${SOURCE_FILE} NAME_WE)
    get_filename_component(SRC_EXT ${SOURCE_FILE} EXT)
    set(OBJ_FILE ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${TARGET_NAME}_dymusl.dir/${SRC_BASENAME}${SRC_EXT}.o)

    add_custom_command(
        OUTPUT ${OBJ_FILE}
        COMMAND ${CMAKE_C_COMPILER}
                ${MUSL_C_CFLAGS}
                -o ${OBJ_FILE}
                -c ${SOURCE_FILE}
        DEPENDS ${SOURCE_FILE} musl_sysroot
        COMMENT "Building dynamic musl C object ${SRC_BASENAME}${SRC_EXT}"
    )

    # Link dynamically — use crt1.o + libc.so (not libc.a), with dynamic linker
    add_custom_command(
        OUTPUT ${PROGRAM_ELF}
        COMMAND ${CMAKE_C_COMPILER}
                -no-pie -nostartfiles -nostdlib
                ${MUSL_CRT1_O}
                ${MUSL_CRTI_O}
                -Wl,--dynamic-linker=${MUSL_DYNAMIC_LINKER_PATH}
                -Wl,-z,max-page-size=0x1000
                -Wl,-z,common-page-size=0x1000
                -Wl,--build-id=none
                -L${MUSL_SYSROOT}/lib
                -o ${PROGRAM_ELF}
                ${OBJ_FILE}
                -lc
                ${LIBGCC_PATH}
                ${MUSL_CRTN_O}
        DEPENDS ${OBJ_FILE} musl_sysroot
        COMMENT "Linking dynamic musl program ${TARGET_NAME}"
    )

    add_custom_target(${TARGET_NAME}_dymusl ALL DEPENDS ${PROGRAM_ELF})
    add_dependencies(user_programs ${TARGET_NAME}_dymusl)

    list(APPEND USER_PROGRAMS ${TARGET_NAME})
    set(USER_PROGRAMS ${USER_PROGRAMS} PARENT_SCOPE)
endfunction()
