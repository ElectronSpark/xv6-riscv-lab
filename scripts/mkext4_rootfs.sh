#!/bin/bash
# mkext4_rootfs.sh - Create an ext4 root filesystem image from sysroot
#
# Usage: mkext4_rootfs.sh <output.img> <readme> <cpython_src_dir> <size_mb> <sysroot_dir>
#
# The entire sysroot tree is copied as the filesystem base.
# Additional runtime directories (dev, proc, tmp, sys) are created.
# Python standard library is copied from the cpython source tree.

set -e

OUTPUT="$1"
README="$2"
CPYTHON_SRC="$3"
SIZE_MB="${4:-64}"
SYSROOT_DIR="$5"

if [ -z "$OUTPUT" ] || [ -z "$README" ] || [ -z "$CPYTHON_SRC" ] || [ -z "$SYSROOT_DIR" ]; then
    echo "Usage: $0 <output.img> <readme> <cpython_src_dir> <size_mb> <sysroot_dir>" >&2
    exit 1
fi

STAGING=$(mktemp -d)
cleanup() {
    rm -rf "$STAGING"
}
trap cleanup EXIT

echo "mkext4_rootfs: staging directory: $STAGING"

# Copy the entire sysroot as the filesystem base, excluding static libraries
# and development headers (not needed at runtime, saves ~240MB)
echo "mkext4_rootfs: copying sysroot from $SYSROOT_DIR ..."
rsync -a --exclude='*.a' --exclude='*.o' --exclude='*.old' \
         --exclude='include/' --exclude='pkgconfig/' \
    "$SYSROOT_DIR/" "$STAGING/"
SYSROOT_SIZE=$(du -sh "$SYSROOT_DIR" | cut -f1)
STAGING_SIZE=$(du -sh "$STAGING" | cut -f1)
echo "mkext4_rootfs: sysroot: ${SYSROOT_SIZE} -> staging: ${STAGING_SIZE} (excludes .a/.o/include)"

# Ensure runtime directories exist (not part of sysroot)
mkdir -p "$STAGING/dev"
mkdir -p "$STAGING/proc"
mkdir -p "$STAGING/tmp"
mkdir -p "$STAGING/sys"
mkdir -p "$STAGING/etc"

# ── Network configuration (user-space DNS via musl) ──────────────────────────
# Fallback resolv.conf — init will overwrite with DHCP/config-derived DNS
# at boot. The .bak is a static safety net in case networking doesn't come up.
cat > "$STAGING/etc/resolv.conf.bak" <<'RESOLV'
nameserver 10.0.2.3
RESOLV
# Start with a minimal resolv.conf; init replaces it at boot.
cat > "$STAGING/etc/resolv.conf" <<'RESOLV'
nameserver 127.0.0.1
RESOLV

cat > "$STAGING/etc/hosts" <<'HOSTS'
127.0.0.1 localhost
HOSTS

# ── User database files (multi-user support) ────────────────────────────────
# /etc/passwd – root + guest + sshd accounts
cat > "$STAGING/etc/passwd" <<'PASSWD'
root:x:0:0:root:/root:/bin/sh
sshd:x:74:74:Privilege-separated SSH:/var/empty:/bin/false
guest:x:1000:1000:Guest User:/home/guest:/bin/sh
nobody:x:65534:65534:Nobody:/nonexistent:/bin/false
PASSWD

# /etc/group – root + guest + sshd groups
cat > "$STAGING/etc/group" <<'GROUP'
root:x:0:root
wheel:x:10:root
sshd:x:74:
guest:x:1000:guest
nogroup:x:65534:
GROUP

# /etc/shadow – password hashes (root has no password initially, guest locked)
# Use SHA-512 ($6$) placeholder. root has empty-password hash (just run passwd
# after boot to set one). guest is locked (! prefix).
# Field 3 (sp_lstchg) must be non-zero, otherwise login treats the password as
# expired and forces an immediate change.  Use a recent epoch-day count.
cat > "$STAGING/etc/shadow" <<'SHADOW'
root::20517:0:99999:7:::
sshd:!:20517:0:99999:7:::
guest:!:20517:0:99999:7:::
nobody:!:20517:0:99999:7:::
SHADOW
chmod 600 "$STAGING/etc/shadow" 2>/dev/null || true

# Create home directories
mkdir -p "$STAGING/root"
mkdir -p "$STAGING/home/guest"

# ── SSH server setup ─────────────────────────────────────────────────────────
# Create privilege-separation directory for sshd (must exist and be owned by root)
mkdir -p "$STAGING/var/empty"
mkdir -p "$STAGING/var/run"
chmod 0755 "$STAGING/var/empty"
mkdir -p "$STAGING/etc/ssh"

# Generate SSH host keys if ssh-keygen is available on the host
if command -v ssh-keygen >/dev/null 2>&1; then
    echo "mkext4_rootfs: generating SSH host keys..."
    ssh-keygen -t ed25519 -f "$STAGING/etc/ssh/ssh_host_ed25519_key" -N "" -q 2>/dev/null || true
    ssh-keygen -t rsa -b 2048 -f "$STAGING/etc/ssh/ssh_host_rsa_key" -N "" -q 2>/dev/null || true
    ssh-keygen -t ecdsa -b 256 -f "$STAGING/etc/ssh/ssh_host_ecdsa_key" -N "" -q 2>/dev/null || true
else
    echo "mkext4_rootfs: WARNING: ssh-keygen not found, host keys not generated" >&2
    echo "mkext4_rootfs:   Generate manually: ssh-keygen -A -f /path/to/sysroot" >&2
fi

# sshd_config — minimal configuration for xv6
cat > "$STAGING/etc/ssh/sshd_config" <<'SSHD_CONFIG'
# xv6 OpenSSH Server Configuration

# Listen on all interfaces, port 22
Port 22
ListenAddress 0.0.0.0

# Host keys
HostKey /etc/ssh/ssh_host_ed25519_key
HostKey /etc/ssh/ssh_host_rsa_key
HostKey /etc/ssh/ssh_host_ecdsa_key

# Authentication
PermitRootLogin yes
PasswordAuthentication yes
PermitEmptyPasswords yes
PubkeyAuthentication yes
AuthorizedKeysFile .ssh/authorized_keys

# Logging (goes to syslog → /dev/log, silently dropped if no daemon)
SyslogFacility AUTH
LogLevel INFO

# Disable features not supported on xv6
UseDNS no
PrintMotd no
Compression no
X11Forwarding no
GatewayPorts no
TCPKeepAlive yes

# Subsystems
Subsystem sftp /bin/sftp

# Allow env passing
AcceptEnv LANG LC_*

# Privilege separation chroot
ChrootDirectory none
SSHD_CONFIG

# ssh_config — client configuration
cat > "$STAGING/etc/ssh/ssh_config" <<'SSH_CONFIG'
# xv6 OpenSSH Client Configuration
Host *
    StrictHostKeyChecking no
    UserKnownHostsFile /dev/null
    LogLevel ERROR
SSH_CONFIG

# Create .ssh directory for root
mkdir -p "$STAGING/root/.ssh"
chmod 700 "$STAGING/root/.ssh" 2>/dev/null || true

# /etc/shells – valid login shells
cat > "$STAGING/etc/shells" <<'SHELLS'
/bin/sh
SHELLS

# ── /etc/daemons — programs started automatically by init ────────────────────
# Each line is a command (with optional arguments) run as a background daemon.
# init places each daemon in its own process group.
cat > "$STAGING/etc/daemons" <<'DAEMONS'
# xv6 daemon processes — started by init at boot
# Each line: /path/to/binary [args...]
# Lines starting with # are comments.

/bin/telnetd
/bin/sshd -D -e
DAEMONS

# ── lwIP network configuration ───────────────────────────────────────────────
# The kernel reads /etc/network.conf at boot to configure the network.
# mode=dhcp:   use DHCP (default if this file is missing)
# mode=static: use the ip/netmask/gateway below
cat > "$STAGING/etc/network.conf" <<'NETCONF'
# xv6 network configuration
# Supported keys: mode, ip, netmask, gateway, dns, hostname
#
# mode=dhcp        — obtain IP via DHCP (default)
# mode=static      — use static IP below
#
mode=dhcp
#ip=192.168.0.201
#netmask=255.255.255.0
#gateway=192.168.0.1
#dns=8.8.8.8
hostname=xv6
NETCONF

# Copy README to root
if [ -f "$README" ]; then
    cp "$README" "$STAGING/README.md"
fi

# Ensure ld-musl dynamic linker symlink is a *relative* link to libc.so.
# The sysroot may contain an absolute symlink (pointing to the host build path)
# which won't resolve inside the target rootfs.
# Handle both RISC-V and x86_64 architectures.
if [ -f "$STAGING/lib/libc.so" ]; then
    for LDMUSL in "$STAGING"/lib/ld-musl-*.so.1; do
        if [ -e "$LDMUSL" ] || [ -L "$LDMUSL" ]; then
            LDNAME=$(basename "$LDMUSL")
            ln -sf libc.so "$STAGING/lib/$LDNAME"
            echo "mkext4_rootfs: ensured $LDNAME -> libc.so (relative)"
        fi
    done
fi

for PYSO in "$STAGING"/lib/libpython3*.so.1.0; do
    if [ -f "$PYSO" ]; then
        BASENAME=$(basename "$PYSO" .1.0)
        ln -sf "$(basename "$PYSO")" "$STAGING/lib/$BASENAME"
    fi
done

# Copy Python standard library
mkdir -p "$STAGING/usr/local/lib/python3.12/lib-dynload"
if [ -d "$SYSROOT_DIR/lib/python3.12/lib-dynload" ]; then
    cp -P "$SYSROOT_DIR"/lib/python3.12/lib-dynload/*.so \
        "$STAGING/usr/local/lib/python3.12/lib-dynload/" 2>/dev/null || true
fi
PYLIB_SRC="${CPYTHON_SRC}/Lib"
if [ -d "$PYLIB_SRC" ]; then
    # Directories to exclude (saves ~35 MB)
    EXCLUDES=(
        test tests idlelib turtledemo ensurepip lib2to3
        tkinter __pycache__ distutils msilib
    )

    RSYNC_EXCLUDES=()
    for exc in "${EXCLUDES[@]}"; do
        RSYNC_EXCLUDES+=(--exclude="$exc")
    done

    rsync -a "${RSYNC_EXCLUDES[@]}" \
        --include='*/' \
        --include='*.py' \
        --include='*.pem' \
        --include='*.txt' \
        --include='*.cfg' \
        --exclude='*.pyc' \
        "$PYLIB_SRC/" "$STAGING/usr/local/lib/python3.12/"

    # Pre-compile .py -> .pyc to avoid expensive on-target compilation.
    # This dramatically speeds up Python startup because each import no
    # longer needs to parse source and write .pyc through ext2.
    echo "mkext4_rootfs: pre-compiling Python bytecode..."
    python3 -m compileall -q -j0 -b "$STAGING/usr/local/lib/python3.12/" 2>/dev/null || true

    PYFILE_COUNT=$(find "$STAGING/usr/local/lib/python3.12/" -type f | wc -l)
    PYC_COUNT=$(find "$STAGING/usr/local/lib/python3.12/" -name '*.pyc' | wc -l)
    PYUSED=$(du -sh "$STAGING/usr/local/lib/python3.12/" | cut -f1)
    echo "mkext4_rootfs: Python stdlib: ${PYFILE_COUNT} files (${PYC_COUNT} .pyc) (${PYUSED})"
else
    echo "mkext4_rootfs: warning: Python Lib not found at ${PYLIB_SRC}" >&2
fi

BIN_COUNT=$(find "$STAGING/bin/" -type f 2>/dev/null | wc -l)
TOTAL_USED=$(du -sh "$STAGING" | cut -f1)
echo "mkext4_rootfs: ${BIN_COUNT} programs, total ${TOTAL_USED}"

# Create the ext4 image populated from staging (no root/mount required)
mke2fs -F -t ext4 -O extent,dir_index,filetype,sparse_super -b 4096 -d "$STAGING" "$OUTPUT" "${SIZE_MB}m" >/dev/null 2>&1

# Fix directory ownership inside the ext4 image.
# mke2fs -d preserves host UID/GID; when building as non-root, directories
# like /var/empty end up owned by the builder's UID instead of root.
# sshd requires /var/empty to be owned by root:root with mode 0755.
if command -v debugfs >/dev/null 2>&1; then
    debugfs -w "$OUTPUT" <<'DEBUGFS_FIX' 2>/dev/null
set_inode_field /var/empty uid 0
set_inode_field /var/empty gid 0
set_inode_field /var/empty mode 040755
set_inode_field /var uid 0
set_inode_field /var gid 0
DEBUGFS_FIX
    echo "mkext4_rootfs: fixed /var/empty ownership (root:root, 0755)"
fi

echo "mkext4_rootfs: created ${OUTPUT} (${SIZE_MB} MB ext4)"

cleanup
trap - EXIT
