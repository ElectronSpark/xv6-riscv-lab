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

set(KERNEL_BINARY kernel)
set(KERNEL_DECOMPILED kernel.asm)
set(KERNEL_SYMBOLS kernel.sym)

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
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -I${KERNEL_DIR}")

# check whether it needs to turn off stack protector
execute_process(
    COMMAND ${CMAKE_C_COMPILER} "-fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector"
    OUTPUT_VARIABLE CFLAG_NO_STACK_PROTECTOR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CFLAG_NO_STACK_PROTECTOR}")

# Separate .S and .c files for kernel source files
set(KERNEL_C_FILES
    ${KERNEL_DIR}/backtrace.c
    ${KERNEL_DIR}/file.c
    ${KERNEL_DIR}/net.c
    ${KERNEL_DIR}/printf.c
    ${KERNEL_DIR}/start.c
    ${KERNEL_DIR}/sysproc.c
    ${KERNEL_DIR}/bio.c        
    ${KERNEL_DIR}/fs.c      
    ${KERNEL_DIR}/page.c  
    ${KERNEL_DIR}/proc.c       
    ${KERNEL_DIR}/string.c   
    ${KERNEL_DIR}/trap.c
    ${KERNEL_DIR}/console.c    
    ${KERNEL_DIR}/kalloc.c  
    ${KERNEL_DIR}/pci.c   
    ${KERNEL_DIR}/slab.c       
    ${KERNEL_DIR}/syscall.c  
    ${KERNEL_DIR}/uart.c
    ${KERNEL_DIR}/e1000.c      
    ${KERNEL_DIR}/log.c     
    ${KERNEL_DIR}/pipe.c  
    ${KERNEL_DIR}/sleeplock.c  
    ${KERNEL_DIR}/sysfile.c  
    ${KERNEL_DIR}/virtio_disk.c
    ${KERNEL_DIR}/exec.c       
    ${KERNEL_DIR}/main.c    
    ${KERNEL_DIR}/plic.c  
    ${KERNEL_DIR}/spinlock.c   
    ${KERNEL_DIR}/sysnet.c   
    ${KERNEL_DIR}/vm.c
)

set(KERNEL_S_FILES
    ${KERNEL_DIR}/entry.S
    ${KERNEL_DIR}/swtch.S
    ${KERNEL_DIR}/trampoline.S
    ${KERNEL_DIR}/kernelvec.S
)

# Define kernel object files
FILE(GLOB KERNEL_OBJS
    ${KERNEL_S_FILES}
    ${KERNEL_C_FILES}
)

set_source_files_properties(${KERNEL_OBJS} PROPERTIES LANGUAGE C)

# Add kernel executable
add_executable(${KERNEL_BINARY} ${KERNEL_OBJS})

# Set compiler and linker flags for the kernel
target_compile_options(${KERNEL_BINARY} PRIVATE -nostdlib -ffreestanding -mcmodel=medany)
set_target_properties(${KERNEL_BINARY} PROPERTIES LINK_FLAGS
    "-T ${KERNEL_DIR}/kernel.ld -z max-page-size=4096")

# decompile the kernel binary
add_custom_target(${KERNEL_DECOMPILED}
    COMMAND ${CMAKE_OBJDUMP} -S ${KERNEL_BINARY} > ${KERNEL_DECOMPILED}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Decompiling kernel binary to ${KERNEL_DECOMPILED}"
    DEPENDS ${KERNEL_BINARY}
)

# extract symbols from the kernel binary
add_custom_target(${KERNEL_SYMBOLS}
    COMMAND ${CMAKE_OBJDUMP} -t ${KERNEL_BINARY} | sed '1,/SYMBOL TABLE/d\; s/ .* / /\; /^$$/d' > ${KERNEL_SYMBOLS}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Extracting symbols from kernel binary to ${KERNEL_SYMBOLS}"
    DEPENDS ${KERNEL_BINARY}
)

# Add user programs
file(GLOB USER_PROGRAMS ${USER_DIR}/*.c)
# Modify user program output to match the .c file name without a suffix
foreach(PROGRAM ${USER_PROGRAMS})
    get_filename_component(PROGRAM_NAME ${PROGRAM} NAME_WE)
    add_executable(${PROGRAM_NAME} ${PROGRAM})
    target_compile_options(${PROGRAM_NAME} PRIVATE -nostdlib -ffreestanding)
    target_link_options(${PROGRAM_NAME} PRIVATE -T ${USER_DIR}/user.ld -nostdlib -ffreestanding)
endforeach()

# Add a custom target for building the file system image
add_custom_target(fs.img ALL
    COMMAND ${CMAKE_COMMAND} -E echo "Building file system image..."
    COMMAND mkfs/mkfs fs.img README ${USER_PROGRAMS}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Creating file system image"
)
