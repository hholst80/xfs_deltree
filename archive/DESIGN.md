# xfs_deltree: original filesystem-independent design

Historical research, retained for context. The implemented project is an offline XFS experiment; see the [current README](../README.md).

## Recommendation

Implement a user-space command with a POSIX directory-descriptor traversal and bounded parallelism across directories. Keep a synchronous backend as the reference implementation; evaluate Linux io_uring as an optional backend. Offer a separate, explicitly asynchronous rename-and-clean-up mode only if low latency at the original pathname is needed. A new kernel syscall is a possible later experiment, but it cannot eliminate the filesystem's per-entry work through existing generic interfaces.

This is a researched design, not an implemented or benchmarked utility. The workspace was empty when inspected on 2026-09-15. Performance improvements below are hypotheses to measure against GNU rm. “Btrfs” is assumed to be the filesystem meant by “btfs”.

## What can be improved

For an ordinary directory tree containing N entries, existing POSIX interfaces require individual removals: unlink non-directory entries, then remove empty directories. Thus the generic operation needs at least O(N) entry operations; actual cost also depends on directory lookup, file extents, filesystem metadata and storage. There is no portable operation to unlink a populated directory and implicitly discard its descendants. See the [unlink interface](https://man7.org/linux/man-pages/man2/unlink.2.html) and [rmdir interface](https://man7.org/linux/man-pages/man2/rmdir.2.html).

Unlinking a name does not necessarily release its data: external hard links and open references can retain the file. Command completion should mean that the requested names were removed, without promising immediate space reclamation or crash durability.

Three distinct targets should be measured:

| Target | Approach | Limit |
| --- | --- | --- |
| Finish all namespace removals faster | Efficient traversal and bounded concurrency | Still removes each entry |
| Make the original pathname disappear quickly | Same-filesystem rename into a private cleanup area | Cleanup and space reclamation remain pending |
| Reduce entry-level metadata work fundamentally | Filesystem-native deletion units | Requires filesystem support or advance layout choices |

## Portable engine

Use C or Rust with a small POSIX interface layer. The baseline needs openat, fdopendir/readdir, fstatat, fstat, unlinkat and threading; it needs no ext4, XFS or Btrfs metadata knowledge.

1. Resolve the requested parent and retain its directory descriptor. Reject the filesystem root, empty operands, and final `.` or `..` components. Define root-symlink handling explicitly; a directory-only command can reject it.
2. Open the target and each child directory relative to a retained parent descriptor, using O_DIRECTORY and O_NOFOLLOW. Walk input path components explicitly if intermediate symlinks must also be forbidden; O_NOFOLLOW alone protects only the final component.
3. Enumerate entries in filesystem order. Skip `.` and `..`. Use readdir's type information where available; if absent or DT_UNKNOWN, use fstatat with AT_SYMLINK_NOFOLLOW. Treat type information as a hint that can become stale.
4. Unlink ordinary entries with unlinkat(parent_fd, basename, 0). Do not open each file, read its contents, truncate it, or build an absolute pathname for each operation.
5. Schedule child directories as independent tasks. Remove a directory with unlinkat(parent_fd, basename, AT_REMOVEDIR) only after enumeration has ended and all its child work has completed successfully.
6. Record failures and continue independent work. Return failure if entries remain because of errors. Distinguish a readdir error from end-of-directory by clearing and checking errno. Permit a bounded rescan after ENOTEMPTY; continuous writers must not cause an infinite retry loop.

The [readdir documentation](https://man7.org/linux/man-pages/man3/readdir.3.html) describes optional type information, DT_UNKNOWN and directory-stream ownership. Copy names before another readdir call overwrites their storage. Directory enumeration is not a snapshot; do not assume directory offsets are byte offsets or stable under arbitrary mutations.

Use an explicit traversal stack, a bounded queue and a descriptor budget. Keep parent descriptors alive until dependent removals finish. When the queue is full, process work locally instead of having every worker block trying to enqueue children. Do not block every worker waiting for descendants queued behind it: use completion counters and runnable continuation tasks. Very deep trees require a deliberate descriptor-exhaustion policy; fail cleanly initially, or implement verified descriptor reopening later. An explicit stack alone does not solve descriptor exhaustion.

Avoid pre-counting the tree, sorting all names, per-file logging and per-file durability flushes. These add work without changing deletion semantics. Aggregate progress counters periodically.

GNU rm already uses descriptor-relative unlinkat and FTS_NOSTAT physical traversal. These choices establish a sensible baseline; they are not evidence that DELTREE will beat rm. See [coreutils remove.c](https://raw.githubusercontent.com/coreutils/coreutils/master/src/remove.c).

## Concurrency and Linux acceleration

Start with configurable worker counts of 1, 2, 4 and 8 for evaluation. Assign one enumerator/deletion owner per directory, allowing separate directories to progress concurrently. Linux removal takes the parent directory lock exclusively, so many workers unlinking siblings contend on that lock. Cross-directory work can still encounter shared inode, journal and storage bottlenecks. These are deductions from the [kernel directory-locking rules](https://www.kernel.org/doc/html/latest/filesystems/directory-locking.html), not measured speedups.

An optional io_uring backend can submit batches of IORING_OP_UNLINKAT operations and collect completions. It remains filesystem-independent on Linux, but is not a POSIX-portable API. Probe availability and retain the synchronous fallback. Keep each name buffer and directory descriptor alive until completion; explicitly wait for all child completions before scheduling directory removal. Bound outstanding requests.

io_uring batches submission, not filesystem transactions. The [kernel implementation](https://raw.githubusercontent.com/torvalds/linux/master/io_uring/fs.c) forces unlink requests onto asynchronous execution and calls the normal filename_unlinkat/filename_rmdir paths. Worker scheduling may outweigh saved syscall overhead, especially for hot-cache siblings. Benchmark before making this backend the default.

## Namespace and concurrency contract

Never follow descendant symlinks. Preserve hard links outside the tree. Leave permission, sticky-bit and immutable-file decisions to ordinary operating-system checks. Do not silently chmod entries to force deletion.

For Linux mount-boundary protection, use openat2 with RESOLVE_NO_XDEV when opening descendants, alongside suitable symlink restrictions. Establish the target first so its containing mount can be accepted. A portable st_dev comparison is only a device-boundary check: it misses same-filesystem bind mounts and is not an exact substitute. See [openat2](https://man7.org/linux/man-pages/man2/openat2.2.html). Skipped mounts must result in an incomplete-deletion status. Nested Btrfs subvolumes can also require special treatment; report failures rather than invoking filesystem-specific destruction implicitly.

Directory descriptors prevent a later symlink replacement from redirecting an already-open descriptor. They do not freeze ancestry: someone can rename an opened directory outside the requested tree while the command continues operating on it. Likewise, an identity check followed by unlink is not an atomic compare-and-delete. Strong containment against hostile concurrent renames requires additional isolation or kernel support. For the first implementation, require a quiescent or cooperatively managed tree for strict scope guarantees, and report concurrent-change errors conservatively.

## Optional asynchronous mode

Provide an explicit mode such as `deltree --defer PATH`:

1. Create a private job directory in a cleanup area on the same mounted filesystem, outside the target tree.
2. Record enough job identity and state for restart recovery.
3. Rename the target into the job directory under a previously unused name.
4. Return an accepted job ID after the configured persistence requirements are satisfied.
5. Have a supervised worker perform normal recursive deletion, retaining status and retry information on failure.

The rename removes the old pathname without traversing the descendants, but does not reclaim their storage. Cross-mount renames fail; do not substitute copying. Existing directory descriptors, working directories and open files remain relevant after rename. A private cleanup directory prevents new pathname access through its parent but does not revoke those existing references. See [rename](https://man7.org/linux/man-pages/man2/rename.2.html).

Recovery must handle crashes between state creation, rename and completion, including orphan job directories. If durable acceptance is promised, design and verify directory and job-record synchronization for the supported operating systems. A detached shell process by itself is not a durable cleanup service. Avoid returning ordinary “deletion complete” success for merely accepted work.

## Would a kernel DELTREE help?

A proposed Linux deltreeat(parent_fd, name, flags) could enumerate and remove entries through the VFS, the kernel's common filesystem interface. This could save user/kernel transitions and copying directory names out and back. It would still invoke ordinary filesystem unlink/rmdir operations and their accounting, permissions and notification machinery. It would not automatically become one transaction or an O(1) deletion.

A credible kernel proposal needs bounded traversal memory, lock ordering, cancellation, rescheduling, mount policy, concurrent-rename semantics, partial-progress/error reporting, and preservation of security checks and notifications. Holding an entire tree locked for the operation is not a practical default for large trees. Begin such a proposal only if measurements show user-space overhead is a substantial fraction of runtime after existing batching options have been evaluated.

Dropping a subtree root and reclaiming descendants later needs persistent recovery machinery and correct handling of external links and references. Existing generic VFS removal interfaces do not supply that feature. Btrfs offers an analogous facility for pre-existing subvolumes: namespace removal returns before background cleanup finishes. It applies to subvolumes, not arbitrary directories, and cannot be the cross-filesystem implementation. See [Btrfs subvolume deletion](https://btrfs.readthedocs.io/en/latest/btrfs-subvolume.html).

## Validation before choosing defaults

Compare GNU `rm -rf -- TREE`, `find TREE -depth -delete`, DELTREE with one worker, bounded workers, and optional io_uring. Use equivalent symlink/mount policies and separately measure deferred acceptance and complete cleanup.

Use disposable, freshly populated ext4, XFS and Btrfs test filesystems. Include a single flat directory with many files, many independent directories, deep trees, empty files, files with allocated extents, long names, hard links and symlinks. Test SSD/NVMe and HDD where relevant; run warm-cache and cold-cache experiments separately. Recreate equivalent data between runs, allow previous cleanup to settle, randomize order and report repeated-run distributions. Snapshot cloning can alter deletion costs and should be a separate workload.

Measure wall time, CPU time, entries per second, peak memory, descriptor usage and metadata I/O. Collect syscall counts and profiles in separate instrumented runs. Measure command completion and subsequent persistence/cleanup costs separately; none of these guarantees reclamation while other references remain. Never infer ext4/XFS/Btrfs performance from a container overlay filesystem.

Correctness tests should cover external symlink targets, surviving external hard links, unusual filename bytes, unreadable directories, read-only mounts, mount boundaries including bind mounts, concurrent creators and renames, interrupted work, descriptor exhaustion, and deferred-job restart recovery. Run permission tests as an unprivileged user. Only delete fixtures created explicitly for these tests.

The initial milestone is a correct synchronous engine plus a reproducible comparison against rm. Add directory concurrency next, and retain io_uring only where measurements justify it. No universal speedup factor can be promised from the interface design alone.
