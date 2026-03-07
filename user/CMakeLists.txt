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

# Architecture-specific flags shared by xv6-native user programs and musl ports.
if(ARCH STREQUAL "x86_64")
    set(USER_ARCH_CFLAGS "")
    set(USER_HOST_TRIPLE "x86_64-linux-gnu")
else()
    set(USER_ARCH_CFLAGS "-march=rv64gc -mabi=lp64d -mcmodel=medany")
    set(USER_HOST_TRIPLE "riscv64-linux-gnu")
endif()
separate_arguments(USER_ARCH_CFLAGS_CMD NATIVE_COMMAND "${USER_ARCH_CFLAGS}")

set(MUSL_SYSROOT_DEP_FILES
    ${MUSL_CRT1_O}
    ${MUSL_CRTI_O}
    ${MUSL_CRTN_O}
    ${MUSL_SYSROOT}/lib/libc.so)

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
target_compile_options(${LIB_USERLIB} PRIVATE ${USER_ARCH_CFLAGS_CMD})
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
    ${USER_DIR}/iovectest.c
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
    target_compile_options(_${PROGRAM_NAME} PRIVATE ${USER_ARCH_CFLAGS_CMD})
    target_link_options(_${PROGRAM_NAME} PRIVATE
        ${MUSL_CRT1_O}
        ${MUSL_CRTI_O}
        --dynamic-linker=${MUSL_DYNAMIC_LINKER_PATH}
        "SHELL:-z max-page-size=4096"
        "SHELL:-z common-page-size=4096"
        --build-id=none
        -L${MUSL_SYSROOT}/lib)
    target_link_libraries(_${PROGRAM_NAME} PRIVATE
        "--whole-archive" ${LIB_USERLIB} "--no-whole-archive"
        -lc
        ${LIBGCC_PATH}
        ${MUSL_CRTN_O})
    add_dependencies(_${PROGRAM_NAME} musl_sysroot)
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
        add_musl_dynamic_program(${TARGET_PREFIX} "${MUSL_PROG_DIR}/${SRC_NAME}.c")
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
set(NCURSES_SHARED_BUILD_DIR "${CMAKE_BINARY_DIR}/ncurses/build-xv6-shared")
set(NCURSES_LIB_SO "${MUSL_SYSROOT}/lib/libncurses.so")
set(NCURSES_INCLUDE_DIR "${NCURSES_BUILD_DIR}/include")
set(MUSL_COMPAT_DIR "${CMAKE_SOURCE_DIR}/user/musl-xv6/compat")

if(ARCH STREQUAL "x86_64")
    set(NCURSES_HOST_TRIPLE "x86_64-linux-gnu")
    find_program(NCURSES_GCC_PROG x86_64-xv6-linux-musl-gcc
        PATHS ${CMAKE_SOURCE_DIR}/toolchain/x86_64/phase2/bin
              ${CMAKE_SOURCE_DIR}/toolchain/x86_64/bin
        NO_DEFAULT_PATH)
    if(NOT NCURSES_GCC_PROG)
        find_program(NCURSES_GCC_PROG x86_64-xv6-linux-musl-gcc)
    endif()
else()
    set(NCURSES_HOST_TRIPLE "riscv64-linux-gnu")
    find_program(NCURSES_GCC_PROG riscv64-xv6-linux-musl-gcc
        PATHS ${CMAKE_SOURCE_DIR}/toolchain/riscv64/phase2/bin
              ${CMAKE_SOURCE_DIR}/toolchain/riscv64/bin
        NO_DEFAULT_PATH)
    if(NOT NCURSES_GCC_PROG)
        find_program(NCURSES_GCC_PROG riscv64-xv6-linux-musl-gcc)
    endif()
endif()

if(NCURSES_GCC_PROG)
    string(REGEX REPLACE "gcc$" "" NCURSES_CROSS_PREFIX "${NCURSES_GCC_PROG}")
    set(NCURSES_CC "${NCURSES_GCC_PROG}")
    set(NCURSES_AR "${NCURSES_CROSS_PREFIX}ar")
    set(NCURSES_RANLIB "${NCURSES_CROSS_PREFIX}ranlib")
else()
    set(NCURSES_CC "${CMAKE_C_COMPILER}")
    set(NCURSES_AR "${CMAKE_AR}")
    set(NCURSES_RANLIB "${CMAKE_RANLIB}")
endif()

if(EXISTS "${NCURSES_SRC_DIR}/configure")
    add_custom_command(
        OUTPUT ${NCURSES_LIB_A}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${NCURSES_BUILD_DIR}
        COMMAND ${CMAKE_COMMAND} -E chdir ${NCURSES_BUILD_DIR}
                env
            CC=${NCURSES_CC}
            AR=${NCURSES_AR}
            RANLIB=${NCURSES_RANLIB}
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
        DEPENDS ${MUSL_SYSROOT_DEP_FILES} ${NCURSES_SRC_DIR}/configure ${NCURSES_SRC_DIR}/misc/terminfo.src
        COMMENT "Building ncurses static library (musl)"
    )

    add_custom_target(local_ncurses_build DEPENDS ${NCURSES_LIB_A})

    add_custom_command(
        OUTPUT ${NCURSES_LIB_SO}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${NCURSES_SHARED_BUILD_DIR}
        COMMAND ${CMAKE_COMMAND} -E chdir ${NCURSES_SHARED_BUILD_DIR}
                env
            CC=${NCURSES_CC}
            AR=${NCURSES_AR}
            RANLIB=${NCURSES_RANLIB}
                CFLAGS=--sysroot=${MUSL_SYSROOT}\ -O2\ -fPIC\ -nostdinc\ -ffreestanding\ -isystem\ ${MUSL_INCLUDE_DIR}\ -isystem\ ${GCC_INCLUDE_DIR_MUSL}\ -isystem\ ${MUSL_COMPAT_DIR}
                LDFLAGS=-L${MUSL_SYSROOT}/lib
                ${NCURSES_SRC_DIR}/configure
                    --host=${NCURSES_HOST_TRIPLE}
                    --prefix=/
                    --disable-widec
                    --with-shared
                    --without-normal
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
        COMMAND ${CMAKE_COMMAND} -E chdir ${NCURSES_SHARED_BUILD_DIR}/include make -j1
        COMMAND ${CMAKE_COMMAND} -E chdir ${NCURSES_SHARED_BUILD_DIR}/ncurses make -j1 ../lib/libncurses.so.6.4
        COMMAND ${CMAKE_COMMAND} -E make_directory ${MUSL_SYSROOT}/lib
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${NCURSES_SHARED_BUILD_DIR}/lib/libncurses.so.6.4 ${MUSL_SYSROOT}/lib/libncurses.so.6.4
        COMMAND ${CMAKE_COMMAND} -E create_symlink libncurses.so.6.4 ${MUSL_SYSROOT}/lib/libncurses.so.6
        COMMAND ${CMAKE_COMMAND} -E create_symlink libncurses.so.6 ${MUSL_SYSROOT}/lib/libncurses.so
        DEPENDS ${MUSL_SYSROOT_DEP_FILES} ${NCURSES_SRC_DIR}/configure ${NCURSES_SRC_DIR}/misc/terminfo.src
        COMMENT "Building ncurses shared library (musl)"
    )

    add_custom_target(local_ncurses_shared_build DEPENDS ${NCURSES_LIB_SO})
else()
    message(STATUS "Skipping ncurses build (missing submodule at ${NCURSES_SRC_DIR})")
endif()

# ==============================================================================
# readline — Line editor library (static, built against musl + ncurses)
# ==============================================================================
set(READLINE_SRC_DIR "${CMAKE_SOURCE_DIR}/user/readline")
set(READLINE_BUILD_DIR "${CMAKE_BINARY_DIR}/readline/build-xv6")
set(READLINE_LIB_A "${READLINE_BUILD_DIR}/libreadline.a")
set(READLINE_SHARED_BUILD_DIR "${CMAKE_BINARY_DIR}/readline/build-xv6-shared")
set(READLINE_LIB_SO "${MUSL_SYSROOT}/lib/libreadline.so")
set(READLINE_INCLUDE_DIR "${READLINE_BUILD_DIR}")

if(EXISTS "${READLINE_SRC_DIR}/configure")
    add_custom_command(
        OUTPUT ${READLINE_LIB_A}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${READLINE_BUILD_DIR}
        COMMAND ${CMAKE_COMMAND} -E chdir ${READLINE_BUILD_DIR}
                env
            CC=${NCURSES_CC}
            AR=${NCURSES_AR}
            RANLIB=${NCURSES_RANLIB}
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
            COMMAND ${CMAKE_COMMAND} -E touch ${READLINE_LIB_A}
        DEPENDS ${NCURSES_LIB_A} ${READLINE_SRC_DIR}/configure
        COMMENT "Building and installing readline (musl)"
    )

    add_custom_target(local_readline_build DEPENDS ${READLINE_LIB_A})

    add_custom_command(
        OUTPUT ${READLINE_LIB_SO}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${READLINE_SHARED_BUILD_DIR}
        COMMAND ${CMAKE_COMMAND} -E chdir ${READLINE_SHARED_BUILD_DIR}
                env
            CC=${NCURSES_CC}
            AR=${NCURSES_AR}
            RANLIB=${NCURSES_RANLIB}
                CFLAGS=--sysroot=${MUSL_SYSROOT}\ -O2\ -fPIC\ -nostdinc\ -ffreestanding\ -D_POSIX_VERSION=200809L\ -isystem\ ${MUSL_INCLUDE_DIR}\ -isystem\ ${GCC_INCLUDE_DIR_MUSL}\ -isystem\ ${MUSL_COMPAT_DIR}\ -I${NCURSES_INCLUDE_DIR}
                LDFLAGS=-L${MUSL_SYSROOT}/lib
                ${READLINE_SRC_DIR}/configure
                    --host=${USER_HOST_TRIPLE}
                    --prefix=/
                    --enable-shared
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
        COMMAND ${CMAKE_COMMAND} -E chdir ${READLINE_SHARED_BUILD_DIR} make -j4
        COMMAND ${CMAKE_COMMAND} -E make_directory ${MUSL_SYSROOT}/lib
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${READLINE_SHARED_BUILD_DIR}/shlib/libreadline.so.8.2 ${MUSL_SYSROOT}/lib/libreadline.so.8.2
        COMMAND ${CMAKE_COMMAND} -E create_symlink libreadline.so.8.2 ${MUSL_SYSROOT}/lib/libreadline.so.8
        COMMAND ${CMAKE_COMMAND} -E create_symlink libreadline.so.8 ${MUSL_SYSROOT}/lib/libreadline.so
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${READLINE_SHARED_BUILD_DIR}/shlib/libhistory.so.8.2 ${MUSL_SYSROOT}/lib/libhistory.so.8.2
        COMMAND ${CMAKE_COMMAND} -E create_symlink libhistory.so.8.2 ${MUSL_SYSROOT}/lib/libhistory.so.8
        COMMAND ${CMAKE_COMMAND} -E create_symlink libhistory.so.8 ${MUSL_SYSROOT}/lib/libhistory.so
        DEPENDS ${NCURSES_LIB_SO} ${READLINE_SRC_DIR}/configure
        COMMENT "Building readline shared libraries (musl)"
    )

    add_custom_target(local_readline_shared_build DEPENDS ${READLINE_LIB_SO})
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
                LDFLAGS=-no-pie\ -nostartfiles\ -nostdlib\ ${MUSL_CRT1_O}\ ${MUSL_CRTI_O}\ -Wl,--dynamic-linker=${MUSL_DYNAMIC_LINKER_PATH}\ -Wl,-z,max-page-size=0x1000\ -Wl,-z,common-page-size=0x1000\ -Wl,--build-id=none\ -L${HISTEDIT_SHIM_DIR}\ -L${READLINE_BUILD_DIR}\ -L${NCURSES_BUILD_DIR}/lib\ -L${MUSL_SYSROOT}/lib
                LIBS=${HISTEDIT_SHIM_A}\ ${READLINE_LIB_A}\ ${NCURSES_LIB_A}\ -lc\ ${LIBGCC_PATH}\ ${MUSL_CRTN_O}
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
        COMMENT "Configuring dash cross build (dynamic musl, with readline-based libedit)"
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
    DEPENDS ${KQUEUE_COMPAT_SRC} ${MUSL_SYSROOT_DEP_FILES}
    COMMENT "Compiling kqueue compat object for musl"
)
add_custom_target(kqueue_compat DEPENDS ${KQUEUE_COMPAT_OBJ})

set(RISCV_ICACHE_COMPAT_SRC "${CMAKE_SOURCE_DIR}/user/musl-xv6/compat/riscv_flush_icache.c")
set(RISCV_ICACHE_COMPAT_OBJ "")
if(ARCH STREQUAL "riscv")
    set(RISCV_ICACHE_COMPAT_OBJ "${CMAKE_BINARY_DIR}/user/riscv_flush_icache_compat.o")
    add_custom_command(
        OUTPUT ${RISCV_ICACHE_COMPAT_OBJ}
        COMMAND ${CMAKE_C_COMPILER}
                -c -o ${RISCV_ICACHE_COMPAT_OBJ}
                --sysroot=${MUSL_SYSROOT}
                -isystem ${MUSL_INCLUDE_DIR}
                -isystem ${GCC_INCLUDE_DIR_MUSL}
                -isystem ${MUSL_COMPAT_DIR}
                -nostdinc -O2 -fPIC
                ${USER_ARCH_CFLAGS_CMD}
                ${RISCV_ICACHE_COMPAT_SRC}
        DEPENDS ${RISCV_ICACHE_COMPAT_SRC} ${MUSL_SYSROOT_DEP_FILES}
        COMMENT "Compiling RISC-V icache flush compat object for musl"
    )
endif()

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
            CFLAGS=--sysroot=${MUSL_SYSROOT}\ -O2\ -fPIC
            LDFLAGS=-no-pie\ -nostartfiles\ -nostdlib\ ${MUSL_CRT1_O}\ ${MUSL_CRTI_O}\ -Wl,--dynamic-linker=${MUSL_DYNAMIC_LINKER_PATH}\ -Wl,-z,max-page-size=0x1000\ -Wl,-z,common-page-size=0x1000\ -Wl,--build-id=none\ -L${MUSL_SYSROOT}/lib
            EX_LIBS=-lc\ ${LIBGCC_PATH}\ ${MUSL_CRTN_O}
                perl ${OPENSSL_SRC_DIR}/Configure
                    ${OPENSSL_TARGET}
                    --cross-compile-prefix=${OPENSSL_CROSS_PREFIX}
                    --prefix=/
                    --openssldir=/etc/ssl
                    --libdir=lib
                    enable-pic
                    threads
                    no-shared
                    no-dso
                    no-engine
                    no-async
                    no-asm
                    no-tests
                    no-ui-console
                    no-afalgeng
                    no-ktls
                    no-devcryptoeng
                    -DOPENSSL_NO_SECURE_MEMORY
                    -DOPENSSL_RAND_SEED_GETRANDOM
                    -O2
        DEPENDS ${MUSL_SYSROOT_DEP_FILES} ${OPENSSL_SRC_DIR}/Configure
        COMMENT "Configuring OpenSSL 3.0 cross build (${OPENSSL_TARGET}, dynamic executables)"
    )

    add_custom_target(openssl_configure DEPENDS ${OPENSSL_BUILD_DIR}/Makefile)

    # Build OpenSSL libraries (libcrypto.a + libssl.a)
    add_custom_command(
        OUTPUT ${OPENSSL_LIBCRYPTO_A} ${OPENSSL_LIBSSL_A}
        COMMAND ${CMAKE_COMMAND} -E chdir ${OPENSSL_BUILD_DIR}
                make -j1 build_libs
        DEPENDS ${OPENSSL_BUILD_DIR}/Makefile
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
                make -j1 EX_LIBS=-lc\ ${LIBGCC_PATH}\ ${MUSL_CRTN_O} build_programs
        DEPENDS openssl_build
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
                LDFLAGS=-no-pie\ -nostartfiles\ -nostdlib\ ${MUSL_CRT1_O}\ ${MUSL_CRTI_O}\ -Wl,--dynamic-linker=${MUSL_DYNAMIC_LINKER_PATH}\ -Wl,-z,max-page-size=0x1000\ -Wl,-z,common-page-size=0x1000\ -Wl,--build-id=none\ -L${OPENSSL_INSTALL_DIR}/lib\ -L${MUSL_SYSROOT}/lib
                LIBS=-lcrypto\ -lc\ ${LIBGCC_PATH}\ ${MUSL_CRTN_O}
                ${OPENSSH_SRC_DIR}/configure
                    --host=${OPENSSH_HOST_TRIPLE}
                    --build=x86_64-pc-linux-gnu
                    --prefix=/
                    --without-pie
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
        COMMENT "Configuring OpenSSH cross build (${OPENSSH_HOST_TRIPLE}, dynamic musl + OpenSSL)"
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

# ==============================================================================
# SQLite 3 — database engine + shell (static, installed into musl sysroot)
# ==============================================================================
set(SQLITE_SRC_DIR "${CMAKE_SOURCE_DIR}/user/sqlite")
set(SQLITE_BUILD_DIR "${CMAKE_BINARY_DIR}/sqlite/build-xv6")
set(SQLITE_PREFIX_DIR "${MUSL_SYSROOT}")
set(SQLITE_BIN "${SQLITE_PREFIX_DIR}/bin/sqlite3")
set(SQLITE_LIB_A "${SQLITE_PREFIX_DIR}/lib/libsqlite3.a")
set(SQLITE_HEADER "${SQLITE_PREFIX_DIR}/include/sqlite3.h")
set(SQLITE_SYSROOT_LIB "${MUSL_SYSROOT}/lib/libsqlite3.a")
set(SQLITE_SYSROOT_HEADER "${MUSL_SYSROOT}/include/sqlite3.h")

set(CPYTHON_SQLITE_DEPENDS "")
set(CPYTHON_SQLITE_LIBS "")
set(CPYTHON_SQLITE_CONFIGURE_OVERRIDES "")
set(CPYTHON_EXTRA_DEPENDS "")
set(CPYTHON_EXTRA_DEP_FILES "")
set(CPYTHON_EXTRA_CONFIGURE_OVERRIDES "")
set(CPYTHON_LIBFFI_CFLAGS "")
set(CPYTHON_LIBFFI_LIBS "")
set(CPYTHON_MODULE_RUNTIME_LIBS "")

if(EXISTS "${SQLITE_SRC_DIR}/configure")
    add_custom_command(
        OUTPUT ${SQLITE_BUILD_DIR}/Makefile
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SQLITE_BUILD_DIR}
        COMMAND ${CMAKE_COMMAND} -E chdir ${SQLITE_BUILD_DIR}
                env
                CC=${CMAKE_C_COMPILER}
                AR=${CMAKE_AR}
                RANLIB=${CMAKE_RANLIB}
                CC_FOR_BUILD=cc
                CFLAGS=--sysroot=${MUSL_SYSROOT}\ -O2\ -fPIC\ -nostdinc\ -ffreestanding\ -fno-common\ ${USER_ARCH_CFLAGS}\ -DSQLITE_OMIT_LOAD_EXTENSION=1\ -DSQLITE_OMIT_WAL=1\ -DSQLITE_MAX_MMAP_SIZE=0\ -DSQLITE_TEMP_STORE=2\ -DSQLITE_THREADSAFE=1\ -DSQLITE_USE_URI=1\ -isystem\ ${MUSL_INCLUDE_DIR}\ -isystem\ ${GCC_INCLUDE_DIR_MUSL}\ -isystem\ ${MUSL_COMPAT_DIR}
                LDFLAGS=-no-pie\ -nostartfiles\ -nostdlib\ ${MUSL_CRT1_O}\ ${MUSL_CRTI_O}\ -Wl,--dynamic-linker=${MUSL_DYNAMIC_LINKER_PATH}\ -Wl,-z,max-page-size=0x1000\ -Wl,-z,common-page-size=0x1000\ -Wl,--build-id=none\ -L${MUSL_SYSROOT}/lib\ -lc\ ${LIBGCC_PATH}\ ${MUSL_CRTN_O}
                ${SQLITE_SRC_DIR}/configure
                    --host=${USER_HOST_TRIPLE}
                    --prefix=${SQLITE_PREFIX_DIR}
                    --bindir=${SQLITE_PREFIX_DIR}/bin
                    --libdir=${SQLITE_PREFIX_DIR}/lib
                    --includedir=${SQLITE_PREFIX_DIR}/include
                    --disable-shared
                    --disable-readline
                    --disable-math
                    --disable-load-extension
                    ac_cv_func_fdatasync=yes
                    ac_cv_func_usleep=yes
                    ac_cv_func_localtime_r=yes
                    ac_cv_func_gmtime_r=yes
                    ac_cv_func_malloc_usable_size=no
                    ac_cv_func_posix_fallocate=no
        DEPENDS ${MUSL_SYSROOT_DEP_FILES} ${SQLITE_SRC_DIR}/configure
        COMMENT "Configuring SQLite cross build (dynamic musl)"
    )

    add_custom_target(sqlite_configure DEPENDS ${SQLITE_BUILD_DIR}/Makefile)

    add_custom_command(
        OUTPUT ${SQLITE_BIN} ${SQLITE_LIB_A} ${SQLITE_HEADER}
        COMMAND ${CMAKE_COMMAND} -E chdir ${SQLITE_BUILD_DIR} make -j16
        COMMAND ${CMAKE_COMMAND} -E chdir ${SQLITE_BUILD_DIR} make install
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SQLITE_PREFIX_DIR}/bin
        COMMAND ${CMAKE_C_COMPILER}
                --sysroot=${MUSL_SYSROOT}
                -O2 -nostdinc -ffreestanding -fno-common -fno-pie -no-pie
                ${USER_ARCH_CFLAGS_CMD}
                -D_HAVE_SQLITE_CONFIG_H -DBUILD_sqlite -DNDEBUG
                -DSQLITE_OMIT_LOAD_EXTENSION=1 -DSQLITE_OMIT_WAL=1
                -DSQLITE_MAX_MMAP_SIZE=0 -DSQLITE_TEMP_STORE=2
                -DSQLITE_THREADSAFE=1 -DSQLITE_USE_URI=1
                -isystem ${MUSL_INCLUDE_DIR}
                -isystem ${GCC_INCLUDE_DIR_MUSL}
                -isystem ${MUSL_COMPAT_DIR}
                -I${SQLITE_BUILD_DIR}
                -I${SQLITE_SRC_DIR}/src
                -I${SQLITE_SRC_DIR}/ext/rtree
                -I${SQLITE_SRC_DIR}/ext/icu
                -I${SQLITE_SRC_DIR}/ext/fts3
                -I${SQLITE_SRC_DIR}/ext/session
                -I${SQLITE_SRC_DIR}/ext/misc
                -no-pie -nostartfiles -nostdlib
                ${MUSL_CRT1_O}
                ${MUSL_CRTI_O}
                -Wl,--dynamic-linker=${MUSL_DYNAMIC_LINKER_PATH}
                -Wl,-z,max-page-size=0x1000
                -Wl,-z,common-page-size=0x1000
                -Wl,--build-id=none
                -L${MUSL_SYSROOT}/lib
                -o ${SQLITE_BIN}
                ${SQLITE_BUILD_DIR}/shell.c
                ${SQLITE_BUILD_DIR}/sqlite3.c
                -lc
                ${LIBGCC_PATH}
                ${MUSL_CRTN_O}
        DEPENDS ${SQLITE_BUILD_DIR}/Makefile
        COMMENT "Building SQLite static library and shell"
    )

    add_custom_target(sqlite_build DEPENDS ${SQLITE_BIN} ${SQLITE_LIB_A} ${SQLITE_HEADER})
    add_dependencies(user_programs sqlite_build)

    set(CPYTHON_SQLITE_DEPENDS sqlite_build)
    set(CPYTHON_SQLITE_LIBS " -lsqlite3")
    set(CPYTHON_SQLITE_CONFIGURE_OVERRIDES
        ac_cv_header_sqlite3_h=yes
        ac_cv_lib_sqlite3_sqlite3_open=yes
        ac_cv_search_sqlite3_open=none\ required)
    list(APPEND CPYTHON_EXTRA_DEPENDS sqlite_build)
    list(APPEND CPYTHON_EXTRA_DEP_FILES ${SQLITE_LIB_A} ${SQLITE_HEADER})
    list(APPEND CPYTHON_EXTRA_CONFIGURE_OVERRIDES ${CPYTHON_SQLITE_CONFIGURE_OVERRIDES})
else()
    message(STATUS "Skipping SQLite build (missing source tree at ${SQLITE_SRC_DIR})")
endif()

# ==============================================================================
# zlib — compression library for CPython `zlib`
# ==============================================================================
set(ZLIB_SRC_DIR "${CMAKE_SOURCE_DIR}/user/zlib")
set(ZLIB_BUILD_DIR "${CMAKE_BINARY_DIR}/zlib/build-xv6")
set(ZLIB_LIB_A "${MUSL_SYSROOT}/lib/libz.a")
set(ZLIB_HEADER "${MUSL_SYSROOT}/include/zlib.h")

if(EXISTS "${ZLIB_SRC_DIR}/CMakeLists.txt")
    add_custom_command(
        OUTPUT ${ZLIB_LIB_A} ${ZLIB_HEADER}
        COMMAND ${CMAKE_COMMAND}
                -S ${ZLIB_SRC_DIR}
                -B ${ZLIB_BUILD_DIR}
                -DCMAKE_SYSTEM_NAME=Linux
                -DCMAKE_BUILD_TYPE=Release
                -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                -DCMAKE_AR=${CMAKE_AR}
                -DCMAKE_RANLIB=${CMAKE_RANLIB}
                -DCMAKE_INSTALL_PREFIX=/
            -DCMAKE_INSTALL_LIBDIR=/lib
            -DCMAKE_INSTALL_INCLUDEDIR=/include
                -DCMAKE_POSITION_INDEPENDENT_CODE=ON
                -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
                -DCMAKE_C_FLAGS=--sysroot=${MUSL_SYSROOT}\ -O2\ -fPIC\ -nostdinc\ -ffreestanding\ ${USER_ARCH_CFLAGS}\ -isystem\ ${MUSL_INCLUDE_DIR}\ -isystem\ ${GCC_INCLUDE_DIR_MUSL}\ -isystem\ ${MUSL_COMPAT_DIR}
                -DZLIB_BUILD_SHARED=OFF
                -DZLIB_BUILD_STATIC=ON
                -DZLIB_BUILD_TESTING=OFF
                -DZLIB_INSTALL=ON
        COMMAND ${CMAKE_COMMAND} --build ${ZLIB_BUILD_DIR} --target zlibstatic -j16
        COMMAND ${CMAKE_COMMAND} -E env DESTDIR=${MUSL_SYSROOT} ${CMAKE_COMMAND} --install ${ZLIB_BUILD_DIR}
        DEPENDS ${MUSL_SYSROOT_DEP_FILES} ${ZLIB_SRC_DIR}/CMakeLists.txt
        COMMENT "Building zlib static library for CPython"
    )
    add_custom_target(zlib_build DEPENDS ${ZLIB_LIB_A} ${ZLIB_HEADER})
    list(APPEND CPYTHON_EXTRA_DEPENDS zlib_build)
    list(APPEND CPYTHON_EXTRA_DEP_FILES ${ZLIB_LIB_A} ${ZLIB_HEADER})
else()
    message(STATUS "Skipping zlib build (missing source tree at ${ZLIB_SRC_DIR})")
endif()

# ==============================================================================
# bzip2 — compression library for CPython `_bz2`
# ==============================================================================
set(BZIP2_SRC_DIR "${CMAKE_SOURCE_DIR}/user/bzip2")
set(BZIP2_BUILD_DIR "${CMAKE_BINARY_DIR}/bzip2/build-xv6")
set(BZIP2_LIB_A "${MUSL_SYSROOT}/lib/libbz2.a")
set(BZIP2_HEADER "${MUSL_SYSROOT}/include/bzlib.h")

if(EXISTS "${BZIP2_SRC_DIR}/CMakeLists.txt")
    add_custom_command(
        OUTPUT ${BZIP2_LIB_A} ${BZIP2_HEADER}
        COMMAND ${CMAKE_COMMAND}
                -S ${BZIP2_SRC_DIR}
                -B ${BZIP2_BUILD_DIR}
                -DCMAKE_SYSTEM_NAME=Linux
                -DCMAKE_BUILD_TYPE=Release
                -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                -DCMAKE_AR=${CMAKE_AR}
                -DCMAKE_RANLIB=${CMAKE_RANLIB}
                -DCMAKE_INSTALL_PREFIX=/
            -DCMAKE_INSTALL_LIBDIR=/lib
            -DCMAKE_INSTALL_INCLUDEDIR=/include
                -DCMAKE_POSITION_INDEPENDENT_CODE=ON
                -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
                -DCMAKE_C_FLAGS=--sysroot=${MUSL_SYSROOT}\ -O2\ -fPIC\ -nostdinc\ -ffreestanding\ ${USER_ARCH_CFLAGS}\ -isystem\ ${MUSL_INCLUDE_DIR}\ -isystem\ ${GCC_INCLUDE_DIR_MUSL}\ -isystem\ ${MUSL_COMPAT_DIR}
                -DENABLE_LIB_ONLY=ON
                -DENABLE_APP=OFF
                -DENABLE_EXAMPLES=OFF
                -DENABLE_DOCS=OFF
                -DENABLE_SHARED_LIB=OFF
                -DENABLE_STATIC_LIB=ON
        COMMAND ${CMAKE_COMMAND} --build ${BZIP2_BUILD_DIR} --target bz2_static -j16
        COMMAND ${CMAKE_COMMAND} -E make_directory ${MUSL_SYSROOT}/lib
        COMMAND ${CMAKE_COMMAND} -E make_directory ${MUSL_SYSROOT}/include
        COMMAND ${CMAKE_COMMAND} -E make_directory ${MUSL_SYSROOT}/lib/pkgconfig
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BZIP2_BUILD_DIR}/libbz2_static.a ${BZIP2_LIB_A}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BZIP2_SRC_DIR}/bzlib.h ${BZIP2_HEADER}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BZIP2_BUILD_DIR}/bzip2.pc ${MUSL_SYSROOT}/lib/pkgconfig/bzip2.pc
        DEPENDS ${MUSL_SYSROOT_DEP_FILES} ${BZIP2_SRC_DIR}/CMakeLists.txt
        COMMENT "Building bzip2 static library for CPython"
    )
    add_custom_target(bzip2_build DEPENDS ${BZIP2_LIB_A} ${BZIP2_HEADER})
    list(APPEND CPYTHON_EXTRA_DEPENDS bzip2_build)
    list(APPEND CPYTHON_EXTRA_DEP_FILES ${BZIP2_LIB_A} ${BZIP2_HEADER})
else()
    message(STATUS "Skipping bzip2 build (missing source tree at ${BZIP2_SRC_DIR})")
endif()

# ==============================================================================
# xz/liblzma — compression library for CPython `_lzma`
# ==============================================================================
set(XZ_SRC_DIR "${CMAKE_SOURCE_DIR}/user/xz")
set(XZ_BUILD_DIR "${CMAKE_BINARY_DIR}/xz/build-xv6")
set(XZ_LIB_A "${MUSL_SYSROOT}/lib/liblzma.a")
set(XZ_HEADER "${MUSL_SYSROOT}/include/lzma.h")

if(EXISTS "${XZ_SRC_DIR}/CMakeLists.txt")
    add_custom_command(
        OUTPUT ${XZ_LIB_A} ${XZ_HEADER}
        COMMAND ${CMAKE_COMMAND}
                -S ${XZ_SRC_DIR}
                -B ${XZ_BUILD_DIR}
                -DCMAKE_SYSTEM_NAME=Linux
                -DCMAKE_BUILD_TYPE=Release
                -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                -DCMAKE_AR=${CMAKE_AR}
                -DCMAKE_RANLIB=${CMAKE_RANLIB}
                -DCMAKE_INSTALL_PREFIX=/
            -DCMAKE_INSTALL_LIBDIR=/lib
            -DCMAKE_INSTALL_INCLUDEDIR=/include
                -DCMAKE_POSITION_INDEPENDENT_CODE=ON
                -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
                -DCMAKE_C_FLAGS=--sysroot=${MUSL_SYSROOT}\ -O2\ -fPIC\ -nostdinc\ -ffreestanding\ ${USER_ARCH_CFLAGS}\ -isystem\ ${MUSL_INCLUDE_DIR}\ -isystem\ ${GCC_INCLUDE_DIR_MUSL}\ -isystem\ ${MUSL_COMPAT_DIR}
                -DBUILD_SHARED_LIBS=OFF
                -DXZ_NLS=OFF
            -DXZ_THREADS=no
                -DXZ_SYMBOL_VERSIONING=no
        COMMAND ${CMAKE_COMMAND} --build ${XZ_BUILD_DIR} --target liblzma -j16
        COMMAND ${CMAKE_COMMAND} -E env DESTDIR=${MUSL_SYSROOT} ${CMAKE_COMMAND} --install ${XZ_BUILD_DIR} --component liblzma_Development
        DEPENDS ${MUSL_SYSROOT_DEP_FILES} ${XZ_SRC_DIR}/CMakeLists.txt
        COMMENT "Building liblzma static library for CPython"
    )
    add_custom_target(xz_build DEPENDS ${XZ_LIB_A} ${XZ_HEADER})
    list(APPEND CPYTHON_EXTRA_DEPENDS xz_build)
    list(APPEND CPYTHON_EXTRA_DEP_FILES ${XZ_LIB_A} ${XZ_HEADER})
else()
    message(STATUS "Skipping xz build (missing source tree at ${XZ_SRC_DIR})")
endif()

# ==============================================================================
# libffi — foreign-function interface library for CPython `_ctypes`
# ==============================================================================
set(LIBFFI_SRC_DIR "${CMAKE_SOURCE_DIR}/user/libffi")
set(LIBFFI_BUILD_DIR "${CMAKE_BINARY_DIR}/libffi/build-xv6")
set(LIBFFI_VERSION "3.5.2")
set(LIBFFI_LIB_A "${MUSL_SYSROOT}/lib/libffi.a")
set(LIBFFI_INCLUDE_DIR "${MUSL_SYSROOT}/include")
set(LIBFFI_HEADER "${MUSL_SYSROOT}/include/ffi.h")

if(EXISTS "${LIBFFI_SRC_DIR}/configure.ac")
    add_custom_command(
        OUTPUT ${LIBFFI_BUILD_DIR}/Makefile
        COMMAND ${CMAKE_COMMAND} -E make_directory ${LIBFFI_BUILD_DIR}
        COMMAND ${CMAKE_COMMAND} -E chdir ${LIBFFI_SRC_DIR} autoreconf -fi
        COMMAND ${CMAKE_COMMAND} -E chdir ${LIBFFI_BUILD_DIR}
                env
                CC=${CMAKE_C_COMPILER}
                AR=${CMAKE_AR}
                RANLIB=${CMAKE_RANLIB}
                CFLAGS=--sysroot=${MUSL_SYSROOT}\ -O2\ -fPIC\ -nostdinc\ -ffreestanding\ ${USER_ARCH_CFLAGS}\ -isystem\ ${MUSL_INCLUDE_DIR}\ -isystem\ ${GCC_INCLUDE_DIR_MUSL}\ -isystem\ ${MUSL_COMPAT_DIR}
                ${LIBFFI_SRC_DIR}/configure
                    --host=${USER_HOST_TRIPLE}
                    --prefix=/
                    --disable-shared
                    --enable-static
                    --disable-docs
        DEPENDS ${MUSL_SYSROOT_DEP_FILES} ${LIBFFI_SRC_DIR}/configure.ac
        COMMENT "Configuring libffi cross build for CPython"
    )

    add_custom_command(
        OUTPUT ${LIBFFI_LIB_A} ${LIBFFI_HEADER}
        COMMAND ${CMAKE_COMMAND} -E chdir ${LIBFFI_BUILD_DIR} make -j16
        COMMAND ${CMAKE_COMMAND} -E chdir ${LIBFFI_BUILD_DIR} make install DESTDIR=${MUSL_SYSROOT}
        DEPENDS ${LIBFFI_BUILD_DIR}/Makefile
        COMMENT "Building libffi static library for CPython"
    )
    add_custom_target(libffi_build DEPENDS ${LIBFFI_LIB_A} ${LIBFFI_HEADER})
    list(APPEND CPYTHON_EXTRA_DEPENDS libffi_build)
    list(APPEND CPYTHON_EXTRA_DEP_FILES ${LIBFFI_LIB_A} ${LIBFFI_HEADER})
    set(CPYTHON_LIBFFI_CFLAGS "-I${LIBFFI_INCLUDE_DIR} -I${MUSL_SYSROOT}/include")
    set(CPYTHON_LIBFFI_LIBS "-lffi")
else()
    message(STATUS "Skipping libffi build (missing source tree at ${LIBFFI_SRC_DIR})")
endif()

# ==============================================================================
# libuuid compat — minimal UUID generator for CPython `_uuid`
# ==============================================================================
set(UUID_COMPAT_SRC "${CMAKE_SOURCE_DIR}/user/musl-xv6/compat/libuuid.c")
set(UUID_COMPAT_HEADER_SRC "${CMAKE_SOURCE_DIR}/user/musl-xv6/compat/uuid/uuid.h")
set(UUID_BUILD_DIR "${CMAKE_BINARY_DIR}/uuid/build-xv6")
set(UUID_COMPAT_OBJ "${UUID_BUILD_DIR}/libuuid.o")
set(UUID_LIB_A "${MUSL_SYSROOT}/lib/libuuid.a")
set(UUID_HEADER_DIR "${MUSL_SYSROOT}/include/uuid")
set(UUID_HEADER "${UUID_HEADER_DIR}/uuid.h")
set(UUID_HEADER_FLAT "${MUSL_SYSROOT}/include/uuid.h")

if(EXISTS "${UUID_COMPAT_SRC}" AND EXISTS "${UUID_COMPAT_HEADER_SRC}")
    add_custom_command(
        OUTPUT ${UUID_LIB_A} ${UUID_HEADER} ${UUID_HEADER_FLAT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${UUID_BUILD_DIR}
        COMMAND ${CMAKE_C_COMPILER}
                -c -o ${UUID_COMPAT_OBJ}
                --sysroot=${MUSL_SYSROOT}
                -O2 -fPIC -nostdinc -ffreestanding -fno-common
                ${USER_ARCH_CFLAGS_CMD}
                -isystem ${MUSL_INCLUDE_DIR}
                -isystem ${GCC_INCLUDE_DIR_MUSL}
                -isystem ${MUSL_COMPAT_DIR}
                -I${CMAKE_SOURCE_DIR}/user/musl-xv6/compat
                ${UUID_COMPAT_SRC}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${MUSL_SYSROOT}/lib
        COMMAND ${CMAKE_AR} rcs ${UUID_LIB_A} ${UUID_COMPAT_OBJ}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${UUID_HEADER_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${UUID_COMPAT_HEADER_SRC} ${UUID_HEADER}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${UUID_COMPAT_HEADER_SRC} ${UUID_HEADER_FLAT}
        DEPENDS ${MUSL_SYSROOT_DEP_FILES} ${UUID_COMPAT_SRC} ${UUID_COMPAT_HEADER_SRC}
        COMMENT "Building libuuid compatibility library for CPython"
    )
    add_custom_target(uuid_build DEPENDS ${UUID_LIB_A} ${UUID_HEADER} ${UUID_HEADER_FLAT})
    list(APPEND CPYTHON_EXTRA_DEPENDS uuid_build)
    list(APPEND CPYTHON_EXTRA_DEP_FILES ${UUID_LIB_A} ${UUID_HEADER} ${UUID_HEADER_FLAT})
    list(APPEND CPYTHON_EXTRA_CONFIGURE_OVERRIDES
        ac_cv_header_uuid_h=yes
        ac_cv_header_uuid_uuid_h=yes
        ac_cv_lib_uuid_uuid_generate_time=yes
        ac_cv_lib_uuid_uuid_generate_time_safe=yes)
else()
    message(STATUS "Skipping libuuid compat build (missing sources at ${UUID_COMPAT_SRC})")
endif()

# CPython — Python 3.12 interpreter (built against musl + ncurses + readline)
# ==============================================================================
set(CPYTHON_SRC_DIR "${CMAKE_SOURCE_DIR}/user/v6-cpython" CACHE PATH "CPython source directory")
set(CPYTHON_BUILD_DIR "${CMAKE_BINARY_DIR}/cpython/build-3.12" CACHE PATH "CPython out-of-tree build directory")
set(CPYTHON_BIN "${CPYTHON_BUILD_DIR}/python" CACHE FILEPATH "CPython interpreter binary path")
set(CPYTHON_SHARED_LIB "${MUSL_SYSROOT}/lib/libpython3.12d.so.1.0")
set(CPYTHON_INSTALL_STAMP "${CPYTHON_BUILD_DIR}/.sharedmods_installed")
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
    execute_process(
        COMMAND ${CPYTHON_CC} -print-file-name=libgcc_s.so.1
        OUTPUT_VARIABLE CPYTHON_LIBGCC_S_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    set(CPYTHON_SHARED_RUNTIME_LIBS "")
    if(EXISTS "${CPYTHON_LIBGCC_S_PATH}")
        set(CPYTHON_LIBGCC_S_SYSROOT "${MUSL_SYSROOT}/lib/libgcc_s.so.1")
        set(CPYTHON_LIBGCC_S_LINK "${MUSL_SYSROOT}/lib/libgcc_s.so")
        add_custom_command(
            OUTPUT ${CPYTHON_LIBGCC_S_SYSROOT} ${CPYTHON_LIBGCC_S_LINK}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${CPYTHON_LIBGCC_S_PATH}
                    ${CPYTHON_LIBGCC_S_SYSROOT}
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                    libgcc_s.so.1
                    ${CPYTHON_LIBGCC_S_LINK}
            DEPENDS ${CPYTHON_LIBGCC_S_PATH} ${MUSL_SYSROOT_DEP_FILES}
            COMMENT "Staging shared libgcc runtime into musl sysroot"
        )
        add_custom_target(cpython_libgcc_s_runtime DEPENDS ${CPYTHON_LIBGCC_S_SYSROOT} ${CPYTHON_LIBGCC_S_LINK})
        list(APPEND CPYTHON_EXTRA_DEPENDS cpython_libgcc_s_runtime)
        list(APPEND CPYTHON_EXTRA_DEP_FILES ${CPYTHON_LIBGCC_S_SYSROOT} ${CPYTHON_LIBGCC_S_LINK})
        set(CPYTHON_SHARED_RUNTIME_LIBS "-L${MUSL_SYSROOT}/lib -lgcc_s")
    endif()

    set(CPYTHON_MODULE_RUNTIME_LIBS "${CPYTHON_SHARED_RUNTIME_LIBS} -lm")
    if(CPYTHON_LIBFFI_LIBS)
        set(CPYTHON_LIBFFI_LIBS "${CPYTHON_LIBFFI_LIBS} ${CPYTHON_MODULE_RUNTIME_LIBS}")
    endif()
    if(RISCV_ICACHE_COMPAT_OBJ AND CPYTHON_LIBFFI_LIBS)
        set(CPYTHON_LIBFFI_LIBS "${CPYTHON_LIBFFI_LIBS} ${RISCV_ICACHE_COMPAT_OBJ}")
        list(APPEND CPYTHON_EXTRA_DEP_FILES ${RISCV_ICACHE_COMPAT_OBJ})
    endif()

    set(CPYTHON_EXTENSION_SUFFIX ".cpython-312d-${USER_HOST_TRIPLE}.so")
    set(CPYTHON_REQUIRED_SHARED_MODULE_TARGETS
        "Modules/_uuid${CPYTHON_EXTENSION_SUFFIX}")
    if(CPYTHON_LIBFFI_LIBS)
        list(APPEND CPYTHON_REQUIRED_SHARED_MODULE_TARGETS
            "Modules/_ctypes${CPYTHON_EXTENSION_SUFFIX}")
    endif()

    set(CPYTHON_SETUP_LOCAL_OPENSSL_LINES "")
    if(EXISTS "${OPENSSL_SRC_DIR}/Configure")
        string(APPEND CPYTHON_SETUP_LOCAL_OPENSSL_LINES
            "_ssl _ssl.c -I${OPENSSL_INSTALL_DIR}/include -L${OPENSSL_INSTALL_DIR}/lib -L${MUSL_SYSROOT}/lib -lssl -lcrypto ${CPYTHON_SHARED_RUNTIME_LIBS}\n"
            "_hashlib _hashopenssl.c -I${OPENSSL_INSTALL_DIR}/include -L${OPENSSL_INSTALL_DIR}/lib -L${MUSL_SYSROOT}/lib -lcrypto ${CPYTHON_SHARED_RUNTIME_LIBS}\n")
    endif()

    set(CPYTHON_SETUP_LOCAL_CURSES_LINES "")
    if(EXISTS "${NCURSES_SRC_DIR}/configure")
        string(APPEND CPYTHON_SETUP_LOCAL_CURSES_LINES
            "_curses _cursesmodule.c -I${MUSL_SYSROOT}/include -L${MUSL_SYSROOT}/lib -lncurses\n")
    endif()

    # Keep extension modules shared by default and only add explicit
    # overrides for modules whose link requirements are not detected
    # correctly during cross-configuration.
    set(CPYTHON_SETUP_LOCAL "${CPYTHON_BUILD_DIR}/Modules/Setup.local")
    set(SETUP_LOCAL_CONTENT [==[
*shared*
@CURSES_SETUP_LINES@
@OPENSSL_SETUP_LINES@
]==])
    string(REPLACE "@CURSES_SETUP_LINES@" "${CPYTHON_SETUP_LOCAL_CURSES_LINES}" SETUP_LOCAL_CONTENT "${SETUP_LOCAL_CONTENT}")
    string(REPLACE "@OPENSSL_SETUP_LINES@" "${CPYTHON_SETUP_LOCAL_OPENSSL_LINES}" SETUP_LOCAL_CONTENT "${SETUP_LOCAL_CONTENT}")
    if(EXISTS "${CMAKE_BINARY_DIR}/cpython_setup_local.txt")
        file(READ "${CMAKE_BINARY_DIR}/cpython_setup_local.txt" _cpython_setup_local_existing)
    else()
        set(_cpython_setup_local_existing "")
    endif()
    if(NOT _cpython_setup_local_existing STREQUAL SETUP_LOCAL_CONTENT)
        file(WRITE "${CMAKE_BINARY_DIR}/cpython_setup_local.txt" "${SETUP_LOCAL_CONTENT}")
    endif()

    add_custom_command(
        OUTPUT ${CPYTHON_BUILD_DIR}/Makefile
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CPYTHON_BUILD_DIR}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CPYTHON_BUILD_DIR}/Modules
        COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_BINARY_DIR}/cpython_setup_local.txt"
                ${CPYTHON_SETUP_LOCAL}
        COMMAND ${CMAKE_COMMAND} -E chdir ${CPYTHON_BUILD_DIR}
                env
                CC=${CPYTHON_CC}
                AR=${CPYTHON_AR}
                RANLIB=${CPYTHON_RANLIB}
                READELF=${CPYTHON_READELF}
                PKG_CONFIG=false
            "LIBFFI_CFLAGS=${CPYTHON_LIBFFI_CFLAGS}"
            "LIBFFI_LIBS=${CPYTHON_LIBFFI_LIBS}"
                CPPFLAGS=--sysroot=${MUSL_SYSROOT}\ -isystem\ ${MUSL_INCLUDE_DIR}\ -isystem\ ${MUSL_COMPAT_DIR}\ -I${OPENSSL_INSTALL_DIR}/include\ -I${NCURSES_INCLUDE_DIR}\ -I${CMAKE_SOURCE_DIR}/user\ -D_POSIX_C_SOURCE=200809L\ -DSSIZE_MAX=__LONG_MAX__\ -DSA_ONSTACK=0
                CFLAGS=--sysroot=${MUSL_SYSROOT}\ -isystem\ ${MUSL_INCLUDE_DIR}\ -isystem\ ${CPYTHON_GCC_INCLUDE_DIR}\ -isystem\ ${MUSL_COMPAT_DIR}\ -I${OPENSSL_INSTALL_DIR}/include\ -I${NCURSES_INCLUDE_DIR}\ -I${CMAKE_SOURCE_DIR}/user\ -D_POSIX_C_SOURCE=200809L\ -DSSIZE_MAX=__LONG_MAX__\ -DSA_ONSTACK=0
                LDFLAGS=-no-pie\ -nostartfiles\ -nostdlib\ ${MUSL_CRT1_O}\ ${MUSL_CRTI_O}\ -Wl,--export-dynamic\ -Wl,--dynamic-linker=${CPYTHON_DYNAMIC_LINKER}\ -Wl,-rpath,/lib\ -Wl,-z,max-page-size=0x1000\ -Wl,-z,common-page-size=0x1000\ -Wl,--build-id=none\ -L${OPENSSL_INSTALL_DIR}/lib\ -L${NCURSES_BUILD_DIR}/lib\ -L${READLINE_BUILD_DIR}\ -L${MUSL_SYSROOT}/lib
                "LIBS=-lreadline -lncurses${CPYTHON_SQLITE_LIBS} -lpthread -lm -lc ${CPYTHON_SHARED_RUNTIME_LIBS}"
                ${CPYTHON_SRC_DIR}/configure
                    --host=${CPYTHON_HOST_TRIPLE}
                    --build=x86_64-pc-linux-gnu
                    --prefix=/
                    --with-build-python=python3
                    --with-pydebug
                    --enable-shared
                    --without-static-libpython
                    --without-ensurepip
                    ac_cv_file__dev_ptmx=no
                    ac_cv_file__dev_ptc=no
                    ac_cv_header_readline_readline_h=yes
                    ac_cv_lib_readline_readline=yes
                    ac_cv_readline_append_history=yes
                    ac_cv_readline_rl_completion_display_matches_hook=yes
                    ac_cv_readline_rl_completion_matches=yes
                    ac_cv_readline_rl_pre_input_hook=yes
                    ac_cv_readline_rl_resize_terminal=yes
                    ac_cv_header_panel_h=no
                    ac_cv_lib_panel_update_panels=no
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
                    ${CPYTHON_EXTRA_CONFIGURE_OVERRIDES}
                    --disable-ipv6
        DEPENDS ${CPYTHON_SRC_DIR}/configure
            ${CMAKE_BINARY_DIR}/cpython_setup_local.txt
            ${MUSL_LINKER_SCRIPT}
            ${MUSL_CRT1_O}
            ${MUSL_CRTI_O}
            ${MUSL_CRTN_O}
            ${MUSL_SYSROOT}/lib/libc.so
            ${NCURSES_LIB_A}
            ${NCURSES_LIB_SO}
            ${READLINE_LIB_A}
            ${READLINE_LIB_SO}
            ${OPENSSL_INSTALL_DIR}/lib/libcrypto.a
            ${OPENSSL_INSTALL_DIR}/lib/libssl.a
            ${CPYTHON_EXTRA_DEP_FILES}
        COMMENT "Configuring CPython cross build (musl)"
    )

    add_custom_target(cpython_configure DEPENDS ${CPYTHON_BUILD_DIR}/Makefile)

        set(CPYTHON_SHARED_FLAGS "${CPYTHON_CC} -shared --sysroot=${MUSL_SYSROOT} -nostdlib -L${MUSL_SYSROOT}/lib -Wl,-rpath,/lib -lc ${CPYTHON_SHARED_RUNTIME_LIBS}")

    add_custom_command(
        OUTPUT ${CPYTHON_BIN} ${CPYTHON_SHARED_LIB} ${CPYTHON_INSTALL_STAMP}
        COMMAND ${CMAKE_COMMAND} -E chdir ${CPYTHON_BUILD_DIR} make -j16 python
            "SYSLIBS=${KQUEUE_COMPAT_OBJ} -lreadline -lncurses -lm -lc ${CPYTHON_SHARED_RUNTIME_LIBS} ${MUSL_CRTN_O}"
            "BLDSHARED=${CPYTHON_SHARED_FLAGS}"
            "LDSHARED=${CPYTHON_SHARED_FLAGS}"
        COMMAND ${CMAKE_COMMAND} -E chdir ${CPYTHON_BUILD_DIR} make -j16 sharedmods
            "SYSLIBS=${KQUEUE_COMPAT_OBJ} -lreadline -lncurses -lm -lc ${CPYTHON_SHARED_RUNTIME_LIBS} ${MUSL_CRTN_O}"
            "BLDSHARED=${CPYTHON_SHARED_FLAGS}"
            "LDSHARED=${CPYTHON_SHARED_FLAGS}"
        COMMAND ${CMAKE_COMMAND} -E chdir ${CPYTHON_BUILD_DIR} make -j1 ${CPYTHON_REQUIRED_SHARED_MODULE_TARGETS}
            "SYSLIBS=${KQUEUE_COMPAT_OBJ} -lreadline -lncurses -lm -lc ${CPYTHON_SHARED_RUNTIME_LIBS} ${MUSL_CRTN_O}"
            "BLDSHARED=${CPYTHON_SHARED_FLAGS}"
            "LDSHARED=${CPYTHON_SHARED_FLAGS}"
        COMMAND /bin/bash ${CMAKE_SOURCE_DIR}/scripts/cpython_post_install.sh ${CPYTHON_BUILD_DIR} ${MUSL_SYSROOT}
        COMMAND ${CMAKE_COMMAND} -E touch ${CPYTHON_INSTALL_STAMP}
        DEPENDS ${CPYTHON_BUILD_DIR}/Makefile ${KQUEUE_COMPAT_OBJ} ${CPYTHON_EXTRA_DEP_FILES}
        COMMENT "Building CPython with shared libpython support (musl)"
    )

        add_custom_target(cpython_build DEPENDS ${CPYTHON_BIN} ${CPYTHON_SHARED_LIB} ${CPYTHON_INSTALL_STAMP})
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
            -no-pie -nostartfiles -nostdlib
            -Wl,--dynamic-linker=${MUSL_DYNAMIC_LINKER_PATH}
            -Wl,-z,max-page-size=4096
            -Wl,-z,common-page-size=4096
            -Wl,--build-id=none
            -L${MUSL_SYSROOT}/lib
            -o ${NCURSES_SH_ELF}
            ${MUSL_CRT1_O}
            ${MUSL_CRTI_O}
            ${NCURSES_SH_OBJ}
            ${NCURSES_LIB_A}
            -lc
            ${LIBGCC_PATH}
            ${MUSL_CRTN_O}
        DEPENDS ${NCURSES_SH_OBJ} musl_sysroot local_ncurses_build
        COMMENT "Linking ncurses shell program _sh (dynamic musl)"
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
    cmake_parse_arguments(SYSROOT_INSTALL "" "" "DEPENDS" ${ARGN})
    get_filename_component(DEST_DIR ${DEST_PATH} DIRECTORY)
    add_custom_command(
        OUTPUT ${DEST_PATH}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${DEST_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SRC_PATH} ${DEST_PATH}
        DEPENDS ${SRC_PATH} ${SYSROOT_INSTALL_DEPENDS}
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
    syscalltest iovectest
)
foreach(PROG ${USERLIB_ONLY_PROGS})
    set(SRC "${CMAKE_BINARY_DIR}/user/_${PROG}")
    if(TARGET _${PROG})
        sysroot_install(${SRC} "${SYSROOT_BIN_DIR}/${PROG}" DEPENDS _${PROG})
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
    if(TARGET _${BUILD_NAME}_dymusl)
        sysroot_install(${SRC} "${SYSROOT_BIN_DIR}/${FRIENDLY_NAME}" DEPENDS _${BUILD_NAME}_dymusl)
    endif()
endforeach()

# ncurses shell (musl) — the primary "sh"
if(TARGET _sh_musl)
    sysroot_install("${CMAKE_BINARY_DIR}/user/_sh" "${SYSROOT_BIN_DIR}/sh" DEPENDS _sh_musl)
endif()

# msh — basic musl shell (keep alongside ncurses sh for reference)
if(TARGET _msh_musl)
    sysroot_install("${CMAKE_BINARY_DIR}/user/_msh" "${SYSROOT_BIN_DIR}/msh" DEPENDS _msh_musl)
endif()

# dyntest (dynamic linking test)
if(TARGET _dyntest_dymusl)
    sysroot_install("${CMAKE_BINARY_DIR}/user/_dyntest" "${SYSROOT_BIN_DIR}/dyntest" DEPENDS _dyntest_dymusl)
endif()

# dash shell (statically linked against musl)
if(TARGET dash_build)
    sysroot_install("${DASH_BIN}" "${SYSROOT_BIN_DIR}/dash" DEPENDS dash_build)
endif()

# CPython interpreter (dynamically linked against musl)
if(TARGET cpython_build)
    sysroot_install("${CPYTHON_BIN}" "${SYSROOT_BIN_DIR}/python3.12" DEPENDS cpython_build)
    # python → python3.12 symlink
    add_custom_command(
        OUTPUT "${SYSROOT_BIN_DIR}/python"
        COMMAND ${CMAKE_COMMAND} -E create_symlink python3.12 "${SYSROOT_BIN_DIR}/python"
        DEPENDS "${SYSROOT_BIN_DIR}/python3.12"
        COMMENT "sysroot: python → python3.12 symlink"
    )
    set(SYSROOT_TARGETS ${SYSROOT_TARGETS}
        "${SYSROOT_BIN_DIR}/python"
        ${CPYTHON_SHARED_LIB}
        ${CPYTHON_INSTALL_STAMP})
endif()

# OpenSSH binaries (statically linked against musl + OpenSSL)
if(TARGET openssh_build)
    foreach(PROG sshd ssh ssh-keygen ssh-keyscan scp sftp)
        sysroot_install("${OPENSSH_BUILD_DIR}/${PROG}" "${SYSROOT_BIN_DIR}/${PROG}" DEPENDS openssh_build)
    endforeach()
    # sshd-session goes to /libexec/ (OpenSSH 9.8+ listener/session split)
    set(SYSROOT_LIBEXEC_DIR "${MUSL_SYSROOT}/libexec")
    sysroot_install("${OPENSSH_BUILD_DIR}/sshd-session" "${SYSROOT_LIBEXEC_DIR}/sshd-session" DEPENDS openssh_build)
endif()

# OpenSSL CLI tool (optional — useful for key generation and diagnostics)
if(TARGET openssl_cli)
    sysroot_install("${OPENSSL_CLI_BIN}" "${SYSROOT_BIN_DIR}/openssl" DEPENDS openssl_cli)
endif()

# SQLite shell
if(TARGET sqlite_build)
    set(SYSROOT_TARGETS ${SYSROOT_TARGETS} "${SQLITE_BIN}" "${SQLITE_LIB_A}" "${SQLITE_HEADER}")
endif()

# Aggregate target
if(SYSROOT_TARGETS)
    # Write a stamp file whenever sysroot is (re)populated so that
    # downstream add_custom_command rules (fs.img) can depend on an
    # actual file rather than only a target name.
    set(SYSROOT_STAMP "${MUSL_SYSROOT}/.sysroot_stamp")
    set(SYSROOT_DEP_FILES ${SYSROOT_TARGETS})
    set(SYSROOT_STAMP "${SYSROOT_STAMP}" PARENT_SCOPE)
    set(SYSROOT_DEP_FILES ${SYSROOT_DEP_FILES} PARENT_SCOPE)
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
    if(TARGET sqlite_build)
        add_dependencies(sysroot_populate sqlite_build)
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
