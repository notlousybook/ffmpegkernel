/* ============================================================================
 * smp/kern.h - bare-metal SMP: per-CPU state, cooperative scheduler,
 * futex-style sync, and the pthread surface FFmpeg links against.
 *
 * Design notes:
 *  - APs start via INIT/SIPI into a real-mode trampoline at phys 0x7000,
 *    reuse the BSP GDT/page tables, and land in ap_entry().
 *  - Cooperative scheduling only: threads run until they park on a futex
 *    or exit.  Frame-threaded H.264 uses worker_count <= cpus-1 so each
 *    worker effectively owns a core.
 *  - One global runqueue + one futex-table lock; wakeups kick every other
 *    core with a level-assert IPI (vector SMP_IPI_VEC) that breaks hlt.
 *  - GS base = &per-CPU struct; gs:[0] is the current thread pointer.
 * ==========================================================================*/
#ifndef KERN_SMP_H
#define KERN_SMP_H
#include <stdint.h>
#include <stddef.h>

#define SMP_MAX_CPU   8
#define SMP_MAX_THR   32              /* pooled threads                  */
#define SMP_TLS_SLOTS 16
#define SMP_IPI_VEC   0x50            /* resched kick                    */
#define SMP_TIMER_VEC 0x60            /* LAPIC TSC-deadline timer        */
#define TRAMP_PHYS    0x7000UL
#define TRAMP_VECTOR  (TRAMP_PHYS >> 12)

enum kth_state { TH_EMPTY = 0, TH_RUNNABLE, TH_RUNNING, TH_WAITING, TH_DONE };

struct kthread {
    void           *sp;               /* 0  : saved stack pointer         */
    void         (*fn)(void *);       /* 8  : entry                       */
    void           *arg;              /* 16 : argument                    */
    int             state;            /* 24                               */
    struct kthread *rq_next;          /* 32 : runqueue link               */
    struct kthread *wk_next;          /* 40 : futex bucket link           */
    struct kthread *joiner;           /* 48 : parked joiner (single)      */
    void           *retval;           /* 56                               */
    unsigned long   tid;              /* 64 : 1-based handle              */
    void           *tls[SMP_TLS_SLOTS];
};

struct kcpu {
    struct kthread *cur;              /* offset 0 - asm depends on this   */
    unsigned        id;
    volatile int    online;
    struct kthread  boot_thr;         /* represents whoever started here  */
    unsigned char   ap_stack[32 * 1024] __attribute__((aligned(16)));
};

extern struct kcpu  kcpus[SMP_MAX_CPU];
extern volatile int kncpu_online;

static inline struct kthread *gs_cur(void)
{
    struct kthread *c;
    __asm__ volatile("movq %%gs:(0), %0" : "=r"(c));
    return c;
}
static inline void gs_set(struct kthread *t)
{
    __asm__ volatile("movq %0, %%gs:(0)" :: "r"(t));
}

/* --- core.c -------------------------------------------------------------- */
void cpu_relax(void);
void sched_init_bsp(void);                       /* wire CPU0 boot thread  */
void sched_switch_away(void);                    /* park/yield primitive   */
int  kthread_spawn(void (*fn)(void *), void *arg,
                   unsigned long stack_bytes, unsigned long *tid_out);
void kth_exit_impl(struct kthread *me, void *rv);
void ap_entry(unsigned long idx);

struct kthread *kth_by_tid(unsigned long tid);

/* --- futex.c ------------------------------------------------------------- */
void futex_wait(void *key);
int  futex_wake(void *key, int n);

#endif /* KERN_SMP_H */
