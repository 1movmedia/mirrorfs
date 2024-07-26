# mirrorfs

A [FUSE](https://en.wikipedia.org/wiki/Filesystem_in_Userspace) filesystem that
mirrors operations and checks for consistency.  mirrorfs ensures that the
results of operations are exactly the same, e.g., system call results, errnos,
stat structs, read buffers.  This can help test an in-development filesystem
against a known good one, e.g., ext4.

## Usage

On Fedora, install dependencies via:

```
sudo dnf install fuse3 fuse3-devel fuse3-libs
```

Build via `make` then run via:

```
./mirrorfs PATH1 PATH2 [PATH3 ...] MOUNT_PATH
```

You can specify up to 9 paths to mirror before the mount path.

If you provide the `-f` option mirrorfs will start in the foreground and log
its operations. Now programs can interact with `MOUNT_PATH` as usual. When
mirrorfs detects an inconsistency between any of the mirrored paths, it will log the diverging result and abort.

## License

Copyright (C) 2019 Andrew Gaul

Licensed under the MIT License
