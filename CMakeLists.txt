# xv6-os kernel sub-repo — standalone CMake project.
#
# Lifted from the original xv6 monolithic top-level CMakeLists.txt, with
# user/, mkfs, ext4 fs.img, qemu, and doxygen sections removed (those
# belong in the umbrella, not the kernel sub-repo).
#
# This file owns:
#   * toolchain detection (TOOLPREFIX or auto-discovery)
#   * ARCH / PLATFORM / OPT_LEVEL / LAB cache vars
#   * shared CMAKE_C_FLAGS for kernel + bootloader
#   * dispatch into kernel/ and arch/${ARCH}/boot/
#
# Standalone usage:
#   cmake -S . -B build -DARCH=riscv -DPLATFORM=qemu -DOPT_LEVEL=0
#   cmake --build build -j --target kernel
#
# Umbrella usage: invoked by xv6-os/cmake/BuildKernel.cmake, which sets
# CMAKE_C_COMPILER from the cross toolchain and forwards ARCH/PLATFORM.

cmake_minimum_required(VERSION 3.10)

# Prevent in-source builds.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not allowed. Use a separate build dir.")
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
    set(ARCH "$ENV{ARCH}" CACHE STRING "Target architecture (riscv or x86_64)" FORCE)
endif()

if(NOT ARCH MATCHES "^(riscv|x86_64)$")
    message(FATAL_ERROR "Invalid ARCH: ${ARCH}. Must be 'riscv' or 'x86_64'")
endif()

# ==============================================================================
# Platform Configuration
# ==============================================================================
if(NOT DEFINED PLATFORM)
    if(DEFINED ENV{PLATFORM})
        set(PLATFORM "$ENV{PLATFORM}")
    else()
        set(PLATFORM "qemu")
    endif()
endif()

if(NOT PLATFORM MATCHES "^(qemu|orangepi)$")
    message(FATAL_ERROR "Invalid PLATFORM: ${PLATFORM}. Must be 'qemu' or 'orangepi'")
endif()

# ==============================================================================
# OpenSBI configuration (kernel needs to know the path; firmware itself
# is built by the umbrella, NOT this sub-repo).
# ==============================================================================
set(OPENSBI_MODE "default" CACHE STRING "OpenSBI mode: default, none, build, or external")
set_property(CACHE OPENSBI_MODE PROPERTY STRINGS "default" "none" "build" "external")
set(OPENSBI_PATH "" CACHE FILEPATH "Path to external OpenSBI fw_jump.elf")
if(NOT OPENSBI_MODE MATCHES "^(default|none|build|external)$")
    message(FATAL_ERROR "Invalid OPENSBI_MODE: ${OPENSBI_MODE}")
endif()
if(OPENSBI_MODE STREQUAL "build")
    message(FATAL_ERROR
        "OPENSBI_MODE=build is not supported in the standalone kernel "
        "sub-repo. The umbrella builds opensbi separately and passes "
        "OPENSBI_MODE=external + OPENSBI_PATH.")
endif()
if(OPENSBI_MODE STREQUAL "external")
    if(NOT OPENSBI_PATH OR NOT EXISTS "${OPENSBI_PATH}")
        message(FATAL_ERROR "OPENSBI_MODE=external but OPENSBI_PATH missing or invalid: ${OPENSBI_PATH}")
    endif()
endif()

# ==============================================================================
# LAB / OPT_LEVEL
# ==============================================================================
if(NOT DEFINED ENV{LAB} OR "$ENV{LAB}" STREQUAL "")
    set(ENV{LAB} "util")
endif()
set(LAB_NAME "$ENV{LAB}")

if(NOT DEFINED OPT_LEVEL)
    if(DEFINED ENV{OPT})
        set(OPT_LEVEL "$ENV{OPT}" CACHE STRING "Compiler optimization level")
    else()
        set(OPT_LEVEL "0" CACHE STRING "Compiler optimization level")
    endif()
endif()
if(NOT OPT_LEVEL MATCHES "^(0|1|2|3|s|g|fast)$")
    message(FATAL_ERROR "Invalid OPT_LEVEL: ${OPT_LEVEL}")
endif()

message(STATUS "")
message(STATUS "========================================")
message(STATUS "  xv6 kernel build configuration")
message(STATUS "----------------------------------------")
message(STATUS "  ARCH:         ${ARCH}")
message(STATUS "  Platform:     ${PLATFORM}")
message(STATUS "  OpenSBI:      ${OPENSBI_MODE}")
message(STATUS "  Optimization: -O${OPT_LEVEL}")
message(STATUS "  Lab:          ${LAB_NAME}")
message(STATUS "========================================")
message(STATUS "")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Perl REQUIRED)

# Bare-metal toolchains lack libc/crt0 — skip linker test.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# ==============================================================================
# Toolchain detection — must happen BEFORE project()
# ==============================================================================
set(USE_ENV_TOOLPREFIX OFF)
if(DEFINED ENV{TOOLPREFIX} AND NOT "$ENV{TOOLPREFIX}" STREQUAL "")
    if((ARCH STREQUAL "riscv"  AND "$ENV{TOOLPREFIX}" MATCHES "riscv") OR
       (ARCH STREQUAL "x86_64" AND "$ENV{TOOLPREFIX}" MATCHES "x86_64"))
        set(USE_ENV_TOOLPREFIX ON)
    else()
        message(STATUS "TOOLPREFIX=$ENV{TOOLPREFIX} doesn't match ARCH=${ARCH}, auto-discovering")
    endif()
endif()

if(USE_ENV_TOOLPREFIX)
    set(CMAKE_C_COMPILER   $ENV{TOOLPREFIX}gcc CACHE STRING "C compiler" FORCE)
    set(CMAKE_ASM_COMPILER $ENV{TOOLPREFIX}gcc CACHE STRING "ASM compiler" FORCE)
    set(CMAKE_LINKER       $ENV{TOOLPREFIX}ld  CACHE FILEPATH "Linker" FORCE)
    set(CMAKE_OBJCOPY      $ENV{TOOLPREFIX}objcopy)
    set(CMAKE_OBJDUMP      $ENV{TOOLPREFIX}objdump)
    set(CMAKE_C_LINK_EXECUTABLE
        "$ENV{TOOLPREFIX}ld <LINK_FLAGS> -o <TARGET> <LINK_LIBRARIES> <OBJECTS>")
    if("$ENV{TOOLPREFIX}" MATCHES "xv6-linux-musl-$")
        set(XV6_MUSL_TOOLCHAIN TRUE)
    else()
        set(XV6_MUSL_TOOLCHAIN FALSE)
    endif()
else()
    if(ARCH STREQUAL "riscv")
        message(STATUS "TOOLPREFIX not set, searching for RISC-V toolchain...")
        find_program(RISCV_GCC riscv64-xv6-linux-musl-gcc)
        if(NOT RISCV_GCC)
            find_program(RISCV_GCC riscv64-unknown-elf-gcc PATHS /opt/riscv/bin PATH_SUFFIXES bin NO_DEFAULT_PATH)
        endif()
        if(NOT RISCV_GCC)
            find_program(RISCV_GCC riscv64-unknown-elf-gcc)
        endif()
        if(RISCV_GCC)
            get_filename_component(TOOLPREFIX ${RISCV_GCC} DIRECTORY)
            if(RISCV_GCC MATCHES "riscv64-xv6-linux-musl-gcc$")
                set(TOOLPREFIX "${TOOLPREFIX}/riscv64-xv6-linux-musl-")
                set(XV6_MUSL_TOOLCHAIN TRUE)
            else()
                set(TOOLPREFIX "${TOOLPREFIX}/riscv64-unknown-elf-")
                set(XV6_MUSL_TOOLCHAIN FALSE)
            endif()
            set(CMAKE_C_COMPILER   ${TOOLPREFIX}gcc CACHE STRING "C compiler" FORCE)
            set(CMAKE_ASM_COMPILER ${TOOLPREFIX}gcc CACHE STRING "ASM compiler" FORCE)
            set(CMAKE_LINKER       ${TOOLPREFIX}ld  CACHE FILEPATH "Linker" FORCE)
            set(CMAKE_OBJCOPY      ${TOOLPREFIX}objcopy)
            set(CMAKE_OBJDUMP      ${TOOLPREFIX}objdump)
            set(CMAKE_C_LINK_EXECUTABLE
                "${TOOLPREFIX}ld <LINK_FLAGS> -o <TARGET> <LINK_LIBRARIES> <OBJECTS>")
            message(STATUS "Found RISC-V toolchain: ${TOOLPREFIX}")
        else()
            message(FATAL_ERROR
                "TOOLPREFIX not set and no RISC-V toolchain found.\n"
                "  Searched: riscv64-xv6-linux-musl-gcc, riscv64-unknown-elf-gcc")
        endif()
    elseif(ARCH STREQUAL "x86_64")
        message(STATUS "Searching for x86_64 toolchain...")
        find_program(X86_GCC x86_64-xv6-linux-musl-gcc)
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
            set(CMAKE_C_COMPILER   ${TOOLPREFIX}gcc CACHE STRING "C compiler" FORCE)
            set(CMAKE_ASM_COMPILER ${TOOLPREFIX}gcc CACHE STRING "ASM compiler" FORCE)
            set(CMAKE_LINKER       ${TOOLPREFIX}ld  CACHE FILEPATH "Linker" FORCE)
            set(CMAKE_OBJCOPY      ${TOOLPREFIX}objcopy)
            set(CMAKE_OBJDUMP      ${TOOLPREFIX}objdump)
            set(CMAKE_C_LINK_EXECUTABLE
                "${TOOLPREFIX}ld <LINK_FLAGS> -o <TARGET> <LINK_LIBRARIES> <OBJECTS>")
            message(STATUS "Found x86_64 toolchain: ${TOOLPREFIX}")
        else()
            message(FATAL_ERROR
                "No x86_64 toolchain found.\n"
                "  Searched: x86_64-xv6-linux-musl-gcc, x86_64-elf-gcc, x86_64-linux-gnu-gcc")
        endif()
    endif()
endif()

project(xv6-${ARCH}-kernel LANGUAGES C ASM)

if(ARCH STREQUAL "riscv")
    add_compile_definitions(CONFIG_ARCH_RISCV=1)
elseif(ARCH STREQUAL "x86_64")
    add_compile_definitions(CONFIG_ARCH_X86_64=1)
endif()

set(CMAKE_C_STANDARD_REQUIRED OFF)

# Source-tree paths used by the kernel sub-CMakeLists.
set(KERNEL_DIR ${CMAKE_SOURCE_DIR}/kernel)
# USER_DIR is referenced by some kernel headers / scripts. In the new
# layout there is no user dir under kernel/, so point to a non-existent
# placeholder. Sub-CMakeLists must guard usage with EXISTS().
set(USER_DIR ${CMAKE_SOURCE_DIR}/__no_user_dir__)

# ==============================================================================
# Global Compiler Flags (verbatim from monolithic top CMakeLists)
# ==============================================================================
string(CONCAT CMAKE_C_FLAGS "${CMAKE_C_FLAGS}"
    " -Wall -Werror -O${OPT_LEVEL} -fno-omit-frame-pointer -ggdb -gdwarf-2"
    " -fno-common -nostdlib"
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
    " -mcmodel=large"
)

if(ARCH STREQUAL "riscv")
    string(APPEND CMAKE_C_FLAGS " -march=rv64imac_zicsr_zifencei -mabi=lp64")
elseif(ARCH STREQUAL "x86_64")
    string(APPEND CMAKE_C_FLAGS " -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-avx -mgeneral-regs-only")
endif()

execute_process(
    COMMAND bash -c "${CMAKE_C_COMPILER} -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector"
    OUTPUT_VARIABLE FNO_STACK_PROTECTOR
    OUTPUT_STRIP_TRAILING_WHITESPACE)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${FNO_STACK_PROTECTOR}")

if(NOT "$ENV{KMM_SANITIZE_CHECK}" STREQUAL "")
    list(APPEND XCFLAGS -DKERNEL_PAGE_SANITIZER=1)
endif()

string(TOUPPER "${LAB_NAME}" LABUPPER)
list(APPEND XCFLAGS -DSOL_${LABUPPER} -DLAB_${LABUPPER})
string(REPLACE ";" " " XCFLAGS_STR "${XCFLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${XCFLAGS_STR}")

# ==============================================================================
# Compression (kernel image only — fs.img is umbrella's concern)
# ==============================================================================
if(PLATFORM STREQUAL "orangepi")
    set(ENABLE_COMPRESSION ON CACHE BOOL "Compress kernel image with gzip" FORCE)
else()
    option(ENABLE_COMPRESSION "Compress kernel image with gzip" OFF)
endif()
if(ENABLE_COMPRESSION)
    find_program(GZIP gzip)
    if(NOT GZIP)
        message(WARNING "gzip not found - compression disabled")
        set(ENABLE_COMPRESSION OFF)
    endif()
endif()

# ==============================================================================
# Kernel + bootloader subprojects
# ==============================================================================
add_subdirectory(kernel)
if(ARCH STREQUAL "riscv")
    add_subdirectory(arch/riscv/boot)
endif()

set(XV6_SPARSE_JOBS "4" CACHE STRING "Parallel sparse jobs for kernel-sparse")
find_program(SPARSE_EXECUTABLE sparse)
if(SPARSE_EXECUTABLE)
    set(_sparse_command ${SPARSE_EXECUTABLE})
else()
    set(_sparse_command sparse)
endif()

add_custom_target(kernel-sparse
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/run_sparse.py
            --compile-commands ${CMAKE_BINARY_DIR}/compile_commands.json
            --sparse ${_sparse_command}
            --jobs ${XV6_SPARSE_JOBS}
    DEPENDS generate_asm_offsets
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running sparse over kernel sources"
    VERBATIM
)
