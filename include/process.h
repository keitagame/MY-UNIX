#pragma once
#ifndef _PROCESS_H
#define _PROCESS_H

/* ============================================================
 * process.h - プロセス・スレッド管理ヘッダ
 * ============================================================ */

#include "types.h"
#include "kernel.h"
#include "mm.h"

/* ============================================================
 * x86_64 CPU コンテキスト
 * ============================================================ */

/* 割り込み/例外時のCPUレジスタ (スタック上の配置) */
typedef struct cpu_context {
    /* 汎用レジスタ (pushaq順) */
    uint64_t r15, r14, r13, r12;
    uint64_t r11, r10, r9,  r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    /* 割り込みフレーム */
    uint64_t int_no;   /* 割り込み番号 */
    uint64_t err_code; /* エラーコード */
    /* CPU自動プッシュ */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} PACKED cpu_context_t;

/* スレッドコンテキスト切り替え用 */
typedef struct thread_context {
    uint64_t rbx, rbp, r12, r13, r14, r15;
    uint64_t rsp;
    uint64_t rip;
    uint64_t rflags;
    /* FPU/SSE状態 */
    uint8_t  fpu_state[512] __attribute__((aligned(16)));
    bool     fpu_valid;
} thread_context_t;

/* ============================================================
 * シグナル
 * ============================================================ */
#define NSIG        64
#define SIGMIN      1
#define SIGMAX      NSIG

/* シグナル番号 */
#define SIGHUP      1
#define SIGINT      2
#define SIGQUIT     3
#define SIGILL      4
#define SIGTRAP     5
#define SIGABRT     6
#define SIGBUS      7
#define SIGFPE      8
#define SIGKILL     9
#define SIGUSR1     10
#define SIGSEGV     11
#define SIGUSR2     12
#define SIGPIPE     13
#define SIGALRM     14
#define SIGTERM     15
#define SIGSTKFLT   16
#define SIGCHLD     17
#define SIGCONT     18
#define SIGSTOP     19
#define SIGTSTP     20
#define SIGTTIN     21
#define SIGTTOU     22
#define SIGURG      23
#define SIGXCPU     24
#define SIGXFSZ     25
#define SIGVTALRM   26
#define SIGPROF     27
#define SIGWINCH    28
#define SIGIO       29
#define SIGPWR      30
#define SIGSYS      31

/* sigaction フラグ */
#define SA_NOCLDSTOP  1
#define SA_NOCLDWAIT  2
#define SA_SIGINFO    4
#define SA_ONSTACK    0x08000000
#define SA_RESTART    0x10000000
#define SA_NODEFER    0x40000000
#define SA_RESETHAND  0x80000000

/* シグナルハンドラ特殊値 */
#define SIG_DFL  ((sighandler_t)0)
#define SIG_IGN  ((sighandler_t)1)
#define SIG_ERR  ((sighandler_t)-1)

typedef void (*sighandler_t)(int);

typedef uint64_t sigset_t;  /* 最大64シグナル */

struct sigaction {
    sighandler_t sa_handler;
    sigset_t     sa_mask;
    uint32_t     sa_flags;
};

/* シグナルキュー要素 */
typedef struct siginfo {
    int      si_signo;
    int      si_errno;
    int      si_code;
    pid_t    si_pid;
    uid_t    si_uid;
    int      si_status;
    void    *si_addr;
    long     si_value;
} siginfo_t;

/* ============================================================
 * ファイル記述子テーブル
 * ============================================================ */
struct file;

typedef struct fd_table {
    struct file *fds[FD_MAX];
    uint8_t      flags[FD_MAX];  /* FD_CLOEXEC等 */
    spinlock_t   lock;
    atomic_t     refcount;
} fd_table_t;

#define FD_FLAG_CLOEXEC  1

fd_table_t *fd_table_create(void);
fd_table_t *fd_table_clone(fd_table_t *src);
void        fd_table_destroy(fd_table_t *fdt);
int         fd_alloc(fd_table_t *fdt, struct file *f);
int         fd_alloc_at(fd_table_t *fdt, int fd, struct file *f);
struct file *fd_get(fd_table_t *fdt, int fd);
int         fd_close(fd_table_t *fdt, int fd);
void        fd_close_on_exec(fd_table_t *fdt);

/* ============================================================
 * プロセス状態
 * ============================================================ */
typedef enum {
    PROC_CREATED    = 0,  /* 作成直後 */
    PROC_RUNNING    = 1,  /* 実行中 */
    PROC_READY      = 2,  /* 実行可能 */
    PROC_SLEEPING   = 3,  /* I/O等で待機 */
    PROC_STOPPED    = 4,  /* シグナルで停止 */
    PROC_ZOMBIE     = 5,  /* 終了待ち */
    PROC_DEAD       = 6,  /* 完全終了 */
} proc_state_t;

/* ============================================================
 * プロセス制御ブロック (PCB)
 * ============================================================ */
#define PROC_NAME_MAX  256
#define PROC_COMM_MAX  16

typedef struct process {
    /* アイデンティティ */
    pid_t    pid;
    pid_t    ppid;
    pid_t    pgid;       /* プロセスグループID */
    pid_t    sid;        /* セッションID */
    
    /* ユーザー情報 */
    uid_t    uid, euid, suid;
    gid_t    gid, egid, sgid;
    
    /* プロセス名 */
    char     name[PROC_NAME_MAX];
    char     comm[PROC_COMM_MAX];  /* 短縮コマンド名 */
    
    /* 状態 */
    proc_state_t state;
    int          exit_code;     /* 終了コード */
    bool         killed;        /* 強制終了フラグ */
    
    /* メモリ空間 */
    vmm_space_t *vm_space;
    uintptr_t    brk;           /* ヒープbreak */
    uintptr_t    brk_start;     /* ヒープ開始 */
    
    /* スレッドコンテキスト */
    thread_context_t ctx;
    uintptr_t        kernel_stack;      /* カーネルスタック */
    uintptr_t        kernel_stack_top;
    uintptr_t        user_stack_top;    /* ユーザースタックトップ */
    
    /* TSS RSP0 (syscall時のカーネルスタック) */
    uintptr_t        tss_rsp0;
    
    /* ファイルシステム */
    fd_table_t  *fd_table;
    struct vnode *cwd;          /* カレントディレクトリ */
    char         cwd_path[PATH_MAX];
    mode_t       umask;
    
    /* シグナル */
    struct sigaction sig_handlers[NSIG + 1];
    sigset_t         sig_pending;
    sigset_t         sig_blocked;
    sigset_t         sig_saved_mask;  /* sigsuspend用 */
    struct list_head sig_queue;       /* キューイングされたシグナル */
    spinlock_t       sig_lock;
    
    /* スケジューラ関連 */
    int       priority;     /* スケジューリング優先度 (0=最高, 39=最低) */
    int       nice;         /* nice値 (-20〜19) */
    uint64_t  time_slice;   /* 残りタイムスライス (tick) */
    uint64_t  total_time;   /* 累積CPU時間 */
    uint64_t  sched_time;   /* 最後にスケジュールされた時刻 */
    
    /* 待機 */
    wait_queue_t *wait_channel; /* 待機中のチャンネル */
    int           wait_errno;   /* 待機解除後のerrno */
    
    /* プロセスツリー */
    struct list_head sibling;   /* 兄弟プロセスリスト */
    struct list_head children;  /* 子プロセスリスト */
    struct process  *parent;
    
    /* スケジューラリスト */
    struct list_head run_list;  /* runqueueリスト */
    
    /* グローバルプロセスリスト */
    struct list_head all_list;
    
    /* 子プロセス終了通知 */
    wait_queue_t *wait_child;
    
    /* タイマー */
    uint64_t    alarm_time;     /* アラーム時刻 (ms) */
    
    /* アドレス空間情報 */
    uintptr_t   text_start, text_end;
    uintptr_t   data_start, data_end;
    
    /* 参照カウント */
    atomic_t    refcount;
    
    /* ロック */
    spinlock_t  lock;
    
} process_t;

/* ============================================================
 * プロセスAPI
 * ============================================================ */

/* 初期化 */
void proc_init(void);

/* プロセス作成/破棄 */
process_t *proc_create(const char *name);
process_t *proc_fork(process_t *parent);
void       proc_destroy(process_t *proc);

/* 参照カウント */
process_t *proc_get(pid_t pid);
void       proc_put(process_t *proc);

/* 状態遷移 */
void proc_set_state(process_t *proc, proc_state_t state);
void proc_sleep(wait_queue_t *wq);
int  proc_sleep_timeout(wait_queue_t *wq, uint64_t ms);
void proc_wake(process_t *proc);

/* プロセス終了 */
NORETURN void proc_exit(int code);
pid_t         proc_wait(pid_t pid, int *status, int options);

/* ELF実行 */
int proc_exec(process_t *proc, const char *path, 
              char *const argv[], char *const envp[]);

/* グローバル変数 */
extern process_t *g_current;   /* 現在実行中のプロセス */
extern pid_t      g_next_pid;

/* PID管理 */
pid_t    pid_alloc(void);
void     pid_free(pid_t pid);
process_t *pid_to_proc(pid_t pid);

/* wait オプション */
#define WNOHANG    1
#define WUNTRACED  2
#define WCONTINUED 8

#define WIFEXITED(s)    (((s) & 0x7f) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#define WIFSIGNALED(s)  (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WIFSTOPPED(s)   (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)     (((s) >> 8) & 0xff)

/* ============================================================
 * スケジューラAPI
 * ============================================================ */
void sched_init(void);
void sched_add(process_t *proc);
void sched_remove(process_t *proc);
void sched_yield(void);
void schedule(void);
void sched_tick(void);  /* タイマー割り込みから呼ぶ */

/* コンテキストスイッチ (asm) */
void context_switch(thread_context_t *from, thread_context_t *to);
void context_switch_to_user(cpu_context_t *ctx) NORETURN;

/* アイドルスレッド */
void idle_thread(void);

/* ============================================================
 * シグナルAPI
 * ============================================================ */
void signal_init_proc(process_t *proc);
int  signal_send(process_t *target, int signo, const siginfo_t *info);
int  signal_send_pid(pid_t pid, int signo);
int  signal_send_group(pid_t pgid, int signo);
void signal_handle(cpu_context_t *ctx);
int  signal_blocked(process_t *proc, int signo);
void signal_restore_mask(process_t *proc);

/* ============================================================
 * ELFローダー
 * ============================================================ */
typedef struct {
    uintptr_t entry;        /* エントリポイント */
    uintptr_t stack_top;    /* ユーザースタックトップ */
    uintptr_t text_start;
    uintptr_t text_end;
    uintptr_t data_start;
    uintptr_t data_end;
    uintptr_t brk;          /* ヒープ開始 (brk) */
} elf_load_info_t;

int elf_load(vmm_space_t *space, const char *path,
             char *const argv[], char *const envp[],
             elf_load_info_t *info);

#endif /* _PROCESS_H */
