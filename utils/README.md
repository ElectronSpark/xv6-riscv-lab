# Host Utilities

## xv6fuse

`xv6fuse` is a [FUSE](https://github.com/libfuse/libfuse) filesystem adapter
that allows mounting an xv6 `fs.img` on the host Linux system. It attempts a
read-write mapping when possible and automatically falls back to read-only if
the image is not writable.

### Build

```sh
gcc -Wall -Wextra -O2 utils/xv6fuse.c -o utils/xv6fuse \
    $(pkg-config --cflags --libs fuse3)
```

> **Dependencies:** Install the development headers for FUSE 3
> (`libfuse3-dev` on Debian/Ubuntu, `fuse3-devel` on Fedora/RHEL).

### Usage

```sh
utils/xv6fuse --image=build/fs.img <mountpoint>
```

Use `--readonly` to force a read-only mount even when the image is writable.
Unmount with `fusermount3 -u <mountpoint>` when done.

Capabilities:

- Create and delete regular files, and edit their contents from the host.
- Create directories and populate them with host-managed entries.
- Create and read symbolic links (up to 10 chained resolutions).
- Automatically allocates and frees data blocks as files grow and shrink.

Limitations:

- Directory removal and renaming remain unsupported.
- Extending files via `truncate` to lengths larger than their current size is
    not yet implemented.
