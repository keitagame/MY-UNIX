/* ============================================================
 * pmm.c - 物理メモリマネージャ
 * ビットマップベースのページアロケータ
 * ============================================================ */

#include "../include/types.h"
#include "../include/kernel.h"
#include "../include/mm.h"
#include "../include/multiboot2.h"

/* ============================================================
 * ビットマップ物理メモリマネージャ
 * ============================================================ */
#define PMM_BITMAP_BITS_PER_WORD  64
#define PMM_MAX_REGIONS           32

typedef struct {
    uint64_t base;   /* 物理ベースアドレス */
    uint64_t size;   /* バイト数 */
    int      type;   /* MB2_MMAP_* */
} mem_region_t;

/* ============================================================
 * グローバル状態
 * ============================================================ */
static uint64_t *g_bitmap    = NULL;  /* ビットマップ (1=使用中) */
static size_t    g_total_pages = 0;
static size_t    g_free_pages  = 0;
static size_t    g_used_pages  = 0;
static uint64_t  g_max_phys    = 0;   /* 最大物理アドレス */

static mem_region_t g_regions[PMM_MAX_REGIONS];
static int          g_n_regions = 0;

static spinlock_t g_pmm_lock = SPINLOCK_INIT;

/* カーネルの物理アドレス範囲 (予約) */
extern uint8_t _kernel_start[];
extern uint8_t _kernel_end[];

/* ============================================================
 * ビットマップ操作
 * ============================================================ */
static ALWAYS_INLINE void bitmap_set(uint64_t page)
{
    g_bitmap[page / 64] |= BIT(page % 64);
}

static ALWAYS_INLINE void bitmap_clear(uint64_t page)
{
    g_bitmap[page / 64] &= ~BIT(page % 64);
}

static ALWAYS_INLINE bool bitmap_test(uint64_t page)
{
    return (g_bitmap[page / 64] & BIT(page % 64)) != 0;
}

/* ============================================================
 * PMM 初期化
 * ============================================================ */
void pmm_init_from_multiboot(struct multiboot2_info *mbi)
{
    /* メモリマップタグを探す */
    struct mb2_tag_mmap *mmap_tag =
        (struct mb2_tag_mmap *)mb2_find_tag(mbi, MB2_TAG_MMAP);
    
    if (!mmap_tag) {
        PANIC("PMM: no memory map from bootloader");
    }
    
    g_max_phys  = 0;
    g_n_regions = 0;
    
    /* メモリ領域列挙 */
    struct mb2_mmap_entry *entry = mmap_tag->entries;
    size_t n_entries = (mmap_tag->size - sizeof(*mmap_tag)) / mmap_tag->entry_size;
    
    printk(KERN_INFO "PMM: memory map (%zu entries):\n", n_entries);
    
    for (size_t i = 0; i < n_entries; i++) {
        const char *type_str;
        switch (entry->type) {
        case MB2_MMAP_AVAILABLE:        type_str = "Available"; break;
        case MB2_MMAP_RESERVED:         type_str = "Reserved";  break;
        case MB2_MMAP_ACPI_RECLAIMABLE: type_str = "ACPI Recl"; break;
        case MB2_MMAP_NVS:              type_str = "ACPI NVS";  break;
        case MB2_MMAP_BADRAM:           type_str = "Bad RAM";   break;
        default:                         type_str = "Unknown";   break;
        }
        
        printk(KERN_INFO "  [%zu] 0x%016llx - 0x%016llx (%s, %llu MB)\n",
               i,
               (unsigned long long)entry->addr,
               (unsigned long long)(entry->addr + entry->len),
               type_str,
               (unsigned long long)(entry->len >> 20));
        
        if (g_n_regions < PMM_MAX_REGIONS) {
            g_regions[g_n_regions].base = entry->addr;
            g_regions[g_n_regions].size = entry->len;
            g_regions[g_n_regions].type = entry->type;
            g_n_regions++;
        }
        
        uint64_t end = entry->addr + entry->len;
        if (end > g_max_phys) g_max_phys = end;
        
        entry = (struct mb2_mmap_entry *)((uint8_t *)entry + mmap_tag->entry_size);
    }
    
    /* 総ページ数 */
    g_total_pages = g_max_phys / PAGE_SIZE;
    
    /* ビットマップ配置
     * ビットマップ自身をカーネル直後に配置する
     * (カーネルは lower-half では 1MB に配置) */
    size_t bitmap_bytes = ALIGN_UP(g_total_pages / 8, PAGE_SIZE);
    
    /* カーネルの物理終端アドレスを求める */
    uintptr_t kern_end_phys = VIRT_TO_PHYS((uintptr_t)_kernel_end);
    kern_end_phys = ALIGN_UP(kern_end_phys, PAGE_SIZE);
    
    /* ビットマップをカーネル直後の物理メモリに配置 */
    uintptr_t bitmap_phys = kern_end_phys;
    g_bitmap = (uint64_t *)PHYS_TO_VIRT(bitmap_phys);
    
    /* 全ページを使用中にマーク */
    memset(g_bitmap, 0xFF, bitmap_bytes);
    
    g_free_pages = 0;
    g_used_pages = g_total_pages;
    
    /* 使用可能な領域を解放 */
    for (int r = 0; r < g_n_regions; r++) {
        if (g_regions[r].type != MB2_MMAP_AVAILABLE) continue;
        
        uint64_t start = ALIGN_UP(g_regions[r].base, PAGE_SIZE);
        uint64_t end   = ALIGN_DOWN(g_regions[r].base + g_regions[r].size, PAGE_SIZE);
        
        for (uint64_t p = start; p < end; p += PAGE_SIZE) {
            pfn_t pfn = p / PAGE_SIZE;
            if (pfn < g_total_pages && bitmap_test(pfn)) {
                bitmap_clear(pfn);
                g_free_pages++;
                g_used_pages--;
            }
        }
    }
    
    /* 予約済み領域を再度マーク */
    
    /* 1. ゼロページ (NULL参照対策) */
    pmm_reserve_region(0, PAGE_SIZE);
    
    /* 2. BIOS/低メモリ (0 - 1MB) */
    pmm_reserve_region(0, 0x100000);
    
    /* 3. カーネルイメージ */
    uintptr_t kern_start_phys = VIRT_TO_PHYS((uintptr_t)_kernel_start);
    pmm_reserve_region(kern_start_phys,
                       kern_end_phys - kern_start_phys);
    
    /* 4. ビットマップ自身 */
    pmm_reserve_region(bitmap_phys, bitmap_bytes);
    
    /* 5. multiboot2 情報 */
    uintptr_t mbi_phys = VIRT_TO_PHYS((uintptr_t)mbi);
    pmm_reserve_region(mbi_phys, mbi->total_size);
    
    /* 6. initrd モジュール */
    struct mb2_tag_module *mod =
        (struct mb2_tag_module *)mb2_find_tag(mbi, MB2_TAG_MODULE);
    if (mod) {
        pmm_reserve_region(mod->mod_start,
                           mod->mod_end - mod->mod_start);
    }
    
    printk(KERN_INFO "PMM: initialized: total=%zu free=%zu used=%zu (pages)\n",
           g_total_pages, g_free_pages, g_used_pages);
    printk(KERN_INFO "PMM: total memory: %zu MB\n",
           (g_total_pages * PAGE_SIZE) >> 20);
    printk(KERN_INFO "PMM: bitmap at 0x%p (%zu KB)\n",
           g_bitmap, bitmap_bytes >> 10);
}

/* ============================================================
 * 予約 / 解放
 * ============================================================ */
void pmm_reserve_region(uint64_t base, uint64_t size)
{
    uint64_t start = ALIGN_DOWN(base, PAGE_SIZE);
    uint64_t end   = ALIGN_UP(base + size, PAGE_SIZE);
    
    for (uint64_t p = start; p < end; p += PAGE_SIZE) {
        pfn_t pfn = p / PAGE_SIZE;
        if (pfn < g_total_pages && !bitmap_test(pfn)) {
            bitmap_set(pfn);
            g_free_pages--;
            g_used_pages++;
        }
    }
}

void pmm_add_region(uint64_t base, uint64_t size)
{
    uint64_t start = ALIGN_UP(base, PAGE_SIZE);
    uint64_t end   = ALIGN_DOWN(base + size, PAGE_SIZE);
    
    spinlock_lock(&g_pmm_lock);
    for (uint64_t p = start; p < end; p += PAGE_SIZE) {
        pfn_t pfn = p / PAGE_SIZE;
        if (pfn < g_total_pages && bitmap_test(pfn)) {
            bitmap_clear(pfn);
            g_free_pages++;
            g_used_pages--;
        }
    }
    spinlock_unlock(&g_pmm_lock);
}

/* ============================================================
 * ページ割り当て
 * ============================================================ */

/* 高速な次の空きページ探索 (ヒント付き) */
static size_t g_alloc_hint = 0;  /* 次の探索開始位置 */

uintptr_t pmm_alloc(void)
{
    spinlock_lock(&g_pmm_lock);
    
    if (g_free_pages == 0) {
        spinlock_unlock(&g_pmm_lock);
        return 0;  /* Out of memory */
    }
    
    size_t words = (g_total_pages + 63) / 64;
    size_t start_word = g_alloc_hint / 64;
    
    /* 2パス: ヒントから末尾、次に先頭から */
    for (int pass = 0; pass < 2; pass++) {
        size_t from = pass ? 0 : start_word;
        size_t to   = pass ? start_word : words;
        
        for (size_t w = from; w < to; w++) {
            if (g_bitmap[w] == UINT64_MAX) continue;  /* 全使用 */
            
            /* 空きビットを探す */
            uint64_t inv = ~g_bitmap[w];
            int bit = __builtin_ctzll(inv);
            pfn_t pfn = (pfn_t)(w * 64 + bit);
            
            if (pfn >= g_total_pages) continue;
            
            bitmap_set(pfn);
            g_free_pages--;
            g_used_pages++;
            g_alloc_hint = pfn + 1;
            if (g_alloc_hint >= g_total_pages) g_alloc_hint = 0;
            
            spinlock_unlock(&g_pmm_lock);
            
            uintptr_t phys = pfn * PAGE_SIZE;
            /* ゼロクリア */
            memset(PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
            return phys;
        }
    }
    
    spinlock_unlock(&g_pmm_lock);
    return 0;
}

uintptr_t pmm_alloc_zone(zone_id_t zone)
{
    uint64_t max_phys;
    switch (zone) {
    case ZONE_LOW:    max_phys = PMM_ZONE_LOW;    break;
    case ZONE_DMA:    max_phys = PMM_ZONE_DMA;    break;
    case ZONE_NORMAL: max_phys = PMM_ZONE_NORMAL; break;
    default:          return pmm_alloc();
    }
    
    spinlock_lock(&g_pmm_lock);
    size_t max_pfn = max_phys / PAGE_SIZE;
    if (max_pfn > g_total_pages) max_pfn = g_total_pages;
    
    for (size_t pfn = 1; pfn < max_pfn; pfn++) {
        if (!bitmap_test(pfn)) {
            bitmap_set(pfn);
            g_free_pages--;
            g_used_pages++;
            spinlock_unlock(&g_pmm_lock);
            
            uintptr_t phys = (uintptr_t)pfn * PAGE_SIZE;
            memset(PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
            return phys;
        }
    }
    
    spinlock_unlock(&g_pmm_lock);
    return 0;
}

uintptr_t pmm_alloc_n(size_t n)
{
    if (n == 0) return 0;
    if (n == 1) return pmm_alloc();
    
    spinlock_lock(&g_pmm_lock);
    
    if (g_free_pages < n) {
        spinlock_unlock(&g_pmm_lock);
        return 0;
    }
    
    /* 連続したn個の空きページを探す */
    size_t count = 0;
    pfn_t  start = 0;
    
    for (pfn_t pfn = 1; pfn < g_total_pages; pfn++) {
        if (!bitmap_test(pfn)) {
            if (count == 0) start = pfn;
            count++;
            if (count == n) {
                /* 確保 */
                for (pfn_t p = start; p < start + n; p++) {
                    bitmap_set(p);
                    g_free_pages--;
                    g_used_pages++;
                }
                spinlock_unlock(&g_pmm_lock);
                
                uintptr_t phys = start * PAGE_SIZE;
                memset(PHYS_TO_VIRT(phys), 0, n * PAGE_SIZE);
                return phys;
            }
        } else {
            count = 0;
        }
    }
    
    spinlock_unlock(&g_pmm_lock);
    return 0;
}

void pmm_free(uintptr_t phys)
{
    if (!phys || !IS_ALIGNED(phys, PAGE_SIZE)) return;
    
    pfn_t pfn = phys / PAGE_SIZE;
    if (pfn == 0 || pfn >= g_total_pages) return;
    
    spinlock_lock(&g_pmm_lock);
    
    if (!bitmap_test(pfn)) {
        /* 二重解放 - パニック */
        spinlock_unlock(&g_pmm_lock);
        PANIC("PMM: double free at phys=0x%llx pfn=%llu",
              (unsigned long long)phys, (unsigned long long)pfn);
        return;
    }
    
    bitmap_clear(pfn);
    g_free_pages++;
    g_used_pages--;
    
    /* ヒントを更新 (頻繁に解放→割り当てする場合に効果的) */
    if (pfn < g_alloc_hint) g_alloc_hint = pfn;
    
    spinlock_unlock(&g_pmm_lock);
}

void pmm_free_n(uintptr_t phys, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        pmm_free(phys + i * PAGE_SIZE);
    }
}

/* ============================================================
 * 統計
 * ============================================================ */
size_t pmm_total_pages(void) { return g_total_pages; }
size_t pmm_free_pages(void)  { return g_free_pages;  }
size_t pmm_used_pages(void)  { return g_used_pages;  }

/* ============================================================
 * デバッグ
 * ============================================================ */
void pmm_dump(void)
{
    printk(KERN_DEBUG "PMM status:\n");
    printk(KERN_DEBUG "  Total: %zu pages (%zu MB)\n",
           g_total_pages, (g_total_pages * PAGE_SIZE) >> 20);
    printk(KERN_DEBUG "  Free:  %zu pages (%zu MB)\n",
           g_free_pages, (g_free_pages * PAGE_SIZE) >> 20);
    printk(KERN_DEBUG "  Used:  %zu pages (%zu MB)\n",
           g_used_pages, (g_used_pages * PAGE_SIZE) >> 20);
}
