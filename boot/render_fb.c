/* ============================================================================
 * boot/render_fb.c - QEMU ramfb driver + swscale render path.
 *
 * ramfb displays a region of GUEST RAM that firmware/guest selects through
 * fw_cfg: no PCI enumeration, no BAR mapping - we just point QEMU at our own
 * buffer and write pixels into it.
 *
 *   1. scan fw_cfg file directory ("etc/ramfb") for its selector
 *   2. program mode via one fw_cfg DMA write (XRGB8888, W x H)
 *   3. sws_scale(YUV420P -> BGRA) straight into that buffer each frame
 * ==========================================================================*/
#include <stdint.h>
#include <stddef.h>
#include "libavutil/frame.h"
#include "libswscale/swscale.h"

#define FW_CFG_PORT_SEL   0x510
#define FW_CFG_PORT_DATA  0x511
#define FW_CFG_DMA_HI     0x514        /* addr[63:32], big-endian region     */
#define FW_CFG_DMA_LO     0x518        /* addr[31:0]; THIS write triggers    */
#define FW_CFG_FILE_DIR   0x19

#define DRM_FORMAT_XRGB8888 0x34325258u          /* 'XR24'                  */

static unsigned short g_select;                   /* etc/ramfb selector      */
static unsigned char *g_fb;                       /* our scanout buffer      */
static int g_w, g_h;
static struct SwsContext *g_sws;

static inline void outw_(unsigned short p, unsigned short v)
    { __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p)); }
static inline void outl_(unsigned short p, unsigned int v)
    { __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(p)); }
static inline void outb_(unsigned short p, unsigned char v)
    { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline unsigned char inb_(unsigned short p)
    { unsigned char v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }

static unsigned int rd32be(void) {                /* read u32 from 0x511     */
    unsigned int v = 0;
    for (int i = 0; i < 4; i++) v = (v << 8) | inb_(FW_CFG_PORT_DATA);
    return v;
}
static unsigned short rd16be(void) {
    unsigned short v = 0;
    for (int i = 0; i < 2; i++) v = (v << 8) | inb_(FW_CFG_PORT_DATA);
    return v;
}

struct fw_dma {                                   /* all fields BIG-endian   */
    unsigned int ctrl;
    unsigned int len;
    unsigned long long addr;
} __attribute__((packed));

static unsigned int fw_write(unsigned short select, const void *buf,
                             unsigned len)
{
    /* Descriptor lives in guest RAM, every field BIG-endian (QEMU reads it
     * that way).  The 64-bit descriptor ADDRESS register is an 8-byte
     * BIG-endian window at 0x514: high half at 0x514, low half at 0x518 -
     * and writing the low half IS the trigger.  Because the region decodes
     * big-endian, each outl() must carry its dword pre-swapped.
     * Control bits (fw_cfg_keys.h): SELECT=0x08, WRITE=0x10.             */
    static struct fw_dma d __attribute__((aligned(8)));
    unsigned int pa = (unsigned)&d;
    unsigned int *p = (unsigned int *)&d;
    p[0] = __builtin_bswap32(((unsigned int)select << 16)
                             | (1u << 3)                    /* SELECT     */
                             | (1u << 4));                  /* WRITE      */
    p[1] = __builtin_bswap32(len);
    p[2] = __builtin_bswap32((unsigned)((unsigned long)buf >> 32));
    p[3] = __builtin_bswap32((unsigned)(unsigned long)buf); /* DATA addr!! */
    outl_(FW_CFG_DMA_HI, 0u);
    outl_(FW_CFG_DMA_LO, __builtin_bswap32(pa));      /* kicks DMA        */
    while (__builtin_bswap32(*(volatile unsigned int *)p)
           & ~(1u << 0))                              /* until complete   */
        __asm__ volatile("pause");
    return __builtin_bswap32(*(volatile unsigned int *)p); /* 0 ok, 1 err */
}

static unsigned char fb_pixels[4096 * 2304 * 4] __attribute__((aligned(4096)));

int fb_init(int w, int h)
{
    g_w = w; g_h = h;
    if (w > 4096 || h > 4096) return -1;

    /* scan fw_cfg file directory for "etc/ramfb"                            */
    outw_(FW_CFG_PORT_SEL, FW_CFG_FILE_DIR);
    unsigned int count = rd32be();
    for (unsigned int i = 0; i < count; i++) {
        unsigned int   size   = rd32be();
        unsigned short select = rd16be();
        rd16be();                                     /* reserved              */
        char name[57];
        for (int j = 0; j < 56; j++) name[j] = (char)inb_(FW_CFG_PORT_DATA);
        name[56] = 0;                                 /* always eat 56 bytes */
        if (!__builtin_strcmp(name, "etc/ramfb")) { g_select = select; break; }
        (void)size;
    }
    if (!g_select) return -1;

    /* program mode: struct QemuRAMFBCfg, all fields BIG-endian              */
    static unsigned char cfg[28];
    unsigned long long a = (unsigned long long)(unsigned long)fb_pixels;
    for (int i = 0; i < 8; i++) cfg[i]     = (unsigned char)(a >> (56 - 8*i));
    unsigned int fcc  = __builtin_bswap32(DRM_FORMAT_XRGB8888);
    unsigned int zero = 0;
    __builtin_memcpy(cfg + 8,  &fcc, 4);
    __builtin_memcpy(cfg + 12, &zero, 4);             /* flags                 */
    unsigned int bw = __builtin_bswap32((unsigned)w);
    unsigned int bh = __builtin_bswap32((unsigned)h);
    unsigned int bs = __builtin_bswap32((unsigned)(w * 4));
    __builtin_memcpy(cfg + 16, &bw, 4);
    __builtin_memcpy(cfg + 20, &bh, 4);
    __builtin_memcpy(cfg + 24, &bs, 4);
    if (fw_write(g_select, cfg, sizeof cfg) != 0) {   /* ERROR bit set      */
        g_select = 0;
        return -2;
    }

    __builtin_memset(fb_pixels, 0x30, sizeof fb_pixels);  /* gray wash         */
    return 0;
}

void fb_blit(const void *src, unsigned long bytes)
{
    if (bytes > (unsigned long)g_w * g_h * 4) bytes = (unsigned long)g_w * g_h * 4;
    __builtin_memcpy(fb_pixels, src, bytes);
}

/* Convert one decoded AVFrame (YUV420P) and push it to the scanout.         */
void fb_render(const void *framep)
{
    const AVFrame *frm = framep;
    if (!g_select || !g_sws) return;
    uint8_t *dst[4]  = { fb_pixels, 0, 0, 0 };
    int      dstls[4] = { g_w * 4, 0, 0, 0 };
    sws_scale(g_sws, (const unsigned char * const*)frm->data,
              frm->linesize, 0, frm->height > g_h ? g_h : frm->height,
              dst, dstls);
}

void fb_set_scaler(int srcw, int srch)
{
    g_sws = sws_getContext(srcw, srch, AV_PIX_FMT_YUV420P,
                           g_w,  g_h,  AV_PIX_FMT_BGRA,
                           SWS_BILINEAR, NULL, NULL, NULL);
}
