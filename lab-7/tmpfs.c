#include "vfs.h"
#include "mm.h"

extern void  *k_memcpy(void *dst, const void *src, unsigned long n);
extern void  *k_memset(void *s, int c, unsigned long n);
extern int    k_strcmp(const char *a, const char *b);
extern char  *k_strncpy(char *dst, const char *src, unsigned long n);

#define TMPFS_MAX_FILE_NAME 15
#define TMPFS_MAX_DIR_ENTRY 16
#define TMPFS_MAX_FILE_SIZE 4096

enum fsnode_type { FS_DIR, FS_FILE };

struct tmpfs_vnode {
    enum fsnode_type type;
    char name[TMPFS_MAX_FILE_NAME + 1];
    struct vnode *entry[TMPFS_MAX_DIR_ENTRY];
    char *data;
    size_t size;
};

static int tmpfs_open(struct vnode *file_node, struct file **target);
static int tmpfs_close(struct file *file);
static int tmpfs_read(struct file *file, void *buf, size_t len);
static int tmpfs_write(struct file *file, const void *buf, size_t len);
static int tmpfs_lookup(struct vnode *dir_node, struct vnode **target,
                        const char *component_name);
static int tmpfs_create(struct vnode *dir_node, struct vnode **target,
                        const char *component_name);
static int tmpfs_mkdir_op(struct vnode *dir_node, struct vnode **target,
                          const char *component_name);

static struct file_operations tmpfs_file_ops;
static struct vnode_operations tmpfs_vnode_ops;
static int tmpfs_ops_initialized;

static void tmpfs_init_ops(void) {
    if (tmpfs_ops_initialized) return;
    tmpfs_file_ops.open = tmpfs_open;
    tmpfs_file_ops.close = tmpfs_close;
    tmpfs_file_ops.read = tmpfs_read;
    tmpfs_file_ops.write = tmpfs_write;
    tmpfs_file_ops.lseek64 = 0;
    tmpfs_vnode_ops.lookup = tmpfs_lookup;
    tmpfs_vnode_ops.create = tmpfs_create;
    tmpfs_vnode_ops.mkdir = tmpfs_mkdir_op;
    tmpfs_ops_initialized = 1;
}

static struct vnode *tmpfs_create_vnode(enum fsnode_type type) {
    struct vnode *node = (struct vnode *)allocate(sizeof(struct vnode));
    if (!node) return 0;
    struct tmpfs_vnode *inode = (struct tmpfs_vnode *)allocate(sizeof(struct tmpfs_vnode));
    if (!inode) { free(node); return 0; }
    node->mount = 0;
    node->internal = inode;
    node->v_ops = &tmpfs_vnode_ops;
    node->f_ops = &tmpfs_file_ops;
    inode->type = type;
    inode->name[0] = '\0';
    k_memset(inode->entry, 0, sizeof(inode->entry));
    inode->data = 0;
    inode->size = 0;
    return node;
}

int tmpfs_setup_mount(struct filesystem *fs, struct mount *mnt) {
    tmpfs_init_ops();
    mnt->root = tmpfs_create_vnode(FS_DIR);
    mnt->fs = fs;
    return 0;
}

static int tmpfs_open(struct vnode *file_node, struct file **target) {
    (*target)->vnode = file_node;
    (*target)->f_ops = &tmpfs_file_ops;
    (*target)->f_pos = 0;
    return 0;
}

static int tmpfs_close(struct file *file) {
    free(file);
    return 0;
}

static int tmpfs_read(struct file *file, void *buf, size_t len) {
    struct tmpfs_vnode *inode = (struct tmpfs_vnode *)file->vnode->internal;
    if (!inode->data)
        return 0;
    size_t remaining = inode->size - file->f_pos;
    if (len > remaining)
        len = remaining;
    k_memcpy(buf, inode->data + file->f_pos, len);
    file->f_pos += len;
    return (int)len;
}

static int tmpfs_write(struct file *file, const void *buf, size_t len) {
    struct tmpfs_vnode *inode = (struct tmpfs_vnode *)file->vnode->internal;
    if (!inode->data) {
        inode->data = (char *)allocate(TMPFS_MAX_FILE_SIZE);
        if (!inode->data) return -1;
    }
    k_memcpy(inode->data + file->f_pos, buf, len);
    file->f_pos += len;
    // update size of inode if the file position after writing exceeds current size
    if (file->f_pos > inode->size)
        inode->size = file->f_pos;
    return (int)len;
}

static int tmpfs_lookup(struct vnode *dir_node, struct vnode **target,
                        const char *component_name) {
    struct tmpfs_vnode *dentry = (struct tmpfs_vnode *)dir_node->internal;
    for (int i = 0; i < TMPFS_MAX_DIR_ENTRY; i++) {
        if (!dentry->entry[i])
            return -1;
        struct tmpfs_vnode *inode = (struct tmpfs_vnode *)dentry->entry[i]->internal;
        if (k_strcmp(inode->name, component_name) == 0) {
            *target = dentry->entry[i];
            return 0;
        }
    }
    return -1;
}

static int tmpfs_create(struct vnode *dir_node, struct vnode **target,
                        const char *component_name) {
    struct tmpfs_vnode *dir = (struct tmpfs_vnode *)dir_node->internal;
    // check if the file already exists
    for (int i = 0; i < TMPFS_MAX_DIR_ENTRY; i++) {
        if (!dir->entry[i]) continue;
        struct tmpfs_vnode *inode = (struct tmpfs_vnode *)dir->entry[i]->internal;
        if (k_strcmp(inode->name, component_name) == 0)
            return -1;
    }

    // create the vnode for new file
    struct vnode *node = tmpfs_create_vnode(FS_FILE);
    if (!node) return -1;
    
    struct tmpfs_vnode *inode = (struct tmpfs_vnode *)node->internal;
    k_strncpy(inode->name, component_name, TMPFS_MAX_FILE_NAME);
    // set last char to null terminator in case component_name as longer than TMPFS_MAX_FILE_NAME
    inode->name[TMPFS_MAX_FILE_NAME] = '\0';
    for (int i = 0; i < TMPFS_MAX_DIR_ENTRY; i++) {
        if (!dir->entry[i]) {
            dir->entry[i] = node;
            break;
        }
    }
    *target = node;
    return 0;
}

static int tmpfs_mkdir_op(struct vnode *dir_node, struct vnode **target,
                          const char *component_name) {
    struct tmpfs_vnode *dir = (struct tmpfs_vnode *)dir_node->internal;
    for (int i = 0; i < TMPFS_MAX_DIR_ENTRY; i++) {
        if (!dir->entry[i]) continue;
        struct tmpfs_vnode *inode = (struct tmpfs_vnode *)dir->entry[i]->internal;
        if (k_strcmp(inode->name, component_name) == 0)
            return -1;
    }
    struct vnode *node = tmpfs_create_vnode(FS_DIR);
    if (!node) return -1;
    struct tmpfs_vnode *inode = (struct tmpfs_vnode *)node->internal;
    k_strncpy(inode->name, component_name, TMPFS_MAX_FILE_NAME);
    inode->name[TMPFS_MAX_FILE_NAME] = '\0';
    for (int i = 0; i < TMPFS_MAX_DIR_ENTRY; i++) {
        if (!dir->entry[i]) {
            dir->entry[i] = node;
            break;
        }
    }
    *target = node;
    return 0;
}
