/*
 * bits/mman.h — xv6 arch-specific mman overrides for musl
 *
 * musl's include/sys/mman.h defines all standard MAP_*, PROT_*,
 * MADV_*, MCL_*, MS_*, MREMAP_* macros. This file is included
 * AFTER those definitions and should only contain arch-specific
 * overrides. xv6/riscv64 uses standard Linux values, so this
 * file is intentionally empty.
 */
