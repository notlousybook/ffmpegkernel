/* ============================================================================
 * test_harness.c — freestanding "hello from the decoder" for the FFmpeg shim
 *
 * Statically links libavformat (mov/mp4 demuxer) + libavcodec (h264 decoder)
 * against stubs/syscall_shim.c + stubs/libm_shim.c, then:
 *   1. wires the embedded MP4 blob as a fake initrd (rd_base/rd_size)
 *   2. exercises open/read/lseek/fstat/close through the shim
 *   3. feeds it into AVIOContext -> mov demuxer -> h264 decoder
 *   4. prints per-frame stats and exits non-zero on failure.
 *
 * Entry is `_start` (no crt0). Runs as-is under `qemu-x86_64`, natively, or
 * as a `-kernel` payload for an ELF-loading bare-metal kernel.
 *
 * API note: avcodec_decode_video2 was REMOVED in FFmpeg 5.0; this harness
 * auto-selects the old/new decode API at compile time so it also works with
 * legacy 3.x/4.x tarballs.
 * ==========================================================================*/
#include <stdio.h>      /* declarations only - real symbols come from shim */
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>     /* close() prototype                               */
#include <sys/stat.h>   /* struct stat / fstat                             */
#include <fcntl.h>      /* O_RDONLY                                        */
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

extern unsigned char  *rd_base;         /* provided weak by syscall_shim.c */
extern unsigned long   rd_size;

#include "test_mp4.h"                   /* embedded 1MB H.264 test clip    */

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/opt.h"

#ifdef FB_PLAYBACK
extern int  fb_init(int w, int h);
extern void fb_set_scaler(int w, int h);
extern void fb_render(const void *framep);
extern void fb_set_quality(int high);
extern int  g_fb_high_quality; /* 0 = speed (FAST_BILINEAR + fast YUV), 1 = precomputed BILINEAR */
static AVRational        g_frate;
static int               g_pace_on;
/* decode-ahead ring: producer (decoder) parks frames, presenter pops them
 * on a fixed deadline grid.  Decode bursts land in the queue instead of
 * stretching the inter-frame interval -> no visible hitching.            */
#define RING_N 32
#define RING_MASK (RING_N - 1)
static AVFrame          *g_ring[RING_N];
static volatile int      g_rh, g_rt;       /* consumer / producer cursor   */
static long long         g_slot_ns = 41667000;
static unsigned long long g_next_dl;       /* next presentation deadline ns*/
static int               g_presented;
static unsigned long long g_fts[640];     /* presentation timestamps ns  */
static unsigned long long g_rts[640];     /* raw tsc at presentation      */
static unsigned long long g_tsc0_pace;
static int                g_nfts;
static unsigned long long g_rsum, g_rmax; /* fb_render cycle stats         */
static unsigned long      g_rcnt;
static int                g_rinit;
static unsigned long long now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL
         + (unsigned long long)ts.tv_nsec;
}
static void pace_begin(void)
{
    g_slot_ns = g_frate.num
        ? (long long)(1000000000LL * g_frate.den / g_frate.num) : 41667000LL;
    if (!g_ring[0])
        for (int i = 0; i < RING_N; i++) g_ring[i] = av_frame_alloc();
    g_rh = g_rt = 0; g_presented = 0;
    g_next_dl = now_ns() + (unsigned long long)g_slot_ns;
    g_tsc0_pace = __builtin_ia32_rdtsc();
    g_pace_on = 1;
}
/* present every frame whose deadline has passed (usually 0 or 1)         */
static void rp_pump(void)
{
    for (;;) {
        if (g_rh == g_rt) return;
        unsigned long long now = now_ns();
        if ((long long)(g_next_dl - now) > 0) return;
        AVFrame *f = g_ring[g_rh & RING_MASK];
        unsigned long long a = __builtin_ia32_rdtsc();
        fb_render(f);
        unsigned long long c = __builtin_ia32_rdtsc() - a;
        g_rsum += c; g_rcnt++;
        if (!g_rinit) { g_rinit = 1; g_rmax = 0; }
        else if (c > g_rmax) g_rmax = c;
        {   unsigned long long tt = __builtin_ia32_rdtsc();
            if (g_nfts < 640) { g_fts[g_nfts] = g_next_dl; g_rts[g_nfts] = tt; g_nfts++; } }
        av_frame_unref(f);
        g_rh++; g_presented++;
        if ((g_presented % 30) == 0)
            printf("    [rp] n=%d dt_ns=%lld dt_tsc=%llu rh=%d rt=%d\n",
                   g_presented,
                   (long long)(g_fts[g_nfts-1] - g_fts[g_nfts-2]),
                   g_rts[g_nfts-1] - g_rts[g_nfts-2], g_rh, g_rt);
        unsigned long long n2 = now_ns();
        g_next_dl += (unsigned long long)g_slot_ns;
    }
}
#endif

/* ------------------------------------------------------------- entry point */
__asm__(
    ".text\n"
    ".globl _start\n"
    "_start:\n"
    "    xor %rbp, %rbp\n"
    "    mov $_stack_top, %rsp\n"
    "    call kmain\n"
    "    mov %eax, %edi\n"
    "    call _exit\n"
    "1:  hlt\n"
    "    jmp 1b\n"
    ".bss\n"
    ".align 16\n"
    "_stack_bottom:\n"
    "    .skip 262144\n"                /* 256 KiB stack                  */
    "_stack_top:\n"
    ".previous\n");

/* --------------------------------------------------------- console routing */
/* Debug aid (hosted validation only): raw-syscall SIGSEGV handler that prints
 * the faulting RIP so crashes inside FFmpeg can be located without gdb.      */
#ifdef SHIM_DEBUG_SEGV
struct ksig { void *h; unsigned long flags; void *restorer; unsigned long mask; };
static void segv_restorer(void);
__asm__(".text\nsegv_restorer:\n mov $15, %rax\n syscall\n");
static long sys5(long n, long a, long b, long c, long d) {
    long ret; register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall":"=a"(ret):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10)
                     :"rcx","r11","memory");
    return ret;
}
static void segv_h(int sig, void *si, void *uctx)
{
    unsigned long *mc = uctx;
    unsigned long rip = mc[21];               /* &uc->uc_mcontext.gregs[REG_RIP] */
    char msg[96];
    int n = snprintf(msg, sizeof msg,
                     "\n*** SIGSEGV sig=%d rip=%p ***\n", sig, (void *)rip);
    __asm__ volatile("syscall"::"a"(1L),"D"(2L),"S"(msg),"d"((long)n));
    __asm__ volatile("syscall"::"a"(231L),"D"(139L));   /* exit_group(139)   */
    (void)si;
}
static void install_segv(void)
{
    struct ksig sa = { segv_h, 0x04000004u /*SA_RESTORER|SA_SIGINFO*/,
                       segv_restorer, 0 };
    if (sys5(13 /*rt_sigaction*/, 11 /*SIGSEGV*/, (long)&sa, 0, 8) != 0)
}
#else
static void install_segv(void) {}
#endif


static void ff_log_cb(void *avcl, int level, const char *fmt, va_list vl)
{
    (void)avcl;
    if (level > AV_LOG_WARNING) return;
    char line[1024];
    vsnprintf(line, sizeof line, fmt, vl);
    fprintf(stderr, "[ff] %s", line);
}

#define CHECK(cond, ...) do { if (!(cond)) { \
        printf("FAIL: " __VA_ARGS__); printf("\n"); return 1; } } while (0)

/* ------------------------------------------------------------ AVIO shims  */
static int rd_read(void *opaque, uint8_t *buf, int buf_size)
{
    int fd = *(int *)opaque;
    return (int)read(fd, buf, (unsigned long)buf_size);
}
static int64_t rd_seek(void *opaque, int64_t offset, int whence)
{
    int fd = *(int *)opaque;
    if (whence & AVSEEK_SIZE) {
        struct stat st;
        fstat(fd, &st);
        return (int64_t)st.st_size;
    }
    return lseek(fd, offset, whence);   /* SEEK_SET/CUR/END == 0/1/2      */
}

/* ------------------------------------------------------------------- main  */
int kmain(void)
{
    install_segv();
    printf("\n=== ffmpeg-on-bare-metal: freestanding decode harness ===\n");

    /* 1. expose the RAM disk: -initrd wiring wins, else the embedded blob --*/
    if (!rd_size) { rd_base = test_mp4; rd_size = test_mp4_len; }
    printf("[1] ramdisk wired: base=%p size=%lu\n", rd_base, rd_size);

    /* 2. exercise the file-API shims ---------------------------------------*/
    int fd = open("initrd://test.mp4", O_RDONLY);
    CHECK(fd >= 0, "open() failed");
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size > 0,
          "fstat failed (%ld)", (long)st.st_size);
    off_t seeked = lseek(fd, 4, SEEK_SET);
    CHECK(seeked == 4, "lseek failed");
    char magic[4] = {0};
    CHECK(read(fd, magic, 4) == 4, "read failed");
    CHECK(memcmp(magic, "ftyp", 4) == 0, "not an mp4? magic=%.4s", magic);
    lseek(fd, 0, SEEK_SET);
    printf("[2] open/fstat/lseek/read OK (fd=%d, magic='%.4s')\n", fd, magic);

    /* 3. demuxer + decoder setup -------------------------------------------*/
    av_log_set_callback(ff_log_cb);

    uint8_t *iobuf = malloc(1 << 16);
    CHECK(iobuf != NULL, "malloc(iobuf) failed");
    AVIOContext *avio = avio_alloc_context(iobuf, 1 << 16, 0, &fd,
                                           rd_read, NULL, rd_seek);
    CHECK(avio != NULL, "avio_alloc_context failed");

    AVFormatContext *fmt = avformat_alloc_context();
    CHECK(fmt != NULL, "avformat_alloc_context failed");
    fmt->pb = avio;

    AVInputFormat *movfmt = av_find_input_format("mov");   /* mov,mp4,m4a... */
    CHECK(movfmt != NULL, "mov demuxer not compiled in");
    av_log_set_level(AV_LOG_DEBUG);
    int ret;
#ifndef BENCH_QUIET
    unsigned long long tp = __builtin_ia32_rdtsc();
#endif
#ifdef BENCH_QUIET
    ret =
#else
    ret =
#endif
    avformat_open_input(&fmt, NULL, movfmt, NULL);
    CHECK(ret == 0, "avformat_open_input: %d", ret);
#ifndef BENCH_QUIET
    printf("[phase] open_input   = %10llu kcy\n",
           (__builtin_ia32_rdtsc()-tp)>>10);
    tp = __builtin_ia32_rdtsc();
#endif
    ret = avformat_find_stream_info(fmt, NULL);
    CHECK(ret >= 0, "avformat_find_stream_info: %d", ret);
#ifndef BENCH_QUIET
    printf("[phase] find_info    = %10llu kcy\n", (__builtin_ia32_rdtsc()-tp)>>10);
    tp = __builtin_ia32_rdtsc();
#endif

    int vidx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            { vidx = (int)i; break; }
    CHECK(vidx >= 0, "no video stream");
    AVStream *vs = fmt->streams[vidx];
    printf("[3] stream #%d: codec=%s %dx%d duration=%ld\n",
           vidx, avcodec_get_name(vs->codecpar->codec_id),
           vs->codecpar->width, vs->codecpar->height,
           (long)vs->duration);

#ifdef FB_PLAYBACK
    if (fb_init(vs->codecpar->width, vs->codecpar->height) == 0) {
        /* speed-favored by default: FAST_BILINEAR + SSE YUV fast path.
         * To request precomputed high-quality SWS_BILINEAR, set:
         *   g_fb_high_quality = 1;  // or fb_set_quality(1)
         * before fb_set_scaler, or pass "quality" on kernel cmdline. */
        // fb_set_quality(g_fb_high_quality); // already default 0
        fb_set_scaler(vs->codecpar->width, vs->codecpar->height);
        g_frate = fmt->streams[vidx]->avg_frame_rate;
        printf("[fb] ramfb %dx%d XRGB8888 live - playing at source rate (%s)\n",
               vs->codecpar->width, vs->codecpar->height,
               g_fb_high_quality ? "high-quality BILINEAR" : "FAST_BILINEAR+fastYUV");
    } else {
        printf("[fb] ramfb init failed r=%d\n",
               fb_init(vs->codecpar->width, vs->codecpar->height));
    }
#endif

    const AVCodec *dec = avcodec_find_decoder(vs->codecpar->codec_id);
    CHECK(dec != NULL, "h264 decoder missing");
    AVCodecContext *ctx = avcodec_alloc_context3(dec);
    CHECK(ctx != NULL, "avcodec_alloc_context3");
    ret = avcodec_parameters_to_context(ctx, vs->codecpar);
    CHECK(ret == 0, "parameters_to_context: %d", ret);
    {
        extern int kncpu_online;
        int workers = kncpu_online > 1 ? kncpu_online - 1 : 1;
        ctx->thread_count = workers;
        ctx->thread_type  = FF_THREAD_FRAME;
        ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx->skip_loop_filter = AVDISCARD_ALL;
        av_opt_set(ctx->priv_data, "tune", "fastdecode", 0);
        printf("[thr] decoder threads=%d (cpus online=%d) flags FAST|LOW_DELAY skip_loop_filter\n",
               workers, kncpu_online);
    }
    ret = avcodec_open2(ctx, dec, NULL);
    CHECK(ret == 0, "avcodec_open2: %d", ret);

    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frm = av_frame_alloc();
    CHECK(pkt && frm, "packet/frame alloc");

    /* 4. decode loop (old API compiled out on FFmpeg >= 5.0) ---------------*/
    unsigned long long tsc0 = __builtin_ia32_rdtsc();
    int loops = 0;
#ifdef FB_PLAYBACK
    if (g_frate.num) pace_begin();
#endif
    int frames = 0, sent_all = 0;
    unsigned long sum_first = 0, sum_last = 0;
    while (1) { /* loop forever for visible demo */
    loops++;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 133, 100)
    for (;;) {                                        /* send/receive API    */
        AVFrame *out = NULL;
        if (!sent_all) {
            if ((ret = av_read_frame(fmt, pkt)) < 0) sent_all = 1;
            else if (pkt->stream_index != vidx) { av_packet_unref(pkt); continue; }
        }
        if (!sent_all) {
                ret = avcodec_send_packet(ctx, pkt);
            if (ret < 0 && ret != AVERROR(EAGAIN))
                { printf("[ff] send_packet err %d\n", ret); break; }
            /* packet ref is intentionally leaked/reused per original harness;
             * av_read_frame will overwrite pkt's buffer on next iteration.
             * We only unref non-video packets above to avoid audio leak. */
        }
        ret = avcodec_receive_frame(ctx, frm);
        if (ret == 0) out = frm;
        else if (ret == AVERROR_EOF) break;
        else if (ret == AVERROR(EAGAIN)) {
            if (sent_all) break;
            continue;
        }
#else
    for (;;) {
        int got = 0;
        if (!sent_all && (ret = av_read_frame(fmt, pkt)) < 0) sent_all = 1;
        ret = avcodec_decode_video2(ctx, frm, &got, sent_all ? NULL : pkt);
        if (ret < 0) { printf("[ff] decode err %d\n", ret); break; }
        AVFrame *out = got ? frm : NULL;
        if (sent_all && !got) break;
#endif
        if (out) {
            const uint8_t *Y = frm->data[0];
            int ls = frm->linesize[0] > 0 ? frm->linesize[0] : frm->width;
            unsigned long s = 0;
            for (int yy = 0; yy < frm->height; yy += 7)          /* sparse */
                for (int xx = 0; xx < frm->width; xx += 7)
                    s += Y[yy * ls + xx];
            sum_last = s;
            if (!frames++) sum_first = s;
#ifndef BENCH_QUIET
            if (frames <= 3 || (frames % 30) == 0)
                printf("    frame %4d: %dx%d pts=%ld luma_sum=%lx\n",
                       frames, frm->width, frm->height,
                       (long)(frm->pts == AV_NOPTS_VALUE ? -1 : frm->pts), s);
#endif
#ifdef FB_PLAYBACK
            {   /* decode-ahead: park frame in ring, presenter drains it on
                 * the deadline grid; block (politely) when ring is full   */
                while (g_rt - g_rh >= RING_N - 2) {
                    rp_pump();
                    struct timespec ts = { 0, 100000 }; nanosleep(&ts, NULL);
                }
                av_frame_ref(g_ring[g_rt & RING_MASK], frm);
                g_rt++;
                rp_pump();
            }
#endif
            av_frame_unref(frm);
            if (frames >= 1000) { printf("    (frame cap reached)\n"); break; }
        }
    }
    unsigned long long tsc1_inner = __builtin_ia32_rdtsc();
#ifdef FB_PLAYBACK
    printf("[loop %d] decoded %d frames in %llu kcy/frame presented=%d wall_tsc=%llu\n", loops, frames, (unsigned long long)((tsc1_inner - tsc0) >> 10) / (frames ? frames : 1), g_presented, __builtin_ia32_rdtsc() - g_tsc0_pace);
    if (g_nfts > 30) {
        long long sum=0,mn=0,mx=0; int late=0;
        long long budget = g_frate.num ? (long long)(1000000LL * g_frate.den / g_frate.num)*1000LL : 41667000LL;
        for(int i=1;i<g_nfts;i++){ long long d=g_fts[i]-g_fts[i-1]; if(!mn||d<mn) mn=d; if(d>mx) mx=d; sum+=d; if(d>budget+2000000) late++; }
        printf("[pace] n=%d avg=%lld min=%lld max=%lld late=%d\n", g_nfts-1, sum/(g_nfts-1), mn,mx,late);
    }
    printf("[loop %d] looping...\n", loops);
    /* wrap: refill producer side; keep the presentation metronome running
     * so the loop point is seamless (leftover ring frames flush first).  */
    av_seek_frame(fmt, vidx, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(ctx);
    lseek(fd, 0, SEEK_SET);
    frames = 0; sent_all = 0;
    g_nfts = 0; g_rsum = 0; g_rmax = 0; g_rcnt = 0; g_rinit = 0;
    tsc0 = __builtin_ia32_rdtsc();
    continue;
#else
    break;
#endif
    } /* while(1) */
    unsigned long long tsc1 = __builtin_ia32_rdtsc();
    printf("[phase] codec_open   = %10llu kcy\n", (tsc1-tsc0)>>10);
    printf("[bench] decoded %d frames in %llu cycles (%llu kcy/frame avg incl find_info)\n",
           frames, (unsigned long long)(tsc1 - tsc0),
           (unsigned long long)((tsc1 - tsc0) >> 10) / (frames ? frames : 1));
#ifdef FB_PLAYBACK
    if (g_nfts > 30) {
        long long sum = 0, mn = 0, mx = 0; int late = 0;
        long long budget = g_frate.num
            ? (long long)(1000000LL * g_frate.den / g_frate.num) * 1000LL
            : 41667000LL;
        for (int i = 1; i < g_nfts; i++) {
            long long d = (long long)(g_fts[i] - g_fts[i - 1]);
            if (!mn || d < mn) mn = d;
            if (d > mx) mx = d;
            sum += d;
            if (d > budget + 2000000LL) late++;    /* slot blown by >2ms */
        }
        printf("[pace] src_fps=%d/%d n=%d avg_us=%lld min_us=%lld max_us=%lld "
               "late=%d\n",
               g_frate.num, g_frate.den, g_nfts - 1,
               sum / (g_nfts - 1), mn, mx, late);
        printf("[render] frames=%lu avg_kcy=%llu max_kcy=%llu\n",
               g_rcnt, g_rcnt ? g_rsum / g_rcnt >> 10 : 0,
               g_rmax >> 10);
    }
#endif
    avcodec_send_packet(ctx, NULL);       /* drain                            */

    /* 5. verdict ------------------------------------------------------------*/
    printf("[4] decoded %d frame(s); luma sums first=%lx last=%lx\n",
           frames, sum_first, sum_last);
    CHECK(frames > 0, "decoder produced no frames");
    CHECK(sum_first > 0, "first frame luma empty");
    CHECK(sum_first != sum_last, "frames identical - suspicious");

    av_packet_free(&pkt); av_frame_free(&frm);
    avcodec_free_context(&ctx); avformat_close_input(&fmt);
    avio_context_free(&avio);
    close(fd);

    printf("=== DECODE_OK frames=%d ===\n", frames);
    return 0;
}
