/* ============================================================================
 * smp/pthread_impl.c - the pthread API FFmpeg's threading layer calls,
 * implemented on futex buckets (smp/futex.c) + core.c parking.
 *
 * Types are GLIBC's opaque blobs (pulled in via <pthread.h>); at runtime
 * only this file interprets them, using the first machine word of each
 * blob as its state.
 * ==========================================================================*/
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include "kern.h"

/* glibc blobs: mutex 40B (int __lock at offset 0), cond 48B
 * (unsigned long long __wseq at offset 0), once_t int, key unsigned.      */
typedef struct { volatile int w; } kmutex_view;
typedef struct { volatile unsigned long long seq; } kcond_view;

extern void kftx_lock(void);
extern void kftx_unlock(void);
extern int  kth_try_join(struct kthread *me, struct kthread *t);

/* ---- lifecycle ----------------------------------------------------------- */
int pthread_create(pthread_t *t, const pthread_attr_t *attr,
                   void *(*fn)(void *), void *arg)
{
    (void)attr;
    unsigned long tid;
    if (kthread_spawn(fn, arg, 256u << 10, &tid)) return -1;
    if (t) *t = tid;
    return 0;
}

pthread_t pthread_self(void)
{
    struct kthread *me = gs_cur();
    return me ? me->tid : 0;
}

int pthread_join(pthread_t th, void **retval)
{
    struct kthread *me = gs_cur();
    struct kthread *t = kth_by_tid(th);
    if (!t || t == me) return -1;
    for (;;) {
        int done = kth_try_join(me, t);
        if (done < 0) return -1;               /* someone else joined       */
        if (done) break;
        futex_wait(&t->joiner);                /* parked; exit wakes us     */
    }
    if (retval) *retval = t->retval;
    return 0;
}

int pthread_detach(pthread_t th) { (void)th; return 0; }

void pthread_exit_shim(void *rv)
{
    extern void kth_exit_impl(struct kthread *, void *);
    kth_exit_impl(gs_cur(), rv);               /* never returns             */
}

/* ---- mutex ---------------------------------------------------------------- */
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)
{ (void)attr; ((kmutex_view *)m)->w = 0; return 0; }
int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }

int pthread_mutex_lock(pthread_mutex_t *m)
{
    for (;;) {
        unsigned long expected = 0;
        if (__atomic_compare_exchange_n(&((kmutex_view *)m)->w, &expected, 1ul, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return 0;
        cpu_relax();
        futex_wait((void *)&((kmutex_view *)m)->w);
    }
}
int pthread_mutex_trylock(pthread_mutex_t *m)
{
    unsigned long expected = 0;
    if (__atomic_compare_exchange_n(&((kmutex_view *)m)->w, &expected, 1ul, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 0;
    return -1;
}
int pthread_mutex_unlock(pthread_mutex_t *m)
{
    __atomic_store_n(&((kmutex_view *)m)->w, 0ul, __ATOMIC_RELEASE);
    futex_wake((void *)&((kmutex_view *)m)->w, 1);
    return 0;
}

/* ---- cond ------------------------------------------------------------------ */
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr)
{ (void)attr; ((kcond_view *)c)->seq = 0; return 0; }
int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    unsigned long seq = __atomic_load_n(&((kcond_view *)c)->seq, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(m);
    /* recheck under bucket semantics: park only if no signal since         */
    if (__atomic_load_n(&((kcond_view *)c)->seq, __ATOMIC_SEQ_CST) == seq)
        futex_wait((void *)&((kcond_view *)c)->seq);
    else
        sched_switch_away();
    pthread_mutex_lock(m);
    return 0;
}

int pthread_cond_signal(pthread_cond_t *c)
{
    __atomic_fetch_add(&((kcond_view *)c)->seq, 1ul, __ATOMIC_SEQ_CST);
    futex_wake((void *)&((kcond_view *)c)->seq, 1);
    return 0;
}
int pthread_cond_broadcast(pthread_cond_t *c)
{
    __atomic_fetch_add(&((kcond_view *)c)->seq, 1ul, __ATOMIC_SEQ_CST);
    futex_wake((void *)&((kcond_view *)c)->seq, SMP_MAX_THR);
    return 0;
}

/* ---- once + TLS -------------------------------------------------------------- */
static volatile unsigned long once_lock;

int pthread_once(pthread_once_t *o, void (*fn)(void))
{
    if (__atomic_load_n(o, __ATOMIC_ACQUIRE)) return 0;
    kftx_lock();
    if (!*o) { fn(); __atomic_store_n(o, 1, __ATOMIC_RELEASE); }
    kftx_unlock();
    return 0;
}

static void (*tls_dtor[SMP_TLS_SLOTS])(void *);

int pthread_key_create(pthread_key_t *k, void (*dtor)(void *))
{
    for (int i = 0; i < SMP_TLS_SLOTS; i++) {
        if (!__atomic_load_n(&tls_dtor[i], __ATOMIC_RELAXED)) {
            tls_dtor[i] = dtor;
            if (k) *k = (pthread_key_t)(i + 1);
            return 0;
        }
    }
    return -1;
}
int pthread_key_delete(pthread_key_t k)
{
    if (!k || k > SMP_TLS_SLOTS) return -1;
    tls_dtor[k - 1] = 0;
    return 0;
}
int pthread_setspecific(pthread_key_t k, const void *v)
{
    if (!k || k > SMP_TLS_SLOTS) return -1;
    gs_cur()->tls[k - 1] = (void *)v;
    return 0;
}
void *pthread_getspecific(pthread_key_t k)
{
    if (!k || k > SMP_TLS_SLOTS) return 0;
    return gs_cur()->tls[k - 1];
}

/* FFmpeg names its worker threads via prctl(PR_SET_NAME); we have no
 * task names - accept and ignore.                                          */
int prctl(int op, unsigned long a2, unsigned long a3,
          unsigned long a4, unsigned long a5)
{ (void)op;(void)a2;(void)a3;(void)a4;(void)a5; return 0; }
