/* standalone allocator stress test: compiles syscall_shim.c hosted-style and
 * validates heap invariants (no overlapping live blocks) after every op.    */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

void *malloc(size_t), *calloc(size_t,size_t), *realloc(void*,size_t);
void free(void*);
long write(int,const void*,unsigned long);
void *memalign(size_t,size_t);

#define MAXLIVE 4096
static struct { void *p; size_t n; } live[MAXLIVE];
static int nlive;

long g_iter;
static struct { long it; int i, op; void *p; size_t n; } hist[40];
static int hn;
static void hlog(int i, int op, void *p, size_t n)
    { hist[hn & 31].it=g_iter; hist[hn&31].i=i; hist[hn&31].op=op;
      hist[hn&31].p=p; hist[hn&31].n=n; hn++; }
static void die2(const char *m, void *p, size_t n, int ci);
static void die(const char *m, void *p, size_t n) { die2(m, p, n, -1); }
static void die2(const char *m, void *p, size_t n, int ci) {
    fprintf(stderr, "INVARIANT FAIL at iter=%ld: %s p=%p n=%zu\n", g_iter, m, p, n);
    if (ci >= 0)
        fprintf(stderr, "  conflicts with LIVE[%d] = %p n=%zu\n",
                ci, live[ci].p, live[ci].n);
    for (int k = 0; k < hn && k < 32; k++)
        fprintf(stderr, "  it=%ld i=%d op=%d -> %p n=%zu\n",
                hist[k].it, hist[k].i, hist[k].op, hist[k].p, hist[k].n);
    exit(1);
}
static void check_overlap(void *p, size_t n, const char *what) {
    if (!p) return;
    unsigned char *b = p, *e = b + n;
    for (int i = 0; i < nlive; i++) {
        unsigned char *lb = live[i].p, *le = lb + live[i].n;
        /* header region of new block starts 64B below payload */
        unsigned char *hb = b - 64;
        if ((void*)lb == p && live[i].n == n) continue;   /* self */
        if (e > lb - 64 && hb < le)
            die2(what, p, n, i);
    }
}
static void add_live(void *p, size_t n) {
    check_overlap(p, n, "alloc overlaps live");
    if (nlive < MAXLIVE) { live[nlive].p = p; live[nlive].n = n; nlive++; }
}
static void del_live(void *p) {
    for (int i = 0; i < nlive; i++)
        if (live[i].p == p) { live[i] = live[nlive-1]; nlive--; return; }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* keep glibc stdio OUT of our heap */
    setvbuf(stderr, NULL, _IONBF, 0);
    srand(1234);
    void *ptrs[512] = {0};
    size_t sizes[512] = {0};
    FILE *oplog = fopen("/tmp/opencode/ops.txt", "w");
    for (long iter = 0; iter < 2000000; iter++) {
        if (iter > 400 && nlive == 0) {} /* keep going */
        g_iter = iter;
        int i = rand() % 512;
        int op = rand() % 100;

        /* pre-op integrity sweep using OUR OWN size bookkeeping */
        int nbad = 0;
        for (int k = 0; k < nlive; k++) {
            if (!live[k].p || !live[k].n) continue;
            struct { size_t sz, magic; } *h =
                (void *)((char *)live[k].p - 64);
            if (h->magic != 0x5EAF00DC0DE00001ull ||
                h->sz != ((live[k].n + 63u) & ~(size_t)63)) {
                fprintf(stderr,
                    "PRE-OP FAIL iter=%ld victim[%d] p=%p n=%zu "
                    "hdr={sz=%lu magic=%lx} cur[i=%d op=%d]\n",
                    g_iter, k, live[k].p, live[k].n,
                    h->sz, h->magic, i, op);
                /* keep scanning: how many are hit? */
                nbad++;
            }
        }
        if (nbad) exit(2);

        { char b[80]; int k=snprintf(b,sizeof b,"OP%ld i=%d op=%d\n",iter,i,op); write(2,b,k);} 
        if (op < 45 || !ptrs[i]) {                    /* alloc */
            size_t n = rand() % 3000;
            void *p = (op & 1) ? calloc(1, n ? n : 1) : malloc(n);
            del_live(ptrs[i]);                        /* leak-check lite */
            hlog(i, 0, p, n);
            ptrs[i] = p; sizes[i] = n;
            add_live(p, n);
        } else if (op < 75) {                          /* realloc */
            size_t n = rand() % 8000;
            void *q = realloc(ptrs[i], n);
            hlog(i, 1, q, n);
            del_live(ptrs[i]);
            ptrs[i] = q; sizes[i] = n;
            add_live(q, n);
        } else if (op < 90) {                          /* free */
            hlog(i, 2, ptrs[i], sizes[i]);
            free(ptrs[i]);
            del_live(ptrs[i]);
            ptrs[i] = NULL; sizes[i] = 0;
        } else {                                       /* memalign big-align */
            size_t n = rand() % 500;
            size_t a = 128u << (rand() % 4);           /* 128..1024 */
            void *p = memalign(a, n);
            del_live(ptrs[i]);
            ptrs[i] = p; sizes[i] = n;
            add_live(p, n);
            if (((unsigned long)p & (a - 1)) && p) die("misaligned", p, n);
        }
        /* touch memory to catch aliasing */
        if (ptrs[i] && sizes[i]) memset(ptrs[i], (int)(iter & 0x7f), sizes[i] < 64 ? sizes[i] : 64);

        /* full heap audit: every live block must carry a pristine header */
        for (int k = 0; k < nlive; k++) {
            struct { size_t sz, magic; void *nxt; } *h =
                (void *)((char *)live[k].p - 64);
            if (h->magic != 0x5EAF00DC0DE00001ull ||
                h->sz != ((live[k].n + 63u) & ~(size_t)63)) {
                fprintf(stderr,
                    "HEAP AUDIT FAIL iter=%ld victim[%d] p=%p n=%zu "
                    "hdr={sz=%lu magic=%lx nxt=%p} cur i=%d op=%d\n",
                    g_iter, k, live[k].p, live[k].n,
                    h->sz, h->magic, (void*)h->nxt, i, op);
                exit(1);
            }
        }
    }
    fclose(oplog);
    printf("allocator stress: PASS (%d live)\n", nlive);
    return 0;
}
