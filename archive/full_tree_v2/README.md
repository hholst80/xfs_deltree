# xfs_deltree (archived whole-tree implementation)

Historical snapshot; not part of the current build. See the [current README](../../README.md) for project status and current results.

An experimental offline C99 command that deletes a directory tree by changing XFS metadata through libxfs. It does not call unlink, unlinkat or rmdir. It requires an unmounted filesystem and does not modify or extend the kernel.

The command is implemented and tested on disposable XFS images and unmounted loop devices. This is new filesystem-writing software, not an upstream XFS utility. The tests establish specific correctness properties, not production certification or a guarantee against every power-loss/storage failure. No production-filesystem deletion was validated.

## Build

```sh
make -j4
./xfs_deltree --help
```

The application is compiled with `-std=c99`. Dependencies are the included xfsprogs v6.16.0 source tree, a C compiler, make, autoconf/automake/libtool, uuid, liburcu, inih and blkid development packages. Configure also detects optional system libraries. Upstream libxfs itself uses GNU C and its headers require GNU extensions; the Makefile provides compatibility definitions for the C99 application. There is no C++ code. The command is single-threaded; libxfs/liburcu have internal pthread dependencies. This build targets 64-bit Linux with liburcu atomic64 support.

`vendor/xfsprogs` is pinned at `1d287f3d958ebc425275d6a08ad6977e13e52fac`. No changes to the vendored libxfs source are required. The application batches inode allocation-map updates and retains empty inode chunks for reuse. Inodes themselves are freed and counted as available; some inode-table storage remains allocated. The linked program is distributed under GPL-2.0; see LICENSE and the upstream license notices.

## Usage

```text
xfs_deltree [--dry-run | --journal FILE] [--memory-mb N] DEVICE /PATH-IN-XFS
xfs_deltree --recover JOURNAL DEVICE
```

DEVICE is a block device or an unattached regular XFS image. The path is inside that filesystem, not the host mount path. For `/mnt/test-xfs/tree` on `/dev/mapper/test-xfs`, the internal path is `/tree`.

Example commands for an operator to run after stopping users of the filesystem:

```sh
sudo umount /mnt/test-xfs
sudo ./xfs_deltree --dry-run /dev/mapper/test-xfs /tree
sudo ./xfs_deltree --journal /var/tmp/tree-delete.undo /dev/mapper/test-xfs /tree
sudo xfs_repair -n /dev/mapper/test-xfs
sudo mount /mnt/test-xfs
```

The journal filename must be new and on durable storage outside the target filesystem. Existing journals are never overwritten; tmpfs and ramfs journals are rejected. A dry run performs discovery and prepares the full metadata update in memory without writing source bytes. Omitting `--journal` also selects dry-run behavior. There are no interactive prompts.

`--memory-mb` defaults to 1024 and independently bounds the sector overlay and estimated inode-index storage. The extent index uses the same entry-count limit as the inode index; a reverse-map rebuild may retain up to four records per selected extent in one allocation group. These are independent bounds, not a total RSS limit: extent/rebuild indexes, libxfs caches, allocation overhead and commit buffers add to peak memory. Exceeding a preparation limit aborts before source writes. Progress is printed to stderr at phase changes and approximately once per second while work advances, including during `--dry-run`.

Exit status is zero on a completed dry run, committed deletion or successful recovery; failures return nonzero. Termination during preparation leaves the source unchanged. Termination during application requires recovery.

## Live telemetry

Both `--dry-run` and `--journal` print progress automatically. Reports show run mode, elapsed time, current phase, completed items, average phase processing rate, bytes read/written, recent I/O throughput, and peak resident memory. Known totals include a percentage and an estimated remaining time for that phase. Directory discovery has no known total, so it reports discovered inodes and rate without a misleading percentage or ETA.

Dry runs report `written=0.0 MiB`: proposed metadata changes remain in the memory overlay. Read/write throughput measures completed userspace I/O, including cache hits and journal traffic; it is not physical drive throughput. Writes are not durable until the named sync phase finishes. Progress is emitted by the existing single thread between operations; a blocked read or fsync can pause updates, with the current phase still visible. No telemetry is sent anywhere.

```sh
./xfs_deltree --dry-run /dev/mapper/test-xfs /tree
```

A process already running uses its original executable and will not gain telemetry from a rebuild.

## Recovery

Before the first source write, the command durably saves original contents of affected 512-byte sectors, protected by CRC32C checksums. Version 2 journals omit final-sector copies, which recovery never needs; recovery also accepts version 1 journals. It also syncs the journal directory. It then installs temporary invalid primary-superblock magic (`XDT!`), preventing normal mounts of a partially applied filesystem. Final filesystem magic is restored only after all other updates have been synced.

If application fails or is interrupted, keep the device offline and run:

```sh
sudo ./xfs_deltree --recover /var/tmp/tree-delete.undo /dev/mapper/test-xfs
sudo xfs_repair -n /dev/mapper/test-xfs
```

Recovery validates the entire journal before writing, restores original metadata, syncs it, restores the original superblock last, and appends a completion record. Recovery can itself be interrupted and resumed. A completed rollback is a no-op on subsequent recovery calls. A completed deletion refuses rollback. Do not use xfs_repair to clear temporary magic or mount/use a device with an unfinished update. Losing the external journal during application can make recovery require specialist repair.

Recovery checks filesystem UUID and size. For regular images it also checks the inode number. Device major/minor numbers can change after reboot and are recorded only as diagnostic identity. Always supply the original device: a cloned block filesystem can have the same UUID and size. Corruption of the journal, unreadable storage, or damage to the UUID-bearing superblock can prevent automatic recovery. Journals are retained for explicit operator cleanup after verification.

## Supported scope

Supported and exercised: clean XFS v5 with an internal log; 512-byte and 4096-byte filesystem sectors; shortform/block/leaf/node directories; regular and sparse files; symlinks; FIFOs; internal/external hard links; local and remote extended attributes; reflinks; reverse mappings; sparse inode allocation; bigtime and large extent counters.

Rejected before source application: mounted block devices; images attached to loop devices; filesystem root; dot/dotdot path components; symlink traversal; non-directory targets; dirty/unreadable logs; active quota flags; realtime storage; external logs; parent pointers; metadata-directory and zoned features; immutable/append-only inodes; inconsistent directory references. Filesystems should already be healthy; this is not a repair tool. The kernel's O_EXCL block-device claim provides exclusivity. Regular images also use an advisory lock and must not be accessed or attached by another process during the operation.

## Design

1. Exclusively open the unmounted source, validate its superblock, and require a clean XFS log.
2. Resolve the internal path through XFS directory structures. Scan the tree once and collect unique inode numbers and reference counts, without retaining full paths or every filename.
3. Remove the target's single entry from its surviving parent with `libxfs_dir_removename`. This parent still needs directory-index maintenance because it survives.
4. Process inodes in allocation-chunk order, using the actual chunk boundaries from the inode B-tree. Update each chunk's inode/free-inode bitmap once for up to 64 deleted inodes, and batch inode-core writes. Ordinary files are loaded once; directory-entry types let discovery defer loading them. Preserve external hard links. Internal directory name indexes are discarded wholesale.
5. Collect unshared regular-file extents from inline extent arrays. Sort them by physical address and combine adjacent extents within the same allocation group. Rebuild heavily affected reverse-map B-trees with libxfs's bulk loader, preserving other owners and reusing existing tree blocks. Return surplus tree blocks through normal libxfs accounting. Smaller deletions or mappings that do not match exactly use ordinary reverse-map updates. Shared files, B-tree extent forks, attributes and directories retain normal libxfs truncation/refcount handling.
6. Intercept libxfs reads/writes using an in-memory sector overlay. Repeated updates collapse into each sector's final contents. No source writes occur in this phase.
7. Sort sectors by device offset, omit unchanged sectors, prepare the durable undo journal, and apply contiguous writes up to 1 MiB. Sync metadata and the final superblock before marking completion.

CRC32C uses runtime-selected SSE4.2 acceleration on supported x86-64 CPUs, with the portable libfrog implementation as fallback. Checksums remain enabled for both filesystem metadata and journals.

This eliminates per-file pathname deletion and much internal directory maintenance, but still processes every affected inode and its extents. It does not promise constant-time deletion or universal improvement over rm. It uses no discard/TRIM, does not securely erase file contents, and retains free inode chunks for reuse.

`archive/unlink_prototype` contains the abandoned syscall approach and is not built. [../DESIGN.md](../DESIGN.md) records the earlier research; its recommendation is superseded by this implementation.

## Validation and performance

```sh
make test
python3 tests/benchmark.py --files 20000 --runs 5 --shape branching --bytes-per-file 4096 --sector-size 4096
python3 tests/benchmark.py --files 100000 --runs 5 --shape branching --bytes-per-file 4096 --sector-size 4096
```

Integration tests require root, loop-device support, mount namespaces, mkfs.xfs and xfs_repair. They create disposable images under the project directory and use a private mount namespace. The 16 integration tests cover dense reverse-map rebuilding and exact-byte rollback, version 1 journal compatibility, sparse-inode/finobt variations, interleaved survivors and inode reuse, as well as deletion followed by `xfs_repair -n`, mount and new allocation, preservation of external hard links and reflinks, sparse files, extended attributes, exact-byte dry-run preservation, interrupted commit/rollback, invalid journals, root/path refusals, mounted-device refusal, unsupported quota flags, and 4 KiB sectors. The CRC test compares 131,072 seed/alignment/length combinations with portable libfrog. Quota refusal uses a superblock flag fixture because the development kernel lacks XFS quota support.

`make sanitize` provides an optional AddressSanitizer/UndefinedBehaviorSanitizer run for the application code (the upstream libraries are not instrumented). It requires compiler sanitizer runtimes, which are absent on the development host, so that run has not been completed here. GCC static analysis is used as an additional check.

The test-only `XFS_DELTREE_TEST_CRASH_AFTER` environment variable terminates application after a chosen number of planned sectors to exercise recovery. Do not set it for normal operation.

### Measured speedup

The optimized tool exceeded 2× on small allocated files spread across directories. Results below use 4 KiB per file and 4 KiB filesystem sectors; offline times include durable journal creation and filesystem writes. The comparison uses `rm -rf` command return time, without requiring `rm` to perform an additional sync.

| Tree | Files | Runs | Offline median | `rm -rf` median | Speedup |
| --- | ---: | ---: | ---: | ---: | ---: |
| 128 child directories | 20,000 | 5 | 0.168 s | 0.406 s | 2.42× |
| 128 child directories | 100,000 | 5 | 0.775 s | 3.146 s | 4.06× |
| One flat directory | 20,000 | 3 | 0.176 s | 0.488 s | 2.77× |

Raw measurements are in `benchmarks/final-20k-4k-sector4k.jsonl`, `benchmarks/final-100k-4k-sector4k.jsonl`, and `benchmarks/final-flat-20k-4k-sector4k.jsonl`. Each final benchmark deletion is followed, outside the timed interval, by `xfs_repair -n` and a mount check for target removal and survivor contents. Explicit `rm` plus filesystem-sync timings are also recorded.

These are local warm-cache measurements on disposable XFS images backed by the workspace's XFS filesystem. Images are copied with reflink where available; the host cache is not dropped. Target filesystems have reverse mappings and reflinks enabled. Journal storage is the host filesystem, outside the target image. Fixture creation, mounting, unmounting and validation are excluded. This demonstrates the requested improvement on these workloads, not a guaranteed 2× on other storage, fragmented data, shared files, small deletions, or end-to-end downtime including unmount/remount. Reproduce the benchmark on disposable storage representative of the real drive.

### Optimization experiments

On 20,000 allocated 4 KiB files across 128 directories with 512-byte sectors, inode batching and a smaller undo journal alone took about 0.46 s. Combining data extents reduced this to 0.30 s; checksum acceleration and physically ordered reverse-map batches brought it to about 0.28 s. Bulk rebuilding affected reverse-map trees reduced it to 0.17 s. Earlier experiment logs remain under `benchmarks/`; the initial empty-file baseline remains in `benchmarks/baseline-empty.jsonl` and `benchmarks/baseline-100k.jsonl`.

Whole-leaf pruning was investigated and discarded: XFS's own metadata mappings were interleaved with file mappings, so complete leaves could not be discarded on the tested layout. The final implementation uses the existing libxfs bulk loader and needs no custom B-tree deletion primitive. Reverse-map rebuilding is limited to allocation groups with at least 1,024 selected extents whose estimated tree capacity is at most four times the number selected, avoiding a whole-filesystem scan for a small subtree.
