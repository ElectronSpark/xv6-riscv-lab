#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 4 ]]; then
  echo "usage: $0 <xv6fuse> <image> <readme> <files...>" >&2
  exit 2
fi

XV6FUSE_BIN="$1"
IMAGE_PATH="$2"
README_PATH="$3"
shift 3

if [[ ! -x "$XV6FUSE_BIN" ]]; then
  echo "xv6fuse binary not executable: $XV6FUSE_BIN" >&2
  exit 1
fi

if [[ ! -f "$IMAGE_PATH" ]]; then
  echo "image not found: $IMAGE_PATH" >&2
  exit 1
fi

if [[ ! -f "$README_PATH" ]]; then
  echo "README not found: $README_PATH" >&2
  exit 1
fi

MOUNT_DIR="$(mktemp -d)"
FUSE_LOG="$(mktemp)"
FUSE_PID=""

cleanup() {
  set +e
  if mountpoint -q "$MOUNT_DIR"; then
    if command -v fusermount3 >/dev/null 2>&1; then
      fusermount3 -u "$MOUNT_DIR"
    elif command -v fusermount >/dev/null 2>&1; then
      fusermount -u "$MOUNT_DIR"
    fi
  fi

  if [[ -n "$FUSE_PID" ]] && kill -0 "$FUSE_PID" >/dev/null 2>&1; then
    kill "$FUSE_PID" >/dev/null 2>&1 || true
    wait "$FUSE_PID" >/dev/null 2>&1 || true
  fi

  rm -f "$FUSE_LOG"
  rmdir "$MOUNT_DIR" >/dev/null 2>&1 || true
}
trap cleanup EXIT

"$XV6FUSE_BIN" --image="$IMAGE_PATH" -f "$MOUNT_DIR" >"$FUSE_LOG" 2>&1 &
FUSE_PID=$!

for _ in $(seq 1 100); do
  if mountpoint -q "$MOUNT_DIR"; then
    break
  fi
  sleep 0.05
done

if ! mountpoint -q "$MOUNT_DIR"; then
  cat "$FUSE_LOG" >&2 || true
  echo "failed to mount image via xv6fuse" >&2
  exit 1
fi

mkdir -p "$MOUNT_DIR/bin"
cp "$README_PATH" "$MOUNT_DIR/README.md"

for src in "$@"; do
  if [[ ! -e "$src" ]]; then
    echo "skipping missing input: $src" >&2
    continue
  fi

  base="$(basename "$src")"
  name="${base#_}"

  if [[ -d "$src" ]]; then
    cp -a "$src" "$MOUNT_DIR/bin/$name"
  else
    cp "$src" "$MOUNT_DIR/bin/$name"
    chmod 0755 "$MOUNT_DIR/bin/$name" || true
  fi
done

sync
