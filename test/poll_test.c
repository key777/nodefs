/* Exercise FUSE callbacks without requiring a mounted filesystem. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#define fuse_get_context test_fuse_get_context
#define fuse_notify_poll test_fuse_notify_poll
#define fuse_pollhandle_destroy test_fuse_pollhandle_destroy
#include "../src/nodefs.c"
#include <assert.h>
#include <stdatomic.h>
#include <unistd.h>

static struct fuse_context test_context;
static atomic_uint notified, destroyed;
static pthread_mutex_t notify_gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t notify_gate_cond = PTHREAD_COND_INITIALIZER;
static int notify_block, notify_entered, notify_continue;

struct fuse_context *test_fuse_get_context(void)
{
    return &test_context;
}

int test_fuse_notify_poll(struct fuse_pollhandle *ph)
{
    assert(ph != NULL);
    nodefs_mount_t *mount = test_context.private_data;
    /* Actual notification I/O must run outside the filesystem lock. */
    assert(pthread_rwlock_wrlock(&mount->fs->lock) == 0);
    pthread_rwlock_unlock(&mount->fs->lock);
    pthread_mutex_lock(&notify_gate_lock);
    if (notify_block) {
        notify_entered = 1;
        pthread_cond_broadcast(&notify_gate_cond);
        while (!notify_continue)
            pthread_cond_wait(&notify_gate_cond, &notify_gate_lock);
    }
    pthread_mutex_unlock(&notify_gate_lock);
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

static unsigned queue_poll(const nodefs_context_t *ctx)
{
    return *(unsigned *)nodefs_context_userdata(ctx) ? POLLIN : 0;
}

static int queue_read(char *buf, size_t size, off_t offset, const nodefs_context_t *ctx)
{
    unsigned *count = nodefs_context_userdata(ctx);
    assert(offset == 0);
    if (size == 0)
        return 0;
    if (*count == 0)
        return -EAGAIN;
    buf[0] = 'x';
    --*count;
    return 1;
}

static size_t zero_size(const nodefs_context_t *ctx)
{
    (void)ctx;
    return 0;
}

struct blocked_read {
    nodefs_t *fs;
    struct fuse_file_info fi;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int entered, proceed;
    atomic_int unregistered;
};

static void unlock_mutex(void *arg)
{
    pthread_mutex_unlock(arg);
}

static int slow_read(char *buf, size_t size, off_t offset, const nodefs_context_t *ctx)
{
    struct blocked_read *state = nodefs_context_userdata(ctx);
    (void)buf; (void)size; (void)offset;
    assert(nodefs_notify_poll(state->fs, "blocked") == 0);
    assert(nodefs_unregister(state->fs, "blocked") == -EDEADLK);
    pthread_mutex_lock(&state->lock);
    pthread_cleanup_push(unlock_mutex, &state->lock);
    state->entered = 1;
    pthread_cond_broadcast(&state->cond);
    while (!state->proceed)
        pthread_cond_wait(&state->cond, &state->lock);
    pthread_cleanup_pop(1);
    return -EAGAIN;
}

static void *run_read(void *arg)
{
    struct blocked_read *state = arg;
    char buf[4];
    assert(nodefs_read("/blocked", buf, sizeof(buf), 0, &state->fi) == -EAGAIN);
    return NULL;
}

static void *run_unregister(void *arg)
{
    struct blocked_read *state = arg;
    atomic_store(&state->unregistered, nodefs_unregister(state->fs, "blocked"));
    return NULL;
}

static void test_callback_drain(int cancel)
{
    nodefs_t *fs = nodefs_create();
    nodefs_mount_t mount = { .fs = fs };
    struct blocked_read state = {
        .fs = fs, .fi = { .flags = O_RDONLY },
        .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER,
        .unregistered = INT_MIN,
    };
    nodefs_ops_t ops = { .read = slow_read };
    nodefs_node_t node = {
        .name = "blocked", .mode = 0444, .ops = &ops,
        .user_data = &state, .flags = NODEFS_NODE_STREAM,
    };
    pthread_t reader, remover;
    test_context.private_data = &mount;
    assert(nodefs_register(fs, &node) == 0);
    assert(nodefs_open("/blocked", &state.fi) == 0);
    assert(pthread_create(&reader, NULL, run_read, &state) == 0);
    pthread_mutex_lock(&state.lock);
    while (!state.entered)
        pthread_cond_wait(&state.cond, &state.lock);
    pthread_mutex_unlock(&state.lock);
    assert(pthread_create(&remover, NULL, run_unregister, &state) == 0);
    /* Removal becomes visible before the active callback finishes. */
    int detached = 0;
    for (unsigned i = 0; i < 1000 && !detached; i++) {
        pthread_rwlock_rdlock(&fs->lock);
        detached = nodefs_find_path(fs, "/blocked") == NULL;
        pthread_rwlock_unlock(&fs->lock);
        if (!detached)
            usleep(1000);
    }
    assert(detached);
    assert(atomic_load(&state.unregistered) == INT_MIN);
    nodefs_node_t other = { .name = "other", .mode = 0444, .ops = &ops };
    assert(nodefs_register(fs, &other) == 0);
    if (cancel) {
        assert(pthread_cancel(reader) == 0);
    } else {
        pthread_mutex_lock(&state.lock);
        state.proceed = 1;
        pthread_cond_broadcast(&state.cond);
        pthread_mutex_unlock(&state.lock);
    }
    void *result;
    assert(pthread_join(reader, &result) == 0);
    if (cancel)
        assert(result == PTHREAD_CANCELED);
    assert(pthread_join(remover, NULL) == 0);
    assert(atomic_load(&state.unregistered) == 0);
    assert(nodefs_release("/blocked", &state.fi) == 0);
    nodefs_destroy(fs);
    pthread_cond_destroy(&state.cond);
    pthread_mutex_destroy(&state.lock);
}

static void *run_notify(void *arg)
{
    assert(nodefs_notify_poll(arg, "rx") == 0);
    return NULL;
}

static void test_notify_close(void)
{
    nodefs_t *fs = nodefs_create();
    nodefs_mount_t mount = { .fs = fs };
    unsigned count = 0, events;
    nodefs_ops_t ops = { .read = queue_read, .poll = queue_poll };
    nodefs_node_t node = {
        .name = "rx", .mode = 0444, .ops = &ops, .user_data = &count,
    };
    struct fuse_file_info fi = { .flags = O_RDONLY };
    pthread_t notifier;
    unsigned before = destroyed;
    test_context.private_data = &mount;
    assert(nodefs_register(fs, &node) == 0);
    assert(nodefs_open("/rx", &fi) == 0);
    assert(nodefs_poll("/rx", &fi, new_pollhandle(), &events) == 0);
    pthread_mutex_lock(&notify_gate_lock);
    notify_block = 1;
    notify_entered = notify_continue = 0;
    pthread_mutex_unlock(&notify_gate_lock);
    assert(pthread_create(&notifier, NULL, run_notify, fs) == 0);
    pthread_mutex_lock(&notify_gate_lock);
    while (!notify_entered)
        pthread_cond_wait(&notify_gate_cond, &notify_gate_lock);
    pthread_mutex_unlock(&notify_gate_lock);
    /* Replace then close while the old handle is in notification I/O. */
    assert(nodefs_poll("/rx", &fi, new_pollhandle(), &events) == 0);
    assert(destroyed == before);
    assert(nodefs_release("/rx", &fi) == 0);
    assert(destroyed == before + 1);
    pthread_mutex_lock(&notify_gate_lock);
    notify_continue = 1;
    pthread_cond_broadcast(&notify_gate_cond);
    pthread_mutex_unlock(&notify_gate_lock);
    assert(pthread_join(notifier, NULL) == 0);
    assert(destroyed == before + 2);
    notify_block = 0;
    nodefs_destroy(fs);
}

struct unmount_state {
    nodefs_mount_t *mount;
    atomic_int done;
};

static void *run_unmount(void *arg)
{
    struct unmount_state *state = arg;
    nodefs_fuse_destroy(state->mount);
    atomic_store(&state->done, 1);
    return NULL;
}

static void test_notify_unmount(void)
{
    nodefs_t *fs = nodefs_create();
    nodefs_mount_t mount = { .fs = fs };
    struct unmount_state state = { .mount = &mount };
    unsigned count = 0, events, before = destroyed;
    nodefs_ops_t ops = { .read = queue_read, .poll = queue_poll };
    nodefs_node_t node = {
        .name = "rx", .mode = 0444, .ops = &ops, .user_data = &count,
    };
    struct fuse_file_info fi = { .flags = O_RDONLY };
    pthread_t notifier, unmount;
    test_context.private_data = &mount;
    assert(nodefs_register(fs, &node) == 0);
    assert(nodefs_open("/rx", &fi) == 0);
    assert(nodefs_poll("/rx", &fi, new_pollhandle(), &events) == 0);
    pthread_mutex_lock(&notify_gate_lock);
    notify_block = 1;
    notify_entered = notify_continue = 0;
    pthread_mutex_unlock(&notify_gate_lock);
    assert(pthread_create(&notifier, NULL, run_notify, fs) == 0);
    pthread_mutex_lock(&notify_gate_lock);
    while (!notify_entered)
        pthread_cond_wait(&notify_gate_cond, &notify_gate_lock);
    pthread_mutex_unlock(&notify_gate_lock);
    assert(pthread_create(&unmount, NULL, run_unmount, &state) == 0);
    int retiring = 0;
    for (unsigned i = 0; i < 1000 && !retiring; i++) {
        pthread_rwlock_rdlock(&fs->lock);
        retiring = fs->retiring;
        pthread_rwlock_unlock(&fs->lock);
        if (!retiring)
            usleep(1000);
    }
    assert(retiring && atomic_load(&state.done) == 0);
    assert(destroyed == before);
    assert(nodefs_notify_poll(fs, "rx") == -ESHUTDOWN);
    pthread_mutex_lock(&notify_gate_lock);
    notify_continue = 1;
    pthread_cond_broadcast(&notify_gate_cond);
    pthread_mutex_unlock(&notify_gate_lock);
    assert(pthread_join(notifier, NULL) == 0);
    assert(pthread_join(unmount, NULL) == 0);
    assert(destroyed == before + 1 && atomic_load(&state.done) == 1);
    notify_block = 0;
    nodefs_destroy(fs);
}

struct context_probe {
    int flags, has_open, flags_available;
    unsigned seen;
};

static struct context_probe *check_context(const nodefs_context_t *ctx)
{
    struct context_probe *probe = nodefs_context_userdata(ctx);
    assert(probe != NULL);
    assert(nodefs_context_has_open(ctx) == probe->has_open);
    assert(nodefs_context_flags(ctx) == (probe->flags_available ? probe->flags : -ENODATA));
    return probe;
}

static size_t probe_size(const nodefs_context_t *ctx)
{
    check_context(ctx)->seen |= 1u;
    return 1;
}

static int probe_read(char *buf, size_t size, off_t offset,
                       const nodefs_context_t *ctx)
{
    assert(size == 1 && offset == 0);
    check_context(ctx)->seen |= 2u;
    buf[0] = 'x';
    return 1;
}

static int probe_write(const char *buf, size_t size, off_t offset,
                        const nodefs_context_t *ctx)
{
    (void)buf; (void)offset;
    check_context(ctx)->seen |= 4u;
    return (int)size;
}

static unsigned probe_poll(const nodefs_context_t *ctx)
{
    check_context(ctx)->seen |= 8u;
    return POLLIN;
}

static void test_context_api(void)
{
    struct context_probe probe = {0};
    nodefs_t *fs = nodefs_create();
    nodefs_mount_t mount = { .fs = fs };
    nodefs_ops_t ops = {
        .read = probe_read, .write = probe_write,
        .size = probe_size, .poll = probe_poll,
    };
    nodefs_node_t node = {
        .name = "probe", .mode = 0666, .ops = &ops, .user_data = &probe,
    };
    struct fuse_file_info fi = { .flags = O_RDWR | O_NONBLOCK };
    struct fuse_file_info other = { .flags = O_RDONLY };
    struct stat st;
    char buf[4];
    unsigned events;
    test_context.private_data = &mount;
    assert(nodefs_context_userdata(NULL) == NULL);
    assert(nodefs_context_has_open(NULL) == 0);
    assert(nodefs_context_flags(NULL) == -EINVAL);
    assert(nodefs_register(fs, &node) == 0);
    assert(nodefs_getattr("/probe", &st, NULL) == 0);
    assert(st.st_size == 1);
    assert(nodefs_open("/probe", &fi) == 0);
    assert(nodefs_open("/probe", &other) == 0);
    probe.has_open = 1;
    probe.flags = fi.flags;
    assert(nodefs_getattr("/probe", &st, &fi) == 0);
    probe.flags_available = 1;
    assert(nodefs_read("/probe", buf, sizeof(buf), 0, &fi) == 1);
    assert(nodefs_write("/probe", "x", 1, 0, &fi) == 1);
    probe.flags_available = 0;
    assert(nodefs_poll("/probe", &fi, NULL, &events) == 0);
    assert(probe.seen == 15u);
    probe.flags_available = 1;
    /* Use this operation's fi flags, not a copy saved at open. */
    fi.flags = O_RDWR;
    probe.flags = fi.flags;
    assert(nodefs_read("/probe", buf, sizeof(buf), 0, &fi) == 1);
    probe.flags = other.flags;
    assert(nodefs_read("/probe", buf, sizeof(buf), 0, &other) == 1);
    assert(nodefs_release("/probe", &fi) == 0);
    assert(nodefs_release("/probe", &other) == 0);
    nodefs_destroy(fs);
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
    test_callback_drain(0);
    test_callback_drain(1);
    test_notify_close();
    test_notify_unmount();
    test_context_api();
    return 0;
}
