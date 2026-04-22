/*
 * Kernel symbols placeholder section
 *
 * This file is auto-generated during the build process.
 * It reserves space for the embedded symbol table.
 *
 * Build process:
 * 1. Phase 1: Partial link all objects (ld -r) to get kernel.o
 * 2. Estimate symbol size from kernel.o
 * 3. Generate this placeholder with the calculated size
 * 4. Phase 2: Final link kernel.o + placeholder → kernel
 * 5. Generate actual symbols from final kernel
 * 6. Embed symbols into the kernel image
 *
 * The size is fixed upfront, ensuring addresses don't shift
 * between symbol generation and embedding.
 */

.section .ksymbols, "a", @progbits
.global __ksymbols_placeholder_start
.global __ksymbols_placeholder_end

__ksymbols_placeholder_start:
    /* 
     * Default minimal placeholder (1 byte).
     * This will be replaced by gen_ksymbols_placeholder.py
     * with a properly sized placeholder during the build.
     */
    .byte 0x0a
__ksymbols_placeholder_end:
