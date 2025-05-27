set(KERNEL_BINARY kernel)
set(KERNEL_DECOMPILED kernel.asm)
set(KERNEL_SYMBOLS kernel.sym)

# Separate .S and .c files for kernel source files
set(KERNEL_C_FILES
    ${KERNEL_DIR}/kalloc
    ${KERNEL_DIR}/string
    ${KERNEL_DIR}/main
    ${KERNEL_DIR}/vm
    ${KERNEL_DIR}/proc
    ${KERNEL_DIR}/trap
    ${KERNEL_DIR}/syscall
    ${KERNEL_DIR}/sysproc
    ${KERNEL_DIR}/bio
    ${KERNEL_DIR}/fs
    ${KERNEL_DIR}/log
    ${KERNEL_DIR}/sleeplock
    ${KERNEL_DIR}/file
    ${KERNEL_DIR}/pipe
    ${KERNEL_DIR}/exec
    ${KERNEL_DIR}/sysfile
    ${KERNEL_DIR}/plic
    ${KERNEL_DIR}/virtio_disk
    ${KERNEL_DIR}/backtrace
    ${KERNEL_DIR}/e1000
    ${KERNEL_DIR}/net
    ${KERNEL_DIR}/pci
    ${KERNEL_DIR}/sysnet
    ${KERNEL_DIR}/page
    ${KERNEL_DIR}/slab
    ${KERNEL_DIR}/start
    ${KERNEL_DIR}/console
    ${KERNEL_DIR}/printf
    ${KERNEL_DIR}/uart
    ${KERNEL_DIR}/spinlock
    ${KERNEL_DIR}/hlist
)

set(KERNEL_S_FILES
    ${KERNEL_DIR}/entry.S
    ${KERNEL_DIR}/swtch.S
    ${KERNEL_DIR}/trampoline.S
    ${KERNEL_DIR}/kernelvec.S
)

set_source_files_properties(${KERNEL_C_FILES}.c PROPERTIES LANGUAGE C)
set_source_files_properties(${KERNEL_S_FILES} PROPERTIES LANGUAGE C)

# Add kernel executable
add_executable(${KERNEL_BINARY} ${KERNEL_S_FILES} ${KERNEL_C_FILES}.c)

# Use target_link_options to set linker flags so they appear in <LINK_FLAGS>
target_link_options(${KERNEL_BINARY} PRIVATE -z max-page-size=4096 -T ${KERNEL_DIR}/kernel.ld)

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