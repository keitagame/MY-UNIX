#pragma once
#ifndef _MM_H
#define _MM_H

/* ============================================================
 * mm.h - メモリ管理ヘッダ (PMM / VMM / Heap)
 * ============================================================ */

#include "../types.h"

/* ============================================================
 * 物理メモリマネージャ (PMM)
 * ============================================================ */

/* 物理ページフレーム番号 */
typedef uint64_t pfn_t;

/* ページ数制限 */
#define PMM_MAX_PAGES   (1ULL << 30)  /* 最大4TB */
#define PMM_ZONE_LOW    (0x100000ULL)          /* 1MB以下 */
#define PMM_ZONE_DMA    (0x1000000ULL)         /* 16MB以下 */
#define PMM_ZONE_NORMAL (0x100000000ULL)       /* 4GB以下 */
#define PMM_ZONE_HIGH   UINT64_MAX             /* それ以上 */

/* ゾーンID */
typedef enum {
    ZONE_LOW    = 0,  /* ISA DMA等 */
    ZONE_DMA    = 1,  /* 旧DMAデバイス */
    ZONE_NORMAL = 2,  /* 通常カーネル使用 */
    ZONE_HIGH   = 3,  /* 64bitのみ */
    ZONE_COUNT  = 4,
} zone_id_t;

/* PMM初期化 */
void pmm_init(uint64_t *mem_map, size_t entries);
void pmm_add_region(uint64_t base, uint64_t size);
void pmm_reserve_region(uint64_t base, uint64_t size);

/* ページ割り当て */
uintptr_t pmm_alloc(void);               /* 1ページ割り当て */
uintptr_t pmm_alloc_zone(zone_id_t z);   /* ゾーン指定 */
uintptr_t pmm_alloc_n(size_t n);         /* n連続ページ */
void      pmm_free(uintptr_t phys);      /* ページ解放 */
void      pmm_free_n(uintptr_t phys, size_t n);

/* 統計 */
size_t    pmm_total_pages(void);
size_t    pmm_free_pages(void);
size_t    pmm_used_pages(void);

/* ============================================================
 * 仮想メモリマネージャ (VMM)
 * ============================================================ */

/* x86_64 ページングフラグ */
#define PTE_PRESENT     BIT(0)
#define PTE_WRITABLE    BIT(1)
#define PTE_USER        BIT(2)
#define PTE_WRITE_THRU  BIT(3)
#define PTE_NO_CACHE    BIT(4)
#define PTE_ACCESSED    BIT(5)
#define PTE_DIRTY       BIT(6)
#define PTE_HUGE        BIT(7)  /* 2MB/1GBページ */
#define PTE_GLOBAL      BIT(8)
#define PTE_NX          BIT(63) /* No Execute */

/* 一般的なマッピング組み合わせ */
#define VMM_KERN_RW   (PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL)
#define VMM_KERN_RO   (PTE_PRESENT | PTE_GLOBAL)
#define VMM_KERN_RWX  (PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL)
#define VMM_USER_RW   (PTE_PRESENT | PTE_WRITABLE | PTE_USER)
#define VMM_USER_RO   (PTE_PRESENT | PTE_USER)
#define VMM_USER_RWX  (PTE_PRESENT | PTE_WRITABLE | PTE_USER)
#define VMM_DEVICE    (PTE_PRESENT | PTE_WRITABLE | PTE_NO_CACHE)

/* アドレス空間 */
#define VMM_USER_START  0x0000000000400000ULL  /* ユーザーコード開始 */
#define VMM_USER_END    0x00007FFFFFFFF000ULL  /* ユーザー空間終端 */
#define VMM_STACK_TOP   0x00007FFFFFFFE000ULL  /* ユーザースタックトップ */
#define VMM_STACK_SIZE  (8 * 1024 * 1024)      /* 8MB スタック */
#define VMM_MMAP_BASE   0x0000700000000000ULL  /* mmap開始アドレス */
#define VMM_KERN_START  0xFFFFFFFF80000000ULL  /* カーネル開始 */

/* ページテーブル構造体 */
typedef uint64_t pte_t;
typedef uint64_t pde_t;
typedef uint64_t pdpte_t;
typedef uint64_t pml4e_t;

typedef struct {
    pml4e_t entries[512];
} PACKED pml4_t;

typedef struct {
    pdpte_t entries[512];
} PACKED pdpt_t;

typedef struct {
    pde_t entries[512];
} PACKED pd_t;

typedef struct {
    pte_t entries[512];
} PACKED pt_t;

/* アドレス空間 */
typedef struct vmm_space {
    pml4_t  *pml4;         /* PML4 物理アドレス */
    uintptr_t pml4_phys;
    spinlock_t lock;
    atomic_t   refcount;
    
    /* VMエリアのリスト */
    struct list_head vma_list;
    
    /* 統計 */
    size_t  n_pages;       /* マップ済みページ数 */
    size_t  n_vmas;        /* VMA数 */
} vmm_space_t;

/* 仮想メモリエリア */
typedef struct vma {
    struct list_head list;
    uintptr_t  start;      /* 開始アドレス (ページ境界) */
    uintptr_t  end;        /* 終端アドレス (含まない) */
    uint64_t   flags;      /* PTE フラグ */
    uint32_t   prot;       /* PROT_READ/WRITE/EXEC */
    uint32_t   mmap_flags; /* MAP_PRIVATE/SHARED/etc */
    int        fd;         /* ファイルマッピング: fd (-1なら匿名) */
    off_t      offset;     /* ファイルオフセット */
    struct vnode *vnode;   /* ファイルvnode (あれば) */
    atomic_t   refcount;
} vma_t;

/* VMM初期化 */
void vmm_init(void);
vmm_space_t *vmm_create_space(void);
vmm_space_t *vmm_clone_space(vmm_space_t *src);
void         vmm_destroy_space(vmm_space_t *space);
void         vmm_switch_space(vmm_space_t *space);
vmm_space_t *vmm_kernel_space(void);

/* ページマッピング */
int  vmm_map(vmm_space_t *space, uintptr_t virt, uintptr_t phys, uint64_t flags);
int  vmm_map_range(vmm_space_t *space, uintptr_t virt, uintptr_t phys,
                   size_t n_pages, uint64_t flags);
int  vmm_unmap(vmm_space_t *space, uintptr_t virt);
int  vmm_unmap_range(vmm_space_t *space, uintptr_t virt, size_t n_pages);

/* アドレス解決 */
uintptr_t vmm_virt_to_phys(vmm_space_t *space, uintptr_t virt);
bool      vmm_is_mapped(vmm_space_t *space, uintptr_t virt);

/* ユーザー空間操作 */
uintptr_t vmm_alloc_user(vmm_space_t *space, uintptr_t hint, size_t size,
                          uint64_t flags);
int       vmm_free_user(vmm_space_t *space, uintptr_t virt, size_t size);
int       vmm_protect(vmm_space_t *space, uintptr_t virt, size_t size, uint32_t prot);

/* VMA管理 */
vma_t    *vmm_find_vma(vmm_space_t *space, uintptr_t addr);
vma_t    *vmm_create_vma(vmm_space_t *space, uintptr_t start, size_t size,
                          uint32_t prot, uint32_t flags);
void      vmm_destroy_vma(vmm_space_t *space, vma_t *vma);

/* カーネルページマッピング */
void  vmm_kmap_phys(uintptr_t virt, uintptr_t phys, size_t size, uint64_t flags);
void *vmm_kmap_temp(uintptr_t phys);
void  vmm_kunmap_temp(void *virt);

/* ============================================================
 * カーネルヒープ (slab風アロケータ)
 * ============================================================ */

/* ヒープ初期化 */
void  heap_init(void);

/* 基本アロケーション */
void *kmalloc(size_t size);
void *kzalloc(size_t size);       /* ゼロ初期化 */
void *krealloc(void *ptr, size_t size);
void *kcalloc(size_t n, size_t size);
void  kfree(void *ptr);

/* アラインメント指定アロケーション */
void *kmalloc_aligned(size_t size, size_t align);
void  kfree_aligned(void *ptr);

/* ページアロケーション (kmalloc上位版) */
void *alloc_pages(size_t n);       /* n * PAGE_SIZE バイト確保 */
void  free_pages(void *ptr, size_t n);

/* デバッグ統計 */
typedef struct {
    size_t total_allocated;
    size_t total_freed;
    size_t current_usage;
    size_t peak_usage;
    size_t alloc_count;
    size_t free_count;
} heap_stats_t;

void heap_get_stats(heap_stats_t *stats);

/* ============================================================
 * ユーザー空間メモリアクセスの安全操作
 * ============================================================ */
int  copy_from_user(void *dst, const void *user_src, size_t len);
int  copy_to_user(void *user_dst, const void *src, size_t len);
ssize_t strncpy_from_user(char *dst, const char *user_src, size_t maxlen);
bool    access_ok(const void *addr, size_t size);

/* ============================================================
 * カーネル物理メモリマップ
 * ============================================================ */
/* 物理アドレスのカーネル空間へのマッピング ( identity-mapped領域) */
#define PHYS_MAP_BASE   0xFFFF800000000000ULL  /* 物理→仮想直接マップベース */
#define PHYS_MAP_END    0xFFFFFF0000000000ULL

static ALWAYS_INLINE void *phys_to_kvirt(uintptr_t phys) {
    return (void *)(phys + PHYS_MAP_BASE);
}
static ALWAYS_INLINE uintptr_t kvirt_to_phys(const void *virt) {
    return (uintptr_t)virt - PHYS_MAP_BASE;
}

#endif /* _MM_H */
