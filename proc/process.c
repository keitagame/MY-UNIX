/* ============================================================
 * process.c - プロセス管理 (完全版)
 * fork / exec / wait / exit / signal
 * ============================================================ */

#include "../include/types.h"
#include "../include/kernel.h"
#include "../include/mm.h"
#include "../include/process.h"
#include "../include/fs.h"
#include "../include/syscall.h"

/* ============================================================
 * グローバル状態
 * ============================================================ */
process_t *g_current = NULL;
pid_t      g_next_pid = 1;

#define PID_MAX 65536
static process_t *g_pid_table[PID_MAX];
static spinlock_t g_pid_lock    = SPINLOCK_INIT;

static LIST_HEAD(g_all_procs);
static spinlock_t g_procs_lock  = SPINLOCK_INIT;

process_t *g_init_proc = NULL;

#define KERNEL_STACK_SIZE  (16 * 1024)

/* ============================================================
 * PID 管理
 * ============================================================ */
pid_t pid_alloc(void)
{
    spinlock_lock(&g_pid_lock);
    pid_t start = g_next_pid;
    do {
        if (g_next_pid >= PID_MAX) g_next_pid = 2;
        if (!g_pid_table[g_next_pid]) {
            pid_t pid = g_next_pid++;
            spinlock_unlock(&g_pid_lock);
            return pid;
        }
        g_next_pid++;
    } while (g_next_pid != start);
    spinlock_unlock(&g_pid_lock);
    return -1;
}

void pid_free(pid_t pid)
{
    if (pid <= 0 || pid >= PID_MAX) return;
    spinlock_lock(&g_pid_lock);
    g_pid_table[pid] = NULL;
    spinlock_unlock(&g_pid_lock);
}

process_t *pid_to_proc(pid_t pid)
{
    if (pid <= 0 || pid >= PID_MAX) return NULL;
    spinlock_lock(&g_pid_lock);
    process_t *p = g_pid_table[pid];
    if (p) atomic_inc(&p->refcount);
    spinlock_unlock(&g_pid_lock);
    return p;
}

process_t *proc_get(pid_t pid) { return pid_to_proc(pid); }

void proc_put(process_t *proc)
{
    if (!proc) return;
    if (atomic_dec_and_test(&proc->refcount))
        proc_destroy(proc);
}

/* ============================================================
 * proc_create - 新規プロセス生成
 * ============================================================ */
process_t *proc_create(const char *name)
{
    process_t *proc = kzalloc(sizeof(process_t));
    if (!proc) return NULL;

    proc->pid  = pid_alloc();
    if (proc->pid < 0) { kfree(proc); return NULL; }
    proc->ppid = 0;
    proc->pgid = proc->pid;
    proc->sid  = proc->pid;
    proc->uid  = proc->euid = proc->suid = 0;
    proc->gid  = proc->egid = proc->sgid = 0;

    strncpy(proc->name, name ? name : "?", PROC_NAME_MAX - 1);
    strncpy(proc->comm, name ? name : "?", PROC_COMM_MAX - 1);

    proc->state     = PROC_CREATED;
    proc->exit_code = 0;
    proc->killed    = false;

    /* アドレス空間 */
    proc->vm_space = vmm_create_space();
    if (!proc->vm_space) goto fail_vm;

    /* カーネルスタック */
    uintptr_t kstack_phys = pmm_alloc_n(KERNEL_STACK_SIZE / PAGE_SIZE);
    if (!kstack_phys) goto fail_stack;
    proc->kernel_stack     = (uintptr_t)phys_to_kvirt(kstack_phys);
    proc->kernel_stack_top = proc->kernel_stack + KERNEL_STACK_SIZE;
    proc->tss_rsp0         = proc->kernel_stack_top;

    /* FDテーブル */
    proc->fd_table = fd_table_create();
    if (!proc->fd_table) goto fail_fd;

    /* カレントディレクトリ */
    extern vnode_t *g_root_vnode;
    proc->cwd = g_root_vnode;
    strcpy(proc->cwd_path, "/");
    proc->umask = 022;

    /* スケジューラ */
    proc->priority   = 20;
    proc->nice       = 0;
    proc->time_slice = HZ / 10;

    /* リスト初期化 */
    INIT_LIST_HEAD(&proc->sibling);
    INIT_LIST_HEAD(&proc->children);
    INIT_LIST_HEAD(&proc->run_list);
    INIT_LIST_HEAD(&proc->all_list);
    INIT_LIST_HEAD(&proc->sig_queue);

    proc->wait_child = wq_create();
    signal_init_proc(proc);
    spinlock_init(&proc->lock);
    spinlock_init(&proc->sig_lock);
    atomic_set(&proc->refcount, 1);

    /* PIDテーブル & グローバルリスト */
    spinlock_lock(&g_pid_lock);
    g_pid_table[proc->pid] = proc;
    spinlock_unlock(&g_pid_lock);

    spinlock_lock(&g_procs_lock);
    list_add_tail(&proc->all_list, &g_all_procs);
    spinlock_unlock(&g_procs_lock);

    return proc;

fail_fd:
    pmm_free_n(kstack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
fail_stack:
    vmm_destroy_space(proc->vm_space);
fail_vm:
    pid_free(proc->pid);
    kfree(proc);
    return NULL;
}

/* ============================================================
 * proc_destroy - プロセスリソース解放
 * ============================================================ */
void proc_destroy(process_t *proc)
{
    if (!proc) return;

    /* グローバルリストから削除 */
    spinlock_lock(&g_procs_lock);
    list_del(&proc->all_list);
    spinlock_unlock(&g_procs_lock);

    pid_free(proc->pid);

    if (proc->vm_space) vmm_destroy_space(proc->vm_space);
    if (proc->fd_table) fd_table_destroy(proc->fd_table);
    if (proc->wait_child) wq_destroy(proc->wait_child);

    pmm_free_n(kvirt_to_phys((void*)proc->kernel_stack),
               KERNEL_STACK_SIZE / PAGE_SIZE);
    kfree(proc);
}

/* ============================================================
 * proc_fork
 * ============================================================ */
process_t *proc_fork(process_t *parent)
{
    process_t *child = kzalloc(sizeof(process_t));
    if (!child) return NULL;

    child->pid  = pid_alloc();
    if (child->pid < 0) { kfree(child); return NULL; }
    child->ppid = parent->pid;
    child->pgid = parent->pgid;
    child->sid  = parent->sid;

    child->uid  = parent->uid;  child->euid = parent->euid; child->suid = parent->suid;
    child->gid  = parent->gid;  child->egid = parent->egid; child->sgid = parent->sgid;

    strncpy(child->name, parent->name, PROC_NAME_MAX - 1);
    strncpy(child->comm, parent->comm, PROC_COMM_MAX - 1);

    child->state     = PROC_CREATED;
    child->exit_code = 0;
    child->killed    = false;
    child->umask     = parent->umask;

    /* アドレス空間をコピー (COW) */
    child->vm_space = vmm_clone_space(parent->vm_space);
    if (!child->vm_space) goto fail;
    child->brk       = parent->brk;
    child->brk_start = parent->brk_start;

    /* カーネルスタック */
    uintptr_t kstack_phys = pmm_alloc_n(KERNEL_STACK_SIZE / PAGE_SIZE);
    if (!kstack_phys) goto fail_vm;
    child->kernel_stack     = (uintptr_t)phys_to_kvirt(kstack_phys);
    child->kernel_stack_top = child->kernel_stack + KERNEL_STACK_SIZE;
    child->tss_rsp0         = child->kernel_stack_top;

    /* 親のコンテキストコピー */
    child->ctx           = parent->ctx;
    child->user_stack_top = parent->user_stack_top;

    /* FDテーブルをクローン */
    child->fd_table = fd_table_clone(parent->fd_table);
    if (!child->fd_table) goto fail_stack;

    /* カレントディレクトリ */
    child->cwd = parent->cwd;
    if (child->cwd) vnode_get(child->cwd);
    strcpy(child->cwd_path, parent->cwd_path);

    /* シグナルハンドラコピー (execはリセット) */
    memcpy(child->sig_handlers, parent->sig_handlers,
           sizeof(child->sig_handlers));
    child->sig_pending  = 0;          /* 子にはペンディングシグナルなし */
    child->sig_blocked  = parent->sig_blocked;

    child->priority   = parent->priority;
    child->nice       = parent->nice;
    child->time_slice = HZ / 10;

    INIT_LIST_HEAD(&child->sibling);
    INIT_LIST_HEAD(&child->children);
    INIT_LIST_HEAD(&child->run_list);
    INIT_LIST_HEAD(&child->all_list);
    INIT_LIST_HEAD(&child->sig_queue);

    child->wait_child = wq_create();
    spinlock_init(&child->lock);
    spinlock_init(&child->sig_lock);
    atomic_set(&child->refcount, 1);

    /* 親子関係 */
    spinlock_lock(&parent->lock);
    child->parent = parent;
    list_add_tail(&child->sibling, &parent->children);
    spinlock_unlock(&parent->lock);

    /* PIDテーブル & グローバル */
    spinlock_lock(&g_pid_lock);
    g_pid_table[child->pid] = child;
    spinlock_unlock(&g_pid_lock);

    spinlock_lock(&g_procs_lock);
    list_add_tail(&child->all_list, &g_all_procs);
    spinlock_unlock(&g_procs_lock);

    return child;

fail_stack:
    pmm_free_n(kstack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
fail_vm:
    vmm_destroy_space(child->vm_space);
fail:
    pid_free(child->pid);
    kfree(child);
    return NULL;
}

/* ============================================================
 * proc_exit
 * ============================================================ */
NORETURN void proc_exit(int code)
{
    process_t *proc = g_current;
    KASSERT(proc != NULL, "proc_exit with no current process");

    uint64_t flags;
    IRQ_SAVE(flags);

    proc->exit_code = (code & 0xFF) << 8;
    proc->state     = PROC_ZOMBIE;

    /* 全FDをクローズ */
    if (proc->fd_table) {
        for (int i = 0; i < FD_MAX; i++) {
            fd_close(proc->fd_table, i);
        }
    }

    /* 子プロセスをinitに再親付け */
    spinlock_lock(&proc->lock);
    struct list_head *pos, *n;
    list_for_each_safe(pos, n, &proc->children) {
        process_t *child = list_entry(pos, process_t, sibling);
        list_del(&child->sibling);
        if (g_init_proc && proc != g_init_proc) {
            spinlock_lock(&g_init_proc->lock);
            child->parent = g_init_proc;
            child->ppid   = g_init_proc->pid;
            list_add_tail(&child->sibling, &g_init_proc->children);
            spinlock_unlock(&g_init_proc->lock);
        }
    }
    spinlock_unlock(&proc->lock);

    /* 親にSIGCHLDを送る */
    process_t *parent = proc->parent;
    if (parent) {
        signal_send(parent, SIGCHLD, NULL);
        wq_wake_all(parent->wait_child);
    }

    /* スケジューラから削除して次のプロセスへ */
    sched_remove(proc);

    IRQ_RESTORE(flags);

    schedule();

    /* ここには到達しない */
    cpu_halt();
}

/* ============================================================
 * proc_wait
 * ============================================================ */
pid_t proc_wait(pid_t target_pid, int *status, int options)
{
    process_t *proc = g_current;

    for (;;) {
        /* 子プロセスをスキャン */
        spinlock_lock(&proc->lock);

        process_t *found = NULL;
        bool has_children = false;

        struct list_head *pos;
        list_for_each(pos, &proc->children) {
            process_t *child = list_entry(pos, process_t, sibling);
            has_children = true;

            if (target_pid > 0 && child->pid != target_pid) continue;
            if (target_pid == 0 && child->pgid != proc->pgid) continue;
            if (target_pid < -1 && child->pgid != -target_pid) continue;

            if (child->state == PROC_ZOMBIE) {
                found = child;
                break;
            }
        }

        spinlock_unlock(&proc->lock);

        if (!has_children) return -ECHILD;

        if (found) {
            pid_t pid = found->pid;
            if (status) *status = found->exit_code;

            /* 子をリストから除去 */
            spinlock_lock(&proc->lock);
            list_del(&found->sibling);
            spinlock_unlock(&proc->lock);

            proc_destroy(found);
            return pid;
        }

        if (options & WNOHANG) return 0;

        /* 子の終了を待つ */
        proc_sleep(proc->wait_child);

        if (proc->killed) return -EINTR;
    }
}

/* ============================================================
 * proc_sleep / proc_wake
 * ============================================================ */
void proc_set_state(process_t *proc, proc_state_t state)
{
    proc->state = state;
}

void proc_sleep(wait_queue_t *wq)
{
    process_t *proc = g_current;
    proc->state        = PROC_SLEEPING;
    proc->wait_channel = wq;
    wq_wait(wq);
    proc->wait_channel = NULL;
    proc->state        = PROC_RUNNING;
}

int proc_sleep_timeout(wait_queue_t *wq, uint64_t ms)
{
    process_t *proc = g_current;
    proc->state        = PROC_SLEEPING;
    proc->wait_channel = wq;
    int r = wq_wait_timeout(wq, ms);
    proc->wait_channel = NULL;
    proc->state        = PROC_RUNNING;
    return r;
}

void proc_wake(process_t *proc)
{
    if (proc->state == PROC_SLEEPING) {
        proc->state = PROC_READY;
        if (proc->wait_channel)
            wq_wake_one(proc->wait_channel);
        sched_add(proc);
    }
}

/* ============================================================
 * 初期化
 * ============================================================ */
void proc_init(void)
{
    memset(g_pid_table, 0, sizeof(g_pid_table));
    g_next_pid = 1;
    INIT_LIST_HEAD(&g_all_procs);
    printk(KERN_INFO "Proc: initialized (PID_MAX=%d)\n", PID_MAX);
}

/* ============================================================
 * FDテーブル実装
 * ============================================================ */
fd_table_t *fd_table_create(void)
{
    fd_table_t *fdt = kzalloc(sizeof(fd_table_t));
    if (!fdt) return NULL;
    spinlock_init(&fdt->lock);
    atomic_set(&fdt->refcount, 1);
    return fdt;
}

fd_table_t *fd_table_clone(fd_table_t *src)
{
    fd_table_t *dst = kzalloc(sizeof(fd_table_t));
    if (!dst) return NULL;

    spinlock_lock(&src->lock);
    for (int i = 0; i < FD_MAX; i++) {
        if (src->fds[i]) {
            dst->fds[i]   = src->fds[i];
            dst->flags[i] = src->flags[i];
            file_get(dst->fds[i]);
        }
    }
    spinlock_unlock(&src->lock);

    spinlock_init(&dst->lock);
    atomic_set(&dst->refcount, 1);
    return dst;
}

void fd_table_destroy(fd_table_t *fdt)
{
    if (!fdt) return;
    if (!atomic_dec_and_test(&fdt->refcount)) return;

    for (int i = 0; i < FD_MAX; i++) {
        if (fdt->fds[i]) {
            file_put(fdt->fds[i]);
            fdt->fds[i] = NULL;
        }
    }
    kfree(fdt);
}

int fd_alloc(fd_table_t *fdt, file_t *f)
{
    spinlock_lock(&fdt->lock);
    for (int i = 0; i < FD_MAX; i++) {
        if (!fdt->fds[i]) {
            fdt->fds[i]   = f;
            fdt->flags[i] = 0;
            file_get(f);
            spinlock_unlock(&fdt->lock);
            return i;
        }
    }
    spinlock_unlock(&fdt->lock);
    return -EMFILE;
}

int fd_alloc_at(fd_table_t *fdt, int fd, file_t *f)
{
    if (fd < 0 || fd >= FD_MAX) return -EBADF;
    spinlock_lock(&fdt->lock);
    if (fdt->fds[fd]) file_put(fdt->fds[fd]);
    fdt->fds[fd]   = f;
    fdt->flags[fd] = 0;
    file_get(f);
    spinlock_unlock(&fdt->lock);
    return fd;
}

file_t *fd_get(fd_table_t *fdt, int fd)
{
    if (fd < 0 || fd >= FD_MAX) return NULL;
    spinlock_lock(&fdt->lock);
    file_t *f = fdt->fds[fd];
    if (f) file_get(f);
    spinlock_unlock(&fdt->lock);
    return f;
}

int fd_close(fd_table_t *fdt, int fd)
{
    if (fd < 0 || fd >= FD_MAX) return -EBADF;
    spinlock_lock(&fdt->lock);
    file_t *f = fdt->fds[fd];
    if (!f) { spinlock_unlock(&fdt->lock); return -EBADF; }
    fdt->fds[fd]   = NULL;
    fdt->flags[fd] = 0;
    spinlock_unlock(&fdt->lock);
    file_put(f);
    return 0;
}

void fd_close_on_exec(fd_table_t *fdt)
{
    spinlock_lock(&fdt->lock);
    for (int i = 0; i < FD_MAX; i++) {
        if (fdt->fds[i] && (fdt->flags[i] & FD_FLAG_CLOEXEC)) {
            file_put(fdt->fds[i]);
            fdt->fds[i]   = NULL;
            fdt->flags[i] = 0;
        }
    }
    spinlock_unlock(&fdt->lock);
}

/* ============================================================
 * シグナル初期化
 * ============================================================ */
void signal_init_proc(process_t *proc)
{
    memset(proc->sig_handlers, 0, sizeof(proc->sig_handlers));
    proc->sig_pending    = 0;
    proc->sig_blocked    = 0;
    proc->sig_saved_mask = 0;
    INIT_LIST_HEAD(&proc->sig_queue);
    spinlock_init(&proc->sig_lock);
}

/* ============================================================
 * シグナル送信
 * ============================================================ */
int signal_send(process_t *target, int signo, const siginfo_t *info)
{
    if (!target || signo <= 0 || signo > NSIG) return -EINVAL;
    if (target->state == PROC_ZOMBIE || target->state == PROC_DEAD)
        return -ESRCH;

    spinlock_lock(&target->sig_lock);
    target->sig_pending |= BIT(signo - 1);
    spinlock_unlock(&target->sig_lock);

    if (target->state == PROC_SLEEPING)
        proc_wake(target);

    return 0;
}

int signal_send_pid(pid_t pid, int signo)
{
    process_t *proc = pid_to_proc(pid);
    if (!proc) return -ESRCH;
    int r = signal_send(proc, signo, NULL);
    proc_put(proc);
    return r;
}

int signal_send_group(pid_t pgid, int signo)
{
    spinlock_lock(&g_procs_lock);
    struct list_head *pos;
    list_for_each(pos, &g_all_procs) {
        process_t *p = list_entry(pos, process_t, all_list);
        if (p->pgid == pgid)
            signal_send(p, signo, NULL);
    }
    spinlock_unlock(&g_procs_lock);
    return 0;
}

/* ============================================================
 * シグナルハンドリング (ユーザーモード復帰時)
 * ============================================================ */
void signal_handle(cpu_context_t *ctx)
{
    process_t *proc = g_current;
    if (!proc) return;

    spinlock_lock(&proc->sig_lock);
    uint64_t pending = proc->sig_pending & ~proc->sig_blocked;
    spinlock_unlock(&proc->sig_lock);

    if (!pending) return;

    /* 最優先のシグナルを処理 */
    int signo = __builtin_ctzll(pending) + 1;

    spinlock_lock(&proc->sig_lock);
    proc->sig_pending &= ~BIT(signo - 1);
    spinlock_unlock(&proc->sig_lock);

    struct sigaction *sa = &proc->sig_handlers[signo];

    if (sa->sa_handler == SIG_IGN) return;
    if (sa->sa_handler == SIG_DFL) {
        /* デフォルト動作 */
        switch (signo) {
        case SIGCHLD:
        case SIGURG:
        case SIGWINCH:
            return;  /* 無視 */
        case SIGSTOP:
        case SIGTSTP:
        case SIGTTIN:
        case SIGTTOU:
            proc->state = PROC_STOPPED;
            schedule();
            return;
        case SIGCONT:
            proc->state = PROC_RUNNING;
            return;
        default:
            /* 終了 */
            proc->exit_code = signo;
            proc_exit(signo);
            break;
        }
        return;
    }

    /* ユーザーシグナルハンドラを呼び出す
     * ユーザースタックにシグナルフレームをプッシュ */
    if ((ctx->cs & 3) != 3) return;  /* カーネルモードからは呼べない */

    /* シグナルフレーム構造 */
    typedef struct {
        uint64_t ret_addr;    /* sigreturn のアドレス */
        cpu_context_t saved_ctx;
        siginfo_t info;
    } sigframe_t;

    uintptr_t sp = ctx->rsp - sizeof(sigframe_t) - 128; /* red zone */
    sp = ALIGN_DOWN(sp, 16);

    if (!access_ok((void*)sp, sizeof(sigframe_t))) {
        signal_send(proc, SIGSEGV, NULL);
        return;
    }

    /* シグナルフレームをユーザースタックに書く */
    sigframe_t *frame = (sigframe_t *)sp;
    frame->saved_ctx = *ctx;

    /* sigreturn トランポリン (実際はvDSOに置く) */
    frame->ret_addr = 0; /* TODO: vDSO sigreturn address */

    /* ハンドラを呼ぶようにコンテキストを書き換え */
    ctx->rip = (uint64_t)sa->sa_handler;
    ctx->rsp = sp;
    ctx->rdi = signo;   /* arg1: signum */
    ctx->rsi = 0;       /* arg2: siginfo (TODO) */
    ctx->rdx = 0;       /* arg3: ucontext (TODO) */

    /* SA_RESETHAND */
    if (sa->sa_flags & SA_RESETHAND) {
        sa->sa_handler = SIG_DFL;
        sa->sa_flags   = 0;
    }

    /* シグナルマスク */
    spinlock_lock(&proc->sig_lock);
    proc->sig_saved_mask = proc->sig_blocked;
    proc->sig_blocked   |= sa->sa_mask | BIT(signo - 1);
    spinlock_unlock(&proc->sig_lock);
}

int signal_blocked(process_t *proc, int signo)
{
    return !!(proc->sig_blocked & BIT(signo - 1));
}

void signal_restore_mask(process_t *proc)
{
    spinlock_lock(&proc->sig_lock);
    proc->sig_blocked = proc->sig_saved_mask;
    spinlock_unlock(&proc->sig_lock);
}
