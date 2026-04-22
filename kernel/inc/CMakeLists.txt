# Generate asm-offsets.h from structure definitions using Python script
# This creates a header file with structure offset definitions for assembly files

set(GEN_ASM_OFFSETS_SCRIPT ${CMAKE_SOURCE_DIR}/scripts/gen_asm_offsets.py)
set(ASM_OFFSETS_H ${CMAKE_CURRENT_BINARY_DIR}/asm-offsets.h)

# Always use the same compiler cmake chose for the kernel — this is already
# validated for the target architecture. TOOLPREFIX may point to a different
# arch's toolchain (e.g. riscv when building for x86_64), so we must NOT use it.
set(ASM_OFFSETS_COMPILER ${CMAKE_C_COMPILER})

# Uppercase arch name for the CONFIG_ARCH_* preprocessor define
string(TOUPPER "${ARCH}" ARCH_UPPER)

# Arch-specific core register/CPU header (e.g. riscv.h for RISC-V, x86.h for x86_64)
if(ARCH STREQUAL "x86_64")
    set(ARCH_CORE_HEADER "x86.h")
else()
    set(ARCH_CORE_HEADER "riscv.h")
endif()

# Define structures to generate offsets for
# Format: --struct name:header for auto-detection of all fields
set(ASM_OFFSET_STRUCTS
    trapframe:../../arch/${ARCH}/inc/trapframe.h
    utrapframe:../../arch/${ARCH}/inc/trapframe.h
    context:../../arch/${ARCH}/inc/trapframe.h
    cpu_local:smp/percpu_types.h
    thread:proc/thread_types.h
)

# Define prerequisite headers needed for compilation
set(ASM_OFFSET_HEADERS
    types.h
    param.h
    ../../arch/${ARCH}/inc/${ARCH_CORE_HEADER}
)

# Build the command line arguments for headers
set(HEADER_ARGS)
foreach(header ${ASM_OFFSET_HEADERS})
    list(APPEND HEADER_ARGS --header ${header})
endforeach()

# Build the command line arguments for structures
set(STRUCT_ARGS)
foreach(struct ${ASM_OFFSET_STRUCTS})
    list(APPEND STRUCT_ARGS --struct ${struct})
endforeach()

# Build dependency list from all header files
set(HEADER_DEPENDENCIES ${GEN_ASM_OFFSETS_SCRIPT})
foreach(header ${ASM_OFFSET_HEADERS})
    list(APPEND HEADER_DEPENDENCIES ${CMAKE_SOURCE_DIR}/kernel/inc/${header})
endforeach()
# Extract unique structure header files from ASM_OFFSET_STRUCTS
set(STRUCT_HEADERS)
foreach(struct ${ASM_OFFSET_STRUCTS})
    string(REGEX REPLACE "^[^:]+:(.+)$" "\\1" header_file ${struct})
    list(APPEND STRUCT_HEADERS ${CMAKE_SOURCE_DIR}/kernel/inc/${header_file})
endforeach()
list(REMOVE_DUPLICATES STRUCT_HEADERS)
list(APPEND HEADER_DEPENDENCIES ${STRUCT_HEADERS})

# Generate asm-offsets.h using the Python script
add_custom_command(
    OUTPUT ${ASM_OFFSETS_H}
    COMMAND ${GEN_ASM_OFFSETS_SCRIPT}
        --compiler ${ASM_OFFSETS_COMPILER}
        --include ${CMAKE_SOURCE_DIR}/kernel/inc
        --include ${CMAKE_SOURCE_DIR}/arch/${ARCH}/inc
        --define CONFIG_ARCH_${ARCH_UPPER}=1
        ${HEADER_ARGS}
        ${STRUCT_ARGS}
        --output ${ASM_OFFSETS_H}
    DEPENDS ${HEADER_DEPENDENCIES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Generating asm-offsets.h for assembly files"
    VERBATIM
)

# Create a custom target that other targets can depend on
add_custom_target(generate_asm_offsets
    DEPENDS ${ASM_OFFSETS_H}
)
