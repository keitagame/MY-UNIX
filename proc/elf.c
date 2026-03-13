/* ============================================================
 * elf.c - ELF64 バイナリローダー
 * Linux x86_64 ELF ダイナミック/スタティックバイナリをロード
 * ============================================================ */

#include "../include/types.h"
#include "../include/kernel.h"
#include "../include/mm.h"
#include "../include/process.h"
#include "../include/fs.h"

/* ============================================================
 * ELF64 定数・構造体
 * ============================================================ */
#define ELF_MAGIC  0x464C457F  /* "\x7fELF" little endian */

/* ELF クラス */
#define ELFCLASS32  1
#define ELFCLASS64  2

/* ELF データエンコーディング */
#define ELFDATA2LSB 1  /* リトルエンディアン */

/* ELF タイプ */
#define ET_NONE  0
#define ET_REL   1
#define ET_EXEC  2
#define ET_DYN   3
#define ET_CORE  4

/* マシンタイプ */
#define EM_X86_64  62

/* プログラムヘッダタイプ */
#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6
#define PT_TLS     7
#define PT_GNU_STACK 0x6474e551
#define PT_GNU_RELRO 0x6474e552

/* プログラムヘッダフラグ */
#define PF_X  1  /* Execute */
#define PF_W  2  /* Write */
#define PF_R  4  /* Read */

/* セクションヘッダタイプ */
#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_HASH     5
#define SHT_DYNAMIC  6
#define SHT_NOTE     7
#define SHT_NOBITS   8
#define SHT_REL      9

/* ELF64 ヘッダ */
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} PACKED Elf64_Ehdr;

/* ELF64 プログラムヘッダ */
typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} PACKED Elf64_Phdr;

/* ELF64 セクションヘッダ */
typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} PACKED Elf64_Shdr;

/* AT_* auxiliary vector */
#define AT_NULL    0
#define AT_IGNORE  1
#define AT_EXECFD  2
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7
#define AT_FLAGS   8
#define AT_ENTRY   9
#define AT_UID    11
#define AT_EUID   12
#define AT_GID    13
#define AT_EGID   14
#define AT_PLATFORM 15
#define AT_RANDOM  25
#define AT_EXECFN  31

/* ============================================================
 * ELF ロード実装
 * ============================================================ */

/* ELF ヘッダ検証 */
static int elf_check_header(const Elf64_Ehdr *ehdr)
{
    if (*(uint32_t*)ehdr->e_ident != ELF_MAGIC) {
        printk(KERN_DEBUG "ELF: bad magic 0x%08x\n", *(uint32_t*)ehdr->e_ident);
        return -ENOEXEC;
    }
    if (ehdr->e_ident[4] != ELFCLASS64) {
        printk(KERN_DEBUG "ELF: not 64-bit\n");
        return -ENOEXEC;
    }
    if (ehdr->e_ident[5] != ELFDATA2LSB) {
        printk(KERN_DEBUG "ELF: not little-endian\n");
        return -ENOEXEC;
    }
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        printk(KERN_DEBUG "ELF: not executable (type=%d)\n", ehdr->e_type);
        return -ENOEXEC;
    }
    if (ehdr->e_machine != EM_X86_64) {
        printk(KERN_DEBUG "ELF: not x86_64\n");
        return -ENOEXEC;
    }
    return 0;
}

/* PT_LOADセグメントのマッピング */
static int elf_map_segment(vmm_space_t *space,
                            file_t *f,
                            const Elf64_Phdr *phdr,
                            uint64_t load_bias)
{
    uintptr_t vaddr   = (uintptr_t)(phdr->p_vaddr + load_bias);
    uint64_t  memsz   = phdr->p_memsz;
    uint64_t  filesz  = phdr->p_filesz;
    uint64_t  offset  = phdr->p_offset;

    if (memsz == 0) return 0;

    /* ページアライン */
    uintptr_t page_vaddr  = ALIGN_DOWN(vaddr, PAGE_SIZE);
    uintptr_t page_offset = vaddr - page_vaddr;
    size_t    map_size    = ALIGN_UP(memsz + page_offset, PAGE_SIZE);
    size_t    n_pages     = map_size / PAGE_SIZE;

    /* フラグ変換 */
    uint64_t flags = PTE_PRESENT | PTE_USER;
    if (phdr->p_flags & PF_W) flags |= PTE_WRITABLE;
    if (!(phdr->p_flags & PF_X)) flags |= PTE_NX;

    /* ページを割り当ててマップ */
    for (size_t i = 0; i < n_pages; i++) {
        uintptr_t page_addr = page_vaddr + i * PAGE_SIZE;

        /* 既にマップされていたら既存ページを使う */
        if (vmm_is_mapped(space, page_addr)) continue;

        uintptr_t phys = pmm_alloc();
        if (!phys) return -ENOMEM;

        int r = vmm_map(space, page_addr, phys, flags);
        if (r < 0) {
            pmm_free(phys);
            return r;
        }
    }

    /* ファイルからデータを読み込む */
    if (filesz > 0) {
        /* 一時的にカーネル空間からユーザーページにアクセス */
        uint8_t *buf = kmalloc(filesz);
        if (!buf) return -ENOMEM;

        /* ファイルを読む */
        if (f->ops && f->ops->read) {
            ssize_t n = f->ops->read(f, buf, filesz);
            if (n < 0 || (uint64_t)n < filesz) {
                /* 不完全読み込み: 残りはゼロ */
                if (n > 0) {
                    /* 読めた分だけコピー */
                }
            }
        } else {
            /* vnodeから直接読む */
            ssize_t n = f->vnode->ops->read(f->vnode, buf, filesz, offset);
            UNUSED(n);
        }

        /* ユーザーページに書き込む (物理アドレス経由) */
        uint64_t write_off = 0;
        while (write_off < filesz) {
            uintptr_t target_virt = vaddr + write_off;
            uintptr_t target_page = ALIGN_DOWN(target_virt, PAGE_SIZE);
            uintptr_t phys = vmm_virt_to_phys(space, target_page);
            if (!phys) { kfree(buf); return -EFAULT; }

            void *kaddr = phys_to_kvirt(phys);
            uintptr_t page_off = target_virt - target_page;
            size_t can_write = MIN(PAGE_SIZE - page_off, filesz - write_off);

            memcpy((uint8_t*)kaddr + page_off, buf + write_off, can_write);
            write_off += can_write;
        }

        kfree(buf);
    }

    return 0;
}

/* ============================================================
 * elf_load - ELF バイナリをアドレス空間にロード
 * ============================================================ */
int elf_load(vmm_space_t *space, const char *path,
             char *const argv[], char *const envp[],
             elf_load_info_t *info)
{
    file_t *f = NULL;
    int r = vfs_open(path, O_RDONLY, 0, &f);
    if (r < 0) return r;

    /* ELFヘッダ読み込み */
    Elf64_Ehdr ehdr;
    ssize_t n = f->vnode->ops->read(f->vnode, &ehdr, sizeof(ehdr), 0);
    if (n < (ssize_t)sizeof(ehdr)) { r = -ENOEXEC; goto out; }

    r = elf_check_header(&ehdr);
    if (r < 0) goto out;

    /* プログラムヘッダ読み込み */
    if (ehdr.e_phnum == 0 || ehdr.e_phentsize < sizeof(Elf64_Phdr)) {
        r = -ENOEXEC;
        goto out;
    }

    size_t phdr_size = (size_t)ehdr.e_phnum * sizeof(Elf64_Phdr);
    Elf64_Phdr *phdrs = kmalloc(phdr_size);
    if (!phdrs) { r = -ENOMEM; goto out; }

    n = f->vnode->ops->read(f->vnode, phdrs, phdr_size, ehdr.e_phoff);
    if (n < (ssize_t)phdr_size) { r = -ENOEXEC; goto out_phdrs; }

    /* load_bias (ET_DYN はPIE) */
    uint64_t load_bias = 0;
    if (ehdr.e_type == ET_DYN) {
        /* PIE: 適当なベースアドレス */
        load_bias = 0x400000;
    }

    /* PT_INTERP チェック (dynamic linker) */
    char interp_path[PATH_MAX] = {0};
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type == PT_INTERP) {
            size_t isz = MIN(phdrs[i].p_filesz, (uint64_t)(PATH_MAX - 1));
            f->vnode->ops->read(f->vnode, interp_path, isz, phdrs[i].p_offset);
            break;
        }
    }

    /* アドレス範囲を計算 */
    uintptr_t text_start = UINT64_MAX, text_end = 0;
    uintptr_t data_start = UINT64_MAX, data_end = 0;
    uintptr_t brk        = 0;

    /* PT_LOAD セグメントをマップ */
    for (int i = 0; i < ehdr.e_phnum; i++) {
        Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;

        r = elf_map_segment(space, f, ph, load_bias);
        if (r < 0) goto out_phdrs;

        uintptr_t seg_start = (uintptr_t)(ph->p_vaddr + load_bias);
        uintptr_t seg_end   = seg_start + ph->p_memsz;

        if (ph->p_flags & PF_X) {
            if (seg_start < text_start) text_start = seg_start;
            if (seg_end   > text_end)   text_end   = seg_end;
        } else {
            if (seg_start < data_start) data_start = seg_start;
            if (seg_end   > data_end)   data_end   = seg_end;
        }

        if (seg_end > brk) brk = seg_end;
    }

    brk = ALIGN_UP(brk, PAGE_SIZE);

    /* ユーザースタック設定 */
    uintptr_t stack_top  = VMM_STACK_TOP;
    uintptr_t stack_size = VMM_STACK_SIZE;
    uintptr_t stack_base = stack_top - stack_size;

    /* スタックページを割り当て */
    for (uintptr_t p = stack_base; p < stack_top; p += PAGE_SIZE) {
        uintptr_t phys = pmm_alloc();
        if (!phys) { r = -ENOMEM; goto out_phdrs; }
        vmm_map(space, p, phys, VMM_USER_RW);
    }

    /* argv/envp/AUX をスタックに積む */
    uintptr_t sp = stack_top;

    /* 文字列データをスタックトップ付近にコピー */
    /* まず全文字列のサイズを計算 */
    int argc = 0;
    size_t argv_total = 0;
    if (argv) {
        for (int i = 0; argv[i]; i++, argc++) {
            argv_total += strlen(argv[i]) + 1;
        }
    }
    int envc = 0;
    size_t envp_total = 0;
    if (envp) {
        for (int i = 0; envp[i]; i++, envc++) {
            envp_total += strlen(envp[i]) + 1;
        }
    }

    /* スタック上に文字列を配置 */
    sp -= argv_total + envp_total + PATH_MAX;
    sp = ALIGN_DOWN(sp, 16);

    /* argv ポインタ配列 */
    uintptr_t str_ptr = sp;
    uintptr_t *argv_ptrs = kmalloc((argc + 1) * sizeof(uintptr_t));
    if (!argv_ptrs) { r = -ENOMEM; goto out_phdrs; }

    for (int i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]) + 1;
        uintptr_t phys = vmm_virt_to_phys(space, ALIGN_DOWN(str_ptr, PAGE_SIZE));
        if (phys) {
            void *kp = phys_to_kvirt(phys);
            uintptr_t off = str_ptr & (PAGE_SIZE - 1);
            memcpy((uint8_t*)kp + off, argv[i], MIN(len, PAGE_SIZE - off));
        }
        argv_ptrs[i] = str_ptr;
        str_ptr += len;
    }
    argv_ptrs[argc] = 0;

    /* envp ポインタ配列 */
    uintptr_t *envp_ptrs = kmalloc((envc + 1) * sizeof(uintptr_t));
    if (!envp_ptrs) { kfree(argv_ptrs); r = -ENOMEM; goto out_phdrs; }

    for (int i = 0; i < envc; i++) {
        size_t len = strlen(envp[i]) + 1;
        uintptr_t phys = vmm_virt_to_phys(space, ALIGN_DOWN(str_ptr, PAGE_SIZE));
        if (phys) {
            void *kp = phys_to_kvirt(phys);
            uintptr_t off = str_ptr & (PAGE_SIZE - 1);
            memcpy((uint8_t*)kp + off, envp[i], MIN(len, PAGE_SIZE - off));
        }
        envp_ptrs[i] = str_ptr;
        str_ptr += len;
    }
    envp_ptrs[envc] = 0;

    /* AUX ベクタを配置 */
    typedef struct { uint64_t a_type; uint64_t a_val; } auxv_t;
    auxv_t auxv[] = {
        { AT_PAGESZ, PAGE_SIZE         },
        { AT_PHDR,   (uint64_t)(load_bias + ehdr.e_phoff) },
        { AT_PHENT,  ehdr.e_phentsize  },
        { AT_PHNUM,  ehdr.e_phnum      },
        { AT_ENTRY,  ehdr.e_entry + load_bias },
        { AT_UID,    0 },
        { AT_EUID,   0 },
        { AT_GID,    0 },
        { AT_EGID,   0 },
        { AT_NULL,   0 },
    };
    int n_auxv = ARRAY_SIZE(auxv);

    /* スタックフレーム構築
     * [ argc | argv[0..argc] | 0 | envp[0..envc] | 0 | auxv... ]  */
    sp = ALIGN_DOWN(sp - 8, 16);
    /* AUX */
    sp -= n_auxv * sizeof(auxv_t);
    uintptr_t aux_sp = sp;
    /* envp */
    sp -= (envc + 1) * sizeof(uintptr_t);
    uintptr_t envp_sp = sp;
    /* argv */
    sp -= (argc + 1) * sizeof(uintptr_t);
    uintptr_t argv_sp = sp;
    /* argc */
    sp -= sizeof(uint64_t);
    uintptr_t argc_sp = sp;

    sp = ALIGN_DOWN(sp, 16);

    /* 物理ページに書き込む汎用関数 */
    #define WRITE_USER(addr, data, sz) do { \
        void *_kp = phys_to_kvirt(vmm_virt_to_phys(space, ALIGN_DOWN((uintptr_t)(addr), PAGE_SIZE))); \
        if (_kp) memcpy((uint8_t*)_kp + ((uintptr_t)(addr) & (PAGE_SIZE-1)), (data), (sz)); \
    } while(0)

    uint64_t _argc = argc;
    WRITE_USER(argc_sp, &_argc, sizeof(uint64_t));
    WRITE_USER(argv_sp, argv_ptrs, (argc + 1) * sizeof(uintptr_t));
    WRITE_USER(envp_sp, envp_ptrs, (envc + 1) * sizeof(uintptr_t));
    WRITE_USER(aux_sp,  auxv, n_auxv * sizeof(auxv_t));

    kfree(argv_ptrs);
    kfree(envp_ptrs);

    /* 結果を返す */
    info->entry      = ehdr.e_entry + load_bias;
    info->stack_top  = sp;
    info->text_start = (text_start == UINT64_MAX) ? 0 : text_start;
    info->text_end   = text_end;
    info->data_start = (data_start == UINT64_MAX) ? 0 : data_start;
    info->data_end   = data_end;
    info->brk        = brk;

    r = 0;

out_phdrs:
    kfree(phdrs);
out:
    file_put(f);
    return r;
}

/* ============================================================
 * proc_exec - ELF を現在のプロセスに上書き実行
 * ============================================================ */
int proc_exec(process_t *proc, const char *path,
              char *const argv[], char *const envp[])
{
    /* 新しいアドレス空間を作成 */
    vmm_space_t *new_space = vmm_create_space();
    if (!new_space) return -ENOMEM;

    elf_load_info_t info;
    int r = elf_load(new_space, path, argv, envp, &info);
    if (r < 0) {
        vmm_destroy_space(new_space);
        return r;
    }

    /* 古いアドレス空間を破棄 */
    vmm_space_t *old_space = proc->vm_space;
    proc->vm_space = new_space;
    vmm_switch_space(new_space);
    vmm_destroy_space(old_space);

    /* FDのCLOEXECをクローズ */
    fd_close_on_exec(proc->fd_table);

    /* プロセス情報更新 */
    const char *basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;
    strncpy(proc->comm, basename, PROC_COMM_MAX - 1);
    strncpy(proc->name, path, PROC_NAME_MAX - 1);

    proc->brk        = info.brk;
    proc->brk_start  = info.brk;
    proc->text_start = info.text_start;
    proc->text_end   = info.text_end;
    proc->data_start = info.data_start;
    proc->data_end   = info.data_end;
    proc->user_stack_top = info.stack_top;

    /* シグナルハンドラをリセット */
    for (int i = 1; i <= NSIG; i++) {
        if (proc->sig_handlers[i].sa_handler != SIG_IGN) {
            proc->sig_handlers[i].sa_handler = SIG_DFL;
            proc->sig_handlers[i].sa_flags   = 0;
            proc->sig_handlers[i].sa_mask    = 0;
        }
    }

    /* ユーザーモードで実行開始 */
    cpu_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.rip    = info.entry;
    ctx.rsp    = info.stack_top;
    ctx.cs     = 0x2B;  /* user code64 | RPL3 */
    ctx.ss     = 0x23;  /* user data   | RPL3 */
    ctx.rflags = RFLAGS_IF;

    /* カーネルスタックにコンテキストを積んでiretq */
    context_switch_to_user(&ctx);

    /* ここには到達しない */
    return 0;
}
