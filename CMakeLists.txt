cmake_minimum_required(VERSION 3.10)

# Project name
project(xv6-riscv LANGUAGES C ASM)

find_package(Perl REQUIRED)

# Set the RISC-V toolchain using a single base path environment variable
if(DEFINED ENV{TOOLPREFIX})
    set(CMAKE_C_COMPILER $ENV{TOOLPREFIX}gcc)
    set(CMAKE_ASM_COMPILER $ENV{TOOLPREFIX}as)
    set(CMAKE_LINKER $ENV{TOOLPREFIX}ld)
    set(CMAKE_OBJCOPY $ENV{TOOLPREFIX}objcopy)
    set(CMAKE_OBJDUMP $ENV{TOOLPREFIX}objdump)
else()
    message(FATAL_ERROR "Environment variable TOOLPREFIX is not set")
endif()

# Specify the C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Define kernel and user directories
set(KERNEL_DIR ${CMAKE_SOURCE_DIR}/kernel)
set(USER_DIR ${CMAKE_SOURCE_DIR}/user)

# Add global compiler options
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Werror -O0 -fno-omit-frame-pointer -ggdb -gdwarf-2")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -MD")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mcmodel=medany")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-common -nostdlib")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-builtin-strncpy -fno-builtin-strncmp -fno-builtin-strlen -fno-builtin-memset")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-builtin-memmove -fno-builtin-memcmp -fno-builtin-log -fno-builtin-bzero")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-builtin-strchr -fno-builtin-exit -fno-builtin-malloc -fno-builtin-putc")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-builtin-free")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-builtin-memcpy -Wno-main")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-builtin-printf -fno-builtin-fprintf -fno-builtin-vprintf")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -I${CMAKE_SOURCE_DIR}")

# check whether it needs to turn off stack protector
execute_process(
    COMMAND ${CMAKE_C_COMPILER} "-fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector"
    OUTPUT_VARIABLE CFLAG_NO_STACK_PROTECTOR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CFLAG_NO_STACK_PROTECTOR}")

add_subdirectory(kernel)
add_subdirectory(user)

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
    ${KERNEL_DIR}/fs.h 
    ${KERNEL_DIR}/param.h
)

#@TODO: add XCFLAGS
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/mkfs
    COMMAND gcc ${CMAKE_SOURCE_DIR}/mkfs/mkfs.c -Werror -Wall -I${CMAKE_SOURCE_DIR} -o ${CMAKE_CURRENT_BINARY_DIR}/mkfs
    DEPENDS ${MKFS_SOURCE_FILES}
    COMMENT "Building mkfs..."
)

# Update USER_PROGRAMS to include the correct path
# list(TRANSFORM USER_PROGRAMS PREPEND "user/")

# Update the custom command for fs.img to include the correct path for user executables
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fs.img
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/mkfs fs.img ${USER_PROGRAMS}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/user
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/mkfs ${CMAKE_SOURCE_DIR}/README ${USER_PROGRAMS}
    COMMENT "Creating file system image with user programs and README"
)

add_custom_target(fs.img ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/fs.img
    COMMENT "Creating file system image"
)


# @TODO: run etags on kernel and _init