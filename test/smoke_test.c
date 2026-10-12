#include "nodefs.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int value_read(char *buf, size_t size, off_t offset, void *user_data)
{
    const char value[] = "42\n";
    size_t len = sizeof(value) - 1;

    (void)user_data;

    if (offset < 0) {
        return -EINVAL;
    }
    if ((size_t)offset >= len) {
        return 0;
    }
    if (size > len - (size_t)offset) {
        size = len - (size_t)offset;
    }

    memcpy(buf, value + offset, size);
    return (int)size;
}

static size_t value_size(void *user_data)
{
    (void)user_data;
    return sizeof("42\n") - 1;
}

static const nodefs_ops_t value_ops = {
    .read = value_read,
    .size = value_size,
};

static const nodefs_node_t hello_node = {
    .name = "hello",
    .mode = 0444,
    .ops = &value_ops,
};

static const nodefs_node_t motor_speed_node = {
    .name = "can0/motor0/state/speed",
    .mode = 0444,
    .ops = &value_ops,
};

static const nodefs_node_t imu_accel_node = {
    .name = "can0/imu0/state/accel",
    .mode = 0444,
    .ops = &value_ops,
};

static const nodefs_node_t duplicate_motor_speed_node = {
    .name = "can0/motor0/state/speed",
    .mode = 0444,
    .ops = &value_ops,
};

static const nodefs_node_t can0_file_node = {
    .name = "can0",
    .mode = 0444,
    .ops = &value_ops,
};

static const nodefs_node_t concurrent_node = {
    .name = "shared/value",
    .mode = 0444,
    .ops = &value_ops,
};

static const nodefs_node_t file_as_dir_node = {
    .name = "hello/child",
    .mode = 0444,
    .ops = &value_ops,
};

static const nodefs_node_t absolute_path_node = {
    .name = "/can0/motor0/state/speed",
    .mode = 0444,
    .ops = &value_ops,
};

static const nodefs_node_t empty_segment_node = {
    .name = "can0//motor0",
    .mode = 0444,
    .ops = &value_ops,
};

static const nodefs_node_t trailing_slash_node = {
    .name = "can0/motor0/",
    .mode = 0444,
    .ops = &value_ops,
};

static const nodefs_node_t dot_segment_node = {
    .name = "can0/./motor0",
    .mode = 0444,
    .ops = &value_ops,
};

static const nodefs_node_t dotdot_segment_node = {
    .name = "can0/../motor0",
    .mode = 0444,
    .ops = &value_ops,
};

static int expect_register(nodefs_t *fs, const nodefs_node_t *node, int expected)
{
    int ret = nodefs_register(fs, node);

    return ret == expected ? 0 : 1;
}

static int expect_unregister(nodefs_t *fs, const char *name, int expected)
{
    int ret = nodefs_unregister(fs, name);

    return ret == expected ? 0 : 1;
}

typedef struct {
    nodefs_t *fs;
    const nodefs_node_t *node;
    int ret;
} thread_arg_t;

static void* register_thread(void *arg)
{
    thread_arg_t *thread_arg = (thread_arg_t*)arg;

    thread_arg->ret = nodefs_register(thread_arg->fs, thread_arg->node);
    return NULL;
}

static void* unregister_thread(void *arg)
{
    thread_arg_t *thread_arg = (thread_arg_t*)arg;

    thread_arg->ret = nodefs_unregister(thread_arg->fs,
                                        thread_arg->node->name);
    return NULL;
}

static int test_concurrent_updates(void)
{
    enum { THREAD_COUNT = 8 };
    thread_arg_t args[THREAD_COUNT];
    pthread_t threads[THREAD_COUNT];
    nodefs_t *fs;
    int created = 0;
    int success_count = 0;
    int duplicate_count = 0;
    int failed = 0;

    fs = nodefs_create();
    if (fs == NULL) {
        return 1;
    }

    for (int i = 0; i < THREAD_COUNT; i++) {
        args[i].fs = fs;
        args[i].node = &concurrent_node;
        args[i].ret = -EINVAL;
        if (pthread_create(&threads[i], NULL, register_thread, &args[i]) != 0) {
            failed = 1;
            break;
        }
        created++;
    }

    for (int i = 0; i < created; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            failed = 1;
            continue;
        }
        if (args[i].ret == 0) {
            success_count++;
        } else if (args[i].ret == -EEXIST) {
            duplicate_count++;
        } else {
            failed = 1;
        }
    }

    if (created != THREAD_COUNT ||
        success_count != 1 ||
        duplicate_count != THREAD_COUNT - 1) {
        failed = 1;
    }

    created = 0;
    success_count = 0;
    duplicate_count = 0;
    for (int i = 0; i < THREAD_COUNT; i++) {
        args[i].ret = -EINVAL;
        if (pthread_create(&threads[i], NULL, unregister_thread, &args[i]) != 0) {
            failed = 1;
            break;
        }
        created++;
    }

    for (int i = 0; i < created; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            failed = 1;
            continue;
        }
        if (args[i].ret == 0) {
            success_count++;
        } else if (args[i].ret == -ENOENT) {
            duplicate_count++;
        } else {
            failed = 1;
        }
    }

    if (created != THREAD_COUNT ||
        success_count != 1 ||
        duplicate_count != THREAD_COUNT - 1) {
        failed = 1;
    }

    failed |= expect_register(fs, &can0_file_node, 0);
    failed |= expect_unregister(fs, can0_file_node.name, 0);
    nodefs_destroy(fs);
    return failed;
}

int main(void)
{
    nodefs_t *fs = nodefs_create();
    int failed = 0;

    if (freopen("/dev/null", "w", stderr) == NULL) {
        return 1;
    }

    if (fs == NULL) {
        return 1;
    }

    failed |= expect_register(fs, &hello_node, 0);
    failed |= expect_register(fs, &motor_speed_node, 0);
    failed |= expect_register(fs, &imu_accel_node, 0);
    failed |= expect_register(fs, &duplicate_motor_speed_node, -EEXIST);
    failed |= expect_register(fs, &file_as_dir_node, -ENOTDIR);
    failed |= expect_register(fs, &absolute_path_node, -EINVAL);
    failed |= expect_register(fs, &empty_segment_node, -EINVAL);
    failed |= expect_register(fs, &trailing_slash_node, -EINVAL);
    failed |= expect_register(fs, &dot_segment_node, -EINVAL);
    failed |= expect_register(fs, &dotdot_segment_node, -EINVAL);

    failed |= expect_unregister(fs, "can0", -EISDIR);
    failed |= expect_unregister(fs, "missing", -ENOENT);
    failed |= expect_unregister(fs, NULL, -EINVAL);
    failed |= expect_unregister(fs, "", -EINVAL);
    failed |= expect_unregister(fs, "/hello", -EINVAL);
    failed |= expect_unregister(fs, "hello/", -EINVAL);

    failed |= expect_unregister(fs, motor_speed_node.name, 0);
    failed |= expect_unregister(fs, motor_speed_node.name, -ENOENT);
    failed |= expect_register(fs, &motor_speed_node, 0);
    failed |= expect_unregister(fs, motor_speed_node.name, 0);
    failed |= expect_unregister(fs, imu_accel_node.name, 0);
    failed |= expect_register(fs, &can0_file_node, 0);
    failed |= expect_unregister(fs, can0_file_node.name, 0);
    failed |= expect_register(fs, &motor_speed_node, 0);

    failed |= test_concurrent_updates();
    nodefs_destroy(fs);
    return failed ? 2 : 0;
}
