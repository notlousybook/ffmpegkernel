/* ============================================================================
 * smp/core.c - cooperative scheduler core: runqueue, stack-swapping
 * context switch, thread bootstrap/exit, per-CPU entry points.
 *
 * asm-visible struct offsets: kthread.sp=0, .fn=8, .arg=16.
 * ==========================================================================*/
#include <stdint.h>
#include <stddef.h>
#include "kern.h"
#include "../stubs/shim.h"

struct kcpu   kcpus[SMP_MAX_CPU];
static struct kthread pool[SMP_MAX_THR];
static int    pool_n;
static struct kthread *rq_head, *rq_tail;
volatile unsigned long kth_sched_lock;

/* shared with futex.c for ordered state transitions                        */
void kcore_lock(void)
{
    volatile unsigned long *l = &kth_sched_lock;
    for (;;) {
        unsigned long expected = 0;
        if (__atomic_compare_exchange_n(l, &expected, 1ul, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
        cpu_relax();
    }
}
void kcore_unlock(void)
{ __atomic_store_n(&kth_sched_lock, 0ul, __ATOMIC_RELEASE); }

void cpu_relax(void) { __asm__ volatile("pause"); }

static void rq_push(struct kthread *t)
{
    t->rq_next = 0;
    if (rq_tail) rq_tail->rq_next = t;
    else         rq_head = t;
    rq_tail = t;
}
static struct kthread *rq_pop(void)
{
    struct kthread *t = rq_head;
    if (!t) return 0;
    rq_head = t->rq_next;
    if (!rq_head) rq_tail = 0;
    t->rq_next = 0;
    return t;
}

void sched_make_runnable(struct kthread *t)
{
    kcore_lock();
    if (t->state == TH_WAITING || t->state == TH_EMPTY) {
        t->state = TH_RUNNABLE;
        rq_push(t);
    }
    kcore_unlock();
}

static void wake_other_cpus(void)
{
    extern void ipi_all_excl_self(unsigned cmd);
    ipi_all_excl_self(1u << 14);
}

/* ---- context switch / bootstrap stubs ------------------------------------ */
asm(
".text\n"
".globl kth_switch\n"
"kth_switch:\n"                             /* rdi=&old->sp   rsi=new sp    */
"    pushq %rbp\n"
"    pushq %rbx\n"
"    pushq %r12\n"
"    pushq %r13\n"
"    pushq %r14\n"
"    pushq %r15\n"
"    movq  %rsp, (%rdi)\n"
"    movq  %rsi, %rsp\n"
"    popq  %r15\n"
"    popq  %r14\n"
"    popq  %r13\n"
"    popq  %r12\n"
"    popq  %rbx\n"
"    popq  %rbp\n"
"    retq\n"

".globl kth_boot_asm\n"
"kth_boot_asm:\n"
"    movq %gs:(0), %rcx\n"                  /* cur                          */
"    movq 16(%rcx), %rdi\n"                 /* arg                          */
"    callq *8(%rcx)\n"                      /* fn(arg)                      */
"    movq %rax, %rsi\n"                     /* retval                       */
"    movq %gs:(0), %rdi\n"                  /* me                           */
"    callq kth_exit_impl\n"
"1: cli; hlt; jmp 1b\n");

extern void kth_switch(void **old_sp, void *new_sp);
void kth_exit_impl(struct kthread *me, void *rv);
void sched_loop_idle_forever(void);

/* ---- BSP boot pseudo-thread ---------------------------------------------- */
static struct kthread bsp_thr;

void sched_init_bsp(void)
{
    /* GS base must be valid BEFORE any gs-relative access                  */
    { unsigned lo_ = (unsigned)(unsigned long)&kcpus[0],
               hi_ = (unsigned)((unsigned long long)(unsigned long)&kcpus[0] >> 32);
      __asm__ volatile("wrmsr" :: "a"(lo_), "d"(hi_), "c"(0xC0000101)); } /* IA32_GS_BASE */
    bsp_thr.state = TH_RUNNING;
    bsp_thr.tid   = 0;
    gs_set(&bsp_thr);
    __atomic_store_n(&kcpus[0].online, 1, __ATOMIC_SEQ_CST);
}

/* ---- spawn ---------------------------------------------------------------- */
int kthread_spawn(void (*fn)(void *), void *arg, unsigned long stack_bytes,
                  unsigned long *tid_out)
{
    if (!fn || (long)stack_bytes <= 4096) return -1;

    extern int shim_heap_alloc(size_t n, void **out);
    unsigned char *stk = 0;
    if (shim_heap_alloc((size_t)stack_bytes + 64, (void **)&stk)) return -1;
    stk = (unsigned char *)(((unsigned long)stk + 63) & ~63ul);

    if (pool_n >= SMP_MAX_THR) return -1;
    struct kthread *t = &pool[pool_n];

    t->fn = fn; t->arg = arg;
    t->state = TH_EMPTY;
    t->rq_next = t->wk_next = t->joiner = 0;
    t->retval = 0;
    for (int i = 0; i < SMP_TLS_SLOTS; i++) t->tls[i] = 0;

    /* bootstrap frame matches kth_switch's pops: [r15..rbp][ret]           */
    unsigned long *sp = (unsigned long *)(unsigned long)(stk + stack_bytes);
    { extern char kth_boot_asm[]; *(--sp)=(unsigned long)(unsigned long)&kth_boot_asm[0]; }
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;
    t->sp = (void *)sp;

    pool_n++;
    t->tid = (unsigned long)pool_n;          /* 1-based handle              */
    *tid_out = t->tid;
    sched_make_runnable(t);
    wake_other_cpus();
    return 0;
}

struct kthread *kth_by_tid(unsigned long tid)
{
    if (!tid || tid > (unsigned long)pool_n) return 0;
    return &pool[tid - 1];
}

void kth_exit_impl(struct kthread *me, void *rv)
{
    me->retval = rv;
    struct kthread *j;
    kcore_lock();
    me->state = TH_DONE;
    j = me->joiner;
    me->joiner = 0;
    kcore_unlock();
    if (j) {
        sched_make_runnable(j);
        wake_other_cpus();
    }
    sched_loop_idle_forever();               /* become the core's dispatcher */
}

/* ---- switching ------------------------------------------------------------ */
static void idle_wait(void) { __asm__ volatile("sti\n\thlt\n\tcli"); }

void sched_switch_away(void)
{
    struct kthread *me = gs_cur();
    for (;;) {
        if (me->state == TH_RUNNABLE) {      /* woken before we slept       */
            me->state = TH_RUNNING;
            return;
        }
        kcore_lock();
        struct kthread *next = rq_pop();
        kcore_unlock();

        if (!next) { idle_wait(); continue; }

        next->state = TH_RUNNING;
        gs_set(next);
        kth_switch(&me->sp, next->sp);
        /* resumed later with gs:[cur] == me                                */
        return;
    }
}

void sched_loop_idle_forever(void)
{
    for (;;)
        sched_switch_away();
}

void ap_entry(unsigned long idx)
{
    struct kcpu *c = &kcpus[idx];
    { unsigned lo_ = (unsigned)(unsigned long)c,
               hi_ = (unsigned)((unsigned long long)(unsigned long)c >> 32);
      __asm__ volatile("wrmsr" :: "a"(lo_), "d"(hi_), "c"(0xC0000101)); } /* IA32_GS_BASE */
    c->id = idx;
    c->boot_thr.state = TH_RUNNING;
    c->cur = &c->boot_thr;
    {
        extern void lapic_enable(void);
        lapic_enable();
    }
    __atomic_store_n(&c->online, 1, __ATOMIC_SEQ_CST);
    sched_loop_idle_forever();
}
