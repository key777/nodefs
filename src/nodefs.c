#define FUSE_USE_VERSION 31
#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <fcntl.h>
#include <fuse3/fuse.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "list.h"
#include "nodefs.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef enum {
    NODEFS_ENTRY_DIR,
    NODEFS_ENTRY_FILE,
} nodefs_entry_type_t;

typedef struct _nodefs_entry_t {
    char *name;
    nodefs_entry_type_t type;
    const nodefs_node_t *node;
    struct _nodefs_entry_t *parent;
    struct list_head children;
    struct list_head sibling;
} nodefs_entry_t;

typedef struct {
    nodefs_entry_t *entry;
    struct fuse_pollhandle *ph;
    struct list_head link;
} nodefs_handle_t;

struct nodefs {
    nodefs_entry_t root;
    pthread_rwlock_t lock;
    struct fuse *fuse;
    struct list_head handles;
};

typedef struct _nodefs_mount_t {
    nodefs_t *fs;
} nodefs_mount_t;

static void nodefs_log_error(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    fprintf(stderr, "nodefs: ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

static void* nodefs_get_userdata(void)
{
    struct fuse_context *ctx = fuse_get_context();

    if (ctx == NULL || ctx->private_data == NULL) {
        return NULL;
    }
    return ctx->private_data;
}

static int nodefs_valid_path_segment(const char *segment, size_t len)
{
    if (segment == NULL || len == 0) {
        return 0;
    }
    if ((len == 1 && segment[0] == '.') ||
        (len == 2 && segment[0] == '.' && segment[1] == '.')) {
        return 0;
    }
    return memchr(segment, '/', len) == NULL;
}

static int nodefs_path_next_segment(const char **cursor,
                                    const char **segment,
                                    size_t *len)
{
    const char *start;
    const char *slash;

    if (cursor == NULL || *cursor == NULL || segment == NULL || len == NULL) {
        return -EINVAL;
    }

    start = *cursor;
    if (*start == '\0') {
        return 0;
    }

    slash = strchr(start, '/');
    if (slash == NULL) {
        *segment = start;
        *len = strlen(start);
        *cursor = start + *len;
    } else {
        *segment = start;
        *len = (size_t)(slash - start);
        *cursor = slash + 1;
    }

    if (!nodefs_valid_path_segment(*segment, *len)) {
        return -EINVAL;
    }

    return 1;
}

static int nodefs_valid_node_path(const char *path)
{
    const char *cursor = path;
    const char *segment;
    size_t len;
    int ret;

    if (path == NULL || path[0] == '\0' || path[0] == '/') {
        return 0;
    }
    if (path[strlen(path) - 1] == '/') {
        return 0;
    }

    do {
        ret = nodefs_path_next_segment(&cursor, &segment, &len);
        if (ret < 0) {
            return 0;
        }
    } while (ret > 0);

    return 1;
}

static nodefs_entry_t* nodefs_find_child(nodefs_entry_t *parent,
                                         const char *name,
                                         size_t name_len)
{
    nodefs_entry_t *entry;

    if (parent == NULL || parent->type != NODEFS_ENTRY_DIR) {
        return NULL;
    }

    list_for_each_entry(entry, &parent->children, sibling) {
        if (strlen(entry->name) == name_len &&
            strncmp(entry->name, name, name_len) == 0) {
            return entry;
        }
    }
    return NULL;
}

static char* nodefs_strndup(const char *str, size_t len)
{
    char *dup = malloc(len + 1);

    if (dup == NULL) {
        return NULL;
    }

    memcpy(dup, str, len);
    dup[len] = '\0';
    return dup;
}

static nodefs_entry_t* nodefs_entry_create(const char *name, size_t name_len,
                                           nodefs_entry_type_t type)
{
    nodefs_entry_t *entry = malloc(sizeof(*entry));

    if (entry == NULL) {
        return NULL;
    }

    entry->name = nodefs_strndup(name, name_len);
    if (entry->name == NULL) {
        free(entry);
        return NULL;
    }

    entry->type = type;
    entry->node = NULL;
    entry->parent = NULL;
    INIT_LIST_HEAD(&entry->children);
    INIT_LIST_HEAD(&entry->sibling);
    return entry;
}

static void nodefs_entry_destroy(nodefs_entry_t *entry)
{
    nodefs_entry_t *child;
    nodefs_entry_t *next;

    if (entry == NULL) {
        return;
    }

    if (!list_empty(&entry->children)) {
        list_for_each_entry_safe(child, next, &entry->children, sibling) {
            list_del(&child->sibling);
            nodefs_entry_destroy(child);
        }
    }

    free(entry->name);
    free(entry);
}

static nodefs_entry_t* nodefs_find_path(nodefs_t *fs, const char *path)
{
    nodefs_entry_t *entry;
    const char *cursor;
    const char *segment;
    size_t len;
    int ret;

    if (fs == NULL || path == NULL) {
        return NULL;
    }
    if (strcmp(path, "/") == 0 || path[0] == '\0') {
        return &fs->root;
    }
    if (path[0] != '/') {
        return NULL;
    }

    entry = &fs->root;
    cursor = path + 1;
    do {
        ret = nodefs_path_next_segment(&cursor, &segment, &len);
        if (ret < 0) {
            return NULL;
        }
        if (ret == 0) {
            return entry;
        }
        entry = nodefs_find_child(entry, segment, len);
        if (entry == NULL) {
            return NULL;
        }
    } while (ret > 0);

    return entry;
}

static char* nodefs_make_absolute_path(const char *name)
{
    size_t name_len;
    char *path;

    if (name == NULL) {
        return NULL;
    }

    name_len = strlen(name);
    if (name_len > SIZE_MAX - 2) {
        return NULL;
    }

    path = malloc(name_len + 2);
    if (path == NULL) {
        return NULL;
    }

    path[0] = '/';
    memcpy(path + 1, name, name_len + 1);
    return path;
}

/*
 * Invalidate the changed path and all of its parent directories.  Cache
 * timeouts are disabled as a fallback, but explicit invalidation makes
 * changes visible sooner when the filesystem is already mounted.
 */
static void nodefs_invalidate_paths(nodefs_t *fs, const char *name)
{
    struct fuse *fuse;
    char *path;
    char *cursor;

    if (fs == NULL || name == NULL) {
        return;
    }

    path = nodefs_make_absolute_path(name);
    if (path == NULL) {
        return;
    }

    pthread_rwlock_rdlock(&fs->lock);
    fuse = fs->fuse;
    if (fuse != NULL) {
        fuse_invalidate_path(fuse, "/");
        for (cursor = path + 1; *cursor != '\0'; cursor++) {
            if (*cursor != '/') {
                continue;
            }

            *cursor = '\0';
            fuse_invalidate_path(fuse, path);
            *cursor = '/';
        }
        fuse_invalidate_path(fuse, path);
    }
    pthread_rwlock_unlock(&fs->lock);

    free(path);
}

static nodefs_entry_t* nodefs_insert_path(nodefs_t *fs, const nodefs_node_t *node)
{
    nodefs_entry_t *parent;
    nodefs_entry_t *entry;
    const char *cursor;
    const char *segment;
    size_t len;
    int ret;

    if (fs == NULL || node == NULL || !nodefs_valid_node_path(node->name)) {
        errno = EINVAL;
        return NULL;
    }

    parent = &fs->root;
    cursor = node->name;
    do {
        ret = nodefs_path_next_segment(&cursor, &segment, &len);
        if (ret <= 0) {
            errno = EINVAL;
            return NULL;
        }

        entry = nodefs_find_child(parent, segment, len);
        if (*cursor == '\0') {
            if (entry != NULL) {
                errno = EEXIST;
                return NULL;
            }

            entry = nodefs_entry_create(segment, len, NODEFS_ENTRY_FILE);
            if (entry == NULL) {
                errno = ENOMEM;
                return NULL;
            }
            entry->node = node;
            entry->parent = parent;
            list_add_tail(&entry->sibling, &parent->children);
            return entry;
        }

        if (entry == NULL) {
            entry = nodefs_entry_create(segment, len, NODEFS_ENTRY_DIR);
            if (entry == NULL) {
                errno = ENOMEM;
                return NULL;
            }
            entry->parent = parent;
            list_add_tail(&entry->sibling, &parent->children);
        } else if (entry->type != NODEFS_ENTRY_DIR) {
            errno = ENOTDIR;
            return NULL;
        }

        parent = entry;
    } while (ret > 0);

    errno = EINVAL;
    return NULL;
}

nodefs_t* nodefs_create(void)
{
    nodefs_t *fs = malloc(sizeof(*fs));

    if (fs == NULL) {
        return NULL;
    }

    if (pthread_rwlock_init(&fs->lock, NULL) != 0) {
        free(fs);
        return NULL;
    }

    fs->root.name = NULL;
    fs->root.type = NODEFS_ENTRY_DIR;
    fs->root.node = NULL;
    fs->root.parent = NULL;
    INIT_LIST_HEAD(&fs->root.children);
    INIT_LIST_HEAD(&fs->root.sibling);
    fs->fuse = NULL;
    INIT_LIST_HEAD(&fs->handles);
    return fs;
}

/* Caller holds the filesystem write lock. */
static void nodefs_clear_handles(nodefs_t *fs)
{
    nodefs_handle_t *handle, *next;

    list_for_each_entry_safe(handle, next, &fs->handles, link) {
        if (handle->ph != NULL)
            fuse_pollhandle_destroy(handle->ph);
        list_del(&handle->link);
        free(handle);
    }
}

static void nodefs_clear(nodefs_t *fs)
{
    nodefs_entry_t *entry;
    nodefs_entry_t *next;

    if (fs == NULL || list_empty(&fs->root.children)) {
        return;
    }

    list_for_each_entry_safe(entry, next, &fs->root.children, sibling) {
        list_del(&entry->sibling);
        nodefs_entry_destroy(entry);
    }

    INIT_LIST_HEAD(&fs->root.children);
}

void nodefs_destroy(nodefs_t *fs)
{
    if (fs == NULL) {
        return;
    }

    pthread_rwlock_wrlock(&fs->lock);
    nodefs_clear_handles(fs);
    nodefs_clear(fs);
    pthread_rwlock_unlock(&fs->lock);
    pthread_rwlock_destroy(&fs->lock);
    free(fs);
}

int nodefs_register(nodefs_t *fs, const nodefs_node_t *node)
{
    nodefs_entry_t *entry;
    int ret;

    errno = 0;
    if (fs == NULL || node == NULL) {
        return -EINVAL;
    }

    pthread_rwlock_wrlock(&fs->lock);
    entry = nodefs_insert_path(fs, node);
    ret = 0;
    if (entry == NULL) {
        if (errno == EEXIST) {
            nodefs_log_error("duplicate node: %s", node != NULL ? node->name : "(null)");
            ret = -EEXIST;
        }
        if (ret == 0 && errno == ENOTDIR) {
            nodefs_log_error("path conflict with file node: %s",
                             node != NULL ? node->name : "(null)");
            ret = -ENOTDIR;
        }
        if (ret == 0 && errno == ENOMEM) {
            ret = -ENOMEM;
        }
        if (ret == 0) {
            ret = -EINVAL;
        }
    }
    pthread_rwlock_unlock(&fs->lock);

    if (ret == 0) {
        nodefs_invalidate_paths(fs, node->name);
    }

    return ret;
}

int nodefs_unregister(nodefs_t *fs, const char *name)
{
    nodefs_entry_t *entry;
    nodefs_entry_t *parent;
    nodefs_entry_t *grandparent;
    char *path;
    int ret = 0;

    if (fs == NULL || !nodefs_valid_node_path(name)) {
        return -EINVAL;
    }

    path = nodefs_make_absolute_path(name);
    if (path == NULL) {
        return -ENOMEM;
    }

    pthread_rwlock_wrlock(&fs->lock);
    entry = nodefs_find_path(fs, path);
    if (entry == NULL) {
        ret = -ENOENT;
    } else if (entry->type != NODEFS_ENTRY_FILE) {
        ret = -EISDIR;
    } else {
        nodefs_handle_t *handle;

        list_for_each_entry(handle, &fs->handles, link) {
            if (handle->entry == entry) {
                handle->entry = NULL;
                if (handle->ph != NULL)
                    fuse_notify_poll(handle->ph);
            }
        }
        parent = entry->parent;
        list_del(&entry->sibling);
        nodefs_entry_destroy(entry);

        while (parent != &fs->root && list_empty(&parent->children)) {
            grandparent = parent->parent;
            list_del(&parent->sibling);
            nodefs_entry_destroy(parent);
            parent = grandparent;
        }
    }
    pthread_rwlock_unlock(&fs->lock);

    if (ret == 0) {
        nodefs_invalidate_paths(fs, name);
    }

    free(path);
    return ret;
}

int nodefs_notify_poll(nodefs_t *fs, const char *name)
{
    nodefs_entry_t *entry;
    nodefs_handle_t *handle;
    char *path;
    int ret = 0;

    if (fs == NULL || !nodefs_valid_node_path(name))
        return -EINVAL;
    path = nodefs_make_absolute_path(name);
    if (path == NULL)
        return -ENOMEM;

    pthread_rwlock_wrlock(&fs->lock);
    entry = nodefs_find_path(fs, path);
    if (entry == NULL)
        ret = -ENOENT;
    else if (entry->type != NODEFS_ENTRY_FILE)
        ret = -EISDIR;
    else {
        list_for_each_entry(handle, &fs->handles, link) {
            if (handle->entry == entry && handle->ph != NULL) {
                int err = fuse_notify_poll(handle->ph);
                if (err != 0 && ret == 0)
                    ret = err;
            }
        }
    }
    pthread_rwlock_unlock(&fs->lock);
    free(path);
    return ret;
}

static int nodefs_getattr(const char *path, struct stat *stbuf,
                          struct fuse_file_info *fi)
{
    nodefs_mount_t *mount = (nodefs_mount_t*)nodefs_get_userdata();
    nodefs_entry_t *entry;
    const nodefs_node_t *node;
    int ret = 0;

    (void)fi;
    memset(stbuf, 0, sizeof(*stbuf));

    if (mount == NULL || mount->fs == NULL) {
        return -ENOENT;
    }

    pthread_rwlock_rdlock(&mount->fs->lock);
    entry = (mount != NULL) ? nodefs_find_path(mount->fs, path) : NULL;
    if (entry == NULL) {
        ret = -ENOENT;
        goto out;
    }

    if (entry->type == NODEFS_ENTRY_DIR) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        goto out;
    }

    node = entry->node;
    stbuf->st_mode = S_IFREG | node->mode;
    stbuf->st_nlink = 1;
    stbuf->st_size = (node->ops != NULL && node->ops->size != NULL) ?
        node->ops->size(node->user_data) : 0;

out:
    pthread_rwlock_unlock(&mount->fs->lock);
    return ret;
}

static int nodefs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                          off_t offset, struct fuse_file_info *fi,
                          enum fuse_readdir_flags flags)
{
    nodefs_mount_t *mount = (nodefs_mount_t*)nodefs_get_userdata();
    nodefs_entry_t *dir;
    nodefs_entry_t *entry;

    (void)offset;
    (void)fi;
    (void)flags;

    if (mount == NULL || mount->fs == NULL) {
        return -ENOENT;
    }

    pthread_rwlock_rdlock(&mount->fs->lock);
    dir = (mount != NULL) ? nodefs_find_path(mount->fs, path) : NULL;
    if (dir == NULL) {
        pthread_rwlock_unlock(&mount->fs->lock);
        return -ENOENT;
    }
    if (dir->type != NODEFS_ENTRY_DIR) {
        pthread_rwlock_unlock(&mount->fs->lock);
        return -ENOTDIR;
    }

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    list_for_each_entry(entry, &dir->children, sibling) {
        filler(buf, entry->name, NULL, 0, 0);
    }
    pthread_rwlock_unlock(&mount->fs->lock);
    return 0;
}

static int nodefs_open(const char *path, struct fuse_file_info *fi)
{
    nodefs_mount_t *mount = (nodefs_mount_t*)nodefs_get_userdata();
    nodefs_entry_t *entry;
    const nodefs_node_t *node;
    int accmode;
    nodefs_handle_t *handle;

    if (mount == NULL || mount->fs == NULL) {
        return -ENOENT;
    }

    pthread_rwlock_wrlock(&mount->fs->lock);
    entry = (mount != NULL) ? nodefs_find_path(mount->fs, path) : NULL;
    if (entry == NULL) {
        pthread_rwlock_unlock(&mount->fs->lock);
        return -ENOENT;
    }
    if (entry->type != NODEFS_ENTRY_FILE) {
        pthread_rwlock_unlock(&mount->fs->lock);
        return -EISDIR;
    }

    node = entry->node;
    if (node->ops == NULL) {
        pthread_rwlock_unlock(&mount->fs->lock);
        return -EACCES;
    }

    accmode = fi->flags & O_ACCMODE;
    if ((accmode == O_RDONLY || accmode == O_RDWR) && node->ops->read == NULL) {
        pthread_rwlock_unlock(&mount->fs->lock);
        return -EACCES;
    }
    if ((accmode == O_WRONLY || accmode == O_RDWR) && node->ops->write == NULL) {
        pthread_rwlock_unlock(&mount->fs->lock);
        return -EACCES;
    }

    handle = calloc(1, sizeof(*handle));
    if (handle == NULL) {
        pthread_rwlock_unlock(&mount->fs->lock);
        return -ENOMEM;
    }
    handle->entry = entry;
    list_add_tail(&handle->link, &mount->fs->handles);
    fi->fh = (uint64_t)(uintptr_t)handle;
    if (node->flags & NODEFS_NODE_STREAM) {
        fi->direct_io = 1;
        fi->nonseekable = 1;
    }
    pthread_rwlock_unlock(&mount->fs->lock);
    return 0;
}

static int nodefs_release(const char *path, struct fuse_file_info *fi)
{
    nodefs_mount_t *mount = nodefs_get_userdata();
    nodefs_handle_t *handle = (nodefs_handle_t *)(uintptr_t)fi->fh;

    (void)path;
    if (mount == NULL || mount->fs == NULL || handle == NULL)
        return 0;
    pthread_rwlock_wrlock(&mount->fs->lock);
    if (handle->ph != NULL)
        fuse_pollhandle_destroy(handle->ph);
    list_del(&handle->link);
    free(handle);
    fi->fh = 0;
    pthread_rwlock_unlock(&mount->fs->lock);
    return 0;
}

static int nodefs_poll(const char *path, struct fuse_file_info *fi,
                       struct fuse_pollhandle *ph, unsigned *reventsp)
{
    nodefs_mount_t *mount = nodefs_get_userdata();
    nodefs_handle_t *handle = (nodefs_handle_t *)(uintptr_t)fi->fh;
    const nodefs_node_t *node;

    (void)path;
    *reventsp = 0;
    if (mount == NULL || mount->fs == NULL || handle == NULL) {
        if (ph != NULL)
            fuse_pollhandle_destroy(ph);
        return -EBADF;
    }

    pthread_rwlock_wrlock(&mount->fs->lock);
    /* Register before querying readiness: a concurrent producer's notify
     * waits for this lock, so a readiness transition cannot be lost. */
    if (ph != NULL) {
        if (handle->ph != NULL)
            fuse_pollhandle_destroy(handle->ph);
        handle->ph = ph;
    }
    if (handle->entry == NULL) {
        *reventsp = POLLHUP | POLLERR;
    } else {
        node = handle->entry->node;
        if (node->ops->poll != NULL)
            *reventsp = node->ops->poll(node->user_data);
        else {
            if (node->ops->read != NULL)
                *reventsp |= POLLIN | POLLRDNORM;
            if (node->ops->write != NULL)
                *reventsp |= POLLOUT | POLLWRNORM;
        }
    }
    pthread_rwlock_unlock(&mount->fs->lock);
    return 0;
}

static int nodefs_read(const char *path, char *buf, size_t size, off_t offset,
                       struct fuse_file_info *fi)
{
    nodefs_mount_t *mount = (nodefs_mount_t*)nodefs_get_userdata();
    nodefs_entry_t *entry;
    const nodefs_node_t *node;
    size_t node_size;
    int ret;

    if (offset < 0) {
        return -EINVAL;
    }

    if (mount == NULL || mount->fs == NULL) {
        return -ENOENT;
    }

    pthread_rwlock_rdlock(&mount->fs->lock);
    if (fi->fh != 0 &&
        ((nodefs_handle_t *)(uintptr_t)fi->fh)->entry == NULL) {
        pthread_rwlock_unlock(&mount->fs->lock);
        return -ENODEV;
    }
    entry = (mount != NULL) ? nodefs_find_path(mount->fs, path) : NULL;
    if (entry == NULL) {
        pthread_rwlock_unlock(&mount->fs->lock);
        return -ENOENT;
    }
    if (entry->type != NODEFS_ENTRY_FILE) {
        pthread_rwlock_unlock(&mount->fs->lock);
        return -EISDIR;
    }

    node = entry->node;
    if (node->ops == NULL || node->ops->read == NULL) {
        pthread_rwlock_unlock(&mount->fs->lock);
        return -EACCES;
    }

    if (size == 0) {
        pthread_rwlock_unlock(&mount->fs->lock);
        return 0;
    }

    if (!(node->flags & NODEFS_NODE_STREAM) && node->ops->size != NULL) {
        node_size = node->ops->size(node->user_data);
        if ((uint64_t)offset >= node_size) {
            pthread_rwlock_unlock(&mount->fs->lock);
            return 0;
        }
        if (size > node_size - (size_t)offset) {
            size = node_size - (size_t)offset;
        }
    }

    ret = node->ops->read(buf, size,
                          (node->flags & NODEFS_NODE_STREAM) ? 0 : offset,
                          node->user_data);
    pthread_rwlock_unlock(&mount->fs->lock);
    return ret;
}

static int nodefs_write(const char *path, const char *buf, size_t size,
                        off_t offset, struct fuse_file_info *fi)
{
    nodefs_mount_t *mount = (nodefs_mount_t*)nodefs_get_userdata();
    nodefs_entry_t *entry;
    const nodefs_node_t *node;
    int ret;

    if (offset < 0) {
        return -EINVAL;
    }

    if (mount == NULL || mount->fs == NULL) {
        return -ENOENT;
    }

    pthread_rwlock_rdlock(&mount->fs->lock);
    if (fi->fh != 0 &&
        ((nodefs_handle_t *)(uintptr_t)fi->fh)->entry == NULL) {
        pthread_rwlock_unlock(&mount->fs->lock);
        return -ENODEV;
    }
    entry = (mount != NULL) ? nodefs_find_path(mount->fs, path) : NULL;
    if (entry == NULL) {
        pthread_rwlock_unlock(&mount->fs->lock);
        return -ENOENT;
    }
    if (entry->type != NODEFS_ENTRY_FILE) {
        pthread_rwlock_unlock(&mount->fs->lock);
        return -EISDIR;
    }

    node = entry->node;
    if (node->ops == NULL || node->ops->write == NULL) {
        pthread_rwlock_unlock(&mount->fs->lock);
        return -EACCES;
    }

    ret = node->ops->write(buf, size, offset, node->user_data);
    pthread_rwlock_unlock(&mount->fs->lock);
    return ret;
}

static void* nodefs_fuse_init(struct fuse_conn_info *conn,
                              struct fuse_config *cfg)
{
    struct fuse_context *ctx = fuse_get_context();
    nodefs_mount_t *mount;

    (void)conn;

    if (ctx == NULL || ctx->private_data == NULL) {
        return NULL;
    }

    mount = (nodefs_mount_t*)ctx->private_data;
    if (mount->fs != NULL) {
        pthread_rwlock_wrlock(&mount->fs->lock);
        mount->fs->fuse = ctx->fuse;
        pthread_rwlock_unlock(&mount->fs->lock);
    }

    if (cfg != NULL) {
        cfg->entry_timeout = 0;
        cfg->negative_timeout = 0;
        cfg->attr_timeout = 0;
    }

    return ctx->private_data;
}

static void nodefs_fuse_destroy(void *private_data)
{
    nodefs_mount_t *mount = (nodefs_mount_t*)private_data;

    if (mount == NULL || mount->fs == NULL) {
        return;
    }

    pthread_rwlock_wrlock(&mount->fs->lock);
    mount->fs->fuse = NULL;
    nodefs_clear_handles(mount->fs);
    pthread_rwlock_unlock(&mount->fs->lock);
}

static const struct fuse_operations g_nodefs_oper = {
    .init = nodefs_fuse_init,
    .destroy = nodefs_fuse_destroy,
    .getattr = nodefs_getattr,
    .readdir = nodefs_readdir,
    .open = nodefs_open,
    .release = nodefs_release,
    .poll = nodefs_poll,
    .read = nodefs_read,
    .write = nodefs_write,
};

int nodefs_run(nodefs_t *fs, const char *mountpoint,
               const nodefs_config_t *config)
{
    nodefs_mount_t mount = {
        .fs = fs,
    };
    char mount_buf[PATH_MAX];
    const char *program_name = "nodefs";
    char *argv[8];
    int argc = 0;

    if (fs == NULL || mountpoint == NULL || mountpoint[0] == '\0') {
        return -EINVAL;
    }
    if (snprintf(mount_buf, sizeof(mount_buf), "%s", mountpoint) >=
        (int)sizeof(mount_buf)) {
        return -ENAMETOOLONG;
    }

    if (config != NULL && config->program_name != NULL) {
        program_name = config->program_name;
    }

    argv[argc++] = (char*)program_name;
    if (config == NULL || config->foreground) {
        argv[argc++] = "-f";
    }
    if (config != NULL && config->single_thread) {
        argv[argc++] = "-s";
    }
    if (config != NULL && config->debug) {
        argv[argc++] = "-d";
    }
    argv[argc++] = mount_buf;
    argv[argc] = NULL;

    return fuse_main(argc, argv, &g_nodefs_oper, &mount);
}
