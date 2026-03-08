#!/bin/bash
# Cross-compilation pkg-config wrapper for xv6.
#
# Redirects pkg-config queries into the cross sysroot so that -I/-L paths
# returned by .pc files are relative to the target file system.
#
# Usage:
#   XV6_SYSROOT=/path/to/sysroot  xv6-pkg-config.sh [pkg-config args...]
#
# The wrapper is referenced from the meson cross-file and passed as the
# pkg-config binary, ensuring that meson finds target-specific libraries
# (e.g. OpenBLAS) instead of host ones.

set -euo pipefail

if [ -z "${XV6_SYSROOT:-}" ]; then
    echo "xv6-pkg-config: XV6_SYSROOT is not set" >&2
    exit 1
fi

export PKG_CONFIG_SYSROOT_DIR="${XV6_SYSROOT}"
export PKG_CONFIG_LIBDIR="${XV6_SYSROOT}/lib/pkgconfig:${XV6_SYSROOT}/share/pkgconfig"
export PKG_CONFIG_PATH=""

exec pkg-config "$@"
