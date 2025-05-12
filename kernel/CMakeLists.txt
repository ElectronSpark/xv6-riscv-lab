set(KERNEL_BINARY kernel)
set(KERNEL_DECOMPILED kernel.asm)
set(KERNEL_SYMBOLS kernel.sym)

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