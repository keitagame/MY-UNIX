#pragma once
#ifndef _KERNEL_H
#define _KERNEL_H

/* ============================================================
 * kernel.h - カーネルメインヘッダ
 * ============================================================ */

#include "types.h"

/* ============================================================
 * カーネルパニック・アサート
 * ============================================================ */
NORETURN void panic(const char *fmt, ...);
NORETURN void panic_at(const char *file, int line, const char *func, const char *fmt, ...);

#define PANIC(fmt, ...) \
    panic_at(__FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define ASSERT(cond) \
    do { \
        if (UNLIKELY(!(cond))) \
            panic_at(__FILE__, __LINE__, __func__, "Assertion failed: " #cond); \
    } while(0)

#define KASSERT(cond, fmt, ...) \
    do { \
        if (UNLIKELY(!(cond))) \
            panic_at(__FILE__, __LINE__, __func__, "Assertion failed: " #cond ": " fmt, ##__VA_ARGS__); \
    } while(0)

/* ============================================================
 * printk - カーネルログ出力
 * ============================================================ */
int printk(const char *fmt, ...);
int vprintk(const char *fmt, __builtin_va_list args);

/* ログレベル */
#define KERN_EMERG   "[EMERG] "
#define KERN_ALERT   "[ALERT] "
#define KERN_CRIT    "[CRIT]  "
#define KERN_ERR     "[ERROR] "
#define KERN_WARNING "[WARN]  "
#define KERN_NOTICE  "[NOTE]  "
#define KERN_INFO    "[INFO]  "
#define KERN_DEBUG   "[DEBUG] "

/* ============================================================
 * 文字列操作 (libc非依存)
 * ============================================================ */
size_t  strlen(const char *s);
int     strcmp(const char *a, const char *b);
int     strncmp(const char *a, const char *b, size_t n);
char   *strcpy(char *dst, const char *src);
char   *strncpy(char *dst, const char *src, size_t n);
char   *strcat(char *dst, const char *src);
char   *strncat(char *dst, const char *src, size_t n);
char   *strchr(const char *s, int c);
char   *strrchr(const char *s, int c);
char   *strstr(const char *haystack, const char *needle);
char   *strdup_k(const char *s);       /* カーネル内strdup */

void   *memset(void *dst, int c, size_t n);
void   *memcpy(void *dst, const void *src, size_t n);
void   *memmove(void *dst, const void *src, size_t n);
int     memcmp(const void *a, const void *b, size_t n);
void   *memchr(const void *s, int c, size_t n);

int     snprintf(char *buf, size_t size, const char *fmt, ...);
int     vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list args);

/* ============================================================
 * アーキテクチャ初期化
 * ============================================================ */
void arch_init_early(void);   /* GDT/IDT/ページング */
void arch_init_late(void);    /* LAPIC/SMP/その他 */
void arch_init_cpu(void);     /* CPU固有機能 */

/* ============================================================
 * メモリ管理初期化
 * ============================================================ */
struct multiboot2_info;
void mm_init(struct multiboot2_info *mbi);

/* ============================================================
 * サブシステム初期化
 * ============================================================ */
void drivers_init(void);
void vfs_init(void);
void proc_init(void);
void syscall_init(void);
void timer_init(void);

/* ============================================================
 * プロセス起動
 * ============================================================ */
void kernel_main_thread(void);

/* ============================================================
 * タイマー
 * ============================================================ */
extern volatile uint64_t g_ticks;   /* システムチック数 */
uint64_t get_time_ms(void);
void timer_sleep(uint64_t ms);

/* PIT定数 */
#define HZ          1000             /* 1ms タイマー分解能 */
#define PIT_CHANNEL0  0x40
#define PIT_COMMAND   0x43

/* ============================================================
 * スピンロック
 * ============================================================ */
typedef struct {
    volatile int locked;
} spinlock_t;

#define SPINLOCK_INIT  { .locked = 0 }

static ALWAYS_INLINE void spinlock_init(spinlock_t *lock) {
    lock->locked = 0;
}
static ALWAYS_INLINE void spinlock_lock(spinlock_t *lock) {
    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}
static ALWAYS_INLINE void spinlock_unlock(spinlock_t *lock) {
    __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
}
static ALWAYS_INLINE bool spinlock_trylock(spinlock_t *lock) {
    return !__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE);
}

/* IRQセーフスピンロック */
typedef struct {
    spinlock_t lock;
    uint64_t   flags;
} irqlock_t;

static ALWAYS_INLINE void irqlock_lock(irqlock_t *l) {
    IRQ_SAVE(l->flags);
    spinlock_lock(&l->lock);
}
static ALWAYS_INLINE void irqlock_unlock(irqlock_t *l) {
    spinlock_unlock(&l->lock);
    IRQ_RESTORE(l->flags);
}

/* ============================================================
 * ウェイトキュー
 * ============================================================ */
struct wait_queue_head;
typedef struct wait_queue_head wait_queue_t;

wait_queue_t *wq_create(void);
void wq_destroy(wait_queue_t *wq);
void wq_wait(wait_queue_t *wq);
void wq_wake_one(wait_queue_t *wq);
void wq_wake_all(wait_queue_t *wq);
int  wq_wait_timeout(wait_queue_t *wq, uint64_t ms);

/* ============================================================
 * アトミック操作
 * ============================================================ */
typedef struct { volatile int32_t val; } atomic_t;
typedef struct { volatile int64_t val; } atomic64_t;

#define ATOMIC_INIT(v) { .val = (v) }

static ALWAYS_INLINE int32_t atomic_read(const atomic_t *a) {
    return __atomic_load_n(&a->val, __ATOMIC_SEQ_CST);
}
static ALWAYS_INLINE void atomic_set(atomic_t *a, int32_t v) {
    __atomic_store_n(&a->val, v, __ATOMIC_SEQ_CST);
}
static ALWAYS_INLINE int32_t atomic_add_return(atomic_t *a, int32_t v) {
    return __atomic_add_fetch(&a->val, v, __ATOMIC_SEQ_CST);
}
static ALWAYS_INLINE int32_t atomic_sub_return(atomic_t *a, int32_t v) {
    return __atomic_sub_fetch(&a->val, v, __ATOMIC_SEQ_CST);
}
static ALWAYS_INLINE void atomic_inc(atomic_t *a) { atomic_add_return(a, 1); }
static ALWAYS_INLINE void atomic_dec(atomic_t *a) { atomic_sub_return(a, 1); }
static ALWAYS_INLINE int32_t atomic_inc_return(atomic_t *a) { return atomic_add_return(a, 1); }
static ALWAYS_INLINE int32_t atomic_dec_return(atomic_t *a) { return atomic_sub_return(a, 1); }
static ALWAYS_INLINE bool atomic_dec_and_test(atomic_t *a) { return atomic_sub_return(a, 1) == 0; }
static ALWAYS_INLINE bool atomic_cmpxchg(atomic_t *a, int32_t old_v, int32_t new_v) {
    return __atomic_compare_exchange_n(&a->val, &old_v, new_v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/* ============================================================
 * リンクリスト
 * ============================================================ */
struct list_head {
    struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name)      struct list_head name = LIST_HEAD_INIT(name)

static ALWAYS_INLINE void INIT_LIST_HEAD(struct list_head *list) {
    list->next = list;
    list->prev = list;
}
static ALWAYS_INLINE bool list_empty(const struct list_head *head) {
    return head->next == head;
}
static ALWAYS_INLINE void __list_add(struct list_head *new,
                                      struct list_head *prev,
                                      struct list_head *next) {
    next->prev = new;
    new->next  = next;
    new->prev  = prev;
    prev->next = new;
}
static ALWAYS_INLINE void list_add(struct list_head *new, struct list_head *head) {
    __list_add(new, head, head->next);
}
static ALWAYS_INLINE void list_add_tail(struct list_head *new, struct list_head *head) {
    __list_add(new, head->prev, head);
}
static ALWAYS_INLINE void list_del(struct list_head *entry) {
    entry->prev->next = entry->next;
    entry->next->prev = entry->prev;
    entry->next = NULL;
    entry->prev = NULL;
}
static ALWAYS_INLINE void list_move(struct list_head *list, struct list_head *head) {
    list_del(list);
    list_add(list, head);
}
static ALWAYS_INLINE void list_move_tail(struct list_head *list, struct list_head *head) {
    list_del(list);
    list_add_tail(list, head);
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)

#define list_for_each(pos, head) \
    for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

#define list_for_each_safe(pos, n, head) \
    for ((pos) = (head)->next, (n) = (pos)->next; \
         (pos) != (head); \
         (pos) = (n), (n) = (pos)->next)

#define list_for_each_entry(pos, head, member) \
    for ((pos) = list_entry((head)->next, typeof(*(pos)), member); \
         &(pos)->member != (head); \
         (pos) = list_entry((pos)->member.next, typeof(*(pos)), member))

#define list_for_each_entry_safe(pos, n, head, member) \
    for ((pos) = list_entry((head)->next, typeof(*(pos)), member), \
         (n)   = list_entry((pos)->member.next, typeof(*(pos)), member); \
         &(pos)->member != (head); \
         (pos) = (n), (n) = list_entry((n)->member.next, typeof(*(pos)), member))

/* ============================================================
 * ハッシュテーブル (シンプル実装)
 * ============================================================ */
#define HASH_BITS  8
#define HASH_SIZE  (1 << HASH_BITS)
#define HASH_MASK  (HASH_SIZE - 1)

static ALWAYS_INLINE uint32_t hash_ptr(uintptr_t ptr, unsigned bits) {
    uint64_t val = ptr;
    val ^= val >> 33;
    val *= 0xff51afd7ed558ccdULL;
    val ^= val >> 33;
    val *= 0xc4ceb9fe1a85ec53ULL;
    val ^= val >> 33;
    return (uint32_t)(val >> (64 - bits));
}

static ALWAYS_INLINE uint32_t hash_string(const char *s, unsigned bits) {
    uint32_t hash = 5381;
    while (*s) {
        hash = ((hash << 5) + hash) + (unsigned char)*s++;
    }
    return hash & ((1 << bits) - 1);
}

#endif /* _KERNEL_H */
