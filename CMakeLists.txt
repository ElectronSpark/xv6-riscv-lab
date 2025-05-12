cmake_minimum_required(VERSION 3.10)

# Project name
project(xv6-riscv LANGUAGES C ASM)

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

# Add a custom target for building the file system image
add_custom_target(fs.img ALL
    COMMAND ${CMAKE_COMMAND} -E echo "Building file system image..."
    COMMAND mkfs/mkfs fs.img README ${USER_PROGRAMS}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Creating file system image"
)


# @TODO: run etags on kernel and _init