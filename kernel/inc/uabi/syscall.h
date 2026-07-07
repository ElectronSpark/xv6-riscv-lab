#ifndef __USER_ABI_SYSCALL_H
#define __USER_ABI_SYSCALL_H

#include "compiler.h"

/*
 * System call numbers — grouped by subsystem with gaps for future use.
 *
 * xv6-private syscalls live above Linux's native ABI ranges so raw Linux
 * syscall numbers can be routed by the kernel without libc-side renumbering.
 */

// --- Process management (1-19) ---
#define SYS_clone 1201
#define SYS_vfork 1202
#define SYS_exit 1203
#define SYS_exit_group 1204
#define SYS_wait 1205
#define SYS_exec 1206
#define SYS_kill 1207
#define SYS_tgkill 1208
#define SYS_getpid 1209
#define SYS_gettid 1210
#define SYS_sleep 1211
#define SYS_pause 1212
#define SYS_uptime 1213
#define SYS_sbrk 1214
#define SYS_setpgid 1215
#define SYS_getpgid 1216
#define SYS_setsid 1217
#define SYS_getsid 1218
#define SYS_getrandom 1219

// --- File system / VFS (20-49) ---
#define SYS_open 1220
#define SYS_close 1221
#define SYS_read 1222
#define SYS_write 1223
#define SYS_dup 1224
#define SYS_pipe 1225
#define SYS_fstat 1226
#define SYS_link 1227
#define SYS_unlink 1228
#define SYS_symlink 1229
#define SYS_mkdir 1230
#define SYS_mknod 1231
#define SYS_chdir 1232
#define SYS_chroot 1233
#define SYS_mount 1234
#define SYS_umount 1235
#define SYS_connect 1236
#define SYS_getdents 1237
#define SYS_getcwd 1238
#define SYS_sync 1239
#define SYS_ioctl 1240
#define SYS_tcgetattr 1241
#define SYS_tcsetattr 1242
#define SYS_lseek 1243
#define SYS_dup2 1244
#define SYS_fcntl 1245
#define SYS_access 1246
#define SYS_rename 1247
#define SYS_readlink 1248
#define SYS_stat 1249

// --- Filesystem info ---
#define SYS_statfs 855

// --- Memory management (50-69) ---
#define SYS_mmap 1250
#define SYS_munmap 1251
#define SYS_mprotect 1252
#define SYS_mremap 1253
#define SYS_msync 1254
#define SYS_mincore 1255
#define SYS_madvise 1256
#define SYS_gettimeofday 1257
#define SYS_waitpid 1258
#define SYS_nanosleep 1259
#define SYS_ftruncate 1260
#define SYS_getppid 1261
#define SYS_uname 1262
#define SYS_lstat 1263
#define SYS_poll 1264
// 65-69 reserved

// --- Signals (70-89) ---
#define SYS_sigaction 1270
#define SYS_sigreturn 1271
#define SYS_sigpending 1272
#define SYS_sigprocmask 1273
#define SYS_sigalarm 1274
#define SYS_sigsuspend 1275
#define SYS_sigwait 1276
#define SYS_tkill 1277
// 78-89 reserved

// --- Debug / introspection (90-99) ---
#define SYS_memstat 1290
#define SYS_dumpchan 1292
#define SYS_dumppcache 1293
#define SYS_dumprq 1294
#define SYS_kernbase 1295
#define SYS_dumpinode 1296
#define SYS_dumpblk 1297
#define SYS_losetup 1298
// 1291 reserved for the old dumpproc slot.
// 1299 reserved

// --- Legacy xv6 tools ---
#define SYS_kstatsctl 1367
#define SYS_kstats2 1368
#define SYS_dumpproc 1369
#define SYS_kprofile_pgroup 1370
#define SYS_kprofile_prepty_ring 1371
#define SYS_kprofile_userpc_ctl 1372
#define SYS_kprofile_userpc_snapshot 1373
#define SYS_kprofile_vfs_enoent_snapshot 1374

// Max syscall number (update when adding new syscalls)
#define SYS_MAXNUM 1374

#endif /* __USER_ABI_SYSCALL_H */
