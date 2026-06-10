#include "vfs.h"
#include "mm.h"

extern size_t k_strlen(const char *s);
extern int    k_strcmp(const char *a, const char *b);
extern void  *k_memset(void *s, int c, unsigned long n);

extern int tmpfs_setup_mount(struct filesystem *fs, struct mount *mount);

struct mount *rootfs;
static struct filesystem fs_list[MAX_FS];

int register_filesystem(struct filesystem *fs) {
    for (int i = 0; i < MAX_FS; i++) {
        if (fs_list[i].name == 0) {
            fs_list[i].name = fs->name;
            fs_list[i].setup_mount = fs->setup_mount;
            return i;
        }
    }
    return -1;
}

int vfs_open(const char *pathname, int flags, struct file **target) {
    struct vnode *vnode;
    if (vfs_lookup(pathname, &vnode) != 0) {
        if (!(flags & O_CREAT))
            return -1;
        int pos = 0;
        for (int i = 0; pathname[i]; i++)
            if (pathname[i] == '/')
                pos = i;
        char dirname[MAX_PATH];
        k_memset(dirname, 0, MAX_PATH);
        for (int i = 0; i < pos; i++)
            dirname[i] = pathname[i];
        const char *filename = pathname + pos + 1;
        if (vfs_lookup(dirname, &vnode) != 0)
            return -1;
        if (vnode->v_ops->create(vnode, &vnode, filename) != 0)
            return -1;
    }
    struct file *f = (struct file *)allocate(sizeof(struct file));
    if (!f) return -1;
    f->flags = flags;
    vnode->f_ops->open(vnode, &f);
    *target = f;
    return 0;
}

int vfs_close(struct file *file) {
    return file->f_ops->close(file);
}

int vfs_read(struct file *file, void *buf, size_t len) {
    return file->f_ops->read(file, buf, len);
}

int vfs_write(struct file *file, const void *buf, size_t len) {
    return file->f_ops->write(file, buf, len);
}

int vfs_lookup(const char *pathname, struct vnode **target) {
    // target is output parameter for the vnode of this pathname
    if (k_strlen(pathname) == 0) {
        *target = rootfs->root;
        return 0;
    }

    struct vnode *node = rootfs->root;
    // we can change root by mount, so we need to follow the mount to get the real root
    while (node->mount)
        node = node->mount->root;

    char component[MAX_PATH];
    int idx = 0;

    for (int i = 0; pathname[i]; i++) {
        if (pathname[i] == '/') {
            component[idx] = '\0';
            if (idx == 0) continue;
            if (node->v_ops->lookup(node, &node, component) != 0)
                return -1;
            while (node->mount)
                node = node->mount->root;
            idx = 0;
        } else {
            component[idx++] = pathname[i];
        }
    }
    component[idx] = '\0';

    if (idx > 0) {
        if (node->v_ops->lookup(node, &node, component) != 0)
            return -1;
        while (node->mount)
            node = node->mount->root;
    }

    *target = node;
    return 0;
}

int vfs_mkdir(const char *pathname) {
    struct vnode *vnode;
    if (vfs_lookup(pathname, &vnode) == 0)
        return -1;

    int pos = 0;
    for (int i = 0; pathname[i]; i++)
        if (pathname[i] == '/')
            pos = i;

    char dirname[MAX_PATH];
    k_memset(dirname, 0, MAX_PATH);
    for (int i = 0; i < pos; i++)
        dirname[i] = pathname[i];
    const char *basename = pathname + pos + 1;

    if (vfs_lookup(dirname, &vnode) != 0)
        return -1;
    if (!vnode->v_ops->mkdir)
        return -1;

    struct vnode *new_dir;
    return vnode->v_ops->mkdir(vnode, &new_dir, basename);
}

int vfs_mount(const char *target, const char *filesystem) {
    struct vnode *mount_point;
    if (vfs_lookup(target, &mount_point) != 0)
        return -1;

    struct filesystem *fs = 0;
    for (int i = 0; i < MAX_FS; i++) {
        if (fs_list[i].name && k_strcmp(fs_list[i].name, filesystem) == 0) {
            fs = &fs_list[i];
            break;
        }
    }
    if (!fs) return -1;

    struct mount *mnt = (struct mount *)allocate(sizeof(struct mount));
    if (!mnt) return -1;

    fs->setup_mount(fs, mnt);
    mount_point->mount = mnt;
    return 0;
}

void rootfs_init(void) {
    rootfs = (struct mount *)allocate(sizeof(struct mount));
    struct filesystem tmpfs;
    tmpfs.name = "tmpfs";
    tmpfs.setup_mount = tmpfs_setup_mount;
    int id = register_filesystem(&tmpfs);
    fs_list[id].setup_mount(&fs_list[id], rootfs);
}
