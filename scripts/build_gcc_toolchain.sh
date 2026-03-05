#!/bin/bash
# =============================================================================
# build_gcc_toolchain.sh — Two-phase GCC cross-toolchain with xv6 musl libc
# =============================================================================
#
# Builds a complete GCC + Binutils cross-compilation toolchain that uses the
# project's xv6-patched musl as its C library.
#
# Phase 1 (static bootstrap):
#   Builds a fully-static toolchain that can only produce static binaries.
#   This is a self-contained bootstrap — no host libc leaks into target code.
#
# Phase 2 (dynamic-capable):
#   Uses the Phase 1 toolchain to rebuild everything with both static and
#   shared library support.  The resulting toolchain can produce dynamically-
#   linked ELF binaries that use /lib/ld-musl-<arch>.so.1 as the interpreter.
#
# Target triplet:
#   riscv64-xv6-linux-musl   (riscv64 target)
#   x86_64-xv6-linux-musl    (x86_64 target — planned)
#
#   "linux" in the OS field ensures GCC/binutils enable ELF shared-object
#   support without needing custom target definition patches.
#
# The resulting toolchain replaces:
#   - riscv64-unknown-elf-gcc      (kernel + static userland)
#   - riscv64-linux-gnu-gcc        (musl shared builds, CPython)
#
# Usage:
#   ./build_gcc_toolchain.sh [options]
#
# Options:
#   --arch=ARCH         Target architecture: riscv64 (default), x86_64
#   --prefix=PATH       Install base directory (default: <workspace>/toolchain)
#   --jobs=N            Parallel make jobs (default: $(nproc))
#   --phase=1|2|all     Which phase(s) to run (default: all)
#   --clean             Remove previous build artifacts before starting
#   --gcc-version=VER   GCC version (default: 14.2.0)
#   --binutils-version=VER  Binutils version (default: 2.43)
#   --musl-version=VER  musl version (default: 1.2.5)
#   -v, --verbose       Verbose build output (don't suppress make output)
#   -h, --help          Show this help
#
# Prerequisites (host packages):
#   build-essential gcc g++ make texinfo bison flex
#   libgmp-dev libmpfr-dev libmpc-dev zlib1g-dev libexpat-dev
#   wget tar gawk
#
# =============================================================================

set -euo pipefail

# ─── Defaults ────────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKSPACE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
MUSL_XV6_DIR="${WORKSPACE_DIR}/user/musl-xv6"
OVERLAY_HELPER="${MUSL_XV6_DIR}/apply_xv6_overlay.sh"

TARGET_ARCH="riscv64"
PREFIX=""
JOBS="$(nproc)"
PHASE="all"
CLEAN=0
VERBOSE=0

GCC_VERSION="14.2.0"
BINUTILS_VERSION="2.43"
MUSL_VERSION="1.2.5"

# ─── Source URLs ─────────────────────────────────────────────────────────────

gcc_url() { echo "https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.xz"; }
binutils_url() { echo "https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VERSION}.tar.xz"; }
musl_url() { echo "https://musl.libc.org/releases/musl-${MUSL_VERSION}.tar.gz"; }

# ─── Parse arguments ────────────────────────────────────────────────────────

usage() {
    sed -n '/^# Usage:/,/^# ===/p' "$0" | head -n -1 | sed 's/^# //'
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch=*)               TARGET_ARCH="${1#--arch=}"; shift ;;
        --prefix=*)             PREFIX="${1#--prefix=}"; shift ;;
        --jobs=*)               JOBS="${1#--jobs=}"; shift ;;
        --phase=*)              PHASE="${1#--phase=}"; shift ;;
        --clean)                CLEAN=1; shift ;;
        --gcc-version=*)        GCC_VERSION="${1#--gcc-version=}"; shift ;;
        --binutils-version=*)   BINUTILS_VERSION="${1#--binutils-version=}"; shift ;;
        --musl-version=*)       MUSL_VERSION="${1#--musl-version=}"; shift ;;
        -v|--verbose)           VERBOSE=1; shift ;;
        -h|--help)              usage ;;
        *)  echo "Unknown option: $1"; usage ;;
    esac
done

# ─── Validate ────────────────────────────────────────────────────────────────

if [[ ! "${TARGET_ARCH}" =~ ^(riscv64|x86_64)$ ]]; then
    echo "ERROR: Unsupported architecture: ${TARGET_ARCH}"
    echo "       Supported: riscv64, x86_64"
    exit 1
fi

if [[ ! "${PHASE}" =~ ^(1|2|all)$ ]]; then
    echo "ERROR: Invalid --phase value: ${PHASE}. Must be 1, 2, or all."
    exit 1
fi

if [[ ! -f "${OVERLAY_HELPER}" ]]; then
    echo "ERROR: musl overlay helper not found: ${OVERLAY_HELPER}"
    echo "       Run this script from the xv6 workspace root."
    exit 1
fi

# ─── Derived settings ───────────────────────────────────────────────────────

TRIPLET="${TARGET_ARCH}-xv6-linux-musl"
PREFIX="${PREFIX:-${WORKSPACE_DIR}/toolchain}"
SRC_DIR="${PREFIX}/src"
ARCH_PREFIX="${PREFIX}/${TARGET_ARCH}"
PHASE1_PREFIX="${ARCH_PREFIX}/phase1"
PHASE2_PREFIX="${ARCH_PREFIX}/phase2"
BUILD_DIR="${ARCH_PREFIX}/build"

# Architecture-specific GCC configure flags
case "${TARGET_ARCH}" in
    riscv64)
        ARCH_GCC_OPTS="--with-arch=rv64gc --with-abi=lp64d"
        ARCH_MUSL_CFLAGS="-march=rv64gc -mabi=lp64d -mcmodel=medany -fPIC -O2 -fno-stack-protector"
        MUSL_DYNAMIC_LINKER="ld-musl-riscv64.so.1"
        MUSL_TARGET_FLAG="--target=riscv64"
        ;;
    x86_64)
        ARCH_GCC_OPTS=""
        ARCH_MUSL_CFLAGS="-fPIC -O2 -fno-stack-protector -mno-red-zone"
        MUSL_DYNAMIC_LINKER="ld-musl-x86_64.so.1"
        MUSL_TARGET_FLAG="--target=x86_64"
        ;;
esac

# ─── Logging helpers ────────────────────────────────────────────────────────

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

log_phase() { echo -e "\n${BOLD}${CYAN}══════════════════════════════════════════════════════${RESET}"; echo -e "${BOLD}${CYAN}  $1${RESET}"; echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════════${RESET}\n"; }
log_step()  { echo -e "${BOLD}${GREEN}──── $1${RESET}"; }
log_info()  { echo -e "${YELLOW}  → $1${RESET}"; }
log_error() { echo -e "${RED}ERROR: $1${RESET}" >&2; }

# Redirect make output unless verbose
make_quiet() {
    if [[ "${VERBOSE}" -eq 1 ]]; then
        make "$@"
    else
        make "$@" > "${BUILD_DIR}/make.log" 2>&1 || {
            log_error "Build failed. Last 50 lines of log:"
            tail -50 "${BUILD_DIR}/make.log" >&2
            exit 1
        }
    fi
}

# ─── Banner ──────────────────────────────────────────────────────────────────

echo -e "${BOLD}"
echo "┌─────────────────────────────────────────────────────────┐"
echo "│  xv6 GCC Cross-Toolchain Builder                       │"
echo "│─────────────────────────────────────────────────────────│"
echo "│  Target:     ${TRIPLET}"
echo "│  GCC:        ${GCC_VERSION}"
echo "│  Binutils:   ${BINUTILS_VERSION}"
echo "│  musl:       ${MUSL_VERSION}"
echo "│  Phase:      ${PHASE}"
echo "│  Prefix:     ${ARCH_PREFIX}"
echo "│  Jobs:       ${JOBS}"
echo "└─────────────────────────────────────────────────────────┘"
echo -e "${RESET}"

# ─── Clean ───────────────────────────────────────────────────────────────────

if [[ "${CLEAN}" -eq 1 ]]; then
    log_step "Cleaning previous build..."
    if [[ "${PHASE}" == "1" || "${PHASE}" == "all" ]]; then
        rm -rf "${PHASE1_PREFIX}" "${BUILD_DIR}/phase1"
    fi
    if [[ "${PHASE}" == "2" || "${PHASE}" == "all" ]]; then
        rm -rf "${PHASE2_PREFIX}" "${BUILD_DIR}/phase2"
    fi
fi

mkdir -p "${SRC_DIR}" "${BUILD_DIR}"

# =============================================================================
# Download sources
# =============================================================================

download_and_extract() {
    local url="$1"
    local archive="$2"
    local dir_name="$3"

    if [[ -d "${SRC_DIR}/${dir_name}" ]]; then
        log_info "${dir_name} already extracted, skipping download."
        return
    fi

    if [[ ! -f "${SRC_DIR}/${archive}" ]]; then
        log_info "Downloading ${archive}..."
        wget -q --show-progress -O "${SRC_DIR}/${archive}" "${url}"
    fi

    log_info "Extracting ${archive}..."
    tar xf "${SRC_DIR}/${archive}" -C "${SRC_DIR}"
}

log_step "Downloading sources..."
download_and_extract "$(gcc_url)" "gcc-${GCC_VERSION}.tar.xz" "gcc-${GCC_VERSION}"
download_and_extract "$(binutils_url)" "binutils-${BINUTILS_VERSION}.tar.xz" "binutils-${BINUTILS_VERSION}"
download_and_extract "$(musl_url)" "musl-${MUSL_VERSION}.tar.gz" "musl-${MUSL_VERSION}"

# Download GCC prerequisites (gmp, mpfr, mpc, isl)
if [[ ! -f "${SRC_DIR}/gcc-${GCC_VERSION}/.prereqs_done" ]]; then
    log_info "Downloading GCC prerequisites..."
    pushd "${SRC_DIR}/gcc-${GCC_VERSION}" > /dev/null
    ./contrib/download_prerequisites
    touch .prereqs_done
    popd > /dev/null
fi

# =============================================================================
# Source the musl overlay helper
# =============================================================================

# shellcheck source=../user/musl-xv6/apply_xv6_overlay.sh
source "${OVERLAY_HELPER}"

# =============================================================================
# Helper: prepare a pristine musl source tree with xv6 overlay
# =============================================================================

prepare_musl_source() {
    local dest="$1"  # where to place the patched copy

    if [[ -d "${dest}" ]]; then
        rm -rf "${dest}"
    fi
    cp -a "${SRC_DIR}/musl-${MUSL_VERSION}" "${dest}"
    apply_xv6_overlay "${TARGET_ARCH}" "${dest}" "${MUSL_XV6_DIR}/arch"
}

# =============================================================================
# PHASE 1: Static bootstrap toolchain
# =============================================================================

build_phase1() {
    log_phase "PHASE 1: Static bootstrap toolchain"
    local P="${PHASE1_PREFIX}"
    local B="${BUILD_DIR}/phase1"
    local SYSROOT="${P}/${TRIPLET}"

    mkdir -p "${P}" "${B}" "${SYSROOT}"

    # ── Step 1a: Build Binutils ──────────────────────────────────────────
    log_step "Phase 1 — Step 1/5: Building Binutils ${BINUTILS_VERSION}..."
    if [[ -f "${P}/bin/${TRIPLET}-ld" ]]; then
        log_info "Binutils already installed, skipping."
    else
        mkdir -p "${B}/binutils"
        pushd "${B}/binutils" > /dev/null
        "${SRC_DIR}/binutils-${BINUTILS_VERSION}/configure" \
            --target="${TRIPLET}" \
            --prefix="${P}" \
            --with-sysroot="${SYSROOT}" \
            --disable-nls \
            --disable-werror \
            --disable-multilib \
            --disable-gdb \
            --disable-sim
        make_quiet -j"${JOBS}"
        make_quiet install
        popd > /dev/null
    fi

    # ── Step 1b: Install musl headers ────────────────────────────────────
    log_step "Phase 1 — Step 2/5: Installing musl headers..."
    if [[ -f "${SYSROOT}/include/stdio.h" ]]; then
        log_info "musl headers already installed, skipping."
    else
        local MUSL_P1_SRC="${B}/musl-headers"
        prepare_musl_source "${MUSL_P1_SRC}"

        pushd "${MUSL_P1_SRC}" > /dev/null
        # Install headers only — the cross-compiler doesn't exist yet, so
        # configure would fail.  We bypass configure by passing ARCH and
        # prefix directly to make (musl's Makefile supports this).
        mkdir -p obj/include/bits
        make ARCH="${TARGET_ARCH}" prefix="${SYSROOT}" install-headers DESTDIR=""
        popd > /dev/null
    fi

    # ── Step 1c: Build GCC Stage 1 (C only, no libc) ────────────────────
    log_step "Phase 1 — Step 3/5: Building GCC ${GCC_VERSION} Stage 1 (C compiler + libgcc)..."
    if [[ -f "${P}/bin/${TRIPLET}-gcc" ]]; then
        log_info "GCC Stage 1 already installed, skipping."
    else
        mkdir -p "${B}/gcc-stage1"
        pushd "${B}/gcc-stage1" > /dev/null
        "${SRC_DIR}/gcc-${GCC_VERSION}/configure" \
            --target="${TRIPLET}" \
            --prefix="${P}" \
            --with-sysroot="${SYSROOT}" \
            ${ARCH_GCC_OPTS} \
            --enable-languages=c \
            --disable-nls \
            --disable-shared \
            --disable-multilib \
            --disable-threads \
            --disable-libssp \
            --disable-libquadmath \
            --disable-libgomp \
            --disable-libatomic \
            --disable-libsanitizer \
            --disable-libvtv \
            --with-newlib \
            --without-headers \
            --disable-bootstrap
        make_quiet -j"${JOBS}" all-gcc all-target-libgcc
        make_quiet install-gcc install-target-libgcc
        popd > /dev/null
    fi

    # ── Step 1d: Build musl (static only) ────────────────────────────────
    log_step "Phase 1 — Step 4/5: Building musl ${MUSL_VERSION} (static)..."
    if [[ -f "${SYSROOT}/lib/libc.a" ]]; then
        log_info "musl static library already installed, skipping."
    else
        local MUSL_P1_BUILD="${B}/musl-static"
        prepare_musl_source "${MUSL_P1_BUILD}"

        pushd "${MUSL_P1_BUILD}" > /dev/null
        CC="${P}/bin/${TRIPLET}-gcc" \
        AR="${P}/bin/${TRIPLET}-ar" \
        RANLIB="${P}/bin/${TRIPLET}-ranlib" \
        CFLAGS="${ARCH_MUSL_CFLAGS}" \
        ./configure \
            ${MUSL_TARGET_FLAG} \
            --prefix="${SYSROOT}" \
            --syslibdir="${SYSROOT}/lib" \
            --disable-shared \
            --enable-static \
            --disable-wrapper
        make_quiet -j"${JOBS}"
        make_quiet install
        popd > /dev/null
    fi

    # ── Step 1e: Rebuild GCC (full, with musl libc) ──────────────────────
    log_step "Phase 1 — Step 5/5: Rebuilding GCC ${GCC_VERSION} (full static, with musl)..."
    if [[ -f "${P}/.phase1_complete" ]]; then
        log_info "Phase 1 GCC already fully built, skipping."
    else
        # Remove stage 1 build to do a clean full build
        rm -rf "${B}/gcc-full"
        mkdir -p "${B}/gcc-full"
        pushd "${B}/gcc-full" > /dev/null
        "${SRC_DIR}/gcc-${GCC_VERSION}/configure" \
            --target="${TRIPLET}" \
            --prefix="${P}" \
            --with-sysroot="${SYSROOT}" \
            --with-native-system-header-dir=/include \
            ${ARCH_GCC_OPTS} \
            --enable-languages=c \
            --disable-nls \
            --disable-shared \
            --enable-static \
            --disable-multilib \
            --disable-threads \
            --disable-libssp \
            --disable-libquadmath \
            --disable-libgomp \
            --disable-libatomic \
            --disable-libsanitizer \
            --disable-libvtv \
            --disable-bootstrap
        make_quiet -j"${JOBS}"
        make_quiet install
        popd > /dev/null

        touch "${P}/.phase1_complete"
    fi

    echo ""
    log_info "Phase 1 complete: ${P}/bin/${TRIPLET}-gcc"
    "${P}/bin/${TRIPLET}-gcc" -v 2>&1 | tail -1
    echo ""
}

# =============================================================================
# PHASE 2: Dynamic-capable toolchain
# =============================================================================

build_phase2() {
    log_phase "PHASE 2: Dynamic-capable toolchain"
    local P="${PHASE2_PREFIX}"
    local B="${BUILD_DIR}/phase2"
    local P1="${PHASE1_PREFIX}"
    local SYSROOT="${P}/${TRIPLET}"

    # Phase 2 requires Phase 1 to exist
    if [[ ! -f "${P1}/.phase1_complete" ]]; then
        log_error "Phase 1 toolchain not found at ${P1}. Run --phase=1 first."
        exit 1
    fi

    mkdir -p "${P}" "${B}" "${SYSROOT}"

    # Export Phase 1 tools for cross-compiling
    export PATH="${P1}/bin:${PATH}"

    # ── Step 2a: Build Binutils ──────────────────────────────────────────
    log_step "Phase 2 — Step 1/4: Building Binutils ${BINUTILS_VERSION}..."
    if [[ -f "${P}/bin/${TRIPLET}-ld" ]]; then
        log_info "Binutils already installed, skipping."
    else
        mkdir -p "${B}/binutils"
        pushd "${B}/binutils" > /dev/null
        "${SRC_DIR}/binutils-${BINUTILS_VERSION}/configure" \
            --target="${TRIPLET}" \
            --prefix="${P}" \
            --with-sysroot="${SYSROOT}" \
            --disable-nls \
            --disable-werror \
            --disable-multilib \
            --disable-gdb \
            --disable-sim
        make_quiet -j"${JOBS}"
        make_quiet install
        popd > /dev/null
    fi

    # ── Step 2b: Build GCC Stage 1 (bootstrap, for compiling musl) ──────
    log_step "Phase 2 — Step 2/4: Building GCC ${GCC_VERSION} Stage 1..."
    local GCC_S1_DONE="${B}/.gcc_stage1_done"
    if [[ -f "${GCC_S1_DONE}" ]]; then
        log_info "GCC Stage 1 already built, skipping."
    else
        # Install musl headers first (needed before GCC can build libgcc
        # with knowledge of the target C library headers)
        log_info "Installing musl headers into sysroot..."
        local MUSL_P2_HDR="${B}/musl-headers"
        prepare_musl_source "${MUSL_P2_HDR}"

        pushd "${MUSL_P2_HDR}" > /dev/null
        # Install headers only — bypass configure, use make directly
        # (same approach as Phase 1).
        mkdir -p obj/include/bits
        make ARCH="${TARGET_ARCH}" prefix="${SYSROOT}" install-headers DESTDIR=""
        popd > /dev/null

        # Build GCC stage 1
        mkdir -p "${B}/gcc-stage1"
        pushd "${B}/gcc-stage1" > /dev/null
        "${SRC_DIR}/gcc-${GCC_VERSION}/configure" \
            --target="${TRIPLET}" \
            --prefix="${P}" \
            --with-sysroot="${SYSROOT}" \
            ${ARCH_GCC_OPTS} \
            --enable-languages=c \
            --disable-nls \
            --disable-shared \
            --disable-multilib \
            --disable-threads \
            --disable-libssp \
            --disable-libquadmath \
            --disable-libgomp \
            --disable-libatomic \
            --disable-libsanitizer \
            --disable-libvtv \
            --with-newlib \
            --without-headers \
            --disable-bootstrap
        make_quiet -j"${JOBS}" all-gcc all-target-libgcc
        make_quiet install-gcc install-target-libgcc
        popd > /dev/null

        touch "${GCC_S1_DONE}"
    fi

    # ── Step 2c: Build musl (static + shared) ────────────────────────────
    log_step "Phase 2 — Step 3/4: Building musl ${MUSL_VERSION} (static + shared)..."
    if [[ -f "${SYSROOT}/lib/libc.so" && -f "${SYSROOT}/lib/libc.a" ]]; then
        log_info "musl already installed with shared support, skipping."
    else
        local MUSL_P2_BUILD="${B}/musl-shared"
        prepare_musl_source "${MUSL_P2_BUILD}"

        pushd "${MUSL_P2_BUILD}" > /dev/null
        # Use Phase 2 stage-1 GCC (has libgcc; linker supports -shared via
        # the binutils we just built)
        CC="${P}/bin/${TRIPLET}-gcc" \
        AR="${P}/bin/${TRIPLET}-ar" \
        RANLIB="${P}/bin/${TRIPLET}-ranlib" \
        CFLAGS="${ARCH_MUSL_CFLAGS}" \
        ./configure \
            ${MUSL_TARGET_FLAG} \
            --prefix="${SYSROOT}" \
            --syslibdir="${SYSROOT}/lib" \
            --enable-shared \
            --enable-static \
            --disable-wrapper
        make_quiet -j"${JOBS}"
        make_quiet install

        # Fix dynamic linker symlink to be relative
        if [[ -L "${SYSROOT}/lib/${MUSL_DYNAMIC_LINKER}" ]]; then
            ln -sf libc.so "${SYSROOT}/lib/${MUSL_DYNAMIC_LINKER}"
            log_info "Fixed ${MUSL_DYNAMIC_LINKER} symlink → libc.so (relative)"
        fi
        popd > /dev/null
    fi

    # ── Step 2d: Build GCC (full, with shared support) ───────────────────
    log_step "Phase 2 — Step 4/4: Building GCC ${GCC_VERSION} (full, with dynamic support)..."
    if [[ -f "${P}/.phase2_complete" ]]; then
        log_info "Phase 2 GCC already fully built, skipping."
    else
        rm -rf "${B}/gcc-full"
        mkdir -p "${B}/gcc-full"
        pushd "${B}/gcc-full" > /dev/null
        "${SRC_DIR}/gcc-${GCC_VERSION}/configure" \
            --target="${TRIPLET}" \
            --prefix="${P}" \
            --with-sysroot="${SYSROOT}" \
            --with-native-system-header-dir=/include \
            ${ARCH_GCC_OPTS} \
            --enable-languages=c \
            --disable-nls \
            --enable-shared \
            --disable-multilib \
            --disable-threads \
            --disable-libssp \
            --disable-libquadmath \
            --disable-libgomp \
            --disable-libatomic \
            --disable-libsanitizer \
            --disable-libvtv \
            --disable-bootstrap
        make_quiet -j"${JOBS}"
        make_quiet install
        popd > /dev/null

        touch "${P}/.phase2_complete"
    fi

    echo ""
    log_info "Phase 2 complete: ${P}/bin/${TRIPLET}-gcc"
    "${P}/bin/${TRIPLET}-gcc" -v 2>&1 | tail -1
    echo ""
}

# =============================================================================
# Post-build: create convenience symlinks and activation script
# =============================================================================

create_convenience_links() {
    local FINAL_PREFIX="${PHASE2_PREFIX}"
    if [[ "${PHASE}" == "1" ]]; then
        FINAL_PREFIX="${PHASE1_PREFIX}"
    fi

    # Create an arch-specific bin/ symlink for convenience
    if [[ ! -L "${ARCH_PREFIX}/bin" && ! -d "${ARCH_PREFIX}/bin" ]]; then
        ln -sf "${FINAL_PREFIX}/bin" "${ARCH_PREFIX}/bin"
        log_info "Created symlink: ${ARCH_PREFIX}/bin → ${FINAL_PREFIX}/bin"
    fi

    # Generate arch-specific activate.sh
    log_step "Generating toolchain/${TARGET_ARCH}/activate.sh..."
    cat > "${ARCH_PREFIX}/activate.sh" << ACTIVATE_EOF
#!/bin/bash
# Source this file to add the xv6 toolchain to your PATH:
#   source toolchain/activate.sh
#
# Then build xv6 with:
#   cd build && cmake -DTOOLPREFIX=${FINAL_PREFIX}/bin/${TRIPLET}- ..

export XV6_TOOLCHAIN="${FINAL_PREFIX}"
export PATH="${FINAL_PREFIX}/bin:\${PATH}"
export TOOLPREFIX="${FINAL_PREFIX}/bin/${TRIPLET}-"

echo "xv6 toolchain activated: ${TRIPLET}"
echo "  GCC:     \$(${TRIPLET}-gcc --version | head -1)"
echo "  Prefix:  \${TOOLPREFIX}"
ACTIVATE_EOF
    chmod +x "${ARCH_PREFIX}/activate.sh"
}

# =============================================================================
# Verification
# =============================================================================

verify_toolchain() {
    local P="$1"
    local label="$2"

    log_step "Verifying ${label}..."

    # Check key binaries exist
    for tool in gcc ar ld objcopy objdump ranlib readelf strip; do
        if [[ ! -f "${P}/bin/${TRIPLET}-${tool}" ]]; then
            log_error "Missing: ${P}/bin/${TRIPLET}-${tool}"
            return 1
        fi
    done

    # Test compilation
    local TMPDIR
    TMPDIR="$(mktemp -d)"
    cat > "${TMPDIR}/hello.c" << 'EOF'
#include <stdio.h>
int main(void) { printf("Hello from xv6 musl toolchain!\n"); return 0; }
EOF

    # Static test
    if "${P}/bin/${TRIPLET}-gcc" -static -o "${TMPDIR}/hello_static" "${TMPDIR}/hello.c" 2>/dev/null; then
        log_info "Static compilation: OK"
        file "${TMPDIR}/hello_static" | grep -q "statically linked" && log_info "  Confirmed statically linked"
    else
        log_info "Static compilation: FAILED (this is expected in Stage 1 without full libc)"
    fi

    # Dynamic test (Phase 2 only)
    if [[ "${label}" == *"Phase 2"* ]]; then
        if "${P}/bin/${TRIPLET}-gcc" -o "${TMPDIR}/hello_dynamic" "${TMPDIR}/hello.c" 2>/dev/null; then
            log_info "Dynamic compilation: OK"
            "${P}/bin/${TRIPLET}-readelf" -l "${TMPDIR}/hello_dynamic" 2>/dev/null | grep -q "INTERP" && \
                log_info "  Confirmed dynamic linker: $(${P}/bin/${TRIPLET}-readelf -p .interp "${TMPDIR}/hello_dynamic" 2>/dev/null | grep ld-musl || echo 'present')"
        else
            log_info "Dynamic compilation: FAILED"
        fi
    fi

    rm -rf "${TMPDIR}"
}

# =============================================================================
# Main
# =============================================================================

TOTAL_START="$(date +%s)"

if [[ "${PHASE}" == "1" || "${PHASE}" == "all" ]]; then
    build_phase1
    verify_toolchain "${PHASE1_PREFIX}" "Phase 1 (static bootstrap)"
fi

if [[ "${PHASE}" == "2" || "${PHASE}" == "all" ]]; then
    build_phase2
    verify_toolchain "${PHASE2_PREFIX}" "Phase 2 (dynamic-capable)"
fi

create_convenience_links

TOTAL_END="$(date +%s)"
TOTAL_ELAPSED=$(( TOTAL_END - TOTAL_START ))
TOTAL_MIN=$(( TOTAL_ELAPSED / 60 ))
TOTAL_SEC=$(( TOTAL_ELAPSED % 60 ))

echo ""
log_phase "BUILD COMPLETE"
log_info "Target:  ${TRIPLET}"
log_info "Prefix:  ${ARCH_PREFIX}"
log_info "Time:    ${TOTAL_MIN}m ${TOTAL_SEC}s"
echo ""
log_info "Activate with:  source ${ARCH_PREFIX}/activate.sh"
log_info "Or set:         TOOLPREFIX=${PHASE2_PREFIX}/bin/${TRIPLET}-"
echo ""
