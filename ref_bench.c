/* ============================================================================
 * ref_bench.c — hosted reference: SAME FFmpeg static libs, SAME input file,
 * SAME decode loop as test_harness.c, but built normally against glibc.
 * Any perf delta between this and test_harness.elf is attributable purely to
 * the shim layer (allocator/string/printf) + freestanding environment.
 *
 * Build: see Makefile target ref_bench
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

static unsigned char *g_blob; static long g_size;
static int ram_read(void *opaque, uint8_t *buf, int buf_size)
{
    long *pos = opaque;
    long avail = g_size > *pos ? g_size - *pos : 0;
    long n = buf_size < avail ? buf_size : avail;
    if (n <= 0) return AVERROR_EOF;
    memcpy(buf, g_blob + *pos, n);
    *pos += n;
    return (int)n;
}
static int64_t ram_seek(void *opaque, int64_t off, int whence)
{
    long *pos = opaque;
    if (whence & AVSEEK_SIZE) return g_size;
    long np = whence == SEEK_SET ? off : whence == SEEK_CUR ? *pos + off
                                                            : g_size + off;
    if (np < 0) np = 0;
    if (np > g_size) np = g_size;
    *pos = np;
    return np;
}

static void ff_log_cb(void *avcl, int level, const char *fmt, va_list vl)
{
    (void)avcl;
    if (level > AV_LOG_WARNING) return;
    char line[1024];
    vsnprintf(line, sizeof line, fmt, vl);
    fprintf(stderr, "[ff] %s", line);
}

int main(void)
{
    av_log_set_level(AV_LOG_WARNING);

    /* load file into RAM, then feed via AVIO exactly like the harness */
    FILE *f = fopen("test.mp4", "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *blob = malloc(fsz);
    if (fread(blob, 1, fsz, f) != (size_t)fsz) return 1;
    fclose(f);
    g_blob = blob; g_size = fsz;

    static long rd_pos = 0;
    AVIOContext *avio = avio_alloc_context(malloc(1 << 16), 1 << 16, 0, &rd_pos,
                                           ram_read, NULL, ram_seek);
    AVFormatContext *fmt = avformat_alloc_context();
    fmt->pb = avio;
    const AVInputFormat *movfmt = av_find_input_format("mov");
    int ret = avformat_open_input(&fmt, NULL, movfmt, NULL);
    if (ret < 0) { fprintf(stderr, "open failed %d\n", ret); return 1; }
    ret = avformat_find_stream_info(fmt, NULL);
    if (ret < 0) { fprintf(stderr, "find_info failed\n"); return 1; }

    int vidx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            { vidx = (int)i; break; }
    AVStream *vs = fmt->streams[vidx];
    printf("stream: %s %dx%d\n", avcodec_get_name(vs->codecpar->codec_id),
           vs->codecpar->width, vs->codecpar->height);

    const AVCodec *dec = avcodec_find_decoder(vs->codecpar->codec_id);
    AVCodecContext *ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, vs->codecpar);
    ret = avcodec_open2(ctx, dec, NULL);
    if (ret < 0) { fprintf(stderr, "open2 failed\n"); return 1; }

    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frm = av_frame_alloc();
    int frames = 0, sent_all = 0;
    unsigned long long tsc0 = __builtin_ia32_rdtsc(), tsc1;

    for (;;) {
        if (!sent_all && av_read_frame(fmt, pkt) < 0) sent_all = 1;
        else if (!sent_all && pkt->stream_index != vidx) continue;

        if (!sent_all) {
            ret = avcodec_send_packet(ctx, pkt);
            if (ret < 0 && ret != AVERROR(EAGAIN)) break;
        }
        ret = avcodec_receive_frame(ctx, frm);
        if (ret == AVERROR_EOF) break;
        if (ret == AVERROR(EAGAIN)) { if (sent_all) break; continue; }
        if (ret == 0) {
            frames++;
            av_frame_unref(frm);
            if (frames >= 1000) break;
        }
    }
    tsc1 = __builtin_ia32_rdtsc();

    printf("[bench] decoded %d frames in %llu cycles\n", frames,
           (unsigned long long)(tsc1 - tsc0));
    printf("=== REF_DECODE_OK frames=%d ===\n", frames);
    return 0;
}
