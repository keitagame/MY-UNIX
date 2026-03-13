#pragma once
#ifndef _KERNEL_TYPES_H
#define _KERNEL_TYPES_H

/* ============================================================
 * types.h - 基本型定義
 * Unix互換カーネル
 * ============================================================ */

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;

typedef uint64_t uintptr_t;
typedef int64_t  intptr_t;
typedef uint64_t size_t;
typedef int64_t  ssize_t;
typedef int64_t  off_t;
typedef uint64_t ino_t;
typedef uint32_t mode_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef int32_t  pid_t;
typedef int32_t  tid_t;
typedef uint64_t dev_t;
typedef uint64_t nlink_t;
typedef uint64_t blksize_t;
typedef uint64_t blkcnt_t;
typedef int64_t  time_t;
typedef int64_t  suseconds_t;

/* NULL定義 */
#ifndef NULL
#define NULL ((void*)0)
#endif

/* bool型 */
typedef _Bool bool;
#define true  1
#define false 0

/* 最大値・最小値 */
#define UINT8_MAX   0xFF
#define UINT16_MAX  0xFFFF
#define UINT32_MAX  0xFFFFFFFFU
#define UINT64_MAX  0xFFFFFFFFFFFFFFFFULL
#define INT8_MAX    0x7F
#define INT16_MAX   0x7FFF
#define INT32_MAX   0x7FFFFFFF
#define INT64_MAX   0x7FFFFFFFFFFFFFFFLL
#define INT8_MIN    (-0x80)
#define INT16_MIN   (-0x8000)
#define INT32_MIN   (-0x80000000)
#define INT64_MIN   (-0x8000000000000000LL)
#define SIZE_MAX    UINT64_MAX

/* アライメントマクロ */
#define ALIGN_UP(x, a)    (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a)  ((x) & ~((a) - 1))
#define IS_ALIGNED(x, a)  (((x) & ((a) - 1)) == 0)

/* ビット操作マクロ */
#define BIT(n)            (1ULL << (n))
#define SET_BIT(x, n)     ((x) |= BIT(n))
#define CLR_BIT(x, n)     ((x) &= ~BIT(n))
#define TST_BIT(x, n)     (!!((x) & BIT(n)))

/* コンパイラヒント */
#define LIKELY(x)     __builtin_expect(!!(x), 1)
#define UNLIKELY(x)   __builtin_expect(!!(x), 0)
#define UNUSED(x)     ((void)(x))
#define PACKED        __attribute__((packed))
#define NORETURN      __attribute__((noreturn))
#define NOINLINE      __attribute__((noinline))
#define ALWAYS_INLINE __attribute__((always_inline)) inline
#define WEAK          __attribute__((weak))
#define SECTION(s)    __attribute__((section(s)))

/* カーネルアドレス定数 */
#define KERNEL_VMA_BASE   0xFFFFFFFF80000000ULL  /* カーネル仮想ベースアドレス */
#define KERNEL_PHYS_BASE  0x0000000000100000ULL  /* カーネル物理ベースアドレス (1MB) */
#define PAGE_SIZE         4096ULL
#define PAGE_SHIFT        12
#define PAGE_MASK         (~(PAGE_SIZE - 1))

/* 物理/仮想アドレス変換 */
#define PHYS_TO_VIRT(p)   ((void*)((uintptr_t)(p) + KERNEL_VMA_BASE))
#define VIRT_TO_PHYS(v)   ((uintptr_t)(v) - KERNEL_VMA_BASE)

/* コンテナof マクロ */
#define container_of(ptr, type, member) \
    ((type*)((char*)(ptr) - __builtin_offsetof(type, member)))

/* offsetof */
#define offsetof(type, member) __builtin_offsetof(type, member)

/* 最小・最大マクロ */
#define MIN(a, b)  ((a) < (b) ? (a) : (b))
#define MAX(a, b)  ((a) > (b) ? (a) : (b))

/* 配列サイズ */
#define ARRAY_SIZE(arr)  (sizeof(arr) / sizeof((arr)[0]))

/* バリア */
#define barrier()         __asm__ volatile("" ::: "memory")
#define mb()              __asm__ volatile("mfence" ::: "memory")
#define rmb()             __asm__ volatile("lfence" ::: "memory")
#define wmb()             __asm__ volatile("sfence" ::: "memory")

/* I/Oポートアクセス */
static ALWAYS_INLINE void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
static ALWAYS_INLINE uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static ALWAYS_INLINE void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" :: "a"(val), "Nd"(port));
}
static ALWAYS_INLINE uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static ALWAYS_INLINE void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" :: "a"(val), "Nd"(port));
}
static ALWAYS_INLINE uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* io_wait */
static ALWAYS_INLINE void io_wait(void) {
    outb(0x80, 0);
}

/* MSR読み書き */
static ALWAYS_INLINE uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static ALWAYS_INLINE void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

/* CR読み書き */
static ALWAYS_INLINE uint64_t read_cr0(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr0, %0" : "=r"(val));
    return val;
}
static ALWAYS_INLINE void write_cr0(uint64_t val) {
    __asm__ volatile("mov %0, %%cr0" :: "r"(val));
}
static ALWAYS_INLINE uint64_t read_cr2(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr2, %0" : "=r"(val));
    return val;
}
static ALWAYS_INLINE uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}
static ALWAYS_INLINE void write_cr3(uint64_t val) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(val) : "memory");
}
static ALWAYS_INLINE uint64_t read_cr4(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr4, %0" : "=r"(val));
    return val;
}
static ALWAYS_INLINE void write_cr4(uint64_t val) {
    __asm__ volatile("mov %0, %%cr4" :: "r"(val));
}

/* TLBフラッシュ */
static ALWAYS_INLINE void invlpg(uintptr_t addr) {
    __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
}
static ALWAYS_INLINE void flush_tlb(void) {
    write_cr3(read_cr3());
}

/* 割り込み制御 */
static ALWAYS_INLINE void sti(void) { __asm__ volatile("sti"); }
static ALWAYS_INLINE void cli(void) { __asm__ volatile("cli"); }
static ALWAYS_INLINE void hlt(void) { __asm__ volatile("hlt"); }
static ALWAYS_INLINE uint64_t read_rflags(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    return flags;
}

/* CPU停止 */
static ALWAYS_INLINE NORETURN void cpu_halt(void) {
    cli();
    for(;;) hlt();
}

/* RFLAGS IFビット */
#define RFLAGS_IF   BIT(9)

/* 割り込み保存/復元 */
#define IRQ_SAVE(flags) \
    do { (flags) = read_rflags(); cli(); } while(0)
#define IRQ_RESTORE(flags) \
    do { if ((flags) & RFLAGS_IF) sti(); } while(0)

/* タイムスタンプカウンタ */
static ALWAYS_INLINE uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* POSIXタイムスタンプ構造体 */
struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

struct timeval {
    time_t      tv_sec;
    suseconds_t tv_usec;
};

/* statバッファ */
struct stat {
    dev_t     st_dev;
    ino_t     st_ino;
    mode_t    st_mode;
    nlink_t   st_nlink;
    uid_t     st_uid;
    gid_t     st_gid;
    dev_t     st_rdev;
    off_t     st_size;
    blksize_t st_blksize;
    blkcnt_t  st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
};

/* ファイル種別ビット */
#define S_IFMT    0170000
#define S_IFSOCK  0140000
#define S_IFLNK   0120000
#define S_IFREG   0100000
#define S_IFBLK   0060000
#define S_IFDIR   0040000
#define S_IFCHR   0020000
#define S_IFIFO   0010000

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)

/* パーミッションビット */
#define S_ISUID  04000
#define S_ISGID  02000
#define S_ISVTX  01000
#define S_IRWXU  0700
#define S_IRUSR  0400
#define S_IWUSR  0200
#define S_IXUSR  0100
#define S_IRWXG  070
#define S_IRGRP  040
#define S_IWGRP  020
#define S_IXGRP  010
#define S_IRWXO  07
#define S_IROTH  04
#define S_IWOTH  02
#define S_IXOTH  01

/* open フラグ */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_ACCMODE   3
#define O_CREAT     0x40
#define O_EXCL      0x80
#define O_NOCTTY    0x100
#define O_TRUNC     0x200
#define O_APPEND    0x400
#define O_NONBLOCK  0x800
#define O_CLOEXEC   0x80000
#define O_DIRECTORY 0x10000
#define O_NOFOLLOW  0x20000

/* lseek whence */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* mmap prot */
#define PROT_NONE   0
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

/* mmap flags */
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED     0x10
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FAILED    ((void*)-1)

/* ファイル記述子制限 */
#define FD_MAX        1024

/* パス最大長 */
#define PATH_MAX      4096
#define NAME_MAX      255

/* エラーコード */
#define EPERM    1
#define ENOENT   2
#define ESRCH    3
#define EINTR    4
#define EIO      5
#define ENXIO    6
#define E2BIG    7
#define ENOEXEC  8
#define EBADF    9
#define ECHILD   10
#define EAGAIN   11
#define ENOMEM   12
#define EACCES   13
#define EFAULT   14
#define EBUSY    16
#define EEXIST   17
#define EXDEV    18
#define ENODEV   19
#define ENOTDIR  20
#define EISDIR   21
#define EINVAL   22
#define ENFILE   23
#define EMFILE   24
#define ENOTTY   25
#define EFBIG    27
#define ENOSPC   28
#define ESPIPE   29
#define EROFS    30
#define EMLINK   31
#define EPIPE    32
#define ERANGE   34
#define EDEADLK  35
#define ENAMETOOLONG 36
#define ENOSYS   38
#define ENOTEMPTY 39
#define ELOOP    40
#define EWOULDBLOCK EAGAIN
#define ENOMSG   42
#define ENODATA  61
#define EOVERFLOW 75
#define EILSEQ   84
#define ENOTSUP  95
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define ECONNREFUSED 111
#define ETIMEDOUT 110
#define ENOTCONN  107
#define EPROTO   71
#define EPROTONOSUPPORT 93

#endif /* _KERNEL_TYPES_H */
