/* ============================================================
 * gdt.c - グローバルディスクリプタテーブル + TSS
 * x86_64 セグメント設定
 * ============================================================ */

#include "../../include/types.h"
#include "../../include/kernel.h"

/* ============================================================
 * GDT エントリ
 * ============================================================ */
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} PACKED gdt_entry_t;

/* TSS ディスクリプタは 16バイト (システムセグメント) */
typedef struct {
    uint16_t length;
    uint16_t base_low;
    uint8_t  base_mid1;
    uint8_t  flags1;
    uint8_t  flags2;
    uint8_t  base_mid2;
    uint32_t base_high;
    uint32_t reserved;
} PACKED tss_desc_t;

/* GDT ポインタ */
typedef struct {
    uint16_t  limit;
    uint64_t  base;
} PACKED gdt_ptr_t;

/* ============================================================
 * TSS (Task State Segment)
 * ============================================================ */
typedef struct {
    uint32_t reserved0;
    uint64_t rsp[3];      /* RSP0, RSP1, RSP2 */
    uint64_t reserved1;
    uint64_t ist[7];      /* IST1〜IST7 */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
    uint8_t  iopb[8192];  /* I/Oポートマップ (8192バイト) */
} PACKED tss_t;

/* ============================================================
 * GDT セグメントセレクタ
 * ============================================================ */
#define GDT_NULL_SEL    0x00  /* NULL */
#define GDT_KERN_CODE   0x08  /* カーネルコード 64bit */
#define GDT_KERN_DATA   0x10  /* カーネルデータ */
#define GDT_USER_CODE32 0x18  /* ユーザーコード 32bit (互換) */
#define GDT_USER_DATA   0x20  /* ユーザーデータ */
#define GDT_USER_CODE64 0x28  /* ユーザーコード 64bit */
#define GDT_TSS_SEL     0x30  /* TSS (16バイト = 2エントリ) */
#define GDT_TLS_SEL     0x40  /* スレッドローカルストレージ */

/* GDT エントリ数 */
#define GDT_ENTRIES  10

/* ============================================================
 * アクセス/粒度フラグ
 * ============================================================ */
/* access byte */
#define GDT_ACC_PRESENT     0x80
#define GDT_ACC_PRIV_RING0  0x00
#define GDT_ACC_PRIV_RING3  0x60
#define GDT_ACC_TYPE_SYS    0x00
#define GDT_ACC_TYPE_CODE   0x18
#define GDT_ACC_TYPE_DATA   0x10
#define GDT_ACC_EXEC        0x08
#define GDT_ACC_DC          0x04  /* 方向/適合 */
#define GDT_ACC_RW          0x02  /* 読み書き可 */
#define GDT_ACC_ACCESSED    0x01
/* granularity byte (上4bit) */
#define GDT_GRAN_4K         0x80  /* 4KB粒度 */
#define GDT_GRAN_32BIT      0x40  /* 32bitモード */
#define GDT_GRAN_64BIT      0x20  /* 64bitモード (Lビット) */
/* システムセグメントタイプ */
#define GDT_SYS_TSS         0x09  /* Available 64bit TSS */
#define GDT_SYS_TSS_BUSY    0x0B  /* Busy 64bit TSS */

/* ============================================================
 * GDT 実データ
 * ============================================================ */
static gdt_entry_t g_gdt[GDT_ENTRIES] __attribute__((aligned(8)));
static tss_desc_t  g_tss_desc          __attribute__((aligned(8)));
static tss_t       g_tss               __attribute__((aligned(4096)));
static gdt_ptr_t   g_gdt_ptr;

/* IST スタック (ダブルフォルト/NMI用) */
#define IST_STACK_SIZE 4096
static uint8_t g_ist_stack[7][IST_STACK_SIZE] __attribute__((aligned(16)));

/* ============================================================
 * ヘルパー: GDTエントリ設定
 * ============================================================ */
static void gdt_set_entry(int idx,
                           uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t gran)
{
    g_gdt[idx].base_low  = (uint16_t)(base & 0xFFFF);
    g_gdt[idx].base_mid  = (uint8_t)((base >> 16) & 0xFF);
    g_gdt[idx].base_high = (uint8_t)((base >> 24) & 0xFF);
    g_gdt[idx].limit_low = (uint16_t)(limit & 0xFFFF);
    g_gdt[idx].access    = access;
    g_gdt[idx].granularity = (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));
}

/* ============================================================
 * TSS ディスクリプタ設定 (16バイト拡張エントリ)
 * ============================================================ */
static void gdt_set_tss_desc(uint64_t base, uint32_t limit)
{
    g_tss_desc.length    = (uint16_t)(limit & 0xFFFF);
    g_tss_desc.base_low  = (uint16_t)(base & 0xFFFF);
    g_tss_desc.base_mid1 = (uint8_t)((base >> 16) & 0xFF);
    g_tss_desc.flags1    = GDT_ACC_PRESENT | GDT_SYS_TSS;
    g_tss_desc.flags2    = (uint8_t)(((limit >> 16) & 0x0F));
    g_tss_desc.base_mid2 = (uint8_t)((base >> 24) & 0xFF);
    g_tss_desc.base_high = (uint32_t)(base >> 32);
    g_tss_desc.reserved  = 0;
}

/* ============================================================
 * TSS 初期化
 * ============================================================ */
static void tss_init(void)
{
    memset(&g_tss, 0, sizeof(g_tss));

    /* カーネルスタックポインタ (Ring0) - 後でプロセス切り替え時に更新 */
    /* ここではブートスタックを仮設定 */
    extern uint8_t boot_stack_top[];
    g_tss.rsp[0] = (uint64_t)boot_stack_top;

    /* IST スタック設定 */
    for (int i = 0; i < 7; i++) {
        g_tss.ist[i] = (uint64_t)(g_ist_stack[i] + IST_STACK_SIZE);
    }

    /* I/Oポートマップは全禁止 (0xFF) */
    g_tss.iopb_offset = (uint16_t)offsetof(tss_t, iopb);
    memset(g_tss.iopb, 0xFF, sizeof(g_tss.iopb));
}

/* ============================================================
 * GDT 初期化
 * ============================================================ */
void gdt_init(void)
{
    /* 0: NULL ディスクリプタ */
    gdt_set_entry(0, 0, 0, 0, 0);

    /* 1: カーネルコード 64bit (0x08)
     * Lビット=1, D=0, G=1, P=1, DPL=0, Type=Code/Readable */
    gdt_set_entry(1, 0, 0xFFFFF,
                  GDT_ACC_PRESENT | GDT_ACC_PRIV_RING0 | GDT_ACC_TYPE_CODE | GDT_ACC_RW,
                  GDT_GRAN_4K | GDT_GRAN_64BIT);

    /* 2: カーネルデータ (0x10)
     * 64bitモードではほぼ無効だが設定する */
    gdt_set_entry(2, 0, 0xFFFFF,
                  GDT_ACC_PRESENT | GDT_ACC_PRIV_RING0 | GDT_ACC_TYPE_DATA | GDT_ACC_RW,
                  GDT_GRAN_4K | GDT_GRAN_32BIT);

    /* 3: ユーザーコード 32bit 互換モード (0x18) */
    gdt_set_entry(3, 0, 0xFFFFF,
                  GDT_ACC_PRESENT | GDT_ACC_PRIV_RING3 | GDT_ACC_TYPE_CODE | GDT_ACC_RW,
                  GDT_GRAN_4K | GDT_GRAN_32BIT);

    /* 4: ユーザーデータ (0x20) */
    gdt_set_entry(4, 0, 0xFFFFF,
                  GDT_ACC_PRESENT | GDT_ACC_PRIV_RING3 | GDT_ACC_TYPE_DATA | GDT_ACC_RW,
                  GDT_GRAN_4K | GDT_GRAN_32BIT);

    /* 5: ユーザーコード 64bit (0x28) */
    gdt_set_entry(5, 0, 0xFFFFF,
                  GDT_ACC_PRESENT | GDT_ACC_PRIV_RING3 | GDT_ACC_TYPE_CODE | GDT_ACC_RW,
                  GDT_GRAN_4K | GDT_GRAN_64BIT);

    /* 6〜7: TSS (0x30) - 16バイトエントリ */
    tss_init();
    gdt_set_tss_desc((uint64_t)&g_tss, sizeof(g_tss) - 1);
    /* GDTの6番目にTSSディスクリプタ (128bit) を配置 */
    memcpy(&g_gdt[6], &g_tss_desc, sizeof(g_tss_desc));

    /* 8: TLS セグメント (0x40) - FS/GSベース用ダミー */
    gdt_set_entry(8, 0, 0xFFFFF,
                  GDT_ACC_PRESENT | GDT_ACC_PRIV_RING3 | GDT_ACC_TYPE_DATA | GDT_ACC_RW,
                  GDT_GRAN_4K | GDT_GRAN_32BIT);

    /* 9: 予備 */
    gdt_set_entry(9, 0, 0, 0, 0);

    /* GDT ポインタ設定 */
    g_gdt_ptr.limit = (uint16_t)(sizeof(g_gdt) + sizeof(g_tss_desc) - 1);
    g_gdt_ptr.base  = (uint64_t)g_gdt;

    /* GDT ロード */
    extern void gdt_flush(gdt_ptr_t *ptr);
    gdt_flush(&g_gdt_ptr);

    /* TSS ロード */
    extern void tss_flush(uint16_t sel);
    tss_flush(GDT_TSS_SEL);

    printk(KERN_INFO "GDT: initialized (%d entries, TSS at 0x%p)\n",
           GDT_ENTRIES, &g_tss);
}

/* ============================================================
 * TSS RSP0 更新 (プロセス切り替え時)
 * ============================================================ */
void tss_set_rsp0(uint64_t rsp0)
{
    g_tss.rsp[0] = rsp0;
}

uint64_t tss_get_rsp0(void)
{
    return g_tss.rsp[0];
}

/* ============================================================
 * GDT セレクタ公開
 * ============================================================ */
uint16_t gdt_kern_code_sel(void) { return GDT_KERN_CODE; }
uint16_t gdt_kern_data_sel(void) { return GDT_KERN_DATA; }
uint16_t gdt_user_code_sel(void) { return GDT_USER_CODE64 | 3; /* RPL=3 */ }
uint16_t gdt_user_data_sel(void) { return GDT_USER_DATA  | 3; /* RPL=3 */ }
uint16_t gdt_tss_sel(void)       { return GDT_TSS_SEL; }
