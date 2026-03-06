# Plan: Port OpenSSL + OpenSSH to xv6

**TL;DR:** Add OpenSSL 3.x as a statically-linked crypto library and OpenSSH (sshd + ssh client) to xv6, following the established autotools cross-compilation pattern used by dash and CPython. This requires: (1) adding compat shims for missing POSIX APIs (syslog, `/dev/urandom`), (2) cross-building OpenSSL as static `libcrypto.a`/`libssl.a`, (3) cross-building OpenSSH statically linked against those libs + musl, (4) integrating host-key generation and `sshd` user into the rootfs. Both RISC-V and x86_64 architectures will be supported.

## Context

This modified xv6 supports:
- Sessions, process groups, thread groups (processes), and multi-thread (clone-based)
- Limited TTY/PTY support (`/dev/ptmx` → `/dev/pts/N`, `TIOCGPTN`, `TIOCSCTTY`, `TIOCGPGRP`/`TIOCSPGRP`, termios `TCGETS`/`TCSETS`)
- Full BSD socket API (TCP/UDP via lwIP, AF_UNIX with socketpair, AF_NETLINK)
- musl libc 1.2.5 (both static and shared builds)
- Existing third-party ports: ncurses, readline, dash, CPython 3.12
- User management: `/etc/passwd`, `/etc/shadow`, `/etc/group`, `crypt()`, `setuid`/`setgid`
- `getrandom()` syscall (nr 19) and `/dev/random` device
- Telnetd already demonstrates the fork → setsid → PTY → exec(login) pattern that sshd needs

### Custom xv6 musl toolchain (available)

Both architectures have a dedicated two-phase GCC 14.2.0 cross-toolchain built with the project's xv6-patched musl:

| Toolchain | Triplet | Phase 2 Location |
|-----------|---------|------------------|
| RISC-V | `riscv64-xv6-linux-musl` | `toolchain/riscv64/phase2/` |
| x86_64 | `x86_64-xv6-linux-musl` | `toolchain/x86_64/phase2/` |

- Both produce static + dynamic ELF binaries with full musl sysroot (headers, `libc.a`, `libc.so`, CRT objects)
- The active `build/` already uses the riscv64 toolchain
- OpenSSL/OpenSSH can use `--host=riscv64-xv6-linux-musl` / `--host=x86_64-xv6-linux-musl` directly — no need to juggle multiple compilers

### struct stat — ALREADY DONE

The musl `struct stat` overlay has been fully extended to the standard 128-byte Linux/POSIX layout for both architectures. The kernel's `struct stat` at `kernel/inc/vfs/stat.h` matches exactly — all fields (`st_uid`, `st_gid`, `st_rdev`, `st_blksize`, `st_blocks`, timestamps) are present. The old custom `fstatat.c` / `kstat.h` conversion shims have been removed; musl's built-in fstatat works directly. **No action needed.**

## Phase 1 — Compat shims for missing APIs

1. **Add `/dev/urandom` to init.c** — add `mknod("/dev/urandom", S_IFCHR | 0666, makedev(RANDOM_MAJOR, RANDOM_MINOR))` right after the existing `/dev/random` mknod in `user/musl-xv6/programs/init.c` (line 317). Both devices use the same kernel driver (xorshift64* PRNG). OpenSSL probes `/dev/urandom` as a fallback; `getrandom()` syscall (nr 19) is the primary path and already works.

2. **Add `syslog` compat shim** in `user/musl-xv6/compat/`. Create `syslog_shim.c` providing `openlog()`, `syslog()`, `closelog()`, `vsyslog()` that write formatted messages to stderr. OpenSSH calls these extensively. The shim will be compiled into a `libssh_compat.a` archive and linked into the OpenSSH binary.

3. **Consider stubs for `getrlimit`/`flock`** — OpenSSH checks these. `SYS_prlimit64` (996) and `SYS_flock` (873) both stub to `-ENOSYS` in the kernel. musl will forward the syscall and get -ENOSYS back, so OpenSSH will handle the errors at runtime. If configure-time detection fails, we can add `ac_cv_*` overrides. No compat shim needed upfront — evaluate during build.

## Phase 2 — OpenSSL static library

4. **Add OpenSSL 3.x source** as a git submodule at `user/openssl/` (e.g., OpenSSL 3.0 LTS or 3.3 stable). The submodule approach matches ncurses/readline/dash/cpython.

5. **Add OpenSSL build section** in `user/CMakeLists.txt` following the ncurses/readline pattern. Key configure invocation:

    - `./Configure` (OpenSSL uses a Perl configure, not autotools) with target `linux-generic64` (RISC-V) or `linux-x86_64` (x86_64)
    - Set `CC`, `AR`, `RANLIB` to the cross-compiler
    - Set `CFLAGS` with `--sysroot=${MUSL_SYSROOT} -nostdinc -isystem ${MUSL_INCLUDE_DIR} -isystem ${GCC_INCLUDE_DIR_MUSL} -isystem ${MUSL_COMPAT_DIR}`
    - Pass `no-shared no-dso no-engine no-async no-threads no-asm no-tests` — disable shared libs (static only), disable dlopen-based engine loading, disable async (uses `sigaltstack`), disable assembly optimizations, skip tests
    - `no-afalgeng no-ktls no-ui-console` — disable kernel TLS, AF_ALG, interactive UI
    - `--prefix=/ --openssldir=/etc/ssl`
    - `make -j16 build_libs` (build only libraries, not the CLI tool)
    - `make install_dev DESTDIR=${MUSL_SYSROOT}` (install headers + `libcrypto.a` + `libssl.a` to sysroot)
    - Outputs: `${OPENSSL_BUILD_DIR}/libcrypto.a`, `${OPENSSL_BUILD_DIR}/libssl.a`
    - CMake target: `local_openssl_build`, depends on `musl_sysroot`
    - The custom xv6 musl toolchain handles `--sysroot` automatically — CFLAGS only need arch-specific additions

6. **Architecture-specific OpenSSL configure targets:**
    - RISC-V: `linux-generic64` with `-march=rv64gc -mabi=lp64d -mcmodel=medany`
    - x86_64: `linux-x86_64` with `no-asm` (due to `-mno-sse` constraint) or `linux-generic64`

## Phase 3 — OpenSSH build

7. **Add OpenSSH source** as a git submodule at `user/openssh/` (e.g., OpenSSH 9.x). OpenSSH uses autotools (`configure`).

8. **Build the syslog compat shim** into a `libssh_compat.a` static archive (similar to the `libedit.a` shim pattern used for dash). This archive collects: syslog shim (from step 2) and any other stubs needed during the build.

9. **Add OpenSSH build section** in `user/CMakeLists.txt`. Key configure invocation:

    - `./configure --host=${TRIPLET} --prefix=/ --sysconfdir=/etc/ssh`
    - `CC`, `AR`, `RANLIB` = xv6 musl cross-compiler (from toolchain)
    - `CFLAGS` = sysroot + nostdinc + isystem musl + compat + OpenSSL include path
    - `LDFLAGS` = `-static -nostartfiles -nostdlib ${MUSL_CRT1_O} ${MUSL_CRTI_O} -Wl,-z,max-page-size=0x1000 -Wl,-z,common-page-size=0x1000 -Wl,--build-id=none -T ${MUSL_LINKER_SCRIPT} -L${OPENSSL_BUILD_DIR} -L<compat_lib_dir>`
    - `LIBS` = `${MUSL_LIBC_A} ${LIBGCC_PATH} ${MUSL_CRTN_O}`
    - Extensive `ac_cv_*` overrides:
      - `ac_cv_func_getaddrinfo=yes`, `ac_cv_func_getnameinfo=yes` (networking works)
      - `ac_cv_func_openpty=no` (OpenSSH should use `/dev/ptmx` directly)
      - `ac_cv_func_login=no`, `ac_cv_func_logout=no` (no utmp)
      - `ac_cv_func_syslog=yes` (our compat shim provides it)
      - `ac_cv_func_dlopen=no` (no dynamic loading)
      - `ac_cv_func_setrlimit=yes` (our compat shim)
      - `ac_cv_func_setitimer=no`, `ac_cv_func_sigaltstack=no`
      - `ac_cv_header_utmp_h=no`, `ac_cv_header_utmpx_h=no` (no login accounting)
      - `ac_cv_header_lastlog_h=no`
      - `--without-pam --without-selinux --without-audit --without-kerberos5`
      - `--with-sandbox=no` — disable seccomp/pledge sandbox (xv6 has none)
      - `--with-ssl-dir=${MUSL_SYSROOT}` (finds OpenSSL headers + libs)
      - `--with-privsep-user=sshd --with-privsep-path=/var/empty`
    - `make -j16` to build `sshd` and `ssh`
    - `make install-nokeys DESTDIR=${MUSL_SYSROOT}` (install binaries without generating host keys)
    - Outputs: `${OPENSSH_BUILD_DIR}/sshd`, `${OPENSSH_BUILD_DIR}/ssh`
    - CMake targets: `openssh_configure` → `openssh_build`, depends on `local_openssl_build`, `musl_sysroot`

10. **Wire into the build system:**
    - Add `sysroot_install()` calls for `sshd` → `${SYSROOT_BIN_DIR}/sshd` and `ssh` → `${SYSROOT_BIN_DIR}/ssh`
    - Add `ssh-keygen` → `${SYSROOT_BIN_DIR}/ssh-keygen` (needed for host key generation)
    - Add `add_dependencies(user_programs openssh_build)`

## Phase 4 — Filesystem integration

11. **Update `scripts/mkext4_rootfs.sh`:**
    - Create `/etc/ssh/` directory in staging
    - Generate host keys at image build time: `ssh-keygen -t rsa -f staging/etc/ssh/ssh_host_rsa_key -N ""` (and ed25519, ecdsa)
    - Create `/var/empty/` (sshd privilege separation chroot directory)
    - Add `sshd` user to `/etc/passwd`: `sshd:x:74:74:sshd privsep:/var/empty:/bin/false`
    - Add `sshd` group to `/etc/group`: `sshd:x:74:`
    - Write minimal `/etc/ssh/sshd_config`:
      - `Port 22`
      - `PermitRootLogin yes` (for testing)
      - `PasswordAuthentication yes`
      - `UsePAM no`
      - `UsePrivilegeSeparation no` (initially; iterate if chroot works)
      - `HostKey /etc/ssh/ssh_host_rsa_key` (etc.)
      - `SyslogFacility AUTH`
      - `LogLevel INFO`
    - Add `/bin/ssh` to `/etc/shells`
    - Create `/dev/urandom → /dev/random` symlink (or let kernel create it)

12. **QEMU network forwarding:** Ensure the QEMU launch command (in CMake or Makefile) forwards host port to guest port 22. For SLIRP networking, add `-netdev user,id=net0,hostfwd=tcp::2222-:22` so `ssh -p 2222 root@localhost` connects to xv6's sshd.

## Phase 5 — Testing

13. **Boot xv6, start sshd manually** with `/bin/sshd -D -d` (foreground + debug) to verify it binds to port 22, completes key exchange, and authenticates.

14. **Test SSH client** from within xv6: `/bin/ssh root@<ip>` to connect back to itself (loopback) or to an external host.

15. **Automate sshd start** — modify `user/musl-xv6/programs/init.c` (the musl init) or the startup script to launch sshd as a daemon at boot, similar to how `getty` is spawned.

## Verification Checklist

- [ ] Build: `cmake .. && make -j16` completes with OpenSSL + OpenSSH targets
- [ ] `sshd` binary exists in `sysroot/bin/sshd`, is statically linked (verify with `file` command)
- [ ] Boot xv6, run `sshd -D -d`, connect from host via `ssh -p 2222 root@localhost`
- [ ] SSH key exchange completes, password prompt appears, shell spawns via PTY
- [ ] Run `ssh` client inside xv6 to verify bidirectional connectivity

## Key Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Linking | Fully static | Avoids dlopen issues, produces self-contained binaries like dash |
| OpenSSL `no-asm` | Yes (both arches) | x86_64 builds disable SSE/AVX (`-mno-sse`); C fallback is slower but portable. RISC-V has no optimized asm in OpenSSL anyway |
| OpenSSL `no-threads no-async` | Yes | sshd is fork-based, not multithreaded; async uses `sigaltstack` which is stubbed |
| Syslog | stderr shim | Simplest approach, matches how `sshd -D` already logs to stderr in debug mode |
| Host keys | Generated at image build time | Simpler than first-boot generation; avoids needing `ssh-keygen` to run inside xv6 before sshd can start |
| Privilege separation | `UsePrivilegeSeparation no` initially | Iterate once chroot behavior is verified; `sshd` user + `/var/empty` provisioned for future enablement |
| Architectures | Both RISC-V and x86_64 | Mirrors existing multi-arch support throughout the build system |
