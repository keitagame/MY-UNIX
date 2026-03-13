#pragma once
#ifndef _FS_H
#define _FS_H

/* ============================================================
 * fs.h - 仮想ファイルシステム (VFS) ヘッダ
 * ============================================================ */

#include "types.h"
#include "kernel.h"

/* ============================================================
 * 前方宣言
 * ============================================================ */
struct vnode;
struct vfs_ops;
struct file_ops;
struct filesystem;
struct mount;
struct dirent;

/* ============================================================
 * vnode (仮想ノード)
 * ============================================================ */

typedef enum {
    VN_REG   = 1,  /* 通常ファイル */
    VN_DIR   = 2,  /* ディレクトリ */
    VN_CHR   = 3,  /* キャラクタデバイス */
    VN_BLK   = 4,  /* ブロックデバイス */
    VN_FIFO  = 5,  /* パイプ */
    VN_LNK   = 6,  /* シンボリックリンク */
    VN_SOCK  = 7,  /* ソケット */
} vnode_type_t;

struct vnode_ops {
    /* ファイル操作 */
    int     (*open)   (struct vnode *vn, int flags, mode_t mode);
    int     (*close)  (struct vnode *vn);
    ssize_t (*read)   (struct vnode *vn, void *buf, size_t len, off_t off);
    ssize_t (*write)  (struct vnode *vn, const void *buf, size_t len, off_t off);
    int     (*truncate)(struct vnode *vn, off_t size);
    int     (*mmap)   (struct vnode *vn, uintptr_t addr, size_t len, off_t off, int prot);
    
    /* ディレクトリ操作 */
    int     (*lookup) (struct vnode *dir, const char *name, struct vnode **out);
    int     (*create) (struct vnode *dir, const char *name, mode_t mode, struct vnode **out);
    int     (*mkdir)  (struct vnode *dir, const char *name, mode_t mode);
    int     (*unlink) (struct vnode *dir, const char *name);
    int     (*rmdir)  (struct vnode *dir, const char *name);
    int     (*rename) (struct vnode *old_dir, const char *old_name,
                       struct vnode *new_dir, const char *new_name);
    int     (*link)   (struct vnode *dir, const char *name, struct vnode *target);
    int     (*symlink)(struct vnode *dir, const char *name, const char *target);
    int     (*readdir)(struct vnode *dir, off_t off, struct dirent *ent);
    int     (*readlink)(struct vnode *vn, char *buf, size_t len);
    
    /* メタデータ */
    int     (*stat)   (struct vnode *vn, struct stat *st);
    int     (*chmod)  (struct vnode *vn, mode_t mode);
    int     (*chown)  (struct vnode *vn, uid_t uid, gid_t gid);
    int     (*utimes) (struct vnode *vn, const struct timespec times[2]);
    
    /* 同期 */
    int     (*fsync)  (struct vnode *vn);
    
    /* ioctl */
    int     (*ioctl)  (struct vnode *vn, unsigned long req, void *arg);
    
    /* poll */
    int     (*poll)   (struct vnode *vn, int events);
    
    /* 解放 */
    void    (*release)(struct vnode *vn);
};

typedef struct vnode {
    vnode_type_t  type;
    mode_t        mode;
    uid_t         uid;
    gid_t         gid;
    ino_t         ino;
    dev_t         dev;
    off_t         size;
    nlink_t       nlink;
    
    struct timespec atime, mtime, ctime;
    
    const struct vnode_ops *ops;
    struct filesystem      *fs;
    struct mount           *mount;   /* このvnodeへのマウント (あれば) */
    
    void          *private;  /* FSプライベートデータ */
    
    atomic_t       refcount;
    spinlock_t     lock;
    
    /* ページキャッシュ */
    struct list_head page_cache;
    
    /* ディレクトリエントリキャッシュ */
    struct list_head dentry_cache;
    
    /* マウントポイント逆参照 */
    struct vnode  *covered;   /* このvnodeが覆っているvnode */
    
} vnode_t;

/* ============================================================
 * ディレクトリエントリ
 * ============================================================ */
struct dirent {
    ino_t    d_ino;
    off_t    d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[NAME_MAX + 1];
};

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK    10
#define DT_SOCK   12
#define DT_WHT    14

/* ============================================================
 * ファイル構造体
 * ============================================================ */
struct file_ops {
    ssize_t (*read)   (struct file *f, void *buf, size_t len);
    ssize_t (*write)  (struct file *f, const void *buf, size_t len);
    off_t   (*lseek)  (struct file *f, off_t off, int whence);
    int     (*ioctl)  (struct file *f, unsigned long req, void *arg);
    int     (*poll)   (struct file *f, int events);
    int     (*readdir)(struct file *f, struct dirent *ent);
    int     (*mmap)   (struct file *f, uintptr_t addr, size_t len, off_t off, int prot);
    int     (*close)  (struct file *f);
    int     (*fsync)  (struct file *f);
};

typedef struct file {
    vnode_t            *vnode;
    const struct file_ops *ops;
    off_t               offset;
    int                 flags;   /* O_RDONLY等 */
    mode_t              mode;
    atomic_t            refcount;
    spinlock_t          lock;
    void               *private;  /* パイプ等のプライベートデータ */
} file_t;

/* ============================================================
 * ファイルシステム記述子
 * ============================================================ */
typedef struct filesystem {
    const char *name;           /* "ext2", "initrd", "tmpfs", etc. */
    
    /* マウント */
    int (*mount)(struct mount *mp, const char *device, uint32_t flags);
    int (*umount)(struct mount *mp);
    
    /* ルートvnode取得 */
    int (*get_root)(struct mount *mp, vnode_t **out);
    
    /* 同期 */
    int (*sync)(struct mount *mp);
    
    struct list_head list;
} filesystem_t;

/* ============================================================
 * マウントポイント
 * ============================================================ */
typedef struct mount {
    filesystem_t    *fs;
    vnode_t         *root;      /* FSのルートvnode */
    vnode_t         *mount_pt;  /* マウント先vnode */
    char             path[PATH_MAX];
    uint32_t         flags;
    void            *private;   /* FSプライベートデータ */
    struct list_head list;
} mount_t;

/* ============================================================
 * VFS API
 * ============================================================ */

/* 初期化 */
void vfs_init(void);

/* ファイルシステム登録 */
int  vfs_register_fs(filesystem_t *fs);
int  vfs_unregister_fs(filesystem_t *fs);

/* マウント操作 */
int  vfs_mount(const char *device, const char *path,
               const char *fstype, uint32_t flags);
int  vfs_umount(const char *path);
mount_t *vfs_find_mount(const char *path);

/* パス解決 */
int vfs_lookup(vnode_t *start, const char *path, vnode_t **out);
int vfs_lookup_parent(vnode_t *start, const char *path,
                       vnode_t **parent_out, char *name_out);

/* ファイル操作 */
int    vfs_open(const char *path, int flags, mode_t mode, file_t **out);
int    vfs_openat(vnode_t *dirfd, const char *path, int flags, mode_t mode, file_t **out);
file_t *file_alloc(vnode_t *vn, int flags);
void   file_get(file_t *f);
void   file_put(file_t *f);

/* vnode操作 */
vnode_t *vnode_alloc(void);
void     vnode_get(vnode_t *vn);
void     vnode_put(vnode_t *vn);

/* ============================================================
 * ファイルシステム群
 * ============================================================ */

/* initrd (CPIO形式) */
void initrd_init(uintptr_t start, size_t size);
filesystem_t *initrd_get_fs(void);

/* tmpfs */
filesystem_t *tmpfs_get_fs(void);

/* devfs */
filesystem_t *devfs_get_fs(void);
void devfs_init(void);

/* procfs */
filesystem_t *procfs_get_fs(void);

/* ============================================================
 * デバイス管理
 * ============================================================ */
typedef struct device {
    char     name[64];
    dev_t    devno;
    int      type;   /* VN_CHR or VN_BLK */
    const struct vnode_ops *ops;
    void    *private;
    struct list_head list;
} device_t;

int    dev_register(device_t *dev);
int    dev_unregister(dev_t devno);
device_t *dev_find_by_name(const char *name);
device_t *dev_find_by_devno(dev_t devno);
vnode_t  *dev_make_vnode(dev_t devno);

/* デバイス番号 */
#define MAKEDEV(maj, min)  (((uint64_t)(maj) << 20) | ((uint64_t)(min)))
#define MAJOR(dev)         ((uint32_t)((dev) >> 20))
#define MINOR(dev)         ((uint32_t)((dev) & 0xFFFFF))

/* 既知メジャー番号 */
#define DEV_MEM_MAJOR   1   /* /dev/mem, /dev/null, /dev/zero */
#define DEV_TTY_MAJOR   4   /* /dev/tty, /dev/console */
#define DEV_TTY_MAJOR2  5
#define DEV_LOOP_MAJOR  7
#define DEV_DISK_MAJOR  8   /* /dev/sda, /dev/sdb */
#define DEV_RANDOM_MAJOR 1  /* /dev/random minor=8, /dev/urandom minor=9 */
#define DEV_FB_MAJOR    29
#define DEV_INPUT_MAJOR 13

/* ============================================================
 * パイプ
 * ============================================================ */
int pipe_create(file_t **read_end, file_t **write_end);

/* ============================================================
 * ターミナル/TTY
 * ============================================================ */
#include <stdint.h>

/* termios */
typedef uint32_t tcflag_t;
typedef uint8_t  cc_t;
typedef uint32_t speed_t;

#define NCCS 32

struct termios {
    tcflag_t c_iflag;   /* 入力フラグ */
    tcflag_t c_oflag;   /* 出力フラグ */
    tcflag_t c_cflag;   /* 制御フラグ */
    tcflag_t c_lflag;   /* ローカルフラグ */
    cc_t     c_cc[NCCS];
};

/* c_iflag */
#define IGNBRK  0000001
#define BRKINT  0000002
#define IGNPAR  0000004
#define INPCK   0000020
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IXON    0002000
#define IXOFF   0010000

/* c_oflag */
#define OPOST   0000001
#define ONLCR   0000004

/* c_lflag */
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define IEXTEN  0100000

/* c_cc インデックス */
#define VEOF    4
#define VEOL    11
#define VERASE  2
#define VINTR   0
#define VKILL   3
#define VMIN    6
#define VQUIT   1
#define VSTART  8
#define VSTOP   9
#define VSUSP   10
#define VTIME   5

/* ioctl コマンド */
#define TCGETS          0x5401
#define TCSETS          0x5402
#define TCSETSW         0x5403
#define TCSETSF         0x5404
#define TIOCGWINSZ      0x5413
#define TIOCSWINSZ      0x5414
#define TIOCGPGRP       0x540F
#define TIOCSPGRP       0x5410
#define TIOCGPTN        0x80045430
#define TIOCSPTLCK      0x40045431

struct winsize {
    uint16_t ws_row, ws_col;
    uint16_t ws_xpixel, ws_ypixel;
};

/* TTY初期化 */
void tty_init(void);
vnode_t *tty_get_console(void);

#endif /* _FS_H */
