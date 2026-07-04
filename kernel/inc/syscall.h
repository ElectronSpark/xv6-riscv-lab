#ifndef __KERNEL_SYSCALL_H
#define __KERNEL_SYSCALL_H

#ifdef HOST_LIBC_PROGRAM
#include <sys/syscall.h>
#else

#include "compiler.h"

/*
 * xv6-private syscall numbers live in the 1200+ range.  That keeps the
 * Linux ABI slots free for native-number compatibility shims in the kernel.
 */

// --- Process management (1-19) ---
#define SYS_clone         1201
#define SYS_vfork         1202
#define SYS_exit          1203
#define SYS_exit_group    1204
#define SYS_wait          1205
#define SYS_exec          1206
#define SYS_kill          1207
#define SYS_tgkill        1208
#define SYS_getpid        1209
#define SYS_gettid       1210
#define SYS_sleep        1211
#define SYS_pause        1212
#define SYS_uptime       1213
#define SYS_sbrk         1214
#define SYS_setpgid      1215
#define SYS_getpgid      1216
#define SYS_setsid       1217
#define SYS_getsid       1218
#define SYS_getrandom    1219

// --- File system / VFS (20-49) ---
#define SYS_open         1220
#define SYS_close        1221
#define SYS_read         1222
#define SYS_write        1223
#define SYS_dup          1224
#define SYS_pipe         1225
#define SYS_fstat        1226
#define SYS_link         1227
#define SYS_unlink       1228
#define SYS_symlink      1229
#define SYS_mkdir        1230
#define SYS_mknod        1231
#define SYS_chdir        1232
#define SYS_chroot       1233
#define SYS_mount        1234
#define SYS_umount       1235
#define SYS_connect      1236
#define SYS_getdents     1237
#define SYS_getcwd       1238
#define SYS_sync         1239
#define SYS_ioctl        1240
#define SYS_tcgetattr    1241
#define SYS_tcsetattr    1242
#define SYS_lseek        1243
#define SYS_dup2         1244
#define SYS_fcntl        1245
#define SYS_access       1246
#define SYS_rename       1247
#define SYS_readlink     1248
#define SYS_stat         1249

// --- File system / VFS continued (50-69, mixed with memory) ---
#define SYS_mmap         1250
#define SYS_munmap       1251
#define SYS_mprotect     1252
#define SYS_mremap       1253
#define SYS_msync        1254
#define SYS_mincore      1255
#define SYS_madvise      1256
#define SYS_gettimeofday 1257
#define SYS_waitpid      1258
#define SYS_nanosleep    1259
#define SYS_ftruncate    1260
#define SYS_getppid      1261
#define SYS_uname        1262
#define SYS_lstat        1263
#define SYS_poll         1264
#define SYS_kqueue       1265
#define SYS_kevent_register 1266
#define SYS_kevent_wait  1267
#define SYS_brk          1268
#define SYS_futex        1269

// --- Signals (70-89) ---
#define SYS_sigaction    1270
#define SYS_sigreturn    1271
#define SYS_sigpending   1272
#define SYS_sigprocmask  1273
#define SYS_sigalarm     1274
#define SYS_sigsuspend   1275
#define SYS_sigwait      1276
#define SYS_tkill        1277
// 78-89 reserved

// --- Debug / introspection (90-99) ---
#define SYS_memstat      1290
#define SYS_dumpchan     1292
#define SYS_dumppcache   1293
#define SYS_dumprq       1294
#define SYS_kernbase     1295
#define SYS_dumpinode    1296
#define SYS_dumpblk      1297
#define SYS_losetup      1298
// 1291 reserved for the old dumpproc slot.
// 1299 reserved

// --- Network / sockets (100-119) ---
#define SYS_socket       1300
#define SYS_bind         1301
#define SYS_listen       1302
#define SYS_accept       1303
#define SYS_sconnect     1304   // "sconnect" to avoid clash with legacy SYS_connect
#define SYS_sendto       1305
#define SYS_recvfrom     1306
#define SYS_setsockopt   1307
#define SYS_getsockopt   1308
#define SYS_shutdown     1309
#define SYS_getpeername  1310
#define SYS_getsockname  1311
#define SYS_sendmsg      1312
#define SYS_recvmsg      1313
#define SYS_accept4      1314
#define SYS_sendfile     1315
// 116-119 reserved

// --- Extended syscalls (120-139) ---
#define SYS_openat         1320
#define SYS_writev         1321
#define SYS_readv          1322
#define SYS_set_tid_address 1323
#define SYS_clock_gettime  1324
#define SYS_clock_getres   1325
#define SYS_pread64        1326
#define SYS_pwrite64       1327
#define SYS_fstatat        1328
#define SYS_pipe2          1329
#define SYS_mkdirat        1330
#define SYS_mknodat        1331
#define SYS_unlinkat       1332
#define SYS_linkat         1333
#define SYS_symlinkat      1334
#define SYS_readlinkat     1335
#define SYS_renameat       1336
#define SYS_faccessat      1337
#define SYS_dup3           1338
#define SYS_getuid         1339
#define SYS_geteuid        1340
#define SYS_getgid         1341
#define SYS_getegid        1342
#define SYS_setuid         1343
#define SYS_setgid         1344
#define SYS_setreuid       1345
#define SYS_setregid       1346
#define SYS_getgroups      1347
#define SYS_setgroups      1348
// 149-157 reserved

// --- x86_64-specific syscalls ---
// musl's __set_thread_area hardcodes Linux's __NR_arch_prctl (158)
#define SYS_arch_prctl     158

// --- Resource limits ---
#define SYS_prlimit64      1359

// --- Kernel statistics ---
#define SYS_kstats         1360

// --- Network configuration ---
#define SYS_netconf        1361

// --- Vectored positional I/O ---
#define SYS_preadv         1362
#define SYS_pwritev        1363
#define SYS_preadv2        1364
#define SYS_pwritev2       1365

// --- Power management ---
#define SYS_poweroff       1366

// --- Profiling controls ---
#define SYS_kstatsctl      1367
#define SYS_kstats2        1368

// --- Legacy xv6 tools ---
#define SYS_dumpproc       1369
#define SYS_kprofile_pgroup 1370

// Max syscall number (update when adding new syscalls)
#define SYS_MAXNUM         1370

// --- Linux time64 compatibility stubs used by musl on rv64 ---
// These are intentionally unsupported in xv6 and return -ENOSYS via
// dispatcher stubs to avoid unknown-syscall log spam.
#define SYS_sched_rr_get_interval_time64 868
#define SYS_recvmmsg_time64              872
#define SYS_pselect6_time64              878
#define SYS_mq_timedsend_time64          887
#define SYS_mq_timedreceive_time64       888

// --- Resource limit stubs (high musl numbers) ---
#define SYS_getrlimit       840
#define SYS_setrlimit       841

// --- Additional socket stubs (high musl numbers) ---
#define SYS_sendmmsg        842

// --- File ownership and permission syscalls (musl high numbers) ---
#define SYS_umask           847
#define SYS_fchownat        939
#define SYS_fchown          940
#define SYS_fchmodat        941
#define SYS_fchmod          942
#define SYS_setresuid       858
#define SYS_setresgid       859
#define SYS_getresuid       903
#define SYS_getresgid       904
#define SYS_getitimer       943
#define SYS_setitimer       944
#define SYS_ppoll           945

// --- IPC stubs (musl high numbers, all return -ENOSYS) ---
#define SYS_semtimedop      923
#define SYS_shmget          924
#define SYS_shmdt           925
#define SYS_shmctl          926
#define SYS_shmat           927
#define SYS_semop           928
#define SYS_semget          929
#define SYS_semctl          930
#define SYS_msgsnd          931
#define SYS_msgrcv          932
#define SYS_msgget          933
#define SYS_msgctl          934

// --- Other musl stubs (return -ENOSYS) ---
#define SYS_munlockall      884
#define SYS_munlock         885
#define SYS_mlockall        892
#define SYS_mlock2          893
#define SYS_utimensat       938
#define SYS_clone3          937
#define SYS_rt_sigqueueinfo 936
#define SYS_clock_settime   935
#define SYS_fdatasync       915
#define SYS_fsync           908
#define SYS_get_robust_list 907
#define SYS_set_robust_list 866
#define SYS_sigaltstack     856
#define SYS_prctl           950
#define SYS_sysinfo         851
#define SYS_getrusage       902
#define SYS_getpriority     905
#define SYS_setpriority     860
#define SYS_memfd_create    894

// --- Linux filesystem/event compatibility ---
#define SYS_statx           995
#define SYS_inotify_rm_watch 978
#define SYS_inotify_add_watch 979
#define SYS_inotify_init1   980

// --- Scheduler affinity/yield stubs (musl high numbers) ---
#define SYS_sched_getaffinity  963
#define SYS_sched_setaffinity  962
#define SYS_sched_getscheduler 961
#define SYS_sched_setscheduler 960
#define SYS_sched_yield        959
#define SYS_sched_get_priority_max 958
#define SYS_sched_get_priority_min 957

// --- Linux-native x86_64 __NR_getrandom (318) ---
// OpenSSL calls getrandom with the native Linux x86_64 number
// directly instead of xv6's custom SYS_getrandom (19).
#define SYS_getrandom_x86   318
#define SYS_memfd_create_x86 319
#define SYS_memfd_create_generic 279

#define SYS_socketpair      974

// --- Aliases for musl stub numbers (mapped to real kernel numbers) ---
// musl defines these at high numbers (975-993) as stubs.
// We keep the kernel using compact numbers (112-115) and remap the musl
// stubs in musl-xv6/arch/*/bits/syscall.h.in to match.

// --- epoll/eventfd Linux ABI compatibility ---
// The kernel implements epoll as wrappers over kqueue.  Keep the public
// SYS_epoll* names on Linux ABI numbers, and retain the old high xv6-musl
// numbers as legacy aliases for already-built binaries.
#if defined(__x86_64__)
#define SYS_epoll_create    213
#define SYS_epoll_wait      232
#define SYS_epoll_ctl       233
#define SYS_epoll_pwait     281
#define SYS_signalfd        282
#define SYS_signalfd4       289
#define SYS_eventfd2        290
#define SYS_epoll_create1   291
#else
#define SYS_eventfd2        19
#define SYS_epoll_create1   20
#define SYS_epoll_ctl       21
#define SYS_epoll_pwait     22
#endif
#define SYS_eventfd2_legacy       985
#define SYS_epoll_pwait_legacy    986
#define SYS_epoll_ctl_legacy      987
#define SYS_epoll_create1_legacy  988
#define SYS_membarrier      998
#define SYS_fadvise64       989
#define SYS_fallocate       990
#define SYS_prlimit64_musl  996

// --- timerfd (match musl-xv6 syscall.h.in numbers) ---
#define SYS_timerfd_create  983
#define SYS_timerfd_settime 982
#define SYS_timerfd_gettime 981

// --- clock_nanosleep (match musl-xv6 syscall.h.in number) ---
#define SYS_clock_nanosleep 956

// --- Linux-native x86_64 time syscall ---
#define SYS_clock_nanosleep_x86 230

// --- Linux-native x86_64 event/futex syscalls ---
// Some third-party libraries use Linux UAPI numbers directly instead of the
// musl-xv6 high-number aliases.  Keep these as aliases to the real xv6
// implementations so WebKit/GLib event loops do not silently lose kernel
// services when code bypasses libc wrappers.
#define SYS_read_x86               0
#define SYS_write_x86              1
#define SYS_open_x86               2
#define SYS_close_x86              3
#define SYS_stat_x86               4
#define SYS_fstat_x86              5
#define SYS_lstat_x86              6
#define SYS_poll_x86               7
#define SYS_lseek_x86              8
#define SYS_mmap_x86               9
#define SYS_mprotect_x86           10
#define SYS_munmap_x86             11
#define SYS_brk_x86                12
#define SYS_rt_sigaction_x86       13
#define SYS_rt_sigprocmask_x86     14
#define SYS_rt_sigreturn_x86       15
#define SYS_ioctl_x86              16
#define SYS_pread64_x86            17
#define SYS_pwrite64_x86           18
#define SYS_readv_x86              19
#define SYS_writev_x86             20
#define SYS_access_x86             21
#define SYS_pipe_x86               22
#define SYS_select_x86             23
#define SYS_sched_yield_x86        24
#define SYS_mremap_x86             25
#define SYS_msync_x86              26
#define SYS_mincore_x86            27
#define SYS_madvise_x86            28
#define SYS_shmget_x86             29
#define SYS_shmat_x86              30
#define SYS_shmctl_x86             31
#define SYS_dup_x86                32
#define SYS_dup2_x86               33
#define SYS_pause_x86              34
#define SYS_nanosleep_x86          35
#define SYS_getitimer_x86          36
#define SYS_alarm_x86              37
#define SYS_setitimer_x86          38
#define SYS_getpid_x86             39
#define SYS_sendfile_x86           40
#define SYS_socket_x86             41
#define SYS_connect_x86            42
#define SYS_accept_x86             43
#define SYS_sendto_x86             44
#define SYS_recvfrom_x86           45
#define SYS_sendmsg_x86            46
#define SYS_recvmsg_x86            47
#define SYS_shutdown_x86           48
#define SYS_bind_x86               49
#define SYS_listen_x86             50
#define SYS_getsockname_x86        51
#define SYS_getpeername_x86        52
#define SYS_socketpair_x86         53
#define SYS_setsockopt_x86         54
#define SYS_getsockopt_x86         55
#define SYS_clone_x86              56
#define SYS_fork_x86               57
#define SYS_vfork_x86              58
#define SYS_execve_x86             59
#define SYS_exit_x86               60
#define SYS_wait4_x86              61
#define SYS_kill_x86               62
#define SYS_uname_x86              63
#define SYS_semget_x86             64
#define SYS_semop_x86              65
#define SYS_semctl_x86             66
#define SYS_shmdt_x86              67
#define SYS_msgget_x86             68
#define SYS_msgsnd_x86             69
#define SYS_msgrcv_x86             70
#define SYS_msgctl_x86             71
#define SYS_fcntl_x86              72
#define SYS_flock_x86              73
#define SYS_fsync_x86              74
#define SYS_fdatasync_x86          75
#define SYS_ftruncate_x86          77
#define SYS_truncate_x86           76
#define SYS_getdents_x86           78
#define SYS_getcwd_x86             79
#define SYS_chdir_x86              80
#define SYS_fchdir_x86             81
#define SYS_rename_x86             82
#define SYS_mkdir_x86              83
#define SYS_rmdir_x86              84
#define SYS_creat_x86              85
#define SYS_link_x86               86
#define SYS_unlink_x86             87
#define SYS_symlink_x86            88
#define SYS_readlink_x86           89
#define SYS_chmod_x86              90
#define SYS_fchmod_x86             91
#define SYS_chown_x86              92
#define SYS_fchown_x86             93
#define SYS_lchown_x86             94
#define SYS_umask_x86              95
#define SYS_gettimeofday_x86       96
#define SYS_getrlimit_x86          97
#define SYS_getrusage_x86          98
#define SYS_sysinfo_x86            99
#define SYS_times_x86              100
#define SYS_getuid_x86             102
#define SYS_getgid_x86             104
#define SYS_setuid_x86             105
#define SYS_setgid_x86             106
#define SYS_geteuid_x86            107
#define SYS_getegid_x86            108
#define SYS_setpgid_x86            109
#define SYS_getppid_x86            110
#define SYS_getpgrp_x86            111
#define SYS_setsid_x86             112
#define SYS_setreuid_x86           113
#define SYS_setregid_x86           114
#define SYS_getgroups_x86          115
#define SYS_setgroups_x86          116
#define SYS_setresuid_x86          117
#define SYS_getresuid_x86          118
#define SYS_setresgid_x86          119
#define SYS_getresgid_x86          120
#define SYS_getpgid_x86            121
#define SYS_setfsuid_x86           122
#define SYS_setfsgid_x86           123
#define SYS_getsid_x86             124
#define SYS_capget_x86             125
#define SYS_capset_x86             126
#define SYS_rt_sigpending_x86      127
#define SYS_rt_sigtimedwait_x86    128
#define SYS_rt_sigqueueinfo_x86    129
#define SYS_rt_sigsuspend_x86      130
#define SYS_sigaltstack_x86        131
#define SYS_utime_x86              132
#define SYS_mknod_x86              133
#define SYS_statfs_x86             137
#define SYS_fstatfs_x86            138
#define SYS_getpriority_x86        140
#define SYS_setpriority_x86        141
#define SYS_sched_setparam_x86     142
#define SYS_sched_getparam_x86     143
#define SYS_sched_setscheduler_x86 144
#define SYS_sched_getscheduler_x86 145
#define SYS_sched_get_priority_max_x86 146
#define SYS_sched_get_priority_min_x86 147
#define SYS_sched_rr_get_interval_x86 148
#define SYS_mlock_x86              149
#define SYS_munlock_x86            150
#define SYS_mlockall_x86           151
#define SYS_munlockall_x86         152
#define SYS_prctl_x86              157
#define SYS_setrlimit_x86          160
#define SYS_chroot_x86             161
#define SYS_sync_x86               162
#define SYS_mount_x86              165
#define SYS_umount2_x86            166
#define SYS_reboot_x86             169
#define SYS_gettid_x86             186
#define SYS_readahead_x86          187
#define SYS_tkill_x86              200
#define SYS_setxattr_x86           188
#define SYS_lsetxattr_x86          189
#define SYS_fsetxattr_x86          190
#define SYS_getxattr_x86           191
#define SYS_lgetxattr_x86          192
#define SYS_fgetxattr_x86          193
#define SYS_listxattr_x86          194
#define SYS_llistxattr_x86         195
#define SYS_flistxattr_x86         196
#define SYS_removexattr_x86        197
#define SYS_lremovexattr_x86       198
#define SYS_fremovexattr_x86       199
#define SYS_time_x86               201
#define SYS_futex_x86              202
#define SYS_sched_setaffinity_x86  203
#define SYS_sched_getaffinity_x86  204
#define SYS_getdents64_x86         217
#define SYS_set_tid_address_x86    218
#define SYS_semtimedop_x86         220
#define SYS_fadvise64_x86          221
#define SYS_clock_settime_x86      227
#define SYS_clock_gettime_x86      228
#define SYS_clock_getres_x86       229
#define SYS_exit_group_x86         231
#define SYS_tgkill_x86             234
#define SYS_utimes_x86             235
#define SYS_pselect6_x86           270
#define SYS_ppoll_x86              271
#define SYS_set_robust_list_x86    273
#define SYS_get_robust_list_x86    274
#define SYS_sync_file_range_x86    277
#define SYS_openat_x86             257
#define SYS_mkdirat_x86            258
#define SYS_mknodat_x86            259
#define SYS_fchownat_x86           260
#define SYS_newfstatat_x86         262
#define SYS_unlinkat_x86           263
#define SYS_renameat_x86           264
#define SYS_linkat_x86             265
#define SYS_symlinkat_x86          266
#define SYS_readlinkat_x86         267
#define SYS_fchmodat_x86           268
#define SYS_faccessat_x86          269
#define SYS_utimensat_x86          280
#define SYS_waitid_x86             247
#define SYS_ioprio_set_x86         251
#define SYS_ioprio_get_x86         252
#define SYS_futimesat_x86          261
#define SYS_inotify_init_x86       253
#define SYS_inotify_add_watch_x86  254
#define SYS_inotify_rm_watch_x86   255
#define SYS_timerfd_create_x86     283
#define SYS_eventfd_x86            284
#define SYS_fallocate_x86          285
#define SYS_timerfd_settime_x86    286
#define SYS_timerfd_gettime_x86    287
#define SYS_accept4_x86            288
#define SYS_inotify_init1_x86      294
#define SYS_dup3_x86               292
#define SYS_pipe2_x86              293
#define SYS_preadv_x86             295
#define SYS_pwritev_x86            296
#define SYS_recvmmsg_x86           299
#define SYS_prlimit64_x86          302
#define SYS_sendmmsg_x86           307
#define SYS_syncfs_x86             306
#define SYS_getcpu_x86             309
#define SYS_copy_file_range_x86    326
#define SYS_renameat2_x86          316
#define SYS_sched_setattr_x86      314
#define SYS_sched_getattr_x86      315
#define SYS_membarrier_x86         324
#define SYS_mlock2_x86             325
#define SYS_preadv2_x86            327
#define SYS_pwritev2_x86           328
#define SYS_statx_x86              332
#define SYS_rseq_x86               334
#define SYS_clone3_x86             435
#define SYS_close_range_x86        436
#define SYS_openat2_x86            437
#define SYS_faccessat2_x86         439
#define SYS_fchmodat2_x86          452

// --- Linux x86_64 optional/privileged compatibility numbers ---
#define SYS_ptrace_x86             101
#define SYS_syslog_x86             103
#define SYS_uselib_x86             134
#define SYS_personality_x86        135
#define SYS_ustat_x86              136
#define SYS_sysfs_x86              139
#define SYS_vhangup_x86            153
#define SYS_modify_ldt_x86         154
#define SYS_pivot_root_x86         155
#define SYS__sysctl_x86            156
#define SYS_adjtimex_x86           159
#define SYS_acct_x86               163
#define SYS_settimeofday_x86       164
#define SYS_swapon_x86             167
#define SYS_swapoff_x86            168
#define SYS_sethostname_x86        170
#define SYS_setdomainname_x86      171
#define SYS_iopl_x86               172
#define SYS_ioperm_x86             173
#define SYS_create_module_x86      174
#define SYS_init_module_x86        175
#define SYS_delete_module_x86      176
#define SYS_get_kernel_syms_x86    177
#define SYS_query_module_x86       178
#define SYS_quotactl_x86           179
#define SYS_nfsservctl_x86         180
#define SYS_getpmsg_x86            181
#define SYS_putpmsg_x86            182
#define SYS_afs_syscall_x86        183
#define SYS_tuxcall_x86            184
#define SYS_security_x86           185
#define SYS_set_thread_area_x86    205
#define SYS_io_setup_x86           206
#define SYS_io_destroy_x86         207
#define SYS_io_getevents_x86       208
#define SYS_io_submit_x86          209
#define SYS_io_cancel_x86          210
#define SYS_get_thread_area_x86    211
#define SYS_lookup_dcookie_x86     212
#define SYS_epoll_ctl_old_x86      214
#define SYS_epoll_wait_old_x86     215
#define SYS_remap_file_pages_x86   216
#define SYS_restart_syscall_x86    219
#define SYS_timer_create_x86       222
#define SYS_timer_settime_x86      223
#define SYS_timer_gettime_x86      224
#define SYS_timer_getoverrun_x86   225
#define SYS_timer_delete_x86       226
#define SYS_vserver_x86            236
#define SYS_mbind_x86              237
#define SYS_set_mempolicy_x86      238
#define SYS_get_mempolicy_x86      239
#define SYS_mq_open_x86            240
#define SYS_mq_unlink_x86          241
#define SYS_mq_timedsend_x86       242
#define SYS_mq_timedreceive_x86    243
#define SYS_mq_notify_x86          244
#define SYS_mq_getsetattr_x86      245
#define SYS_kexec_load_x86         246
#define SYS_add_key_x86            248
#define SYS_request_key_x86        249
#define SYS_keyctl_x86             250
#define SYS_migrate_pages_x86      256
#define SYS_unshare_x86            272
#define SYS_splice_x86             275
#define SYS_tee_x86                276
#define SYS_vmsplice_x86           278
#define SYS_move_pages_x86         279
#define SYS_rt_tgsigqueueinfo_x86  297
#define SYS_perf_event_open_x86    298
#define SYS_fanotify_init_x86      300
#define SYS_fanotify_mark_x86      301
#define SYS_name_to_handle_at_x86  303
#define SYS_open_by_handle_at_x86  304
#define SYS_clock_adjtime_x86      305
#define SYS_setns_x86              308
#define SYS_process_vm_readv_x86   310
#define SYS_process_vm_writev_x86  311
#define SYS_kcmp_x86               312
#define SYS_finit_module_x86       313
#define SYS_seccomp_x86            317
#define SYS_kexec_file_load_x86    320
#define SYS_bpf_x86                321
#define SYS_execveat_x86           322
#define SYS_userfaultfd_x86        323
#define SYS_pkey_mprotect_x86      329
#define SYS_pkey_alloc_x86         330
#define SYS_pkey_free_x86          331
#define SYS_io_pgetevents_x86      333
#define SYS_pidfd_send_signal_x86  424
#define SYS_io_uring_setup_x86     425
#define SYS_io_uring_enter_x86     426
#define SYS_io_uring_register_x86  427
#define SYS_open_tree_x86          428
#define SYS_move_mount_x86         429
#define SYS_fsopen_x86             430
#define SYS_fsconfig_x86           431
#define SYS_fsmount_x86            432
#define SYS_fspick_x86             433
#define SYS_pidfd_open_x86         434
#define SYS_pidfd_getfd_x86        438
#define SYS_process_madvise_x86    440
#define SYS_mount_setattr_x86      442
#define SYS_quotactl_fd_x86        443
#define SYS_landlock_create_ruleset_x86 444
#define SYS_landlock_add_rule_x86  445
#define SYS_landlock_restrict_self_x86 446
#define SYS_memfd_secret_x86       447
#define SYS_process_mrelease_x86   448
#define SYS_futex_waitv_x86        449
#define SYS_set_mempolicy_home_node_x86 450
#define SYS_cachestat_x86          451
#define SYS_map_shadow_stack_x86   453
#define SYS_futex_wake_x86         454
#define SYS_futex_wait_x86         455
#define SYS_futex_requeue_x86      456
#define SYS_statmount_x86          457
#define SYS_listmount_x86          458
#define SYS_lsm_get_self_attr_x86  459
#define SYS_lsm_set_self_attr_x86  460
#define SYS_lsm_list_modules_x86   461

// --- Linux generic time64/event aliases with non-conflicting numbers ---
#define SYS_prctl_generic          167
#define SYS_gettimeofday_generic   169
#define SYS_gettid_generic         178
#define SYS_sysinfo_generic        179
#define SYS_brk_generic            214
#define SYS_munmap_generic         215
#define SYS_mremap_generic         216
#define SYS_mmap_generic           222
#define SYS_mprotect_generic       226
#define SYS_msync_generic          227
#define SYS_mlock_generic          228
#define SYS_munlock_generic        229
#define SYS_mlockall_generic       230
#define SYS_munlockall_generic     231
#define SYS_mincore_generic        232
#define SYS_madvise_generic        233
#define SYS_prlimit64_generic      261
#define SYS_getrandom_generic      278
#define SYS_membarrier_generic     283
#define SYS_mlock2_generic         284
#define SYS_statx_generic          291
#define SYS_clock_gettime64_generic 403
#define SYS_clock_getres_time64_generic 406
#define SYS_clock_nanosleep_time64 407
#define SYS_timerfd_gettime64      410
#define SYS_timerfd_settime64      411
#define SYS_pselect6_time64_generic 413
#define SYS_ppoll_time64           414
#define SYS_futex_time64           422
#define SYS_epoll_pwait2           441
#define SYS_clone3_generic         435

// --- File locking (match musl-xv6 syscall.h.in number) ---
#define SYS_flock           912

// --- Power management (high number for musl compat) ---
#define SYS_reboot          873

// --- Filesystem info (match musl-xv6 syscall.h.in numbers) ---
#define SYS_statfs          855
#define SYS_fstatfs         909

#endif /* HOST_LIBC_PROGRAM */

#endif /* __KERNEL_SYSCALL_H */
