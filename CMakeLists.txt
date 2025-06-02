cmake_minimum_required(VERSION 3.10)

# Project name
project(xv6-riscv LANGUAGES C ASM)

find_package(Perl REQUIRED)

# Set the RISC-V toolchain using a single base path environment variable
if(DEFINED ENV{TOOLPREFIX})
    set(CMAKE_C_COMPILER $ENV{TOOLPREFIX}gcc CACHE STRING "C compiler" FORCE)
    # set(CMAKE_ASM_COMPILER $ENV{TOOLPREFIX}as)
    set(CMAKE_LINKER $ENV{TOOLPREFIX}ld CACHE FILEPATH "Linker" FORCE)
    set(CMAKE_OBJCOPY $ENV{TOOLPREFIX}objcopy)
    set(CMAKE_OBJDUMP $ENV{TOOLPREFIX}objdump)
    # Use ld directly for linking executables
    set(CMAKE_C_LINK_EXECUTABLE "$ENV{TOOLPREFIX}ld <LINK_FLAGS> -o <TARGET> <LINK_LIBRARIES> <OBJECTS>")
else()
    message(FATAL_ERROR "Environment variable TOOLPREFIX is not set")
endif()

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

# check whether it needs to turn off stack protector
execute_process(
    COMMAND bash -c "${CMAKE_C_COMPILER} -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector"
    OUTPUT_VARIABLE FNO_STACK_PROTECTOR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${FNO_STACK_PROTECTOR}")

if(NOT $ENV{LAB} STREQUAL "")
    execute_process(
        COMMAND bash -c "echo $ENV{LAB} | tr a-z A-Z"
        OUTPUT_VARIABLE LABUPPER
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    set(XCFLAGS "${XCFLAGS} -DSOL_${LABUPPER} -DLAB_${LABUPPER}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${XCFLAGS}")
    message("In lab: ${LABUPPER}")
endif()

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
list(TRANSFORM USER_PROGRAMS PREPEND "${CMAKE_CURRENT_BINARY_DIR}/user/")

# Update the custom command for fs.img to include the correct path for user executables
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fs.img
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/mkfs fs.img ${USER_PROGRAMS}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/mkfs ${CMAKE_SOURCE_DIR}/README ${USER_PROGRAMS}
    COMMENT "Creating file system image with user programs and README"
)

add_custom_target(fs_img ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/fs.img
    COMMENT "Creating file system image"
)


# @TODO: run etags on kernel and _init
# Find QEMU
find_program(QEMU_EXECUTABLE NAMES qemu-system-riscv64 qemu-system-riscv32 qemu-system-riscv)
if(NOT QEMU_EXECUTABLE)
    message(FATAL_ERROR "QEMU executable not found. Please install QEMU or set the QEMU environment variable.")
endif()
set(QEMU ${QEMU_EXECUTABLE})

# Generate a unique GDB port
execute_process(
    COMMAND bash -c "expr \$(id -u) % 5000 + 25000"
    OUTPUT_VARIABLE GDBPORT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Generate QEMU GDB stub options
# Check if QEMU supports the '-gdb' option
execute_process(
    COMMAND ${QEMU} -help
    OUTPUT_VARIABLE QEMU_HELP
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(QEMU_HELP MATCHES "-gdb")
    set(QEMUGDB -gdb tcp::${GDBPORT})
else()
    set(QEMUGDB -s -p ${GDBPORT})
endif()

# Set default CPUS
if(NOT DEFINED ENV{CPUS} OR "$ENV{CPUS}" STREQUAL "")
    set(CPUS 3)
else()
    set(CPUS $ENV{CPUS})
endif()

# If LAB is fs, set CPUS to 1
if(DEFINED ENV{LAB} AND "$ENV{LAB}" STREQUAL "fs")
    set(CPUS 1)
endif()

# Generate unique FWDPORT1 and FWDPORT2 using CMake math and user id
execute_process(
    COMMAND id -u
    OUTPUT_VARIABLE USER_ID
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
math(EXPR FWDPORT1 "${USER_ID} % 5000 + 25999")
math(EXPR FWDPORT2 "${USER_ID} % 5000 + 30999")

# Compose QEMU options
set(QEMUOPTS
    -machine virt
    -bios none
    -kernel ${CMAKE_CURRENT_BINARY_DIR}/kernel/kernel
    -m 128M
    -smp ${CPUS}
    -nographic
    -global virtio-mmio.force-legacy=false
    -drive file=fs.img,if=none,format=raw,id=x0
    -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
    -netdev user,id=net0,hostfwd=udp::${FWDPORT1}-:2000,hostfwd=udp::${FWDPORT2}-:2001
    -object filter-dump,id=net0,netdev=net0,file=packets.pcap
    -device e1000,netdev=net0,bus=pcie.0
)

# Add custom target to run QEMU
add_custom_target(qemu
    COMMAND ${QEMU} ${QEMUOPTS}
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/kernel/kernel ${CMAKE_CURRENT_BINARY_DIR}/fs.img
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running QEMU with kernel and fs.img"
)

# Generate .gdbinit from template
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/.gdbinit
    COMMAND sed "s/:1234/:${GDBPORT}/" < ${CMAKE_SOURCE_DIR}/.gdbinit.tmpl-riscv > ${CMAKE_CURRENT_BINARY_DIR}/.gdbinit
    DEPENDS ${CMAKE_SOURCE_DIR}/.gdbinit.tmpl-riscv
    COMMENT "Generating .gdbinit with unique GDB port"
)

add_custom_target(gdbinit ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/.gdbinit
)

# Add custom target to run QEMU with GDB stub
add_custom_target(qemu-gdb
    COMMAND ${CMAKE_COMMAND} -E echo "*** Now run 'gdb' in another window."
    COMMAND ${QEMU} ${QEMUOPTS} -S ${QEMUGDB}
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/kernel/kernel ${CMAKE_CURRENT_BINARY_DIR}/.gdbinit ${CMAKE_CURRENT_BINARY_DIR}/fs.img
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running QEMU with GDB stub"
)

# If LAB is net, generate SERVERPORT
if(DEFINED ENV{LAB} AND "$ENV{LAB}" STREQUAL "net")
    execute_process(
        COMMAND bash -c "expr \$(id -u) % 5000 + 25099"
        OUTPUT_VARIABLE SERVERPORT
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()