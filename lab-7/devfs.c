#include "vfs.h"
#include "mm.h"
#include "vm.h"

extern void *k_memcpy(void *dst, const void *src, unsigned long n);
extern void *k_memset(void *s, int c, unsigned long n);
extern int k_strcmp(const char *a, const char *b);
extern char *k_strncpy(char *dst, const char *src, unsigned long n);
extern void uart_putc(char c);
extern char uart_async_getc(void);
extern void uart_puts(const char *s);
extern void uart_hex(unsigned long h);

#define DEVFS_MAX_FILE_NAME 15
#define DEVFS_MAX_DIR_ENTRY 16

enum devfs_type { DEVFS_DIR, DEVFS_FILE };

struct devfs_vnode {
    enum devfs_type type;
    char name[DEVFS_MAX_FILE_NAME + 1];
    struct vnode *entry[DEVFS_MAX_DIR_ENTRY];
};

/* ---- UART device ---- */

static int uart_dev_open(struct vnode *file_node, struct file **target) {
    (*target)->vnode = file_node;
    (*target)->f_ops = file_node->f_ops;
    (*target)->f_pos = 0;
    return 0;
}

static int uart_dev_close(struct file *file) {
    free(file);
    return 0;
}

static int uart_dev_read(struct file *file, void *buf, size_t len) {
    (void)file;
    char *b = (char *)buf;
    for (size_t i = 0; i < len; i++) {
        b[i] = uart_async_getc();
        if (b[i] == '\r' || b[i] == '\n') {
            b[i] = '\n';
            return (int)(i + 1);
        }
    }
    return (int)len;
}

static int uart_dev_write(struct file *file, const void *buf, size_t len) {
    (void)file;
    const char *b = (const char *)buf;
    for (size_t i = 0; i < len; i++) {
        if (b[i] == '\n') uart_putc('\r');
        uart_putc(b[i]);
    }
    return (int)len;
}

/* ---- Framebuffer device ---- */

#define FB_WIDTH  1920
#define FB_HEIGHT 1080
#define FB_BPP    4

#ifdef QEMU
#define FB_BASE   (0x87000000UL + PAGE_OFFSET)
#else
#define FB_BASE   (0x7f700000UL + PAGE_OFFSET)
#endif

#define FB_IOCTL_GET_INFO 0

struct framebuffer_info {
    unsigned int width;
    unsigned int height;
    unsigned int bpp;
};

#ifdef QEMU

#define QEMU_PACKED __attribute__((packed))

static inline unsigned long bswap64(unsigned long x) {
    return ((x & 0x00000000000000FFULL) << 56) |
           ((x & 0x000000000000FF00ULL) << 40) |
           ((x & 0x0000000000FF0000ULL) << 24) |
           ((x & 0x00000000FF000000ULL) <<  8) |
           ((x & 0x000000FF00000000ULL) >>  8) |
           ((x & 0x0000FF0000000000ULL) >> 24) |
           ((x & 0x00FF000000000000ULL) >> 40) |
           ((x & 0xFF00000000000000ULL) >> 56);
}

static inline unsigned int bswap32(unsigned int x) {
    return ((x & 0x000000FF) << 24) |
           ((x & 0x0000FF00) <<  8) |
           ((x & 0x00FF0000) >>  8) |
           ((x & 0xFF000000) >> 24);
}

static inline unsigned short bswap16(unsigned short x) {
    return (unsigned short)(((x & 0x00FF) << 8) | ((x & 0xFF00) >> 8));
}

struct QEMU_PACKED RAMFBCfg {
    unsigned long  addr;
    unsigned int   fourcc;
    unsigned int   flags;
    unsigned int   width;
    unsigned int   height;
    unsigned int   stride;
};

#define FW_CFG_BASE   (0x10100000UL + PAGE_OFFSET)
#define FW_CFG_DMA    ((volatile unsigned long *)(FW_CFG_BASE + 0x10))
#define FW_CFG_DMA_CTL_READ   0x02
#define FW_CFG_DMA_CTL_SELECT 0x08
#define FW_CFG_DMA_CTL_WRITE  0x10
#define FW_CFG_DMA_CTL_ERROR  0x01
#define FW_CFG_FILE_DIR 0x19
#define XRGB8888 875713112
#define VA_TO_PA_DMA(va) ((unsigned long)(va) - PAGE_OFFSET)

struct QEMU_PACKED FWCfgFile {
    unsigned int   size;
    unsigned short select;
    unsigned short reserved;
    char name[56];
};

struct QEMU_PACKED FWCfgDmaAccess {
    unsigned int  control;
    unsigned int  length;
    unsigned long address;
};

extern int k_strncmp(const char *a, const char *b, unsigned long n);

static void fw_cfg_dma_transfer(void *address, unsigned int length, unsigned int control) {
    struct FWCfgDmaAccess access = {
        .control = bswap32(control),
        .length = bswap32(length),
        .address = bswap64(VA_TO_PA_DMA((unsigned long)address)),
    };
    *FW_CFG_DMA = bswap64(VA_TO_PA_DMA((unsigned long)&access));
    while (bswap32(access.control) & ~FW_CFG_DMA_CTL_ERROR)
        ;
}

static void fw_cfg_read_entry(void *buf, int e, int len) {
    unsigned int control = ((unsigned int)e << 16) | FW_CFG_DMA_CTL_SELECT | FW_CFG_DMA_CTL_READ;
    fw_cfg_dma_transfer(buf, (unsigned int)len, control);
}

static void fw_cfg_write_entry(void *buf, int e, int len) {
    unsigned int control = ((unsigned int)e << 16) | FW_CFG_DMA_CTL_SELECT | FW_CFG_DMA_CTL_WRITE;
    fw_cfg_dma_transfer(buf, (unsigned int)len, control);
}

static int fw_cfg_find_file(const char *name) {
    unsigned int count = 0;
    fw_cfg_read_entry(&count, FW_CFG_FILE_DIR, sizeof(count));
    count = bswap32(count);
    for (unsigned int i = 0; i < count; i++) {
        struct FWCfgFile file;
        fw_cfg_dma_transfer(&file, sizeof(file), FW_CFG_DMA_CTL_READ);
        if (k_strncmp(name, file.name, sizeof(file.name)) == 0)
            return bswap16(file.select);
    }
    return -1;
}

static void fb_init_ramfb(void) {
    int sel = fw_cfg_find_file("etc/ramfb");
    if (sel < 0) {
        uart_puts("fb: ramfb not found\r\n");
        return;
    }
    struct RAMFBCfg cfg = {
        .addr = bswap64(VA_TO_PA_DMA(FB_BASE)),
        .fourcc = bswap32(XRGB8888),
        .flags = bswap32(0),
        .width = bswap32(FB_WIDTH),
        .height = bswap32(FB_HEIGHT),
        .stride = bswap32(FB_WIDTH * FB_BPP),
    };
    fw_cfg_write_entry(&cfg, sel, sizeof(struct RAMFBCfg));
}

#else /* Board */

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
    }
    __sync_synchronize();
}

#endif

static int fb_dev_open(struct vnode *file_node, struct file **target) {
    (*target)->vnode = file_node;
    (*target)->f_ops = file_node->f_ops;
    (*target)->f_pos = 0;
#ifdef QEMU
    fb_init_ramfb();
#endif
    return 0;
}

static int fb_dev_close(struct file *file) {
    free(file);
    return 0;
}

static int fb_dev_write(struct file *file, const void *buf, size_t len) {
    unsigned char *fb = (unsigned char *)FB_BASE;
    unsigned long max_size = (unsigned long)FB_WIDTH * FB_HEIGHT * FB_BPP;
    if (file->f_pos + len > max_size)
        len = max_size - file->f_pos;
    k_memcpy(fb + file->f_pos, buf, len);
    file->f_pos += len;
    return (int)len;
}

static long fb_dev_lseek64(struct file *file, long offset, int whence) {
    if (whence == 0) {
#ifndef QEMU
        if (file->f_pos > 0 && (size_t)offset < file->f_pos)
            flush_dcache((void *)FB_BASE, (unsigned long)FB_WIDTH * FB_HEIGHT * FB_BPP);
#endif
        file->f_pos = (size_t)offset;
        return offset;
    }
    return -1;
}

static int fb_dev_ioctl(struct file *file, unsigned long request, void *arg) {
    (void)file;
    if (request == FB_IOCTL_GET_INFO) {
        struct framebuffer_info *info = (struct framebuffer_info *)arg;
        info->width = FB_WIDTH;
        info->height = FB_HEIGHT;
        info->bpp = FB_BPP;
        return 0;
    }
    return -1;
}

/* ---- devfs common ops ---- */

static struct file_operations uart_dev_fops;
static struct file_operations fb_dev_fops;
static struct file_operations devfs_dir_fops;
static struct vnode_operations devfs_vops;

static int devfs_lookup(struct vnode *dir_node, struct vnode **target,
                        const char *component_name) {
    struct devfs_vnode *d = (struct devfs_vnode *)dir_node->internal;
    if (d->type != DEVFS_DIR) return -1;
    for (int i = 0; i < DEVFS_MAX_DIR_ENTRY; i++) {
        if (!d->entry[i]) return -1;
        struct devfs_vnode *c = (struct devfs_vnode *)d->entry[i]->internal;
        if (k_strcmp(c->name, component_name) == 0) {
            *target = d->entry[i];
            return 0;
        }
    }
    return -1;
}

static int devfs_create(struct vnode *dir_node, struct vnode **target,
                        const char *component_name) {
    (void)dir_node; (void)target; (void)component_name;
    return -1;
}

static int devfs_mkdir_op(struct vnode *dir_node, struct vnode **target,
                          const char *component_name) {
    (void)dir_node; (void)target; (void)component_name;
    return -1;
}

static int devfs_ops_inited;

static void devfs_init_ops(void) {
    if (devfs_ops_inited) return;
    uart_dev_fops.open    = uart_dev_open;
    uart_dev_fops.close   = uart_dev_close;
    uart_dev_fops.read    = uart_dev_read;
    uart_dev_fops.write   = uart_dev_write;
    uart_dev_fops.lseek64 = 0;
    uart_dev_fops.ioctl   = 0;
    fb_dev_fops.open      = fb_dev_open;
    fb_dev_fops.close     = fb_dev_close;
    fb_dev_fops.read      = 0;
    fb_dev_fops.write     = fb_dev_write;
    fb_dev_fops.lseek64   = fb_dev_lseek64;
    fb_dev_fops.ioctl     = fb_dev_ioctl;
    devfs_dir_fops.open   = uart_dev_open;
    devfs_dir_fops.close  = uart_dev_close;
    devfs_dir_fops.read   = 0;
    devfs_dir_fops.write  = 0;
    devfs_dir_fops.lseek64 = 0;
    devfs_dir_fops.ioctl  = 0;
    devfs_vops.lookup = devfs_lookup;
    devfs_vops.create = devfs_create;
    devfs_vops.mkdir  = devfs_mkdir_op;
    devfs_ops_inited = 1;
}

int devfs_setup_mount(struct filesystem *fs, struct mount *mnt) {
    devfs_init_ops();

    struct vnode *root = (struct vnode *)allocate(sizeof(struct vnode));
    struct devfs_vnode *ri = (struct devfs_vnode *)allocate(sizeof(struct devfs_vnode));
    root->mount = 0;
    root->parent = root;
    root->internal = ri;
    root->v_ops = &devfs_vops;
    root->f_ops = &devfs_dir_fops;
    ri->type = DEVFS_DIR;
    ri->name[0] = '\0';
    k_memset(ri->entry, 0, sizeof(ri->entry));

    struct vnode *uart = (struct vnode *)allocate(sizeof(struct vnode));
    struct devfs_vnode *ui = (struct devfs_vnode *)allocate(sizeof(struct devfs_vnode));
    uart->mount = 0;
    uart->parent = root;
    uart->internal = ui;
    uart->v_ops = &devfs_vops;
    uart->f_ops = &uart_dev_fops;
    ui->type = DEVFS_FILE;
    k_strncpy(ui->name, "uart", DEVFS_MAX_FILE_NAME);
    ui->name[DEVFS_MAX_FILE_NAME] = '\0';
    k_memset(ui->entry, 0, sizeof(ui->entry));

    struct vnode *fb = (struct vnode *)allocate(sizeof(struct vnode));
    struct devfs_vnode *fi = (struct devfs_vnode *)allocate(sizeof(struct devfs_vnode));
    fb->mount = 0;
    fb->parent = root;
    fb->internal = fi;
    fb->v_ops = &devfs_vops;
    fb->f_ops = &fb_dev_fops;
    fi->type = DEVFS_FILE;
    k_strncpy(fi->name, "fb", DEVFS_MAX_FILE_NAME);
    fi->name[DEVFS_MAX_FILE_NAME] = '\0';
    k_memset(fi->entry, 0, sizeof(fi->entry));

    ri->entry[0] = uart;
    ri->entry[1] = fb;

    mnt->root = root;
    mnt->fs = fs;
    return 0;
}

void devfs_init(void) {
    struct filesystem devfs;
    devfs.name = "devfs";
    devfs.setup_mount = devfs_setup_mount;
    register_filesystem(&devfs);

    vfs_mkdir("/dev");
    vfs_mount("/dev", "devfs");
}
