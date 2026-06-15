#include "vfs.h"
#include "mm.h"

extern void  *k_memcpy(void *dst, const void *src, unsigned long n);
extern void  *k_memset(void *s, int c, unsigned long n);
extern int    k_strcmp(const char *a, const char *b);
extern char  *k_strncpy(char *dst, const char *src, unsigned long n);
extern int    k_strncmp(const char *a, const char *b, unsigned long n);

extern const void *g_initrd;

#define RAMFS_MAX_FILE_NAME 15
#define RAMFS_MAX_DIR_ENTRY 32

enum ramfs_type { RAMFS_DIR, RAMFS_FILE };

struct ramfs_vnode {
    enum ramfs_type type;
    char name[RAMFS_MAX_FILE_NAME + 1];
    struct vnode *entry[RAMFS_MAX_DIR_ENTRY];
    const char *data;
    size_t size;
};

struct cpio_hdr {
    char magic[6];
    char ino[8];
    char mode[8];
    char uid[8];
    char gid[8];
    char nlink[8];
    char mtime[8];
    char filesize[8];
    char devmajor[8];
    char devminor[8];
    char rdevmajor[8];
    char rdevminor[8];
    char namesize[8];
    char check[8];
};

static unsigned int cpio_hex(const char *s, int n) {
    unsigned int r = 0;
    while (n-- > 0) {
        r <<= 4;
        if (*s >= 'A') r += *s++ - 'A' + 10;
        else            r += *s++ - '0';
    }
    return r;
}

static int align4(int n) { return (n + 3) & ~3; }

static int ramfs_open(struct vnode *file_node, struct file **target);
static int ramfs_close(struct file *file);
static int ramfs_read(struct file *file, void *buf, size_t len);
static int ramfs_write(struct file *file, const void *buf, size_t len);
static int ramfs_lookup(struct vnode *dir_node, struct vnode **target,
                        const char *component_name);
static int ramfs_create(struct vnode *dir_node, struct vnode **target,
                        const char *component_name);
static int ramfs_mkdir_op(struct vnode *dir_node, struct vnode **target,
                          const char *component_name);

static struct file_operations ramfs_file_ops;
static struct vnode_operations ramfs_vnode_ops;
static int ramfs_ops_inited;

static void ramfs_init_ops(void) {
    if (ramfs_ops_inited) return;
    ramfs_file_ops.open    = ramfs_open;
    ramfs_file_ops.close   = ramfs_close;
    ramfs_file_ops.read    = ramfs_read;
    ramfs_file_ops.write   = ramfs_write;
    ramfs_file_ops.lseek64 = 0;
    ramfs_file_ops.ioctl = 0;
    ramfs_vnode_ops.lookup = ramfs_lookup;
    ramfs_vnode_ops.create = ramfs_create;
    ramfs_vnode_ops.mkdir  = ramfs_mkdir_op;
    ramfs_ops_inited = 1;
}

static struct vnode *ramfs_create_vnode(enum ramfs_type type) {
    struct vnode *node = (struct vnode *)allocate(sizeof(struct vnode));
    if (!node) return 0;
    struct ramfs_vnode *inode = (struct ramfs_vnode *)allocate(sizeof(struct ramfs_vnode));
    if (!inode) { free(node); return 0; }
    node->mount = 0;
    node->parent = node;
    node->internal = inode;
    node->v_ops = &ramfs_vnode_ops;
    node->f_ops = &ramfs_file_ops;
    inode->type = type;
    inode->name[0] = '\0';
    k_memset(inode->entry, 0, sizeof(inode->entry));
    inode->data = 0;
    inode->size = 0;
    return node;
}

int ramfs_setup_mount(struct filesystem *fs, struct mount *mnt) {
    ramfs_init_ops();

    struct vnode *root = ramfs_create_vnode(RAMFS_DIR);
    mnt->root = root;
    mnt->fs = fs;

    if (!g_initrd) return 0;

    const unsigned char *p = (const unsigned char *)g_initrd;
    struct ramfs_vnode *root_inode = (struct ramfs_vnode *)root->internal;
    int idx = 0;

    while (1) {
        const struct cpio_hdr *hdr = (const struct cpio_hdr *)p;
        if (k_strncmp(hdr->magic, "070701", 6) != 0) break;

        int namesize = (int)cpio_hex(hdr->namesize, 8);
        int filesize = (int)cpio_hex(hdr->filesize, 8);
        const char *name = (const char *)p + sizeof(struct cpio_hdr);

        if (k_strcmp(name, "TRAILER!!!") == 0) break;

        const char *data = (const char *)p
            + align4((int)sizeof(struct cpio_hdr) + namesize);
        p += align4((int)sizeof(struct cpio_hdr) + namesize)
           + align4(filesize);

        if (k_strcmp(name, ".") == 0) continue;
        if (filesize == 0) continue;
        if (idx >= RAMFS_MAX_DIR_ENTRY) break;

        struct vnode *fnode = ramfs_create_vnode(RAMFS_FILE);
        if (!fnode) continue;
        fnode->parent = root;

        struct ramfs_vnode *fi = (struct ramfs_vnode *)fnode->internal;
        k_strncpy(fi->name, name, RAMFS_MAX_FILE_NAME);
        fi->name[RAMFS_MAX_FILE_NAME] = '\0';
        fi->data = data;
        fi->size = (size_t)filesize;

        root_inode->entry[idx++] = fnode;
    }

    return 0;
}

static int ramfs_open(struct vnode *file_node, struct file **target) {
    (*target)->vnode = file_node;
    (*target)->f_ops = &ramfs_file_ops;
    (*target)->f_pos = 0;
    return 0;
}

static int ramfs_close(struct file *file) {
    free(file);
    return 0;
}

static int ramfs_read(struct file *file, void *buf, size_t len) {
    struct ramfs_vnode *inode = (struct ramfs_vnode *)file->vnode->internal;
    if (!inode->data) return 0;
    size_t remaining = inode->size - file->f_pos;
    if (len > remaining) len = remaining;
    k_memcpy(buf, (void *)(inode->data + file->f_pos), len);
    file->f_pos += len;
    return (int)len;
}

static int ramfs_write(struct file *file, const void *buf, size_t len) {
    (void)file; (void)buf; (void)len;
    return -1;
}

static int ramfs_lookup(struct vnode *dir_node, struct vnode **target,
                        const char *component_name) {
    struct ramfs_vnode *dentry = (struct ramfs_vnode *)dir_node->internal;
    if (dentry->type != RAMFS_DIR) return -1;
    for (int i = 0; i < RAMFS_MAX_DIR_ENTRY; i++) {
        if (!dentry->entry[i]) return -1;
        struct ramfs_vnode *inode =
            (struct ramfs_vnode *)dentry->entry[i]->internal;
        if (k_strcmp(inode->name, component_name) == 0) {
            *target = dentry->entry[i];
            return 0;
        }
    }
    return -1;
}

static int ramfs_create(struct vnode *dir_node, struct vnode **target,
                        const char *component_name) {
    (void)dir_node; (void)target; (void)component_name;
    return -1;
}

static int ramfs_mkdir_op(struct vnode *dir_node, struct vnode **target,
                          const char *component_name) {
    (void)dir_node; (void)target; (void)component_name;
    return -1;
}

void ramfs_init(void) {
    struct filesystem ramfs;
    ramfs.name = "ramfs";
    ramfs.setup_mount = ramfs_setup_mount;
    register_filesystem(&ramfs);

    vfs_mkdir("/ramfs");
    vfs_mount("/ramfs", "ramfs");
}
