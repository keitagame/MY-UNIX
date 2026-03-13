/* ============================================================
 * main.c - カーネルエントリポイント + 共通ライブラリ
 * ============================================================ */

#include "include/types.h"
#include "include/kernel.h"
#include "include/mm.h"
#include "include/process.h"
#include "include/fs.h"
#include "include/syscall.h"
#include "include/multiboot2.h"

/* ============================================================
 * シリアルポート (デバッグ出力 COM1)
 * ============================================================ */
#define COM1_BASE     0x3F8
#define COM1_DATA     (COM1_BASE + 0)
#define COM1_IER      (COM1_BASE + 1)
#define COM1_FCR      (COM1_BASE + 2)
#define COM1_LCR      (COM1_BASE + 3)
#define COM1_MCR      (COM1_BASE + 4)
#define COM1_LSR      (COM1_BASE + 5)
#define COM1_LSR_THRE 0x20  /* Transmit Hold Register Empty */

static bool g_serial_ok = false;

static void serial_init(void)
{
    outb(COM1_IER, 0x00);   /* 割り込み無効 */
    outb(COM1_LCR, 0x80);   /* DLAB=1 */
    outb(COM1_DATA, 1);     /* Divisor low: 115200bps */
    outb(COM1_IER,  0);     /* Divisor high */
    outb(COM1_LCR, 0x03);   /* 8N1 */
    outb(COM1_FCR, 0xC7);   /* FIFO有効 */
    outb(COM1_MCR, 0x03);   /* DTR+RTS */
    g_serial_ok = true;
}

static void serial_putc(char c)
{
    if (!g_serial_ok) return;
    while (!(inb(COM1_LSR) & COM1_LSR_THRE));
    outb(COM1_DATA, (uint8_t)c);
    if (c == '\n') {
        while (!(inb(COM1_LSR) & COM1_LSR_THRE));
        outb(COM1_DATA, '\r');
    }
}

static void serial_puts(const char *s)
{
    while (*s) serial_putc(*s++);
}

/* ============================================================
 * VGA テキストコンソール
 * ============================================================ */
#define VGA_BASE    0xB8000
#define VGA_COLS    80
#define VGA_ROWS    25
#define VGA_WHITE_ON_BLACK 0x07
#define VGA_RED_ON_BLACK   0x04
#define VGA_GREEN_ON_BLACK 0x02

static volatile uint16_t *g_vga = (volatile uint16_t *)(0xFFFFFFFF800B8000ULL);
static int g_vga_col = 0;
static int g_vga_row = 0;
static uint8_t g_vga_color = VGA_WHITE_ON_BLACK;

static void vga_scroll(void)
{
    for (int row = 0; row < VGA_ROWS - 1; row++)
        for (int col = 0; col < VGA_COLS; col++)
            g_vga[row * VGA_COLS + col] = g_vga[(row + 1) * VGA_COLS + col];
    for (int col = 0; col < VGA_COLS; col++)
        g_vga[(VGA_ROWS - 1) * VGA_COLS + col] = (uint16_t)(' ' | ((uint16_t)g_vga_color << 8));
}

static void vga_putc(char c)
{
    if (c == '\n') {
        g_vga_col = 0;
        if (++g_vga_row >= VGA_ROWS) { vga_scroll(); g_vga_row = VGA_ROWS - 1; }
    } else if (c == '\r') {
        g_vga_col = 0;
    } else if (c == '\t') {
        g_vga_col = (g_vga_col + 8) & ~7;
        if (g_vga_col >= VGA_COLS) {
            g_vga_col = 0;
            if (++g_vga_row >= VGA_ROWS) { vga_scroll(); g_vga_row = VGA_ROWS - 1; }
        }
    } else {
        if (g_vga_col < VGA_COLS)
            g_vga[g_vga_row * VGA_COLS + g_vga_col++] =
                (uint16_t)((uint8_t)c | ((uint16_t)g_vga_color << 8));
        if (g_vga_col >= VGA_COLS) {
            g_vga_col = 0;
            if (++g_vga_row >= VGA_ROWS) { vga_scroll(); g_vga_row = VGA_ROWS - 1; }
        }
    }
}

static void vga_clear(void)
{
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        g_vga[i] = (uint16_t)(' ' | ((uint16_t)VGA_WHITE_ON_BLACK << 8));
    g_vga_col = g_vga_row = 0;
}

/* ============================================================
 * printk / vprintk / vsnprintf
 * ============================================================ */
static spinlock_t g_printk_lock = SPINLOCK_INIT;

static void printk_putc(char c)
{
    vga_putc(c);
    serial_putc(c);
}

static void printk_puts(const char *s)
{
    while (*s) printk_putc(*s++);
}

/* 簡易 vsnprintf 実装 */
int vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list args)
{
    size_t pos = 0;
    #define PUT(c) do { if (pos + 1 < size) buf[pos++] = (c); } while(0)

    while (*fmt) {
        if (*fmt != '%') { PUT(*fmt++); continue; }
        fmt++;

        /* フラグ */
        bool zero_pad = false;
        bool left_align = false;
        bool plus = false;
        bool alt = false;
        while (*fmt == '0' || *fmt == '-' || *fmt == '+' || *fmt == '#') {
            if (*fmt == '0') zero_pad = true;
            if (*fmt == '-') left_align = true;
            if (*fmt == '+') plus = true;
            if (*fmt == '#') alt = true;
            fmt++;
        }
        UNUSED(plus); UNUSED(alt);

        /* 幅 */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        /* 長さ修飾子 */
        bool is_long = false;
        bool is_longlong = false;
        bool is_size = false;
        if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') { fmt++; is_longlong = true; }
            else is_long = true;
        } else if (*fmt == 'z') {
            fmt++; is_size = true;
        } else if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h') fmt++;
        }

        char spec = *fmt++;
        char tmp[32];
        const char *s_val = NULL;
        size_t s_len = 0;

        switch (spec) {
        case 'd': case 'i': {
            int64_t val;
            if (is_longlong)    val = (int64_t)__builtin_va_arg(args, long long);
            else if (is_long || is_size) val = (int64_t)__builtin_va_arg(args, long);
            else                val = (int64_t)__builtin_va_arg(args, int);

            bool neg = val < 0;
            uint64_t uval = neg ? (uint64_t)(-val) : (uint64_t)val;
            int i = 0;
            if (uval == 0) tmp[i++] = '0';
            while (uval > 0) { tmp[i++] = '0' + (int)(uval % 10); uval /= 10; }
            if (neg) tmp[i++] = '-';
            /* 反転 */
            for (int a = 0, b = i-1; a < b; a++, b--) { char t = tmp[a]; tmp[a] = tmp[b]; tmp[b] = t; }
            tmp[i] = '\0';
            s_val = tmp; s_len = (size_t)i;
            break;
        }
        case 'u': {
            uint64_t val;
            if (is_longlong)    val = __builtin_va_arg(args, unsigned long long);
            else if (is_long || is_size) val = (uint64_t)__builtin_va_arg(args, unsigned long);
            else                val = (uint64_t)__builtin_va_arg(args, unsigned int);
            int i = 0;
            if (val == 0) tmp[i++] = '0';
            while (val > 0) { tmp[i++] = '0' + (int)(val % 10); val /= 10; }
            for (int a = 0, b = i-1; a < b; a++, b--) { char t = tmp[a]; tmp[a] = tmp[b]; tmp[b] = t; }
            tmp[i] = '\0';
            s_val = tmp; s_len = (size_t)i;
            break;
        }
        case 'x': case 'X': case 'p': {
            uint64_t val;
            if (spec == 'p')    val = (uint64_t)(uintptr_t)__builtin_va_arg(args, void*);
            else if (is_longlong) val = __builtin_va_arg(args, unsigned long long);
            else if (is_long || is_size) val = (uint64_t)__builtin_va_arg(args, unsigned long);
            else                val = (uint64_t)__builtin_va_arg(args, unsigned int);

            const char *hex = (spec == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            int i = 0;
            if (val == 0) tmp[i++] = '0';
            while (val > 0) { tmp[i++] = hex[val & 0xF]; val >>= 4; }
            if (spec == 'p' && i < 16) { while (i < 16) tmp[i++] = '0'; }
            for (int a = 0, b = i-1; a < b; a++, b--) { char t = tmp[a]; tmp[a] = tmp[b]; tmp[b] = t; }
            tmp[i] = '\0';
            s_val = tmp; s_len = (size_t)i;
            if (spec == 'p') { PUT('0'); PUT('x'); }
            break;
        }
        case 's': {
            s_val = __builtin_va_arg(args, const char*);
            if (!s_val) s_val = "(null)";
            s_len = strlen(s_val);
            break;
        }
        case 'c': {
            tmp[0] = (char)__builtin_va_arg(args, int);
            tmp[1] = '\0';
            s_val = tmp; s_len = 1;
            break;
        }
        case '%': PUT('%'); continue;
        default:  PUT('%'); PUT(spec); continue;
        }

        if (s_val) {
            /* パディング */
            int pad = (int)((size_t)width > s_len ? (size_t)width - s_len : 0);
            if (!left_align) {
                char pad_c = zero_pad ? '0' : ' ';
                for (int i = 0; i < pad; i++) PUT(pad_c);
            }
            for (size_t i = 0; i < s_len; i++) PUT(s_val[i]);
            if (left_align)
                for (int i = 0; i < pad; i++) PUT(' ');
        }
    }
    #undef PUT
    if (size > 0) buf[pos] = '\0';
    return (int)pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    int r = vsnprintf(buf, size, fmt, args);
    __builtin_va_end(args);
    return r;
}

int vprintk(const char *fmt, __builtin_va_list args)
{
    char buf[1024];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    spinlock_lock(&g_printk_lock);
    printk_puts(buf);
    spinlock_unlock(&g_printk_lock);
    return len;
}

int printk(const char *fmt, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    int r = vprintk(fmt, args);
    __builtin_va_end(args);
    return r;
}

/* ============================================================
 * panic
 * ============================================================ */
NORETURN void panic_at(const char *file, int line, const char *func, const char *fmt, ...)
{
    cli();
    g_vga_color = VGA_RED_ON_BLACK;

    printk("\n\n*** KERNEL PANIC ***\n");
    printk("File: %s  Line: %d  Func: %s\n", file, line, func);

    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    vprintk(fmt, args);
    __builtin_va_end(args);
    printk("\n");

    serial_puts("\n*** KERNEL PANIC ***\n");

    /* スタックトレース (簡略) */
    uint64_t *rbp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    printk("Stack trace:\n");
    for (int i = 0; i < 8 && rbp; i++) {
        uint64_t ret = rbp[1];
        if (ret < 0xFFFFFFFF80000000ULL) break;
        printk("  [%d] 0x%016llx\n", i, (unsigned long long)ret);
        rbp = (uint64_t *)rbp[0];
    }

    cpu_halt();
}

NORETURN void panic(const char *fmt, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    cli();
    g_vga_color = VGA_RED_ON_BLACK;
    printk("\n*** KERNEL PANIC: ");
    vprintk(fmt, args);
    printk(" ***\n");
    __builtin_va_end(args);
    cpu_halt();
}

/* ============================================================
 * 文字列関数
 * ============================================================ */
size_t strlen(const char *s)
{
    size_t n = 0;
    while (*s++) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n-- && *a && *a == *b) { a++; b++; }
    if (n == (size_t)-1) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (n && (*d = *src)) { d++; src++; n--; }
    while (n--) *d++ = '\0';
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++));
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (*d) d++;
    while (n-- && (*d = *src)) { d++; src++; }
    *d = '\0';
    return dst;
}

char *strchr(const char *s, int c)
{
    while (*s) { if (*s == (char)c) return (char*)s; s++; }
    return (c == '\0') ? (char*)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    while (*s) { if (*s == (char)c) last = s; s++; }
    return (char*)last;
}

char *strstr(const char *haystack, const char *needle)
{
    size_t nlen = strlen(needle);
    if (!nlen) return (char*)haystack;
    while (*haystack) {
        if (strncmp(haystack, needle, nlen) == 0) return (char*)haystack;
        haystack++;
    }
    return NULL;
}

char *strdup_k(const char *s)
{
    size_t len = strlen(s) + 1;
    char *d = kmalloc(len);
    if (d) memcpy(d, s, len);
    return d;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = (uint8_t)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    if (dst < src) return memcpy(dst, src, n);
    uint8_t *d = (uint8_t *)dst + n;
    const uint8_t *s = (const uint8_t *)src + n;
    while (n--) *--d = *--s;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    while (n--) {
        if (*pa != *pb) return *pa - *pb;
        pa++; pb++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = (const uint8_t *)s;
    while (n--) { if (*p == (uint8_t)c) return (void*)p; p++; }
    return NULL;
}

/* ============================================================
 * パイプ実装
 * ============================================================ */
#define PIPE_BUF_SIZE 65536

typedef struct {
    uint8_t  buf[PIPE_BUF_SIZE];
    size_t   read_pos;
    size_t   write_pos;
    size_t   avail;
    bool     write_closed;
    bool     read_closed;
    spinlock_t lock;
    wait_queue_t *rq;   /* 読み取り待ちキュー */
    wait_queue_t *wq;   /* 書き込み待ちキュー */
    atomic_t refcount;
} pipe_t;

static ssize_t pipe_read_fn(struct file *f, void *buf, size_t len)
{
    pipe_t *p = (pipe_t *)f->private;
    if (!p) return -EBADF;

    for (;;) {
        spinlock_lock(&p->lock);
        if (p->avail > 0) {
            size_t n = MIN(len, p->avail);
            uint8_t *dst = (uint8_t *)buf;
            for (size_t i = 0; i < n; i++) {
                dst[i] = p->buf[p->read_pos % PIPE_BUF_SIZE];
                p->read_pos++;
            }
            p->avail -= n;
            spinlock_unlock(&p->lock);
            wq_wake_one(p->wq);
            return (ssize_t)n;
        }
        if (p->write_closed) {
            spinlock_unlock(&p->lock);
            return 0;
        }
        spinlock_unlock(&p->lock);
        wq_wait(p->rq);
        if (g_current->killed) return -EINTR;
    }
}

static ssize_t pipe_write_fn(struct file *f, const void *buf, size_t len)
{
    pipe_t *p = (pipe_t *)f->private;
    if (!p) return -EBADF;
    if (p->read_closed) {
        signal_send(g_current, SIGPIPE, NULL);
        return -EPIPE;
    }

    size_t written = 0;
    const uint8_t *src = (const uint8_t *)buf;

    while (written < len) {
        spinlock_lock(&p->lock);
        size_t space = PIPE_BUF_SIZE - p->avail;
        if (space > 0) {
            size_t n = MIN(len - written, space);
            for (size_t i = 0; i < n; i++) {
                p->buf[(p->write_pos) % PIPE_BUF_SIZE] = src[written + i];
                p->write_pos++;
            }
            p->avail += n;
            written  += n;
            spinlock_unlock(&p->lock);
            wq_wake_one(p->rq);
        } else {
            spinlock_unlock(&p->lock);
            wq_wait(p->wq);
            if (g_current->killed) return written ? (ssize_t)written : -EINTR;
        }
    }
    return (ssize_t)written;
}

static int pipe_close_read(struct file *f)
{
    pipe_t *p = (pipe_t *)f->private;
    if (p) { p->read_closed = true; wq_wake_all(p->wq); }
    if (atomic_dec_and_test(&p->refcount)) {
        wq_destroy(p->rq); wq_destroy(p->wq); kfree(p);
    }
    return 0;
}

static int pipe_close_write(struct file *f)
{
    pipe_t *p = (pipe_t *)f->private;
    if (p) { p->write_closed = true; wq_wake_all(p->rq); }
    if (atomic_dec_and_test(&p->refcount)) {
        wq_destroy(p->rq); wq_destroy(p->wq); kfree(p);
    }
    return 0;
}

static const struct file_ops g_pipe_read_ops  = { .read  = pipe_read_fn,  .close = pipe_close_read  };
static const struct file_ops g_pipe_write_ops = { .write = pipe_write_fn, .close = pipe_close_write };

int pipe_create(file_t **read_end, file_t **write_end)
{
    pipe_t *p = kzalloc(sizeof(pipe_t));
    if (!p) return -ENOMEM;

    p->rq = wq_create();
    p->wq = wq_create();
    spinlock_init(&p->lock);
    atomic_set(&p->refcount, 2);

    file_t *rf = kzalloc(sizeof(file_t));
    file_t *wf = kzalloc(sizeof(file_t));
    if (!rf || !wf) { kfree(rf); kfree(wf); kfree(p); return -ENOMEM; }

    rf->ops     = &g_pipe_read_ops;
    rf->private = p;
    rf->flags   = O_RDONLY;
    atomic_set(&rf->refcount, 1);
    spinlock_init(&rf->lock);

    wf->ops     = &g_pipe_write_ops;
    wf->private = p;
    wf->flags   = O_WRONLY;
    atomic_set(&wf->refcount, 1);
    spinlock_init(&wf->lock);

    *read_end  = rf;
    *write_end = wf;
    return 0;
}

/* ============================================================
 * TTY (簡易シリアルコンソール)
 * ============================================================ */
static ssize_t tty_read(vnode_t *vn, void *buf, size_t len, off_t off)
{
    UNUSED(vn); UNUSED(off);
    /* COM1からブロッキング読み取り */
    uint8_t *p = (uint8_t *)buf;
    size_t i;
    for (i = 0; i < len; i++) {
        /* 受信待ち */
        while (!(inb(COM1_BASE + 5) & 1)) {
            sched_yield();
            if (g_current && g_current->killed) return i ? (ssize_t)i : -EINTR;
        }
        uint8_t c = inb(COM1_DATA);
        if (c == '\r') c = '\n';
        p[i] = c;
        /* エコー */
        serial_putc((char)c);
        if (c == '\n') { i++; break; }
    }
    return (ssize_t)i;
}

static ssize_t tty_write(vnode_t *vn, const void *buf, size_t len, off_t off)
{
    UNUSED(vn); UNUSED(off);
    const char *p = (const char *)buf;
    for (size_t i = 0; i < len; i++) {
        serial_putc(p[i]);
        vga_putc(p[i]);
    }
    return (ssize_t)len;
}

static int tty_ioctl(vnode_t *vn, unsigned long req, void *arg)
{
    UNUSED(vn);
    switch (req) {
    case TIOCGWINSZ: {
        struct winsize ws = { .ws_row = VGA_ROWS, .ws_col = VGA_COLS };
        if (arg) memcpy(arg, &ws, sizeof(ws));
        return 0;
    }
    case TCGETS: {
        if (arg) {
            struct termios t;
            memset(&t, 0, sizeof(t));
            t.c_iflag = ICRNL;
            t.c_oflag = OPOST | ONLCR;
            t.c_cflag = 0xBF;
            t.c_lflag = ICANON | ECHO | ECHOE | ECHOK | ISIG;
            t.c_cc[VINTR]  = 3;
            t.c_cc[VQUIT]  = 28;
            t.c_cc[VERASE] = 127;
            t.c_cc[VKILL]  = 21;
            t.c_cc[VEOF]   = 4;
            t.c_cc[VMIN]   = 1;
            memcpy(arg, &t, sizeof(t));
        }
        return 0;
    }
    case TCSETS: case TCSETSW: case TCSETSF: return 0;
    case TIOCGPGRP: if (arg) *(pid_t*)arg = g_current ? g_current->pgid : 0; return 0;
    case TIOCSPGRP: return 0;
    default: return -ENOTTY;
    }
}

static const struct vnode_ops g_tty_ops = {
    .read  = tty_read,
    .write = tty_write,
    .ioctl = tty_ioctl,
};

static vnode_t g_tty_vnode;

void tty_init(void)
{
    memset(&g_tty_vnode, 0, sizeof(g_tty_vnode));
    g_tty_vnode.type = VN_CHR;
    g_tty_vnode.mode = S_IFCHR | 0666;
    g_tty_vnode.ops  = &g_tty_ops;
    atomic_set(&g_tty_vnode.refcount, 1);
    spinlock_init(&g_tty_vnode.lock);
}

vnode_t *tty_get_console(void) { return &g_tty_vnode; }

/* ============================================================
 * kernel_main_thread - init プロセス起動
 * ============================================================ */
void kernel_main_thread(void)
{
    printk(KERN_INFO "Kernel: starting init process\n");

    /* /sbin/init または /init を実行 */
    static const char *init_paths[] = {
        "/sbin/init", "/init", "/bin/init", "/bin/sh", NULL
    };

    process_t *init = proc_create("init");
    if (!init) PANIC("Failed to create init process");

    g_init_proc = init;
    g_current   = init;
    init->state = PROC_RUNNING;
    init->pid   = 1;

    /* stdin / stdout / stderr = /dev/console */
    file_t *con = file_alloc(&g_tty_vnode, O_RDWR);
    if (con) {
        fd_alloc_at(init->fd_table, 0, con);  /* stdin  */
        fd_alloc_at(init->fd_table, 1, con);  /* stdout */
        fd_alloc_at(init->fd_table, 2, con);  /* stderr */
        file_put(con);
    }

    /* init 引数 */
    char *argv[] = { (char*)"/sbin/init", NULL };
    char *envp[] = {
        (char*)"PATH=/bin:/sbin:/usr/bin:/usr/sbin",
        (char*)"HOME=/root",
        (char*)"TERM=linux",
        (char*)"SHELL=/bin/sh",
        (char*)"USER=root",
        (char*)"LOGNAME=root",
        NULL
    };

    /* init パスを順に試す */
    for (int i = 0; init_paths[i]; i++) {
        argv[0] = (char*)init_paths[i];
        int r = proc_exec(init, init_paths[i], argv, envp);
        if (r == 0) break;
        printk(KERN_WARNING "init: %s not found (%d)\n", init_paths[i], r);
    }

    PANIC("No init found! Create /sbin/init, /init, or /bin/sh in initrd.");
}

/* ============================================================
 * kernel_entry - C言語エントリポイント
 * boot.asm から呼ばれる
 * ============================================================ */
void kernel_entry(uint32_t magic, uint32_t mbi_phys)
{
    /* 早期出力 */
    serial_init();
    vga_clear();

    printk("\n");
    printk("  ██╗   ██╗███╗   ██╗██╗██╗  ██╗    ██╗  ██╗███████╗██████╗ ███╗   ██╗███████╗██╗\n");
    printk("  ██║   ██║████╗  ██║██║╚██╗██╔╝    ██║ ██╔╝██╔════╝██╔══██╗████╗  ██║██╔════╝██║\n");
    printk("  ██║   ██║██╔██╗ ██║██║ ╚███╔╝     █████╔╝ █████╗  ██████╔╝██╔██╗ ██║█████╗  ██║\n");
    printk("  ██║   ██║██║╚██╗██║██║ ██╔██╗     ██╔═██╗ ██╔══╝  ██╔══██╗██║╚██╗██║██╔══╝  ██║\n");
    printk("  ╚██████╔╝██║ ╚████║██║██╔╝ ██╗    ██║  ██╗███████╗██║  ██║██║ ╚████║███████╗███████╗\n");
    printk("   ╚═════╝ ╚═╝  ╚═══╝╚═╝╚═╝  ╚═╝   ╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝╚═╝  ╚═══╝╚══════╝╚══════╝\n");
    printk("\n  Unix互換カーネル v0.1.0 (x86_64)  - Copyright (C) 2024\n\n");

    /* Multiboot2 検証 */
    if (magic != MULTIBOOT2_MAGIC)
        PANIC("Invalid Multiboot2 magic: 0x%x (expected 0x%x)", magic, MULTIBOOT2_MAGIC);

    struct multiboot2_info *mbi =
        (struct multiboot2_info *)(uintptr_t)(mbi_phys + KERNEL_VMA_BASE);

    printk(KERN_INFO "Boot: Multiboot2 OK, info at 0x%p\n", mbi);

    /* === アーキテクチャ初期化 === */
    printk(KERN_INFO "Arch: initializing GDT/IDT...\n");
    arch_init_early();

    /* === メモリ管理初期化 === */
    printk(KERN_INFO "MM: initializing physical memory...\n");
    extern void pmm_init_from_multiboot(struct multiboot2_info *);
    pmm_init_from_multiboot(mbi);

    printk(KERN_INFO "MM: initializing virtual memory...\n");
    vmm_init();

    printk(KERN_INFO "MM: initializing heap...\n");
    heap_init();

    /* === デバイス/FS初期化 === */
    printk(KERN_INFO "TTY: initializing console...\n");
    tty_init();

    printk(KERN_INFO "VFS: initializing...\n");
    vfs_init();
    devfs_init();

    /* initrd を探してマウント */
    struct mb2_tag_module *mod_tag =
        (struct mb2_tag_module *)mb2_find_tag(mbi, MB2_TAG_MODULE);

    if (!mod_tag)
        PANIC("No initrd module found! Pass initrd to GRUB.");

    uintptr_t initrd_phys  = mod_tag->mod_start;
    uintptr_t initrd_end   = mod_tag->mod_end;
    size_t    initrd_size  = initrd_end - initrd_phys;

    printk(KERN_INFO "initrd: at phys=0x%08llx size=%zu KB cmd='%s'\n",
           (unsigned long long)initrd_phys, initrd_size >> 10,
           mod_tag->cmdline);

    uintptr_t initrd_virt = (uintptr_t)phys_to_kvirt(initrd_phys);
    initrd_init(initrd_virt, initrd_size);

    /* / を initrd でマウント */
    vfs_mount(NULL, "/", "initrd", 0);

    /* /dev を devfs でマウント */
    vfs_mount(NULL, "/dev", "devfs", 0);

    /* /tmp を tmpfs でマウント */
    tmpfs_get_fs();
    vfs_mount(NULL, "/tmp", "tmpfs", 0);

    /* /proc は後で実装 */

    /* === プロセス管理初期化 === */
    printk(KERN_INFO "Proc: initializing...\n");
    proc_init();
    sched_init();

    /* === システムコール初期化 === */
    printk(KERN_INFO "Syscall: initializing...\n");
    syscall_init();

    /* === 遅延アーキテクチャ初期化 (割り込み有効化) === */
    printk(KERN_INFO "Arch: finalizing (enabling interrupts)...\n");
    arch_init_late();

    printk(KERN_INFO "=== Kernel initialized successfully ===\n");
    printk(KERN_INFO "Memory: %zu MB total, %zu MB free\n",
           (pmm_total_pages() * PAGE_SIZE) >> 20,
           (pmm_free_pages() * PAGE_SIZE) >> 20);

    /* init プロセスを起動 */
    kernel_main_thread();

    /* ここには到達しない */
    cpu_halt();
}

/* リンカスクリプト用シンボル (弱参照) */
uint8_t _kernel_start[0] SECTION(".text") WEAK;
uint8_t _kernel_end[0]   SECTION(".bss")  WEAK;
