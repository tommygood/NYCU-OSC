#include "vfs.h"
#include "mm.h"

extern void *k_memset(void *s, int c, unsigned long n);
extern int k_strcmp(const char *a, const char *b);
extern char *k_strncpy(char *dst, const char *src, unsigned long n);
extern void uart_putc(char c);
extern char uart_async_getc(void);

#define DEVFS_MAX_FILE_NAME 15
#define DEVFS_MAX_DIR_ENTRY 16

enum devfs_type { DEVFS_DIR, DEVFS_FILE };

struct devfs_vnode {
    enum devfs_type type;
    char name[DEVFS_MAX_FILE_NAME + 1];
    struct vnode *entry[DEVFS_MAX_DIR_ENTRY];
};

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

static struct file_operations uart_dev_fops;
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
    devfs_dir_fops.open   = uart_dev_open;
    devfs_dir_fops.close  = uart_dev_close;
    devfs_dir_fops.read   = 0;
    devfs_dir_fops.write  = 0;
    devfs_dir_fops.lseek64 = 0;
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

    ri->entry[0] = uart;

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
