#include "vfs.h"
#include "mm.h"
#include "thread.h"

extern size_t k_strlen(const char *s);
extern int    k_strcmp(const char *a, const char *b);
extern void  *k_memset(void *s, int c, unsigned long n);

extern int tmpfs_setup_mount(struct filesystem *fs, struct mount *mount);

struct mount *rootfs;
static struct filesystem fs_list[MAX_FS];

static struct vnode *get_cwd(void) {
    struct task_struct *cur = get_current();
    if (cur && cur->cwd)
        return cur->cwd;
    return rootfs->root;
}

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

static void resolve_component(struct vnode **node, const char *component) {
    /* This is only called for "." and ".." — regular lookup is done inline */
    if (k_strcmp(component, ".") == 0) {
        return;
    }
    if (k_strcmp(component, "..") == 0) {
        *node = (*node)->parent;
    }
}

int vfs_lookup(const char *pathname, struct vnode **target) {
    if (k_strlen(pathname) == 0) {
        *target = get_cwd();
        return 0;
    }

    struct vnode *node;
    if (pathname[0] == '/')
        node = rootfs->root;
    else
        node = get_cwd();

    while (node->mount)
        node = node->mount->root;

    char component[MAX_PATH];
    int idx = 0;

    for (int i = 0; pathname[i]; i++) {
        if (pathname[i] == '/') {
            component[idx] = '\0';
            if (idx == 0) { continue; }
            if (k_strcmp(component, ".") == 0 || k_strcmp(component, "..") == 0) {
                resolve_component(&node, component);
            } else {
                if (node->v_ops->lookup(node, &node, component) != 0)
                    return -1;
                while (node->mount)
                    node = node->mount->root;
            }
            idx = 0;
        } else {
            component[idx++] = pathname[i];
        }
    }
    component[idx] = '\0';

    if (idx > 0) {
        if (k_strcmp(component, ".") == 0 || k_strcmp(component, "..") == 0) {
            resolve_component(&node, component);
        } else {
            if (node->v_ops->lookup(node, &node, component) != 0)
                return -1;
            while (node->mount)
                node = node->mount->root;
        }
    }

    *target = node;
    return 0;
}

int vfs_open(const char *pathname, int flags, struct file **target) {
    struct vnode *vnode;
    if (vfs_lookup(pathname, &vnode) != 0) {
        if (!(flags & O_CREAT))
            return -1;

        int pos = -1;
        for (int i = 0; pathname[i]; i++)
            if (pathname[i] == '/')
                pos = i;

        char dirname[MAX_PATH];
        k_memset(dirname, 0, MAX_PATH);
        const char *filename;

        if (pos < 0) {
            filename = pathname;
        } else if (pos == 0) {
            dirname[0] = '/';
            filename = pathname + 1;
        } else {
            for (int i = 0; i < pos; i++)
                dirname[i] = pathname[i];
            filename = pathname + pos + 1;
        }

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

int vfs_mkdir(const char *pathname) {
    struct vnode *vnode;
    if (vfs_lookup(pathname, &vnode) == 0)
        return -1;

    int pos = -1;
    for (int i = 0; pathname[i]; i++)
        if (pathname[i] == '/')
            pos = i;

    char dirname[MAX_PATH];
    k_memset(dirname, 0, MAX_PATH);
    const char *basename;

    if (pos < 0) {
        basename = pathname;
    } else if (pos == 0) {
        dirname[0] = '/';
        basename = pathname + 1;
    } else {
        for (int i = 0; i < pos; i++)
            dirname[i] = pathname[i];
        basename = pathname + pos + 1;
    }

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
    mnt->mount_point = mount_point;
    mnt->root->parent = mount_point->parent;
    mount_point->mount = mnt;
    return 0;
}

int vfs_chdir(const char *path) {
    struct vnode *node;
    if (vfs_lookup(path, &node) != 0)
        return -1;
    get_current()->cwd = node;
    return 0;
}

long vfs_lseek64(struct file *file, long offset, int whence) {
    if (file->f_ops->lseek64)
        return file->f_ops->lseek64(file, offset, whence);
    if (whence == 0) {
        file->f_pos = (size_t)offset;
        return offset;
    }
    return -1;
}

int vfs_ioctl(struct file *file, unsigned long request, void *arg) {
    if (file->f_ops->ioctl)
        return file->f_ops->ioctl(file, request, arg);
    return -1;
}

void rootfs_init(void) {
    rootfs = (struct mount *)allocate(sizeof(struct mount));
    rootfs->mount_point = 0;
    struct filesystem tmpfs;
    tmpfs.name = "tmpfs";
    tmpfs.setup_mount = tmpfs_setup_mount;
    int id = register_filesystem(&tmpfs);
    fs_list[id].setup_mount(&fs_list[id], rootfs);
}
