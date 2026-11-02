# xfs_deltree

> **Status: failed experiment (so far).** The goal was substantially faster deletion of large directory trees than `rm -rf`. Small synthetic workloads improved, but the latest million-file experiment took **63.28 s versus 64.29 s for `rm -rf`**: effectively a tie, from a single run. This project has not demonstrated a useful large-scale performance advantage and is **not production-ready**.

This repository publishes the implementation, tests, and negative results for further investigation. The command works on tested disposable filesystems and has checkpoint recovery, but those engineering results do not establish the intended performance win. It requires an unmounted XFS filesystem; it is not a general replacement for `rm`.

`xfs_deltree` is an experimental offline C99 command that deletes a directory tree by changing XFS metadata through libxfs. It does not call unlink, unlinkat or rmdir on target filesystem entries. It requires an unmounted filesystem and does not modify or extend the kernel.

The command is implemented and tested on disposable XFS images and unmounted loop devices. This is new filesystem-writing software, not an upstream XFS utility. The tests establish specific correctness properties, not production certification or a guarantee against every power-loss/storage failure. No production-filesystem deletion has been validated.

## Build

```sh
git clone --recurse-submodules https://github.com/hholst80/xfs_deltree.git
cd xfs_deltree
make -j4
./xfs_deltree --help
```

For an existing checkout, fetch the pinned dependency with `git submodule update --init --recursive`.

The application is compiled with `-std=c99`. Dependencies are the xfsprogs v6.16.0 submodule, a C compiler, make, autoconf/automake/libtool, uuid, liburcu, inih, SQLite3 and blkid development packages. Configure also detects optional system libraries. Upstream libxfs itself uses GNU C and its headers require GNU extensions; the Makefile provides compatibility definitions for the C99 application. There is no C++ code. The command is single-threaded; libxfs/liburcu have internal pthread dependencies. This build targets 64-bit Linux with liburcu atomic64 support.

`vendor/xfsprogs` is pinned at `1d287f3d958ebc425275d6a08ad6977e13e52fac`. No changes to the vendored libxfs source are required. The application batches inode allocation-map updates and retains empty inode chunks for reuse. Inodes themselves are freed and counted as available; some inode-table storage remains allocated. The linked program is distributed under GPL-2.0; see LICENSE and the upstream license notices.

## Usage

```text
xfs_deltree [--dry-run | --journal FILE] DEVICE /PATH-IN-XFS
xfs_deltree --recover JOURNAL DEVICE
```

DEVICE is a block device or an unattached regular XFS image. The path is inside that filesystem, not the host mount path. For `/mnt/test-xfs/tree` on `/dev/mapper/test-xfs`, the internal path is `/tree`.

Example commands for a disposable test filesystem, after stopping its users (replace the device and mount point with your test setup):

```sh
sudo umount /mnt/test-xfs
sudo ./xfs_deltree --dry-run /dev/mapper/test-xfs /tree
sudo ./xfs_deltree --journal /var/tmp/tree-delete.undo /dev/mapper/test-xfs /tree
sudo xfs_repair -n /dev/mapper/test-xfs
sudo mount /mnt/test-xfs
```

The initial journal filename must be new and on durable storage outside the target filesystem. The command replaces its own completed journal at each checkpoint; an existing journal from a previous invocation is never overwritten. Tmpfs and ramfs journals are rejected. Omitting `--journal` selects dry-run behavior. There are no interactive prompts.

Deletion is incremental. Each completed checkpoint leaves a consistent, mountable filesystem with part of the tree deleted. Undo covers only the current unfinished batch; earlier checkpoints stay deleted. An interrupted batch may contain partially applied metadata and must be recovered before mounting. This is recoverability, not atomic writes of an entire batch or undo of the entire deletion.

### Memory and scratch storage

There is no runtime `--memory` or `--memory-mb` option. Compile-time budgets in `config.h` target approximately **128 MiB of active process memory** (excluding the kernel’s reclaimable filesystem cache), independently of total file count. The command collects a bounded group of references, coalesces inode and allocation updates, commits that batch and reuses its memory. A directory with ten million small files does not require ten million in-memory inode records.

The defaults allow up to 32,768 collected entries per subtree and 32,768 queued inode records per checkpoint. Large filesystems (more than 131,072 allocated inode slots) use 8,192 records per checkpoint to limit scattered index updates; larger inode/block geometry reduces this further. Metadata staging has a 64 MiB allocation budget, checkpoint sorting/I/O buffers use about 3 MiB, the bulk extent index is limited to 65,536 records, reverse-map rebuilds to 131,072 records, and libxfs starts with a 512-buffer cache. Directory traversal keeps ancestor history in a temporary disk file. These component budgets target the working set; they are not an operating-system-enforced RSS ceiling for every XFS geometry or libxfs allocation.

A single inode fork with more than 8,192 extents is refused before loading its extent map. A batch exceeding its metadata budget is also refused before applying that batch. Previous checkpoints remain committed. These exceptional cases need a different strategy or a deliberate rebuild with different compile-time bounds; they are not silently processed with unbounded RAM.

`--dry-run` executes the same selection and metadata logic, storing completed simulated batches in a temporary SQLite sector overlay under **`/var/tmp`**. Its page cache is limited to 4 MiB and memory mapping is disabled. Source bytes stay unchanged, but scratch disk usage grows with the amount of changed metadata, potentially several GiB for millions of files. `/var/tmp` must be disk-backed and have enough free space. Scratch files are unlinked immediately and disappear when the process exits; these host scratch-file operations do not delete target filesystem entries.

A real deletion does not need the full dry-run shadow. The rolling undo file is limited by the batch staging budget, approximately 62 MiB at these defaults. Two such files can coexist briefly while preparing the next checkpoint. Exit status is zero on completed deletion, dry run or successful rollback. Failure during preparation leaves that batch unapplied, but earlier checkpoints can already have deleted files.

## Live telemetry

Both `--dry-run` and `--journal` print progress automatically. Reports show run mode, elapsed time, current phase, completed items, average phase processing rate, bytes read/written, recent I/O throughput, and peak resident memory. Checkpoint numbers accompany phase reports; completion includes cumulative deletion counts. Known totals include a percentage and an estimated remaining time for that phase. Directory discovery has no known total, so it reports discovered inodes and rate without a misleading percentage or ETA.

Dry runs report `written=0.0 MiB` for source/journal writes; temporary SQLite writes are excluded from that I/O counter. Each dry-run checkpoint separately reports its scratch database size. Read/write throughput measures completed userspace I/O, including cache hits and journal traffic; it is not physical drive throughput. Writes are not durable until the named sync phase finishes. Progress is emitted by the existing single thread between operations; a blocked read or fsync can pause updates, with the current phase still visible. No telemetry is sent anywhere.

```sh
./xfs_deltree --dry-run /dev/mapper/test-xfs /tree
```

A process already running uses its original executable and will not gain telemetry from a rebuild.

## Recovery

Before applying each batch, the command durably saves original contents of affected 512-byte sectors, protected by CRC32C checksums. Version 2 journals omit final-sector copies; recovery also accepts version 1 journals. For later batches, the next journal is prepared as `JOURNAL.next`, synced, atomically renamed over the previous completed journal, and its directory synced before any source writes. The previous checkpoint must already have a durable completion footer before it can be replaced.

Each application installs temporary invalid primary-superblock magic (`XDT!`), preventing normal mounts of a partially applied filesystem. Final filesystem magic is restored only after all other batch updates have been synced, followed by a durable completion footer. The device remains exclusively held throughout the command.

If application fails or is interrupted, keep the device offline and run:

```sh
sudo ./xfs_deltree --recover /var/tmp/tree-delete.undo /dev/mapper/test-xfs
sudo xfs_repair -n /dev/mapper/test-xfs
```

Recovery validates the entire current journal before writing, restores the state before that batch, syncs it, restores the original superblock last, and appends a rollback completion record. Earlier batches remain deleted. Recovery can itself be interrupted and resumed; a completed rollback is a no-op on subsequent recovery calls. If the current journal says the checkpoint already committed, recovery refuses rollback without writing. The filesystem is already at that checkpoint; verify it before reuse.

To continue an interrupted deletion after recovery and verification, rerun the original target path with a **new journal filename**. The remaining tree is still reachable, so the command discovers the remainder without a full-tree work queue. If the final checkpoint already removed the target, there is nothing left to resume. A crash during next-journal preparation can leave `JOURNAL.next`; it has not been used for source writes until installed as `JOURNAL`. Preserve it until recovery/verification is complete, then remove it with the other old journal files.

Do not use xfs_repair to clear temporary magic or mount/use a device with an unfinished update. The crash protocol assumes working filesystem sync/rename semantics and storage that honors flushes. Losing the external journal during application can make recovery require specialist repair.

Recovery checks filesystem UUID and size. For regular images it also checks the inode number. Device major/minor numbers can change after reboot and are recorded only as diagnostic identity. Always supply the original device: a cloned block filesystem can have the same UUID and size. Corruption of the journal, unreadable storage, or damage to the UUID-bearing superblock can prevent automatic recovery. Journals are retained for explicit operator cleanup after verification.

## Supported scope

Supported and exercised: clean XFS v5 with an internal log; 512-byte and 4096-byte filesystem sectors; shortform/block/leaf/node directories; regular and sparse files; symlinks; FIFOs; internal/external hard links; local and remote extended attributes; reflinks; reverse mappings; sparse inode allocation; bigtime and large extent counters.

Rejected during validation or before applying the affected batch: mounted block devices; images attached to loop devices; filesystem root; dot/dotdot path components; symlink traversal; non-directory targets; dirty/unreadable logs; active quota flags; realtime storage; external logs; parent pointers; metadata-directory and zoned features; immutable/append-only inodes; inconsistent directory references. Filesystems should already be healthy; this is not a repair tool. The kernel's O_EXCL block-device claim provides exclusivity. Regular images also use an advisory lock and must not be accessed or attached by another process during the operation.

## Design

1. Exclusively open the unmounted source, validate its superblock, and require a clean XFS log.
2. Resolve the internal path and collect a bounded subtree. If it does not fit, descend to smaller subtrees or collect a bounded prefix of non-directory entries. A small bounded hint cache avoids repeatedly rescanning known oversized directories.
3. Remove each selected subtree root from its surviving parent using `libxfs_dir_removename`. Complete subtree interiors can be discarded wholesale. For a partially deleted directory, remove selected names through libxfs so its surviving directory index stays valid.
4. Merge selected inode references across subtrees, sort by actual inode allocation-chunk boundaries, and batch allocation-map and inode-core updates. Reference counts include only names removed in this checkpoint: other names, including names in future batches, retain their links.
5. Combine physically adjacent unshared regular-file data frees from bounded inline extent arrays (at most two extents per file with the defaults). Rebuild heavily affected reverse-map B-trees only when their bounded record index fits; otherwise update reverse maps normally. Other files, attributes and directories use normal libxfs extent/refcount handling.
6. Intercept libxfs writes in the bounded sector overlay. Compare new sectors with the source (or dry-run shadow) to avoid storing unchanged portions of large inode buffers. Nothing from the current batch reaches the source during preparation.
7. Flush all metadata into staging, prepare/install the durable rolling undo journal, and apply writes in physical order. Short unchanged gaps are read and written back unchanged to coalesce writes to interleaved inode clusters. Sync metadata, restore the final superblock and persist the completion footer.
8. Release batch memory and continue. Dry-run checkpoints go to the bounded-cache disk shadow instead of the source.

CRC32C uses runtime-selected SSE4.2 acceleration on supported x86-64 CPUs, with the portable libfrog implementation as fallback. Checksums remain enabled for both filesystem metadata and journals.

This eliminates per-file pathname deletion and much internal directory maintenance, but still processes every affected inode and its extents. It does not promise constant-time deletion or universal improvement over rm. It uses no discard/TRIM, does not securely erase file contents, and retains free inode chunks for reuse.

`archive/full_tree_v2` preserves the earlier whole-tree implementation for comparison. `archive/unlink_prototype` contains the abandoned syscall approach and is not built. [archive/DESIGN.md](archive/DESIGN.md) records the earlier filesystem-independent research; its recommendation is superseded by this implementation. Archived source is historical and is not part of the current build.

## Validation and performance

```sh
make test
python3 tests/benchmark.py --files 20000 --runs 5 --shape branching --bytes-per-file 4096 --sector-size 4096
python3 tests/benchmark.py --files 100000 --runs 5 --shape branching --bytes-per-file 4096 --sector-size 4096
```

Integration tests require root, loop-device support, mount namespaces, mkfs.xfs and xfs_repair. They create disposable images under the project directory and use a private mount namespace. The integration tests cover multi-checkpoint deletion and dry-run byte preservation, bounded working memory, internal/external hard links across batches, interruption before/after journal replacement, mid-application, after metadata sync and after completion, recovery followed by resumed deletion, and oversized extent-map refusal. Existing coverage includes dense reverse-map rebuilding, exact-byte first-batch rollback, version 1 journals, finobt/sparse-inode variations, interleaved survivors, inode reuse, mount and new allocation after deletion, reflinks, sparse files, extended attributes, invalid journals, root/path refusals, mounted-device refusal, quota refusal, and 4 KiB sectors. The CRC test compares 131,072 seed/alignment/length combinations with portable libfrog. Quota refusal uses a superblock flag fixture because the development kernel lacks XFS quota support.

Before initial publication, a build from a clean source export succeeded and `make test` passed all **20 integration tests**, plus **131,072 CRC comparisons**. See [publication-validation.log](benchmarks/publication-validation.log). The older failure in `benchmarks/compact-inode-geometry.log` was a fixture-formatting error: the copied image's 4 KiB sectors conflicted with the requested 1 KiB blocks. The test now creates a fresh image with explicit geometry; all three inode/block-size combinations pass. Historical logs are retained unchanged.

`make sanitize` provides an optional AddressSanitizer/UndefinedBehaviorSanitizer run for the application code (the upstream libraries are not instrumented). The historical development environment lacked compiler sanitizer runtimes, so no completed sanitizer run is recorded. Historical GCC static-analysis logs are retained as an additional check.

Test-only environment variables select an application sector count (`XFS_DELTREE_TEST_CRASH_AFTER`), checkpoint (`XFS_DELTREE_TEST_CRASH_BATCH`), protocol boundary (`XFS_DELTREE_TEST_CRASH_POINT`) or completed checkpoint (`XFS_DELTREE_TEST_STOP_AFTER_BATCH`). Do not set these for normal operation.

### Results: the performance goal remains unmet

Each allocated file below contains 4 KiB and filesystem sectors are 4 KiB. Offline times include journal durability and source writes. `rm -rf` times end when the command returns; its additional sync is recorded separately in the raw results. Rows describe different recorded experiments, not one repeated benchmark of the current source.

| Experiment | Tree | Files | Runs | Offline median | `rm -rf` median | Speedup |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Bounded batches | One flat directory | 20,000 | 3 | 0.177 s | 0.466 s | 2.63× |
| Bounded batches | 128 child directories | 20,000 | 3 | 0.177 s | 0.403 s | 2.28× |
| Bounded batches | 128 child directories | 100,000 | 3 | 1.411 s | 3.197 s | 2.27× |
| Bounded batches | 128 child directories | 1,000,000 | 1 | 96.863 s | 64.040 s | 0.66× |
| Later compact inode reads | 128 child directories | 1,000,000 | 1 | 63.278 s | 64.287 s | 1.02× |

The latest optimization reads inode metadata one filesystem block at a time instead of loading an entire inode cluster for sparse selections. It improves the million-file result from slower than `rm` to approximately equal. A single run with a 1.6% difference does not establish a repeatable speedup, let alone the intended substantial improvement. The cost of unmounting and remounting is excluded.

Raw results: [20k](benchmarks/bounded-final-20k-4k-sector4k.jsonl), [100k](benchmarks/bounded-final-100k-4k-sector4k.jsonl), [earlier 1m](benchmarks/bounded-final-1m-4k-sector4k.jsonl), and [latest 1m](benchmarks/compact-inode-1m.jsonl). Earlier whole-tree results in `benchmarks/final-*.jsonl` do not describe the bounded implementation. Intermediate experiments are retained for comparison.

The earlier bounded million-file run completed 123 checkpoints at **42.0 MiB peak RSS**, followed by successful repair/mount verification. The same tree completed `--dry-run` at **45.2 MiB peak RSS**, using **664 MiB of temporary disk storage**; a SHA-256 comparison of the entire image confirmed no source-byte changes. Dry-run logs and checksum verification are in `benchmarks/bounded-final-1m-dry-run*.log`. These memory and integrity measurements belong to that earlier run, not the compact-inode timing above.

Measurements use local warm-cache disposable XFS images backed by the workspace's XFS filesystem, with reflink copies where available; caches are not dropped. Targets have reverse mappings and reflinks enabled. Journal storage is outside the target image. Fixture creation, mounting, unmounting and validation are excluded. Every completed benchmark deletion is checked with `xfs_repair -n` and mounted to verify target removal and survivor contents. Results do not establish performance on other storage, fragmented/shared files, cold caches, or end-to-end downtime. Batching adds syncs and can be slower than the old unbounded implementation.

### Optimization experiments

On 20,000 allocated 4 KiB files across 128 directories with 512-byte sectors, inode batching and a smaller undo journal alone took about 0.46 s. Combining data extents reduced this to 0.30 s; checksum acceleration and physically ordered reverse-map batches brought it to about 0.28 s. Bulk rebuilding affected reverse-map trees reduced it to 0.17 s. Earlier experiment logs remain under `benchmarks/`; the initial empty-file baseline remains in `benchmarks/baseline-empty.jsonl` and `benchmarks/baseline-100k.jsonl`.

Whole-leaf pruning was investigated and discarded: XFS's own metadata mappings were interleaved with file mappings, so complete leaves could not be discarded on the tested layout. The implementation uses the existing libxfs bulk loader and needs no custom B-tree deletion primitive. Reverse-map rebuilding is limited to allocation groups with at least 1,024 selected extents whose estimated tree capacity is at most four times the number selected, avoiding a whole-filesystem scan for a small subtree. The bounded version also caps the rebuild at 131,072 records.
