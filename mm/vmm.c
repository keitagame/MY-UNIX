/* ============================================================
 * vmm.c - 仮想メモリマネージャ
 * x86_64 4レベルページング
 * ============================================================ */

#include "../include/types.h"
#include "../include/kernel.h"
#include "../include/mm.h"

/* ============================================================
 * ページウォーク ヘルパー
 * ============================================================ */

/* 仮想アドレスからインデックス抽出 */
#define VA_PML4_IDX(va)   (((va) >> 39) & 0x1FF)
#define VA_PDPT_IDX(va)   (((va) >> 30) & 0x1FF)
#define VA_PD_IDX(va)     (((va) >> 21) & 0x1FF)
#define VA_PT_IDX(va)     (((va) >> 12) & 0x1FF)
#define VA_OFFSET(va)     ((va) & 0xFFF)

/* PTE から物理アドレス取得 */
#define PTE_ADDR(pte)     ((pte) & 0x000FFFFFFFFFF000ULL)

/* カーネルのPML4 */
static pml4_t  *g_kernel_pml4 = NULL;
static uintptr_t g_kernel_pml4_phys = 0;

static vmm_space_t g_kernel_space;

/* ============================================================
 * ページテーブル要素の物理→仮想変換
 * ============================================================ */
static ALWAYS_INLINE void *pte_to_virt(pte_t pte)
{
    if (!(pte & PTE_PRESENT)) return NULL;
    return phys_to_kvirt(PTE_ADDR(pte));
}

/* ============================================================
 * PTEを取得/作成
 * ============================================================ */

/* level: 4=PML4, 3=PDPT, 2=PD, 1=PT */
static pte_t *vmm_get_pte(pml4_t *pml4, uintptr_t virt, bool create, uint64_t flags)
{
    pml4e_t *pml4e = &pml4->entries[VA_PML4_IDX(virt)];
    
    pdpt_t *pdpt;
    if (!(*pml4e & PTE_PRESENT)) {
        if (!create) return NULL;
        uintptr_t phys = pmm_alloc();
        if (!phys) return NULL;
        *pml4e = phys | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
        pdpt = (pdpt_t *)phys_to_kvirt(phys);
    } else {
        pdpt = (pdpt_t *)pte_to_virt(*pml4e);
    }
    
    pdpte_t *pdpte = &pdpt->entries[VA_PDPT_IDX(virt)];
    
    pd_t *pd;
    if (!(*pdpte & PTE_PRESENT)) {
        if (!create) return NULL;
        uintptr_t phys = pmm_alloc();
        if (!phys) return NULL;
        *pdpte = phys | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
        pd = (pd_t *)phys_to_kvirt(phys);
    } else {
        pd = (pd_t *)pte_to_virt(*pdpte);
    }
    
    pde_t *pde = &pd->entries[VA_PD_IDX(virt)];
    
    pt_t *pt;
    if (!(*pde & PTE_PRESENT)) {
        if (!create) return NULL;
        uintptr_t phys = pmm_alloc();
        if (!phys) return NULL;
        *pde = phys | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
        pt = (pt_t *)phys_to_kvirt(phys);
    } else {
        if (*pde & PTE_HUGE) {
            /* 2MB ページは処理しない (今回は4KBのみ) */
            return NULL;
        }
        pt = (pt_t *)pte_to_virt(*pde);
    }
    
    return &pt->entries[VA_PT_IDX(virt)];
}

/* ============================================================
 * カーネル初期ページングセットアップ
 * ============================================================ */
void vmm_init(void)
{
    /* 現在のCR3がブートPML4 */
    g_kernel_pml4_phys = read_cr3() & PAGE_MASK;
    g_kernel_pml4 = (pml4_t *)phys_to_kvirt(g_kernel_pml4_phys);
    
    /* カーネル空間初期化 */
    memset(&g_kernel_space, 0, sizeof(g_kernel_space));
    g_kernel_space.pml4      = g_kernel_pml4;
    g_kernel_space.pml4_phys = g_kernel_pml4_phys;
    spinlock_init(&g_kernel_space.lock);
    atomic_set(&g_kernel_space.refcount, 1);
    INIT_LIST_HEAD(&g_kernel_space.vma_list);
    
    /* 物理メモリ全体をカーネル空間にマップ
     * PHYS_MAP_BASE + phys → phys の直接マップ */
    uint64_t total_phys = pmm_total_pages() * PAGE_SIZE;
    
    /* 2MBページを使った大容量マッピング */
    vmm_map_phys_huge(g_kernel_pml4, PHYS_MAP_BASE,
                      0, total_phys, VMM_KERN_RW);
    
    printk(KERN_INFO "VMM: initialized, kernel PML4 at 0x%p (phys=0x%llx)\n",
           g_kernel_pml4, (unsigned long long)g_kernel_pml4_phys);
    printk(KERN_INFO "VMM: direct-mapped %llu MB at 0x%016llx\n",
           (unsigned long long)(total_phys >> 20),
           (unsigned long long)PHYS_MAP_BASE);
}

/* 2MB ページでの大容量マッピング */
void vmm_map_phys_huge(pml4_t *pml4, uintptr_t virt_base,
                        uintptr_t phys_base, uint64_t size, uint64_t flags)
{
    /* 2MBページアライン */
    uint64_t huge_size = 2ULL * 1024 * 1024;
    uintptr_t virt = ALIGN_DOWN(virt_base, huge_size);
    uintptr_t phys = ALIGN_DOWN(phys_base, huge_size);
    uint64_t  len  = ALIGN_UP(size, huge_size);
    
    while (len > 0) {
        /* PML4 */
        pml4e_t *pml4e = &pml4->entries[VA_PML4_IDX(virt)];
        pdpt_t *pdpt;
        if (!(*pml4e & PTE_PRESENT)) {
            uintptr_t p = pmm_alloc();
            if (!p) break;
            *pml4e = p | PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL;
            pdpt = (pdpt_t *)phys_to_kvirt(p);
        } else {
            pdpt = (pdpt_t *)pte_to_virt(*pml4e);
        }
        
        /* PDPT */
        pdpte_t *pdpte = &pdpt->entries[VA_PDPT_IDX(virt)];
        pd_t *pd;
        if (!(*pdpte & PTE_PRESENT)) {
            uintptr_t p = pmm_alloc();
            if (!p) break;
            *pdpte = p | PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL;
            pd = (pd_t *)phys_to_kvirt(p);
        } else {
            pd = (pd_t *)pte_to_virt(*pdpte);
        }
        
        /* PD (2MB page) */
        pde_t *pde = &pd->entries[VA_PD_IDX(virt)];
        *pde = phys | PTE_HUGE | flags;
        
        virt += huge_size;
        phys += huge_size;
        len  -= huge_size;
    }
}

/* ============================================================
 * アドレス空間作成/複製/破棄
 * ============================================================ */
vmm_space_t *vmm_create_space(void)
{
    vmm_space_t *space = kzalloc(sizeof(vmm_space_t));
    if (!space) return NULL;
    
    /* PML4 割り当て */
    uintptr_t phys = pmm_alloc();
    if (!phys) {
        kfree(space);
        return NULL;
    }
    
    space->pml4_phys = phys;
    space->pml4      = (pml4_t *)phys_to_kvirt(phys);
    
    /* カーネル空間部分をコピー (PML4上位エントリ) */
    /* 通常: インデックス256以降がカーネル空間 */
    memcpy(space->pml4->entries + 256,
           g_kernel_pml4->entries + 256,
           256 * sizeof(pml4e_t));
    
    spinlock_init(&space->lock);
    atomic_set(&space->refcount, 1);
    INIT_LIST_HEAD(&space->vma_list);
    
    return space;
}

vmm_space_t *vmm_clone_space(vmm_space_t *src)
{
    vmm_space_t *dst = vmm_create_space();
    if (!dst) return NULL;
    
    spinlock_lock(&src->lock);
    
    /* ユーザー空間のVMAを複製 */
    struct list_head *pos;
    list_for_each(pos, &src->vma_list) {
        vma_t *vma = list_entry(pos, vma_t, list);
        
        /* 各VMAページをCopy-on-Write的にコピー */
        uintptr_t addr = vma->start;
        while (addr < vma->end) {
            uintptr_t src_phys = vmm_virt_to_phys(src, addr);
            if (src_phys) {
                /* 新しいページを割り当てて内容をコピー */
                uintptr_t dst_phys = pmm_alloc();
                if (dst_phys) {
                    memcpy(phys_to_kvirt(dst_phys),
                           phys_to_kvirt(src_phys), PAGE_SIZE);
                    vmm_map(dst, addr, dst_phys, vma->flags);
                }
            }
            addr += PAGE_SIZE;
        }
        
        /* VMA構造体をコピー */
        vma_t *new_vma = kmalloc(sizeof(vma_t));
        if (new_vma) {
            *new_vma = *vma;
            INIT_LIST_HEAD(&new_vma->list);
            atomic_set(&new_vma->refcount, 1);
            list_add_tail(&new_vma->list, &dst->vma_list);
            dst->n_vmas++;
        }
    }
    
    dst->n_pages = dst->n_pages; /* 更新済み */
    
    spinlock_unlock(&src->lock);
    return dst;
}

void vmm_destroy_space(vmm_space_t *space)
{
    if (!space) return;
    if (!atomic_dec_and_test(&space->refcount)) return;
    
    /* 全VMAを解放 */
    struct list_head *pos, *n;
    list_for_each_safe(pos, n, &space->vma_list) {
        vma_t *vma = list_entry(pos, vma_t, list);
        
        /* マップされているページを解放 */
        uintptr_t addr = vma->start;
        while (addr < vma->end) {
            uintptr_t phys = vmm_virt_to_phys(space, addr);
            if (phys) {
                vmm_unmap(space, addr);
                pmm_free(phys);
            }
            addr += PAGE_SIZE;
        }
        
        list_del(&vma->list);
        kfree(vma);
    }
    
    /* ページテーブル構造体を解放 */
    /* PML4のユーザー領域エントリをウォーク */
    for (int i4 = 0; i4 < 256; i4++) {
        pml4e_t e4 = space->pml4->entries[i4];
        if (!(e4 & PTE_PRESENT)) continue;
        pdpt_t *pdpt = (pdpt_t *)pte_to_virt(e4);
        
        for (int i3 = 0; i3 < 512; i3++) {
            pdpte_t e3 = pdpt->entries[i3];
            if (!(e3 & PTE_PRESENT) || (e3 & PTE_HUGE)) continue;
            pd_t *pd = (pd_t *)pte_to_virt(e3);
            
            for (int i2 = 0; i2 < 512; i2++) {
                pde_t e2 = pd->entries[i2];
                if (!(e2 & PTE_PRESENT) || (e2 & PTE_HUGE)) continue;
                pmm_free(PTE_ADDR(e2));  /* PT解放 */
            }
            pmm_free(PTE_ADDR(e3));  /* PD解放 */
        }
        pmm_free(PTE_ADDR(e4));  /* PDPT解放 */
    }
    
    pmm_free(space->pml4_phys);
    kfree(space);
}

void vmm_switch_space(vmm_space_t *space)
{
    write_cr3(space->pml4_phys);
}

vmm_space_t *vmm_kernel_space(void)
{
    return &g_kernel_space;
}

/* ============================================================
 * ページマッピング
 * ============================================================ */
int vmm_map(vmm_space_t *space, uintptr_t virt, uintptr_t phys, uint64_t flags)
{
    if (!IS_ALIGNED(virt, PAGE_SIZE) || !IS_ALIGNED(phys, PAGE_SIZE))
        return -EINVAL;
    
    spinlock_lock(&space->lock);
    
    pte_t *pte = vmm_get_pte(space->pml4, virt, true, flags);
    if (!pte) {
        spinlock_unlock(&space->lock);
        return -ENOMEM;
    }
    
    *pte = phys | flags;
    invlpg(virt);
    space->n_pages++;
    
    spinlock_unlock(&space->lock);
    return 0;
}

int vmm_map_range(vmm_space_t *space, uintptr_t virt, uintptr_t phys,
                  size_t n_pages, uint64_t flags)
{
    for (size_t i = 0; i < n_pages; i++) {
        int r = vmm_map(space, virt + i * PAGE_SIZE,
                        phys + i * PAGE_SIZE, flags);
        if (r < 0) return r;
    }
    return 0;
}

int vmm_unmap(vmm_space_t *space, uintptr_t virt)
{
    spinlock_lock(&space->lock);
    
    pte_t *pte = vmm_get_pte(space->pml4, virt, false, 0);
    if (pte && (*pte & PTE_PRESENT)) {
        *pte = 0;
        invlpg(virt);
        if (space->n_pages > 0) space->n_pages--;
    }
    
    spinlock_unlock(&space->lock);
    return 0;
}

int vmm_unmap_range(vmm_space_t *space, uintptr_t virt, size_t n_pages)
{
    for (size_t i = 0; i < n_pages; i++) {
        vmm_unmap(space, virt + i * PAGE_SIZE);
    }
    return 0;
}

/* ============================================================
 * アドレス解決
 * ============================================================ */
uintptr_t vmm_virt_to_phys(vmm_space_t *space, uintptr_t virt)
{
    pml4_t *pml4 = space->pml4;
    
    pml4e_t e4 = pml4->entries[VA_PML4_IDX(virt)];
    if (!(e4 & PTE_PRESENT)) return 0;
    
    pdpt_t  *pdpt = (pdpt_t *)pte_to_virt(e4);
    pdpte_t  e3   = pdpt->entries[VA_PDPT_IDX(virt)];
    if (!(e3 & PTE_PRESENT)) return 0;
    if (e3 & PTE_HUGE) return PTE_ADDR(e3) + (virt & (1ULL*1024*1024*1024 - 1));
    
    pd_t *pd = (pd_t *)pte_to_virt(e3);
    pde_t e2 = pd->entries[VA_PD_IDX(virt)];
    if (!(e2 & PTE_PRESENT)) return 0;
    if (e2 & PTE_HUGE) return PTE_ADDR(e2) + (virt & (2*1024*1024 - 1));
    
    pt_t *pt = (pt_t *)pte_to_virt(e2);
    pte_t e1 = pt->entries[VA_PT_IDX(virt)];
    if (!(e1 & PTE_PRESENT)) return 0;
    
    return PTE_ADDR(e1) + VA_OFFSET(virt);
}

bool vmm_is_mapped(vmm_space_t *space, uintptr_t virt)
{
    return vmm_virt_to_phys(space, virt) != 0;
}

/* ============================================================
 * ユーザー空間割り当て
 * ============================================================ */
uintptr_t vmm_alloc_user(vmm_space_t *space, uintptr_t hint,
                          size_t size, uint64_t flags)
{
    if (hint == 0) hint = VMM_USER_START;
    hint = ALIGN_UP(hint, PAGE_SIZE);
    size = ALIGN_UP(size, PAGE_SIZE);
    size_t n_pages = size / PAGE_SIZE;
    
    /* hint付きで空き仮想アドレスを探す */
    uintptr_t virt = hint;
    while (virt + size <= VMM_USER_END) {
        /* このアドレスに空きがあるか確認 */
        bool ok = true;
        for (size_t i = 0; i < n_pages; i++) {
            if (vmm_is_mapped(space, virt + i * PAGE_SIZE)) {
                virt = virt + (i + 1) * PAGE_SIZE;
                ok = false;
                break;
            }
        }
        if (!ok) continue;
        
        /* 物理ページを割り当てマップ */
        for (size_t i = 0; i < n_pages; i++) {
            uintptr_t phys = pmm_alloc();
            if (!phys) {
                /* ロールバック */
                for (size_t j = 0; j < i; j++) {
                    uintptr_t p = vmm_virt_to_phys(space, virt + j * PAGE_SIZE);
                    vmm_unmap(space, virt + j * PAGE_SIZE);
                    if (p) pmm_free(p);
                }
                return 0;
            }
            if (vmm_map(space, virt + i * PAGE_SIZE, phys, flags) < 0) {
                pmm_free(phys);
                /* ロールバック */
                for (size_t j = 0; j < i; j++) {
                    uintptr_t p = vmm_virt_to_phys(space, virt + j * PAGE_SIZE);
                    vmm_unmap(space, virt + j * PAGE_SIZE);
                    if (p) pmm_free(p);
                }
                return 0;
            }
        }
        return virt;
    }
    
    return 0;  /* OOM */
}

int vmm_free_user(vmm_space_t *space, uintptr_t virt, size_t size)
{
    virt = ALIGN_DOWN(virt, PAGE_SIZE);
    size = ALIGN_UP(size, PAGE_SIZE);
    size_t n_pages = size / PAGE_SIZE;
    
    for (size_t i = 0; i < n_pages; i++) {
        uintptr_t addr = virt + i * PAGE_SIZE;
        uintptr_t phys = vmm_virt_to_phys(space, addr);
        vmm_unmap(space, addr);
        if (phys) pmm_free(phys);
    }
    return 0;
}

int vmm_protect(vmm_space_t *space, uintptr_t virt, size_t size, uint32_t prot)
{
    virt = ALIGN_DOWN(virt, PAGE_SIZE);
    size = ALIGN_UP(size, PAGE_SIZE);
    
    for (uintptr_t addr = virt; addr < virt + size; addr += PAGE_SIZE) {
        spinlock_lock(&space->lock);
        pte_t *pte = vmm_get_pte(space->pml4, addr, false, 0);
        if (pte && (*pte & PTE_PRESENT)) {
            uintptr_t phys = PTE_ADDR(*pte);
            uint64_t flags = PTE_PRESENT | PTE_USER;
            if (prot & PROT_WRITE) flags |= PTE_WRITABLE;
            if (!(prot & PROT_EXEC)) flags |= PTE_NX;
            *pte = phys | flags;
            invlpg(addr);
        }
        spinlock_unlock(&space->lock);
    }
    return 0;
}

/* ============================================================
 * VMA 管理
 * ============================================================ */
vma_t *vmm_find_vma(vmm_space_t *space, uintptr_t addr)
{
    struct list_head *pos;
    list_for_each(pos, &space->vma_list) {
        vma_t *vma = list_entry(pos, vma_t, list);
        if (addr >= vma->start && addr < vma->end)
            return vma;
    }
    return NULL;
}

vma_t *vmm_create_vma(vmm_space_t *space, uintptr_t start, size_t size,
                       uint32_t prot, uint32_t map_flags)
{
    vma_t *vma = kzalloc(sizeof(vma_t));
    if (!vma) return NULL;
    
    vma->start = ALIGN_DOWN(start, PAGE_SIZE);
    vma->end   = ALIGN_UP(start + size, PAGE_SIZE);
    vma->prot  = prot;
    vma->mmap_flags = map_flags;
    vma->fd    = -1;
    atomic_set(&vma->refcount, 1);
    INIT_LIST_HEAD(&vma->list);
    
    /* フラグ変換 */
    vma->flags = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE) vma->flags |= PTE_WRITABLE;
    if (!(prot & PROT_EXEC)) vma->flags |= PTE_NX;
    
    spinlock_lock(&space->lock);
    list_add_tail(&vma->list, &space->vma_list);
    space->n_vmas++;
    spinlock_unlock(&space->lock);
    
    return vma;
}

void vmm_destroy_vma(vmm_space_t *space, vma_t *vma)
{
    spinlock_lock(&space->lock);
    list_del(&vma->list);
    space->n_vmas--;
    spinlock_unlock(&space->lock);
    kfree(vma);
}

/* ============================================================
 * カーネルページマッピング
 * ============================================================ */
void vmm_kmap_phys(uintptr_t virt, uintptr_t phys, size_t size, uint64_t flags)
{
    size_t n = ALIGN_UP(size, PAGE_SIZE) / PAGE_SIZE;
    for (size_t i = 0; i < n; i++) {
        pte_t *pte = vmm_get_pte(g_kernel_pml4, virt + i * PAGE_SIZE, true, flags);
        if (pte) {
            *pte = (phys + i * PAGE_SIZE) | flags;
            invlpg(virt + i * PAGE_SIZE);
        }
    }
}

/* ユーザーアクセス安全操作 */
bool access_ok(const void *addr, size_t size)
{
    uintptr_t start = (uintptr_t)addr;
    if (start + size < start) return false;  /* オーバーフロー */
    return start + size <= VMM_USER_END;
}

int copy_from_user(void *dst, const void *user_src, size_t len)
{
    if (!access_ok(user_src, len)) return -EFAULT;
    /* TODO: 例外ハンドリング付きコピー */
    memcpy(dst, user_src, len);
    return 0;
}

int copy_to_user(void *user_dst, const void *src, size_t len)
{
    if (!access_ok(user_dst, len)) return -EFAULT;
    memcpy(user_dst, src, len);
    return 0;
}

ssize_t strncpy_from_user(char *dst, const char *user_src, size_t maxlen)
{
    if (!access_ok(user_src, 1)) return -EFAULT;
    size_t i;
    for (i = 0; i < maxlen; i++) {
        dst[i] = user_src[i];
        if (dst[i] == '\0') return (ssize_t)i;
    }
    return -E2BIG;
}
