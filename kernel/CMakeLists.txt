set(KERNEL_BINARY kernel)
set(KERNEL_DECOMPILED kernel.asm)
set(KERNEL_SYMBOLS kernel.sym)

if(NOT DEFINED ENV{LAB})
    message(FATAL_ERROR "Environment variable LAB is not set")
else()
    message(STATUS "Building kernel for lab: $ENV{LAB}")
endif()

# Core kernel sources in this directory (excluding subdirectories).
set(KERNEL_C_FILES
    ${KERNEL_DIR}/start_kernel.c
    ${KERNEL_DIR}/string.c
    ${KERNEL_DIR}/bio.c
    ${KERNEL_DIR}/pipe.c
    ${KERNEL_DIR}/exec.c
    ${KERNEL_DIR}/virtio_disk.c
    ${KERNEL_DIR}/backtrace.c
    ${KERNEL_DIR}/e1000.c
    ${KERNEL_DIR}/net.c
    ${KERNEL_DIR}/pci.c
    ${KERNEL_DIR}/sysnet.c
    ${KERNEL_DIR}/hlist.c
    ${KERNEL_DIR}/bintree.c
    ${KERNEL_DIR}/rbtree.c
    ${KERNEL_DIR}/kobject.c
    ${KERNEL_DIR}/bits.c
    ${KERNEL_DIR}/sbi.c
)

if(DEFINED ENV{RWLOCK_TEST})
    set(KERNEL_C_FILES
        ${KERNEL_C_FILES}
        ${KERNEL_DIR}/lock/rwlock_test.c
    )
endif()

if(DEFINED ENV{SEMAPHORE_TEST})
    set(KERNEL_C_FILES
        ${KERNEL_C_FILES}
        ${KERNEL_DIR}/lock/semaphore_test.c
    )
endif()

set(KERNEL_SCAN_OBJS
    ${KERNEL_DIR}/start.c
    ${KERNEL_DIR}/console.c
    ${KERNEL_DIR}/printf.c
    ${KERNEL_DIR}/uart.c
)

if(DEFINED ENV{KSCAN})
    set(KERNEL_SCAN_OBJS
        ${KERNEL_SCAN_OBJS}
        ${KERNEL_DIR}/kscan.c
    )
endif()

if($ENV{LAB} STREQUAL "lock")
    set(KERNEL_C_FILES
        ${KERNEL_C_FILES}
        ${KERNEL_DIR}/stats.c
        ${KERNEL_DIR}/sprintf.c
    )
endif()

if($ENV{LAB} STREQUAL "net")
    set(KERNEL_C_FILES
        ${KERNEL_C_FILES}
        ${KERNEL_DIR}/e1000.c
        ${KERNEL_DIR}/net.c
        ${KERNEL_DIR}/pci.c
    )
endif()

set(KERNEL_S_FILES
    ${KERNEL_DIR}/entry.S
)

# Generate asm-offsets.h before building anything else
add_subdirectory(${KERNEL_DIR}/inc)

# Pull in subdirectory object libraries.
add_subdirectory(${KERNEL_DIR}/interruption)
add_subdirectory(${KERNEL_DIR}/dev)
add_subdirectory(${KERNEL_DIR}/lock)
add_subdirectory(${KERNEL_DIR}/mm)
add_subdirectory(${KERNEL_DIR}/proc)
add_subdirectory(${KERNEL_DIR}/timer)
add_subdirectory(${KERNEL_DIR}/vfs)
add_subdirectory(${KERNEL_DIR}/ipi)

set_source_files_properties(${KERNEL_C_FILES} PROPERTIES LANGUAGE C)
set_source_files_properties(${KERNEL_SCAN_OBJS} PROPERTIES LANGUAGE C)
set_source_files_properties(${KERNEL_S_FILES} PROPERTIES LANGUAGE C)

set(KERNEL_OBJS
    ${KERNEL_S_FILES}
    ${KERNEL_C_FILES}
    ${KERNEL_SCAN_OBJS}
)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffreestanding ")

# Add kernel executable from top-level objects; link in subdir object libraries below.
add_executable(${KERNEL_BINARY} ${KERNEL_OBJS})

# Make sure asm-offsets.h is generated before building kernel
add_dependencies(${KERNEL_BINARY} generate_asm_offsets)

# Object libraries do not automatically link; add their objects explicitly.
target_sources(${KERNEL_BINARY} PRIVATE
    $<TARGET_OBJECTS:interruption>
    $<TARGET_OBJECTS:dev>
    $<TARGET_OBJECTS:lock>
    $<TARGET_OBJECTS:mm>
    $<TARGET_OBJECTS:proc>
    $<TARGET_OBJECTS:vfs_core>
    $<TARGET_OBJECTS:vfs_tmpfs>
    $<TARGET_OBJECTS:vfs_xv6fs>
    $<TARGET_OBJECTS:ipi>
    $<TARGET_OBJECTS:timer>
)

# ==============================================================================
# Platform configuration
# ==============================================================================
# Two kernels are built:
#   - kernel       : For QEMU virt machine, KERNEL_BASE=0x80200000 (default build)
#   - kernel.orangepi : For Orange Pi RV2, KERNEL_BASE=0x00200000
#
# Build targets:
#   make         - builds kernel for QEMU (0x80200000)
#   make qemu    - runs QEMU with the kernel
#   make orangepi - builds kernel + xv6.bin + xv6.sym for Orange Pi (0x00200000)
# ==============================================================================

# QEMU kernel base address
set(KERNEL_BASE_QEMU "0x80200000")
# Orange Pi kernel base address  
set(KERNEL_BASE_ORANGEPI "0x00200000")

# Generate linker scripts for both platforms
set(KERNEL_LD_IN ${KERNEL_DIR}/kernel.ld.in)

# QEMU linker script
set(KERNEL_BASE ${KERNEL_BASE_QEMU})
set(KERNEL_LD_QEMU ${CMAKE_CURRENT_BINARY_DIR}/kernel.ld)
configure_file(${KERNEL_LD_IN} ${KERNEL_LD_QEMU} @ONLY)

# Orange Pi linker script
set(KERNEL_BASE ${KERNEL_BASE_ORANGEPI})
set(KERNEL_LD_ORANGEPI ${CMAKE_CURRENT_BINARY_DIR}/kernel.orangepi.ld)
configure_file(${KERNEL_LD_IN} ${KERNEL_LD_ORANGEPI} @ONLY)

message(STATUS "QEMU kernel base: ${KERNEL_BASE_QEMU}")
message(STATUS "Orange Pi kernel base: ${KERNEL_BASE_ORANGEPI}")

# Use target_link_options to set linker flags so they appear in <LINK_FLAGS>
# Default kernel uses QEMU linker script
target_link_options(${KERNEL_BINARY} PRIVATE 
    -z max-page-size=4096 
    -T ${KERNEL_LD_QEMU}
)

target_link_libraries(${KERNEL_BINARY} PRIVATE interruption dev lock mm proc timer vfs)

# Note: USE_LIBSBI is not applicable for S-mode kernels.
# libsbi is for M-mode firmware (OpenSBI itself), not for S-mode OS kernels.
# S-mode kernels call SBI services via ecall instruction, which is already
# implemented in kernel/sbi.c. The OpenSBI build (OPENSBI_MODE=build) provides
# the fw_jump.elf firmware that QEMU loads to handle these SBI calls.

if(DEFINED ENV{RWLOCK_TEST})
    target_compile_definitions(${KERNEL_BINARY} PRIVATE
        RWAD_WRITE_TEST=1
    )
endif()

if(DEFINED ENV{SEMAPHORE_TEST})
    target_compile_definitions(${KERNEL_BINARY} PRIVATE
        SEMAPHORE_RUNTIME_TEST=1
    )
endif()

# decompile the kernel binary
add_custom_target(${KERNEL_DECOMPILED}
    COMMAND ${CMAKE_OBJDUMP} -S ${KERNEL_BINARY} > ${KERNEL_DECOMPILED}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Decompiling kernel binary to ${KERNEL_DECOMPILED}"
    DEPENDS ${KERNEL_BINARY}
)

# Generate address-to-line mapping for QEMU kernel (replaces kernel.sym)
add_custom_target(${KERNEL_SYMBOLS}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/gen_addrline.sh ${CMAKE_OBJDUMP} ${KERNEL_BINARY} ${KERNEL_SYMBOLS}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating symbols and address-to-line mapping to ${KERNEL_SYMBOLS}"
    DEPENDS ${KERNEL_BINARY}
)

# Add these targets as dependencies to ALL so they're built by default
add_custom_target(kernel_all ALL DEPENDS ${KERNEL_BINARY} ${KERNEL_DECOMPILED} ${KERNEL_SYMBOLS})

# ==============================================================================
# Orange Pi deployment targets
# ==============================================================================
# Build a separate kernel for Orange Pi with different linker script
# We re-link all the same object files with a different linker script

set(KERNEL_ORANGEPI_ELF kernel.orangepi)

add_executable(${KERNEL_ORANGEPI_ELF} EXCLUDE_FROM_ALL ${KERNEL_OBJS})
add_dependencies(${KERNEL_ORANGEPI_ELF} generate_asm_offsets)

target_sources(${KERNEL_ORANGEPI_ELF} PRIVATE
    $<TARGET_OBJECTS:interruption>
    $<TARGET_OBJECTS:dev>
    $<TARGET_OBJECTS:lock>
    $<TARGET_OBJECTS:mm>
    $<TARGET_OBJECTS:proc>
    $<TARGET_OBJECTS:vfs_core>
    $<TARGET_OBJECTS:vfs_tmpfs>
    $<TARGET_OBJECTS:vfs_xv6fs>
    $<TARGET_OBJECTS:ipi>
    $<TARGET_OBJECTS:timer>
)

target_link_options(${KERNEL_ORANGEPI_ELF} PRIVATE 
    -z max-page-size=4096 
    -T ${KERNEL_LD_ORANGEPI}
)

target_link_libraries(${KERNEL_ORANGEPI_ELF} PRIVATE interruption dev lock mm proc timer vfs)

# Generate flat binary for direct loading (no ELF parsing needed)
set(KERNEL_BIN xv6.bin)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${KERNEL_BIN}
    COMMAND ${CMAKE_OBJCOPY} -O binary ${KERNEL_ORANGEPI_ELF} ${KERNEL_BIN}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating flat binary ${KERNEL_BIN} from Orange Pi kernel (entry: ${KERNEL_BASE_ORANGEPI})"
    DEPENDS ${KERNEL_ORANGEPI_ELF}
)

# Generate symbols and address-to-line mapping for Orange Pi kernel
set(KERNEL_SYM_DEPLOY xv6.sym)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${KERNEL_SYM_DEPLOY}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/gen_addrline.sh ${CMAKE_OBJDUMP} ${KERNEL_ORANGEPI_ELF} ${KERNEL_SYM_DEPLOY}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating symbols and address-to-line mapping to ${KERNEL_SYM_DEPLOY}"
    DEPENDS ${KERNEL_ORANGEPI_ELF}
)

# ==============================================================================
# U-Boot script compilation
# ==============================================================================
# Compile .cmd scripts to .scr format using mkimage
# These are architecture-independent and can be compiled on any host

find_program(MKIMAGE mkimage)

if(MKIMAGE)
    message(STATUS "Found mkimage: ${MKIMAGE}")
    
    # List of U-Boot scripts to compile
    set(UBOOT_SCRIPTS
        ${CMAKE_SOURCE_DIR}/boot.cmd
        ${CMAKE_SOURCE_DIR}/xv6.cmd
        ${CMAKE_SOURCE_DIR}/default.cmd
    )
    
    set(UBOOT_SCR_FILES)
    
    foreach(CMD_FILE ${UBOOT_SCRIPTS})
        if(EXISTS ${CMD_FILE})
            get_filename_component(SCRIPT_NAME ${CMD_FILE} NAME_WE)
            set(SCR_FILE ${CMAKE_CURRENT_BINARY_DIR}/${SCRIPT_NAME}.scr)
            
            add_custom_command(
                OUTPUT ${SCR_FILE}
                COMMAND ${MKIMAGE} -C none -A riscv -T script -d ${CMD_FILE} ${SCR_FILE}
                DEPENDS ${CMD_FILE}
                COMMENT "Compiling U-Boot script: ${SCRIPT_NAME}.cmd -> ${SCRIPT_NAME}.scr"
            )
            
            list(APPEND UBOOT_SCR_FILES ${SCR_FILE})
        endif()
    endforeach()
    
    # Target to compile all U-Boot scripts
    add_custom_target(uboot-scripts
        DEPENDS ${UBOOT_SCR_FILES}
        COMMENT "Compiling U-Boot scripts (.cmd -> .scr)"
    )
else()
    message(STATUS "mkimage not found - U-Boot script compilation disabled")
    message(STATUS "  Install with: sudo apt-get install u-boot-tools")
endif()

# Target to build deployment files for Orange Pi
add_custom_target(orangepi
    DEPENDS 
        ${CMAKE_CURRENT_BINARY_DIR}/${KERNEL_BIN}
        ${CMAKE_CURRENT_BINARY_DIR}/${KERNEL_SYM_DEPLOY}
    COMMENT "Building Orange Pi deployment files (xv6.bin, xv6.sym)"
)

# Add uboot-scripts as dependency of orangepi target if mkimage is available
if(MKIMAGE)
    add_dependencies(orangepi uboot-scripts)
endif()

# ==============================================================================
# Deployment target
# ==============================================================================
# Deploy to Orange Pi via scp
# Usage: cmake -DORANGEPI_IP=192.168.0.201 .. && make deploy
# Or:    make deploy ORANGEPI_IP=192.168.0.201

set(ORANGEPI_IP "192.168.0.201" CACHE STRING "Orange Pi IP address for deployment")
set(ORANGEPI_USER "root" CACHE STRING "Orange Pi SSH user for deployment")
set(ORANGEPI_BOOT_DIR "/boot" CACHE STRING "Orange Pi boot directory")

add_custom_target(deploy
    COMMAND scp 
        ${CMAKE_CURRENT_BINARY_DIR}/${KERNEL_BIN}
        ${CMAKE_CURRENT_BINARY_DIR}/${KERNEL_SYM_DEPLOY}
        ${CMAKE_CURRENT_BINARY_DIR}/boot.scr
        ${CMAKE_CURRENT_BINARY_DIR}/default.scr
        ${CMAKE_CURRENT_BINARY_DIR}/xv6.scr
        ${ORANGEPI_USER}@${ORANGEPI_IP}:${ORANGEPI_BOOT_DIR}/
    DEPENDS orangepi
    COMMENT "Deploying to Orange Pi at ${ORANGEPI_USER}@${ORANGEPI_IP}:${ORANGEPI_BOOT_DIR}/"
)

message(STATUS "Deploy target: scp to ${ORANGEPI_USER}@${ORANGEPI_IP}:${ORANGEPI_BOOT_DIR}/")