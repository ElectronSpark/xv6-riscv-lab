cmake_minimum_required(VERSION 3.10)

# Prevent in-source builds
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Please create a 'build' directory and run 'cmake ..' from there.")
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

# Set the RISC-V toolchain BEFORE project() so CMake uses the correct compilers
# CMake detects compilers during project() - setting them afterward has no effect
if(DEFINED ENV{TOOLPREFIX})
    set(CMAKE_C_COMPILER $ENV{TOOLPREFIX}gcc CACHE STRING "C compiler" FORCE)
    set(CMAKE_ASM_COMPILER $ENV{TOOLPREFIX}gcc CACHE STRING "ASM compiler" FORCE)
    set(CMAKE_LINKER $ENV{TOOLPREFIX}ld CACHE FILEPATH "Linker" FORCE)
    set(CMAKE_OBJCOPY $ENV{TOOLPREFIX}objcopy)
    set(CMAKE_OBJDUMP $ENV{TOOLPREFIX}objdump)
    set(CMAKE_C_LINK_EXECUTABLE "$ENV{TOOLPREFIX}ld <LINK_FLAGS> -o <TARGET> <LINK_LIBRARIES> <OBJECTS>")
else()
    # Try to find riscv64-unknown-elf tools automatically
    message(STATUS "TOOLPREFIX environment variable not set, searching for riscv64-unknown-elf tools...")
    find_program(RISCV_GCC riscv64-unknown-elf-gcc PATHS /opt/riscv/bin PATH_SUFFIXES bin NO_DEFAULT_PATH)
    if(NOT RISCV_GCC)
        find_program(RISCV_GCC riscv64-unknown-elf-gcc)
    endif()
    
    if(RISCV_GCC)
        get_filename_component(TOOLPREFIX ${RISCV_GCC} DIRECTORY)
        set(TOOLPREFIX "${TOOLPREFIX}/riscv64-unknown-elf-")
        set(CMAKE_C_COMPILER ${TOOLPREFIX}gcc CACHE STRING "C compiler" FORCE)
        set(CMAKE_ASM_COMPILER ${TOOLPREFIX}gcc CACHE STRING "ASM compiler" FORCE)
        set(CMAKE_LINKER ${TOOLPREFIX}ld CACHE FILEPATH "Linker" FORCE)
        set(CMAKE_OBJCOPY ${TOOLPREFIX}objcopy)
        set(CMAKE_OBJDUMP ${TOOLPREFIX}objdump)
        set(CMAKE_C_LINK_EXECUTABLE "${TOOLPREFIX}ld <LINK_FLAGS> -o <TARGET> <LINK_LIBRARIES> <OBJECTS>")
        message(STATUS "Found RISC-V toolchain: ${TOOLPREFIX}")
    else()
        message(FATAL_ERROR "TOOLPREFIX environment variable not set and riscv64-unknown-elf tools not found in PATH or /opt/riscv/bin")
    endif()
endif()

# Project name
project(xv6-riscv LANGUAGES C ASM)

# Specify the C standard
# set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED OFF)

# Define kernel and user directories
set(KERNEL_DIR ${CMAKE_SOURCE_DIR}/kernel)
set(USER_DIR ${CMAKE_SOURCE_DIR}/user)

# Add global compiler options
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Werror -O0 -fno-omit-frame-pointer -ggdb -gdwarf-2")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mcmodel=medany")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-common -nostdlib")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-builtin-strncpy -fno-builtin-strncmp -fno-builtin-strlen -fno-builtin-memset")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-builtin-memmove -fno-builtin-memcmp -fno-builtin-log -fno-builtin-bzero")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-builtin-strchr -fno-builtin-exit -fno-builtin-malloc -fno-builtin-putc")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-builtin-free")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-builtin-memcpy -Wno-main")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-builtin-printf -fno-builtin-fprintf -fno-builtin-vprintf")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-pie -no-pie")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -I${CMAKE_SOURCE_DIR}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -I${KERNEL_DIR}/inc")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DUSE_SOFTWARE_FFS=1")

# check whether it needs to turn off stack protector
execute_process(
    COMMAND bash -c "${CMAKE_C_COMPILER} -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector"
    OUTPUT_VARIABLE FNO_STACK_PROTECTOR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${FNO_STACK_PROTECTOR}")

if(NOT $ENV{KMM_SANITIZE_CHECK} STREQUAL "")
    message(STATUS "KMM_SANITIZE_CHECK is set to: $ENV{KMM_SANITIZE_CHECK}")
    set(XCFLAGS "${XCFLAGS} -DKERNEL_PAGE_SANITIZER=1")
endif()


if(NOT $ENV{LAB} STREQUAL "")
    execute_process(
        COMMAND bash -c "echo $ENV{LAB} | tr a-z A-Z"
        OUTPUT_VARIABLE LABUPPER
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    set(XCFLAGS "${XCFLAGS} -DSOL_${LABUPPER} -DLAB_${LABUPPER}")
else()
    message(FATAL_ERROR "Environment variable LAB is not set")
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${XCFLAGS}")

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
add_subdirectory(boot)

# Add uboot-scripts as dependency of kernel_all for Orange Pi platform
if(MKIMAGE_FOUND AND PLATFORM STREQUAL "orangepi")
    add_dependencies(kernel_all uboot-scripts)
endif()

# @TODO:
# set(UEXTRA_FILES "")
# if($ENV{LAB} AND $ENV{LAB} STREQUAL "util")
#     set(UEXTRA_FILES 
#         ${USER_DIR}/xargstest.sh
#     )
# endif()
# FILE(UEXTRA ${UEXTRA_FILES})

set(MKFS_SOURCE_FILES
    ${CMAKE_SOURCE_DIR}/mkfs/mkfs.c 
    ${KERNEL_DIR}/inc/vfs/xv6fs/ondisk.h 
    ${KERNEL_DIR}/inc/param.h
)

#@TODO: add XCFLAGS
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/mkfs
    COMMAND gcc ${CMAKE_SOURCE_DIR}/mkfs/mkfs.c -DSOL_${LABUPPER} -DLAB_${LABUPPER} -Werror -Wall -I${CMAKE_SOURCE_DIR} -o ${CMAKE_CURRENT_BINARY_DIR}/mkfs
    DEPENDS ${MKFS_SOURCE_FILES}
    COMMENT "Building mkfs..."
)

# Update USER_PROGRAMS to include the correct path
list(TRANSFORM USER_PROGRAMS PREPEND "${CMAKE_CURRENT_BINARY_DIR}/user/")

# Update the custom command for fs.img to include the correct path for user executables
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fs.img
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/mkfs fs.img ${USER_PROGRAMS} ${CMAKE_SOURCE_DIR}/README.md
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/mkfs ${CMAKE_SOURCE_DIR}/README.md ${USER_PROGRAMS}
    COMMENT "Creating file system image with user programs and README"
)

# Create a second empty xv6fs disk image for the second virtio device
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fs0.img
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/mkfs fs0.img
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/mkfs
    COMMENT "Creating empty xv6fs disk image fs0.img"
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
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/fs.img ${CMAKE_CURRENT_BINARY_DIR}/fs0.img
            $<$<BOOL:${ENABLE_COMPRESSION}>:${FS_IMG_COMPRESSED}>
    COMMENT "Creating file system images"
)


# @TODO: run etags on kernel and _init

# QEMU, GDB, and debug targets
include(${CMAKE_SOURCE_DIR}/cmake/qemu.cmake)

# Doxygen documentation
include(${CMAKE_SOURCE_DIR}/cmake/doc.cmake)