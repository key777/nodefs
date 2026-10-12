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

static int speed_read(char *buf, size_t size, off_t offset)
{
    const char value[] = "1200\n";
    size_t len = sizeof(value) - 1;

    if ((size_t)offset >= len) {
        return 0;
    }
    if (size > len - (size_t)offset) {
        size = len - (size_t)offset;
    }

    memcpy(buf, value + offset, size);
    return size;
}

static size_t speed_size(void)
{
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

## Notes

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
