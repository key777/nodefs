# nodefs

`nodefs` maps named path nodes to a small virtual filesystem view. Callers
register nodes with read, write, and size callbacks, then run the filesystem at
a mount point. Nodes can also be registered or unregistered while the
filesystem is running.

The public API intentionally describes nodes instead of the backend
implementation. Projects using this library should not need to depend on
backend-specific details in their business modules.

## Layout

```text
libs/nodefs/
  include/nodefs.h   public API
  src/nodefs.c       implementation
  src/list.h         internal list helper
  meson.build        Meson library and test definition
```

## Build

Build it from the repository root:

```sh
meson setup build
meson compile -C build
```

Compilation also creates `build/nodefs-<cpu_family>.tar.gz` (for example,
`nodefs-aarch64.tar.gz` for an AArch64 cross build). The archive contains:

```text
nodefs/
  lib/libnodefs.a
  include/nodefs.h
  test/nodefs_smoke_test
```

To build just the archive and its dependencies, run
`meson compile -C build nodefs-archive`. The target device must provide the
FUSE3 runtime library to run the smoke test. Applications linking the static
library also need FUSE3 and thread dependencies.

Run the smoke test:

```sh
meson test -C build nodefs-smoke
```

## Dependencies

`nodefs` uses libfuse3 internally and does not expose FUSE types through its
public API. Native builds find FUSE3 through `pkg-config`. For cross
compilation, configure the parent project with `fuse3_root`, which is consumed
by the nodefs build definition:

```sh
meson setup build-aarch64 \
  --cross-file meson/aarch64-linux.ini \
  -Dfuse3_root=/path/to/fuse3 \
  --buildtype=release
meson compile -C build-aarch64
```

## Example

```c
#include "nodefs.h"

#include <string.h>

static int speed_read(char *buf, size_t size, off_t offset, const nodefs_context_t *ctx)
{
    const char value[] = "1200\n";
    size_t len = sizeof(value) - 1;

    (void)ctx;

    if ((size_t)offset >= len) {
        return 0;
    }
    if (size > len - (size_t)offset) {
        size = len - (size_t)offset;
    }

    memcpy(buf, value + offset, size);
    return size;
}

static size_t speed_size(const nodefs_context_t *ctx)
{
    (void)ctx;
    return sizeof("1200\n") - 1;
}

static const nodefs_ops_t speed_ops = {
    .read = speed_read,
    .size = speed_size,
};

static const nodefs_node_t speed_node = {
    .name = "can0/motor0/state/speed",
    .mode = 0444,
    .ops = &speed_ops,
};

int main(void)
{
    nodefs_config_t config = {
        .program_name = "example",
        .foreground = 1,
    };
    nodefs_t *fs = nodefs_create();
    int ret;

    if (fs == NULL) {
        return 1;
    }

    ret = nodefs_register(fs, &speed_node);
    if (ret == 0) {
        ret = nodefs_run(fs, "/tmp/nodefs-example", &config);
    }

    nodefs_destroy(fs);
    return ret;
}
```

## Callback context

All node callbacks receive `const nodefs_context_t *ctx` instead of a raw
`void *user_data`. Its layout is private. The context is constructed for each
operation and is valid only until that callback returns; do not save or free
its pointer. The node still stores a caller-owned `user_data` pointer.

- `nodefs_context_userdata(ctx)` returns that business pointer.
- `nodefs_context_has_open(ctx)` tells whether open information is available.
- `nodefs_context_flags(ctx)` returns this operation's open flags, or
  `-ENODATA` when this operation carries no flags (`-EINVAL` for NULL).
  `O_RDONLY`, whose value is zero, is valid. Flags come from the current FUSE
  operation rather than a saved copy from open.
- `nodefs_context_interrupted(ctx)` checks request interruption/session exit
  on the callback thread, allowing blocking callbacks to stop bounded waits.

Read, write and poll receive open information. A path-only getattr/size query
may have none. FUSE poll/getattr requests do not supply open flags, so
flags queries return `-ENODATA` even when an open context exists. Size queries
invoked from read receive the read's context, including its current flags.

This changes callback signatures and ABI. Migrate every node implementation
and rebuild applications with the matching header and library. For example:

```c
static int rx_read(char *buf, size_t size, off_t offset,
                   const nodefs_context_t *ctx)
{
    struct queue *q = nodefs_context_userdata(ctx);
    int flags = nodefs_context_flags(ctx);
    if (flags < 0)
        return flags;
    /* Consume q; return -EAGAIN if empty and flags & O_NONBLOCK.
     * Otherwise wait, checking interruption and the queue's close state. */
    return queue_read(q, buf, size, offset, flags);
}
```

## poll / epoll

Nodes may provide `nodefs_ops_t.poll`, returning a current `POLL*` readiness
mask (for example `POLLIN` when a receive queue has data, or zero when empty).
After updating the queue, call `nodefs_notify_poll(fs, "rx")` to wake all
open instances watching that node. The public API uses `<poll.h>` constants
and does not expose FUSE handles. Nodes without a poll callback retain
always-ready behavior for their supported read/write operations.

For a receive queue, set `.flags = NODEFS_NODE_STREAM` on the node. This
disables file caching and seeking, skips the size callback during reads, and
passes offset zero to the read callback. The callback should consume queued
bytes and return zero only when the stream ends (or for a zero-length request).
Stream read callbacks distinguish blocking and nonblocking opens using
`nodefs_context_flags(ctx)`. With `O_NONBLOCK`, return promptly and use poll
or epoll to wait; without it, the callback may wait for a sample. Wake blocked
callbacks before unregistering their node. For FUSE request interruption or
unmount, bounded waits may query `nodefs_context_interrupted(ctx)` and return
`-EINTR`. Poll callbacks themselves must never wait for data.

```c
static unsigned rx_poll(const nodefs_context_t *ctx)
{
    struct queue *q = nodefs_context_userdata(ctx);
    unsigned events;
    pthread_mutex_lock(&q->lock);
    events = q->count ? POLLIN : 0;
    pthread_mutex_unlock(&q->lock);
    return events;
}

/* Producer: publish data before notifying, and release the queue lock first. */
pthread_mutex_lock(&q->lock);
queue_push(q, data, len);
pthread_mutex_unlock(&q->lock);
nodefs_notify_poll(fs, "rx");
```

Callbacks run outside the filesystem lock with an active node reference.
Callbacks may register nodes or publish notifications. Synchronous unregister
from any node callback returns `-EDEADLK`, preventing a callback from waiting
for itself or forming a cross-callback drain cycle. Unregister removes the
path, detaches old descriptors, and waits for existing callbacks before
returning; only then may caller-owned node definitions and state be freed.
Cancellation during a callback releases its active reference.
Release business locks before notifying. Each open
instance has its own poll handle; closing and unmounting release the handles.
Unregistering a node wakes existing watchers with `POLLHUP | POLLERR`; read
and write callbacks through those descriptors return `-ENODEV`, even if the same path
is registered again. Queue consumption semantics are shared between clients
and defined by the node callback.

Notifications snapshot references to FUSE poll handles before releasing the
filesystem lock. Closing or replacing a handle cannot destroy it while a
notification is using it. Unmount rejects new notifications and drains those
already in progress before releasing the FUSE session. Stop all API callers
and producers, and finish the FUSE loop, before calling `nodefs_destroy()`.

Applications use `epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event)` with `EPOLLIN`,
then read after `epoll_wait()`. With `EPOLLET`, drain the queue until `EAGAIN`.
epoll signals readiness; it does not perform the read or guarantee the callback
can never return `EAGAIN` after another reader consumes the data.

The header has new fields: rebuild applications using the static library.
Initialize node and callback structs with designated initializers or zero
initialization so unused optional fields remain zero.

## Node lifecycle

- Node names are relative paths, such as `can0/motor0/state/speed`.
- Intermediate directories are created automatically.
- `nodefs_register()` and `nodefs_unregister()` are safe to call while the
  filesystem is mounted. Registration and removal are protected against
  concurrent FUSE requests.
- `nodefs_unregister()` removes a file node and automatically removes empty
  intermediate directories. Trying to unregister a directory returns
  `-EISDIR`.
- Changes invalidate the affected path and its parent directories. FUSE
  attribute and lookup caches are disabled so dynamically added or removed
  nodes become visible without waiting for a cache timeout.
- Absolute paths, empty path segments, `.`, and `..` are rejected.
- `nodefs` owns registration entries, but the caller owns node definitions and
  callback storage.
- A registered `nodefs_node_t`, its `nodefs_ops_t`, and any callback state must
  remain valid until the node is unregistered and no related operation is in
  progress.
- Callback implementations should handle `offset` correctly for normal file
  read/write behavior.
