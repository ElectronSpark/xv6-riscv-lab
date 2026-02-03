set(KERNEL_BINARY kernel)
set(KERNEL_DECOMPILED kernel.asm)
set(KERNEL_SYMBOLS kernel.sym)

if(NOT DEFINED ENV{LAB})
    message(FATAL_ERROR "Environment variable LAB is not set")
else()
    message(STATUS "Building kernel for lab: $ENV{LAB}")
endif()

# ==============================================================================
# Platform configuration
# ==============================================================================
# Set PLATFORM to select target:
#   - qemu (default): KERNEL_BASE=0x80200000
#   - orangepi: KERNEL_BASE=0x00200000
# ==============================================================================
if(NOT DEFINED PLATFORM)
    set(PLATFORM "qemu")
endif()

if(PLATFORM STREQUAL "orangepi")
    set(KERNEL_BASE "0x00200000")
else()
    set(KERNEL_BASE "0x80200000")
endif()

# Size of parsed symbol index (for rb-tree nodes)
# ~39000 entries @ 80 bytes each = ~3MB
set(KERNEL_SYMBOLS_IDX_SIZE "0x300000")

message(STATUS "Platform: ${PLATFORM}, kernel base: ${KERNEL_BASE}")

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
    list(APPEND KERNEL_C_FILES ${KERNEL_DIR}/lock/rwlock_test.c)
endif()

if(DEFINED ENV{SEMAPHORE_TEST})
    list(APPEND KERNEL_C_FILES ${KERNEL_DIR}/lock/semaphore_test.c)
endif()

set(KERNEL_SCAN_OBJS
    ${KERNEL_DIR}/start.c
    ${KERNEL_DIR}/console.c
    ${KERNEL_DIR}/printf.c
    ${KERNEL_DIR}/uart.c
)

if(DEFINED ENV{KSCAN})
    list(APPEND KERNEL_SCAN_OBJS ${KERNEL_DIR}/kscan.c)
endif()

if($ENV{LAB} STREQUAL "lock")
    list(APPEND KERNEL_C_FILES ${KERNEL_DIR}/stats.c ${KERNEL_DIR}/sprintf.c)
endif()

if($ENV{LAB} STREQUAL "net")
    list(APPEND KERNEL_C_FILES ${KERNEL_DIR}/e1000.c ${KERNEL_DIR}/net.c ${KERNEL_DIR}/pci.c)
endif()

set(KERNEL_S_FILES ${KERNEL_DIR}/entry.S)

# Generate asm-offsets.h before building anything else
add_subdirectory(${KERNEL_DIR}/inc)

# Pull in subdirectory object libraries.
add_subdirectory(${KERNEL_DIR}/irq)
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

set(KERNEL_OBJS ${KERNEL_S_FILES} ${KERNEL_C_FILES} ${KERNEL_SCAN_OBJS})

# Object libraries to link
set(KERNEL_OBJ_LIBS
    $<TARGET_OBJECTS:irq>
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

set(KERNEL_LINK_LIBS irq dev lock mm proc timer vfs)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffreestanding ")

# Generate linker script from template
set(KERNEL_LD_IN ${KERNEL_DIR}/kernel.ld.in)
set(KERNEL_LD ${CMAKE_CURRENT_BINARY_DIR}/kernel.ld)
configure_file(${KERNEL_LD_IN} ${KERNEL_LD} @ONLY)

# ==============================================================================
# Plain kernel (without embedded symbols)
# ==============================================================================
add_executable(${KERNEL_BINARY} ${KERNEL_OBJS})
add_dependencies(${KERNEL_BINARY} generate_asm_offsets)
target_sources(${KERNEL_BINARY} PRIVATE ${KERNEL_OBJ_LIBS})
target_link_options(${KERNEL_BINARY} PRIVATE -z max-page-size=4096 -T ${KERNEL_LD})
target_link_libraries(${KERNEL_BINARY} PRIVATE ${KERNEL_LINK_LIBS})

if(DEFINED ENV{RWLOCK_TEST})
    target_compile_definitions(${KERNEL_BINARY} PRIVATE RWAD_WRITE_TEST=1)
endif()

if(DEFINED ENV{SEMAPHORE_TEST})
    target_compile_definitions(${KERNEL_BINARY} PRIVATE SEMAPHORE_RUNTIME_TEST=1)
endif()

# Decompile the kernel binary
add_custom_target(${KERNEL_DECOMPILED}
    COMMAND ${CMAKE_OBJDUMP} -S ${KERNEL_BINARY} > ${KERNEL_DECOMPILED}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Decompiling kernel binary"
    DEPENDS ${KERNEL_BINARY}
)

# Generate symbol file for plain kernel
add_custom_target(${KERNEL_SYMBOLS}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/gen_addrline.sh ${CMAKE_OBJDUMP} ${KERNEL_BINARY} ${KERNEL_SYMBOLS}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating symbols"
    DEPENDS ${KERNEL_BINARY}
)

# ==============================================================================
# Kernel with embedded symbols (two-phase build)
# ==============================================================================
set(KSYM_PLACEHOLDER_SIZE "0x100000")
set(KSYM_PLACEHOLDER_ASM ${CMAKE_CURRENT_BINARY_DIR}/ksymbols_placeholder.S)

add_custom_command(
    OUTPUT ${KSYM_PLACEHOLDER_ASM}
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/gen_ksymbols_placeholder.py
            ${KSYM_PLACEHOLDER_SIZE} ${KSYM_PLACEHOLDER_ASM}
    COMMENT "Generating ${KSYM_PLACEHOLDER_SIZE} byte .ksymbols placeholder"
    DEPENDS ${CMAKE_SOURCE_DIR}/scripts/gen_ksymbols_placeholder.py
)

add_library(ksymbols_placeholder OBJECT EXCLUDE_FROM_ALL ${KSYM_PLACEHOLDER_ASM})
add_dependencies(ksymbols_placeholder generate_asm_offsets)
set_source_files_properties(${KSYM_PLACEHOLDER_ASM} PROPERTIES GENERATED TRUE)

# Kernel with placeholder
add_executable(kernel_with_symbols_elf EXCLUDE_FROM_ALL ${KERNEL_OBJS})
add_dependencies(kernel_with_symbols_elf generate_asm_offsets ksymbols_placeholder)
target_sources(kernel_with_symbols_elf PRIVATE ${KERNEL_OBJ_LIBS} $<TARGET_OBJECTS:ksymbols_placeholder>)
target_link_options(kernel_with_symbols_elf PRIVATE -z max-page-size=4096 -T ${KERNEL_LD})
target_link_libraries(kernel_with_symbols_elf PRIVATE ${KERNEL_LINK_LIBS})

if(DEFINED ENV{RWLOCK_TEST})
    target_compile_definitions(kernel_with_symbols_elf PRIVATE RWAD_WRITE_TEST=1)
endif()

if(DEFINED ENV{SEMAPHORE_TEST})
    target_compile_definitions(kernel_with_symbols_elf PRIVATE SEMAPHORE_RUNTIME_TEST=1)
endif()

# Generate symbols from kernel with placeholder
set(KERNEL_SYM_FINAL ${CMAKE_CURRENT_BINARY_DIR}/kernel_with_symbols_elf.sym)
add_custom_command(
    OUTPUT ${KERNEL_SYM_FINAL}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/gen_addrline.sh ${CMAKE_OBJDUMP}
            $<TARGET_FILE:kernel_with_symbols_elf> ${KERNEL_SYM_FINAL}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating symbols from kernel with placeholder"
    DEPENDS kernel_with_symbols_elf
)

# Embed symbols into kernel
add_custom_target(kernel_with_symbols
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/embed_ksymbols.py
            $<TARGET_FILE:kernel_with_symbols_elf> ${KERNEL_SYM_FINAL} --elf
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Embedding symbols into kernel ELF"
    DEPENDS kernel_with_symbols_elf ${KERNEL_SYM_FINAL}
            ${CMAKE_SOURCE_DIR}/scripts/embed_ksymbols.py
)

# ==============================================================================
# Final outputs: xv6.bin and xv6.sym
# ==============================================================================
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xv6.bin
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:kernel_with_symbols_elf> xv6.bin
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/patch_image_size.py xv6.bin
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating xv6.bin (${PLATFORM})"
    DEPENDS kernel_with_symbols $<TARGET_FILE:kernel_with_symbols_elf>
)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xv6.sym
    COMMAND ${CMAKE_COMMAND} -E copy ${KERNEL_SYM_FINAL} xv6.sym
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating xv6.sym"
    DEPENDS ${KERNEL_SYM_FINAL}
)

# ==============================================================================
# Compressed kernel image (optional)
# ==============================================================================
# Both QEMU and U-Boot (booti) support gzip-compressed kernel images.
# The compressed image is named xv6.bin.gz
# ==============================================================================
if(ENABLE_COMPRESSION)
    find_program(GZIP gzip)
    if(GZIP)
        add_custom_command(
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xv6.bin.gz
            COMMAND ${GZIP} -9 -k -f xv6.bin
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Compressing xv6.bin -> xv6.bin.gz"
            DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/xv6.bin
        )
        set(KERNEL_BIN_COMPRESSED ${CMAKE_CURRENT_BINARY_DIR}/xv6.bin.gz)
    endif()
endif()

add_custom_target(kernel_all ALL
    DEPENDS kernel_with_symbols ${KERNEL_DECOMPILED}
            ${CMAKE_CURRENT_BINARY_DIR}/xv6.bin
            ${CMAKE_CURRENT_BINARY_DIR}/xv6.sym
            $<$<BOOL:${ENABLE_COMPRESSION}>:${KERNEL_BIN_COMPRESSED}>
)

# Export variables needed by boot/CMakeLists.txt
set(KERNEL_BIN xv6.bin PARENT_SCOPE)
set(KERNEL_BIN_GZ xv6.bin.gz PARENT_SCOPE)
set(KERNEL_SYM_DEPLOY xv6.sym PARENT_SCOPE)
set(ENABLE_COMPRESSION ${ENABLE_COMPRESSION} PARENT_SCOPE)