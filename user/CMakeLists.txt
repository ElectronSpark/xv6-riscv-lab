if(NOT DEFINED ENV{LAB})
    message(FATAL_ERROR "Environment variable LAB is not set")
else()
    message(STATUS "Building user programs for lab: $ENV{LAB}")
endif()

set(USER_INITCODE_BIN initcode)
set(USER_INITCODE_OBJ ${USER_INITCODE_BIN}.out)
set(USER_INITCODE_DECOMPILED ${USER_INITCODE_BIN}.asm)
set(LIB_USERLIB userlib)

# init code
set(KERNEL_INITCODE_FILES ${USER_DIR}/initcode.S)
FILE(GLOB KERNEL_INITCODE_SRC ${KERNEL_INITCODE_FILES})
set_source_files_properties(${KERNEL_INITCODE_SRC} PROPERTIES LANGUAGE C)
add_executable(${USER_INITCODE_OBJ} ${KERNEL_INITCODE_SRC})
target_compile_options(${USER_INITCODE_OBJ} PRIVATE 
-march=rv64g -nostdinc -I${CMAKE_SOURCE_DIR} -I${KERNEL_DIR})
set_target_properties(${USER_INITCODE_OBJ} PROPERTIES LINK_FLAGS
"-N -e start -Ttext 0x1000")

add_custom_target(${USER_INITCODE_BIN}
    COMMAND ${CMAKE_OBJCOPY} -S -O binary ${USER_INITCODE_OBJ} ${USER_INITCODE_BIN}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Creating init code ${USER_INITCODE_BIN}"
    DEPENDS ${USER_INITCODE_OBJ}
)

# decompile init code
add_custom_target(${USER_INITCODE_DECOMPILED}
    COMMAND ${CMAKE_OBJDUMP} -S ${USER_INITCODE_OBJ} > ${USER_INITCODE_DECOMPILED}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Decompiling init code binary to ${USER_INITCODE_DECOMPILED}"
    DEPENDS ${USER_INITCODE_OBJ}
)


# add user libraries
set(LIBC_DIR ${USER_DIR}/libc)

set(USER_LIB_C_SRC
    ${USER_DIR}/ulib.c
    ${USER_DIR}/printf.c
    ${USER_DIR}/umalloc.c
    ${LIBC_DIR}/src/musl_port_stubs.c
    ${LIBC_DIR}/src/errno/errno.c
)
if($ENV{LAB} STREQUAL "lock")
    set(USER_LIB_C_SRC 
        ${USER_LIB_C_SRC}
        ${USER_DIR}/statistics.c
    )
endif()

set_source_files_properties(${USER_LIB_C_SRC} PROPERTIES LANGUAGE C)

# Add a custom command to generate usys.S from usys.pl
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/usys.S
    COMMAND perl ${USER_DIR}/usys.pl > ${CMAKE_CURRENT_BINARY_DIR}/usys.S
    DEPENDS ${USER_DIR}/usys.pl
    COMMENT "Generating usys.S from usys.pl"
)

# Define a custom target to ensure usys.S is generated before anything else
add_custom_target(
    generate_usys ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/usys.S
)

# Mark usys.S as a generated file and include it in the user library
set(USER_LIB_S_SRC
    ${CMAKE_CURRENT_BINARY_DIR}/usys.S
)
set_source_files_properties(${USER_LIB_S_SRC} PROPERTIES GENERATED TRUE)
set_source_files_properties(${USER_LIB_S_SRC} PROPERTIES LANGUAGE C)

# Ensure userlib depends on the generation of usys.S
add_library(${LIB_USERLIB} STATIC 
    ${USER_LIB_C_SRC}
    ${USER_LIB_S_SRC}
)
add_dependencies(${LIB_USERLIB} generate_usys)

# Ensure proper linking and include directories for userlib
target_link_options(${LIB_USERLIB} PRIVATE
    "-z max-page-size=4096 -T ${USER_DIR}/user.ld")
target_include_directories(${LIB_USERLIB} PUBLIC ${USER_DIR} ${LIBC_DIR}/include)

include(ExternalProject)

set(MUSL_VERSION 1.2.4)
set(MUSL_URL https://musl.libc.org/releases/musl-${MUSL_VERSION}.tar.gz)
set(MUSL_PREFIX ${CMAKE_BINARY_DIR}/musl)
set(MUSL_SOURCE_DIR ${MUSL_PREFIX}/src/musl-${MUSL_VERSION})
set(MUSL_BUILD_DIR ${MUSL_PREFIX}/build)
set(MUSL_INSTALL_DIR ${MUSL_PREFIX}/install)
set(MUSL_PATCH_FILE ${CMAKE_SOURCE_DIR}/user/libc/patches/musl-xv6-port.patch)
set(MUSL_LIBC_PATH ${MUSL_INSTALL_DIR}/lib/libc.a)

ExternalProject_Add(musl_external
    PREFIX            ${MUSL_PREFIX}
    URL               ${MUSL_URL}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    UPDATE_COMMAND    ""
    SOURCE_DIR        ${MUSL_SOURCE_DIR}
    BINARY_DIR        ${MUSL_BUILD_DIR}
    BUILD_IN_SOURCE   OFF
    PATCH_COMMAND     patch -d ${MUSL_SOURCE_DIR} -p0 --forward -i ${MUSL_PATCH_FILE}
    CONFIGURE_COMMAND CC=${CMAKE_C_COMPILER} AR=${CMAKE_AR} RANLIB=${CMAKE_RANLIB} ${MUSL_SOURCE_DIR}/configure --prefix=${MUSL_INSTALL_DIR} --host=riscv64 --target=riscv64 --disable-shared --enable-static
    BUILD_COMMAND     ${CMAKE_MAKE_PROGRAM}
    INSTALL_COMMAND   ${CMAKE_MAKE_PROGRAM} install
    BUILD_BYPRODUCTS  ${MUSL_LIBC_PATH}
)

add_library(musl_libc STATIC IMPORTED GLOBAL)
set_target_properties(musl_libc PROPERTIES IMPORTED_LOCATION ${MUSL_LIBC_PATH})
add_dependencies(musl_libc musl_external)
add_dependencies(${LIB_USERLIB} musl_external)

target_include_directories(${LIB_USERLIB} PUBLIC ${MUSL_INSTALL_DIR}/include)
target_compile_definitions(${LIB_USERLIB} PRIVATE XV6_MUSL_PORT=1)

execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-libgcc-file-name
    OUTPUT_VARIABLE RISCV_LIBGCC
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT RISCV_LIBGCC)
    message(FATAL_ERROR "Failed to locate libgcc for the RISC-V toolchain")
endif()

# Add user programs
set(USER_PROGRAMS_C_FILES
    ${USER_DIR}/cat.c
	${USER_DIR}/echo.c
    # @TODO:
	# ${USER_DIR}/forktest.c
	${USER_DIR}/grep.c
	${USER_DIR}/init.c
	${USER_DIR}/kill.c
	${USER_DIR}/ln.c
	${USER_DIR}/ls.c
	${USER_DIR}/mkdir.c
	${USER_DIR}/rm.c
	${USER_DIR}/sh.c
	${USER_DIR}/stressfs.c
	${USER_DIR}/usertests.c
	${USER_DIR}/grind.c
	${USER_DIR}/wc.c
	${USER_DIR}/zombie.c
	${USER_DIR}/sleep.c
	${USER_DIR}/pingpong.c
	${USER_DIR}/primes.c
	${USER_DIR}/find.c
	${USER_DIR}/xargs.c
    ${USER_DIR}/free.c
    ${USER_DIR}/ps.c
    ${USER_DIR}/dumpchan.c
    ${USER_DIR}/testsig.c
    ${USER_DIR}/cowtest.c
)

set(USER_PROGRAMS_C_FILES 
    ${USER_PROGRAMS_C_FILES}
    ${USER_DIR}/bigfile.c
    ${USER_DIR}/symlinktest.c
)


if($ENV{LAB} STREQUAL "lazy")
    set(USER_PROGRAMS_C_FILES 
        ${USER_PROGRAMS_C_FILES}
        ${USER_DIR}/lazytests.c
    )
endif()

if($ENV{LAB} STREQUAL "syscall")
    set(USER_PROGRAMS_C_FILES 
        ${USER_PROGRAMS_C_FILES}
        ${USER_DIR}/attack.c
        ${USER_DIR}/attacktest.c
        ${USER_DIR}/secret.c
    )
endif()

if($ENV{LAB} STREQUAL "lock")
    set(USER_PROGRAMS_C_FILES 
        ${USER_PROGRAMS_C_FILES}
        ${USER_DIR}/stats.c
        ${USER_DIR}/kalloctest.c
	    ${USER_DIR}/bcachetest.c
    )
endif()

if($ENV{LAB} STREQUAL "traps")
    set(USER_PROGRAMS_C_FILES 
        ${USER_PROGRAMS_C_FILES}
        ${USER_DIR}/call.c
        ${USER_DIR}/bttest.c
    )
endif()

# # @TODO:
# if($ENV{LAB} STREQUAL "thread")
#     set(USER_PROGRAMS_C_FILES 
#         ${USER_PROGRAMS_C_FILES}
#         ${USER_DIR}/uthread.c
#         ${USER_DIR}/uthread_switch.S
#     )
#     set_source_files_properties(${USER_DIR}/uthread_switch.S PROPERTIES LANGUAGE ASM)
#     add_executable(uthread_switch ${USER_DIR}/uthread_switch.S)
#     target_compile_options(uthread_switch PRIVATE 
#         -march=rv64g -nostdinc -I${CMAKE_SOURCE_DIR} -I${KERNEL_DIR})
#     set_target_properties(uthread_switch PROPERTIES LINK_FLAGS
#         "-N -e main -Ttext 0")
# endif()

if($ENV{LAB} STREQUAL "pgtbl")
    set(USER_PROGRAMS_C_FILES 
        ${USER_PROGRAMS_C_FILES}
        ${USER_DIR}/pgtbltest.c
    )
endif()

if($ENV{LAB} STREQUAL "mmap")
    set(USER_PROGRAMS_C_FILES 
        ${USER_PROGRAMS_C_FILES}
        ${USER_DIR}/mmaptest.c
    )
endif()

if($ENV{LAB} STREQUAL "net")
    set(USER_PROGRAMS_C_FILES 
        ${USER_PROGRAMS_C_FILES}
        ${USER_DIR}/nettest.c
    )
endif()

set(USER_PROGRAMS_C_FILES
    ${USER_PROGRAMS_C_FILES}
    ${USER_DIR}/hello_musl.c
)

# Modify user program output to match the .c file name without a suffix
foreach(PROGRAM ${USER_PROGRAMS_C_FILES})
    get_filename_component(PROGRAM_NAME ${PROGRAM} NAME_WE)
    list(APPEND USER_PROGRAMS _${PROGRAM_NAME})
    add_executable(_${PROGRAM_NAME} ${PROGRAM})
    target_link_options(_${PROGRAM_NAME} PRIVATE -z max-page-size=4096 -T ${USER_DIR}/user.ld)
    target_link_libraries(_${PROGRAM_NAME} PRIVATE
        "--whole-archive" ${LIB_USERLIB} "--no-whole-archive"
        musl_libc
        ${RISCV_LIBGCC}
    )
    set_target_properties(_${PROGRAM_NAME} PROPERTIES OUTPUT_NAME _${PROGRAM_NAME})
endforeach()

# Modify the user_programs target to include all executables
add_custom_target(user_programs ALL
    DEPENDS ${USER_PROGRAMS}
    COMMENT "Building all user programs"
)
add_dependencies(user_programs musl_external)

# Pass the populated USER_PROGRAMS variable to the parent scope
set(USER_PROGRAMS ${USER_PROGRAMS} PARENT_SCOPE)
