# fstrace: record and replay filesystem syscalls

The `realworld` campaign uses these tools for the kernel `du` row of Fig. 10.
The filesystem syscalls of a real program are recorded once and replayed on
a fresh SplineFS or ext4 tree.
`make -C utils` builds both tools into `utils/bin/`.

## 1. Record

`libfstrace_record.so` is an `LD_PRELOAD` library.  It hooks the open, read,
write, sync, rename, unlink, mkdir, rmdir, stat, truncate and `getdents64`
calls, and writes one line per call with the path, the recorded file
descriptor or return value, and `errno` on failure.  The format is at the top
of `fstrace_record.c`.  Recording needs no module and no test device.

```sh
LD_PRELOAD=$PWD/utils/bin/libfstrace_record.so \
FSTRACE_OUT=/tmp/du.trace FSTRACE_PATH=/data/linux \
du -s /data/linux
```

`FSTRACE_PATH` keeps only calls under that prefix.

## 2. Replay

```sh
utils/bin/fstrace_replay --map /data/linux/=/mnt/test/linux/ /tmp/du.trace
```

`fstrace_replay` maps recorded file descriptors to live ones and rewrites
path prefixes with `--map FROM=TO`.  It prints operation counts, bytes and
operations per second.

- `--warmup`: replay only the operations that build the tree.  `realworld`
  replays the prepare trace this way, untimed.
- `--no-data`: skip reads, writes and `getdents64`.
- `--hold-fds`: ignore recorded closes.
- `--bytes-pattern N`: the byte that replayed writes contain (default 0xab).
- `--throttle-writes N`: `sync_file_range` every N writes.

## 3. Limitations

- libc `opendir`/`readdir` are not hooked, so a program that lists
  directories through them (such as `find`) records only its per-entry calls.
- Reads through `mmap` are not recorded.
- Child processes inherit the library and append to the same trace.
- Replay is single-threaded and keeps the recorded order.
