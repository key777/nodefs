"""Package the built library, smoke test, and public header."""

import sys
import tarfile


def main():
    output, library, smoke_test, header = sys.argv[1:]
    with tarfile.open(output, "w:gz") as archive:
        for source, destination in (
            (library, "nodefs/lib/libnodefs.a"),
            (smoke_test, "nodefs/test/nodefs_smoke_test"),
            (header, "nodefs/include/nodefs.h"),
        ):
            archive.add(source, arcname=destination)


if __name__ == "__main__":
    main()
