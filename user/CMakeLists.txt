if(NOT DEFINED ENV{LAB})
    message(FATAL_ERROR "Environment variable LAB is not set")
else()
    message(STATUS "Building user programs for lab: $ENV{LAB}")
endif()

# musl libc — the standard C library for xv6 user programs
include(${CMAKE_SOURCE_DIR}/cmake/musl.cmake)

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
    ${USER_DIR}/waitgdb.c
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


# ==============================================================================
# musl libc programs
# ==============================================================================
# Build musl-linked versions of core programs.
# These use standard POSIX headers from musl instead of kernel/inc/*.
# Source files are in user/musl-xv6/programs/ — adapted copies of the
# originals in user/ with kernel header references replaced by POSIX.
set(MUSL_PROG_DIR "${CMAKE_SOURCE_DIR}/user/musl-xv6/programs")

if(EXISTS "${MUSL_PROG_DIR}/cat.c")
    add_musl_program(mcat "${MUSL_PROG_DIR}/cat.c")
endif()
if(EXISTS "${MUSL_PROG_DIR}/echo.c")
    add_musl_program(mecho "${MUSL_PROG_DIR}/echo.c")
endif()
if(EXISTS "${MUSL_PROG_DIR}/grep.c")
    add_musl_program(mgrep "${MUSL_PROG_DIR}/grep.c")
endif()
if(EXISTS "${MUSL_PROG_DIR}/ls.c")
    add_musl_program(mls "${MUSL_PROG_DIR}/ls.c")
endif()
if(EXISTS "${MUSL_PROG_DIR}/wc.c")
    add_musl_program(mwc "${MUSL_PROG_DIR}/wc.c")
endif()
if(EXISTS "${MUSL_PROG_DIR}/mkdir.c")
    add_musl_program(mmkdir "${MUSL_PROG_DIR}/mkdir.c")
endif()
if(EXISTS "${MUSL_PROG_DIR}/rm.c")
    add_musl_program(mrm "${MUSL_PROG_DIR}/rm.c")
endif()
if(EXISTS "${MUSL_PROG_DIR}/ln.c")
    add_musl_program(mln "${MUSL_PROG_DIR}/ln.c")
endif()
if(EXISTS "${MUSL_PROG_DIR}/kill.c")
    add_musl_program(mkill "${MUSL_PROG_DIR}/kill.c")
endif()
if(EXISTS "${MUSL_PROG_DIR}/sleep.c")
    add_musl_program(msleep "${MUSL_PROG_DIR}/sleep.c")
endif()
if(EXISTS "${MUSL_PROG_DIR}/init.c")
    add_musl_program(minit "${MUSL_PROG_DIR}/init.c")
endif()
if(EXISTS "${MUSL_PROG_DIR}/sh.c")
    add_musl_program(msh "${MUSL_PROG_DIR}/sh.c")
endif()
if(EXISTS "${MUSL_PROG_DIR}/hello.c")
    add_musl_program(mhello "${MUSL_PROG_DIR}/hello.c")
endif()

# ==============================================================================
# ncurses — Terminal UI library (static, built against musl)
# ==============================================================================
set(NCURSES_SRC_DIR "${CMAKE_SOURCE_DIR}/user/ncurses")
set(NCURSES_BUILD_DIR "${CMAKE_BINARY_DIR}/ncurses/build-xv6")
set(NCURSES_LIB_A "${NCURSES_BUILD_DIR}/lib/libncurses.a")
set(NCURSES_INCLUDE_DIR "${NCURSES_BUILD_DIR}/include")
set(MUSL_COMPAT_DIR "${CMAKE_SOURCE_DIR}/user/musl-xv6/compat")

if(EXISTS "${NCURSES_SRC_DIR}/configure")
    add_custom_command(
        OUTPUT ${NCURSES_LIB_A}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${NCURSES_BUILD_DIR}
        COMMAND ${CMAKE_COMMAND} -E chdir ${NCURSES_BUILD_DIR}
                env
                CC=${CMAKE_C_COMPILER}
                AR=${CMAKE_AR}
                RANLIB=${CMAKE_RANLIB}
                CFLAGS=-O2\ -nostdinc\ -ffreestanding\ -isystem\ ${MUSL_INCLUDE_DIR}\ -isystem\ ${GCC_INCLUDE_DIR_MUSL}\ -isystem\ ${MUSL_COMPAT_DIR}
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
                    --with-fallbacks=xterm,xterm-256color,vt100,dumb
                    cf_cv_have_tcgetattr=yes
                    ac_cv_header_termios_h=yes
        COMMAND ${CMAKE_COMMAND} -E chdir ${NCURSES_BUILD_DIR} make -j1
        # Create ncurses.h symlink expected by CPython's py_curses.h
        COMMAND ${CMAKE_COMMAND} -E create_symlink curses.h ${NCURSES_INCLUDE_DIR}/ncurses.h
        DEPENDS musl_sysroot ${NCURSES_SRC_DIR}/configure ${NCURSES_SRC_DIR}/misc/terminfo.src
        COMMENT "Building ncurses static library (musl)"
    )

    add_custom_target(local_ncurses_build DEPENDS ${NCURSES_LIB_A})
else()
    message(STATUS "Skipping ncurses build (missing submodule at ${NCURSES_SRC_DIR})")
endif()

# ==============================================================================
# readline — Line editor library (static, built against musl + ncurses)
# ==============================================================================
set(READLINE_SRC_DIR "${CMAKE_SOURCE_DIR}/user/readline")
set(READLINE_BUILD_DIR "${CMAKE_BINARY_DIR}/readline/build-xv6")
set(READLINE_LIB_A "${READLINE_BUILD_DIR}/libreadline.a")
set(READLINE_INCLUDE_DIR "${READLINE_BUILD_DIR}")

if(EXISTS "${READLINE_SRC_DIR}/configure")
    add_custom_command(
        OUTPUT ${READLINE_LIB_A}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${READLINE_BUILD_DIR}
        COMMAND ${CMAKE_COMMAND} -E chdir ${READLINE_BUILD_DIR}
                env
                CC=${CMAKE_C_COMPILER}
                AR=${CMAKE_AR}
                RANLIB=${CMAKE_RANLIB}
                CFLAGS=-O2\ -nostdinc\ -ffreestanding\ -D_POSIX_VERSION=200809L\ -isystem\ ${MUSL_INCLUDE_DIR}\ -isystem\ ${GCC_INCLUDE_DIR_MUSL}\ -isystem\ ${MUSL_COMPAT_DIR}\ -I${NCURSES_INCLUDE_DIR}
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
        COMMENT "Building readline static library (musl)"
    )

    add_custom_target(local_readline_build DEPENDS ${READLINE_LIB_A})
else()
    message(STATUS "Skipping readline build (missing submodule at ${READLINE_SRC_DIR})")
endif()

# ==============================================================================
# dash — Debian Almquist Shell (built against musl)
# ==============================================================================
set(DASH_SRC_DIR "${CMAKE_SOURCE_DIR}/user/dash")
set(DASH_BUILD_DIR "${CMAKE_BINARY_DIR}/dash/build-xv6")
set(DASH_BIN "${DASH_BUILD_DIR}/src/dash")
set(DASH_FS_BIN "${CMAKE_BINARY_DIR}/user/_dash")

# libedit shim: maps BSD editline API → GNU readline for dash
set(HISTEDIT_SHIM_SRC "${CMAKE_SOURCE_DIR}/user/musl-xv6/compat/histedit_shim.c")
set(HISTEDIT_SHIM_DIR "${CMAKE_BINARY_DIR}/dash/libedit-shim")
set(HISTEDIT_SHIM_OBJ "${HISTEDIT_SHIM_DIR}/histedit_shim.o")
set(HISTEDIT_SHIM_A   "${HISTEDIT_SHIM_DIR}/libedit.a")

if(EXISTS "${DASH_SRC_DIR}/configure")

    # Build the libedit shim (depends on readline + ncurses builds)
    add_custom_command(
        OUTPUT ${HISTEDIT_SHIM_OBJ}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${HISTEDIT_SHIM_DIR}
        COMMAND ${CMAKE_C_COMPILER}
            -c -o ${HISTEDIT_SHIM_OBJ}
            -O2 -nostdinc -ffreestanding -fno-common -fno-pie -no-pie
            -mcmodel=medany -march=rv64gc -mabi=lp64d
            -isystem ${MUSL_INCLUDE_DIR}
            -isystem ${GCC_INCLUDE_DIR_MUSL}
            -isystem ${MUSL_COMPAT_DIR}
            -I${CMAKE_SOURCE_DIR}/user
            -I${NCURSES_INCLUDE_DIR}
            ${HISTEDIT_SHIM_SRC}
        DEPENDS ${HISTEDIT_SHIM_SRC} musl_sysroot local_readline_build local_ncurses_build
        COMMENT "Compiling libedit→readline shim for dash"
    )

    add_custom_command(
        OUTPUT ${HISTEDIT_SHIM_A}
        COMMAND ${CMAKE_AR} rcs ${HISTEDIT_SHIM_A} ${HISTEDIT_SHIM_OBJ}
        DEPENDS ${HISTEDIT_SHIM_OBJ}
        COMMENT "Creating libedit.a shim archive"
    )

    add_custom_target(libedit_shim DEPENDS ${HISTEDIT_SHIM_A})

    # Configure dash with libedit support (backed by our readline shim)
    add_custom_command(
        OUTPUT ${DASH_BUILD_DIR}/Makefile
        COMMAND ${CMAKE_COMMAND} -E make_directory ${DASH_BUILD_DIR}
        COMMAND ${CMAKE_COMMAND} -E chdir ${DASH_BUILD_DIR}
                env
                CC=${CMAKE_C_COMPILER}
                AR=${CMAKE_AR}
                RANLIB=${CMAKE_RANLIB}
                CC_FOR_BUILD=cc
                CFLAGS=-O2\ -nostdinc\ -ffreestanding\ -fno-common\ -fno-pie\ -no-pie\ -mcmodel=medany\ -DFLUSHERR=1\ -isystem\ ${MUSL_INCLUDE_DIR}\ -isystem\ ${GCC_INCLUDE_DIR_MUSL}\ -isystem\ ${MUSL_COMPAT_DIR}\ -I${CMAKE_SOURCE_DIR}/user\ -I${NCURSES_INCLUDE_DIR}
                LDFLAGS=-static\ -nostartfiles\ -nostdlib\ ${MUSL_CRT1_O}\ ${MUSL_CRTI_O}\ -Wl,-z,max-page-size=0x1000\ -T\ ${MUSL_LINKER_SCRIPT}\ -L${HISTEDIT_SHIM_DIR}\ -L${READLINE_BUILD_DIR}\ -L${NCURSES_BUILD_DIR}/lib
                LIBS=${HISTEDIT_SHIM_A}\ ${READLINE_LIB_A}\ ${NCURSES_LIB_A}\ ${MUSL_LIBC_A}\ ${LIBGCC_PATH}\ ${MUSL_CRTN_O}
                ${DASH_SRC_DIR}/configure
                    --host=riscv64-unknown-elf
                    --disable-lineno
                    --with-libedit
                    ac_cv_func_bsearch=yes
                    ac_cv_func_faccessat=yes
                    ac_cv_func_getpwnam=yes
                    ac_cv_func_getrlimit=yes
                    ac_cv_func_isalpha=yes
                    ac_cv_func_killpg=yes
                    ac_cv_func_memfd_create=yes
                    ac_cv_func_memrchr=yes
                    ac_cv_func_mempcpy=yes
                    ac_cv_func_sigsetmask=no
                    ac_cv_func_stpcpy=yes
                    ac_cv_func_strchrnul=yes
                    ac_cv_func_strsignal=yes
                    ac_cv_func_strtod=yes
                    ac_cv_func_strtoimax=yes
                    ac_cv_func_strtoumax=yes
                    ac_cv_func_sysconf=yes
                    ac_cv_func_tee=yes
                    ac_cv_func_wait3=yes
                    ac_cv_func_signal=yes
                    ac_cv_header_alloca_h=yes
                    ac_cv_header_paths_h=yes
                    ac_cv_header_histedit_h=yes
                    ac_cv_header_sys_wait_h=yes
                    ac_cv_lib_edit_history_init=yes
                    ac_cv_sizeof_intmax_t=8
                    ac_cv_sizeof_long_long_int=8
                    dash_cv_have_attribute_alias=yes
        DEPENDS ${DASH_SRC_DIR}/configure musl_sysroot ${HISTEDIT_SHIM_A}
        COMMENT "Configuring dash cross build (musl, with readline-based libedit)"
    )

    add_custom_target(dash_configure DEPENDS ${DASH_BUILD_DIR}/Makefile)
    add_dependencies(dash_configure libedit_shim)

    add_custom_command(
        OUTPUT ${DASH_BIN}
        COMMAND ${CMAKE_COMMAND} -E chdir ${DASH_BUILD_DIR} make -j16 V=1
        DEPENDS dash_configure ${HISTEDIT_SHIM_A}
        COMMENT "Building dash shell (musl, with readline)"
    )

    add_custom_target(dash_build DEPENDS ${DASH_BIN})

    add_custom_command(
        OUTPUT ${DASH_FS_BIN}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/user
        COMMAND ${CMAKE_COMMAND} -E copy ${DASH_BIN} ${DASH_FS_BIN}
        COMMAND ${CMAKE_STRIP} --strip-debug ${DASH_FS_BIN}
        DEPENDS ${DASH_BIN}
        COMMENT "Staging dash shell as _dash for fs image"
    )

    add_custom_target(_dash_musl ALL DEPENDS ${DASH_FS_BIN})
    add_dependencies(user_programs _dash_musl)
    list(APPEND USER_PROGRAMS _dash)
    set(USER_PROGRAMS ${USER_PROGRAMS} PARENT_SCOPE)
else()
    message(STATUS "Skipping dash build (missing submodule at ${DASH_SRC_DIR})")
endif()

# ==============================================================================
# kqueue compat library for musl (provides kqueue/kevent/kevent_register/kevent_wait)
# ==============================================================================
set(KQUEUE_COMPAT_SRC "${CMAKE_SOURCE_DIR}/user/musl-xv6/compat/kqueue.c")
set(KQUEUE_COMPAT_OBJ "${CMAKE_BINARY_DIR}/user/kqueue_compat.o")
add_custom_command(
    OUTPUT ${KQUEUE_COMPAT_OBJ}
    COMMAND ${CMAKE_C_COMPILER}
            -c -o ${KQUEUE_COMPAT_OBJ}
            -isystem ${MUSL_INCLUDE_DIR}
            -isystem ${GCC_INCLUDE_DIR_MUSL}
            -isystem ${MUSL_COMPAT_DIR}
            -nostdinc -O2
            ${KQUEUE_COMPAT_SRC}
    DEPENDS ${KQUEUE_COMPAT_SRC} musl_sysroot
    COMMENT "Compiling kqueue compat object for musl"
)
add_custom_target(kqueue_compat DEPENDS ${KQUEUE_COMPAT_OBJ})

# CPython — Python 3.12 interpreter (built against musl + ncurses + readline)
# ==============================================================================
set(CPYTHON_SRC_DIR "${CMAKE_SOURCE_DIR}/user/v6-cpython" CACHE PATH "CPython source directory")
set(CPYTHON_BUILD_DIR "${CMAKE_BINARY_DIR}/cpython/build-3.12" CACHE PATH "CPython out-of-tree build directory")
set(CPYTHON_BIN "${CPYTHON_BUILD_DIR}/python" CACHE FILEPATH "CPython interpreter binary path")
set(CPYTHON_FS_BIN "${CMAKE_BINARY_DIR}/user/_python")

if(EXISTS "${CPYTHON_SRC_DIR}/configure")
    # Use the linux-gnu cross-compiler for CPython because it supports
    # -shared (needed for building libpython3.12.so and .so extension modules).
    # The bare-metal riscv64-unknown-elf-gcc linker cannot produce shared objects.
    set(CPYTHON_CC "riscv64-linux-gnu-gcc")
    set(CPYTHON_AR "riscv64-linux-gnu-ar")
    set(CPYTHON_RANLIB "riscv64-linux-gnu-ranlib")
    set(CPYTHON_READELF "riscv64-linux-gnu-readelf")
    set(CPYTHON_STRIP "riscv64-linux-gnu-strip")
    # GCC include dir for the linux-gnu toolchain (stdarg.h, etc.)
    execute_process(
        COMMAND riscv64-linux-gnu-gcc -print-file-name=include
        OUTPUT_VARIABLE CPYTHON_GCC_INCLUDE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    # libgcc.a path for the linux-gnu toolchain
    execute_process(
        COMMAND riscv64-linux-gnu-gcc -print-libgcc-file-name
        OUTPUT_VARIABLE CPYTHON_LIBGCC_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    add_custom_command(
        OUTPUT ${CPYTHON_BUILD_DIR}/Makefile
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CPYTHON_BUILD_DIR}
        COMMAND ${CMAKE_COMMAND} -E chdir ${CPYTHON_BUILD_DIR}
                env
                CC=${CPYTHON_CC}
                AR=${CPYTHON_AR}
                RANLIB=${CPYTHON_RANLIB}
                READELF=${CPYTHON_READELF}
                PKG_CONFIG=false
                CPPFLAGS=-isystem\ ${MUSL_INCLUDE_DIR}\ -isystem\ ${MUSL_COMPAT_DIR}\ -I${NCURSES_INCLUDE_DIR}\ -I${CMAKE_SOURCE_DIR}/user\ -D_POSIX_C_SOURCE=200809L\ -DSSIZE_MAX=__LONG_MAX__\ -DSA_ONSTACK=0\ -DPY_NO_THREAD_LOCAL=1
                CFLAGS=-isystem\ ${MUSL_INCLUDE_DIR}\ -isystem\ ${CPYTHON_GCC_INCLUDE_DIR}\ -isystem\ ${MUSL_COMPAT_DIR}\ -I${NCURSES_INCLUDE_DIR}\ -I${CMAKE_SOURCE_DIR}/user\ -D_POSIX_C_SOURCE=200809L\ -DSSIZE_MAX=__LONG_MAX__\ -DSA_ONSTACK=0\ -DPY_NO_THREAD_LOCAL=1
                LDFLAGS=-no-pie\ -nostartfiles\ -nostdlib\ ${MUSL_CRT1_O}\ ${MUSL_CRTI_O}\ -Wl,--export-dynamic\ -Wl,--dynamic-linker=/lib/ld-musl-riscv64.so.1\ -Wl,-z,max-page-size=0x1000\ -Wl,-z,common-page-size=0x1000\ -L${NCURSES_BUILD_DIR}/lib\ -L${READLINE_BUILD_DIR}\ -L${MUSL_SYSROOT}/lib
                LIBS=-lc\ ${CPYTHON_LIBGCC_PATH}
                ${CPYTHON_SRC_DIR}/configure
                    --host=riscv64-unknown-xv6
                    --build=x86_64-pc-linux-gnu
                    --with-build-python=python3
                    --disable-shared
                    --without-ensurepip
                    ac_cv_file__dev_ptmx=no
                    ac_cv_file__dev_ptc=no
                    ac_cv_header_curses_h=no
                    ac_cv_header_ncurses_h=no
                    ac_cv_header_term_h=no
                    ac_cv_lib_ncurses_initscr=no
                    ac_cv_lib_curses_filter=no
                    ac_cv_lib_curses_has_key=no
                    ac_cv_lib_curses_immedok=no
                    ac_cv_lib_curses_is_pad=no
                    ac_cv_lib_curses_is_term_resized=no
                    ac_cv_lib_curses_resizeterm=no
                    ac_cv_lib_curses_resize_term=no
                    ac_cv_lib_curses_syncok=no
                    ac_cv_lib_curses_typeahead=no
                    ac_cv_lib_curses_use_env=no
                    ac_cv_lib_curses_wchgat=no
                    ac_cv_lib_ncursesw_initscr=no
                    ac_cv_header_readline_readline_h=yes
                    ac_cv_lib_readline_readline=yes
                    ac_cv_readline_append_history=yes
                    ac_cv_readline_rl_completion_display_matches_hook=yes
                    ac_cv_readline_rl_completion_matches=yes
                    ac_cv_readline_rl_pre_input_hook=yes
                    ac_cv_readline_rl_resize_terminal=yes
                    ac_cv_header_netinet_in_h=yes
                    ac_cv_header_netinet_tcp_h=yes
                    ac_cv_header_arpa_inet_h=yes
                    ac_cv_header_netdb_h=yes
                    ac_cv_func_inet_aton=yes
                    ac_cv_func_inet_pton=yes
                    ac_cv_func_inet_ntop=yes
                    ac_cv_func_getaddrinfo=yes
                    ac_cv_func_getnameinfo=yes
                    ac_cv_func_gethostbyname=yes
                    ac_cv_header_sys_event_h=yes
                    ac_cv_func_kqueue=yes
                    ac_cv_header_dlfcn_h=yes
                    ac_cv_search_dlopen=none\ required
                    --disable-ipv6
        DEPENDS ${CPYTHON_SRC_DIR}/configure ${MUSL_LINKER_SCRIPT} musl_sysroot local_ncurses_build local_readline_build
        COMMENT "Configuring CPython cross build (musl)"
    )

    # After configure, write Modules/Setup.local to force all useful
    # C extension modules as statically-linked built-ins.  xv6 has no
    # dlopen, so anything left as *shared* in Setup.stdlib is unusable.
    set(CPYTHON_SETUP_LOCAL "${CPYTHON_BUILD_DIR}/Modules/Setup.local")
    set(SETUP_LOCAL_CONTENT [==[
*static*
readline readline.c -I@CMAKE_SOURCE_DIR@/user -L@READLINE_BUILD_DIR@ -lreadline -L@NCURSES_BUILD_DIR@/lib -lncurses
_socket socketmodule.c
select selectmodule.c
_struct _struct.c
math mathmodule.c
_json _json.c
array arraymodule.c
binascii binascii.c
fcntl fcntlmodule.c
_random _randommodule.c
_bisect _bisectmodule.c
_heapq _heapqmodule.c
_csv _csv.c
_pickle _pickle.c
_queue _queuemodule.c
_asyncio _asynciomodule.c
_contextvars _contextvarsmodule.c
_opcode _opcode.c
_datetime _datetimemodule.c
unicodedata unicodedata.c
pyexpat pyexpat.c
_elementtree _elementtree.c
_decimal _decimal/_decimal.c
_md5 md5module.c -I$(srcdir)/Modules/_hacl/include _hacl/Hacl_Hash_MD5.c -D_BSD_SOURCE -D_DEFAULT_SOURCE
_sha1 sha1module.c -I$(srcdir)/Modules/_hacl/include _hacl/Hacl_Hash_SHA1.c -D_BSD_SOURCE -D_DEFAULT_SOURCE
_sha2 sha2module.c -I$(srcdir)/Modules/_hacl/include Modules/_hacl/libHacl_Hash_SHA2.a
_sha3 sha3module.c -I$(srcdir)/Modules/_hacl/include _hacl/Hacl_Hash_SHA3.c -D_BSD_SOURCE -D_DEFAULT_SOURCE
_blake2 _blake2/blake2module.c _blake2/blake2b_impl.c _blake2/blake2s_impl.c
]==])
    # Replace @VAR@ placeholders with actual values
    string(REPLACE "@CMAKE_SOURCE_DIR@" "${CMAKE_SOURCE_DIR}" SETUP_LOCAL_CONTENT "${SETUP_LOCAL_CONTENT}")
    string(REPLACE "@READLINE_BUILD_DIR@" "${READLINE_BUILD_DIR}" SETUP_LOCAL_CONTENT "${SETUP_LOCAL_CONTENT}")
    string(REPLACE "@NCURSES_BUILD_DIR@" "${NCURSES_BUILD_DIR}" SETUP_LOCAL_CONTENT "${SETUP_LOCAL_CONTENT}")
    file(WRITE "${CMAKE_BINARY_DIR}/cpython_setup_local.txt" "${SETUP_LOCAL_CONTENT}")
    add_custom_command(
        OUTPUT ${CPYTHON_BUILD_DIR}/Makefile
        COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_BINARY_DIR}/cpython_setup_local.txt"
                ${CPYTHON_SETUP_LOCAL}
        APPEND
    )

    add_custom_target(cpython_configure DEPENDS ${CPYTHON_BUILD_DIR}/Makefile)

    # Build python interpreter. With --disable-shared, libpython is static  
    # but the binary is dynamically linked to libc.so (enabling dlopen).
    # --export-dynamic exports Python C API symbols for extension .so modules.
    add_custom_command(
        OUTPUT ${CPYTHON_BIN}
        COMMAND ${CMAKE_COMMAND} -E chdir ${CPYTHON_BUILD_DIR} make -j16 python
                "SYSLIBS=${KQUEUE_COMPAT_OBJ} -lc ${CPYTHON_LIBGCC_PATH} ${MUSL_CRTN_O}"
        DEPENDS cpython_configure kqueue_compat
        COMMENT "Building CPython interpreter (musl, dynamic libc)"
    )

    add_custom_target(cpython_build DEPENDS ${CPYTHON_BIN})

    # Stage the python binary for the rootfs image
    add_custom_command(
        OUTPUT ${CPYTHON_FS_BIN}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/user
        COMMAND ${CMAKE_COMMAND} -E copy ${CPYTHON_BIN} ${CPYTHON_FS_BIN}
        COMMAND ${CPYTHON_STRIP} --strip-debug ${CPYTHON_FS_BIN}
        DEPENDS ${CPYTHON_BIN}
        COMMENT "Staging CPython interpreter for fs image"
    )

    add_custom_target(_python_musl ALL DEPENDS ${CPYTHON_FS_BIN})
    add_dependencies(user_programs _python_musl)
    list(APPEND USER_PROGRAMS _python)
    set(USER_PROGRAMS ${USER_PROGRAMS} PARENT_SCOPE)
else()
    message(STATUS "Skipping CPython configure/build targets (missing submodule at ${CPYTHON_SRC_DIR})")
endif()

# ==============================================================================
# ncurses-enhanced shell (sh.c compiled with USE_NCURSES_SHELL against musl)
# ==============================================================================
if(EXISTS "${NCURSES_SRC_DIR}/configure")
    set(NCURSES_SH_SRC "${USER_DIR}/sh.c")
    set(NCURSES_SH_OBJ "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/_sh_musl.dir/sh.c.o")
    set(NCURSES_SH_ELF "${CMAKE_BINARY_DIR}/user/_sh")

    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/_sh_musl.dir)

    add_custom_command(
        OUTPUT ${NCURSES_SH_OBJ}
        COMMAND ${CMAKE_C_COMPILER}
            -Wall -O0 -fno-omit-frame-pointer -ggdb -gdwarf-2
            -march=rv64gc -mabi=lp64d
            -mcmodel=medany -fno-common -fno-pie -no-pie
            -DUSE_NCURSES_SHELL=1
            -nostdinc -ffreestanding
            -isystem ${MUSL_INCLUDE_DIR}
            -isystem ${GCC_INCLUDE_DIR_MUSL}
            -isystem ${NCURSES_INCLUDE_DIR}
            -MD -MF ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/_sh_musl.dir/sh.c.d
            -o ${NCURSES_SH_OBJ}
            -c ${NCURSES_SH_SRC}
        DEPENDS ${NCURSES_SH_SRC} musl_sysroot local_ncurses_build
        COMMENT "Building ncurses shell object (musl)"
    )

    add_custom_command(
        OUTPUT ${NCURSES_SH_ELF}
        COMMAND ${CMAKE_C_COMPILER}
            -nostdlib -static
            -Wl,-z,max-page-size=4096
            -T ${MUSL_LINKER_SCRIPT}
            -o ${NCURSES_SH_ELF}
            ${MUSL_CRT1_O}
            ${MUSL_CRTI_O}
            ${NCURSES_SH_OBJ}
            ${NCURSES_LIB_A}
            ${MUSL_LIBC_A}
            ${LIBGCC_PATH}
            ${MUSL_CRTN_O}
        DEPENDS ${NCURSES_SH_OBJ} musl_sysroot local_ncurses_build ${MUSL_LINKER_SCRIPT}
        COMMENT "Linking ncurses shell program _sh (musl)"
    )

    add_custom_target(_sh_musl ALL DEPENDS ${NCURSES_SH_ELF})
    add_dependencies(user_programs _sh_musl)
    # Override the userlib-based _sh with the ncurses+musl version
    list(REMOVE_ITEM USER_PROGRAMS _sh)
    list(APPEND USER_PROGRAMS _sh)
    set(USER_PROGRAMS ${USER_PROGRAMS} PARENT_SCOPE)
endif()

# Pass the populated USER_PROGRAMS variable to the parent scope
set(USER_PROGRAMS ${USER_PROGRAMS} PARENT_SCOPE)
