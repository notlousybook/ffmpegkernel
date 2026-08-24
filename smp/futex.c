/* smp/futex.c - address-keyed wait buckets + park/wake on core.c parking.
 *
 * Park protocol: waiter enqueues itself under the bucket lock, marks
 * WAITING, then switches away.  A woken-before-sleeping thread simply
 * resumes itself (switch-away rechecks its own state), so wakeups are
 * never lost.
 */
#include <stdint.h>
#include <stddef.h>
#include "kern.h"

#define FTX_BUCKETS 64
static struct kthread *ftx_bucket[FTX_BUCKETS];
static volatile unsigned long ftx_lock;

static void sl_lock(volatile unsigned long *l)
{
    for (;;) {
        unsigned long expected = 0;
        if (__atomic_compare_exchange_n(l, &expected, 1ul, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
        cpu_relax();
    }
}
static void sl_unlock(volatile unsigned long *l)
{ __atomic_store_n(l, 0ul, __ATOMIC_RELEASE); }

void sched_make_runnable(struct kthread *t);
static void kick_cpus(void)
{
    extern void ipi_all_excl_self(unsigned cmd);
    ipi_all_excl_self(1u << 14);
}

static unsigned ftx_hash(const void *key)
{ return ((unsigned)(unsigned long)key >> 4) & (FTX_BUCKETS - 1); }

void futex_wait(void *key)
{
    struct kthread *me = gs_cur();
    sl_lock(&ftx_lock);
    me->state = TH_WAITING;
    unsigned b = ftx_hash(key);
    me->wk_next = ftx_bucket[b];
    ftx_bucket[b] = me;
    sl_unlock(&ftx_lock);
    sched_switch_away();
}

int futex_wake(void *key, int n)
{
    int woke = 0;
    struct kthread *list = 0;
    sl_lock(&ftx_lock);
    unsigned b = ftx_hash(key);
    while (n-- > 0 && ftx_bucket[b]) {
        struct kthread *t = ftx_bucket[b];
        ftx_bucket[b] = t->wk_next;
        t->wk_next = list;
        list = t;
        woke++;
    }
    sl_unlock(&ftx_lock);
    while (list) {
        struct kthread *nx = list->wk_next;
        sched_make_runnable(list);
        list = nx;
    }
    if (woke) kick_cpus();
    return woke;
}

/* ---- shared spinlock used by pthread layer ------------------------------- */
void kftx_lock(void) { sl_lock(&ftx_lock); }
void kftx_unlock(void) { sl_unlock(&ftx_lock); }

/* ---- join registration with proper ordering ------------------------------- */
/* Returns 1 if target already DONE; else registers me as its single
 * joiner (caller then parks on &t->joiner via futex_wait).                  */
extern volatile unsigned long kth_sched_lock;
int kth_try_join(struct kthread *me, struct kthread *t)
{
    extern void kcore_lock(void), kcore_unlock(void);
    kcore_lock();
    int done = (t->state == TH_DONE);
    if (!done && !t->joiner) t->joiner = me;
    else if (!done) { kcore_unlock(); return -1; }   /* second joiner       */
    kcore_unlock();
    return done;
}
