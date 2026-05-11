/*
 * video.c - Framebuffer driver.
 *
 * QEMU:  ramfb device via fw_cfg interface.
 * Board: U-Boot initializes framebuffer at 0x7f700000.
 */

typedef unsigned int   uint32_t;
typedef unsigned long  uint64_t;
typedef unsigned short uint16_t;

extern void *k_memcpy(void *dst, const void *src, unsigned long n);
extern int k_strncmp(const char *a, const char *b, unsigned long n);
extern void uart_puts(const char *s);
extern void uart_hex(unsigned long h);

/* ---- Framebuffer parameters ---- */
// The size of framebuffer is based on connected monitor,
// We have to make sure this paras are larger than or equal to the actual framebuffer size, otherwise the display may be corrupted.

#define FB_WIDTH  1920
#define FB_HEIGHT 1080
#define FB_BPP    4

#ifdef QEMU

#define FB_BASE   0x87000000UL

/* ---- fw_cfg (QEMU firmware configuration) ---- */

#define QEMU_PACKED __attribute__((packed))

static inline uint64_t bswap64(uint64_t x) {
    return ((x & 0x00000000000000FFULL) << 56) |
           ((x & 0x000000000000FF00ULL) << 40) |
           ((x & 0x0000000000FF0000ULL) << 24) |
           ((x & 0x00000000FF000000ULL) <<  8) |
           ((x & 0x000000FF00000000ULL) >>  8) |
           ((x & 0x0000FF0000000000ULL) >> 24) |
           ((x & 0x00FF000000000000ULL) >> 40) |
           ((x & 0xFF00000000000000ULL) >> 56);
}
static inline uint32_t bswap32(uint32_t x) {
    return ((x & 0x000000FF) << 24) |
           ((x & 0x0000FF00) <<  8) |
           ((x & 0x00FF0000) >>  8) |
           ((x & 0xFF000000) >> 24);
}
static inline uint16_t bswap16(uint16_t x) {
    return (uint16_t)(((x & 0x00FF) << 8) | ((x & 0xFF00) >> 8));
}

struct QEMU_PACKED RAMFBCfg {
    uint64_t addr;
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
};

#define FW_CFG_BASE   0x10100000UL
#define FW_CFG_DMA    ((volatile uint64_t *)(FW_CFG_BASE + 0x10))

#define FW_CFG_DMA_CTL_ERROR  0x01
#define FW_CFG_DMA_CTL_READ   0x02
#define FW_CFG_DMA_CTL_SELECT 0x08
#define FW_CFG_DMA_CTL_WRITE  0x10

#define FW_CFG_FILE_DIR 0x19

#define XRGB8888 875713112

struct QEMU_PACKED FWCfgFile {
    uint32_t size;
    uint16_t select;
    uint16_t reserved;
    char name[56];
};

struct QEMU_PACKED FWCfgDmaAccess {
    uint32_t control;
    uint32_t length;
    uint64_t address;
};

static void fw_cfg_dma_transfer(void *address, uint32_t length, uint32_t control) {
    struct FWCfgDmaAccess access = {
        .control = bswap32(control),
        .length = bswap32(length),
        .address = bswap64((uint64_t)address),
    };
    *FW_CFG_DMA = bswap64((uint64_t)&access);
    while (bswap32(access.control) & ~FW_CFG_DMA_CTL_ERROR)
        ;
}

static void fw_cfg_read_entry(void *buf, int e, int len) {
    uint32_t control = ((uint32_t)e << 16) | FW_CFG_DMA_CTL_SELECT | FW_CFG_DMA_CTL_READ;
    fw_cfg_dma_transfer(buf, (uint32_t)len, control);
}

static void fw_cfg_write_entry(void *buf, int e, int len) {
    uint32_t control = ((uint32_t)e << 16) | FW_CFG_DMA_CTL_SELECT | FW_CFG_DMA_CTL_WRITE;
    fw_cfg_dma_transfer(buf, (uint32_t)len, control);
}

static int fw_cfg_find_file(const char *name) {
    uint32_t count = 0;
    fw_cfg_read_entry(&count, FW_CFG_FILE_DIR, sizeof(count));
    count = bswap32(count);
    for (uint32_t i = 0; i < count; i++) {
        struct FWCfgFile file;
        fw_cfg_dma_transfer(&file, sizeof(file), FW_CFG_DMA_CTL_READ);
        if (k_strncmp(name, file.name, sizeof(file.name)) == 0)
            return bswap16(file.select);
    }
    return -1;
}

void video_init(void) {
    int sel = fw_cfg_find_file("etc/ramfb");
    if (sel < 0) {
        uart_puts("video: ramfb not found\r\n");
        return;
    }
    struct RAMFBCfg cfg = {
        .addr = bswap64(FB_BASE),
        .fourcc = bswap32(XRGB8888),
        .flags = bswap32(0),
        .width = bswap32(FB_WIDTH),
        .height = bswap32(FB_HEIGHT),
        .stride = bswap32(FB_WIDTH * FB_BPP),
    };
    fw_cfg_write_entry(&cfg, sel, sizeof(struct RAMFBCfg));
    uart_puts("video: ramfb initialized (");
    uart_hex(FB_BASE);
    uart_puts(")\r\n");
}

#else /* Board (Orange Pi RV2) */

#define FB_BASE 0x7f700000UL

#define CACHE_BLOCK_SIZE 64
#define cbo_flush(start)                \
    ({                                  \
        asm volatile("mv a0, %0\n\t"    \
                     ".word 0x0025200F" \
                     :                  \
                     : "r"(start)       \
                     : "memory", "a0"); \
    })

static void flush_dcache(void *addr, unsigned long len) {
    unsigned long start = (unsigned long)addr & ~(CACHE_BLOCK_SIZE - 1);
    __sync_synchronize();
    for (unsigned long line = start; line < (unsigned long)addr + len;
         line += CACHE_BLOCK_SIZE) {
        cbo_flush(line);
        __sync_synchronize();
    }
}

void video_init(void) {
    uart_puts("video: using U-Boot framebuffer at ");
    uart_hex(FB_BASE);
    uart_puts("\r\n");

    /* Clear screen to black */
    unsigned int *fb = (unsigned int *)FB_BASE;
    for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++)
        fb[i] = 0;
    flush_dcache((void *)FB_BASE, (unsigned long)FB_WIDTH * FB_HEIGHT * FB_BPP);
}

#endif /* QEMU */

/* ---- Display function (shared) ---- */

void video_display(unsigned int *bmp_image, unsigned int width, unsigned int height) {
    unsigned int *fb = (unsigned int *)FB_BASE;
    /* align the image to the center of screen */
    int start_x = ((int)FB_WIDTH - (int)width) / 2;
    int start_y = ((int)FB_HEIGHT - (int)height) / 2;

    /* copy row by row from bmp to frame buffer, and flush dcache for each row to make sure the display can get the updated data in time */
    for (int y = 0; y < (int)height; y++) {
        unsigned int *dst = fb + (start_y + y) * FB_WIDTH + start_x;
        unsigned int *src = bmp_image + y * (int)width;
        k_memcpy(dst, src, width * sizeof(unsigned int));
#ifndef QEMU
        flush_dcache(dst, width * sizeof(unsigned int));
#endif
    }
}
