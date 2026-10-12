#ifndef __NODEFS_H
#define __NODEFS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef struct _nodefs_ops {
    int (*read)(char *buf, size_t size, off_t offset, void *user_data);
    int (*write)(const char *buf, size_t size, off_t offset, void *user_data);
    size_t (*size)(void *user_data);
} nodefs_ops_t;

typedef struct _nodefs_node {
    const char *name;
    mode_t mode;
    const nodefs_ops_t *ops;
    void *user_data;
} nodefs_node_t;

typedef struct _nodefs_config {
    const char *program_name;
    int foreground;
    int single_thread;
    int debug;
} nodefs_config_t;

typedef struct nodefs nodefs_t;

nodefs_t* nodefs_create(void);
void nodefs_destroy(nodefs_t *fs);
int nodefs_register(nodefs_t *fs, const nodefs_node_t *node);
int nodefs_unregister(nodefs_t *fs, const char *name);

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
