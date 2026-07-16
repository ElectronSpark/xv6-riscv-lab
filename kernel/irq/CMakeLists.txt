# NOTE: plic.c and irq.c have been rewritten in Rust (kernel/irq/plic.rs,
# kernel/irq/irq_core.rs — Phase 2 Wave 5); trap.c has been rewritten in
# Rust (kernel/irq/trap.rs — Phase 2 Wave 6); syscall.c has been rewritten
# in Rust (kernel/irq/syscall.rs — Phase 2 Wave 7). Their symbols are
# linked in via the xv6_rust staticlib (see kernel/CMakeLists.txt). The
# `irq` module has no C sources left; this OBJECT library now exists
# solely to assemble kernelvec.S/trampoline.S, which still need a CMake
# C-language object target (they use the C preprocessor + asm-offsets.h —
# a static checked-in header since P3-5, kernel/inc/asm-offsets.h, found
# via the global -I${KERNEL_DIR}/inc; its values are enforced by the
# native Rust types' const asserts, see that header's comment).
set(INTERRUPTION_S_SRCS
    kernelvec.S
    trampoline.S
)

add_library(irq OBJECT
    ${INTERRUPTION_S_SRCS}
)

# These .S files use the C preprocessor; force C language for them.
set_source_files_properties(${INTERRUPTION_S_SRCS} PROPERTIES LANGUAGE C)

# P3-5: asm-offsets.h is a static checked-in header now
# (kernel/inc/asm-offsets.h); the generate_asm_offsets custom target and
# the ${CMAKE_BINARY_DIR}/kernel/inc include dir it wrote into are gone.
