#!/bin/bash
#
# apply_xv6_overlay.sh — Apply xv6-specific patches to a musl source tree.
#
# This is a shared helper used by:
#   - user/musl-xv6/build_musl.sh          (standalone riscv64 musl build)
#   - user/musl-xv6/build_musl_x86_64.sh   (standalone x86_64 musl build)
#   - scripts/build_gcc_toolchain.sh        (two-phase GCC toolchain build)
#
# Usage:
#   source apply_xv6_overlay.sh            # import functions
#   apply_xv6_overlay <arch> <musl_src> <overlay_dir>
#
# Arguments:
#   arch        - "riscv64" or "x86_64"
#   musl_src    - path to extracted musl source tree (musl-X.Y.Z/)
#   overlay_dir - path to user/musl-xv6/arch/ (parent of riscv64/ and x86_64/)
#
# What it changes (ONLY files that differ from upstream Linux musl):
#   - bits/syscall.h.in    — xv6 syscall numbers
#   - kstat.h              — xv6 stat structure layout
#   - clone.s              — xv6 clone takes a struct pointer
#   - __unmapself.s         — xv6 syscall numbers (not Linux hardcoded)
#   - vfork.s              — use clone with CLONE_VM|CLONE_VFORK
#   - fstatat.c            — remove zero-initialiser on kstat (xv6 compat)
#   - brk enabled, guard page removed — MAP_FIXED inside heap breaks sbrk
#   - arch-specific extras (crt_arch.h, syscall_arch.h, bits/*.h if present)
#
# The key insight: musl's RISC-V ecall convention is identical to xv6's
# (a7=syscall#, a0-a5=args, a0=return, negative=negated errno).

set -euo pipefail

apply_xv6_overlay() {
    local arch="$1"
    local musl_src="$2"
    local overlay_dir="$3"

    if [[ ! -d "${musl_src}" ]]; then
        echo "ERROR: musl source directory not found: ${musl_src}"
        return 1
    fi
    if [[ ! -d "${overlay_dir}/${arch}" ]]; then
        echo "ERROR: overlay directory not found: ${overlay_dir}/${arch}"
        return 1
    fi

    local arch_src="${overlay_dir}/${arch}"
    local musl_arch_dir="${musl_src}/arch/${arch}"

    echo "Applying xv6 ${arch} overlay to ${musl_src}..."

    # ── bits/ directory overlay ──────────────────────────────────────────
    # Copy all files from overlay bits/ to musl arch bits/
    if [[ -d "${arch_src}/bits" ]]; then
        mkdir -p "${musl_arch_dir}/bits"
        for f in "${arch_src}/bits/"*; do
            [[ -f "$f" ]] || continue
            cp "$f" "${musl_arch_dir}/bits/"
        done
    fi

    # ── Top-level arch file overlays ─────────────────────────────────────
    # Copy kstat.h, syscall_arch.h, crt_arch.h, etc. if present in overlay
    for f in kstat.h syscall_arch.h crt_arch.h pthread_arch.h reloc.h atomic_arch.h; do
        if [[ -f "${arch_src}/${f}" ]]; then
            cp "${arch_src}/${f}" "${musl_arch_dir}/${f}"
        fi
    done

    # ── clone.s override ─────────────────────────────────────────────────
    # xv6 clone takes a struct pointer, not individual registers.
    # File must be named clone.s (not __clone.s) for musl's REPLACED_OBJS.
    if [[ -f "${arch_src}/clone.s" ]]; then
        mkdir -p "${musl_src}/src/thread/${arch}"
        cp "${arch_src}/clone.s" "${musl_src}/src/thread/${arch}/clone.s"

        # Remove upstream __clone.s if present (x86_64 has one)
        rm -f "${musl_src}/src/thread/${arch}/__clone.s"
    fi

    # ── __unmapself.s override ───────────────────────────────────────────
    # Upstream musl hardcodes Linux syscall numbers in this assembly file.
    # xv6 uses custom numbering (e.g. SYS_munmap=51, SYS_exit=3 instead
    # of Linux's 11 and 60), so we must replace it.
    if [[ -f "${arch_src}/__unmapself.s" ]]; then
        mkdir -p "${musl_src}/src/thread/${arch}"
        cp "${arch_src}/__unmapself.s" "${musl_src}/src/thread/${arch}/__unmapself.s"
    fi

    # ── vfork.s override ─────────────────────────────────────────────────
    # Use clone with CLONE_VM|CLONE_VFORK|SIGCHLD (no SYS_vfork on riscv64)
    if [[ -f "${arch_src}/vfork.s" ]]; then
        mkdir -p "${musl_src}/src/process/${arch}"
        cp "${arch_src}/vfork.s" "${musl_src}/src/process/${arch}/vfork.s"
    fi

    # ── alltypes.h.in override ───────────────────────────────────────────
    if [[ -f "${arch_src}/bits/alltypes.h.in" ]]; then
        cp "${arch_src}/bits/alltypes.h.in" "${musl_arch_dir}/bits/alltypes.h.in"
    fi

    # ── brk enabled — musl malloc can use brk for small allocations ──────
    # The kernel reserves HEAP_RESERVE_PAGES (256 MiB) above the heap start
    # so mmap never collides with the brk region.
    # No patching needed — keep musl's default brk() definition.

    # ── Remove brk guard page — MAP_FIXED inside the heap VMA splits it
    # and breaks sbrk() for programs using the K&R allocator (umalloc.c).
    local malloc_c="${musl_src}/src/malloc/mallocng/malloc.c"
    if [[ -f "${malloc_c}" ]]; then
        sed -i '/need_guard.*mmap.*ctx\.brk.*pagesize/,/MAP_FIXED.*-1.*0);/{
            s/.*if (need_guard) mmap.*/\t\t\t\t\/* xv6: skip guard page *\//
            /MAP_ANON.*MAP_PRIVATE.*MAP_FIXED/d
        }' "${malloc_c}"
    fi

    echo "xv6 ${arch} overlay applied."
}

# If this script is executed directly (not sourced), print usage
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    if [[ $# -lt 3 ]]; then
        echo "Usage: $0 <arch> <musl_src_dir> <overlay_dir>"
        echo "  arch:        riscv64 | x86_64"
        echo "  musl_src_dir: path to extracted musl source"
        echo "  overlay_dir:  path to user/musl-xv6/arch/"
        exit 1
    fi
    apply_xv6_overlay "$1" "$2" "$3"
fi
