/* ============================================================
 * kernel.ld - カーネルリンカスクリプト
 * 仮想アドレス: 0xFFFFFFFF80000000 (higher half)
 * 物理アドレス: 0x0000000000100000 (1MB)
 * ============================================================ */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;
KERNEL_LMA = 0x0000000000100000;

SECTIONS
{
    /* ============================================
     * .multiboot2 - Multiboot2ヘッダ
     * 最初に配置 (ファイル先頭付近に必要)
     * ============================================ */
    . = KERNEL_LMA;

    .multiboot2 : AT(KERNEL_LMA) {
        KEEP(*(.multiboot2))
    }

    /* ============================================
     * 仮想アドレスへ移行
     * ============================================ */
    . = KERNEL_VMA + SIZEOF(.multiboot2) + ALIGNOF(.text);

    _kernel_start = .;

    /* ============================================
     * .text - コードセクション
     * ============================================ */
    .text ALIGN(4096) : AT(ADDR(.text) - KERNEL_VMA + KERNEL_LMA)
    {
        *(.text._start)
        *(.text.boot)
        *(.text*)
        *(.gnu.linkonce.t*)
    }

    /* ============================================
     * .rodata - 読み取り専用データ
     * ============================================ */
    .rodata ALIGN(4096) : AT(ADDR(.rodata) - KERNEL_VMA + KERNEL_LMA)
    {
        *(.rodata*)
        *(.gnu.linkonce.r*)

        /* CTORテーブル (グローバルコンストラクタ) */
        . = ALIGN(8);
        __init_array_start = .;
        *(.init_array*)
        *(.ctors)
        __init_array_end = .;
    }

    /* ============================================
     * .data - 読み書きデータ
     * ============================================ */
    .data ALIGN(4096) : AT(ADDR(.data) - KERNEL_VMA + KERNEL_LMA)
    {
        *(.data*)
        *(.gnu.linkonce.d*)
    }

    /* ============================================
     * .bss - 未初期化データ
     * ============================================ */
    .bss ALIGN(4096) : AT(ADDR(.bss) - KERNEL_VMA + KERNEL_LMA)
    {
        __bss_start = .;
        *(.bss*)
        *(.gnu.linkonce.b*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }

    _kernel_end = .;

    /* ============================================
     * デバッグ情報 (DWARF)
     * ============================================ */
    .debug_info     0 : { *(.debug_info*) }
    .debug_abbrev   0 : { *(.debug_abbrev*) }
    .debug_aranges  0 : { *(.debug_aranges*) }
    .debug_ranges   0 : { *(.debug_ranges*) }
    .debug_line     0 : { *(.debug_line*) }
    .debug_str      0 : { *(.debug_str*) }
    .debug_pubnames 0 : { *(.debug_pubnames*) }
    .debug_pubtypes 0 : { *(.debug_pubtypes*) }
    .debug_frame    0 : { *(.debug_frame*) }
    .debug_loc      0 : { *(.debug_loc*) }

    /* 不要セクション破棄 */
    /DISCARD/ : {
        *(.note*)
        *(.comment)
        *(.eh_frame*)
        *(.gcc_except_table*)
    }
}
