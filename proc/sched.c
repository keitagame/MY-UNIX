/* ============================================================
 * sched.c - スケジューラ (優先度付きラウンドロビン)
 * ============================================================ */

#include "../include/types.h"
#include "../include/kernel.h"
#include "../include/mm.h"
#include "../include/process.h"

/* ============================================================
 * 優先度キュー
 * 40 レベル (0〜39): 0 = 最高優先度
 * ============================================================ */
#define SCHED_PRIO_MAX    40
#define SCHED_PRIO_USER   20   /* 通常ユーザーのデフォルト */
#define SCHED_TIMESLICE   (HZ / 10)  /* 100ms */

typedef struct {
    struct list_head queues[SCHED_PRIO_MAX];
    uint64_t         bitmap;   /* ビットマップ (bit N=1: queue[N]に要素あり) */
    size_t           total;
} run_queue_t;

static run_queue_t g_runq;
static spinlock_t  g_sched_lock = SPINLOCK_INIT;

/* アイドルプロセス */
static process_t *g_idle_proc = NULL;

/* スケジューラ統計 */
static uint64_t g_sched_count  = 0;
static uint64_t g_context_switches = 0;

/* ============================================================
 * ウェイトキュー実装
 * ============================================================ */
struct wait_queue_node {
    process_t        *proc;
    struct list_head  list;
};

struct wait_queue_head {
    struct list_head list;
    spinlock_t       lock;
};

wait_queue_t *wq_create(void)
{
    wait_queue_t *wq = kzalloc(sizeof(wait_queue_t));
    if (!wq) return NULL;
    INIT_LIST_HEAD(&wq->list);
    spinlock_init(&wq->lock);
    return wq;
}

void wq_destroy(wait_queue_t *wq)
{
    kfree(wq);
}

void wq_wait(wait_queue_t *wq)
{
    struct wait_queue_node node;
    node.proc = g_current;
    INIT_LIST_HEAD(&node.list);

    spinlock_lock(&wq->lock);
    list_add_tail(&node.list, &wq->list);
    g_current->state = PROC_SLEEPING;
    spinlock_unlock(&wq->lock);

    schedule();

    spinlock_lock(&wq->lock);
    list_del(&node.list);
    spinlock_unlock(&wq->lock);
}

int wq_wait_timeout(wait_queue_t *wq, uint64_t ms)
{
    struct wait_queue_node node;
    node.proc = g_current;
    INIT_LIST_HEAD(&node.list);

    spinlock_lock(&wq->lock);
    list_add_tail(&node.list, &wq->list);
    g_current->state = PROC_SLEEPING;
    spinlock_unlock(&wq->lock);

    uint64_t deadline = get_time_ms() + ms;

    while (g_current->state == PROC_SLEEPING) {
        if (get_time_ms() >= deadline) {
            spinlock_lock(&wq->lock);
            list_del(&node.list);
            g_current->state = PROC_RUNNING;
            spinlock_unlock(&wq->lock);
            return -ETIMEDOUT;
        }
        schedule();
    }

    spinlock_lock(&wq->lock);
    list_del(&node.list);
    spinlock_unlock(&wq->lock);
    return 0;
}

void wq_wake_one(wait_queue_t *wq)
{
    spinlock_lock(&wq->lock);
    if (!list_empty(&wq->list)) {
        struct wait_queue_node *node =
            list_entry(wq->list.next, struct wait_queue_node, list);
        process_t *proc = node->proc;
        list_del(&node->list);
        spinlock_unlock(&wq->lock);
        proc->state = PROC_READY;
        sched_add(proc);
        return;
    }
    spinlock_unlock(&wq->lock);
}

void wq_wake_all(wait_queue_t *wq)
{
    spinlock_lock(&wq->lock);
    struct list_head *pos, *n;
    list_for_each_safe(pos, n, &wq->list) {
        struct wait_queue_node *node = list_entry(pos, struct wait_queue_node, list);
        process_t *proc = node->proc;
        list_del(&node->list);
        proc->state = PROC_READY;
        sched_add(proc);
    }
    spinlock_unlock(&wq->lock);
}

/* ============================================================
 * runqueue 操作
 * ============================================================ */
static void runq_push(run_queue_t *rq, process_t *proc)
{
    int prio = proc->priority;
    if (prio < 0) prio = 0;
    if (prio >= SCHED_PRIO_MAX) prio = SCHED_PRIO_MAX - 1;

    list_add_tail(&proc->run_list, &rq->queues[prio]);
    rq->bitmap |= BIT(prio);
    rq->total++;
}

static process_t *runq_pop_best(run_queue_t *rq)
{
    if (rq->total == 0) return NULL;

    int prio = __builtin_ctzll(rq->bitmap);
    if (prio >= SCHED_PRIO_MAX) return NULL;

    struct list_head *head = &rq->queues[prio];
    if (list_empty(head)) {
        rq->bitmap &= ~BIT(prio);
        return NULL;
    }

    struct list_head *first = head->next;
    process_t *proc = list_entry(first, process_t, run_list);
    list_del(&proc->run_list);
    INIT_LIST_HEAD(&proc->run_list);

    if (list_empty(head)) rq->bitmap &= ~BIT(prio);
    rq->total--;
    return proc;
}

static void runq_remove(run_queue_t *rq, process_t *proc)
{
    if (list_empty(&proc->run_list)) return;
    list_del(&proc->run_list);
    INIT_LIST_HEAD(&proc->run_list);
    if (rq->total > 0) rq->total--;

    /* ビットマップ更新 */
    int prio = proc->priority;
    if (prio >= 0 && prio < SCHED_PRIO_MAX) {
        if (list_empty(&rq->queues[prio]))
            rq->bitmap &= ~BIT(prio);
    }
}

/* ============================================================
 * スケジューラ初期化
 * ============================================================ */
void sched_init(void)
{
    for (int i = 0; i < SCHED_PRIO_MAX; i++)
        INIT_LIST_HEAD(&g_runq.queues[i]);
    g_runq.bitmap = 0;
    g_runq.total  = 0;

    /* アイドルプロセス作成 */
    g_idle_proc = proc_create("idle");
    KASSERT(g_idle_proc, "Failed to create idle process");
    g_idle_proc->priority = SCHED_PRIO_MAX - 1;  /* 最低優先度 */
    g_idle_proc->state    = PROC_READY;

    printk(KERN_INFO "Sched: initialized (prio=%d levels)\n", SCHED_PRIO_MAX);
}

/* ============================================================
 * API
 * ============================================================ */
void sched_add(process_t *proc)
{
    if (!proc) return;
    spinlock_lock(&g_sched_lock);
    if (proc->state == PROC_READY || proc->state == PROC_RUNNING) {
        runq_push(&g_runq, proc);
    }
    spinlock_unlock(&g_sched_lock);
}

void sched_remove(process_t *proc)
{
    if (!proc) return;
    spinlock_lock(&g_sched_lock);
    runq_remove(&g_runq, proc);
    spinlock_unlock(&g_sched_lock);
}

void sched_yield(void)
{
    g_current->time_slice = 0;
    schedule();
}

/* ============================================================
 * schedule - 次に実行するプロセスを選択してコンテキストスイッチ
 * ============================================================ */
void schedule(void)
{
    uint64_t irq_flags;
    IRQ_SAVE(irq_flags);
    spinlock_lock(&g_sched_lock);

    process_t *prev = g_current;
    process_t *next = NULL;

    /* 実行可能なプロセスを取り出す */
    next = runq_pop_best(&g_runq);

    if (!next) {
        /* アイドルプロセス */
        next = g_idle_proc;
    }

    if (next == prev) {
        spinlock_unlock(&g_sched_lock);
        IRQ_RESTORE(irq_flags);
        return;
    }

    /* タイムスライスリセット */
    next->time_slice  = SCHED_TIMESLICE;
    next->state       = PROC_RUNNING;
    next->sched_time  = g_ticks;

    /* 現在のプロセスをrunqueueに戻す */
    if (prev && prev->state == PROC_RUNNING) {
        prev->state = PROC_READY;
        runq_push(&g_runq, prev);
    }

    g_current = next;
    g_context_switches++;
    g_sched_count++;

    spinlock_unlock(&g_sched_lock);

    /* TSSのRSP0を更新 */
    extern void tss_set_rsp0(uint64_t rsp0);
    tss_set_rsp0(next->tss_rsp0);

    /* アドレス空間切り替え */
    if (prev && prev->vm_space != next->vm_space) {
        vmm_switch_space(next->vm_space);
    }

    /* per-CPUのカーネルスタックを更新 */
    extern void per_cpu_set_kernel_stack(uint64_t rsp);
    per_cpu_set_kernel_stack(next->tss_rsp0);

    /* コンテキストスイッチ */
    if (prev) {
        context_switch(&prev->ctx, &next->ctx);
    } else {
        /* 初回: 直接ジャンプ */
        thread_context_t dummy;
        context_switch(&dummy, &next->ctx);
    }

    IRQ_RESTORE(irq_flags);
}

/* ============================================================
 * sched_tick - タイマー割り込みから呼ばれる
 * ============================================================ */
void sched_tick(void)
{
    process_t *proc = g_current;
    if (!proc) return;

    proc->total_time++;

    if (proc->time_slice > 0)
        proc->time_slice--;

    /* アラーム処理 */
    if (proc->alarm_time && get_time_ms() >= proc->alarm_time) {
        proc->alarm_time = 0;
        signal_send(proc, SIGALRM, NULL);
    }
}

/* ============================================================
 * アイドルループ
 * ============================================================ */
void idle_thread(void)
{
    for (;;) {
        /* 割り込みを待機 */
        sti();
        hlt();
    }
}

/* ============================================================
 * タイム関連
 * ============================================================ */
volatile uint64_t g_ticks = 0;  /* PIT タイマー割り込み毎にインクリメント */

uint64_t get_time_ms(void)
{
    return g_ticks * (1000 / HZ);
}

void timer_sleep(uint64_t ms)
{
    uint64_t deadline = get_time_ms() + ms;
    while (get_time_ms() < deadline) {
        sched_yield();
    }
}
