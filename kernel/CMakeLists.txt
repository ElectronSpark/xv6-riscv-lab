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
    ${KERNEL_DIR}/ramdisk.c
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

# Entry assembly (without placeholder - placeholder is generated during build)
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

# Core kernel sources (without placeholder)
set(KERNEL_CORE_OBJS
    ${KERNEL_S_FILES}
    ${KERNEL_C_FILES}
    ${KERNEL_SCAN_OBJS}
)

# For backward compatibility
set(KERNEL_OBJS ${KERNEL_CORE_OBJS})

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffreestanding ")

# ==============================================================================
# Two-Phase Build with Embedded Symbols
# ==============================================================================
# Phase 1: Partial link (ld -r) to create kernel.o, estimate symbol size
# Phase 2: Generate sized placeholder, final link, embed actual symbols
#
# This ensures symbol addresses are correct because:
# 1. Placeholder size is fixed before final link
# 2. Symbols are generated from the final kernel
# ==============================================================================

# Collect all object file paths for partial link
set(KERNEL_PARTIAL_OBJS
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

# Size of parsed symbol index (for rb-tree nodes)
# ~39000 entries @ 80 bytes each = ~3MB
set(KERNEL_SYMBOLS_IDX_SIZE "0x300000")

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

# ==============================================================================
# Two-Phase Build for Embedded Kernel Symbols
# ==============================================================================
# This build process ensures symbol addresses are correct:
#
# Phase 1 (estimate):
#   - Use the plain kernel (without embedded symbols) to estimate symbol size
#   - Generate address-to-line mapping (kernel.sym)
#   - Calculate required placeholder size with safety margin
#
# Phase 2 (final):
#   - Generate sized placeholder assembly
#   - Link final kernel with placeholder
#   - Generate symbols from final kernel
#   - Embed symbols into kernel binary (in-place patching)
#
# Targets:
#   kernel              - Plain kernel (no embedded symbols, uses external loader)
#   kernel.sym          - Symbol file for plain kernel
#   kernel_with_symbols - Kernel with embedded symbols (two-phase build)
# ==============================================================================

# Phase 1 outputs
set(KERNEL_SYM_PHASE1 ${CMAKE_CURRENT_BINARY_DIR}/${KERNEL_SYMBOLS})

# Phase 2 outputs  
set(KSYM_PLACEHOLDER_ASM ${CMAKE_CURRENT_BINARY_DIR}/ksymbols_sized_placeholder.S)
set(KSYM_PLACEHOLDER_OBJ ${CMAKE_CURRENT_BINARY_DIR}/ksymbols_sized_placeholder.o)
set(KERNEL_WITH_SYMBOLS_ELF ${CMAKE_CURRENT_BINARY_DIR}/kernel_with_symbols)
set(KERNEL_SYM_FINAL ${CMAKE_CURRENT_BINARY_DIR}/kernel_with_symbols.sym)

# Calculate placeholder size with 25% margin and page alignment
# Using a fixed generous size for now (1MB should be plenty)
set(KSYM_PLACEHOLDER_SIZE "0x100000")

# Generate sized placeholder assembly
add_custom_command(
    OUTPUT ${KSYM_PLACEHOLDER_ASM}
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/gen_ksymbols_placeholder.py
            ${KSYM_PLACEHOLDER_SIZE}
            ${KSYM_PLACEHOLDER_ASM}
    COMMENT "Generating ${KSYM_PLACEHOLDER_SIZE} byte placeholder for .ksymbols"
    DEPENDS ${CMAKE_SOURCE_DIR}/scripts/gen_ksymbols_placeholder.py
)

# Create an object library for the placeholder (so we can link it properly)
add_library(ksymbols_placeholder OBJECT EXCLUDE_FROM_ALL ${KSYM_PLACEHOLDER_ASM})
add_dependencies(ksymbols_placeholder generate_asm_offsets)

# Make the placeholder depend on its source generation
set_source_files_properties(${KSYM_PLACEHOLDER_ASM} PROPERTIES GENERATED TRUE)

# Add custom command to ensure placeholder is generated before compilation
add_custom_command(
    OUTPUT ${KSYM_PLACEHOLDER_ASM}_stamp
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/gen_ksymbols_placeholder.py
            ${KSYM_PLACEHOLDER_SIZE}
            ${KSYM_PLACEHOLDER_ASM}
    COMMAND ${CMAKE_COMMAND} -E touch ${KSYM_PLACEHOLDER_ASM}_stamp
    COMMENT "Ensuring placeholder is generated"
    DEPENDS ${CMAKE_SOURCE_DIR}/scripts/gen_ksymbols_placeholder.py
)

add_custom_target(gen_placeholder DEPENDS ${KSYM_PLACEHOLDER_ASM} ${KSYM_PLACEHOLDER_ASM}_stamp)
add_dependencies(ksymbols_placeholder gen_placeholder)

# Phase 2: Kernel with placeholder - redefine as another executable
# We create a new executable that links all the same object files PLUS the placeholder
add_executable(kernel_with_symbols_elf EXCLUDE_FROM_ALL ${KERNEL_OBJS})
add_dependencies(kernel_with_symbols_elf generate_asm_offsets ksymbols_placeholder)

target_sources(kernel_with_symbols_elf PRIVATE
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
    $<TARGET_OBJECTS:ksymbols_placeholder>
)

target_link_options(kernel_with_symbols_elf PRIVATE 
    -z max-page-size=4096 
    -T ${KERNEL_LD_QEMU}
)

target_link_libraries(kernel_with_symbols_elf PRIVATE interruption dev lock mm proc timer vfs)

# Generate symbols from the kernel with placeholder (correct addresses!)
set(KERNEL_SYM_FINAL ${CMAKE_CURRENT_BINARY_DIR}/kernel_with_symbols_elf.sym)
add_custom_command(
    OUTPUT ${KERNEL_SYM_FINAL}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/gen_addrline.sh ${CMAKE_OBJDUMP} 
            $<TARGET_FILE:kernel_with_symbols_elf> ${KERNEL_SYM_FINAL}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating symbols from kernel with placeholder"
    DEPENDS kernel_with_symbols_elf
)

# Target to embed symbols into the kernel binary
add_custom_target(kernel_with_symbols
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/embed_ksymbols.py
            $<TARGET_FILE:kernel_with_symbols_elf>
            ${KERNEL_SYM_FINAL}
            --elf
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Embedding symbols into kernel ELF"
    DEPENDS kernel_with_symbols_elf ${KERNEL_SYM_FINAL}
            ${CMAKE_SOURCE_DIR}/scripts/embed_ksymbols.py
)

# ==============================================================================
# Platform-dependent xv6.bin generation
# ==============================================================================
# Based on PLATFORM variable (qemu or orangepi), build xv6.bin directly
# - PLATFORM=qemu (default): Build xv6.bin from QEMU kernel (0x80200000)
# - PLATFORM=orangepi: Build xv6.bin from Orange Pi kernel (0x00200000)
# ==============================================================================

if(NOT PLATFORM STREQUAL "orangepi")
    # QEMU platform: generate xv6.bin directly from QEMU kernel
    # Note: We depend on both the target AND the ELF file to ensure rebuild
    # when source changes. The embed script modifies the ELF in-place.
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xv6.bin
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:kernel_with_symbols_elf> xv6.bin
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/patch_image_size.py xv6.bin
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating xv6.bin (QEMU platform)"
        DEPENDS kernel_with_symbols $<TARGET_FILE:kernel_with_symbols_elf>
    )
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xv6.sym
        COMMAND ${CMAKE_COMMAND} -E copy ${KERNEL_SYM_FINAL} xv6.sym
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating xv6.sym (QEMU platform)"
        DEPENDS ${KERNEL_SYM_FINAL}
    )
    add_custom_target(kernel_all ALL 
        DEPENDS kernel_with_symbols ${KERNEL_DECOMPILED} 
                ${CMAKE_CURRENT_BINARY_DIR}/xv6.bin
                ${CMAKE_CURRENT_BINARY_DIR}/xv6.sym
    )
endif()

# ==============================================================================
# Orange Pi kernel build (only when PLATFORM=orangepi)
# ==============================================================================
# Build kernel for Orange Pi with embedded symbols using two-phase build.
# The flat binary (xv6.bin) includes embedded symbols for standalone operation.
#
# Targets:
#   orangepi            - Build xv6.bin with embedded symbols
#   orangepi_no_symbols - Build xv6.bin without embedded symbols (smaller)
# ==============================================================================
# ==============================================================================

# Orange Pi linker script (already generated above)
set(KERNEL_ORANGEPI_ELF kernel.orangepi)

# Plain Orange Pi kernel (without embedded symbols) - used for size estimation
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

# ------------------------------------------------------------------------------
# Orange Pi kernel WITH embedded symbols (two-phase build)
# ------------------------------------------------------------------------------

# Create an object library for the Orange Pi placeholder
set(KSYM_PLACEHOLDER_ORANGEPI_ASM ${CMAKE_CURRENT_BINARY_DIR}/ksymbols_placeholder_orangepi.S)

add_custom_command(
    OUTPUT ${KSYM_PLACEHOLDER_ORANGEPI_ASM}
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/gen_ksymbols_placeholder.py
            ${KSYM_PLACEHOLDER_SIZE}
            ${KSYM_PLACEHOLDER_ORANGEPI_ASM}
    COMMENT "Generating ${KSYM_PLACEHOLDER_SIZE} byte placeholder for Orange Pi .ksymbols"
    DEPENDS ${CMAKE_SOURCE_DIR}/scripts/gen_ksymbols_placeholder.py
)

add_library(ksymbols_placeholder_orangepi OBJECT EXCLUDE_FROM_ALL ${KSYM_PLACEHOLDER_ORANGEPI_ASM})
add_dependencies(ksymbols_placeholder_orangepi generate_asm_offsets)
set_source_files_properties(${KSYM_PLACEHOLDER_ORANGEPI_ASM} PROPERTIES GENERATED TRUE)

add_custom_command(
    OUTPUT ${KSYM_PLACEHOLDER_ORANGEPI_ASM}_stamp
    COMMAND ${CMAKE_COMMAND} -E touch ${KSYM_PLACEHOLDER_ORANGEPI_ASM}_stamp
    COMMENT "Orange Pi placeholder stamp"
    DEPENDS ${KSYM_PLACEHOLDER_ORANGEPI_ASM}
)
add_custom_target(gen_placeholder_orangepi DEPENDS ${KSYM_PLACEHOLDER_ORANGEPI_ASM} ${KSYM_PLACEHOLDER_ORANGEPI_ASM}_stamp)
add_dependencies(ksymbols_placeholder_orangepi gen_placeholder_orangepi)

# Orange Pi kernel with placeholder - linked with different linker script
add_executable(kernel_orangepi_with_symbols_elf EXCLUDE_FROM_ALL ${KERNEL_OBJS})
add_dependencies(kernel_orangepi_with_symbols_elf generate_asm_offsets ksymbols_placeholder_orangepi)

target_sources(kernel_orangepi_with_symbols_elf PRIVATE
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
    $<TARGET_OBJECTS:ksymbols_placeholder_orangepi>
)

target_link_options(kernel_orangepi_with_symbols_elf PRIVATE 
    -z max-page-size=4096 
    -T ${KERNEL_LD_ORANGEPI}
)

target_link_libraries(kernel_orangepi_with_symbols_elf PRIVATE interruption dev lock mm proc timer vfs)

# Generate symbols from the Orange Pi kernel with placeholder
set(KERNEL_ORANGEPI_SYM_FINAL ${CMAKE_CURRENT_BINARY_DIR}/kernel_orangepi_with_symbols_elf.sym)
add_custom_command(
    OUTPUT ${KERNEL_ORANGEPI_SYM_FINAL}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/gen_addrline.sh ${CMAKE_OBJDUMP} 
            $<TARGET_FILE:kernel_orangepi_with_symbols_elf> ${KERNEL_ORANGEPI_SYM_FINAL}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating symbols from Orange Pi kernel"
    DEPENDS kernel_orangepi_with_symbols_elf $<TARGET_FILE:kernel_orangepi_with_symbols_elf>
)

# Embed symbols, then convert to flat binary
# We need to copy, embed, then convert to binary - use a custom command chain
set(KERNEL_ORANGEPI_EMBEDDED ${CMAKE_CURRENT_BINARY_DIR}/kernel_orangepi_embedded)

add_custom_command(
    OUTPUT ${KERNEL_ORANGEPI_EMBEDDED}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:kernel_orangepi_with_symbols_elf> ${KERNEL_ORANGEPI_EMBEDDED}
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/embed_ksymbols.py
            ${KERNEL_ORANGEPI_EMBEDDED}
            ${KERNEL_ORANGEPI_SYM_FINAL}
            --elf
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Embedding symbols into Orange Pi kernel"
    DEPENDS kernel_orangepi_with_symbols_elf $<TARGET_FILE:kernel_orangepi_with_symbols_elf>
            ${KERNEL_ORANGEPI_SYM_FINAL}
            ${CMAKE_SOURCE_DIR}/scripts/embed_ksymbols.py
)

# Orange Pi platform: generate xv6.bin directly (mutually exclusive with QEMU)
if(PLATFORM STREQUAL "orangepi")
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xv6.bin
        COMMAND ${CMAKE_OBJCOPY} -O binary ${KERNEL_ORANGEPI_EMBEDDED} xv6.bin
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/patch_image_size.py xv6.bin
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating xv6.bin (Orange Pi platform)"
        DEPENDS ${KERNEL_ORANGEPI_EMBEDDED}
    )
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xv6.sym
        COMMAND ${CMAKE_COMMAND} -E copy ${KERNEL_ORANGEPI_SYM_FINAL} xv6.sym
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating xv6.sym (Orange Pi platform)"
        DEPENDS ${KERNEL_ORANGEPI_SYM_FINAL}
    )
    add_custom_target(kernel_all ALL 
        DEPENDS kernel_with_symbols ${KERNEL_DECOMPILED} 
                ${CMAKE_CURRENT_BINARY_DIR}/xv6.bin
                ${CMAKE_CURRENT_BINARY_DIR}/xv6.sym
    )
endif()

# Target for explicit Orange Pi build (builds xv6.bin for deploy)
add_custom_target(orangepi
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/xv6.bin ${CMAKE_CURRENT_BINARY_DIR}/xv6.sym
    COMMENT "Building Orange Pi kernel"
)

# Export variables needed by boot/CMakeLists.txt
# Both QEMU and Orange Pi output to kernel/xv6.bin (mutually exclusive platforms)
set(KERNEL_BIN xv6.bin PARENT_SCOPE)
set(KERNEL_SYM_DEPLOY xv6.sym PARENT_SCOPE)