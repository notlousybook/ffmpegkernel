/* Hosted no-op pthread stubs ONLY to satisfy FFmpeg configure's link
 * check (-lpthread).  At kernel link time the real smp/ implementations
 * are linked first and win. */
#include <stddef.h>
#include <pthread.h>
int  pthread_create(pthread_t *t, const void *a, void *(*f)(void*), void *x)
    { (void)t;(void)a;(void)f;(void)x; return -1; }
int      pthread_join(pthread_t t, void **r){ (void)t;(void)r; return -1; }
int      pthread_detach(pthread_t t){ (void)t; return 0; }
pthread_t pthread_self(void){ return 0; }
int      pthread_mutex_init(pthread_mutex_t *m, const void *a){ (void)a; m->v=0; return 0; }
int      pthread_mutex_destroy(pthread_mutex_t *m){ (void)m; return 0; }
int      pthread_mutex_lock(pthread_mutex_t *m){ (void)m; return 0; }
int      pthread_mutex_trylock(pthread_mutex_t *m){ (void)m; return 0; }
int      pthread_mutex_unlock(pthread_mutex_t *m){ (void)m; return 0; }
int      pthread_cond_init(pthread_cond_t *c, const void *a){ (void)a; c->seq=0;c->waiters=0; return 0; }
int      pthread_cond_destroy(pthread_cond_t *c){ (void)c; return 0; }
int      pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m){ (void)c;(void)m; return 0; }
int      pthread_cond_signal(pthread_cond_t *c){ (void)c; return 0; }
int      pthread_cond_broadcast(pthread_cond_t *c){ (void)c; return 0; }
int      pthread_once(pthread_once_t *o, void (*f)(void)){ if(o&&!*o){*o=1;f();} return 0; }
int      pthread_key_create(pthread_key_t *k, void (*d)(void*)){ (void)d; if(k)*k=1; return 0; }
int      pthread_key_delete(pthread_key_t k){ (void)k; return 0; }
int      pthread_setspecific(pthread_key_t k, const void *v){ (void)k;(void)v; return 0; }
void    *pthread_getspecific(pthread_key_t k){ (void)k; return 0; }
