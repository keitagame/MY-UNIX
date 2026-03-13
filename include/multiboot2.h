#pragma once
#ifndef _MULTIBOOT2_H
#define _MULTIBOOT2_H

/* ============================================================
 * multiboot2.h - Multiboot2 仕様定義
 * GRUB2 から渡される情報の解析に使用
 * ============================================================ */

#include "types.h"

/* マジックナンバー */
#define MULTIBOOT2_MAGIC           0x36d76289
#define MULTIBOOT2_HEADER_MAGIC    0xe85250d6

/* Multiboot2 ヘッダタグ */
#define MB2_TAG_END                0
#define MB2_TAG_CMDLINE            1
#define MB2_TAG_BOOT_LOADER        2
#define MB2_TAG_MODULE             3
#define MB2_TAG_BASIC_MEMINFO      4
#define MB2_TAG_BOOTDEV            5
#define MB2_TAG_MMAP               6
#define MB2_TAG_VBE                7
#define MB2_TAG_FRAMEBUFFER        8
#define MB2_TAG_ELF_SECTIONS       9
#define MB2_TAG_APM                10
#define MB2_TAG_EFI32              11
#define MB2_TAG_EFI64              12
#define MB2_TAG_SMBIOS             13
#define MB2_TAG_ACPI_OLD           14
#define MB2_TAG_ACPI_NEW           15
#define MB2_TAG_NETWORK            16
#define MB2_TAG_EFI_MMAP           17
#define MB2_TAG_EFI_BS             18
#define MB2_TAG_EFI32_IH           19
#define MB2_TAG_EFI64_IH           20
#define MB2_TAG_LOAD_BASE_ADDR     21

/* メモリタイプ */
#define MB2_MMAP_AVAILABLE         1
#define MB2_MMAP_RESERVED          2
#define MB2_MMAP_ACPI_RECLAIMABLE  3
#define MB2_MMAP_NVS               4
#define MB2_MMAP_BADRAM            5

/* ============================================================
 * Multiboot2 情報構造体
 * ============================================================ */

struct mb2_tag {
    uint32_t type;
    uint32_t size;
} PACKED;

struct mb2_tag_string {
    uint32_t type;
    uint32_t size;
    char     string[0];
} PACKED;

struct mb2_tag_module {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char     cmdline[0];
} PACKED;

struct mb2_tag_basic_meminfo {
    uint32_t type;
    uint32_t size;
    uint32_t mem_lower;   /* KB単位 */
    uint32_t mem_upper;   /* KB単位 */
} PACKED;

struct mb2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t reserved;
} PACKED;

struct mb2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct mb2_mmap_entry entries[0];
} PACKED;

struct mb2_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint16_t reserved;
} PACKED;

struct mb2_tag_elf_sections {
    uint32_t type;
    uint32_t size;
    uint32_t num;
    uint32_t entsize;
    uint32_t shndx;
    char     sections[0];
} PACKED;

struct mb2_tag_acpi {
    uint32_t type;
    uint32_t size;
    uint8_t  rsdp[0];
} PACKED;

/* Multiboot2 情報ブロック全体 */
struct multiboot2_info {
    uint32_t total_size;
    uint32_t reserved;
    struct mb2_tag tags[0];
} PACKED;

/* ============================================================
 * 解析ヘルパー関数
 * ============================================================ */
static inline struct mb2_tag *mb2_first_tag(struct multiboot2_info *mbi) {
    return (struct mb2_tag *)((uintptr_t)mbi + 8);
}

static inline struct mb2_tag *mb2_next_tag(struct mb2_tag *tag) {
    uintptr_t next = (uintptr_t)tag + ALIGN_UP(tag->size, 8);
    return (struct mb2_tag *)next;
}

static inline struct mb2_tag *mb2_find_tag(struct multiboot2_info *mbi, uint32_t type) {
    struct mb2_tag *tag = mb2_first_tag(mbi);
    while (tag->type != MB2_TAG_END) {
        if (tag->type == type)
            return tag;
        tag = mb2_next_tag(tag);
    }
    return NULL;
}

/* ============================================================
 * Multiboot2 ヘッダ (カーネルイメージ内に埋め込む)
 * ============================================================ */
struct mb2_header {
    uint32_t magic;
    uint32_t architecture;  /* 0 = i386 */
    uint32_t header_length;
    uint32_t checksum;      /* -(magic + arch + length) */
} PACKED;

#define MB2_ARCH_I386    0
#define MB2_ARCH_MIPS32  4

#endif /* _MULTIBOOT2_H */
