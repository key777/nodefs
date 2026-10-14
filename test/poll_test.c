/* Exercise FUSE callbacks without requiring a mounted filesystem. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#define fuse_get_context test_fuse_get_context
#define fuse_notify_poll test_fuse_notify_poll
#define fuse_pollhandle_destroy test_fuse_pollhandle_destroy
#include "../src/nodefs.c"
#include <assert.h>

static struct fuse_context test_context;
static unsigned notified, destroyed;

struct fuse_context *test_fuse_get_context(void)
{
    return &test_context;
}

int test_fuse_notify_poll(struct fuse_pollhandle *ph)
{
    assert(ph != NULL);
    notified++;
    return 0;
}

void test_fuse_pollhandle_destroy(struct fuse_pollhandle *ph)
{
    assert(ph != NULL);
    destroyed++;
    free(ph);
}

static struct fuse_pollhandle *new_pollhandle(void)
{
    void *ph = malloc(1);
    assert(ph != NULL);
    return ph;
}

static unsigned queue_poll(void *data)
{
    return *(unsigned *)data ? POLLIN : 0;
}

static int queue_read(char *buf, size_t size, off_t offset, void *data)
{
    unsigned *count = data;
    assert(offset == 0);
    if (size == 0)
        return 0;
    if (*count == 0)
        return -EAGAIN;
    buf[0] = 'x';
    --*count;
    return 1;
}

static size_t zero_size(void *data)
{
    (void)data;
    return 0;
}

int main(void)
{
    unsigned count = 0, events;
    char buf[4];
    nodefs_ops_t ops = {
        .read = queue_read, .size = zero_size, .poll = queue_poll,
    };
    nodefs_node_t node = {
        .name = "rx", .mode = 0444, .ops = &ops,
        .user_data = &count, .flags = NODEFS_NODE_STREAM,
    };
    nodefs_t *fs = nodefs_create();
    nodefs_mount_t mount = { .fs = fs };
    struct fuse_file_info first = { .flags = O_RDONLY | O_NONBLOCK };
    struct fuse_file_info second = { .flags = O_RDONLY | O_NONBLOCK };

    assert(fs != NULL);
    test_context.private_data = &mount;
    assert(nodefs_register(fs, &node) == 0);
    assert(nodefs_open("/rx", &first) == 0);
    assert(nodefs_open("/rx", &second) == 0);
    assert(first.fh != second.fh);
    assert(first.direct_io && first.nonseekable);
    assert(nodefs_poll("/rx", &first, new_pollhandle(), &events) == 0);
    assert(events == 0);
    assert(nodefs_poll("/rx", &second, new_pollhandle(), &events) == 0);
    assert(events == 0);
    assert(nodefs_read("/rx", buf, sizeof(buf), 99, &first) == -EAGAIN);
    count = 1;
    assert(nodefs_notify_poll(fs, "rx") == 0);
    assert(notified == 2);
    assert(nodefs_poll("/rx", &first, NULL, &events) == 0);
    assert(events & POLLIN);
    /* Stream reads ignore both offset and the zero size callback. */
    assert(nodefs_read("/rx", buf, sizeof(buf), 99, &first) == 1);
    assert(buf[0] == 'x');
    assert(nodefs_poll("/rx", &first, new_pollhandle(), &events) == 0);
    assert(events == 0 && destroyed == 1);
    assert(nodefs_unregister(fs, "rx") == 0);
    assert(notified == 4);
    assert(nodefs_poll("/rx", &first, NULL, &events) == 0);
    assert(events == (POLLERR | POLLHUP));
    assert(nodefs_notify_poll(fs, "rx") == -ENOENT);
    assert(nodefs_notify_poll(fs, "/rx") == -EINVAL);
    /* Reusing the path must not reconnect old descriptors. */
    assert(nodefs_register(fs, &node) == 0);
    assert(nodefs_read("/rx", buf, sizeof(buf), 0, &first) == -ENODEV);
    assert(nodefs_release("/rx", &first) == 0);
    assert(nodefs_release("/rx", &second) == 0);
    assert(destroyed == 3);

    /* Regular nodes preserve default readiness and size/EOF behavior. */
    ops.poll = NULL;
    node.flags = 0;
    assert(nodefs_open("/rx", &first) == 0);
    assert(nodefs_poll("/rx", &first, NULL, &events) == 0);
    assert(events & POLLIN);
    assert(nodefs_read("/rx", buf, sizeof(buf), 0, &first) == 0);
    assert(nodefs_poll("/rx", &first, new_pollhandle(), &events) == 0);
    nodefs_fuse_destroy(&mount);
    assert(destroyed == 4);
    nodefs_destroy(fs);
    return 0;
}
