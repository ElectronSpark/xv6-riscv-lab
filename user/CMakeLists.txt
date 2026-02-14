if(NOT DEFINED ENV{LAB})
    message(FATAL_ERROR "Environment variable LAB is not set")
else()
    message(STATUS "Building user programs for lab: $ENV{LAB}")
endif()

# Optional newlib userland (independent from user/* libc sources)
option(USE_NEWLIB "Build newlib-based user programs" OFF)
option(USE_LOCAL_NEWLIB "Use local newlib source/install under user/v6-newlib" ON)
set(NEWLIB_XV6_DIR "${USER_DIR}/v6-newlib/newlib/libc/sys/xv6")
if(USE_NEWLIB)
    message(STATUS "Building with newlib libc support")
    if(USE_LOCAL_NEWLIB)
        message(STATUS "Using local newlib source/install")
    endif()
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
set(USER_LIB_C_SRC
    ${USER_DIR}/ulib.c
    ${USER_DIR}/printf.c
    ${USER_DIR}/umalloc.c
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

if(USE_NEWLIB)
    # Build xv6 newlib shim from local newlib tree
    add_subdirectory(v6-newlib/newlib/libc/sys/xv6 ${CMAKE_CURRENT_BINARY_DIR}/newlib_shim)
endif()

# Ensure proper linking and include directories for userlib
target_link_options(${LIB_USERLIB} PRIVATE
    "-z max-page-size=4096 -T ${USER_DIR}/user.ld")
target_include_directories(${LIB_USERLIB} PRIVATE ${USER_DIR})

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
	${USER_DIR}/mkdir.c
	${USER_DIR}/rm.c
	${USER_DIR}/sh.c
	${USER_DIR}/stressfs.c
	${USER_DIR}/usertests.c
	${USER_DIR}/grind.c
	${USER_DIR}/wc.c
	${USER_DIR}/zombie.c
	${USER_DIR}/sleep.c
    ${USER_DIR}/sync.c
	${USER_DIR}/pingpong.c
	${USER_DIR}/primes.c
	${USER_DIR}/find.c
	${USER_DIR}/xargs.c
    ${USER_DIR}/free.c
    ${USER_DIR}/ps.c
    ${USER_DIR}/dumpchan.c
    ${USER_DIR}/dumppcache.c
    ${USER_DIR}/dumprq.c
    ${USER_DIR}/dumpinode.c
    ${USER_DIR}/testsig.c
    ${USER_DIR}/cowtest.c
    ${USER_DIR}/clonetest.c
    ${USER_DIR}/vforktest.c
    ${USER_DIR}/devtest.c
    ${USER_DIR}/mount.c
    ${USER_DIR}/umount.c
    ${USER_DIR}/mknod.c
    ${USER_DIR}/randtest.c
    ${USER_DIR}/wallclock.c
    ${USER_DIR}/cloexectest.c
)

set(USER_PROGRAMS_C_FILES 
    ${USER_PROGRAMS_C_FILES}
    ${USER_DIR}/bigfile.c
    ${USER_DIR}/symlinktest.c
    ${USER_DIR}/mmaptest.c
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

# Modify user program output to match the .c file name without a suffix
foreach(PROGRAM ${USER_PROGRAMS_C_FILES})
    get_filename_component(PROGRAM_NAME ${PROGRAM} NAME_WE)
    list(APPEND USER_PROGRAMS _${PROGRAM_NAME})
    add_executable(_${PROGRAM_NAME} ${PROGRAM})
    target_link_options(_${PROGRAM_NAME} PRIVATE -z max-page-size=4096 -T ${USER_DIR}/user.ld)
    # Force linker to include all objects from userlib (including usys.S)
    target_link_libraries(_${PROGRAM_NAME} PRIVATE
        "--whole-archive" ${LIB_USERLIB} "--no-whole-archive"
    )
    set_target_properties(_${PROGRAM_NAME} PROPERTIES OUTPUT_NAME _${PROGRAM_NAME})
endforeach()

# Modify the user_programs target to include all executables
add_custom_target(user_programs ALL
    DEPENDS ${USER_PROGRAMS}
    COMMENT "Building all user programs"
)

if(USE_NEWLIB)
    if(USE_LOCAL_NEWLIB)
        set(NEWLIB_LOCAL_ROOT "${CMAKE_SOURCE_DIR}/user/v6-newlib" CACHE PATH "Local newlib source root")
        set(NEWLIB_OUTPUT_ROOT "${CMAKE_BINARY_DIR}/newlib" CACHE PATH "Output root for local newlib artifacts" FORCE)
        set(NEWLIB_LOCAL_INSTALL "${NEWLIB_OUTPUT_ROOT}/install" CACHE PATH "Local newlib install prefix" FORCE)
        set(NEWLIB_LOCAL_BUILD "${NEWLIB_OUTPUT_ROOT}/build-local" CACHE PATH "Local newlib build directory" FORCE)
        set(NEWLIB_LOCAL_ARCH "rv64gc" CACHE STRING "Local newlib --with-arch")
        set(NEWLIB_LOCAL_ABI "lp64d" CACHE STRING "Local newlib --with-abi")

        set(NEWLIB_INCLUDE_DIR "${NEWLIB_LOCAL_ROOT}/newlib/libc/include")
        set(NEWLIB_TOP_INCLUDE_DIR "${NEWLIB_LOCAL_ROOT}/include")
        set(NEWLIB_LIB_DIR "${NEWLIB_LOCAL_INSTALL}/riscv64-unknown-elf/lib")
        set(NEWLIB_LIBC_A "${NEWLIB_LIB_DIR}/libc.a")
        set(NEWLIB_LIBM_A "${NEWLIB_LIB_DIR}/libm.a")

        if(NOT EXISTS "${NEWLIB_INCLUDE_DIR}/sys/stat.h")
            message(FATAL_ERROR "USE_LOCAL_NEWLIB is ON but local headers are missing: ${NEWLIB_INCLUDE_DIR}/sys/stat.h")
        endif()

        add_custom_command(
            OUTPUT ${NEWLIB_LIBC_A} ${NEWLIB_LIBM_A}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${NEWLIB_LOCAL_BUILD}
            COMMAND ${CMAKE_COMMAND} -E chdir ${NEWLIB_LOCAL_BUILD}
                    ${NEWLIB_LOCAL_ROOT}/configure
                    --target=riscv64-unknown-elf
                    --prefix=${NEWLIB_LOCAL_INSTALL}
                --disable-multilib
                --with-arch=${NEWLIB_LOCAL_ARCH}
                --with-abi=${NEWLIB_LOCAL_ABI}
                    --disable-nls
                    --disable-werror
            COMMAND ${CMAKE_COMMAND} -E chdir ${NEWLIB_LOCAL_BUILD} make -j1 all-target-newlib
            COMMAND ${CMAKE_COMMAND} -E chdir ${NEWLIB_LOCAL_BUILD} make install-target-newlib
                COMMAND ${CMAKE_COMMAND} -E make_directory ${NEWLIB_LOCAL_INSTALL}/riscv64-unknown-elf/include/sys
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${NEWLIB_XV6_DIR}/sys/dirent.h
                    ${NEWLIB_LOCAL_INSTALL}/riscv64-unknown-elf/include/sys/dirent.h
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${NEWLIB_XV6_DIR}/sys/random.h
                    ${NEWLIB_LOCAL_INSTALL}/riscv64-unknown-elf/include/sys/random.h
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${NEWLIB_XV6_DIR}/sys/utsname.h
                    ${NEWLIB_LOCAL_INSTALL}/riscv64-unknown-elf/include/sys/utsname.h
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${NEWLIB_XV6_DIR}/sys/termios.h
                    ${NEWLIB_LOCAL_INSTALL}/riscv64-unknown-elf/include/sys/termios.h
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${NEWLIB_XV6_DIR}/sys/ioctl.h
                    ${NEWLIB_LOCAL_INSTALL}/riscv64-unknown-elf/include/sys/ioctl.h
            DEPENDS ${NEWLIB_LOCAL_ROOT}/configure
            COMMENT "Building/installing local newlib libraries"
        )

        add_custom_target(local_newlib_install DEPENDS ${NEWLIB_LIBC_A} ${NEWLIB_LIBM_A})
    endif()

    # Get the toolchain sysroot for newlib headers/libraries
    execute_process(
        COMMAND ${CMAKE_C_COMPILER} -print-sysroot
        OUTPUT_VARIABLE TOOLCHAIN_SYSROOT
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT TOOLCHAIN_SYSROOT)
        get_filename_component(TOOLCHAIN_BIN_DIR ${CMAKE_C_COMPILER} DIRECTORY)
        get_filename_component(TOOLCHAIN_DIR ${TOOLCHAIN_BIN_DIR} DIRECTORY)
        set(TOOLCHAIN_SYSROOT "${TOOLCHAIN_DIR}/riscv64-unknown-elf")
    endif()

    execute_process(
        COMMAND ${CMAKE_C_COMPILER} -print-file-name=include
        OUTPUT_VARIABLE GCC_INCLUDE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    if(NOT USE_LOCAL_NEWLIB)
        set(NEWLIB_INCLUDE_DIR "${TOOLCHAIN_SYSROOT}/include")
        set(NEWLIB_TOP_INCLUDE_DIR "${TOOLCHAIN_SYSROOT}/include")
    endif()

    set(NEWLIB_CRT0_OBJ ${CMAKE_CURRENT_BINARY_DIR}/newlib_shim/crt0.o)

    function(add_newlib_program PROGRAM_NAME SOURCE_FILE)
        get_filename_component(SOURCE_BASENAME ${SOURCE_FILE} NAME_WE)
        set(OBJ_FILE ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${PROGRAM_NAME}.dir/${SOURCE_BASENAME}.c.o)
        set(PROGRAM_ELF ${CMAKE_BINARY_DIR}/user/${PROGRAM_NAME})

        add_custom_command(
            OUTPUT ${OBJ_FILE}
            COMMAND ${CMAKE_C_COMPILER}
                    -Wall -O0 -fno-omit-frame-pointer -ggdb -gdwarf-2
                    -mcmodel=medany -fno-common -fno-pie -no-pie
                    -D_LARGEFILE_SOURCE=1
                    -D_LARGEFILE64_SOURCE=1
                    -D_FILE_OFFSET_BITS=64
                    -D__LARGE64_FILES=1
                    -nostdinc -ffreestanding
                    -isystem ${NEWLIB_XV6_DIR}
                    -isystem ${NEWLIB_XV6_DIR}/include
                    -isystem ${GCC_INCLUDE_DIR}
                    -isystem ${NEWLIB_TOP_INCLUDE_DIR}
                    -isystem ${NEWLIB_INCLUDE_DIR}
                    -MD -MF ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${PROGRAM_NAME}.dir/${SOURCE_BASENAME}.c.d
                    -o ${OBJ_FILE}
                    -c ${SOURCE_FILE}
            DEPENDS ${SOURCE_FILE}
            COMMENT "Building newlib C object ${SOURCE_BASENAME}.c"
        )

        file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${PROGRAM_NAME}.dir)

        add_custom_command(
            OUTPUT ${PROGRAM_ELF}
            COMMAND ${CMAKE_C_COMPILER}
                    -nostdlib
                    -Wl,-z,max-page-size=4096
                    -T ${USER_DIR}/user.ld
                    -o ${PROGRAM_ELF}
                    ${NEWLIB_CRT0_OBJ}
                    ${OBJ_FILE}
                    -Wl,--whole-archive
                    ${CMAKE_CURRENT_BINARY_DIR}/newlib_shim/libxv6_newlib_syscalls.a
                    -Wl,--no-whole-archive
                    $<$<BOOL:${USE_LOCAL_NEWLIB}>:${NEWLIB_LIBC_A}>
                    $<$<BOOL:${USE_LOCAL_NEWLIB}>:${NEWLIB_LIBM_A}>
                    $<$<NOT:$<BOOL:${USE_LOCAL_NEWLIB}>>:-L${TOOLCHAIN_SYSROOT}/lib>
                    $<$<NOT:$<BOOL:${USE_LOCAL_NEWLIB}>>:-lc>
                    $<$<NOT:$<BOOL:${USE_LOCAL_NEWLIB}>>:-lm>
                    -lgcc
                    DEPENDS ${OBJ_FILE} xv6_newlib_syscalls $<$<BOOL:${USE_LOCAL_NEWLIB}>:local_newlib_install>
            COMMENT "Linking newlib program ${PROGRAM_NAME}"
        )

        add_custom_target(${PROGRAM_NAME}_newlib ALL DEPENDS ${PROGRAM_ELF})

        list(APPEND USER_PROGRAMS ${PROGRAM_NAME})
        set(USER_PROGRAMS ${USER_PROGRAMS} PARENT_SCOPE)
    endfunction()

    add_newlib_program(_nwclock ${NEWLIB_XV6_DIR}/src/misc/newlib_wallclock.c)
    add_newlib_program(_newlibtest ${NEWLIB_XV6_DIR}/src/misc/newlibtest.c)
    add_newlib_program(_ntypesize ${NEWLIB_XV6_DIR}/src/misc/newlib_typesizes.c)

    if(USE_LOCAL_NEWLIB)
        add_dependencies(user_programs local_newlib_install)
    endif()
    add_dependencies(user_programs _nwclock_newlib _newlibtest_newlib _ntypesize_newlib)
endif()

# Pass the populated USER_PROGRAMS variable to the parent scope
set(USER_PROGRAMS ${USER_PROGRAMS} PARENT_SCOPE)
