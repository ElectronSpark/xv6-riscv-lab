message(STATUS "Building user programs for lab: $ENV{LAB}")

# musl libc — the standard C library for xv6 user programs
include(${CMAKE_SOURCE_DIR}/arch/${ARCH}/cmake/musl.cmake)

# ==============================================================================
# Init Code (first user-mode binary, embedded in kernel)
# ==============================================================================
set(USER_INITCODE_BIN initcode)
set(USER_INITCODE_OBJ ${USER_INITCODE_BIN}.out)
set(USER_INITCODE_DECOMPILED ${USER_INITCODE_BIN}.asm)
set(LIB_USERLIB userlib)

if(ARCH STREQUAL "x86_64")
    set(USER_LINKER_SCRIPT ${USER_DIR}/x86_64/user.ld)
    set(USER_INITCODE_SRC ${USER_DIR}/x86_64/initcode.S)
    set(USER_USYS_PL ${USER_DIR}/x86_64/usys_x86_64.pl)
else()
    set(USER_LINKER_SCRIPT ${USER_DIR}/user.ld)
    set(USER_INITCODE_SRC ${USER_DIR}/initcode.S)
    set(USER_USYS_PL ${USER_DIR}/usys.pl)
endif()

set_source_files_properties(${USER_INITCODE_SRC} PROPERTIES LANGUAGE C)
add_executable(${USER_INITCODE_OBJ} ${USER_INITCODE_SRC})
if(ARCH STREQUAL "x86_64")
    target_compile_options(${USER_INITCODE_OBJ} PRIVATE
        -nostdinc -I${CMAKE_SOURCE_DIR} -I${KERNEL_DIR})
    set_target_properties(${USER_INITCODE_OBJ} PROPERTIES LINK_FLAGS
        "-N -e start -Ttext 0x1000")
else()
    target_compile_options(${USER_INITCODE_OBJ} PRIVATE
        -march=rv64g -nostdinc -I${CMAKE_SOURCE_DIR} -I${KERNEL_DIR})
    set_target_properties(${USER_INITCODE_OBJ} PROPERTIES LINK_FLAGS
        "-N -e start -Ttext 0x1000")
endif()

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
    list(APPEND USER_LIB_C_SRC ${USER_DIR}/statistics.c)
endif()

set_source_files_properties(${USER_LIB_C_SRC} PROPERTIES LANGUAGE C)

# Generate syscall stubs (usys.S) from usys.pl (arch-specific)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/usys.S
    COMMAND perl ${USER_USYS_PL} > ${CMAKE_CURRENT_BINARY_DIR}/usys.S
    DEPENDS ${USER_USYS_PL}
    COMMENT "Generating usys.S from ${USER_USYS_PL}"
)
add_custom_target(generate_usys ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/usys.S)

set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/usys.S PROPERTIES GENERATED TRUE LANGUAGE C)

add_library(${LIB_USERLIB} STATIC
    ${USER_LIB_C_SRC}
    ${CMAKE_CURRENT_BINARY_DIR}/usys.S
)
add_dependencies(${LIB_USERLIB} generate_usys)
target_link_options(${LIB_USERLIB} PRIVATE "-z max-page-size=4096 -T ${USER_LINKER_SCRIPT}")
target_include_directories(${LIB_USERLIB} PRIVATE ${USER_DIR})

# ==============================================================================
# Userlib Programs (linked against the xv6-native userlib)
# ==============================================================================
# Core programs (always built)
set(USER_PROGRAMS_C_FILES
    ${USER_DIR}/cat.c
    ${USER_DIR}/echo.c
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
    ${USER_DIR}/lsblk.c
    ${USER_DIR}/losetup.c
    ${USER_DIR}/dd.c
    ${USER_DIR}/dh.c
    ${USER_DIR}/mkfs_xv6fs.c
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
    ${USER_DIR}/bigfile.c
    ${USER_DIR}/symlinktest.c
    ${USER_DIR}/mmaptest.c
    ${USER_DIR}/mmapbigfile.c
    ${USER_DIR}/kqueuetest.c
    ${USER_DIR}/forktest.c
    ${USER_DIR}/top.c
    ${USER_DIR}/crashtest.c
    ${USER_DIR}/syscalltest.c
)

# Lab-specific programs
if($ENV{LAB} STREQUAL "lazy")
    list(APPEND USER_PROGRAMS_C_FILES ${USER_DIR}/lazytests.c)
endif()
if($ENV{LAB} STREQUAL "syscall")
    list(APPEND USER_PROGRAMS_C_FILES
        ${USER_DIR}/attack.c ${USER_DIR}/attacktest.c ${USER_DIR}/secret.c)
endif()
if($ENV{LAB} STREQUAL "lock")
    list(APPEND USER_PROGRAMS_C_FILES
        ${USER_DIR}/stats.c ${USER_DIR}/kalloctest.c ${USER_DIR}/bcachetest.c)
endif()
if($ENV{LAB} STREQUAL "traps")
    list(APPEND USER_PROGRAMS_C_FILES ${USER_DIR}/call.c ${USER_DIR}/bttest.c)
endif()
if($ENV{LAB} STREQUAL "pgtbl")
    list(APPEND USER_PROGRAMS_C_FILES ${USER_DIR}/pgtbltest.c)
endif()
if($ENV{LAB} STREQUAL "mmap")
    list(APPEND USER_PROGRAMS_C_FILES ${USER_DIR}/mmaptest.c)
endif()
if($ENV{LAB} STREQUAL "net")
    list(APPEND USER_PROGRAMS_C_FILES ${USER_DIR}/nettest.c)
endif()



# Build each userlib program: source file foo.c → executable _foo
foreach(PROGRAM ${USER_PROGRAMS_C_FILES})
    get_filename_component(PROGRAM_NAME ${PROGRAM} NAME_WE)
    list(APPEND USER_PROGRAMS _${PROGRAM_NAME})
    add_executable(_${PROGRAM_NAME} ${PROGRAM})
    target_link_options(_${PROGRAM_NAME} PRIVATE
        "SHELL:-z max-page-size=4096"
        "SHELL:-z common-page-size=4096"
        --build-id=none
        -T ${USER_LINKER_SCRIPT})
    target_link_libraries(_${PROGRAM_NAME} PRIVATE
        "--whole-archive" ${LIB_USERLIB} "--no-whole-archive")
    set_target_properties(_${PROGRAM_NAME} PROPERTIES OUTPUT_NAME _${PROGRAM_NAME})
endforeach()

add_custom_target(user_programs ALL
    DEPENDS ${USER_PROGRAMS}
    COMMENT "Building all user programs"
)

# ==============================================================================
# Musl Libc Programs
# ==============================================================================
# POSIX-compliant versions of core utilities, linked against musl.
# Sources live in user/musl-xv6/programs/ — adapted from user/ originals.
# Each entry: <target_prefix>:<source_basename>
# ==============================================================================
set(MUSL_PROG_DIR "${CMAKE_SOURCE_DIR}/user/musl-xv6/programs")

# Static musl programs (name_prefix:source_file)
set(MUSL_STATIC_PROGRAMS
    mcat:cat    mecho:echo   mgrep:grep   mls:ls      mwc:wc
    mmkdir:mkdir  mrm:rm     mln:ln       mkill:kill  msleep:sleep
    minit:init    msh:sh     mhello:hello mfptest:fptest
    mnslookup:nslookup  mepolltest:epolltest
    msocktest:socktest
    mifconfig:ifconfig  mroute:route
    mlogin:login   mgetty:getty    mpasswd:passwd
    msu:su         mid:id          mwhoami:whoami
    museradd:useradd  muserdel:userdel
    mchown:chown   mchmod:chmod    mtelnetd:telnetd
)
foreach(MAPPING ${MUSL_STATIC_PROGRAMS})
    string(REPLACE ":" ";" PARTS ${MAPPING})
    list(GET PARTS 0 TARGET_PREFIX)
    list(GET PARTS 1 SRC_NAME)
    if(EXISTS "${MUSL_PROG_DIR}/${SRC_NAME}.c")
        add_musl_program(${TARGET_PREFIX} "${MUSL_PROG_DIR}/${SRC_NAME}.c")
    endif()
endforeach()

# Dynamic musl program (linked against libc.so, same as CPython)
if(EXISTS "${MUSL_PROG_DIR}/dyntest.c")
    add_musl_dynamic_program(dyntest "${MUSL_PROG_DIR}/dyntest.c")
endif()

# ==============================================================================
# ncurses — Terminal UI library (static, built against musl)
# ==============================================================================
set(NCURSES_SRC_DIR "${CMAKE_SOURCE_DIR}/user/ncurses")
set(NCURSES_BUILD_DIR "${CMAKE_BINARY_DIR}/ncurses/build-xv6")
set(NCURSES_LIB_A "${NCURSES_BUILD_DIR}/lib/libncurses.a")
set(NCURSES_INCLUDE_DIR "${NCURSES_BUILD_DIR}/include")
set(MUSL_COMPAT_DIR "${CMAKE_SOURCE_DIR}/user/musl-xv6/compat")

if(ARCH STREQUAL "x86_64")
    set(NCURSES_HOST_TRIPLE "x86_64-linux-gnu")
    set(USER_ARCH_CFLAGS "")
    set(USER_HOST_TRIPLE "x86_64-linux-gnu")
else()
    set(NCURSES_HOST_TRIPLE "riscv64-unknown-elf")
    set(USER_ARCH_CFLAGS "-march=rv64gc -mabi=lp64d -mcmodel=medany")
    set(USER_HOST_TRIPLE "riscv64-unknown-elf")
endif()

# List form of USER_ARCH_CFLAGS for add_custom_command COMMAND arguments
# (the string form is kept for env-var CFLAGS= lines where \ -joining works).
separate_arguments(USER_ARCH_CFLAGS_CMD NATIVE_COMMAND "${USER_ARCH_CFLAGS}")

if(EXISTS "${NCURSES_SRC_DIR}/configure")
    add_custom_command(
        OUTPUT ${NCURSES_LIB_A}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${NCURSES_BUILD_DIR}
        COMMAND ${CMAKE_COMMAND} -E chdir ${NCURSES_BUILD_DIR}
                env
                CC=${CMAKE_C_COMPILER}
                AR=${CMAKE_AR}
                RANLIB=${CMAKE_RANLIB}
                CFLAGS=--sysroot=${MUSL_SYSROOT}\ -O2\ -nostdinc\ -ffreestanding\ -isystem\ ${MUSL_INCLUDE_DIR}\ -isystem\ ${GCC_INCLUDE_DIR_MUSL}\ -isystem\ ${MUSL_COMPAT_DIR}
                ${NCURSES_SRC_DIR}/configure
                    --host=${NCURSES_HOST_TRIPLE}
                    --prefix=/
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
        COMMAND ${CMAKE_COMMAND} -E chdir ${NCURSES_BUILD_DIR} make install DESTDIR=${MUSL_SYSROOT}
        # Create ncurses.h symlink expected by CPython's py_curses.h
        COMMAND ${CMAKE_COMMAND} -E create_symlink curses.h ${NCURSES_INCLUDE_DIR}/ncurses.h
        COMMAND ${CMAKE_COMMAND} -E create_symlink curses.h ${MUSL_SYSROOT}/include/ncurses.h
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
                CFLAGS=--sysroot=${MUSL_SYSROOT}\ -O2\ -nostdinc\ -ffreestanding\ -D_POSIX_VERSION=200809L\ -isystem\ ${MUSL_INCLUDE_DIR}\ -isystem\ ${GCC_INCLUDE_DIR_MUSL}\ -isystem\ ${MUSL_COMPAT_DIR}\ -I${NCURSES_INCLUDE_DIR}
                LDFLAGS=-L${NCURSES_BUILD_DIR}/lib
                ${READLINE_SRC_DIR}/configure
                    --host=${USER_HOST_TRIPLE}
                    --prefix=/
                    --disable-shared
                    --with-curses
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
        COMMAND ${CMAKE_COMMAND} -E chdir ${READLINE_BUILD_DIR} make install DESTDIR=${MUSL_SYSROOT}
        DEPENDS local_ncurses_build ${READLINE_SRC_DIR}/configure
        COMMENT "Building and installing readline (musl)"
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
            --sysroot=${MUSL_SYSROOT}
            -O2 -nostdinc -ffreestanding -fno-common -fno-pie -no-pie
            ${USER_ARCH_CFLAGS_CMD}
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
                CFLAGS=--sysroot=${MUSL_SYSROOT}\ -O2\ -nostdinc\ -ffreestanding\ -fno-common\ -fno-pie\ -no-pie\ ${USER_ARCH_CFLAGS}\ -DFLUSHERR=1\ -isystem\ ${MUSL_INCLUDE_DIR}\ -isystem\ ${GCC_INCLUDE_DIR_MUSL}\ -isystem\ ${MUSL_COMPAT_DIR}\ -I${CMAKE_SOURCE_DIR}/user\ -I${NCURSES_INCLUDE_DIR}
                LDFLAGS=-static\ -nostartfiles\ -nostdlib\ ${MUSL_CRT1_O}\ ${MUSL_CRTI_O}\ -Wl,-z,max-page-size=0x1000\ -Wl,-z,common-page-size=0x1000\ -Wl,--build-id=none\ -T\ ${MUSL_LINKER_SCRIPT}\ -L${HISTEDIT_SHIM_DIR}\ -L${READLINE_BUILD_DIR}\ -L${NCURSES_BUILD_DIR}/lib
                LIBS=${HISTEDIT_SHIM_A}\ ${READLINE_LIB_A}\ ${NCURSES_LIB_A}\ ${MUSL_LIBC_A}\ ${LIBGCC_PATH}\ ${MUSL_CRTN_O}
                ${DASH_SRC_DIR}/configure
                    --host=${USER_HOST_TRIPLE}
                    --prefix=/
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
        COMMAND ${CMAKE_COMMAND} -E make_directory ${MUSL_SYSROOT}/bin
        COMMAND ${CMAKE_COMMAND} -E chdir ${DASH_BUILD_DIR} make install-exec DESTDIR=${MUSL_SYSROOT}
        DEPENDS dash_configure ${HISTEDIT_SHIM_A}
        COMMENT "Building and installing dash shell (musl, with readline)"
    )

    add_custom_target(dash_build DEPENDS ${DASH_BIN})
    add_dependencies(user_programs dash_build)
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
            --sysroot=${MUSL_SYSROOT}
            -isystem ${MUSL_INCLUDE_DIR}
            -isystem ${GCC_INCLUDE_DIR_MUSL}
            -isystem ${MUSL_COMPAT_DIR}
            -nostdinc -O2
            ${KQUEUE_COMPAT_SRC}
    DEPENDS ${KQUEUE_COMPAT_SRC} musl_sysroot
    COMMENT "Compiling kqueue compat object for musl"
)
add_custom_target(kqueue_compat DEPENDS ${KQUEUE_COMPAT_OBJ})

# ==============================================================================
# OpenSSL 3.0 — Cryptography library (static, built against musl)
# ==============================================================================
set(OPENSSL_SRC_DIR "${CMAKE_SOURCE_DIR}/user/openssl")
set(OPENSSL_BUILD_DIR "${CMAKE_BINARY_DIR}/openssl/build-xv6")
set(OPENSSL_LIBCRYPTO_A "${OPENSSL_BUILD_DIR}/libcrypto.a")
set(OPENSSL_LIBSSL_A "${OPENSSL_BUILD_DIR}/libssl.a")
set(OPENSSL_INCLUDE_DIR "${OPENSSL_BUILD_DIR}/include")
set(OPENSSL_INSTALL_DIR "${CMAKE_BINARY_DIR}/openssl/install")

# OpenSSL target and cross-compile prefix (architecture-dependent)
# OpenSSL and OpenSSH require the xv6 musl toolchain (not the bare-metal one).
# We find it explicitly, matching the CPython approach.
if(ARCH STREQUAL "x86_64")
    set(OPENSSL_TARGET "linux-x86_64")
    find_program(OPENSSL_GCC_PROG x86_64-xv6-linux-musl-gcc
        PATHS ${CMAKE_SOURCE_DIR}/toolchain/x86_64/phase2/bin
              ${CMAKE_SOURCE_DIR}/toolchain/x86_64/bin
        NO_DEFAULT_PATH)
    if(NOT OPENSSL_GCC_PROG)
        find_program(OPENSSL_GCC_PROG x86_64-xv6-linux-musl-gcc)
    endif()
else()
    set(OPENSSL_TARGET "linux64-riscv64")
    find_program(OPENSSL_GCC_PROG riscv64-xv6-linux-musl-gcc
        PATHS ${CMAKE_SOURCE_DIR}/toolchain/riscv64/phase2/bin
              ${CMAKE_SOURCE_DIR}/toolchain/riscv64/bin
        NO_DEFAULT_PATH)
    if(NOT OPENSSL_GCC_PROG)
        find_program(OPENSSL_GCC_PROG riscv64-xv6-linux-musl-gcc)
    endif()
endif()
if(OPENSSL_GCC_PROG)
    get_filename_component(_openssl_tooldir ${OPENSSL_GCC_PROG} DIRECTORY)
    string(REGEX REPLACE "gcc$" "" OPENSSL_CROSS_PREFIX "${OPENSSL_GCC_PROG}")
    set(OPENSSL_CC "${OPENSSL_GCC_PROG}")
    set(OPENSSL_AR "${OPENSSL_CROSS_PREFIX}ar")
    set(OPENSSL_RANLIB "${OPENSSL_CROSS_PREFIX}ranlib")
    message(STATUS "OpenSSL/OpenSSH cross-compiler: ${OPENSSL_CC}")
else()
    # Fall back to CMAKE_C_COMPILER if it happens to be the musl toolchain
    string(REGEX REPLACE "gcc$" "" OPENSSL_CROSS_PREFIX "${CMAKE_C_COMPILER}")
    set(OPENSSL_CC "${CMAKE_C_COMPILER}")
    set(OPENSSL_AR "${CMAKE_AR}")
    set(OPENSSL_RANLIB "${CMAKE_RANLIB}")
    message(STATUS "OpenSSL/OpenSSH cross-compiler (fallback): ${OPENSSL_CC}")
endif()

if(EXISTS "${OPENSSL_SRC_DIR}/Configure")
    # Configure OpenSSL with static-only, no-asm for safe cross-compilation
    add_custom_command(
        OUTPUT ${OPENSSL_BUILD_DIR}/Makefile
        COMMAND ${CMAKE_COMMAND} -E make_directory ${OPENSSL_BUILD_DIR}
        COMMAND ${CMAKE_COMMAND} -E chdir ${OPENSSL_BUILD_DIR}
                env
                CROSS_COMPILE=${OPENSSL_CROSS_PREFIX}
                perl ${OPENSSL_SRC_DIR}/Configure
                    ${OPENSSL_TARGET}
                    --cross-compile-prefix=${OPENSSL_CROSS_PREFIX}
                    --prefix=/
                    --openssldir=/etc/ssl
                    --libdir=lib
                    no-shared
                    no-dso
                    no-engine
                    no-async
                    no-threads
                    no-asm
                    no-tests
                    no-ui-console
                    no-afalgeng
                    no-ktls
                    no-devcryptoeng
                    -static
                    -DOPENSSL_NO_SECURE_MEMORY
                    -DOPENSSL_RAND_SEED_GETRANDOM
                    -O2
        DEPENDS musl_sysroot ${OPENSSL_SRC_DIR}/Configure
        COMMENT "Configuring OpenSSL 3.0 cross build (${OPENSSL_TARGET}, static)"
    )

    add_custom_target(openssl_configure DEPENDS ${OPENSSL_BUILD_DIR}/Makefile)

    # Build OpenSSL libraries (libcrypto.a + libssl.a)
    add_custom_command(
        OUTPUT ${OPENSSL_LIBCRYPTO_A} ${OPENSSL_LIBSSL_A}
        COMMAND ${CMAKE_COMMAND} -E chdir ${OPENSSL_BUILD_DIR}
                make -j16 build_libs
        DEPENDS openssl_configure
        COMMENT "Building OpenSSL static libraries (libcrypto.a, libssl.a)"
    )

    # Install OpenSSL headers and libs to a staging area
    add_custom_command(
        OUTPUT ${OPENSSL_INSTALL_DIR}/lib/libcrypto.a
        COMMAND ${CMAKE_COMMAND} -E chdir ${OPENSSL_BUILD_DIR}
                make install_dev DESTDIR=${OPENSSL_INSTALL_DIR}
        # Also install into the main sysroot for other consumers
        COMMAND ${CMAKE_COMMAND} -E chdir ${OPENSSL_BUILD_DIR}
                make install_dev DESTDIR=${MUSL_SYSROOT}
        DEPENDS ${OPENSSL_LIBCRYPTO_A} ${OPENSSL_LIBSSL_A}
        COMMENT "Installing OpenSSL headers and libraries"
    )

    add_custom_target(openssl_build DEPENDS ${OPENSSL_INSTALL_DIR}/lib/libcrypto.a)

    # Optionally build the openssl CLI tool
    set(OPENSSL_CLI_BIN "${OPENSSL_BUILD_DIR}/apps/openssl")
    add_custom_command(
        OUTPUT ${OPENSSL_CLI_BIN}
        COMMAND ${CMAKE_COMMAND} -E chdir ${OPENSSL_BUILD_DIR}
                make -j16 build_programs
        DEPENDS ${OPENSSL_LIBCRYPTO_A} ${OPENSSL_LIBSSL_A}
        COMMENT "Building OpenSSL CLI tool"
    )
    add_custom_target(openssl_cli DEPENDS ${OPENSSL_CLI_BIN})
else()
    message(STATUS "Skipping OpenSSL build (missing submodule at ${OPENSSL_SRC_DIR})")
endif()

# ==============================================================================
# OpenSSH 9.9 — SSH server and client (static, built against musl + OpenSSL)
# ==============================================================================
set(OPENSSH_SRC_DIR "${CMAKE_SOURCE_DIR}/user/openssh")
set(OPENSSH_BUILD_DIR "${CMAKE_BINARY_DIR}/openssh/build-xv6")
set(OPENSSH_SSHD_BIN "${OPENSSH_BUILD_DIR}/sshd")
set(OPENSSH_SSH_BIN "${OPENSSH_BUILD_DIR}/ssh")
set(OPENSSH_KEYGEN_BIN "${OPENSSH_BUILD_DIR}/ssh-keygen")
set(OPENSSH_KEYSCAN_BIN "${OPENSSH_BUILD_DIR}/ssh-keyscan")

if(ARCH STREQUAL "x86_64")
    set(OPENSSH_HOST_TRIPLE "x86_64-xv6-linux-musl")
else()
    set(OPENSSH_HOST_TRIPLE "riscv64-xv6-linux-musl")
endif()

if(EXISTS "${OPENSSH_SRC_DIR}/configure" AND EXISTS "${OPENSSL_SRC_DIR}/Configure")
    # Configure OpenSSH for cross-compilation against musl + OpenSSL
    add_custom_command(
        OUTPUT ${OPENSSH_BUILD_DIR}/Makefile
        COMMAND ${CMAKE_COMMAND} -E make_directory ${OPENSSH_BUILD_DIR}
        COMMAND ${CMAKE_COMMAND} -E chdir ${OPENSSH_BUILD_DIR}
                env
                CC=${OPENSSL_CC}
                AR=${OPENSSL_AR}
                RANLIB=${OPENSSL_RANLIB}
                STRIP=${OPENSSL_CROSS_PREFIX}strip
                CFLAGS=-O2\ -D_GNU_SOURCE\ -DHAVE_ATTRIBUTE__SENTINEL__=1
                LDFLAGS=-static\ -L${OPENSSL_INSTALL_DIR}/lib
                LIBS=-lcrypto
                ${OPENSSH_SRC_DIR}/configure
                    --host=${OPENSSH_HOST_TRIPLE}
                    --build=x86_64-pc-linux-gnu
                    --prefix=/
                    --sbindir=/bin
                    --libexecdir=/libexec
                    --sysconfdir=/etc/ssh
                    --with-privsep-path=/var/empty
                    --with-privsep-user=sshd
                    --with-ssl-dir=${OPENSSL_INSTALL_DIR}
                    --without-pam
                    --without-selinux
                    --without-libedit
                    --without-zlib
                    --without-openssl-header-check
                    --disable-strip
                    --disable-utmp
                    --disable-utmpx
                    --disable-wtmp
                    --disable-wtmpx
                    --disable-lastlog
                    --disable-pkcs11
                    --disable-security-key
                    ac_cv_func_arc4random=no
                    ac_cv_func_arc4random_buf=no
                    ac_cv_func_arc4random_stir=no
                    ac_cv_func_arc4random_uniform=no
                    ac_cv_func_closefrom=no
                    ac_cv_func_endgrent=yes
                    ac_cv_func_err=yes
                    ac_cv_func_errx=yes
                    ac_cv_func_explicit_bzero=yes
                    ac_cv_func_fchmod=yes
                    ac_cv_func_fchown=yes
                    ac_cv_func_freeaddrinfo=yes
                    ac_cv_func_fstatfs=yes
                    ac_cv_func_futimes=yes
                    ac_cv_func_getaddrinfo=yes
                    ac_cv_func_getcwd=yes
                    ac_cv_func_getgrouplist=yes
                    ac_cv_func_getgrset=no
                    ac_cv_func_getlastlogxbyname=no
                    ac_cv_func_getnameinfo=yes
                    ac_cv_func_getopt=yes
                    ac_cv_func_getpeereid=no
                    ac_cv_func_getpeerucred=no
                    ac_cv_func_getrlimit=yes
                    ac_cv_func_glob=yes
                    ac_cv_func_group_from_gid=no
                    ac_cv_func_inet_aton=yes
                    ac_cv_func_inet_ntoa=yes
                    ac_cv_func_inet_ntop=yes
                    ac_cv_func_login_getcapbool=no
                    ac_cv_func_mblen=yes
                    ac_cv_func_memmove=yes
                    ac_cv_func_mkdtemp=yes
                    ac_cv_func_ngetaddrinfo=no
                    ac_cv_func_openpty=yes
                    ac_cv_func_poll=yes
                    ac_cv_func_prctl=yes
                    ac_cv_func_pselect=yes
                    ac_cv_func_readpassphrase=no
                    ac_cv_func_realpath=yes
                    ac_cv_func_recvmsg=yes
                    ac_cv_func_rresvport_af=no
                    ac_cv_func_sendmsg=yes
                    ac_cv_func_setenv=yes
                    ac_cv_func_setgroupent=no
                    ac_cv_func_setgroups=yes
                    ac_cv_func_setlinebuf=yes
                    ac_cv_func_setlogin=no
                    ac_cv_func_setpassent=no
                    ac_cv_func_setpcred=no
                    ac_cv_func_setproctitle=no
                    ac_cv_func_setregid=yes
                    ac_cv_func_setresgid=yes
                    ac_cv_func_setresuid=yes
                    ac_cv_func_setreuid=yes
                    ac_cv_func_setsid=yes
                    ac_cv_func_sigaction=yes
                    ac_cv_func_sigvec=no
                    ac_cv_func_snprintf=yes
                    ac_cv_func_socketpair=yes
                    ac_cv_func_statfs=yes
                    ac_cv_func_statvfs=yes
                    ac_cv_func_fstatvfs=yes
                    ac_cv_func_strftime=yes
                    ac_cv_func_strlcat=yes
                    ac_cv_func_strlcpy=yes
                    ac_cv_func_strptime=yes
                    ac_cv_func_strsep=yes
                    ac_cv_func_strtoll=yes
                    ac_cv_func_strtonum=no
                    ac_cv_func_strtoul=yes
                    ac_cv_func_swap32=no
                    ac_cv_func_sysconf=yes
                    ac_cv_func_tcgetpgrp=yes
                    ac_cv_func_truncate=yes
                    ac_cv_func_unsetenv=yes
                    ac_cv_func_updwtmpx=no
                    ac_cv_func_user_from_uid=no
                    ac_cv_func_usleep=yes
                    ac_cv_func_vasprintf=yes
                    ac_cv_func_vsnprintf=yes
                    ac_cv_func_waitpid=yes
                    ac_cv_func_warn=yes
                    ac_cv_have_control_in_msghdr=yes
                    ac_cv_have_broken_snprintf=no
                    ac_cv_header_endian_h=yes
                    ac_cv_header_features_h=yes
                    ac_cv_header_floatingpoint_h=no
                    ac_cv_header_ifaddrs_h=no
                    ac_cv_header_lastlog_h=no
                    ac_cv_header_linux_if_tun_h=no
                    ac_cv_header_login_h=no
                    ac_cv_header_maillock_h=no
                    ac_cv_header_ndir_h=no
                    ac_cv_header_net_if_tun_h=no
                    ac_cv_header_poll_h=yes
                    ac_cv_header_pty_h=yes
                    ac_cv_header_readpassphrase_h=no
                    ac_cv_header_rpc_types_h=no
                    ac_cv_header_security_pam_appl_h=no
                    ac_cv_header_shadow_h=yes
                    ac_cv_header_sys_dir_h=no
                    ac_cv_header_sys_mount_h=no
                    ac_cv_header_sys_ndir_h=no
                    ac_cv_header_sys_poll_h=yes
                    ac_cv_header_sys_ptms_h=no
                    ac_cv_header_sys_select_h=yes
                    ac_cv_header_sys_statvfs_h=yes
                    ac_cv_header_sys_stropts_h=no
                    ac_cv_header_sys_sysmacros_h=yes
                    ac_cv_header_sys_time_h=yes
                    ac_cv_header_sys_un_h=yes
                    ac_cv_header_util_h=no
                    ac_cv_header_utmp_h=yes
                    ac_cv_header_utmpx_h=no
                    ac_cv_header_vis_h=no
                    ac_cv_lib_crypt_crypt=yes
                    ac_cv_search_crypt=none\ required
                    ac_cv_search_dlopen=no
                    ac_cv_sizeof_short=2
                    ac_cv_sizeof_int=4
                    ac_cv_sizeof_long=8
                    ac_cv_sizeof_long_long=8
        DEPENDS ${OPENSSH_SRC_DIR}/configure openssl_build
        COMMENT "Configuring OpenSSH cross build (${OPENSSH_HOST_TRIPLE}, static, musl + OpenSSL)"
    )

    add_custom_target(openssh_configure DEPENDS ${OPENSSH_BUILD_DIR}/Makefile)

    # Build OpenSSH programs (sshd, ssh, ssh-keygen, ssh-keyscan)
    add_custom_command(
        OUTPUT ${OPENSSH_SSHD_BIN} ${OPENSSH_SSH_BIN} ${OPENSSH_KEYGEN_BIN}
        COMMAND ${CMAKE_COMMAND} -E chdir ${OPENSSH_BUILD_DIR}
                make -j16
        DEPENDS openssh_configure
        COMMENT "Building OpenSSH (sshd, ssh, ssh-keygen, ssh-keyscan)"
    )

    add_custom_target(openssh_build DEPENDS ${OPENSSH_SSHD_BIN})
    add_dependencies(user_programs openssh_build)

    # Install OpenSSH binaries to sysroot
    set(OPENSSH_INSTALL_BINS sshd ssh ssh-keygen ssh-keyscan scp sftp)
    foreach(PROG ${OPENSSH_INSTALL_BINS})
        set(SRC "${OPENSSH_BUILD_DIR}/${PROG}")
        set(DST "${MUSL_SYSROOT}/bin/${PROG}")
        add_custom_command(
            OUTPUT ${DST}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${MUSL_SYSROOT}/bin
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SRC} ${DST}
            DEPENDS ${OPENSSH_SSHD_BIN}
            COMMENT "sysroot: ${DST}"
        )
        list(APPEND OPENSSH_SYSROOT_TARGETS ${DST})
    endforeach()

    # Install sshd-session to /libexec/ (OpenSSH 9.8+ splits sshd into listener + session handler)
    set(SSHD_SESSION_SRC "${OPENSSH_BUILD_DIR}/sshd-session")
    set(SSHD_SESSION_DST "${MUSL_SYSROOT}/libexec/sshd-session")
    add_custom_command(
        OUTPUT ${SSHD_SESSION_DST}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${MUSL_SYSROOT}/libexec
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SSHD_SESSION_SRC} ${SSHD_SESSION_DST}
        DEPENDS ${OPENSSH_SSHD_BIN}
        COMMENT "sysroot: /libexec/sshd-session"
    )
    list(APPEND OPENSSH_SYSROOT_TARGETS ${SSHD_SESSION_DST})

    add_custom_target(openssh_sysroot DEPENDS ${OPENSSH_SYSROOT_TARGETS})
    add_dependencies(openssh_sysroot openssh_build)

    # Install sshd_config to sysroot
    add_custom_command(
        OUTPUT ${MUSL_SYSROOT}/etc/ssh/sshd_config
        COMMAND ${CMAKE_COMMAND} -E make_directory ${MUSL_SYSROOT}/etc/ssh
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${OPENSSH_BUILD_DIR}/sshd_config
                ${MUSL_SYSROOT}/etc/ssh/sshd_config
        DEPENDS openssh_configure
        COMMENT "sysroot: etc/ssh/sshd_config"
    )
    add_custom_target(openssh_config DEPENDS ${MUSL_SYSROOT}/etc/ssh/sshd_config)
else()
    if(NOT EXISTS "${OPENSSL_SRC_DIR}/Configure")
        message(STATUS "Skipping OpenSSH build (missing OpenSSL submodule at ${OPENSSL_SRC_DIR})")
    endif()
    if(NOT EXISTS "${OPENSSH_SRC_DIR}/configure")
        message(STATUS "Skipping OpenSSH build (missing OpenSSH submodule at ${OPENSSH_SRC_DIR})")
    endif()
endif()

# CPython — Python 3.12 interpreter (built against musl + ncurses + readline)
# ==============================================================================
set(CPYTHON_SRC_DIR "${CMAKE_SOURCE_DIR}/user/v6-cpython" CACHE PATH "CPython source directory")
set(CPYTHON_BUILD_DIR "${CMAKE_BINARY_DIR}/cpython/build-3.12" CACHE PATH "CPython out-of-tree build directory")
set(CPYTHON_BIN "${CPYTHON_BUILD_DIR}/python" CACHE FILEPATH "CPython interpreter binary path")
set(CPYTHON_FS_BIN "${CMAKE_BINARY_DIR}/user/_python")

if(EXISTS "${CPYTHON_SRC_DIR}/configure")
    # CPython needs a compiler that supports -shared (for libpython3.12.so and
    # .so extension modules).  The bare-metal *-unknown-elf-ld cannot produce
    # shared objects.  Priority:
    #   1. xv6 musl toolchain (riscv64/x86_64-xv6-linux-musl-gcc)
    #   2. Distro linux-gnu cross-compiler (fallback)
    if(ARCH STREQUAL "x86_64")
        # Try xv6 musl toolchain first
        find_program(CPYTHON_GCC_PROG x86_64-xv6-linux-musl-gcc
            PATHS ${CMAKE_SOURCE_DIR}/toolchain/x86_64/phase2/bin
                  ${CMAKE_SOURCE_DIR}/toolchain/x86_64/bin
            NO_DEFAULT_PATH)
        if(NOT CPYTHON_GCC_PROG)
            find_program(CPYTHON_GCC_PROG x86_64-xv6-linux-musl-gcc)
        endif()
        if(CPYTHON_GCC_PROG)
            get_filename_component(_cpython_tooldir ${CPYTHON_GCC_PROG} DIRECTORY)
            set(_cpython_prefix "${_cpython_tooldir}/x86_64-xv6-linux-musl-")
        else()
            set(_cpython_prefix "x86_64-linux-gnu-")
        endif()
        set(CPYTHON_HOST_TRIPLE "x86_64-unknown-xv6")
        set(CPYTHON_DYNAMIC_LINKER "/lib/ld-musl-x86_64.so.1")
    else()
        # Try xv6 musl toolchain first
        find_program(CPYTHON_GCC_PROG riscv64-xv6-linux-musl-gcc
            PATHS ${CMAKE_SOURCE_DIR}/toolchain/riscv64/phase2/bin
                  ${CMAKE_SOURCE_DIR}/toolchain/riscv64/bin
            NO_DEFAULT_PATH)
        if(NOT CPYTHON_GCC_PROG)
            find_program(CPYTHON_GCC_PROG riscv64-xv6-linux-musl-gcc)
        endif()
        if(CPYTHON_GCC_PROG)
            get_filename_component(_cpython_tooldir ${CPYTHON_GCC_PROG} DIRECTORY)
            set(_cpython_prefix "${_cpython_tooldir}/riscv64-xv6-linux-musl-")
        else()
            set(_cpython_prefix "riscv64-linux-gnu-")
        endif()
        set(CPYTHON_HOST_TRIPLE "riscv64-unknown-xv6")
        set(CPYTHON_DYNAMIC_LINKER "/lib/ld-musl-riscv64.so.1")
    endif()

    set(CPYTHON_CC "${_cpython_prefix}gcc")
    set(CPYTHON_AR "${_cpython_prefix}ar")
    set(CPYTHON_RANLIB "${_cpython_prefix}ranlib")
    set(CPYTHON_READELF "${_cpython_prefix}readelf")
    set(CPYTHON_STRIP "${_cpython_prefix}strip")
    message(STATUS "CPython cross-compiler: ${CPYTHON_CC}")

    execute_process(
        COMMAND ${CPYTHON_CC} -print-file-name=include
        OUTPUT_VARIABLE CPYTHON_GCC_INCLUDE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND ${CPYTHON_CC} -print-libgcc-file-name
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
                CPPFLAGS=--sysroot=${MUSL_SYSROOT}\ -isystem\ ${MUSL_INCLUDE_DIR}\ -isystem\ ${MUSL_COMPAT_DIR}\ -I${NCURSES_INCLUDE_DIR}\ -I${CMAKE_SOURCE_DIR}/user\ -D_POSIX_C_SOURCE=200809L\ -DSSIZE_MAX=__LONG_MAX__\ -DSA_ONSTACK=0
                CFLAGS=--sysroot=${MUSL_SYSROOT}\ -isystem\ ${MUSL_INCLUDE_DIR}\ -isystem\ ${CPYTHON_GCC_INCLUDE_DIR}\ -isystem\ ${MUSL_COMPAT_DIR}\ -I${NCURSES_INCLUDE_DIR}\ -I${CMAKE_SOURCE_DIR}/user\ -D_POSIX_C_SOURCE=200809L\ -DSSIZE_MAX=__LONG_MAX__\ -DSA_ONSTACK=0
                LDFLAGS=-no-pie\ -nostartfiles\ -nostdlib\ ${MUSL_CRT1_O}\ ${MUSL_CRTI_O}\ -Wl,--export-dynamic\ -Wl,--dynamic-linker=${CPYTHON_DYNAMIC_LINKER}\ -Wl,-z,max-page-size=0x1000\ -Wl,-z,common-page-size=0x1000\ -Wl,--build-id=none\ -L${NCURSES_BUILD_DIR}/lib\ -L${READLINE_BUILD_DIR}\ -L${MUSL_SYSROOT}/lib
                LIBS=-lc\ ${CPYTHON_LIBGCC_PATH}
                ${CPYTHON_SRC_DIR}/configure
                    --host=${CPYTHON_HOST_TRIPLE}
                    --build=x86_64-pc-linux-gnu
                    --prefix=/
                    --with-build-python=python3
                    --with-pydebug
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
                    ac_cv_func_select=yes
                    ac_cv_func_pselect=yes
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
        COMMAND ${CMAKE_COMMAND} -E make_directory ${MUSL_SYSROOT}/bin
        COMMAND ${CMAKE_COMMAND} -E chdir ${CPYTHON_BUILD_DIR} make altbininstall DESTDIR=${MUSL_SYSROOT}
                "SYSLIBS=${KQUEUE_COMPAT_OBJ} -lc ${CPYTHON_LIBGCC_PATH} ${MUSL_CRTN_O}"
        COMMAND ${CMAKE_COMMAND} -E create_symlink python3.12 ${MUSL_SYSROOT}/bin/python
        DEPENDS cpython_configure kqueue_compat
        COMMENT "Building and installing CPython interpreter (musl)"
    )

    add_custom_target(cpython_build DEPENDS ${CPYTHON_BIN})
    add_dependencies(user_programs cpython_build)
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
            ${USER_ARCH_CFLAGS_CMD}
            -fno-common -fno-pie -no-pie
            -DUSE_NCURSES_SHELL=1
            --sysroot=${MUSL_SYSROOT}
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
            -Wl,-z,common-page-size=4096
            -Wl,--build-id=none
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

# ==============================================================================
# Populate sysroot/bin/ with xv6 user programs for GDB debugging.
#
# Third-party components install directly via their native install targets:
#   musl       → --prefix=sysroot  (lib/ + include/)
#   ncurses    → make install       (lib/ + include/)
#   readline   → make install       (lib/ + include/readline/)
#   dash       → make install-exec  (bin/dash)
#   CPython    → make altbininstall  (bin/python3.12 + symlink bin/python)
#
# This section handles the remaining xv6-specific programs:
#   userlib programs  → sysroot/bin/<name>
#   musl programs     → sysroot/bin/<name>  (drop the 'm' prefix)
#   ncurses shell     → sysroot/bin/sh
# ==============================================================================
set(SYSROOT_BIN_DIR "${MUSL_SYSROOT}/bin")
set(SYSROOT_TARGETS "")

# Helper: copy a program to sysroot/bin/, tracking it as an output
function(sysroot_install SRC_PATH DEST_PATH)
    get_filename_component(DEST_DIR ${DEST_PATH} DIRECTORY)
    add_custom_command(
        OUTPUT ${DEST_PATH}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${DEST_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SRC_PATH} ${DEST_PATH}
        DEPENDS ${SRC_PATH}
        COMMENT "sysroot: ${DEST_PATH}"
    )
    set(SYSROOT_TARGETS ${SYSROOT_TARGETS} ${DEST_PATH} PARENT_SCOPE)
endfunction()

# Userlib-only programs (no musl equivalent)
set(USERLIB_ONLY_PROGS
    bigfile cloexectest clonetest cowtest dd devtest dh dumpchan dumpinode
    dumppcache dumprq find free forktest grind kqueuetest losetup lsblk mkfs_xv6fs mknod mmapbigfile mmaptest mount pingpong
    primes ps randtest stressfs symlinktest sync testsig umount
    usertests vforktest waitgdb wallclock xargs zombie top
    syscalltest
)
foreach(PROG ${USERLIB_ONLY_PROGS})
    set(SRC "${CMAKE_BINARY_DIR}/user/_${PROG}")
    if(TARGET _${PROG})
        sysroot_install(${SRC} "${SYSROOT_BIN_DIR}/${PROG}")
    endif()
endforeach()

# Musl static programs: _m<name> → bin/<name>  (replaces any userlib version)
set(MUSL_PROG_MAP
    mcat:cat   mecho:echo   mgrep:grep   mls:ls   mwc:wc
    mmkdir:mkdir  mrm:rm  mln:ln  mkill:kill  msleep:sleep
    minit:init  mhello:hello  mfptest:fptest
    mnslookup:nslookup  mepolltest:epolltest
    msocktest:socktest
    mifconfig:ifconfig  mroute:route
    mlogin:login   mgetty:getty    mpasswd:passwd
    msu:su         mid:id          mwhoami:whoami
    museradd:useradd  muserdel:userdel
    mchown:chown   mchmod:chmod    mtelnetd:telnetd
)
foreach(MAPPING ${MUSL_PROG_MAP})
    string(REPLACE ":" ";" PARTS ${MAPPING})
    list(GET PARTS 0 BUILD_NAME)
    list(GET PARTS 1 FRIENDLY_NAME)
    set(SRC "${CMAKE_BINARY_DIR}/user/_${BUILD_NAME}")
    if(TARGET _${BUILD_NAME}_musl)
        sysroot_install(${SRC} "${SYSROOT_BIN_DIR}/${FRIENDLY_NAME}")
    endif()
endforeach()

# ncurses shell (musl) — the primary "sh"
if(TARGET _sh_musl)
    sysroot_install("${CMAKE_BINARY_DIR}/user/_sh" "${SYSROOT_BIN_DIR}/sh")
endif()

# msh — basic musl shell (keep alongside ncurses sh for reference)
if(TARGET _msh_musl)
    sysroot_install("${CMAKE_BINARY_DIR}/user/_msh" "${SYSROOT_BIN_DIR}/msh")
endif()

# dyntest (dynamic linking test)
if(TARGET _dyntest_dymusl)
    sysroot_install("${CMAKE_BINARY_DIR}/user/_dyntest" "${SYSROOT_BIN_DIR}/dyntest")
endif()

# dash shell (statically linked against musl)
if(TARGET dash_build)
    sysroot_install("${DASH_BIN}" "${SYSROOT_BIN_DIR}/dash")
endif()

# CPython interpreter (dynamically linked against musl)
if(TARGET cpython_build)
    sysroot_install("${CPYTHON_BIN}" "${SYSROOT_BIN_DIR}/python3.12")
    # python → python3.12 symlink
    add_custom_command(
        OUTPUT "${SYSROOT_BIN_DIR}/python"
        COMMAND ${CMAKE_COMMAND} -E create_symlink python3.12 "${SYSROOT_BIN_DIR}/python"
        DEPENDS "${SYSROOT_BIN_DIR}/python3.12"
        COMMENT "sysroot: python → python3.12 symlink"
    )
    set(SYSROOT_TARGETS ${SYSROOT_TARGETS} "${SYSROOT_BIN_DIR}/python")
endif()

# OpenSSH binaries (statically linked against musl + OpenSSL)
if(TARGET openssh_build)
    foreach(PROG sshd ssh ssh-keygen ssh-keyscan scp sftp)
        sysroot_install("${OPENSSH_BUILD_DIR}/${PROG}" "${SYSROOT_BIN_DIR}/${PROG}")
    endforeach()
    # sshd-session goes to /libexec/ (OpenSSH 9.8+ listener/session split)
    set(SYSROOT_LIBEXEC_DIR "${MUSL_SYSROOT}/libexec")
    sysroot_install("${OPENSSH_BUILD_DIR}/sshd-session" "${SYSROOT_LIBEXEC_DIR}/sshd-session")
endif()

# OpenSSL CLI tool (optional — useful for key generation and diagnostics)
if(TARGET openssl_cli)
    sysroot_install("${OPENSSL_CLI_BIN}" "${SYSROOT_BIN_DIR}/openssl")
endif()

# Aggregate target
if(SYSROOT_TARGETS)
    # Write a stamp file whenever sysroot is (re)populated so that
    # downstream add_custom_command rules (fs.img) can depend on an
    # actual file rather than only a target name.
    set(SYSROOT_STAMP "${MUSL_SYSROOT}/.sysroot_stamp")
    set(SYSROOT_STAMP "${SYSROOT_STAMP}" PARENT_SCOPE)
    add_custom_command(
        OUTPUT ${SYSROOT_STAMP}
        COMMAND ${CMAKE_COMMAND} -E touch ${SYSROOT_STAMP}
        DEPENDS ${SYSROOT_TARGETS}
        COMMENT "sysroot: stamp"
    )
    add_custom_target(sysroot_populate ALL DEPENDS ${SYSROOT_STAMP})
    add_dependencies(sysroot_populate user_programs)
    if(TARGET dash_build)
        add_dependencies(sysroot_populate dash_build)
    endif()
    if(TARGET cpython_build)
        add_dependencies(sysroot_populate cpython_build)
    endif()
    if(TARGET openssh_build)
        add_dependencies(sysroot_populate openssh_build)
    endif()
    if(TARGET openssl_cli)
        add_dependencies(sysroot_populate openssl_cli)
    endif()
endif()

# Generate .gdbinit for remote debugging inside sysroot/.
# Placed here (at the end) so CPYTHON_BIN, DASH_BIN etc. are defined.
if(ARCH STREQUAL "x86_64")
    set(GDB_ARCH "i386:x86-64")
    set(GDB_COMMAND "gdb bin/<program>")
else()
    set(GDB_ARCH "riscv:rv64")
    set(GDB_COMMAND "gdb-multiarch bin/<program>")
endif()
configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/gdbinit.in
    ${MUSL_SYSROOT}/.gdbinit
    @ONLY
)
