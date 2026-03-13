/* ============================================================
 * heap.c - カーネルヒープアロケータ
 * Slabキャッシュ + 汎用バディアロケータ
 * ============================================================ */

#include "../include/types.h"
#include "../include/kernel.h"
#include "../include/mm.h"

/* ============================================================
 * 汎用ヒープ実装 (free list ベース)
 * 
 * 小サイズ (< 2KB): スラブキャッシュ
 * 大サイズ (≥ 2KB): 直接PMM割り当て
 * ============================================================ */

/* ============================================================
 * ヒープブロックヘッダ
 * ============================================================ */
#define HEAP_MAGIC_FREE  0xDEADBEEF
#define HEAP_MAGIC_USED  0xCAFEBABE

typedef struct heap_block {
    uint32_t magic;
    uint32_t size;           /* ペイロードサイズ (ヘッダ除く) */
    struct heap_block *prev; /* 物理的に前のブロック */
    struct heap_block *next; /* 空きリスト内での次 */
    uint64_t checksum;       /* デバッグ用チェックサム */
} heap_block_t;

#define HEAP_BLOCK_HDR_SIZE  sizeof(heap_block_t)
#define HEAP_MIN_SIZE        16          /* 最小割り当てサイズ */
#define HEAP_ALIGN           16          /* アライメント */

/* ============================================================
 * スラブキャッシュ (固定サイズ高速アロケータ)
 * ============================================================ */
#define SLAB_SIZES_COUNT  13
static const size_t SLAB_SIZES[SLAB_SIZES_COUNT] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768
};

#define SLAB_OBJS_PER_PAGE  (PAGE_SIZE / 64)  /* 最小オブジェクト数の近似 */

typedef struct slab_obj {
    struct slab_obj *next;
} slab_obj_t;

typedef struct slab_cache {
    size_t       obj_size;      /* オブジェクトサイズ */
    slab_obj_t  *free_list;     /* 空きオブジェクトリスト */
    size_t       total;         /* 総オブジェクト数 */
    size_t       used;          /* 使用中オブジェクト数 */
    spinlock_t   lock;
} slab_cache_t;

static slab_cache_t g_slab_caches[SLAB_SIZES_COUNT];

/* ============================================================
 * 汎用ヒープ (大サイズ用)
 * ============================================================ */
#define HEAP_INIT_SIZE   (4 * 1024 * 1024)   /* 初期ヒープサイズ: 4MB */
#define HEAP_GROW_SIZE   (1 * 1024 * 1024)   /* 拡張単位: 1MB */
#define HEAP_VIRT_BASE   0xFFFFF00000000000ULL /* カーネルヒープ仮想ベース */

static uintptr_t g_heap_start = 0;
static uintptr_t g_heap_end   = 0;
static uintptr_t g_heap_brk   = 0;  /* 現在のヒープ末尾 */

static heap_block_t *g_free_list = NULL;  /* 空きブロックリスト */
static spinlock_t    g_heap_lock = SPINLOCK_INIT;

/* ヒープ統計 */
static heap_stats_t g_heap_stats;

/* ============================================================
 * チェックサム
 * ============================================================ */
static uint64_t block_checksum(heap_block_t *blk)
{
    return (uint64_t)blk->magic ^ (uint64_t)blk->size ^
           (uint64_t)(uintptr_t)blk->prev;
}

/* ============================================================
 * スラブキャッシュ初期化・操作
 * ============================================================ */
static void slab_cache_grow(slab_cache_t *cache)
{
    /* 新しいページを割り当て */
    uintptr_t phys = pmm_alloc();
    if (!phys) return;
    
    /* カーネルヒープ仮想空間にマップ */
    static uintptr_t slab_virt = 0;
    if (slab_virt == 0) slab_virt = HEAP_VIRT_BASE + HEAP_INIT_SIZE;
    
    vmm_kmap_phys(slab_virt, phys, PAGE_SIZE, VMM_KERN_RW);
    void *page = (void *)slab_virt;
    slab_virt += PAGE_SIZE;
    
    size_t n = PAGE_SIZE / cache->obj_size;
    uint8_t *p = (uint8_t *)page;
    
    for (size_t i = 0; i < n; i++) {
        slab_obj_t *obj = (slab_obj_t *)p;
        obj->next = cache->free_list;
        cache->free_list = obj;
        p += cache->obj_size;
    }
    
    cache->total += n;
}

static void *slab_alloc(slab_cache_t *cache)
{
    spinlock_lock(&cache->lock);
    
    if (!cache->free_list) {
        slab_cache_grow(cache);
    }
    
    slab_obj_t *obj = cache->free_list;
    if (!obj) {
        spinlock_unlock(&cache->lock);
        return NULL;
    }
    
    cache->free_list = obj->next;
    cache->used++;
    
    spinlock_unlock(&cache->lock);
    
    /* ゼロクリアしない (kmalloc/kzallocで区別) */
    return (void *)obj;
}

static void slab_free(slab_cache_t *cache, void *ptr)
{
    slab_obj_t *obj = (slab_obj_t *)ptr;
    
    spinlock_lock(&cache->lock);
    obj->next = cache->free_list;
    cache->free_list = obj;
    cache->used--;
    spinlock_unlock(&cache->lock);
}

static int slab_find_cache(size_t size)
{
    for (int i = 0; i < SLAB_SIZES_COUNT; i++) {
        if (size <= SLAB_SIZES[i]) return i;
    }
    return -1;
}

/* ============================================================
 * ヒープ初期化
 * ============================================================ */
void heap_init(void)
{
    /* スラブキャッシュ初期化 */
    for (int i = 0; i < SLAB_SIZES_COUNT; i++) {
        g_slab_caches[i].obj_size  = SLAB_SIZES[i];
        g_slab_caches[i].free_list = NULL;
        g_slab_caches[i].total     = 0;
        g_slab_caches[i].used      = 0;
        spinlock_init(&g_slab_caches[i].lock);
    }
    
    /* 汎用ヒープ: 初期メモリ割り当て */
    g_heap_start = HEAP_VIRT_BASE;
    g_heap_end   = HEAP_VIRT_BASE;
    g_heap_brk   = HEAP_VIRT_BASE;
    
    /* 初期ヒープを拡張 */
    size_t n_pages = HEAP_INIT_SIZE / PAGE_SIZE;
    for (size_t i = 0; i < n_pages; i++) {
        uintptr_t phys = pmm_alloc();
        if (!phys) break;
        vmm_kmap_phys(g_heap_end, phys, PAGE_SIZE, VMM_KERN_RW);
        g_heap_end += PAGE_SIZE;
    }
    
    /* 最初の空きブロックを作成 */
    size_t heap_size = g_heap_end - g_heap_start;
    g_free_list = (heap_block_t *)g_heap_start;
    g_free_list->magic    = HEAP_MAGIC_FREE;
    g_free_list->size     = (uint32_t)(heap_size - HEAP_BLOCK_HDR_SIZE);
    g_free_list->prev     = NULL;
    g_free_list->next     = NULL;
    g_free_list->checksum = block_checksum(g_free_list);
    
    g_heap_brk = g_heap_end;
    
    memset(&g_heap_stats, 0, sizeof(g_heap_stats));
    
    printk(KERN_INFO "Heap: initialized at 0x%016llx size=%zu KB\n",
           (unsigned long long)g_heap_start, heap_size >> 10);
}

/* ============================================================
 * ヒープ拡張
 * ============================================================ */
static bool heap_grow(size_t need)
{
    size_t grow = ALIGN_UP(need + HEAP_BLOCK_HDR_SIZE, HEAP_GROW_SIZE);
    size_t n_pages = grow / PAGE_SIZE;
    
    uintptr_t new_start = g_heap_end;
    for (size_t i = 0; i < n_pages; i++) {
        uintptr_t phys = pmm_alloc();
        if (!phys) return false;
        vmm_kmap_phys(g_heap_end, phys, PAGE_SIZE, VMM_KERN_RW);
        g_heap_end += PAGE_SIZE;
    }
    
    /* 新しいブロックを作成 */
    heap_block_t *blk = (heap_block_t *)new_start;
    blk->magic    = HEAP_MAGIC_FREE;
    blk->size     = (uint32_t)(g_heap_end - new_start - HEAP_BLOCK_HDR_SIZE);
    blk->prev     = NULL;
    blk->checksum = block_checksum(blk);
    
    /* 空きリストの末尾に追加 */
    blk->next = g_free_list;
    g_free_list = blk;
    
    return true;
}

/* ============================================================
 * kmalloc / kfree
 * ============================================================ */
void *kmalloc(size_t size)
{
    if (size == 0) return NULL;
    
    size = ALIGN_UP(size, HEAP_ALIGN);
    
    /* スラブキャッシュを試みる */
    int slab_idx = slab_find_cache(size);
    if (slab_idx >= 0) {
        void *ptr = slab_alloc(&g_slab_caches[slab_idx]);
        if (ptr) {
            __atomic_add_fetch(&g_heap_stats.total_allocated, SLAB_SIZES[slab_idx], __ATOMIC_RELAXED);
            __atomic_add_fetch(&g_heap_stats.current_usage,   SLAB_SIZES[slab_idx], __ATOMIC_RELAXED);
            __atomic_add_fetch(&g_heap_stats.alloc_count, 1, __ATOMIC_RELAXED);
            return ptr;
        }
    }
    
    /* 汎用ヒープ */
    spinlock_lock(&g_heap_lock);
    
retry:;
    heap_block_t *blk = g_free_list;
    heap_block_t *best = NULL;
    size_t        best_size = SIZE_MAX;
    
    /* Best-fit 検索 */
    while (blk) {
        if (blk->magic != HEAP_MAGIC_FREE) {
            PANIC("Heap corruption: bad magic 0x%x at %p", blk->magic, blk);
        }
        if (blk->size >= size && blk->size < best_size) {
            best      = blk;
            best_size = blk->size;
            if (best_size == size) break;  /* 完全一致 */
        }
        blk = blk->next;
    }
    
    if (!best) {
        /* ヒープ拡張 */
        spinlock_unlock(&g_heap_lock);
        if (!heap_grow(size)) return NULL;
        spinlock_lock(&g_heap_lock);
        goto retry;
    }
    
    /* 分割: 残りが十分あれば分割する */
    if (best->size >= size + HEAP_BLOCK_HDR_SIZE + HEAP_MIN_SIZE) {
        heap_block_t *split = (heap_block_t *)((uint8_t *)best + HEAP_BLOCK_HDR_SIZE + size);
        split->magic    = HEAP_MAGIC_FREE;
        split->size     = (uint32_t)(best->size - size - HEAP_BLOCK_HDR_SIZE);
        split->prev     = best;
        split->checksum = block_checksum(split);
        
        /* 空きリストで best を split に置換 */
        split->next = best->next;
        if (best->next) {
            /* listから best を削除して split を挿入 */
        }
        
        /* free_list を更新 */
        heap_block_t **pp = &g_free_list;
        while (*pp && *pp != best) pp = &(*pp)->next;
        if (*pp == best) *pp = split;
        split->next = best->next;
    } else {
        /* 分割しない: free_listからbestを除去 */
        heap_block_t **pp = &g_free_list;
        while (*pp && *pp != best) pp = &(*pp)->next;
        if (*pp == best) *pp = best->next;
    }
    
    best->magic    = HEAP_MAGIC_USED;
    best->next     = NULL;
    best->checksum = block_checksum(best);
    
    spinlock_unlock(&g_heap_lock);
    
    void *ptr = (void *)((uint8_t *)best + HEAP_BLOCK_HDR_SIZE);
    
    __atomic_add_fetch(&g_heap_stats.total_allocated, best->size, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_heap_stats.current_usage,   best->size, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_heap_stats.alloc_count, 1, __ATOMIC_RELAXED);
    
    return ptr;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

void *kcalloc(size_t n, size_t size)
{
    if (n == 0 || size == 0) return NULL;
    if (n > SIZE_MAX / size) return NULL;  /* オーバーフロー */
    return kzalloc(n * size);
}

void *krealloc(void *ptr, size_t new_size)
{
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return NULL; }
    
    /* ブロックサイズを確認 */
    heap_block_t *blk = (heap_block_t *)((uint8_t *)ptr - HEAP_BLOCK_HDR_SIZE);
    
    /* スラブ管理かどうかは判定が難しいので新規割り当て+コピー */
    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;
    
    /* 古いサイズを推定 (ブロックヘッダが有効なら使う) */
    size_t copy_size = new_size;
    if (blk->magic == HEAP_MAGIC_USED && blk->size < copy_size) {
        copy_size = blk->size;
    }
    
    memcpy(new_ptr, ptr, copy_size);
    kfree(ptr);
    return new_ptr;
}

void kfree(void *ptr)
{
    if (!ptr) return;
    
    /* スラブキャッシュのオブジェクトか確認 */
    /* アドレスが HEAP_VIRT_BASE 以外ならスラブ */
    uintptr_t addr = (uintptr_t)ptr;
    if (addr < HEAP_VIRT_BASE || addr >= HEAP_VIRT_BASE + HEAP_INIT_SIZE * 4) {
        /* スラブキャッシュ (ページ境界から推定) */
        /* TODO: スラブキャッシュ逆引き */
        /* 現在はシンプルに: アドレスからキャッシュを推定 */
        for (int i = 0; i < SLAB_SIZES_COUNT; i++) {
            /* スラブオブジェクトかはサイズ境界では判定できないが
             * 今はヒューリスティックで */
        }
    }
    
    /* 汎用ヒープ */
    heap_block_t *blk = (heap_block_t *)((uint8_t *)ptr - HEAP_BLOCK_HDR_SIZE);
    
    if (blk->magic == HEAP_MAGIC_FREE) {
        PANIC("kfree: double free at %p", ptr);
        return;
    }
    if (blk->magic != HEAP_MAGIC_USED) {
        PANIC("kfree: invalid pointer %p (magic=0x%x)", ptr, blk->magic);
        return;
    }
    
    spinlock_lock(&g_heap_lock);
    
    blk->magic = HEAP_MAGIC_FREE;
    blk->checksum = block_checksum(blk);
    
    /* 空きリストの先頭に追加 */
    blk->next = g_free_list;
    g_free_list = blk;
    
    __atomic_add_fetch(&g_heap_stats.total_freed,   blk->size, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&g_heap_stats.current_usage, blk->size, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_heap_stats.free_count, 1, __ATOMIC_RELAXED);
    
    spinlock_unlock(&g_heap_lock);
}

/* アライメント付き割り当て */
void *kmalloc_aligned(size_t size, size_t align)
{
    if (align <= HEAP_ALIGN) return kmalloc(size);
    
    /* align分余計に確保してアライメント調整 */
    void *raw = kmalloc(size + align - 1 + sizeof(void*));
    if (!raw) return NULL;
    
    uintptr_t aligned = ALIGN_UP((uintptr_t)raw + sizeof(void*), align);
    *((void**)(aligned - sizeof(void*))) = raw;
    return (void *)aligned;
}

void kfree_aligned(void *ptr)
{
    if (!ptr) return;
    void *raw = *((void**)((uintptr_t)ptr - sizeof(void*)));
    kfree(raw);
}

/* ページ単位割り当て */
void *alloc_pages(size_t n)
{
    size_t size = n * PAGE_SIZE;
    
    /* 連続物理ページを割り当てて仮想空間にマップ */
    uintptr_t phys = pmm_alloc_n(n);
    if (!phys) return NULL;
    
    /* カーネル仮想空間の空きを探す... 今はdirect mapを使う */
    return phys_to_kvirt(phys);
}

void free_pages(void *ptr, size_t n)
{
    if (!ptr) return;
    uintptr_t phys = kvirt_to_phys(ptr);
    pmm_free_n(phys, n);
}

/* 統計取得 */
void heap_get_stats(heap_stats_t *stats)
{
    *stats = g_heap_stats;
}
