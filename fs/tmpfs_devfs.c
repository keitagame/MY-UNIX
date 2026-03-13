/* ============================================================
 * tmpfs.c - テンポラリファイルシステム (メモリ上)
 * devfs.c - デバイスファイルシステム
 * ============================================================ */

#include "../include/types.h"
#include "../include/kernel.h"
#include "../include/mm.h"
#include "../include/fs.h"
#include "../include/process.h"

/* ============================================================
 * tmpfs ノード
 * ============================================================ */
typedef struct tmpfs_node {
    char     name[NAME_MAX + 1];
    vnode_t *vn;
    struct tmpfs_node *parent;
    struct list_head   children;
    struct list_head   sibling;
    uint8_t           *data;       /* ファイルデータ */
    size_t             data_cap;   /* バッファ容量 */
} tmpfs_node_t;

typedef struct {
    tmpfs_node_t *root;
    ino_t         next_ino;
    spinlock_t    lock;
} tmpfs_mount_t;

/* ============================================================
 * tmpfs ヘルパー
 * ============================================================ */
static tmpfs_node_t *tmpfs_find_child(tmpfs_node_t *parent, const char *name)
{
    struct list_head *pos;
    list_for_each(pos, &parent->children) {
        tmpfs_node_t *child = list_entry(pos, tmpfs_node_t, sibling);
        if (strcmp(child->name, name) == 0) return child;
    }
    return NULL;
}

static tmpfs_node_t *tmpfs_node_alloc(tmpfs_mount_t *tfs, const char *name, mode_t mode)
{
    tmpfs_node_t *node = kzalloc(sizeof(tmpfs_node_t));
    if (!node) return NULL;

    strncpy(node->name, name, NAME_MAX);
    INIT_LIST_HEAD(&node->children);
    INIT_LIST_HEAD(&node->sibling);

    node->vn = vnode_alloc();
    if (!node->vn) { kfree(node); return NULL; }

    node->vn->mode    = mode;
    node->vn->ino     = tfs->next_ino++;
    node->vn->private = node;
    if (S_ISDIR(mode)) {
        node->vn->type  = VN_DIR;
        node->vn->nlink = 2;
    } else if (S_ISCHR(mode) || S_ISBLK(mode)) {
        node->vn->type = S_ISCHR(mode) ? VN_CHR : VN_BLK;
    } else {
        node->vn->type  = VN_REG;
        node->vn->nlink = 1;
    }

    return node;
}

/* ============================================================
 * tmpfs vnode ops
 * ============================================================ */
static ssize_t tmpfs_read(vnode_t *vn, void *buf, size_t len, off_t off)
{
    tmpfs_node_t *node = (tmpfs_node_t *)vn->private;
    if (!node || node->vn->type == VN_DIR) return -EISDIR;
    if (off < 0 || (uint64_t)off >= vn->size) return 0;

    size_t avail = vn->size - (size_t)off;
    size_t n     = MIN(len, avail);
    memcpy(buf, node->data + off, n);
    return (ssize_t)n;
}

static ssize_t tmpfs_write(vnode_t *vn, const void *buf, size_t len, off_t off)
{
    tmpfs_node_t *node = (tmpfs_node_t *)vn->private;
    if (!node || node->vn->type == VN_DIR) return -EISDIR;

    size_t new_end = (size_t)off + len;
    if (new_end > node->data_cap) {
        /* バッファ拡張 */
        size_t new_cap = ALIGN_UP(new_end, PAGE_SIZE);
        uint8_t *new_data = krealloc(node->data, new_cap);
        if (!new_data) return -ENOSPC;
        if (new_cap > node->data_cap)
            memset(new_data + node->data_cap, 0, new_cap - node->data_cap);
        node->data     = new_data;
        node->data_cap = new_cap;
    }

    memcpy(node->data + off, buf, len);
    if (new_end > vn->size) vn->size = new_end;

    return (ssize_t)len;
}

static int tmpfs_truncate(vnode_t *vn, off_t size)
{
    tmpfs_node_t *node = (tmpfs_node_t *)vn->private;
    if (!node) return -EINVAL;

    if ((uint64_t)size > node->data_cap) {
        size_t new_cap = ALIGN_UP((size_t)size, PAGE_SIZE);
        uint8_t *new_data = krealloc(node->data, new_cap);
        if (!new_data) return -ENOMEM;
        memset(new_data + node->data_cap, 0, new_cap - node->data_cap);
        node->data     = new_data;
        node->data_cap = new_cap;
    }

    vn->size = (size_t)size;
    return 0;
}

static int tmpfs_lookup(vnode_t *dir, const char *name, vnode_t **out)
{
    tmpfs_node_t *parent = (tmpfs_node_t *)dir->private;
    if (!parent) return -EINVAL;

    /* ".." 処理 */
    if (strcmp(name, "..") == 0) {
        vnode_t *pv = parent->parent ? parent->parent->vn : dir;
        vnode_get(pv);
        *out = pv;
        return 0;
    }
    if (strcmp(name, ".") == 0) {
        vnode_get(dir);
        *out = dir;
        return 0;
    }

    tmpfs_node_t *child = tmpfs_find_child(parent, name);
    if (!child) return -ENOENT;

    vnode_get(child->vn);
    *out = child->vn;
    return 0;
}

static int tmpfs_create(vnode_t *dir, const char *name, mode_t mode, vnode_t **out)
{
    tmpfs_node_t *parent = (tmpfs_node_t *)dir->private;
    if (!parent) return -EINVAL;

    if (tmpfs_find_child(parent, name)) return -EEXIST;

    /* TFSを取得 */
    tmpfs_mount_t *tfs = (tmpfs_mount_t *)dir->fs->name; /* hack */
    /* 実際はmountからprivateで取得すべき */
    static tmpfs_mount_t *g_tfs = NULL; /* 後で修正 */
    if (!g_tfs) return -EIO;

    tmpfs_node_t *node = tmpfs_node_alloc(g_tfs, name, mode | S_IFREG);
    if (!node) return -ENOMEM;

    node->parent = parent;
    list_add_tail(&node->sibling, &parent->children);
    dir->nlink++;

    vnode_get(node->vn);
    *out = node->vn;
    return 0;
}

static int tmpfs_mkdir(vnode_t *dir, const char *name, mode_t mode)
{
    tmpfs_node_t *parent = (tmpfs_node_t *)dir->private;
    if (!parent) return -EINVAL;
    if (tmpfs_find_child(parent, name)) return -EEXIST;

    static tmpfs_mount_t *g_tfs_ptr = NULL;
    if (!g_tfs_ptr) return -EIO;

    tmpfs_node_t *node = tmpfs_node_alloc(g_tfs_ptr, name, mode | S_IFDIR);
    if (!node) return -ENOMEM;

    node->parent = parent;
    list_add_tail(&node->sibling, &parent->children);
    dir->nlink++;
    return 0;
}

static int tmpfs_unlink(vnode_t *dir, const char *name)
{
    tmpfs_node_t *parent = (tmpfs_node_t *)dir->private;
    if (!parent) return -EINVAL;

    tmpfs_node_t *child = tmpfs_find_child(parent, name);
    if (!child) return -ENOENT;
    if (child->vn->type == VN_DIR && !list_empty(&child->children))
        return -ENOTEMPTY;

    list_del(&child->sibling);
    dir->nlink--;

    if (child->data) kfree(child->data);
    vnode_put(child->vn);
    kfree(child);
    return 0;
}

static int tmpfs_stat(vnode_t *vn, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode    = vn->mode;
    st->st_uid     = vn->uid;
    st->st_gid     = vn->gid;
    st->st_size    = vn->size;
    st->st_ino     = vn->ino;
    st->st_nlink   = vn->nlink;
    st->st_blksize = PAGE_SIZE;
    st->st_blocks  = (vn->size + 511) / 512;
    return 0;
}

static int tmpfs_readdir(vnode_t *dir, off_t off, struct dirent *ent)
{
    tmpfs_node_t *parent = (tmpfs_node_t *)dir->private;
    if (!parent) return -EINVAL;

    /* ".": idx=0, "..": idx=1 */
    if (off == 0) {
        ent->d_ino  = dir->ino;
        ent->d_off  = 1;
        ent->d_type = DT_DIR;
        strcpy(ent->d_name, ".");
        ent->d_reclen = sizeof(*ent);
        return 0;
    }
    if (off == 1) {
        vnode_t *pv = parent->parent ? parent->parent->vn : dir;
        ent->d_ino  = pv->ino;
        ent->d_off  = 2;
        ent->d_type = DT_DIR;
        strcpy(ent->d_name, "..");
        ent->d_reclen = sizeof(*ent);
        return 0;
    }

    off_t idx = 2;
    struct list_head *pos;
    list_for_each(pos, &parent->children) {
        tmpfs_node_t *child = list_entry(pos, tmpfs_node_t, sibling);
        if (idx == off) {
            ent->d_ino    = child->vn->ino;
            ent->d_off    = idx + 1;
            ent->d_type   = child->vn->type == VN_DIR ? DT_DIR : DT_REG;
            strncpy(ent->d_name, child->name, NAME_MAX);
            ent->d_reclen = sizeof(*ent);
            return 0;
        }
        idx++;
    }
    return -ENOENT;
}

static const struct vnode_ops g_tmpfs_vnode_ops = {
    .read    = tmpfs_read,
    .write   = tmpfs_write,
    .truncate = tmpfs_truncate,
    .lookup  = tmpfs_lookup,
    .create  = tmpfs_create,
    .mkdir   = tmpfs_mkdir,
    .unlink  = tmpfs_unlink,
    .stat    = tmpfs_stat,
    .readdir = tmpfs_readdir,
};

/* ============================================================
 * tmpfs グローバル
 * ============================================================ */
static tmpfs_mount_t g_tmpfs_inst;

static int tmpfs_mount_fn(struct mount *mp, const char *device, uint32_t flags)
{
    UNUSED(device); UNUSED(flags);
    tmpfs_mount_t *tfs = &g_tmpfs_inst;
    tfs->next_ino = 1;
    spinlock_init(&tfs->lock);
    INIT_LIST_HEAD(&tfs->root->children); /* root初期化は get_root で */
    mp->private = tfs;
    return 0;
}

static int tmpfs_get_root(struct mount *mp, vnode_t **out)
{
    tmpfs_mount_t *tfs = (tmpfs_mount_t *)mp->private;

    tmpfs_node_t *root = kzalloc(sizeof(tmpfs_node_t));
    if (!root) return -ENOMEM;

    strcpy(root->name, "/");
    INIT_LIST_HEAD(&root->children);
    INIT_LIST_HEAD(&root->sibling);
    root->parent = NULL;
    tfs->root    = root;

    vnode_t *vn = vnode_alloc();
    if (!vn) { kfree(root); return -ENOMEM; }

    vn->type    = VN_DIR;
    vn->mode    = S_IFDIR | 0755;
    vn->ino     = tfs->next_ino++;
    vn->nlink   = 2;
    vn->ops     = &g_tmpfs_vnode_ops;
    vn->private = root;
    root->vn    = vn;

    *out = vn;
    return 0;
}

static filesystem_t g_tmpfs = {
    .name      = "tmpfs",
    .mount     = tmpfs_mount_fn,
    .get_root  = tmpfs_get_root,
};

filesystem_t *tmpfs_get_fs(void)
{
    vfs_register_fs(&g_tmpfs);
    return &g_tmpfs;
}

/* ============================================================
 * /dev/null, /dev/zero, /dev/random  (キャラクタデバイス)
 * ============================================================ */

/* /dev/null */
static ssize_t devnull_read(vnode_t *vn, void *buf, size_t len, off_t off)
{ UNUSED(vn); UNUSED(buf); UNUSED(len); UNUSED(off); return 0; }

static ssize_t devnull_write(vnode_t *vn, const void *buf, size_t len, off_t off)
{ UNUSED(vn); UNUSED(buf); UNUSED(off); return (ssize_t)len; }

static const struct vnode_ops g_devnull_ops = {
    .read  = devnull_read,
    .write = devnull_write,
};

/* /dev/zero */
static ssize_t devzero_read(vnode_t *vn, void *buf, size_t len, off_t off)
{ UNUSED(vn); UNUSED(off); memset(buf, 0, len); return (ssize_t)len; }

static const struct vnode_ops g_devzero_ops = {
    .read  = devzero_read,
    .write = devnull_write,
};

/* /dev/random (簡易乱数) */
static uint64_t g_rand_state = 0xDEADBEEFCAFEBABEULL;

static ssize_t devrand_read(vnode_t *vn, void *buf, size_t len, off_t off)
{
    UNUSED(vn); UNUSED(off);
    uint8_t *p = (uint8_t *)buf;
    for (size_t i = 0; i < len; i++) {
        /* xorshift64 */
        g_rand_state ^= g_rand_state << 13;
        g_rand_state ^= g_rand_state >> 7;
        g_rand_state ^= g_rand_state << 17;
        p[i] = (uint8_t)g_rand_state;
    }
    return (ssize_t)len;
}

static const struct vnode_ops g_devrand_ops = {
    .read  = devrand_read,
    .write = devnull_write,
};

/* ============================================================
 * devfs 実装
 * ============================================================ */
static tmpfs_node_t *g_devfs_root_node = NULL;
static tmpfs_mount_t g_devfs_inst;
static vnode_t *g_devfs_root_vn = NULL;

static int devfs_mount_fn(struct mount *mp, const char *device, uint32_t flags)
{
    UNUSED(device); UNUSED(flags);
    g_devfs_inst.next_ino = 100;
    spinlock_init(&g_devfs_inst.lock);
    mp->private = &g_devfs_inst;
    return 0;
}

static int devfs_get_root(struct mount *mp, vnode_t **out)
{
    UNUSED(mp);
    vnode_get(g_devfs_root_vn);
    *out = g_devfs_root_vn;
    return 0;
}

static filesystem_t g_devfs = {
    .name     = "devfs",
    .mount    = devfs_mount_fn,
    .get_root = devfs_get_root,
};

/* デバイスvnodeをdevfsに追加 */
static void devfs_add(const char *name, const struct vnode_ops *ops,
                      mode_t mode, dev_t devno)
{
    if (!g_devfs_root_node) return;

    tmpfs_node_t *node = kzalloc(sizeof(tmpfs_node_t));
    if (!node) return;

    strncpy(node->name, name, NAME_MAX);
    INIT_LIST_HEAD(&node->children);
    INIT_LIST_HEAD(&node->sibling);
    node->parent = g_devfs_root_node;

    vnode_t *vn = vnode_alloc();
    if (!vn) { kfree(node); return; }

    vn->mode    = mode;
    vn->ino     = g_devfs_inst.next_ino++;
    vn->dev     = devno;
    vn->ops     = ops;
    vn->private = node;

    if (S_ISCHR(mode)) vn->type = VN_CHR;
    else if (S_ISBLK(mode)) vn->type = VN_BLK;
    else vn->type = VN_REG;

    node->vn = vn;
    list_add_tail(&node->sibling, &g_devfs_root_node->children);
}

void devfs_init(void)
{
    /* devfsルートノード作成 */
    g_devfs_root_node = kzalloc(sizeof(tmpfs_node_t));
    if (!g_devfs_root_node) return;

    strcpy(g_devfs_root_node->name, "dev");
    INIT_LIST_HEAD(&g_devfs_root_node->children);
    INIT_LIST_HEAD(&g_devfs_root_node->sibling);
    g_devfs_inst.next_ino = 100;

    g_devfs_root_vn = vnode_alloc();
    if (!g_devfs_root_vn) return;

    g_devfs_root_vn->type    = VN_DIR;
    g_devfs_root_vn->mode    = S_IFDIR | 0755;
    g_devfs_root_vn->ino     = g_devfs_inst.next_ino++;
    g_devfs_root_vn->nlink   = 2;
    g_devfs_root_vn->ops     = &g_tmpfs_vnode_ops;  /* ディレクトリops */
    g_devfs_root_vn->private = g_devfs_root_node;
    g_devfs_root_node->vn    = g_devfs_root_vn;

    /* デバイスファイル作成 */
    devfs_add("null",    &g_devnull_ops, S_IFCHR | 0666, MAKEDEV(1, 3));
    devfs_add("zero",    &g_devzero_ops, S_IFCHR | 0666, MAKEDEV(1, 5));
    devfs_add("full",    &g_devzero_ops, S_IFCHR | 0666, MAKEDEV(1, 7));
    devfs_add("random",  &g_devrand_ops, S_IFCHR | 0444, MAKEDEV(1, 8));
    devfs_add("urandom", &g_devrand_ops, S_IFCHR | 0444, MAKEDEV(1, 9));

    vfs_register_fs(&g_devfs);
    printk(KERN_INFO "devfs: initialized (null/zero/random)\n");
}

filesystem_t *devfs_get_fs(void) { return &g_devfs; }
