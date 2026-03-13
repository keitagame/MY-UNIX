/* ============================================================
 * idt.c - 割り込みディスクリプタテーブル + 例外/IRQハンドラ
 * ============================================================ */

#include "../../include/types.h"
#include "../../include/kernel.h"
#include "../../include/process.h"

/* ============================================================
 * IDT エントリ (Gate Descriptor)
 * ============================================================ */
typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;          /* IST インデックス (0=通常スタック) */
    uint8_t  type_attr;    /* タイプ + DPL + P */
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} PACKED idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} PACKED idt_ptr_t;

/* ============================================================
 * ゲートタイプ
 * ============================================================ */
#define IDT_INTERRUPT_GATE  0x8E  /* P=1, DPL=0, Type=Interrupt (14) */
#define IDT_TRAP_GATE       0x8F  /* P=1, DPL=0, Type=Trap (15) */
#define IDT_USER_INTR       0xEE  /* P=1, DPL=3, Type=Interrupt (syscall等) */
#define IDT_USER_TRAP       0xEF  /* P=1, DPL=3, Type=Trap */

/* ============================================================
 * IDT データ
 * ============================================================ */
#define IDT_ENTRIES  256
static idt_entry_t g_idt[IDT_ENTRIES] __attribute__((aligned(16)));
static idt_ptr_t   g_idt_ptr;

/* IRQハンドラテーブル */
#define IRQ_BASE    32
#define IRQ_COUNT   16
typedef void (*irq_handler_t)(int irq, void *data);

typedef struct {
    irq_handler_t handler;
    void         *data;
    const char   *name;
    uint32_t      count;  /* 割り込み回数 */
} irq_desc_t;

static irq_desc_t g_irq_table[IRQ_COUNT];

/* 例外名 */
static const char *g_exception_names[] = {
    "#DE Divide Error",
    "#DB Debug",
    "#NMI Non-Maskable Interrupt",
    "#BP Breakpoint",
    "#OF Overflow",
    "#BR Bound Range Exceeded",
    "#UD Invalid Opcode",
    "#NM Device Not Available",
    "#DF Double Fault",
    "#CSO Coprocessor Segment Overrun",
    "#TS Invalid TSS",
    "#NP Segment Not Present",
    "#SS Stack Fault",
    "#GP General Protection",
    "#PF Page Fault",
    "Reserved 15",
    "#MF x87 Floating Point",
    "#AC Alignment Check",
    "#MC Machine Check",
    "#XF SIMD Floating Point",
    "#VE Virtualization Exception",
    "#CP Control Protection",
    "Reserved 22", "Reserved 23",
    "Reserved 24", "Reserved 25",
    "Reserved 26", "Reserved 27",
    "Reserved 28", "Reserved 29",
    "#SX Security Exception", "Reserved 31",
};

/* ============================================================
 * PIC (8259A) 操作
 * ============================================================ */
#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1
#define PIC_EOI    0x20    /* End Of Interrupt */
#define PIC_READ_IRR 0x0A
#define PIC_READ_ISR 0x0B

static void pic_remap(uint8_t offset1, uint8_t offset2)
{
    /* マスタ/スレーブ PIC 初期化シーケンス */
    outb(PIC1_CMD,  0x11); io_wait();  /* ICW1: init+ICW4 */
    outb(PIC2_CMD,  0x11); io_wait();
    outb(PIC1_DATA, offset1); io_wait();  /* ICW2: vector offset */
    outb(PIC2_DATA, offset2); io_wait();
    outb(PIC1_DATA, 0x04); io_wait();  /* ICW3: slave at IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();  /* ICW4: 8086 mode */
    outb(PIC2_DATA, 0x01); io_wait();
    /* 全マスク (後でアンマスク) */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

static void pic_send_eoi(int irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

static void pic_set_mask(int irq, bool masked)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    uint8_t val = inb(port);
    if (masked)
        val |=  (1 << irq);
    else
        val &= ~(1 << irq);
    outb(port, val);
}

/* ============================================================
 * IDT エントリ設定
 * ============================================================ */
extern uint64_t isr_stub_table[];  /* boot.asm で定義 */
extern void idt_flush(idt_ptr_t *ptr);

static void idt_set_gate(int vec, uint64_t handler, uint8_t type, uint8_t ist)
{
    g_idt[vec].offset_low  = (uint16_t)(handler & 0xFFFF);
    g_idt[vec].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    g_idt[vec].offset_high = (uint32_t)(handler >> 32);
    g_idt[vec].selector    = 0x08;  /* カーネルコードセグメント */
    g_idt[vec].ist         = ist & 0x7;
    g_idt[vec].type_attr   = type;
    g_idt[vec].reserved    = 0;
}

/* ============================================================
 * ページフォルトハンドラ
 * ============================================================ */
static void handle_page_fault(cpu_context_t *ctx)
{
    uintptr_t fault_addr = read_cr2();
    uint64_t  err = ctx->err_code;
    
    bool present  = (err & 1) != 0;
    bool write    = (err & 2) != 0;
    bool user     = (err & 4) != 0;
    bool nx       = (err & 16) != 0;
    
    process_t *proc = g_current;
    
    if (user && proc) {
        /* ユーザー空間のページフォルト - SIGSEGV */
        printk(KERN_WARNING "PF: pid=%d addr=0x%llx err=0x%llx (%s%s%s%s) rip=0x%llx\n",
               proc->pid, (unsigned long long)fault_addr,
               (unsigned long long)err,
               present ? "P" : "NP",
               write   ? " W" : " R",
               user    ? " U" : " K",
               nx      ? " NX" : "",
               (unsigned long long)ctx->rip);
        
        siginfo_t info = {
            .si_signo = SIGSEGV,
            .si_code  = present ? 2 : 1,  /* SEGV_ACCERR / SEGV_MAPERR */
            .si_addr  = (void*)fault_addr,
        };
        signal_send(proc, SIGSEGV, &info);
        return;
    }
    
    /* カーネル空間のページフォルト - パニック */
    PANIC("#PF addr=0x%llx err=0x%llx rip=0x%llx rsp=0x%llx",
          (unsigned long long)fault_addr,
          (unsigned long long)err,
          (unsigned long long)ctx->rip,
          (unsigned long long)ctx->rsp);
}

/* ============================================================
 * 汎用例外ハンドラ
 * ============================================================ */
static void handle_exception(cpu_context_t *ctx)
{
    int vec = (int)ctx->int_no;
    const char *name = (vec < 32) ? g_exception_names[vec] : "Unknown";
    
    if (vec == 3 || vec == 4) {
        /* ブレークポイント / オーバーフロー - デバッグ用 */
        printk(KERN_DEBUG "Exception %d (%s) at RIP=0x%llx\n",
               vec, name, (unsigned long long)ctx->rip);
        return;
    }
    
    process_t *proc = g_current;
    if (proc && (ctx->cs & 3) == 3) {
        /* ユーザープロセスの例外 → シグナル送信 */
        int signo = SIGSEGV;
        if (vec == 0 || vec == 16 || vec == 19) signo = SIGFPE;
        if (vec == 6)  signo = SIGILL;
        if (vec == 7)  signo = SIGFPE;
        if (vec == 13) signo = SIGSEGV;
        
        printk(KERN_WARNING "Exception %d (%s) in pid=%d at RIP=0x%llx -> SIG%d\n",
               vec, name, proc->pid,
               (unsigned long long)ctx->rip, signo);
        
        signal_send(proc, signo, NULL);
        return;
    }
    
    /* カーネルモードの例外 → パニック */
    printk(KERN_CRIT "=== KERNEL EXCEPTION ===\n");
    printk(KERN_CRIT "Exception: %d (%s)\n", vec, name);
    printk(KERN_CRIT "Error code: 0x%llx\n", (unsigned long long)ctx->err_code);
    printk(KERN_CRIT "RIP: 0x%016llx  RSP: 0x%016llx\n",
           (unsigned long long)ctx->rip, (unsigned long long)ctx->rsp);
    printk(KERN_CRIT "RAX: 0x%016llx  RBX: 0x%016llx\n",
           (unsigned long long)ctx->rax, (unsigned long long)ctx->rbx);
    printk(KERN_CRIT "RCX: 0x%016llx  RDX: 0x%016llx\n",
           (unsigned long long)ctx->rcx, (unsigned long long)ctx->rdx);
    printk(KERN_CRIT "RSI: 0x%016llx  RDI: 0x%016llx\n",
           (unsigned long long)ctx->rsi, (unsigned long long)ctx->rdi);
    printk(KERN_CRIT "RBP: 0x%016llx\n", (unsigned long long)ctx->rbp);
    printk(KERN_CRIT "CS:  0x%04llx  RFLAGS: 0x%016llx\n",
           (unsigned long long)ctx->cs, (unsigned long long)ctx->rflags);
    
    cpu_halt();
}

/* ============================================================
 * タイマー割り込みハンドラ (IRQ0)
 * ============================================================ */
extern volatile uint64_t g_ticks;

static void timer_irq_handler(int irq, void *data)
{
    UNUSED(irq); UNUSED(data);
    g_ticks++;
    sched_tick();  /* スケジューラに通知 */
}

/* ============================================================
 * 共通割り込みハンドラ (boot.asm から呼ばれる)
 * ============================================================ */
void isr_common_handler(cpu_context_t *ctx)
{
    uint64_t vec = ctx->int_no;
    
    if (vec < 32) {
        /* CPU 例外 */
        if (vec == 14) {
            handle_page_fault(ctx);
        } else {
            handle_exception(ctx);
        }
    } else if (vec >= IRQ_BASE && vec < IRQ_BASE + IRQ_COUNT) {
        /* ハードウェア割り込み */
        int irq = (int)(vec - IRQ_BASE);
        g_irq_table[irq].count++;
        
        if (g_irq_table[irq].handler) {
            g_irq_table[irq].handler(irq, g_irq_table[irq].data);
        } else {
            /* 未登録割り込みは無視 */
        }
        
        pic_send_eoi(irq);
    } else if (vec >= 48) {
        /* ソフトウェア割り込み / その他 */
    }
    
    /* シグナル処理 */
    if (g_current && g_current->sig_pending & ~g_current->sig_blocked) {
        signal_handle(ctx);
    }
    
    /* スケジューラ (必要なら) */
    if (g_current && g_current->time_slice == 0) {
        schedule();
    }
}

/* ============================================================
 * IRQ 登録 API
 * ============================================================ */
int irq_register(int irq, irq_handler_t handler, void *data, const char *name)
{
    if (irq < 0 || irq >= IRQ_COUNT) return -EINVAL;
    
    uint64_t flags;
    IRQ_SAVE(flags);
    
    g_irq_table[irq].handler = handler;
    g_irq_table[irq].data    = data;
    g_irq_table[irq].name    = name;
    g_irq_table[irq].count   = 0;
    
    pic_set_mask(irq, false);  /* アンマスク */
    
    IRQ_RESTORE(flags);
    
    printk(KERN_INFO "IRQ: registered irq%d (%s)\n", irq, name);
    return 0;
}

void irq_unregister(int irq)
{
    if (irq < 0 || irq >= IRQ_COUNT) return;
    uint64_t flags;
    IRQ_SAVE(flags);
    pic_set_mask(irq, true);
    g_irq_table[irq].handler = NULL;
    IRQ_RESTORE(flags);
}

void irq_enable(int irq)  { pic_set_mask(irq, false); }
void irq_disable(int irq) { pic_set_mask(irq, true);  }

/* ============================================================
 * IDT 初期化
 * ============================================================ */
void idt_init(void)
{
    /* 全エントリにスタブを設定 */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        uint8_t type = IDT_INTERRUPT_GATE;
        uint8_t ist  = 0;
        
        if (i == 8) ist = 1;  /* #DF: IST1スタック使用 */
        if (i == 2) ist = 2;  /* NMI: IST2スタック使用 */
        
        idt_set_gate(i, isr_stub_table[i], type, ist);
    }
    
    /* ユーザーモードからアクセス可能なゲート */
    idt_set_gate(3, isr_stub_table[3], IDT_USER_TRAP, 0);   /* #BP */
    idt_set_gate(4, isr_stub_table[4], IDT_USER_TRAP, 0);   /* #OF */
    
    /* IDT ポインタ設定 */
    g_idt_ptr.limit = (uint16_t)(sizeof(g_idt) - 1);
    g_idt_ptr.base  = (uint64_t)g_idt;
    
    /* IDT ロード */
    idt_flush(&g_idt_ptr);
    
    /* PIC 再マップ (IRQ 0-15 → INT 32-47) */
    pic_remap(IRQ_BASE, IRQ_BASE + 8);
    
    /* IRQテーブル初期化 */
    memset(g_irq_table, 0, sizeof(g_irq_table));
    
    printk(KERN_INFO "IDT: initialized (%d gates, PIC remapped to INT%d)\n",
           IDT_ENTRIES, IRQ_BASE);
}

/* ============================================================
 * PIT タイマー初期化 (IRQ0)
 * ============================================================ */
void timer_init(void)
{
    /* PIT Channel 0: HZ Hz */
    uint32_t divisor = 1193182 / HZ;
    
    outb(PIT_COMMAND, 0x36);  /* CH0, lobyte/hibyte, mode3 (square wave) */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
    
    /* タイマー IRQ 登録 */
    irq_register(0, timer_irq_handler, NULL, "timer");
    
    printk(KERN_INFO "Timer: PIT at %dHz (divisor=%d)\n", HZ, divisor);
}

/* ============================================================
 * SYSCALL/SYSRET 有効化
 * ============================================================ */
#define IA32_STAR  0xC0000081
#define IA32_LSTAR 0xC0000082
#define IA32_FMASK 0xC0000084
#define IA32_EFER  0xC0000080
#define EFER_SCE   (1 << 0)    /* Syscall Enable */

extern void syscall_entry(void);
extern void set_kernel_gs_base(uint64_t base);
extern void set_gs_base(uint64_t base);

/* per-CPU 構造体 (GSベース) */
typedef struct {
    uint64_t kernel_stack;  /* [gs:0] カーネルスタック */
    uint64_t user_stack;    /* [gs:8] ユーザースタック保存 */
    uint32_t cpu_id;        /* [gs:16] CPU ID */
} PACKED per_cpu_t;

static per_cpu_t g_boot_cpu __attribute__((aligned(16)));

void syscall_setup(void)
{
    /* EFER.SCE = 1 (syscall有効) */
    uint64_t efer = rdmsr(IA32_EFER);
    wrmsr(IA32_EFER, efer | EFER_SCE);
    
    /* STAR: CS/SS セレクタ
     * bits[47:32] = kernel CS (SYSCALL: CS=STAR[47:32], SS=STAR[47:32]+8)
     * bits[63:48] = user CS-16 (SYSRET: CS=STAR[63:48]+16, SS=STAR[63:48]+8) */
    uint64_t star = ((uint64_t)0x08 << 32) |  /* kernel CS */
                    ((uint64_t)0x13 << 48);    /* user CS-16 (0x13 → CS=0x2B, SS=0x23) */
    wrmsr(IA32_STAR, star);
    
    /* LSTAR: syscall エントリポイント */
    wrmsr(IA32_LSTAR, (uint64_t)syscall_entry);
    
    /* FMASK: syscall時にクリアするRFLAGSビット */
    wrmsr(IA32_FMASK, 0x200); /* IF クリア */
    
    /* per-CPU 構造体設定 */
    g_boot_cpu.cpu_id = 0;
    set_kernel_gs_base((uint64_t)&g_boot_cpu);
    set_gs_base((uint64_t)&g_boot_cpu);
    
    printk(KERN_INFO "SYSCALL: enabled (LSTAR=0x%p)\n", (void*)syscall_entry);
}

/* per-CPU カーネルスタック更新 */
void per_cpu_set_kernel_stack(uint64_t rsp)
{
    g_boot_cpu.kernel_stack = rsp;
}

/* ============================================================
 * アーキテクチャ初期化まとめ
 * ============================================================ */
void arch_init_early(void)
{
    extern void gdt_init(void);
    gdt_init();
    idt_init();
}

void arch_init_late(void)
{
    timer_init();
    syscall_setup();
    sti();  /* 割り込み有効化 */
    printk(KERN_INFO "Arch: interrupts enabled\n");
}
