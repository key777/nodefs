#ifndef __NODEFS_H
#define __NODEFS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef struct _nodefs_ops {
    int (*read)(char *buf, size_t size, off_t offset, void *user_data);
    int (*write)(const char *buf, size_t size, off_t offset, void *user_data);
    size_t (*size)(void *user_data);
    /* Current POLL* readiness mask; must not block. NULL means always ready. */
    unsigned (*poll)(void *user_data);
} nodefs_ops_t;

/* Queue-style reads: ignore offset/size, bypass cache, no seeking.
 * read must not block: return -EAGAIN while empty, 0 only at end of stream. */
#define NODEFS_NODE_STREAM (1u << 0)

typedef struct _nodefs_node {
    const char *name;
    mode_t mode;
    const nodefs_ops_t *ops;
    void *user_data;
    unsigned flags;
} nodefs_node_t;

typedef struct _nodefs_config {
    const char *program_name;
    int foreground;
    int single_thread;
    int debug;
} nodefs_config_t;

typedef struct nodefs nodefs_t;

nodefs_t* nodefs_create(void);
/* Stop the FUSE loop and all API callers/producers before destroying fs. */
void nodefs_destroy(nodefs_t *fs);
int nodefs_register(nodefs_t *fs, const nodefs_node_t *node);
/* Detach the node and wait for active callbacks. On success, caller-owned
 * node/ops/state may be freed even if old file descriptors remain open.
 * Synchronous unregister from a node callback returns -EDEADLK.
 * Thread cancellation is deferred until removal and drain finish. */
int nodefs_unregister(nodefs_t *fs, const char *name);

/* Publish readiness changes to all open instances. Notification I/O and node
 * callbacks run without the filesystem lock. Notifications do not consume
 * data; release business locks before notifying to keep lock ordering simple.
 * Returns -ESHUTDOWN once unmount starts. */
int nodefs_notify_poll(nodefs_t *fs, const char *name);

/*
 * nodefs_run is blocking, will capture signals (SIGINT, SIGTERM, etc.)
 *
 * param:
 * - fs: nodefs instance
 * - mountpoint: mountpoint path
 * - config: nodefs config
 *
 * return:
 * - 0 success, -1 fail
 */
int nodefs_run(nodefs_t *fs, const char *mountpoint,
               const nodefs_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* __NODEFS_H */
