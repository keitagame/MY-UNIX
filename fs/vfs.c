/* ============================================================
 * vfs.c - 仮想ファイルシステム + initrd (CPIO形式)
 * ============================================================ */

#include "../include/types.h"
#include "../include/kernel.h"
#include "../include/mm.h"
#include "../include/fs.h"
#include "../include/process.h"

/* ============================================================
 * VFS グローバル状態
 * ============================================================ */
vnode_t   *g_root_vnode = NULL;

static LIST_HEAD(g_fs_list);       /* 登録済みFS */
static LIST_HEAD(g_mount_list);    /* マウントリスト */
static LIST_HEAD(g_dev_list);      /* デバイスリスト */
static spinlock_t g_vfs_lock  = SPINLOCK_INIT;
static spinlock_t g_dev_lock  = SPINLOCK_INIT;

/* ============================================================
 * vnode 管理
 * ============================================================ */
vnode_t *vnode_alloc(void)
{
    vnode_t *vn = kzalloc(sizeof(vnode_t));
    if (!vn) return NULL;
    spinlock_init(&vn->lock);
    atomic_set(&vn->refcount, 1);
    INIT_LIST_HEAD(&vn->page_cache);
    INIT_LIST_HEAD(&vn->dentry_cache);
    return vn;
}

void vnode_get(vnode_t *vn)
{
    if (vn) atomic_inc(&vn->refcount);
}

void vnode_put(vnode_t *vn)
{
    if (!vn) return;
    if (atomic_dec_and_test(&vn->refcount)) {
        if (vn->ops && vn->ops->release)
            vn->ops->release(vn);
        kfree(vn);
    }
}

/* ============================================================
 * file 管理
 * ============================================================ */
file_t *file_alloc(vnode_t *vn, int flags)
{
    file_t *f = kzalloc(sizeof(file_t));
    if (!f) return NULL;
    f->vnode  = vn;
    f->flags  = flags;
    f->offset = 0;
    spinlock_init(&f->lock);
    atomic_set(&f->refcount, 1);
    if (vn) vnode_get(vn);
    return f;
}

void file_get(file_t *f)
{
    if (f) atomic_inc(&f->refcount);
}

void file_put(file_t *f)
{
    if (!f) return;
    if (atomic_dec_and_test(&f->refcount)) {
        if (f->ops && f->ops->close) f->ops->close(f);
        else if (f->vnode && f->vnode->ops && f->vnode->ops->close)
            f->vnode->ops->close(f->vnode);
        if (f->vnode) vnode_put(f->vnode);
        kfree(f);
    }
}

/* ============================================================
 * FS 登録
 * ============================================================ */
int vfs_register_fs(filesystem_t *fs)
{
    spinlock_lock(&g_vfs_lock);
    list_add_tail(&fs->list, &g_fs_list);
    spinlock_unlock(&g_vfs_lock);
    printk(KERN_INFO "VFS: registered filesystem '%s'\n", fs->name);
    return 0;
}

static filesystem_t *find_fs(const char *name)
{
    struct list_head *pos;
    list_for_each(pos, &g_fs_list) {
        filesystem_t *fs = list_entry(pos, filesystem_t, list);
        if (strcmp(fs->name, name) == 0) return fs;
    }
    return NULL;
}

/* ============================================================
 * マウント
 * ============================================================ */
int vfs_mount(const char *device, const char *path,
              const char *fstype, uint32_t flags)
{
    filesystem_t *fs = find_fs(fstype);
    if (!fs) {
        printk(KERN_ERR "VFS: unknown filesystem '%s'\n", fstype);
        return -ENODEV;
    }

    mount_t *mp = kzalloc(sizeof(mount_t));
    if (!mp) return -ENOMEM;

    mp->fs    = fs;
    mp->flags = flags;
    strncpy(mp->path, path, PATH_MAX - 1);
    INIT_LIST_HEAD(&mp->list);

    int r = fs->mount(mp, device, flags);
    if (r < 0) {
        kfree(mp);
        return r;
    }

    r = fs->get_root(mp, &mp->root);
    if (r < 0) {
        if (fs->umount) fs->umount(mp);
        kfree(mp);
        return r;
    }

    /* マウントポイントvnodeを見つける */
    if (strcmp(path, "/") == 0) {
        g_root_vnode = mp->root;
        mp->mount_pt = NULL;
    } else {
        vnode_t *mount_vn = NULL;
        r = vfs_lookup(g_root_vnode, path, &mount_vn);
        if (r < 0) {
            vnode_put(mp->root);
            kfree(mp);
            return r;
        }
        mp->mount_pt       = mount_vn;
        mount_vn->mount    = mp;
        mp->root->covered  = mount_vn;
    }

    spinlock_lock(&g_vfs_lock);
    list_add_tail(&mp->list, &g_mount_list);
    spinlock_unlock(&g_vfs_lock);

    printk(KERN_INFO "VFS: mounted %s on %s (%s)\n",
           device ? device : "none", path, fstype);
    return 0;
}

/* ============================================================
 * パス解決
 * ============================================================ */
int vfs_lookup(vnode_t *start, const char *path, vnode_t **out)
{
    if (!path || !out) return -EINVAL;

    vnode_t *current;
    if (path[0] == '/') {
        current = g_root_vnode;
        path++;
    } else {
        current = start ? start : g_current->cwd;
    }

    if (!current) return -ENOENT;
    vnode_get(current);

    char component[NAME_MAX + 1];

    while (*path) {
        /* パス要素を切り出す */
        const char *sep = strchr(path, '/');
        size_t len;
        if (sep) {
            len  = (size_t)(sep - path);
            if (len == 0) { path++; continue; }  /* // */
        } else {
            len = strlen(path);
        }

        if (len > NAME_MAX) { vnode_put(current); return -ENAMETOOLONG; }
        memcpy(component, path, len);
        component[len] = '\0';

        /* マウントポイントを解決 */
        if (current->mount) {
            vnode_t *mounted_root = current->mount->root;
            vnode_put(current);
            vnode_get(mounted_root);
            current = mounted_root;
        }

        if (strcmp(component, ".") == 0) {
            /* 現在ディレクトリ: 何もしない */
        } else if (strcmp(component, "..") == 0) {
            /* 親ディレクトリ */
            if (current->covered) {
                vnode_t *parent = current->covered;
                vnode_put(current);
                vnode_get(parent);
                current = parent;
            } else if (current->ops && current->ops->lookup) {
                vnode_t *parent;
                int r = current->ops->lookup(current, "..", &parent);
                if (r < 0) { vnode_put(current); return r; }
                vnode_put(current);
                current = parent;
            }
        } else {
            /* 通常のlookup */
            if (!current->ops || !current->ops->lookup) {
                vnode_put(current);
                return -ENOTDIR;
            }
            vnode_t *child;
            int r = current->ops->lookup(current, component, &child);
            if (r < 0) { vnode_put(current); return r; }
            vnode_put(current);
            current = child;
        }

        if (sep) path = sep + 1;
        else     break;
    }

    /* マウントポイント最終解決 */
    if (current->mount) {
        vnode_t *r = current->mount->root;
        vnode_put(current);
        vnode_get(r);
        current = r;
    }

    *out = current;
    return 0;
}

int vfs_lookup_parent(vnode_t *start, const char *path,
                       vnode_t **parent_out, char *name_out)
{
    const char *last = strrchr(path, '/');
    if (!last) {
        /* パスにスラッシュなし → 現在ディレクトリが親 */
        vnode_t *cwd = start ? start : g_current->cwd;
        vnode_get(cwd);
        *parent_out = cwd;
        strncpy(name_out, path, NAME_MAX);
        return 0;
    }

    /* 親ディレクトリのパス */
    size_t parent_len = (size_t)(last - path);
    char parent_path[PATH_MAX];
    if (parent_len == 0) {
        strcpy(parent_path, "/");
    } else {
        memcpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
    }

    strncpy(name_out, last + 1, NAME_MAX);
    return vfs_lookup(start, parent_path, parent_out);
}

/* ============================================================
 * ファイルオープン
 * ============================================================ */
static const struct file_ops g_default_file_ops;  /* 前方宣言 */

int vfs_open(const char *path, int flags, mode_t mode, file_t **out)
{
    vnode_t *vn = NULL;
    int r;

    bool create = (flags & O_CREAT) != 0;
    bool excl   = (flags & O_EXCL)  != 0;

    r = vfs_lookup(g_root_vnode, path, &vn);
    if (r == -ENOENT && create) {
        /* ファイル新規作成 */
        vnode_t *parent;
        char name[NAME_MAX + 1];
        r = vfs_lookup_parent(g_root_vnode, path, &parent, name);
        if (r < 0) return r;

        if (!parent->ops || !parent->ops->create) {
            vnode_put(parent);
            return -EACCES;
        }
        r = parent->ops->create(parent, name, mode, &vn);
        vnode_put(parent);
        if (r < 0) return r;
    } else if (r < 0) {
        return r;
    } else if (excl && create) {
        vnode_put(vn);
        return -EEXIST;
    }

    /* ディレクトリフラグチェック */
    if ((flags & O_DIRECTORY) && vn->type != VN_DIR) {
        vnode_put(vn);
        return -ENOTDIR;
    }

    /* openコールバック */
    if (vn->ops && vn->ops->open) {
        r = vn->ops->open(vn, flags, mode);
        if (r < 0) { vnode_put(vn); return r; }
    }

    /* O_TRUNC */
    if ((flags & O_TRUNC) && vn->type == VN_REG) {
        if (vn->ops && vn->ops->truncate)
            vn->ops->truncate(vn, 0);
    }

    file_t *f = file_alloc(vn, flags);
    vnode_put(vn);  /* file_allocが参照を取得済み */
    if (!f) return -ENOMEM;

    *out = f;
    return 0;
}

/* ============================================================
 * デバイス管理
 * ============================================================ */
int dev_register(device_t *dev)
{
    spinlock_lock(&g_dev_lock);
    INIT_LIST_HEAD(&dev->list);
    list_add_tail(&dev->list, &g_dev_list);
    spinlock_unlock(&g_dev_lock);
    printk(KERN_INFO "Dev: registered '%s' (%d:%d)\n",
           dev->name, MAJOR(dev->devno), MINOR(dev->devno));
    return 0;
}

device_t *dev_find_by_name(const char *name)
{
    spinlock_lock(&g_dev_lock);
    struct list_head *pos;
    list_for_each(pos, &g_dev_list) {
        device_t *d = list_entry(pos, device_t, list);
        if (strcmp(d->name, name) == 0) {
            spinlock_unlock(&g_dev_lock);
            return d;
        }
    }
    spinlock_unlock(&g_dev_lock);
    return NULL;
}

device_t *dev_find_by_devno(dev_t devno)
{
    spinlock_lock(&g_dev_lock);
    struct list_head *pos;
    list_for_each(pos, &g_dev_list) {
        device_t *d = list_entry(pos, device_t, list);
        if (d->devno == devno) {
            spinlock_unlock(&g_dev_lock);
            return d;
        }
    }
    spinlock_unlock(&g_dev_lock);
    return NULL;
}

/* ============================================================
 * VFS 初期化
 * ============================================================ */
void vfs_init(void)
{
    INIT_LIST_HEAD(&g_fs_list);
    INIT_LIST_HEAD(&g_mount_list);
    INIT_LIST_HEAD(&g_dev_list);
    printk(KERN_INFO "VFS: initialized\n");
}

/* ============================================================
 * CPIO (newc形式) initrd パーサー
 * ============================================================ */

/* CPIO newc ヘッダ (110 バイト) */
#define CPIO_MAGIC "070701"
#define CPIO_MAGIC_CRC "070702"

typedef struct {
    char c_magic[6];
    char c_ino[8];
    char c_mode[8];
    char c_uid[8];
    char c_gid[8];
    char c_nlink[8];
    char c_mtime[8];
    char c_filesize[8];
    char c_devmajor[8];
    char c_devminor[8];
    char c_rdevmajor[8];
    char c_rdevminor[8];
    char c_namesize[8];
    char c_check[8];
} PACKED cpio_newc_header_t;

/* hex文字列→uint32 */
static uint32_t cpio_parse_hex(const char *s, int n)
{
    uint32_t val = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else d = 0;
        val = (val << 4) | d;
    }
    return val;
}

/* ============================================================
 * initrd FS (メモリ上のCPIOを直接参照するread-only FS)
 * ============================================================ */

typedef struct initrd_entry {
    char     name[PATH_MAX];
    uint32_t mode;
    uint32_t uid, gid;
    uint32_t size;
    uint32_t mtime;
    uint32_t nlink;
    uint32_t ino;
    uint8_t *data;           /* initrdメモリ内のデータポインタ */
    bool     is_dir;
    struct list_head list;
} initrd_entry_t;

typedef struct {
    uint8_t         *base;   /* initrdメモリベース */
    size_t           size;
    struct list_head entries;
    vnode_t         *root_vn;
} initrd_fs_t;

static initrd_fs_t g_initrd;

/* ============================================================
 * initrd vnode 操作
 * ============================================================ */
static ssize_t initrd_vn_read(vnode_t *vn, void *buf, size_t len, off_t off)
{
    initrd_entry_t *ent = (initrd_entry_t *)vn->private;
    if (!ent || ent->is_dir) return -EISDIR;
    if (off < 0) return -EINVAL;
    if ((uint64_t)off >= ent->size) return 0;

    size_t avail = ent->size - (size_t)off;
    size_t n     = MIN(len, avail);
    memcpy(buf, ent->data + off, n);
    return (ssize_t)n;
}

static int initrd_vn_lookup(vnode_t *dir, const char *name, vnode_t **out)
{
    initrd_entry_t *dir_ent = (initrd_entry_t *)dir->private;
    const char *dir_prefix  = dir_ent ? dir_ent->name : "";

    struct list_head *pos;
    list_for_each(pos, &g_initrd.entries) {
        initrd_entry_t *ent = list_entry(pos, initrd_entry_t, list);

        /* ディレクトリ直下の子かチェック */
        const char *ep = ent->name;
        size_t plen = strlen(dir_prefix);

        /* ルートの場合 */
        if (plen == 0 || (plen == 1 && dir_prefix[0] == '.')) {
            /* トップレベルエントリ: '/'を含まない名前 */
            if (strchr(ep, '/') == NULL && strcmp(ep, name) == 0) {
                goto found;
            }
            /* "dir/..."という形式 */
            const char *slash = strchr(ep, '/');
            if (slash) {
                size_t component_len = (size_t)(slash - ep);
                if (strncmp(ep, name, component_len) == 0 && name[component_len] == '\0') {
                    goto found;
                }
            }
        } else {
            /* サブディレクトリ */
            if (strncmp(ep, dir_prefix, plen) == 0 && ep[plen] == '/') {
                const char *child = ep + plen + 1;
                if (strcmp(child, name) == 0) goto found;
                /* 子ディレクトリの名前のみ一致 */
                const char *sl2 = strchr(child, '/');
                if (sl2) {
                    size_t clen = (size_t)(sl2 - child);
                    if (strncmp(child, name, clen) == 0 && name[clen] == '\0') {
                        goto found;
                    }
                }
            }
        }
        continue;

found:;
        vnode_t *vn = vnode_alloc();
        if (!vn) return -ENOMEM;
        vn->type    = ent->is_dir ? VN_DIR : VN_REG;
        vn->mode    = ent->mode;
        vn->uid     = ent->uid;
        vn->gid     = ent->gid;
        vn->size    = ent->size;
        vn->nlink   = ent->nlink;
        vn->ino     = ent->ino;
        vn->ops     = dir->ops;
        vn->fs      = dir->fs;
        vn->private = ent;
        *out = vn;
        return 0;
    }
    return -ENOENT;
}

static int initrd_vn_stat(vnode_t *vn, struct stat *st)
{
    initrd_entry_t *ent = (initrd_entry_t *)vn->private;
    memset(st, 0, sizeof(*st));
    st->st_mode   = ent ? ent->mode : vn->mode;
    st->st_uid    = ent ? ent->uid  : vn->uid;
    st->st_gid    = ent ? ent->gid  : vn->gid;
    st->st_size   = ent ? ent->size : vn->size;
    st->st_ino    = ent ? ent->ino  : vn->ino;
    st->st_nlink  = ent ? ent->nlink : 1;
    st->st_blksize = PAGE_SIZE;
    st->st_blocks  = (st->st_size + 511) / 512;
    if (ent) st->st_mtim.tv_sec = ent->mtime;
    return 0;
}

static int initrd_vn_readdir(vnode_t *dir, off_t off, struct dirent *ent_out)
{
    initrd_entry_t *dir_ent = (initrd_entry_t *)dir->private;
    const char *dir_prefix  = dir_ent ? dir_ent->name : "";
    size_t plen             = strlen(dir_prefix);

    off_t idx = 0;
    struct list_head *pos;
    list_for_each(pos, &g_initrd.entries) {
        initrd_entry_t *ent = list_entry(pos, initrd_entry_t, list);
        const char *ep = ent->name;

        bool match = false;
        const char *child_name = NULL;

        if (plen == 0 || (plen == 1 && dir_prefix[0] == '.')) {
            const char *slash = strchr(ep, '/');
            if (!slash) { match = true; child_name = ep; }
            else {
                /* 最初のスラッシュまでがトップレベル */
                /* 重複を避けるには別途処理が必要だが今回は簡略 */
            }
        } else {
            if (strncmp(ep, dir_prefix, plen) == 0 && ep[plen] == '/') {
                child_name = ep + plen + 1;
                if (strchr(child_name, '/') == NULL) match = true;
            }
        }

        if (match && child_name) {
            if (idx == off) {
                ent_out->d_ino    = ent->ino;
                ent_out->d_off    = idx + 1;
                ent_out->d_type   = ent->is_dir ? DT_DIR : DT_REG;
                strncpy(ent_out->d_name, child_name, NAME_MAX);
                ent_out->d_reclen = sizeof(struct dirent);
                return 0;
            }
            idx++;
        }
    }
    return -ENOENT;  /* 終端 */
}

static const struct vnode_ops g_initrd_vnode_ops = {
    .read    = initrd_vn_read,
    .lookup  = initrd_vn_lookup,
    .stat    = initrd_vn_stat,
    .readdir = initrd_vn_readdir,
};

/* ============================================================
 * initrd FS マウント操作
 * ============================================================ */
static int initrd_mount(struct mount *mp, const char *device, uint32_t flags)
{
    UNUSED(device); UNUSED(flags);
    mp->private = &g_initrd;
    return 0;
}

static int initrd_get_root(struct mount *mp, vnode_t **out)
{
    UNUSED(mp);
    vnode_get(g_initrd.root_vn);
    *out = g_initrd.root_vn;
    return 0;
}

static filesystem_t g_initrd_fs = {
    .name      = "initrd",
    .mount     = initrd_mount,
    .get_root  = initrd_get_root,
};

/* ============================================================
 * CPIO パース & initrd 初期化
 * ============================================================ */
void initrd_init(uintptr_t start, size_t size)
{
    printk(KERN_INFO "initrd: parsing at 0x%llx size=%zu KB\n",
           (unsigned long long)start, size >> 10);

    g_initrd.base = (uint8_t *)start;
    g_initrd.size = size;
    INIT_LIST_HEAD(&g_initrd.entries);

    uint8_t *ptr = g_initrd.base;
    uint8_t *end = ptr + size;
    uint32_t ino_counter = 1;

    while (ptr + sizeof(cpio_newc_header_t) <= end) {
        cpio_newc_header_t *hdr = (cpio_newc_header_t *)ptr;

        /* マジック確認 */
        if (memcmp(hdr->c_magic, CPIO_MAGIC, 6) != 0 &&
            memcmp(hdr->c_magic, CPIO_MAGIC_CRC, 6) != 0) {
            printk(KERN_WARNING "initrd: bad CPIO magic at offset %zu\n",
                   (size_t)(ptr - g_initrd.base));
            break;
        }

        uint32_t name_size  = cpio_parse_hex(hdr->c_namesize, 8);
        uint32_t file_size  = cpio_parse_hex(hdr->c_filesize, 8);
        uint32_t mode       = cpio_parse_hex(hdr->c_mode, 8);
        uint32_t uid        = cpio_parse_hex(hdr->c_uid, 8);
        uint32_t gid        = cpio_parse_hex(hdr->c_gid, 8);
        uint32_t mtime      = cpio_parse_hex(hdr->c_mtime, 8);
        uint32_t nlink      = cpio_parse_hex(hdr->c_nlink, 8);

        ptr += sizeof(cpio_newc_header_t);

        char *name = (char *)ptr;
        ptr = g_initrd.base + ALIGN_UP((uintptr_t)(ptr + name_size) - (uintptr_t)g_initrd.base, 4);

        /* 終端エントリ */
        if (strcmp(name, "TRAILER!!!") == 0) break;

        /* データポインタ */
        uint8_t *data = ptr;
        ptr = g_initrd.base + ALIGN_UP((uintptr_t)(ptr + file_size) - (uintptr_t)g_initrd.base, 4);

        /* エントリ作成 */
        initrd_entry_t *entry = kzalloc(sizeof(initrd_entry_t));
        if (!entry) continue;

        /* 先頭の "./" を除去 */
        const char *clean_name = name;
        if (clean_name[0] == '.' && clean_name[1] == '/') clean_name += 2;
        if (clean_name[0] == '.' && clean_name[1] == '\0') clean_name = "";

        strncpy(entry->name, clean_name, PATH_MAX - 1);
        entry->mode   = mode;
        entry->uid    = uid;
        entry->gid    = gid;
        entry->size   = file_size;
        entry->mtime  = mtime;
        entry->nlink  = nlink;
        entry->ino    = ino_counter++;
        entry->data   = data;
        entry->is_dir = S_ISDIR(mode);
        INIT_LIST_HEAD(&entry->list);

        list_add_tail(&entry->list, &g_initrd.entries);
    }

    /* ルートvnode 作成 */
    g_initrd.root_vn = vnode_alloc();
    g_initrd.root_vn->type    = VN_DIR;
    g_initrd.root_vn->mode    = S_IFDIR | 0755;
    g_initrd.root_vn->nlink   = 2;
    g_initrd.root_vn->ino     = 0;
    g_initrd.root_vn->ops     = &g_initrd_vnode_ops;
    g_initrd.root_vn->private = NULL;

    vfs_register_fs(&g_initrd_fs);

    printk(KERN_INFO "initrd: parsed OK, mounting as /\n");
}

filesystem_t *initrd_get_fs(void) { return &g_initrd_fs; }
