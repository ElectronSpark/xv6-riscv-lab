cmake_minimum_required(VERSION 3.10)

# Prevent in-source builds
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Please create a 'build' directory and run 'cmake ..' from there.")
endif()

# ==============================================================================
# Architecture Configuration
# ==============================================================================
if(NOT DEFINED ARCH)
    if(DEFINED ENV{ARCH})
        set(ARCH "$ENV{ARCH}" CACHE STRING "Target architecture (riscv or x86_64)")
    else()
        set(ARCH "riscv" CACHE STRING "Target architecture (riscv or x86_64)")
    endif()
elseif(DEFINED ENV{ARCH} AND NOT "$ENV{ARCH}" STREQUAL "" AND NOT "$ENV{ARCH}" STREQUAL "${ARCH}")
    # Environment overrides cached value when explicitly set
    set(ARCH "$ENV{ARCH}" CACHE STRING "Target architecture (riscv or x86_64)" FORCE)
endif()

# Validate architecture
if(NOT ARCH MATCHES "^(riscv|x86_64)$")
    message(FATAL_ERROR "Invalid ARCH: ${ARCH}. Must be 'riscv' or 'x86_64'")
endif()

# ==============================================================================
# Platform Configuration
# ==============================================================================
# PLATFORM can be set via cmake -DPLATFORM=orangepi or environment variable
if(NOT DEFINED PLATFORM)
    if(DEFINED ENV{PLATFORM})
        set(PLATFORM "$ENV{PLATFORM}")
    else()
        set(PLATFORM "qemu")
    endif()
endif()

# Validate platform
if(NOT PLATFORM MATCHES "^(qemu|orangepi)$")
    message(FATAL_ERROR "Invalid PLATFORM: ${PLATFORM}. Must be 'qemu' or 'orangepi'")
endif()

# ==============================================================================
# OpenSBI Configuration
# ==============================================================================
# Options:
#   OPENSBI_MODE - Controls how OpenSBI is used:
#     - "default"  : Use QEMU's built-in OpenSBI (default)
#     - "none"     : Run without BIOS (original xv6 mode)
#     - "build"    : Build OpenSBI from source (fw_jump.elf firmware)
#     - "external" : Use an external OpenSBI binary (set OPENSBI_PATH)
#
#   OPENSBI_PATH - Path to external OpenSBI fw_jump.elf (when OPENSBI_MODE=external)
#
# Note: libsbi.a is for M-mode firmware development, not S-mode kernels.
# Your kernel already has SBI ecall wrappers in kernel/sbi.c that work with
# any SBI-compliant firmware (OpenSBI, etc).
# ==============================================================================

set(OPENSBI_MODE "default" CACHE STRING "OpenSBI mode: default, none, build, or external")
set_property(CACHE OPENSBI_MODE PROPERTY STRINGS "default" "none" "build" "external")

set(OPENSBI_PATH "" CACHE FILEPATH "Path to external OpenSBI fw_jump.elf binary")

# Validate OPENSBI_MODE
if(NOT OPENSBI_MODE MATCHES "^(default|none|build|external)$")
    message(FATAL_ERROR "Invalid OPENSBI_MODE: ${OPENSBI_MODE}. Must be one of: default, none, build, external")
endif()

# Validate external path if specified
if(OPENSBI_MODE STREQUAL "external")
    if(NOT OPENSBI_PATH OR NOT EXISTS "${OPENSBI_PATH}")
        message(FATAL_ERROR "OPENSBI_MODE is 'external' but OPENSBI_PATH is not set or file doesn't exist: ${OPENSBI_PATH}")
    endif()
endif()

# ==============================================================================
# LAB Configuration
# ==============================================================================
# Set default value for LAB environment variable
if(NOT DEFINED ENV{LAB} OR "$ENV{LAB}" STREQUAL "")
    set(ENV{LAB} "util")
endif()
set(LAB_NAME "$ENV{LAB}")

# ==============================================================================
# Display Build Configuration Banner
# ==============================================================================
message(STATUS "")
message(STATUS "========================================")
message(STATUS "  xv6-riscv build configuration")
message(STATUS "----------------------------------------")
message(STATUS "  Platform:     ${PLATFORM}")
message(STATUS "  OpenSBI:      ${OPENSBI_MODE}")
message(STATUS "  Lab:          ${LAB_NAME}")
message(STATUS "========================================")
message(STATUS "")

# Build OpenSBI from source if requested (after banner)
if(OPENSBI_MODE STREQUAL "build")
    add_subdirectory(opensbi)
    set(OPENSBI_PATH ${OPENSBI_FW_JUMP})
    message(STATUS "OpenSBI will be built from source: ${OPENSBI_PATH}")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Perl REQUIRED)

# For bare-metal cross-compilers (like riscv64-unknown-elf-gcc), skip the
# linker test during compiler verification. These compilers don't have
# standard C libraries (libc, crt0.o, etc.) so linking a test executable fails.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Set the toolchain BEFORE project() so CMake uses the correct compilers
# CMake detects compilers during project() - setting them afterward has no effect
set(USE_ENV_TOOLPREFIX OFF)
if(DEFINED ENV{TOOLPREFIX} AND NOT "$ENV{TOOLPREFIX}" STREQUAL "")
    if((ARCH STREQUAL "riscv" AND "$ENV{TOOLPREFIX}" MATCHES "riscv") OR
       (ARCH STREQUAL "x86_64" AND "$ENV{TOOLPREFIX}" MATCHES "x86_64"))
        set(USE_ENV_TOOLPREFIX ON)
    else()
        message(STATUS "TOOLPREFIX=$ENV{TOOLPREFIX} doesn't match ARCH=${ARCH}, using auto-discovery")
    endif()
endif()

if(USE_ENV_TOOLPREFIX)
    set(CMAKE_C_COMPILER $ENV{TOOLPREFIX}gcc CACHE STRING "C compiler" FORCE)
    set(CMAKE_ASM_COMPILER $ENV{TOOLPREFIX}gcc CACHE STRING "ASM compiler" FORCE)
    set(CMAKE_LINKER $ENV{TOOLPREFIX}ld CACHE FILEPATH "Linker" FORCE)
    set(CMAKE_OBJCOPY $ENV{TOOLPREFIX}objcopy)
    set(CMAKE_OBJDUMP $ENV{TOOLPREFIX}objdump)
    set(CMAKE_C_LINK_EXECUTABLE "$ENV{TOOLPREFIX}ld <LINK_FLAGS> -o <TARGET> <LINK_LIBRARIES> <OBJECTS>")
    # Detect if the user-provided TOOLPREFIX is the xv6 musl toolchain
    if("$ENV{TOOLPREFIX}" MATCHES "xv6-linux-musl-$")
        set(XV6_MUSL_TOOLCHAIN TRUE)
    else()
        set(XV6_MUSL_TOOLCHAIN FALSE)
    endif()
else()
    if(ARCH STREQUAL "riscv")
        # Search priority:
        #   1. xv6 custom musl toolchain (riscv64-xv6-linux-musl-gcc)
        #   2. Traditional bare-metal toolchain (riscv64-unknown-elf-gcc)
        message(STATUS "TOOLPREFIX not set, searching for RISC-V toolchain...")

        # Try the xv6 musl toolchain first (supports both static and shared linking)
        find_program(RISCV_GCC riscv64-xv6-linux-musl-gcc
            PATHS ${CMAKE_SOURCE_DIR}/toolchain/riscv64/phase2/bin
                  ${CMAKE_SOURCE_DIR}/toolchain/riscv64/phase1/bin
                  ${CMAKE_SOURCE_DIR}/toolchain/riscv64/bin
            NO_DEFAULT_PATH)
        if(NOT RISCV_GCC)
            find_program(RISCV_GCC riscv64-xv6-linux-musl-gcc)
        endif()

        # Fall back to bare-metal toolchain
        if(NOT RISCV_GCC)
            find_program(RISCV_GCC riscv64-unknown-elf-gcc PATHS /opt/riscv/bin PATH_SUFFIXES bin NO_DEFAULT_PATH)
        endif()
        if(NOT RISCV_GCC)
            find_program(RISCV_GCC riscv64-unknown-elf-gcc)
        endif()
        
        if(RISCV_GCC)
            get_filename_component(TOOLPREFIX ${RISCV_GCC} DIRECTORY)
            # Detect which toolchain was found and set the correct prefix
            if(RISCV_GCC MATCHES "riscv64-xv6-linux-musl-gcc$")
                set(TOOLPREFIX "${TOOLPREFIX}/riscv64-xv6-linux-musl-")
                set(XV6_MUSL_TOOLCHAIN TRUE)
            else()
                set(TOOLPREFIX "${TOOLPREFIX}/riscv64-unknown-elf-")
                set(XV6_MUSL_TOOLCHAIN FALSE)
            endif()
            set(CMAKE_C_COMPILER ${TOOLPREFIX}gcc CACHE STRING "C compiler" FORCE)
            set(CMAKE_ASM_COMPILER ${TOOLPREFIX}gcc CACHE STRING "ASM compiler" FORCE)
            set(CMAKE_LINKER ${TOOLPREFIX}ld CACHE FILEPATH "Linker" FORCE)
            set(CMAKE_OBJCOPY ${TOOLPREFIX}objcopy)
            set(CMAKE_OBJDUMP ${TOOLPREFIX}objdump)
            set(CMAKE_C_LINK_EXECUTABLE "${TOOLPREFIX}ld <LINK_FLAGS> -o <TARGET> <LINK_LIBRARIES> <OBJECTS>")
            message(STATUS "Found RISC-V toolchain: ${TOOLPREFIX}")
            if(XV6_MUSL_TOOLCHAIN)
                message(STATUS "  Using xv6 musl toolchain (supports static + dynamic linking)")
            endif()
        else()
            message(FATAL_ERROR "TOOLPREFIX environment variable not set and no RISC-V toolchain found.\n"
                "  Searched for: riscv64-xv6-linux-musl-gcc, riscv64-unknown-elf-gcc\n"
                "  Build the xv6 toolchain: scripts/build_gcc_toolchain.sh --arch=riscv64")
        endif()
    elseif(ARCH STREQUAL "x86_64")
        # Search priority:
        #   1. xv6 custom musl toolchain (x86_64-xv6-linux-musl-gcc)
        #   2. Bare-metal (x86_64-elf-gcc)
        #   3. Host linux-gnu toolchain (x86_64-linux-gnu-gcc)
        message(STATUS "Searching for x86_64 toolchain...")

        # Try the xv6 musl toolchain first
        find_program(X86_GCC x86_64-xv6-linux-musl-gcc
            PATHS ${CMAKE_SOURCE_DIR}/toolchain/x86_64/phase2/bin
                  ${CMAKE_SOURCE_DIR}/toolchain/x86_64/phase1/bin
                  ${CMAKE_SOURCE_DIR}/toolchain/x86_64/bin
            NO_DEFAULT_PATH)
        if(NOT X86_GCC)
            find_program(X86_GCC x86_64-xv6-linux-musl-gcc)
        endif()

        # Fall back to bare-metal / host toolchains
        if(NOT X86_GCC)
            find_program(X86_GCC x86_64-elf-gcc)
        endif()
        if(NOT X86_GCC)
            find_program(X86_GCC x86_64-linux-gnu-gcc)
        endif()
        
        if(X86_GCC)
            get_filename_component(TOOLPREFIX ${X86_GCC} DIRECTORY)
            if(X86_GCC MATCHES "x86_64-xv6-linux-musl-gcc$")
                set(TOOLPREFIX "${TOOLPREFIX}/x86_64-xv6-linux-musl-")
                set(XV6_MUSL_TOOLCHAIN TRUE)
            elseif(X86_GCC MATCHES "x86_64-linux-gnu-gcc$")
                set(TOOLPREFIX "${TOOLPREFIX}/x86_64-linux-gnu-")
                set(XV6_MUSL_TOOLCHAIN FALSE)
            else()
                set(TOOLPREFIX "${TOOLPREFIX}/x86_64-elf-")
                set(XV6_MUSL_TOOLCHAIN FALSE)
            endif()
            set(CMAKE_C_COMPILER ${TOOLPREFIX}gcc CACHE STRING "C compiler" FORCE)
            set(CMAKE_ASM_COMPILER ${TOOLPREFIX}gcc CACHE STRING "ASM compiler" FORCE)
            set(CMAKE_LINKER ${TOOLPREFIX}ld CACHE FILEPATH "Linker" FORCE)
            set(CMAKE_OBJCOPY ${TOOLPREFIX}objcopy)
            set(CMAKE_OBJDUMP ${TOOLPREFIX}objdump)
            set(CMAKE_C_LINK_EXECUTABLE "${TOOLPREFIX}ld <LINK_FLAGS> -o <TARGET> <LINK_LIBRARIES> <OBJECTS>")
            message(STATUS "Found x86_64 toolchain: ${TOOLPREFIX}")
            if(XV6_MUSL_TOOLCHAIN)
                message(STATUS "  Using xv6 musl toolchain (supports static + dynamic linking)")
            endif()
        else()
            message(FATAL_ERROR "No x86_64 toolchain found.\n"
                "  Searched for: x86_64-xv6-linux-musl-gcc, x86_64-elf-gcc, x86_64-linux-gnu-gcc\n"
                "  Build the xv6 toolchain: scripts/build_gcc_toolchain.sh --arch=x86_64")
        endif()
    endif()
endif()

# Project name
project(xv6-${ARCH} LANGUAGES C ASM)

if(ARCH STREQUAL "riscv")
    add_compile_definitions(CONFIG_ARCH_RISCV=1)
elseif(ARCH STREQUAL "x86_64")
    add_compile_definitions(CONFIG_ARCH_X86_64=1)
endif()

# Specify the C standard
# set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED OFF)

# Define kernel and user directories
set(KERNEL_DIR ${CMAKE_SOURCE_DIR}/kernel)
set(USER_DIR ${CMAKE_SOURCE_DIR}/user)

# ==============================================================================
# Global Compiler Flags
# ==============================================================================
string(CONCAT CMAKE_C_FLAGS "${CMAKE_C_FLAGS}"
    " -Wall -Werror -O0 -fno-omit-frame-pointer -ggdb -gdwarf-2"
    " -fno-common -nostdlib"
    # Disable all built-in function assumptions — we provide our own libc
    " -fno-builtin-strncpy -fno-builtin-strncmp -fno-builtin-strlen"
    " -fno-builtin-memset -fno-builtin-memmove -fno-builtin-memcmp"
    " -fno-builtin-memcpy -fno-builtin-log -fno-builtin-bzero"
    " -fno-builtin-strchr -fno-builtin-exit -fno-builtin-malloc"
    " -fno-builtin-putc -fno-builtin-free"
    " -fno-builtin-printf -fno-builtin-fprintf -fno-builtin-vprintf"
    " -fno-pie -no-pie"
    " -Wno-main"
    " -I${CMAKE_SOURCE_DIR}"
    " -I${KERNEL_DIR}/inc"
    " -DUSE_SOFTWARE_FFS=1"
)

if(ARCH STREQUAL "riscv")
    string(APPEND CMAKE_C_FLAGS " -mcmodel=medany -march=rv64imac_zicsr_zifencei -mabi=lp64")
elseif(ARCH STREQUAL "x86_64")
    string(APPEND CMAKE_C_FLAGS " -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-avx -mgeneral-regs-only")
endif()

# check whether it needs to turn off stack protector
execute_process(
    COMMAND bash -c "${CMAKE_C_COMPILER} -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector"
    OUTPUT_VARIABLE FNO_STACK_PROTECTOR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${FNO_STACK_PROTECTOR}")

if(NOT "$ENV{KMM_SANITIZE_CHECK}" STREQUAL "")
    message(STATUS "KMM_SANITIZE_CHECK is set to: $ENV{KMM_SANITIZE_CHECK}")
    list(APPEND XCFLAGS -DKERNEL_PAGE_SANITIZER=1)
endif()

# LAB configuration — uppercase the lab name for preprocessor defines
string(TOUPPER "${LAB_NAME}" LABUPPER)
list(APPEND XCFLAGS -DSOL_${LABUPPER} -DLAB_${LABUPPER})

string(REPLACE ";" " " XCFLAGS_STR "${XCFLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${XCFLAGS_STR}")

# ==============================================================================
# Compression Support (defined before subdirectories for propagation)
# ==============================================================================
# Enable compression for kernel and filesystem images.
# Both QEMU and U-Boot support gzip-compressed images.
# ==============================================================================
option(ENABLE_COMPRESSION "Compress kernel and filesystem images with gzip" ON)

if(ENABLE_COMPRESSION)
    find_program(GZIP gzip)
    if(NOT GZIP)
        message(WARNING "gzip not found - compression disabled")
        set(ENABLE_COMPRESSION OFF)
    else()
        message(STATUS "Compression enabled (gzip found)")
    endif()
endif()

add_subdirectory(kernel)
add_subdirectory(user)
if(ARCH STREQUAL "riscv")
    add_subdirectory(arch/riscv/boot)
endif()

# Add uboot-scripts as dependency of kernel_all for Orange Pi platform
if(MKIMAGE_FOUND AND PLATFORM STREQUAL "orangepi")
    add_dependencies(kernel_all uboot-scripts)
endif()

set(MKFS_SOURCE_FILES
    ${CMAKE_SOURCE_DIR}/mkfs/mkfs.c 
    ${KERNEL_DIR}/inc/vfs/xv6fs/ondisk.h 
    ${KERNEL_DIR}/inc/param.h
)

set(XV6FUSE_SOURCE_FILES
    ${CMAKE_SOURCE_DIR}/utils/xv6fuse.c
    ${CMAKE_SOURCE_DIR}/utils/rbtree.c
    ${CMAKE_SOURCE_DIR}/utils/rbtree.h
)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/mkfs
    COMMAND gcc ${CMAKE_SOURCE_DIR}/mkfs/mkfs.c -DSOL_${LABUPPER} -DLAB_${LABUPPER} -Werror -Wall -I${CMAKE_SOURCE_DIR} -o ${CMAKE_CURRENT_BINARY_DIR}/mkfs
    DEPENDS ${MKFS_SOURCE_FILES}
    COMMENT "Building mkfs..."
)

# Create a GPT-partitioned second disk image with ext4 + xv6fs partitions
set(MK_GPT_DISK_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/mk_gpt_disk.sh")
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xv6fs_test.img
    COMMAND bash ${MK_GPT_DISK_SCRIPT}
            ${CMAKE_CURRENT_BINARY_DIR}/xv6fs_test.img
            ${CMAKE_CURRENT_BINARY_DIR}/mkfs
            512
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/mkfs
            ${MK_GPT_DISK_SCRIPT}
    COMMENT "Creating 512MB GPT disk image (ext4 + xv6fs partitions)..."
)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xv6fuse
    COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/build_xv6fuse.sh
            ${CMAKE_CURRENT_BINARY_DIR}/xv6fuse
            ${CMAKE_SOURCE_DIR}/utils/xv6fuse.c
            ${CMAKE_SOURCE_DIR}/utils/rbtree.c
            ${CMAKE_SOURCE_DIR}/utils
    DEPENDS ${XV6FUSE_SOURCE_FILES}
            ${CMAKE_SOURCE_DIR}/scripts/build_xv6fuse.sh
    COMMENT "Building xv6fuse"
)

set(TERMINFO_DIR ${CMAKE_CURRENT_BINARY_DIR}/terminfo)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/terminfo.stamp
    COMMAND ${CMAKE_COMMAND} -E make_directory ${TERMINFO_DIR}
    COMMAND tic -x -o ${TERMINFO_DIR} -e xterm,vt100,dumb ${CMAKE_SOURCE_DIR}/user/ncurses/misc/terminfo.src
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/terminfo.stamp
    DEPENDS ${CMAKE_SOURCE_DIR}/user/ncurses/misc/terminfo.src
    COMMENT "Generating minimal terminfo database (xterm,vt100,dumb)"
)
add_custom_target(terminfo_db DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/terminfo.stamp)

# ==============================================================================
# Ext4 Root Filesystem Image
# ==============================================================================
# Build a unified ext4 image from the entire sysroot/ directory.
# All user programs, libraries, and third-party components (dash, CPython,
# ncurses, readline) are installed into sysroot/ by their build targets.
# ==============================================================================
set(SYSROOT_DIR "${CMAKE_BINARY_DIR}/sysroot")
set(CPYTHON_SRC_DIR_FOR_FS "${CMAKE_SOURCE_DIR}/user/v6-cpython")
set(MKEXT4_ROOTFS_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/mkext4_rootfs.sh")
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fs.img
    COMMAND bash ${MKEXT4_ROOTFS_SCRIPT}
            ${CMAKE_CURRENT_BINARY_DIR}/fs.img
            ${CMAKE_SOURCE_DIR}/README.md
            ${CPYTHON_SRC_DIR_FOR_FS}
            400
            ${SYSROOT_DIR}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS sysroot_populate
            ${SYSROOT_DEP_FILES}
            ${SYSROOT_STAMP}
            ${MKEXT4_ROOTFS_SCRIPT}
            ${CMAKE_SOURCE_DIR}/README.md
    COMMENT "Creating ext4 rootfs image from sysroot"
)

# Compressed filesystem image
if(ENABLE_COMPRESSION)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fs.img.gz
        COMMAND ${GZIP} -9 -k -f fs.img
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/fs.img
        COMMENT "Compressing fs.img -> fs.img.gz"
    )
    set(FS_IMG_COMPRESSED ${CMAKE_CURRENT_BINARY_DIR}/fs.img.gz)
endif()

add_custom_target(fs_img ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/fs.img
            ${CMAKE_CURRENT_BINARY_DIR}/xv6fs_test.img
            $<$<BOOL:${ENABLE_COMPRESSION}>:${FS_IMG_COMPRESSED}>
    COMMENT "Creating file system images"
)

# QEMU, GDB, and debug targets
include(${CMAKE_SOURCE_DIR}/arch/${ARCH}/cmake/qemu.cmake)

# Doxygen documentation
include(${CMAKE_SOURCE_DIR}/cmake/doc.cmake)