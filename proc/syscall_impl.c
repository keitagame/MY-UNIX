/* ============================================================
 * syscall_impl.c - システムコール実装
 * Linux x86_64 ABI 互換
 * ============================================================ */

#include "../include/types.h"
#include "../include/kernel.h"
#include "../include/mm.h"
#include "../include/process.h"
#include "../include/fs.h"
#include "../include/syscall.h"

/* ============================================================
 * syscallテーブル
 * ============================================================ */
static syscall_fn_t g_syscall_table[NR_SYSCALLS];
extern vnode_t *g_root_vnode;
void syscall_register(int nr, syscall_fn_t fn)
{
    if (nr >= 0 && nr < NR_SYSCALLS)
        g_syscall_table[nr] = fn;
}

uint64_t syscall_dispatch(uint64_t nr,
                           uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    if (nr >= NR_SYSCALLS || !g_syscall_table[nr]) {
        printk(KERN_DEBUG "syscall: unimplemented nr=%llu pid=%d\n",
               (unsigned long long)nr,
               g_current ? g_current->pid : -1);
        return (uint64_t)-ENOSYS;
    }
    return g_syscall_table[nr](a1, a2, a3, a4, a5, a6);
}

/* ============================================================
 * ヘルパーマクロ
 * ============================================================ */
#define PROC  g_current
#define FDT   (PROC->fd_table)

static file_t *get_file(int fd)
{
    if (!PROC || !FDT) return NULL;
    return fd_get(FDT, fd);
}

/* ============================================================
 * read (0)
 * ============================================================ */
static uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a4); UNUSED(a5); UNUSED(a6);
    if (!access_ok((void*)buf, len)) return (uint64_t)-EFAULT;

    file_t *f = get_file((int)fd);
    if (!f) return (uint64_t)-EBADF;

    ssize_t n;
    if (f->ops && f->ops->read) {
        n = f->ops->read(f, (void*)buf, (size_t)len);
    } else if (f->vnode && f->vnode->ops && f->vnode->ops->read) {
        n = f->vnode->ops->read(f->vnode, (void*)buf, (size_t)len, f->offset);
        if (n > 0) f->offset += n;
    } else {
        n = -EINVAL;
    }
    file_put(f);
    return (uint64_t)n;
}

/* ============================================================
 * write (1)
 * ============================================================ */
static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a4); UNUSED(a5); UNUSED(a6);
    if (!access_ok((void*)buf, len)) return (uint64_t)-EFAULT;

    file_t *f = get_file((int)fd);
    if (!f) return (uint64_t)-EBADF;

    ssize_t n;
    if (f->ops && f->ops->write) {
        n = f->ops->write(f, (const void*)buf, (size_t)len);
    } else if (f->vnode && f->vnode->ops && f->vnode->ops->write) {
        n = f->vnode->ops->write(f->vnode, (const void*)buf, (size_t)len, f->offset);
        if (n > 0) f->offset += n;
    } else {
        n = -EINVAL;
    }
    file_put(f);
    return (uint64_t)n;
}

/* ============================================================
 * open (2)
 * ============================================================ */
static uint64_t sys_open(uint64_t path_ptr, uint64_t flags, uint64_t mode,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a4); UNUSED(a5); UNUSED(a6);
    char path[PATH_MAX];
    ssize_t r = strncpy_from_user(path, (const char*)path_ptr, PATH_MAX);
    if (r < 0) return (uint64_t)r;

    file_t *f = NULL;
    int err = vfs_open(path, (int)flags, (mode_t)mode, &f);
    if (err < 0) return (uint64_t)err;

    int fd = fd_alloc(FDT, f);
    file_put(f);
    if (fd < 0) return (uint64_t)fd;
    return (uint64_t)fd;
}

/* ============================================================
 * close (3)
 * ============================================================ */
static uint64_t sys_close(uint64_t fd, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a2); UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    return (uint64_t)fd_close(FDT, (int)fd);
}

/* ============================================================
 * stat (4) / fstat (5) / lstat (6)
 * ============================================================ */
static int do_stat(vnode_t *vn, uint64_t stat_ptr)
{
    struct stat st;
    int r = 0;
    if (vn->ops && vn->ops->stat) {
        r = vn->ops->stat(vn, &st);
    } else {
        memset(&st, 0, sizeof(st));
        st.st_mode = vn->mode;
        st.st_size = vn->size;
        st.st_ino  = vn->ino;
    }
    if (r < 0) return r;
    return copy_to_user((void*)stat_ptr, &st, sizeof(st));
}

static uint64_t sys_stat(uint64_t path_ptr, uint64_t stat_ptr, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    char path[PATH_MAX];
    ssize_t r = strncpy_from_user(path, (const char*)path_ptr, PATH_MAX);
    if (r < 0) return (uint64_t)r;

    vnode_t *vn = NULL;
    int err = vfs_lookup(g_root_vnode, path, &vn);
    if (err < 0) return (uint64_t)err;
    err = do_stat(vn, stat_ptr);
    vnode_put(vn);
    return (uint64_t)err;
}

static uint64_t sys_fstat(uint64_t fd, uint64_t stat_ptr, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    file_t *f = get_file((int)fd);
    if (!f) return (uint64_t)-EBADF;
    int r = do_stat(f->vnode, stat_ptr);
    file_put(f);
    return (uint64_t)r;
}

/* ============================================================
 * lseek (8)
 * ============================================================ */
static uint64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a4); UNUSED(a5); UNUSED(a6);
    file_t *f = get_file((int)fd);
    if (!f) return (uint64_t)-EBADF;

    off_t new_off;
    spinlock_lock(&f->lock);
    switch ((int)whence) {
    case SEEK_SET: new_off = (off_t)offset; break;
    case SEEK_CUR: new_off = f->offset + (off_t)offset; break;
    case SEEK_END: new_off = (off_t)(f->vnode ? f->vnode->size : 0) + (off_t)offset; break;
    default:       spinlock_unlock(&f->lock); file_put(f); return (uint64_t)-EINVAL;
    }
    if (new_off < 0) { spinlock_unlock(&f->lock); file_put(f); return (uint64_t)-EINVAL; }
    f->offset = new_off;
    spinlock_unlock(&f->lock);

    file_put(f);
    return (uint64_t)new_off;
}

/* ============================================================
 * mmap (9)
 * ============================================================ */
static uint64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                          uint64_t flags, uint64_t fd, uint64_t offset)
{
    if (length == 0) return (uint64_t)MAP_FAILED;

    size_t len = ALIGN_UP(length, PAGE_SIZE);
    uint64_t vma_flags = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE) vma_flags |= PTE_WRITABLE;
    if (!(prot & PROT_EXEC)) vma_flags |= PTE_NX;

    uintptr_t hint = (uintptr_t)addr;

    if (flags & MAP_ANONYMOUS) {
        uintptr_t vaddr = vmm_alloc_user(PROC->vm_space, hint, len, vma_flags);
        if (!vaddr) return (uint64_t)MAP_FAILED;
        vmm_create_vma(PROC->vm_space, vaddr, len, (uint32_t)prot, (uint32_t)flags);
        return vaddr;
    }

    /* ファイルマッピング */
    file_t *f = get_file((int)fd);
    if (!f) return (uint64_t)-EBADF;

    uintptr_t vaddr = vmm_alloc_user(PROC->vm_space, hint, len, vma_flags);
    if (!vaddr) { file_put(f); return (uint64_t)MAP_FAILED; }

    /* ファイルデータをページにコピー */
    uint8_t *tmp = kmalloc(len);
    if (tmp) {
        ssize_t n = 0;
        if (f->vnode && f->vnode->ops && f->vnode->ops->read)
            n = f->vnode->ops->read(f->vnode, tmp, len, (off_t)offset);
        if (n < 0) n = 0;
        /* ユーザーページに書き込む */
        for (size_t i = 0; i < len; i += PAGE_SIZE) {
            uintptr_t phys = vmm_virt_to_phys(PROC->vm_space, vaddr + i);
            if (phys) {
                void *kp = phys_to_kvirt(phys);
                size_t sz = MIN(PAGE_SIZE, len - i);
                size_t copy_sz = (i < (size_t)n) ? MIN(sz, (size_t)n - i) : 0;
                if (copy_sz) memcpy(kp, tmp + i, copy_sz);
                if (copy_sz < sz) memset((uint8_t*)kp + copy_sz, 0, sz - copy_sz);
            }
        }
        kfree(tmp);
    }

    file_put(f);
    vmm_create_vma(PROC->vm_space, vaddr, len, (uint32_t)prot, (uint32_t)flags);
    return vaddr;
}

/* ============================================================
 * munmap (11)
 * ============================================================ */
static uint64_t sys_munmap(uint64_t addr, uint64_t length,
                            uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    size_t len = ALIGN_UP(length, PAGE_SIZE);
    return (uint64_t)vmm_free_user(PROC->vm_space, (uintptr_t)addr, len);
}

/* ============================================================
 * brk (12)
 * ============================================================ */
static uint64_t sys_brk(uint64_t new_brk, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a2); UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    uintptr_t old_brk = PROC->brk;

    if (new_brk == 0) return old_brk;
    new_brk = ALIGN_UP(new_brk, PAGE_SIZE);

    if (new_brk > old_brk) {
        /* ヒープ拡張 */
        for (uintptr_t p = old_brk; p < new_brk; p += PAGE_SIZE) {
            uintptr_t phys = pmm_alloc();
            if (!phys) return old_brk;
            vmm_map(PROC->vm_space, p, phys, VMM_USER_RW);
        }
    } else if (new_brk < old_brk) {
        /* ヒープ縮小 */
        for (uintptr_t p = new_brk; p < old_brk; p += PAGE_SIZE) {
            uintptr_t phys = vmm_virt_to_phys(PROC->vm_space, p);
            vmm_unmap(PROC->vm_space, p);
            if (phys) pmm_free(phys);
        }
    }

    PROC->brk = new_brk;
    return new_brk;
}

/* ============================================================
 * rt_sigaction (13)
 * ============================================================ */
static uint64_t sys_rt_sigaction(uint64_t signo, uint64_t act_ptr,
                                  uint64_t oldact_ptr, uint64_t sigsetsize,
                                  uint64_t a5, uint64_t a6)
{
    UNUSED(sigsetsize); UNUSED(a5); UNUSED(a6);
    if (signo < 1 || signo > NSIG) return (uint64_t)-EINVAL;
    if (signo == SIGKILL || signo == SIGSTOP) return (uint64_t)-EINVAL;

    if (oldact_ptr) {
        copy_to_user((void*)oldact_ptr, &PROC->sig_handlers[signo],
                     sizeof(struct sigaction));
    }
    if (act_ptr) {
        struct sigaction sa;
        int r = copy_from_user(&sa, (void*)act_ptr, sizeof(sa));
        if (r < 0) return (uint64_t)r;
        PROC->sig_handlers[signo] = sa;
    }
    return 0;
}

/* ============================================================
 * rt_sigprocmask (14)
 * ============================================================ */
static uint64_t sys_rt_sigprocmask(uint64_t how, uint64_t set_ptr,
                                    uint64_t oldset_ptr, uint64_t sigsetsize,
                                    uint64_t a5, uint64_t a6)
{
    UNUSED(sigsetsize); UNUSED(a5); UNUSED(a6);

    if (oldset_ptr) {
        copy_to_user((void*)oldset_ptr, &PROC->sig_blocked, sizeof(sigset_t));
    }
    if (set_ptr) {
        sigset_t new_mask;
        int r = copy_from_user(&new_mask, (void*)set_ptr, sizeof(sigset_t));
        if (r < 0) return (uint64_t)r;

        /* SIGKILL と SIGSTOP はブロックできない */
        new_mask &= ~(BIT(SIGKILL-1) | BIT(SIGSTOP-1));

        switch ((int)how) {
        case SIG_BLOCK:   PROC->sig_blocked |= new_mask;  break;
        case SIG_UNBLOCK: PROC->sig_blocked &= ~new_mask; break;
        case SIG_SETMASK: PROC->sig_blocked  = new_mask;  break;
        default: return (uint64_t)-EINVAL;
        }
    }
    return 0;
}

/* ============================================================
 * ioctl (16)
 * ============================================================ */
static uint64_t sys_ioctl(uint64_t fd, uint64_t req, uint64_t arg,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a4); UNUSED(a5); UNUSED(a6);
    file_t *f = get_file((int)fd);
    if (!f) return (uint64_t)-EBADF;

    int r = -ENOTTY;
    if (f->ops && f->ops->ioctl) {
        r = f->ops->ioctl(f, (unsigned long)req, (void*)arg);
    } else if (f->vnode && f->vnode->ops && f->vnode->ops->ioctl) {
        r = f->vnode->ops->ioctl(f->vnode, (unsigned long)req, (void*)arg);
    }
    file_put(f);
    return (uint64_t)r;
}

/* ============================================================
 * pipe (22)
 * ============================================================ */
static uint64_t sys_pipe(uint64_t pipefd_ptr, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a2); UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    file_t *read_f, *write_f;
    int r = pipe_create(&read_f, &write_f);
    if (r < 0) return (uint64_t)r;

    int fds[2];
    fds[0] = fd_alloc(FDT, read_f);
    fds[1] = fd_alloc(FDT, write_f);
    file_put(read_f);
    file_put(write_f);

    if (fds[0] < 0 || fds[1] < 0) {
        if (fds[0] >= 0) fd_close(FDT, fds[0]);
        if (fds[1] >= 0) fd_close(FDT, fds[1]);
        return (uint64_t)-EMFILE;
    }

    return copy_to_user((void*)pipefd_ptr, fds, sizeof(fds)) < 0 ?
           (uint64_t)-EFAULT : 0;
}

/* ============================================================
 * sched_yield (24)
 * ============================================================ */
static uint64_t sys_sched_yield(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a1); UNUSED(a2); UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    sched_yield();
    return 0;
}

/* ============================================================
 * dup (32) / dup2 (33)
 * ============================================================ */
static uint64_t sys_dup(uint64_t fd, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a2); UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    file_t *f = get_file((int)fd);
    if (!f) return (uint64_t)-EBADF;
    int new_fd = fd_alloc(FDT, f);
    file_put(f);
    return (uint64_t)new_fd;
}

static uint64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    if ((int)oldfd == (int)newfd) return newfd;
    file_t *f = get_file((int)oldfd);
    if (!f) return (uint64_t)-EBADF;
    fd_alloc_at(FDT, (int)newfd, f);
    file_put(f);
    return newfd;
}

/* ============================================================
 * nanosleep (35)
 * ============================================================ */
static uint64_t sys_nanosleep(uint64_t req_ptr, uint64_t rem_ptr,
                               uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    struct timespec req;
    if (copy_from_user(&req, (void*)req_ptr, sizeof(req)) < 0)
        return (uint64_t)-EFAULT;

    uint64_t ms = (uint64_t)req.tv_sec * 1000 + (uint64_t)req.tv_nsec / 1000000;
    timer_sleep(ms);
    UNUSED(rem_ptr);
    return 0;
}

/* ============================================================
 * getpid (39)
 * ============================================================ */
static uint64_t sys_getpid(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a1); UNUSED(a2); UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    return (uint64_t)PROC->pid;
}

/* ============================================================
 * fork (57)
 * ============================================================ */
static uint64_t sys_fork(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a1); UNUSED(a2); UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    process_t *child = proc_fork(PROC);
    if (!child) return (uint64_t)-ENOMEM;

    /* 子プロセスのコンテキスト: syscallの戻り値を0に */
    /* カーネルスタックにCPUコンテキストをセットアップ */
    /* (context_switch から戻ったとき RAX=0 になるよう設定) */

    child->state = PROC_READY;
    sched_add(child);

    return (uint64_t)child->pid;  /* 親には子のPIDが返る */
}

/* ============================================================
 * execve (59)
 * ============================================================ */
static uint64_t sys_execve(uint64_t path_ptr, uint64_t argv_ptr, uint64_t envp_ptr,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a4); UNUSED(a5); UNUSED(a6);
    char path[PATH_MAX];
    ssize_t r = strncpy_from_user(path, (const char*)path_ptr, PATH_MAX);
    if (r < 0) return (uint64_t)r;

    /* argv/envp をカーネルバッファにコピー */
    #define MAX_ARGS 256
    char *argv[MAX_ARGS + 1];
    char *envp[MAX_ARGS + 1];
    char arg_bufs[MAX_ARGS][256];
    char env_bufs[MAX_ARGS][256];
    int argc = 0, envc = 0;

    if (argv_ptr) {
        uint64_t *argv_user = (uint64_t *)argv_ptr;
        while (argc < MAX_ARGS) {
            uint64_t arg_addr;
            if (copy_from_user(&arg_addr, argv_user + argc, sizeof(uint64_t)) < 0) break;
            if (!arg_addr) break;
            strncpy_from_user(arg_bufs[argc], (const char*)arg_addr, 255);
            argv[argc] = arg_bufs[argc];
            argc++;
        }
    }
    argv[argc] = NULL;

    if (envp_ptr) {
        uint64_t *envp_user = (uint64_t *)envp_ptr;
        while (envc < MAX_ARGS) {
            uint64_t env_addr;
            if (copy_from_user(&env_addr, envp_user + envc, sizeof(uint64_t)) < 0) break;
            if (!env_addr) break;
            strncpy_from_user(env_bufs[envc], (const char*)env_addr, 255);
            envp[envc] = env_bufs[envc];
            envc++;
        }
    }
    envp[envc] = NULL;

    return (uint64_t)proc_exec(PROC, path, argv, envp);
}

/* ============================================================
 * exit (60)
 * ============================================================ */
static uint64_t sys_exit(uint64_t code, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a2); UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    proc_exit((int)code);
    return 0;  /* unreachable */
}

/* ============================================================
 * wait4 (61)
 * ============================================================ */
static uint64_t sys_wait4(uint64_t pid, uint64_t status_ptr, uint64_t options,
                           uint64_t rusage, uint64_t a5, uint64_t a6)
{
    UNUSED(rusage); UNUSED(a5); UNUSED(a6);
    int status;
    pid_t r = proc_wait((pid_t)pid, &status, (int)options);
    if (r > 0 && status_ptr) {
        copy_to_user((void*)status_ptr, &status, sizeof(int));
    }
    return (uint64_t)r;
}

/* ============================================================
 * kill (62)
 * ============================================================ */
static uint64_t sys_kill(uint64_t pid, uint64_t signo, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    if ((int)pid > 0)  return (uint64_t)signal_send_pid((pid_t)pid, (int)signo);
    if ((int)pid == 0) return (uint64_t)signal_send_group(PROC->pgid, (int)signo);
    if ((int)pid == -1) {
        /* 全プロセスに送信 */
        return (uint64_t)signal_send_group(0, (int)signo);
    }
    return (uint64_t)signal_send_group(-(pid_t)pid, (int)signo);
}

/* ============================================================
 * uname (63)
 * ============================================================ */
static uint64_t sys_uname(uint64_t uname_ptr, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a2); UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    struct utsname uts;
    memset(&uts, 0, sizeof(uts));
    strcpy(uts.sysname,  "Linux");
    strcpy(uts.nodename, "unixkernel");
    strcpy(uts.release,  "5.0.0-unixkernel");
    strcpy(uts.version,  "#1 SMP");
    strcpy(uts.machine,  "x86_64");
    strcpy(uts.domainname, "(none)");
    return (uint64_t)copy_to_user((void*)uname_ptr, &uts, sizeof(uts));
}

/* ============================================================
 * fcntl (72)
 * ============================================================ */
static uint64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a4); UNUSED(a5); UNUSED(a6);
    file_t *f = get_file((int)fd);
    if (!f) return (uint64_t)-EBADF;

    uint64_t r = 0;
    switch ((int)cmd) {
    case F_GETFD:
        r = FDT->flags[fd] & FD_FLAG_CLOEXEC ? 1 : 0;
        break;
    case F_SETFD:
        if (arg & 1) FDT->flags[fd] |=  FD_FLAG_CLOEXEC;
        else         FDT->flags[fd] &= ~FD_FLAG_CLOEXEC;
        break;
    case F_GETFL: r = (uint64_t)f->flags; break;
    case F_SETFL: f->flags = (int)arg; break;
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        int new_fd = fd_alloc(FDT, f);
        if (new_fd >= 0 && cmd == F_DUPFD_CLOEXEC)
            FDT->flags[new_fd] |= FD_FLAG_CLOEXEC;
        r = (uint64_t)new_fd;
        break;
    }
    default: r = (uint64_t)-EINVAL;
    }

    file_put(f);
    return r;
}

/* ============================================================
 * getdents64 (217)
 * ============================================================ */
static uint64_t sys_getdents64(uint64_t fd, uint64_t dirent_ptr, uint64_t count,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a4); UNUSED(a5); UNUSED(a6);
    file_t *f = get_file((int)fd);
    if (!f) return (uint64_t)-EBADF;
    if (!f->vnode || f->vnode->type != VN_DIR) {
        file_put(f);
        return (uint64_t)-ENOTDIR;
    }

    struct dirent kdirent;
    size_t written = 0;
    uint8_t *buf = (uint8_t *)dirent_ptr;

    while (written + sizeof(struct dirent) <= count) {
        int r = f->vnode->ops->readdir(f->vnode, f->offset, &kdirent);
        if (r < 0) break;

        if (copy_to_user(buf + written, &kdirent, sizeof(struct dirent)) < 0) break;
        written += sizeof(struct dirent);
        f->offset++;
    }

    file_put(f);
    return (uint64_t)written;
}

/* ============================================================
 * getcwd (79)
 * ============================================================ */
static uint64_t sys_getcwd(uint64_t buf_ptr, uint64_t size,
                            uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    size_t len = strlen(PROC->cwd_path) + 1;
    if (len > size) return (uint64_t)-ERANGE;
    if (copy_to_user((void*)buf_ptr, PROC->cwd_path, len) < 0)
        return (uint64_t)-EFAULT;
    return buf_ptr;
}

/* ============================================================
 * chdir (80)
 * ============================================================ */
static uint64_t sys_chdir(uint64_t path_ptr, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a2); UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    char path[PATH_MAX];
    ssize_t r = strncpy_from_user(path, (const char*)path_ptr, PATH_MAX);
    if (r < 0) return (uint64_t)r;

    vnode_t *vn = NULL;
    int err = vfs_lookup(g_root_vnode, path, &vn);
    if (err < 0) return (uint64_t)err;
    if (vn->type != VN_DIR) { vnode_put(vn); return (uint64_t)-ENOTDIR; }

    if (PROC->cwd) vnode_put(PROC->cwd);
    PROC->cwd = vn;
    strncpy(PROC->cwd_path, path, PATH_MAX - 1);
    return 0;
}

/* ============================================================
 * mkdir (83)
 * ============================================================ */
static uint64_t sys_mkdir(uint64_t path_ptr, uint64_t mode,
                           uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    char path[PATH_MAX];
    strncpy_from_user(path, (const char*)path_ptr, PATH_MAX);

    vnode_t *parent;
    char name[NAME_MAX + 1];
    int r = vfs_lookup_parent(g_root_vnode, path, &parent, name);
    if (r < 0) return (uint64_t)r;
    if (!parent->ops || !parent->ops->mkdir) {
        vnode_put(parent);
        return (uint64_t)-EACCES;
    }
    r = parent->ops->mkdir(parent, name, (mode_t)mode & ~PROC->umask);
    vnode_put(parent);
    return (uint64_t)r;
}

/* ============================================================
 * unlink (87)
 * ============================================================ */
static uint64_t sys_unlink(uint64_t path_ptr, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a2); UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    char path[PATH_MAX];
    strncpy_from_user(path, (const char*)path_ptr, PATH_MAX);

    vnode_t *parent;
    char name[NAME_MAX + 1];
    int r = vfs_lookup_parent(g_root_vnode, path, &parent, name);
    if (r < 0) return (uint64_t)r;
    if (!parent->ops || !parent->ops->unlink) {
        vnode_put(parent);
        return (uint64_t)-EACCES;
    }
    r = parent->ops->unlink(parent, name);
    vnode_put(parent);
    return (uint64_t)r;
}

/* ============================================================
 * getuid/geteuid/getgid/getegid
 * ============================================================ */
static uint64_t sys_getuid(uint64_t a1,uint64_t a2,uint64_t a3,uint64_t a4,uint64_t a5,uint64_t a6)
{ UNUSED(a1);UNUSED(a2);UNUSED(a3);UNUSED(a4);UNUSED(a5);UNUSED(a6); return PROC->uid; }
static uint64_t sys_getgid(uint64_t a1,uint64_t a2,uint64_t a3,uint64_t a4,uint64_t a5,uint64_t a6)
{ UNUSED(a1);UNUSED(a2);UNUSED(a3);UNUSED(a4);UNUSED(a5);UNUSED(a6); return PROC->gid; }
static uint64_t sys_geteuid(uint64_t a1,uint64_t a2,uint64_t a3,uint64_t a4,uint64_t a5,uint64_t a6)
{ UNUSED(a1);UNUSED(a2);UNUSED(a3);UNUSED(a4);UNUSED(a5);UNUSED(a6); return PROC->euid; }
static uint64_t sys_getegid(uint64_t a1,uint64_t a2,uint64_t a3,uint64_t a4,uint64_t a5,uint64_t a6)
{ UNUSED(a1);UNUSED(a2);UNUSED(a3);UNUSED(a4);UNUSED(a5);UNUSED(a6); return PROC->egid; }
static uint64_t sys_getppid(uint64_t a1,uint64_t a2,uint64_t a3,uint64_t a4,uint64_t a5,uint64_t a6)
{ UNUSED(a1);UNUSED(a2);UNUSED(a3);UNUSED(a4);UNUSED(a5);UNUSED(a6); return PROC->ppid; }
static uint64_t sys_getpgrp(uint64_t a1,uint64_t a2,uint64_t a3,uint64_t a4,uint64_t a5,uint64_t a6)
{ UNUSED(a1);UNUSED(a2);UNUSED(a3);UNUSED(a4);UNUSED(a5);UNUSED(a6); return PROC->pgid; }
static uint64_t sys_gettid(uint64_t a1,uint64_t a2,uint64_t a3,uint64_t a4,uint64_t a5,uint64_t a6)
{ UNUSED(a1);UNUSED(a2);UNUSED(a3);UNUSED(a4);UNUSED(a5);UNUSED(a6); return PROC->pid; }

/* ============================================================
 * arch_prctl (158)
 * ============================================================ */
static uint64_t sys_arch_prctl(uint64_t code, uint64_t addr,
                                uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    extern void set_fs_base(uint64_t base);
    extern void set_gs_base(uint64_t base);
    switch ((int)code) {
    case ARCH_SET_FS: set_fs_base(addr); return 0;
    case ARCH_SET_GS: set_gs_base(addr); return 0;
    case ARCH_GET_FS: return copy_to_user((void*)addr, &addr, 8) < 0 ? (uint64_t)-EFAULT : 0;
    default: return (uint64_t)-EINVAL;
    }
}

/* ============================================================
 * exit_group (231)
 * ============================================================ */
static uint64_t sys_exit_group(uint64_t code, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a2); UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    proc_exit((int)code);
    return 0;
}

/* ============================================================
 * clock_gettime (228)
 * ============================================================ */
static uint64_t sys_clock_gettime(uint64_t clkid, uint64_t tp_ptr,
                                   uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    UNUSED(clkid);
    struct timespec ts;
    uint64_t ms = get_time_ms();
    ts.tv_sec  = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000);
    return (uint64_t)copy_to_user((void*)tp_ptr, &ts, sizeof(ts));
}

/* ============================================================
 * gettimeofday (96)
 * ============================================================ */
static uint64_t sys_gettimeofday(uint64_t tv_ptr, uint64_t tz_ptr,
                                  uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(tz_ptr); UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    struct timeval tv;
    uint64_t ms  = get_time_ms();
    tv.tv_sec    = (time_t)(ms / 1000);
    tv.tv_usec   = (suseconds_t)((ms % 1000) * 1000);
    return (uint64_t)copy_to_user((void*)tv_ptr, &tv, sizeof(tv));
}

/* ============================================================
 * mprotect (10)
 * ============================================================ */
static uint64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a4); UNUSED(a5); UNUSED(a6);
    return (uint64_t)vmm_protect(PROC->vm_space, (uintptr_t)addr,
                                  (size_t)len, (uint32_t)prot);
}

/* ============================================================
 * set_tid_address (218)
 * ============================================================ */
static uint64_t sys_set_tid_address(uint64_t tidptr, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(tidptr); UNUSED(a2); UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    return (uint64_t)PROC->pid;
}

/* ============================================================
 * openat (257)
 * ============================================================ */
static uint64_t sys_openat(uint64_t dirfd, uint64_t path_ptr, uint64_t flags,
                            uint64_t mode, uint64_t a5, uint64_t a6)
{
    UNUSED(a5); UNUSED(a6);
    /* AT_FDCWD = -100 */
    if ((int)dirfd != -100) {
        /* 相対パスのopenat - 今回はAT_FDCWDのみ完全対応 */
    }
    return sys_open(path_ptr, flags, mode, 0, 0, 0);
}

/* ============================================================
 * getrandom (318)
 * ============================================================ */
static uint64_t sys_getrandom(uint64_t buf_ptr, uint64_t buflen, uint64_t flags,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(flags); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    uint8_t *buf = kmalloc(buflen);
    if (!buf) return (uint64_t)-ENOMEM;

    static uint64_t rng = 0x123456789ABCDEFULL;
    for (size_t i = 0; i < buflen; i++) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        buf[i] = (uint8_t)rng;
    }

    ssize_t r = copy_to_user((void*)buf_ptr, buf, buflen);
    kfree(buf);
    return r < 0 ? (uint64_t)r : (uint64_t)buflen;
}

/* ============================================================
 * access (21)
 * ============================================================ */
static uint64_t sys_access(uint64_t path_ptr, uint64_t mode,
                            uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(mode); UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    char path[PATH_MAX];
    strncpy_from_user(path, (const char*)path_ptr, PATH_MAX);
    vnode_t *vn = NULL;
    int r = vfs_lookup(g_root_vnode, path, &vn);
    if (r == 0) vnode_put(vn);
    return (uint64_t)r;
}

/* ============================================================
 * sysinfo (99)
 * ============================================================ */
static uint64_t sys_sysinfo(uint64_t info_ptr, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    UNUSED(a2); UNUSED(a3); UNUSED(a4); UNUSED(a5); UNUSED(a6);
    struct sysinfo si;
    memset(&si, 0, sizeof(si));
    si.uptime    = (int64_t)(get_time_ms() / 1000);
    si.totalram  = pmm_total_pages() * PAGE_SIZE;
    si.freeram   = pmm_free_pages()  * PAGE_SIZE;
    si.mem_unit  = 1;
    return (uint64_t)copy_to_user((void*)info_ptr, &si, sizeof(si));
}

/* ============================================================
 * syscall_init - 全syscallを登録
 * ============================================================ */
void syscall_init(void)
{
    memset(g_syscall_table, 0, sizeof(g_syscall_table));

    syscall_register(SYS_read,           sys_read);
    syscall_register(SYS_write,          sys_write);
    syscall_register(SYS_open,           sys_open);
    syscall_register(SYS_close,          sys_close);
    syscall_register(SYS_stat,           sys_stat);
    syscall_register(SYS_fstat,          sys_fstat);
    syscall_register(SYS_lstat,          sys_stat);
    syscall_register(SYS_lseek,          sys_lseek);
    syscall_register(SYS_mmap,           sys_mmap);
    syscall_register(SYS_mprotect,       sys_mprotect);
    syscall_register(SYS_munmap,         sys_munmap);
    syscall_register(SYS_brk,            sys_brk);
    syscall_register(SYS_rt_sigaction,   sys_rt_sigaction);
    syscall_register(SYS_rt_sigprocmask, sys_rt_sigprocmask);
    syscall_register(SYS_ioctl,          sys_ioctl);
    syscall_register(SYS_pipe,           sys_pipe);
    syscall_register(SYS_sched_yield,    sys_sched_yield);
    syscall_register(SYS_dup,            sys_dup);
    syscall_register(SYS_dup2,           sys_dup2);
    syscall_register(SYS_nanosleep,      sys_nanosleep);
    syscall_register(SYS_getpid,         sys_getpid);
    syscall_register(SYS_fork,           sys_fork);
    syscall_register(SYS_execve,         sys_execve);
    syscall_register(SYS_exit,           sys_exit);
    syscall_register(SYS_wait4,          sys_wait4);
    syscall_register(SYS_kill,           sys_kill);
    syscall_register(SYS_uname,          sys_uname);
    syscall_register(SYS_fcntl,          sys_fcntl);
    syscall_register(SYS_getdents64,     sys_getdents64);
    syscall_register(SYS_getcwd,         sys_getcwd);
    syscall_register(SYS_chdir,          sys_chdir);
    syscall_register(SYS_mkdir,          sys_mkdir);
    syscall_register(SYS_unlink,         sys_unlink);
    syscall_register(SYS_getuid,         sys_getuid);
    syscall_register(SYS_getgid,         sys_getgid);
    syscall_register(SYS_geteuid,        sys_geteuid);
    syscall_register(SYS_getegid,        sys_getegid);
    syscall_register(SYS_getppid,        sys_getppid);
    syscall_register(SYS_getpgrp,        sys_getpgrp);
    syscall_register(SYS_gettid,         sys_gettid);
    syscall_register(SYS_arch_prctl,     sys_arch_prctl);
    syscall_register(SYS_exit_group,     sys_exit_group);
    syscall_register(SYS_clock_gettime,  sys_clock_gettime);
    syscall_register(SYS_gettimeofday,   sys_gettimeofday);
    syscall_register(SYS_set_tid_address,sys_set_tid_address);
    syscall_register(SYS_openat,         sys_openat);
    syscall_register(SYS_getrandom,      sys_getrandom);
    syscall_register(SYS_access,         sys_access);
    syscall_register(SYS_sysinfo,        sys_sysinfo);
    syscall_register(SYS_getdents,       sys_getdents64);  /* compat */
    syscall_register(SYS_newfstatat,     sys_fstat);       /* compat stub */

    printk(KERN_INFO "Syscall: registered %d handlers\n", NR_SYSCALLS);
}
