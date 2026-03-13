; ============================================================
; boot.asm - x86_64 Multiboot2 エントリポイント
; GRUB2 から Protected Mode で呼び出されるエントリ
; Long Mode (64bit) へ移行してカーネルのC関数を呼ぶ
; ============================================================

BITS 32

; ============================================================
; Multiboot2 ヘッダ
; ============================================================
section .multiboot2
align 8
mb2_header_start:
    dd  0xe85250d6          ; magic
    dd  0                   ; architecture: i386
    dd  mb2_header_end - mb2_header_start
    dd  -(0xe85250d6 + 0 + (mb2_header_end - mb2_header_start)) ; checksum

    ; フレームバッファタグ (オプション)
    align 8
    dw  5                   ; type: framebuffer
    dw  1                   ; flags: optional
    dd  20                  ; size
    dd  0                   ; width (0=auto)
    dd  0                   ; height (0=auto)
    dd  32                  ; depth

    ; 終端タグ
    align 8
    dw  0                   ; type: end
    dw  0                   ; flags
    dd  8                   ; size
mb2_header_end:

; ============================================================
; GDT (32→64bit 移行用)
; ============================================================
section .data
align 16

gdt64:
    dq 0                    ; NULL descriptor
gdt64_code:
    dq 0x00AF9A000000FFFF   ; 64bit コードセグメント
gdt64_data:
    dq 0x00CF92000000FFFF   ; データセグメント
gdt64_end:

gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dq gdt64

; ============================================================
; ページテーブル (一時的 identity mapping + カーネルhigher-half)
; 物理: 0x0000~0x200000 → 仮想: 0xFFFFFFFF80000000~
;       かつ identity map 0x0000~0x200000 → 0x0000~0x200000
; ============================================================
section .bss
align 4096

global boot_pml4
boot_pml4:
    resb 4096   ; PML4

boot_pdpt_low:
    resb 4096   ; 低位 PDPT (identity)

boot_pdpt_high:
    resb 4096   ; 高位 PDPT (kernel VMA)

boot_pd:
    resb 4096   ; Page Directory (2MB pages)

; ============================================================
; カーネルスタック (boot用)
; ============================================================
align 16
boot_stack_bottom:
    resb 16384          ; 16KB boot stack
boot_stack_top:

; ============================================================
; エントリポイント
; ============================================================
section .text

global _start
extern kernel_entry    ; C言語カーネルエントリ

_start:
    ; スタック設定 (32bit物理アドレス)
    mov esp, boot_stack_top - 0xFFFFFFFF80000000

    ; multiboot2 情報保存
    mov edi, ebx    ; mbi pointer (後でkernel_entryに渡す)
    mov esi, eax    ; magic

    ; PAE有効化
    mov eax, cr4
    or  eax, (1 << 5)   ; CR4.PAE
    mov cr4, eax

    ; ページテーブル構築
    call setup_page_tables

    ; CR3 = PML4物理アドレス
    mov eax, boot_pml4 - 0xFFFFFFFF80000000
    mov cr3, eax

    ; IA32_EFER.LME セット
    mov ecx, 0xC0000080     ; IA32_EFER MSR
    rdmsr
    or  eax, (1 << 8)       ; LME
    wrmsr

    ; PG + PE 有効化
    mov eax, cr0
    or  eax, (1 << 31) | (1 << 0)  ; PG + PE
    mov cr0, eax

    ; GDT64 ロード (物理アドレスで)
    lgdt [gdt64_ptr - 0xFFFFFFFF80000000]

    ; 64bit コードセグメントへジャンプ
    jmp 0x08:long_mode_entry - 0xFFFFFFFF80000000

; ----
setup_page_tables:
    ; PML4[0]   → boot_pdpt_low  (identity map)
    ; PML4[511] → boot_pdpt_high (kernel VMA)
    mov eax, boot_pdpt_low - 0xFFFFFFFF80000000
    or  eax, 3  ; P + RW
    mov [boot_pml4 - 0xFFFFFFFF80000000], eax

    mov eax, boot_pdpt_high - 0xFFFFFFFF80000000
    or  eax, 3
    mov [boot_pml4 - 0xFFFFFFFF80000000 + 511*8], eax

    ; PDPT_LOW[0] → boot_pd
    mov eax, boot_pd - 0xFFFFFFFF80000000
    or  eax, 3
    mov [boot_pdpt_low - 0xFFFFFFFF80000000], eax

    ; PDPT_HIGH[510] → boot_pd  (0xFFFFFFFF80000000 は PDPT[510])
    mov eax, boot_pd - 0xFFFFFFFF80000000
    or  eax, 3
    mov [boot_pdpt_high - 0xFFFFFFFF80000000 + 510*8], eax

    ; PD に 2MB ページ 4枚マップ (8MB = 十分なカーネルサイズ)
    ; 物理 0x0000000 → 0x0800000
    xor ecx, ecx
.pd_loop:
    mov eax, ecx
    shl eax, 21         ; 2MB * index
    or  eax, (1 << 7) | 3  ; PS + P + RW (2MB page)
    mov [boot_pd - 0xFFFFFFFF80000000 + ecx*8], eax
    inc ecx
    cmp ecx, 4
    jl  .pd_loop

    ret

; ============================================================
; 64bit ロングモード エントリ
; ============================================================
BITS 64

long_mode_entry:
    ; セグメントレジスタ初期化
    mov ax, 0x10    ; データセグメント
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax

    ; 仮想アドレスのスタックへ移行
    mov rsp, boot_stack_top

    ; GSベースをカーネルデータ用に設定
    ; (後でper-CPU構造体に変更)

    ; RDI = mbi物理アドレス (32bitだが後で仮想アドレスに変換)
    ; RSI = multiboot magic

    ; カーネルCエントリへジャンプ
    ; kernel_entry(uint32_t magic, uint32_t mbi_phys)
    mov rdi, rsi        ; magic → arg1
    mov rsi, rdi        ; mbi_phys → arg2 (実はEDIに入っている)
    
    ; 正しく渡す
    movzx rdi, edi      ; magic (ESI がEDIとして保存されていた)
    ; mbiはRSI

    call kernel_entry

    ; 戻ってきた場合はhalt
.halt:
    cli
    hlt
    jmp .halt

; ============================================================
; コンテキストスイッチ
; void context_switch(thread_context_t *from, thread_context_t *to)
; RDI = from, RSI = to
; ============================================================
global context_switch
context_switch:
    ; 現在のコンテキストを保存
    mov [rdi + 0],  rbx
    mov [rdi + 8],  rbp
    mov [rdi + 16], r12
    mov [rdi + 24], r13
    mov [rdi + 32], r14
    mov [rdi + 40], r15

    ; RSPとRIPを保存
    lea rax, [rel .return]

    ;lea rax, [rip + .return]
    mov [rdi + 56], rax     ; rip
    mov [rdi + 48], rsp     ; rsp

    ; 新しいコンテキストをロード
    mov rbx, [rsi + 0]
    mov rbp, [rsi + 8]
    mov r12, [rsi + 16]
    mov r13, [rsi + 24]
    mov r14, [rsi + 32]
    mov r15, [rsi + 40]
    mov rsp, [rsi + 48]

    ; 新しいRIPへジャンプ
    jmp [rsi + 56]

.return:
    ret

; ============================================================
; ユーザーモードへの遷移
; void context_switch_to_user(cpu_context_t *ctx)
; RDI = cpu_context_t*
; ============================================================
global context_switch_to_user
context_switch_to_user:
    mov rsp, rdi

    ; レジスタ復元
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; int_no と err_code をスキップ
    add rsp, 16

    ; iretq でユーザーモードへ
    iretq

; ============================================================
; syscall エントリ (SYSCALL命令)
; OS設定: LSTAR = syscall_entry
; RCX = 呼び出し元RIP, R11 = RFLAGS
; 引数: RDI, RSI, RDX, R10, R8, R9
; 番号: RAX
; ============================================================
global syscall_entry
extern syscall_dispatch

syscall_entry:
    ; カーネルスタックに切り替え
    swapgs                      ; GS ← kernel GS (per-CPU)
    mov [gs:8], rsp             ; ユーザーRSPを保存
    mov rsp, [gs:0]             ; カーネルスタックをロード

    ; レジスタ保存
    push rcx    ; user RIP
    push r11    ; user RFLAGS
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; 引数の並べ替え (Linux ABI: r10→rcx)
    mov  rcx, r10

    ; syscall_dispatch(nr, a1, a2, a3, a4, a5, a6)
    ; RAX=nr, RDI=a1, RSI=a2, RDX=a3, R10=a4, R8=a5, R9=a6
    ; C ABI: RDI=nr, RSI=a1, RDX=a2, RCX=a3, R8=a4, R9=a5 (a6はスタック)
    push r9     ; a6 → stack
    mov  r9,  r8    ; a5
    mov  r8,  rcx   ; a4 (= r10)
    mov  rcx, rdx   ; a3
    mov  rdx, rsi   ; a2
    mov  rsi, rdi   ; a1
    mov  rdi, rax   ; nr

    call syscall_dispatch
    add  rsp, 8     ; a6 を除去

    ; 戻り値 RAX は変更しない

    ; レジスタ復元
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    pop r11     ; user RFLAGS
    pop rcx     ; user RIP

    ; ユーザースタックに戻る
    mov rsp, [gs:8]
    swapgs

    sysretq

; ============================================================
; 割り込みスタブ群 (IDT用)
; ============================================================
global isr_stub_table
extern isr_common_handler

; スタブ生成マクロ
%macro ISR_NOERR 1
isr_stub_%1:
    push 0          ; ダミーエラーコード
    push %1         ; 割り込み番号
    jmp  isr_common_stub
%endmacro

%macro ISR_ERR 1
isr_stub_%1:
    push %1         ; 割り込み番号 (エラーコードはCPUがpush済み)
    jmp  isr_common_stub
%endmacro

; 例外スタブ
ISR_NOERR 0   ; #DE Divide Error
ISR_NOERR 1   ; #DB Debug
ISR_NOERR 2   ; NMI
ISR_NOERR 3   ; #BP Breakpoint
ISR_NOERR 4   ; #OF Overflow
ISR_NOERR 5   ; #BR Bound Range
ISR_NOERR 6   ; #UD Invalid Opcode
ISR_NOERR 7   ; #NM Device Not Available
ISR_ERR   8   ; #DF Double Fault
ISR_NOERR 9   ; Coprocessor Segment Overrun
ISR_ERR   10  ; #TS Invalid TSS
ISR_ERR   11  ; #NP Segment Not Present
ISR_ERR   12  ; #SS Stack Fault
ISR_ERR   13  ; #GP General Protection
ISR_ERR   14  ; #PF Page Fault
ISR_NOERR 15  ; Reserved
ISR_NOERR 16  ; #MF x87 FPE
ISR_ERR   17  ; #AC Alignment Check
ISR_NOERR 18  ; #MC Machine Check
ISR_NOERR 19  ; #XF SIMD FPE
ISR_NOERR 20  ; #VE Virtualization
ISR_NOERR 21  ; #CP Control Protection
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; IRQスタブ (32〜47)
%assign i 32
%rep 16
ISR_NOERR i
%assign i i+1
%endrep

; ソフトウェア割り込み (48〜255)
%assign i 48
%rep 208
ISR_NOERR i
%assign i i+1
%endrep

; 共通割り込みスタブ
isr_common_stub:
    ; 汎用レジスタ保存
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; セグメントレジスタ
    xor  eax, eax
    mov  ax, ds
    push rax
    mov  ax, 0x10   ; カーネルデータセグメント
    mov  ds, ax
    mov  es, ax

    ; ハンドラ呼び出し
    mov rdi, rsp    ; cpu_context_t* 渡す
    call isr_common_handler

    ; セグメント復元
    pop rax
    mov ds, ax
    mov es, ax

    ; レジスタ復元
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; int_no と err_code 除去
    add rsp, 16

    iretq

; スタブテーブル (ポインタ配列)
section .data
align 8
isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_%+i
%assign i i+1
%endrep

; ============================================================
; GDT/TSS ロードヘルパー
; ============================================================
section .text

global gdt_flush
gdt_flush:
    ; RDI = GDT pointer
    lgdt [rdi]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax
    push 0x08
    lea rax, [rel .flush_cs]
    ;lea rax, [rip + .flush_cs]
    push rax
    retfq
.flush_cs:
    ret

global tss_flush
tss_flush:
    ; RDI = TSS selector
    ltr di
    ret

global idt_flush
idt_flush:
    ; RDI = IDT pointer
    lidt [rdi]
    ret

; ============================================================
; MSR FS/GS ベース設定 (per-CPU)
; ============================================================
global set_kernel_gs_base
set_kernel_gs_base:
    ; RDI = kernel GS base address
    mov ecx, 0xC0000102     ; IA32_KERNEL_GS_BASE
    mov eax, edi
    shr rdi, 32
    mov edx, edi
    wrmsr
    ret

global set_gs_base
set_gs_base:
    mov ecx, 0xC0000101     ; IA32_GS_BASE
    mov eax, edi
    shr rdi, 32
    mov edx, edi
    wrmsr
    ret

global set_fs_base
set_fs_base:
    mov ecx, 0xC0000100     ; IA32_FS_BASE
    mov eax, edi
    shr rdi, 32
    mov edx, edi
    wrmsr
    ret

; ============================================================
; CPUID
; ============================================================
global cpuid
cpuid:
    ; RDI=leaf, RSI=subleaf, RDX=eax_out, RCX=ebx_out, R8=ecx_out, R9=edx_out
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov  r12, rdx   ; eax_out ptr
    mov  r13, rcx   ; ebx_out ptr
    mov  r14, r8    ; ecx_out ptr
    mov  r15, r9    ; edx_out ptr

    mov  eax, edi
    mov  ecx, esi
    cpuid

    mov [r12], eax
    mov [r13], ebx
    mov [r14], ecx
    mov [r15], edx

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret
