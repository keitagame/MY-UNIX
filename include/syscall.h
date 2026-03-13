#pragma once
#ifndef _SYSCALL_H
#define _SYSCALL_H

/* ============================================================
 * syscall.h - システムコール番号定義
 * Linux x86_64 ABI互換
 * ============================================================ */

/* ファイルI/O */
#define SYS_read            0
#define SYS_write           1
#define SYS_open            2
#define SYS_close           3
#define SYS_stat            4
#define SYS_fstat           5
#define SYS_lstat           6
#define SYS_poll            7
#define SYS_lseek           8
#define SYS_mmap            9
#define SYS_mprotect        10
#define SYS_munmap          11
#define SYS_brk             12
#define SYS_rt_sigaction    13
#define SYS_rt_sigprocmask  14
#define SYS_rt_sigreturn    15
#define SYS_ioctl           16
#define SYS_pread64         17
#define SYS_pwrite64        18
#define SYS_readv           19
#define SYS_writev          20
#define SYS_access          21
#define SYS_pipe            22
#define SYS_select          23
#define SYS_sched_yield     24
#define SYS_mremap          25
#define SYS_msync           26
#define SYS_mincore         27
#define SYS_madvise         28
#define SYS_shmget          29
#define SYS_shmat           30
#define SYS_shmctl          31
#define SYS_dup             32
#define SYS_dup2            33
#define SYS_pause           34
#define SYS_nanosleep       35
#define SYS_getitimer       36
#define SYS_alarm           37
#define SYS_setitimer       38
#define SYS_getpid          39
#define SYS_sendfile        40
#define SYS_socket          41
#define SYS_connect         42
#define SYS_accept          43
#define SYS_sendto          44
#define SYS_recvfrom        45
#define SYS_sendmsg         46
#define SYS_recvmsg         47
#define SYS_shutdown        48
#define SYS_bind            49
#define SYS_listen          50
#define SYS_getsockname     51
#define SYS_getpeername     52
#define SYS_socketpair      53
#define SYS_setsockopt      54
#define SYS_getsockopt      55
#define SYS_clone           56
#define SYS_fork            57
#define SYS_vfork           58
#define SYS_execve          59
#define SYS_exit            60
#define SYS_wait4           61
#define SYS_kill            62
#define SYS_uname           63
#define SYS_semget          64
#define SYS_semop           65
#define SYS_semctl          66
#define SYS_shmdt           67
#define SYS_msgget          68
#define SYS_msgsnd          69
#define SYS_msgrcv          70
#define SYS_msgctl          71
#define SYS_fcntl           72
#define SYS_flock           73
#define SYS_fsync           74
#define SYS_fdatasync       75
#define SYS_truncate        76
#define SYS_ftruncate       77
#define SYS_getdents        78
#define SYS_getcwd          79
#define SYS_chdir           80
#define SYS_fchdir          81
#define SYS_rename          82
#define SYS_mkdir           83
#define SYS_rmdir           84
#define SYS_creat           85
#define SYS_link            86
#define SYS_unlink          87
#define SYS_symlink         88
#define SYS_readlink        89
#define SYS_chmod           90
#define SYS_fchmod          91
#define SYS_chown           92
#define SYS_fchown          93
#define SYS_lchown          94
#define SYS_umask           95
#define SYS_gettimeofday    96
#define SYS_getrlimit       97
#define SYS_getrusage       98
#define SYS_sysinfo         99
#define SYS_times           100
#define SYS_ptrace          101
#define SYS_getuid          102
#define SYS_syslog          103
#define SYS_getgid          104
#define SYS_setuid          105
#define SYS_setgid          106
#define SYS_geteuid         107
#define SYS_getegid         108
#define SYS_setpgid         109
#define SYS_getppid         110
#define SYS_getpgrp         111
#define SYS_setsid          112
#define SYS_setreuid        113
#define SYS_setregid        114
#define SYS_getgroups       115
#define SYS_setgroups       116
#define SYS_setresuid       117
#define SYS_getresuid       118
#define SYS_setresgid       119
#define SYS_getresgid       120
#define SYS_getpgid         121
#define SYS_setfsuid        122
#define SYS_setfsgid        123
#define SYS_getsid          124
#define SYS_capget          125
#define SYS_capset          126
#define SYS_rt_sigpending   127
#define SYS_rt_sigtimedwait 128
#define SYS_rt_sigqueueinfo 129
#define SYS_rt_sigsuspend   130
#define SYS_sigaltstack     131
#define SYS_utime           132
#define SYS_mknod           133
#define SYS_uselib          134
#define SYS_personality     135
#define SYS_ustat           136
#define SYS_statfs          137
#define SYS_fstatfs         138
#define SYS_sysfs           139
#define SYS_getpriority     140
#define SYS_setpriority     141
#define SYS_sched_setparam  142
#define SYS_sched_getparam  143
#define SYS_sched_setscheduler 144
#define SYS_sched_getscheduler 145
#define SYS_sched_get_priority_max 146
#define SYS_sched_get_priority_min 147
#define SYS_sched_rr_get_interval  148
#define SYS_mlock           149
#define SYS_munlock         150
#define SYS_mlockall        151
#define SYS_munlockall      152
#define SYS_vhangup         153
#define SYS_modify_ldt      154
#define SYS_pivot_root      155
#define SYS_prctl           157
#define SYS_arch_prctl      158
#define SYS_adjtimex        159
#define SYS_setrlimit       160
#define SYS_chroot          161
#define SYS_sync            162
#define SYS_acct            163
#define SYS_settimeofday    164
#define SYS_mount           165
#define SYS_umount2         166
#define SYS_swapon          167
#define SYS_swapoff         168
#define SYS_reboot          169
#define SYS_sethostname     170
#define SYS_setdomainname   171
#define SYS_iopl            172
#define SYS_ioperm          173
#define SYS_create_module   174
#define SYS_init_module     175
#define SYS_delete_module   176
#define SYS_get_kernel_syms 177
#define SYS_query_module    178
#define SYS_quotactl        179
#define SYS_nfsservctl      180
#define SYS_gettid          186
#define SYS_readahead       187
#define SYS_setxattr        188
#define SYS_lsetxattr       189
#define SYS_fsetxattr       190
#define SYS_getxattr        191
#define SYS_lgetxattr       192
#define SYS_fgetxattr       193
#define SYS_listxattr       194
#define SYS_llistxattr      195
#define SYS_flistxattr      196
#define SYS_removexattr     197
#define SYS_lremovexattr    198
#define SYS_fremovexattr    199
#define SYS_tkill           200
#define SYS_time            201
#define SYS_futex           202
#define SYS_sched_setaffinity 203
#define SYS_sched_getaffinity 204
#define SYS_set_thread_area 205
#define SYS_io_setup        206
#define SYS_io_destroy      207
#define SYS_io_getevents    208
#define SYS_io_submit       209
#define SYS_io_cancel       210
#define SYS_get_thread_area 211
#define SYS_lookup_dcookie  212
#define SYS_epoll_create    213
#define SYS_epoll_ctl_old   214
#define SYS_epoll_wait_old  215
#define SYS_remap_file_pages 216
#define SYS_getdents64      217
#define SYS_set_tid_address 218
#define SYS_restart_syscall 219
#define SYS_semtimedop      220
#define SYS_fadvise64       221
#define SYS_timer_create    222
#define SYS_timer_settime   223
#define SYS_timer_gettime   224
#define SYS_timer_getoverrun 225
#define SYS_timer_delete    226
#define SYS_clock_settime   227
#define SYS_clock_gettime   228
#define SYS_clock_getres    229
#define SYS_clock_nanosleep 230
#define SYS_exit_group      231
#define SYS_epoll_wait      232
#define SYS_epoll_ctl       233
#define SYS_tgkill          234
#define SYS_utimes          235
#define SYS_waitid          247
#define SYS_openat          257
#define SYS_mkdirat         258
#define SYS_mknodat         259
#define SYS_fchownat        260
#define SYS_futimesat       261
#define SYS_newfstatat      262
#define SYS_unlinkat        263
#define SYS_renameat        264
#define SYS_linkat          265
#define SYS_symlinkat       266
#define SYS_readlinkat      267
#define SYS_fchmodat        268
#define SYS_faccessat       269
#define SYS_pselect6        270
#define SYS_ppoll           271
#define SYS_unshare         272
#define SYS_set_robust_list 273
#define SYS_get_robust_list 274
#define SYS_splice          275
#define SYS_tee             276
#define SYS_sync_file_range 277
#define SYS_vmsplice        278
#define SYS_move_pages      279
#define SYS_utimensat       280
#define SYS_epoll_pwait     281
#define SYS_signalfd        282
#define SYS_timerfd_create  283
#define SYS_eventfd         284
#define SYS_fallocate       285
#define SYS_timerfd_settime 286
#define SYS_timerfd_gettime 287
#define SYS_accept4         288
#define SYS_signalfd4       289
#define SYS_eventfd2        290
#define SYS_epoll_create1   291
#define SYS_dup3            292
#define SYS_pipe2           293
#define SYS_inotify_init1   294
#define SYS_preadv          295
#define SYS_pwritev         296
#define SYS_rt_tgsigqueueinfo 297
#define SYS_perf_event_open 298
#define SYS_recvmmsg        299
#define SYS_fanotify_init   300
#define SYS_prlimit64       302
#define SYS_name_to_handle_at 303
#define SYS_open_by_handle_at 304
#define SYS_clock_adjtime   305
#define SYS_syncfs          306
#define SYS_sendmmsg        307
#define SYS_setns           308
#define SYS_getcpu          309
#define SYS_process_vm_readv  310
#define SYS_process_vm_writev 311
#define SYS_kcmp            312
#define SYS_finit_module    313
#define SYS_sched_setattr   314
#define SYS_sched_getattr   315
#define SYS_renameat2       316
#define SYS_seccomp         317
#define SYS_getrandom       318
#define SYS_memfd_create    319
#define SYS_kexec_file_load 320
#define SYS_bpf             321
#define SYS_execveat        322
#define SYS_userfaultfd     323
#define SYS_membarrier      324
#define SYS_mlock2          325
#define SYS_copy_file_range 326
#define SYS_preadv2         327
#define SYS_pwritev2        328

#define NR_SYSCALLS         400

/* arch_prctl サブコマンド */
#define ARCH_SET_FS    0x1002
#define ARCH_GET_FS    0x1003
#define ARCH_SET_GS    0x1001
#define ARCH_GET_GS    0x1004

/* prctl サブコマンド */
#define PR_SET_NAME    15
#define PR_GET_NAME    16
#define PR_SET_DUMPABLE 4
#define PR_GET_DUMPABLE 3

/* reboot magic */
#define LINUX_REBOOT_MAGIC1    0xfee1dead
#define LINUX_REBOOT_MAGIC2    672274793
#define LINUX_REBOOT_CMD_RESTART  0x01234567
#define LINUX_REBOOT_CMD_HALT     0xcdef0123
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321fedc

/* utsname */
struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

/* rlimit */
struct rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

#define RLIMIT_CPU        0
#define RLIMIT_FSIZE      1
#define RLIMIT_DATA       2
#define RLIMIT_STACK      3
#define RLIMIT_CORE       4
#define RLIMIT_RSS        5
#define RLIMIT_NPROC      6
#define RLIMIT_NOFILE     7
#define RLIMIT_MEMLOCK    8
#define RLIMIT_AS         9
#define RLIMIT_LOCKS      10
#define RLIMIT_SIGPENDING 11
#define RLIMIT_MSGQUEUE   12
#define RLIMIT_NICE       13
#define RLIMIT_RTPRIO     14
#define RLIM_NLIMITS      15
#define RLIM_INFINITY     (~0ULL)

/* sysinfo */
struct sysinfo {
    int64_t  uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
    char     _f[20 - 2 * sizeof(uint64_t) - sizeof(uint32_t)];
};

/* fcntl コマンド */
#define F_DUPFD     0
#define F_GETFD     1
#define F_SETFD     2
#define F_GETFL     3
#define F_SETFL     4
#define F_GETLK     5
#define F_SETLK     6
#define F_SETLKW    7
#define F_SETOWN    8
#define F_GETOWN    9
#define F_SETSIG    10
#define F_GETSIG    11
#define F_DUPFD_CLOEXEC 1030

/* シグナルマスク操作 */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* clone フラグ */
#define CLONE_VM       0x00000100
#define CLONE_FS       0x00000200
#define CLONE_FILES    0x00000400
#define CLONE_SIGHAND  0x00000800
#define CLONE_PTRACE   0x00002000
#define CLONE_VFORK    0x00004000
#define CLONE_PARENT   0x00008000
#define CLONE_THREAD   0x00010000
#define CLONE_NEWNS    0x00020000
#define CLONE_SYSVSEM  0x00040000
#define CLONE_SETTLS   0x00080000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_DETACHED 0x00400000
#define CLONE_UNTRACED 0x00800000
#define CLONE_CHILD_SETTID 0x01000000
#define CLONE_NEWCGROUP 0x02000000
#define CLONE_NEWUTS   0x04000000
#define CLONE_NEWIPC   0x08000000
#define CLONE_NEWUSER  0x10000000
#define CLONE_NEWPID   0x20000000
#define CLONE_NEWNET   0x40000000
#define CLONE_IO       0x80000000

/* iovec */
struct iovec {
    void   *iov_base;
    size_t  iov_len;
};

/* syscall ディスパッチャ初期化 */
void syscall_init(void);

/* syscallハンドラ型 */
typedef uint64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

/* システムコール登録 */
void syscall_register(int nr, syscall_fn_t fn);

/* メインディスパッチャ */
uint64_t syscall_dispatch(uint64_t nr,
                           uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6);

#endif /* _SYSCALL_H */
