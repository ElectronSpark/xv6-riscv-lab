if(NOT DEFINED ENV{LAB})
    message(FATAL_ERROR "Environment variable LAB is not set")
else()
    message(STATUS "Building user programs for lab: $ENV{LAB}")
endif()

# Optional newlib userland (independent from user/* libc sources)
option(USE_NEWLIB "Build newlib-based user programs" ON)
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

# Build shell with newlib+ncurses when available.
if(USE_NEWLIB AND EXISTS "${CMAKE_SOURCE_DIR}/user/ncurses/configure")
    list(REMOVE_ITEM USER_PROGRAMS_C_FILES ${USER_DIR}/sh.c)
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
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${NEWLIB_XV6_DIR}/sgtty.h
                    ${NEWLIB_LOCAL_INSTALL}/riscv64-unknown-elf/include/sgtty.h
            DEPENDS ${NEWLIB_LOCAL_ROOT}/configure
            COMMENT "Building/installing local newlib libraries"
        )

        add_custom_target(local_newlib_install DEPENDS ${NEWLIB_LIBC_A} ${NEWLIB_LIBM_A})

        add_custom_target(newlib_header_compile_check
            COMMAND ${CMAKE_C_COMPILER}
                    -Wall -Werror
                    -isystem ${NEWLIB_LOCAL_INSTALL}/riscv64-unknown-elf/include
                    -fsyntax-only
                    ${NEWLIB_XV6_DIR}/src/misc/newlib_header_probe.c
            DEPENDS local_newlib_install
            COMMENT "Compiling newlib header probe (termios/ioctl/signal)"
        )
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
                    -isystem ${CMAKE_SOURCE_DIR}
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
    add_newlib_program(_nmalloc ${NEWLIB_XV6_DIR}/src/misc/newlib_malloc_smoke.c)

    set(NCURSES_SRC_DIR "${CMAKE_SOURCE_DIR}/user/ncurses")
    set(NCURSES_BUILD_DIR "${CMAKE_BINARY_DIR}/ncurses/build-xv6")
    set(NCURSES_LIB_A "${NCURSES_BUILD_DIR}/lib/libncurses.a")
    set(NCURSES_INCLUDE_DIR "${NCURSES_BUILD_DIR}/include")
    set(NCURSES_SMOKE_SRC "${NEWLIB_XV6_DIR}/src/misc/newlib_ncurses_smoke.c")
    set(NCURSES_SMOKE_OBJ "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/_nncurses_smoke.dir/newlib_ncurses_smoke.c.o")
    set(NCURSES_SMOKE_ELF "${CMAKE_BINARY_DIR}/user/_nncurses_smoke")

    set(READLINE_SRC_DIR "${CMAKE_SOURCE_DIR}/user/readline")
    set(READLINE_BUILD_DIR "${CMAKE_BINARY_DIR}/readline/build-xv6")
    set(READLINE_LIB_A "${READLINE_BUILD_DIR}/libreadline.a")
    set(READLINE_INCLUDE_DIR "${READLINE_BUILD_DIR}")

    # CPython source is a submodule under user/, while build artifacts go under
    # top-level build/ to keep the submodule tree clean.
    set(CPYTHON_SRC_DIR "${CMAKE_SOURCE_DIR}/user/v6-cpython" CACHE PATH "CPython source directory")
    set(CPYTHON_BUILD_DIR "${CMAKE_BINARY_DIR}/cpython/build-3.12" CACHE PATH "CPython out-of-tree build directory")
    set(CPYTHON_BIN "${CPYTHON_BUILD_DIR}/python" CACHE FILEPATH "CPython interpreter binary path")
    set(CPYTHON_FS_BIN "${CMAKE_BINARY_DIR}/user/_python")

    if(EXISTS "${CPYTHON_SRC_DIR}/configure")
        add_custom_command(
            OUTPUT ${CPYTHON_BUILD_DIR}/Makefile
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CPYTHON_BUILD_DIR}
            COMMAND ${CMAKE_COMMAND} -E chdir ${CPYTHON_BUILD_DIR}
                    env
                    CC=${CMAKE_C_COMPILER}
                    AR=${CMAKE_AR}
                    RANLIB=${CMAKE_RANLIB}
                    READELF=${CMAKE_READELF}
                    CPPFLAGS=-I${NEWLIB_XV6_DIR}/include\ -I${NCURSES_INCLUDE_DIR}\ -I${CMAKE_SOURCE_DIR}/user\ -D_POSIX_C_SOURCE=200809L\ -DSSIZE_MAX=__LONG_MAX__\ -DSA_ONSTACK=0\ -DPY_NO_THREAD_LOCAL=1
                    CFLAGS=-I${NEWLIB_XV6_DIR}/include\ -I${NEWLIB_INCLUDE_DIR}\ -I${NCURSES_INCLUDE_DIR}\ -I${CMAKE_SOURCE_DIR}/user\ -D_POSIX_C_SOURCE=200809L\ -DSSIZE_MAX=__LONG_MAX__\ -DSA_ONSTACK=0\ -DPY_NO_THREAD_LOCAL=1
                    LDFLAGS=-Wl,-z,max-page-size=0x1000\ -Wl,-z,common-page-size=0x1000\ -Wl,-T,${USER_DIR}/python_pagealign.ld\ -L${NCURSES_BUILD_DIR}/lib\ -L${READLINE_BUILD_DIR}
                    LIBS=-L${CMAKE_CURRENT_BINARY_DIR}/newlib_shim\ -lxv6_newlib_syscalls\ -lpthread
                    ${CPYTHON_SRC_DIR}/configure
                        --host=riscv64-unknown-linux-gnu
                        --build=x86_64-pc-linux-gnu
                        --with-build-python=python3
                        # --without-pymalloc
                        --disable-shared
                        --without-ensurepip
                        ac_cv_file__dev_ptmx=no
                        ac_cv_file__dev_ptc=no
                        ac_cv_header_curses_h=yes
                        ac_cv_header_ncurses_h=yes
                        ac_cv_header_term_h=yes
                        ac_cv_lib_ncurses_initscr=yes
                        ac_cv_lib_curses_filter=yes
                        ac_cv_lib_curses_has_key=yes
                        ac_cv_lib_curses_immedok=yes
                        ac_cv_lib_curses_is_pad=yes
                        ac_cv_lib_curses_is_term_resized=yes
                        ac_cv_lib_curses_resizeterm=yes
                        ac_cv_lib_curses_resize_term=yes
                        ac_cv_lib_curses_syncok=yes
                        ac_cv_lib_curses_typeahead=yes
                        ac_cv_lib_curses_use_env=yes
                        ac_cv_lib_curses_wchgat=yes
                        ac_cv_lib_ncursesw_initscr=no
                        ac_cv_header_readline_readline_h=yes
                        ac_cv_lib_readline_readline=yes
                        ac_cv_readline_append_history=yes
                        ac_cv_readline_rl_completion_display_matches_hook=yes
                        ac_cv_readline_rl_completion_matches=yes
                        ac_cv_readline_rl_pre_input_hook=yes
                        ac_cv_readline_rl_resize_terminal=yes
            DEPENDS ${CPYTHON_SRC_DIR}/configure ${USER_DIR}/python_pagealign.ld xv6_newlib_syscalls xv6_pthread local_ncurses_build local_readline_build
            COMMENT "Configuring CPython cross build in ${CPYTHON_BUILD_DIR}"
        )

        # After configure, write Modules/Setup.local to enable _curses and
        # readline as statically-linked built-in modules.
        set(CPYTHON_SETUP_LOCAL "${CPYTHON_BUILD_DIR}/Modules/Setup.local")
        add_custom_command(
            OUTPUT ${CPYTHON_BUILD_DIR}/Makefile
            COMMAND ${CMAKE_COMMAND} -E echo
                    "_curses _cursesmodule.c -I${NCURSES_INCLUDE_DIR} -L${NCURSES_BUILD_DIR}/lib -lncurses"
                    > ${CPYTHON_SETUP_LOCAL}
            COMMAND ${CMAKE_COMMAND} -E echo
                    "readline readline.c -I${CMAKE_SOURCE_DIR}/user -L${READLINE_BUILD_DIR} -lreadline -L${NCURSES_BUILD_DIR}/lib -lncurses"
                    >> ${CPYTHON_SETUP_LOCAL}
            APPEND
        )

        add_custom_target(cpython_configure DEPENDS ${CPYTHON_BUILD_DIR}/Makefile)

        add_custom_command(
            OUTPUT ${CPYTHON_BIN}
            COMMAND ${CMAKE_COMMAND} -E chdir ${CPYTHON_BUILD_DIR} make -j16 python
            DEPENDS cpython_configure
            COMMENT "Building CPython interpreter"
        )

        add_custom_target(cpython_build DEPENDS ${CPYTHON_BIN})

        add_custom_command(
            OUTPUT ${CPYTHON_FS_BIN}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/user
            COMMAND ${CMAKE_COMMAND} -E copy ${CPYTHON_BIN} ${CPYTHON_FS_BIN}
            COMMAND ${CMAKE_STRIP} --strip-debug ${CPYTHON_FS_BIN}
            DEPENDS ${CPYTHON_BIN}
            COMMENT "Staging CPython interpreter as _python for fs image"
        )

        add_custom_target(_python_newlib ALL DEPENDS ${CPYTHON_FS_BIN})
        list(APPEND USER_PROGRAMS _python)
        set(USER_PROGRAMS ${USER_PROGRAMS} PARENT_SCOPE)
    else()
        message(STATUS "Skipping CPython configure/build targets (missing submodule at ${CPYTHON_SRC_DIR})")
    endif()

    if(EXISTS "${NCURSES_SRC_DIR}/configure")
        add_custom_command(
            OUTPUT ${NCURSES_LIB_A}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${NCURSES_BUILD_DIR}
            COMMAND ${CMAKE_COMMAND} -E chdir ${NCURSES_BUILD_DIR}
                    env
                    CC=${CMAKE_C_COMPILER}
                    AR=${CMAKE_AR}
                    RANLIB=${CMAKE_RANLIB}
                    CFLAGS=-O2\ -I${NEWLIB_LOCAL_INSTALL}/riscv64-unknown-elf/include
                    ${NCURSES_SRC_DIR}/configure
                        --host=riscv64-unknown-elf
                        --disable-widec
                        --without-shared
                        --without-debug
                        --without-ada
                        --without-cxx
                        --without-cxx-binding
                        --without-manpages
                        --without-progs
                        --without-tests
                        cf_cv_func_tcgetattr=yes
            COMMAND ${CMAKE_COMMAND} -E chdir ${NCURSES_BUILD_DIR} make -j1
            # Create ncurses.h symlink expected by CPython's py_curses.h
            COMMAND ${CMAKE_COMMAND} -E create_symlink curses.h ${NCURSES_INCLUDE_DIR}/ncurses.h
            DEPENDS local_newlib_install ${NCURSES_SRC_DIR}/configure ${NCURSES_SRC_DIR}/misc/terminfo.src
            COMMENT "Building local ncurses static library"
        )

        add_custom_target(local_ncurses_build DEPENDS ${NCURSES_LIB_A})

    if(EXISTS "${READLINE_SRC_DIR}/configure")
        add_custom_command(
            OUTPUT ${READLINE_LIB_A}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${READLINE_BUILD_DIR}
            COMMAND ${CMAKE_COMMAND} -E chdir ${READLINE_BUILD_DIR}
                    env
                    CC=${CMAKE_C_COMPILER}
                    AR=${CMAKE_AR}
                    RANLIB=${CMAKE_RANLIB}
                    CFLAGS=-O2\ -D_POSIX_VERSION=200809L\ -I${NEWLIB_XV6_DIR}/include\ -I${NEWLIB_LOCAL_INSTALL}/riscv64-unknown-elf/include\ -I${NCURSES_INCLUDE_DIR}
                    LDFLAGS=-L${NCURSES_BUILD_DIR}/lib
                    ${READLINE_SRC_DIR}/configure
                        --host=riscv64-unknown-elf
                        --disable-shared
                        --with-curses
                        --without-install
                        bash_cv_termcap_lib=libncurses
                        bash_cv_wcwidth_broken=no
                        bash_cv_func_sigsetjmp=missing
                        bash_cv_signal_vintage=posix
                        bash_cv_posix_signals=yes
                        bash_cv_func_strcoll_broken=no
                        ac_cv_header_termios_h=yes
                        ac_cv_func_tcgetattr=yes
                        ac_cv_func_pselect=yes
            COMMAND ${CMAKE_COMMAND} -E chdir ${READLINE_BUILD_DIR} make -j4
            DEPENDS local_ncurses_build ${READLINE_SRC_DIR}/configure
            COMMENT "Building local readline static library"
        )

        add_custom_target(local_readline_build DEPENDS ${READLINE_LIB_A})
    else()
        message(STATUS "Skipping readline build (missing submodule at ${READLINE_SRC_DIR})")
    endif()

    set(NCURSES_SH_SRC "${USER_DIR}/sh.c")
    set(NCURSES_SH_OBJ "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/_sh_newlib.dir/sh.c.o")
    set(NCURSES_SH_ELF "${CMAKE_BINARY_DIR}/user/_sh")

    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/_sh_newlib.dir)

    add_custom_command(
        OUTPUT ${NCURSES_SH_OBJ}
        COMMAND ${CMAKE_C_COMPILER}
            -Wall -O0 -fno-omit-frame-pointer -ggdb -gdwarf-2
            -mcmodel=medany -fno-common -fno-pie -no-pie
            -D_LARGEFILE_SOURCE=1
            -D_LARGEFILE64_SOURCE=1
            -D_FILE_OFFSET_BITS=64
            -D__LARGE64_FILES=1
            -DUSE_NCURSES_SHELL=1
            -nostdinc -ffreestanding
            -isystem ${NEWLIB_XV6_DIR}
            -isystem ${NEWLIB_XV6_DIR}/include
            -isystem ${GCC_INCLUDE_DIR}
            -isystem ${NEWLIB_TOP_INCLUDE_DIR}
            -isystem ${NEWLIB_INCLUDE_DIR}
            -isystem ${NCURSES_INCLUDE_DIR}
            -MD -MF ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/_sh_newlib.dir/sh.c.d
            -o ${NCURSES_SH_OBJ}
            -c ${NCURSES_SH_SRC}
        DEPENDS ${NCURSES_SH_SRC}
        COMMENT "Building ncurses shell object"
    )

    add_custom_command(
        OUTPUT ${NCURSES_SH_ELF}
        COMMAND ${CMAKE_C_COMPILER}
            -nostdlib
            -Wl,-z,max-page-size=4096
            -T ${USER_DIR}/user.ld
            -o ${NCURSES_SH_ELF}
            ${NEWLIB_CRT0_OBJ}
            ${NCURSES_SH_OBJ}
            -Wl,--whole-archive
            ${CMAKE_CURRENT_BINARY_DIR}/newlib_shim/libxv6_newlib_syscalls.a
            -Wl,--no-whole-archive
            ${NCURSES_LIB_A}
            $<$<BOOL:${USE_LOCAL_NEWLIB}>:${NEWLIB_LIBC_A}>
            $<$<BOOL:${USE_LOCAL_NEWLIB}>:${NEWLIB_LIBM_A}>
            $<$<NOT:$<BOOL:${USE_LOCAL_NEWLIB}>>:-L${TOOLCHAIN_SYSROOT}/lib>
            $<$<NOT:$<BOOL:${USE_LOCAL_NEWLIB}>>:-lc>
            $<$<NOT:$<BOOL:${USE_LOCAL_NEWLIB}>>:-lm>
            -lgcc
        DEPENDS ${NCURSES_SH_OBJ} xv6_newlib_syscalls local_ncurses_build $<$<BOOL:${USE_LOCAL_NEWLIB}>:local_newlib_install>
        COMMENT "Linking ncurses shell program _sh"
    )

    add_custom_target(_sh_newlib ALL DEPENDS ${NCURSES_SH_ELF})
    list(APPEND USER_PROGRAMS _sh)
    set(USER_PROGRAMS ${USER_PROGRAMS} PARENT_SCOPE)

        file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/_nncurses_smoke.dir)

        add_custom_command(
            OUTPUT ${NCURSES_SMOKE_OBJ}
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
                    -isystem ${NCURSES_INCLUDE_DIR}
                    -MD -MF ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/_nncurses_smoke.dir/newlib_ncurses_smoke.c.d
                    -o ${NCURSES_SMOKE_OBJ}
                    -c ${NCURSES_SMOKE_SRC}
            DEPENDS ${NCURSES_SMOKE_SRC}
            COMMENT "Building newlib ncurses smoke object"
        )

        add_custom_command(
            OUTPUT ${NCURSES_SMOKE_ELF}
            COMMAND ${CMAKE_C_COMPILER}
                    -nostdlib
                    -Wl,-z,max-page-size=4096
                    -T ${USER_DIR}/user.ld
                    -o ${NCURSES_SMOKE_ELF}
                    ${NEWLIB_CRT0_OBJ}
                    ${NCURSES_SMOKE_OBJ}
                    -Wl,--whole-archive
                    ${CMAKE_CURRENT_BINARY_DIR}/newlib_shim/libxv6_newlib_syscalls.a
                    -Wl,--no-whole-archive
                    ${NCURSES_LIB_A}
                    $<$<BOOL:${USE_LOCAL_NEWLIB}>:${NEWLIB_LIBC_A}>
                    $<$<BOOL:${USE_LOCAL_NEWLIB}>:${NEWLIB_LIBM_A}>
                    $<$<NOT:$<BOOL:${USE_LOCAL_NEWLIB}>>:-L${TOOLCHAIN_SYSROOT}/lib>
                    $<$<NOT:$<BOOL:${USE_LOCAL_NEWLIB}>>:-lc>
                    $<$<NOT:$<BOOL:${USE_LOCAL_NEWLIB}>>:-lm>
                    -lgcc
            DEPENDS ${NCURSES_SMOKE_OBJ} xv6_newlib_syscalls local_ncurses_build $<$<BOOL:${USE_LOCAL_NEWLIB}>:local_newlib_install>
            COMMENT "Linking newlib program _nncurses_smoke"
        )

        add_custom_target(_nncurses_smoke_newlib ALL DEPENDS ${NCURSES_SMOKE_ELF})
        list(APPEND USER_PROGRAMS _nncurses_smoke)
        set(USER_PROGRAMS ${USER_PROGRAMS} PARENT_SCOPE)
    else()
        message(STATUS "Skipping _nncurses_smoke_newlib (missing submodule at ${NCURSES_SRC_DIR})")
    endif()

    if(USE_LOCAL_NEWLIB)
        add_dependencies(user_programs local_newlib_install)
    endif()
    add_dependencies(user_programs _nwclock_newlib _newlibtest_newlib _ntypesize_newlib _nmalloc_newlib)
    if(TARGET _sh_newlib)
        add_dependencies(user_programs _sh_newlib)
    endif()
    if(TARGET _nncurses_smoke_newlib)
        add_dependencies(user_programs _nncurses_smoke_newlib)
    endif()
    if(TARGET _python_newlib)
        add_dependencies(user_programs _python_newlib)
    endif()
endif()

# Pass the populated USER_PROGRAMS variable to the parent scope
set(USER_PROGRAMS ${USER_PROGRAMS} PARENT_SCOPE)
