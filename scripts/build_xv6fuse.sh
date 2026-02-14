#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 4 ]]; then
  echo "usage: $0 <out> <xv6fuse.c> <rbtree.c> <utils_include_dir>" >&2
  exit 2
fi

OUT="$1"
XV6FUSE_C="$2"
RBTREE_C="$3"
UTILS_INC="$4"

CFLAGS=(-Wall -Wextra -O2 -I"$UTILS_INC")
LIBS=()

if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists fuse3; then
  while IFS= read -r tok; do
    [[ -n "$tok" ]] && CFLAGS+=("$tok")
  done < <(pkg-config --cflags fuse3 | xargs -n1 echo)
  while IFS= read -r tok; do
    [[ -n "$tok" ]] && LIBS+=("$tok")
  done < <(pkg-config --libs fuse3 | xargs -n1 echo)
else
  CFLAGS+=("-I/usr/include/fuse3")
  LIBS+=("-lfuse3" "-pthread")
fi

gcc "${CFLAGS[@]}" "$XV6FUSE_C" "$RBTREE_C" -o "$OUT" "${LIBS[@]}"
