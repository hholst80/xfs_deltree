#!/usr/bin/env python3
"""Integration tests: only newly created XFS images in a private mount namespace."""
import hashlib
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BIN = os.environ.get("XFS_DELTREE_BINARY", str(ROOT / "xfs_deltree"))


def run(*args, ok=True, **kw):
    result = subprocess.run([str(a) for a in args], capture_output=True, timeout=kw.pop("timeout", 120), **kw)
    if ok and result.returncode:
        raise AssertionError(f"{args}: {result.returncode}\n{result.stdout.decode(errors='replace')}\n{result.stderr.decode(errors='replace')}")
    return result


def digest(path):
    with open(path, "rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def detach_loop(loop):
    run("losetup", "-d", loop)
    # LOOP_CLR_FD may defer teardown until probing processes close their fds.
    for _ in range(100):
        if run("losetup", loop, ok=False).returncode:
            return
        time.sleep(0.02)
    raise AssertionError(f"loop device did not detach: {loop}")


class OfflineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix=".xfs-deltree-test-", dir=ROOT)
        cls.base = Path(cls.tmp.name)
        cls.mount = cls.base / "mnt"
        cls.mount.mkdir()
        cls.master = cls.base / "master.img"
        with open(cls.master, "wb") as f:
            f.truncate(512 * 1024 * 1024)
        run("mkfs.xfs", "-f", "-m", "reflink=1,rmapbt=1", cls.master)
        cls.attach(cls.master)
        try:
            outside = cls.mount / "outside"
            outside.mkdir()
            (outside / "keep").write_bytes(b"external hard link content")
            tree = cls.mount / "tree"
            tree.mkdir()
            os.link(outside / "keep", tree / "link1")
            os.link(outside / "keep", tree / "link2")
            (tree / "sym").symlink_to("/outside", target_is_directory=True)
            (tree / "long-sym").symlink_to("z" * 900)
            (tree / "dangling").symlink_to("missing")
            os.mkfifo(tree / "fifo")
            for i in range(32):
                d = tree / f"dir{i}"
                d.mkdir()
                for j in range(16):
                    (d / f"file{j}").write_bytes(b"hello" * 900)
            flat = tree / "flat"
            flat.mkdir()
            for i in range(2400):
                (flat / f"file-{i:06d}-longname").touch()
            (tree / "local-xattr").touch()
            os.setxattr(tree / "local-xattr", "user.small", b"value")
            big = tree / "large-xattr"
            big.write_bytes(b"data" * 10000)
            for i in range(20):
                os.setxattr(big, f"user.large-{i}", bytes([i]) * 4096)
            os.setxattr(flat, "user.dirattr", b"directory-attribute")
            original = outside / "reflink-original"
            original.write_bytes(b"shared blocks" * 65536)
            run("cp", "--reflink=always", original, tree / "reflink-copy")
            os.link(tree / "large-xattr", tree / "internal-hardlink")
            with open(tree / "sparse-extents", "wb") as f:
                for i in range(96):
                    f.seek(i * 1024 * 1024)
                    f.write(b"fragment" * 512)
            cls.shared_digest = digest(original)
        finally:
            cls.detach()

    @classmethod
    def attach(cls, image):
        cls.loop = run("losetup", "--find", "--show", image).stdout.decode().strip()
        try:
            run("mount", "-t", "xfs", "-o", "nouuid", cls.loop, cls.mount)
        except BaseException:
            detach_loop(cls.loop)
            raise

    @classmethod
    def detach(cls):
        run("umount", cls.mount)
        detach_loop(cls.loop)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.image = self.base / f"{self._testMethodName}.img"
        run("cp", "--reflink=auto", self.master, self.image)
        self.journal = self.base / f"{self._testMethodName}.undo"

    def tearDown(self):
        self.image.unlink(missing_ok=True)
        self.journal.unlink(missing_ok=True)

    def test_dry_run_changes_no_bytes(self):
        before = digest(self.image)
        result = run(BIN, "--dry-run", self.image, "/tree")
        self.assertIn(b"Dry run", result.stderr)
        self.assertIn(b"[dry-run +", result.stderr)
        self.assertIn(b"scan directories", result.stderr)
        self.assertIn(b"inodes/s", result.stderr)
        self.assertIn(b"100.0%", result.stderr)
        self.assertIn(b"peak-RSS=", result.stderr)
        for line in result.stderr.splitlines():
            if line.startswith(b"[dry-run +"):
                self.assertIn(b"written=0.0 MiB", line)
        self.assertEqual(digest(self.image), before)

    def test_apply_repair_and_mount(self):
        result = run(BIN, "--journal", self.journal, self.image, "/tree")
        print(result.stderr.decode(), flush=True)
        run("xfs_repair", "-n", self.image)
        self.attach(self.image)
        try:
            self.assertFalse((self.mount / "tree").exists())
            self.assertEqual((self.mount / "outside" / "keep").read_bytes(), b"external hard link content")
            self.assertEqual((self.mount / "outside" / "keep").stat().st_nlink, 1)
            self.assertEqual(digest(self.mount / "outside" / "reflink-original"), self.shared_digest)
            (self.mount / "new-file").write_bytes(b"allocation still works" * 8192)
        finally:
            self.detach()
        run("xfs_repair", "-n", self.image)

    def test_interrupted_commit_recovers_exactly(self):
        before = digest(self.image)
        result = run(BIN, "--journal", self.journal, self.image, "/tree", ok=False,
                     env={**os.environ, "XFS_DELTREE_TEST_CRASH_AFTER": "7"})
        self.assertEqual(result.returncode, 99, result.stderr)
        self.assertNotEqual(digest(self.image), before)
        loop = run("losetup", "--find", "--show", self.image).stdout.decode().strip()
        try:
            result = run("mount", "-t", "xfs", "-o", "nouuid", loop, self.mount, ok=False)
            if not result.returncode:
                run("umount", self.mount)
            self.assertNotEqual(result.returncode, 0, "partially updated image must not mount")
        finally:
            detach_loop(loop)
        run(BIN, "--recover", self.journal, self.image)
        self.assertEqual(digest(self.image), before)
        run(BIN, "--recover", self.journal, self.image)
        self.assertEqual(digest(self.image), before)

    def test_dense_rmap_rebuild_recovery_and_survivors(self):
        run("mkfs.xfs", "-f", "-s", "size=4096", "-m", "rmapbt=1,reflink=1", self.image)
        self.attach(self.image)
        try:
            tree = self.mount / "tree"
            tree.mkdir()
            keep = self.mount / "keep"
            keep.mkdir()
            parents = [tree / str(i) for i in range(32)]
            for parent in parents:
                parent.mkdir()
            for i in range(20000):
                target = parents[i % 32] / str(i)
                target.write_bytes(b"payload" * 512)
                if i % 1000 == 0:
                    os.link(target, keep / str(i))
            (keep / "shared").write_bytes(b"reflink survivor" * 32768)
            run("cp", "--reflink=always", keep / "shared", tree / "shared")
        finally:
            self.detach()
        before = digest(self.image)
        result = run(BIN, "--journal", self.journal, self.image, "/tree", ok=False,
                     env={**os.environ, "XFS_DELTREE_TEST_CRASH_AFTER": "9000"})
        self.assertEqual(result.returncode, 99, result.stderr)
        self.assertIn(b"bulk rebuilt", result.stderr)
        run(BIN, "--recover", self.journal, self.image)
        self.assertEqual(digest(self.image), before)
        self.journal.unlink()
        result = run(BIN, "--journal", self.journal, self.image, "/tree")
        self.assertIn(b"bulk rebuilt", result.stderr)
        run("xfs_repair", "-n", self.image)
        self.attach(self.image)
        try:
            self.assertFalse((self.mount / "tree").exists())
            for i in range(2000):
                (self.mount / ("new-" + str(i))).write_bytes(b"reuse" * 900)
            for i in range(0, 20000, 1000):
                self.assertEqual((keep / str(i)).read_bytes(), b"payload" * 512)
                self.assertEqual((keep / str(i)).stat().st_nlink, 1)
            self.assertEqual((keep / "shared").read_bytes(), b"reflink survivor" * 32768)
        finally:
            self.detach()
        run("xfs_repair", "-n", self.image)

    def test_v1_journal_recovery(self):
        # Translate a prepared v2 undo file into the original v1 layout.
        def checksum(data):
            crc = 0xffffffff
            for byte in data:
                crc ^= byte
                for _ in range(8):
                    crc = (crc >> 1) ^ (0x82f63b78 if crc & 1 else 0)
            return (crc ^ 0xffffffff).to_bytes(4, "little")

        before = digest(self.image)
        result = run(BIN, "--journal", self.journal, self.image, "/tree", ok=False,
                     env={**os.environ, "XFS_DELTREE_TEST_CRASH_AFTER": "7"})
        self.assertEqual(result.returncode, 99, result.stderr)
        data = self.journal.read_bytes()
        self.assertEqual(data[:8], b"XFSDTU02")
        header = b"XFSDTU01" + data[8:508]
        records = bytearray(header + checksum(header))
        for offset in range(512, len(data), 524):
            old = data[offset:offset+520]
            record = old + bytes(512)  # v1 final image is irrelevant to undo
            records.extend(record + checksum(record))
        self.journal.write_bytes(records)
        run(BIN, "--recover", self.journal, self.image)
        self.assertEqual(digest(self.image), before)

    def test_bulk_interleaved_survivors_and_inode_reuse(self):
        for options in ("finobt=0,sparse=0", "finobt=1,sparse=1"):
            with self.subTest(options=options):
                run("mkfs.xfs", "-f", "-m", "rmapbt=1,reflink=1," + options.split(",")[0], "-i", options.split(",")[1], self.image)
                self.attach(self.image)
                try:
                    tree = self.mount / "tree"
                    tree.mkdir()
                    keep = self.mount / "keep"
                    keep.mkdir()
                    for i in range(513):
                        (tree / str(i)).write_bytes(b"delete" * 700)
                        (keep / str(i)).write_bytes(str(i).encode() * 1100)
                    os.link(keep / "0", tree / "external-link")
                    run("fallocate", "-l", "1M", tree / "unwritten")
                    with open(tree / "sparse", "wb") as f:
                        for i in range(4):
                            f.seek(i * 65536)
                            f.write(b"sparse data")
                finally:
                    self.detach()
                run(BIN, "--journal", self.journal, self.image, "/tree")
                run("xfs_repair", "-n", self.image)
                self.attach(self.image)
                try:
                    self.assertFalse((self.mount / "tree").exists())
                    self.assertEqual((keep / "0").stat().st_nlink, 1)
                    for i in range(513):
                        self.assertEqual((keep / str(i)).read_bytes(), str(i).encode() * 1100)
                        (self.mount / ("new-" + str(i))).write_bytes(b"reuse" * 900)
                    for i in range(513):
                        self.assertEqual((keep / str(i)).read_bytes(), str(i).encode() * 1100)
                finally:
                    self.detach()
                run("xfs_repair", "-n", self.image)
                self.journal.unlink()

    def test_root_missing_and_non_directory_refused(self):
        before = digest(self.image)
        for name in ["/", "", "/tree/..", "/absent", "/tree/sym", "/outside/keep"]:
            self.assertNotEqual(run(BIN, self.image, name, ok=False).returncode, 0, name)
        self.assertEqual(digest(self.image), before)

    def test_mounted_image_and_device_refused(self):
        self.attach(self.image)
        try:
            self.assertNotEqual(run(BIN, self.image, "/tree", ok=False).returncode, 0)
            self.assertNotEqual(run(BIN, self.loop, "/tree", ok=False).returncode, 0)
            self.assertTrue((self.mount / "tree").exists())
        finally:
            self.detach()

    def test_completed_journal_refuses_rollback(self):
        run(BIN, "--journal", self.journal, self.image, "/tree")
        after = digest(self.image)
        result = run(BIN, "--recover", self.journal, self.image, ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(digest(self.image), after)

    def test_unmounted_block_device(self):
        loop = run("losetup", "--find", "--show", self.image).stdout.decode().strip()
        try:
            run(BIN, "--journal", self.journal, loop, "/tree")
            run("xfs_repair", "-n", loop)
        finally:
            detach_loop(loop)

    def test_corrupt_journal_rejected_without_writes(self):
        result = run(BIN, "--journal", self.journal, self.image, "/tree", ok=False,
                     env={**os.environ, "XFS_DELTREE_TEST_CRASH_AFTER": "3"})
        self.assertEqual(result.returncode, 99, result.stderr)
        after = digest(self.image)
        saved = self.journal.read_bytes()
        with open(self.journal, "r+b") as f:
            f.seek(600)
            f.write(bytes([saved[600] ^ 1]))
        result = run(BIN, "--recover", self.journal, self.image, ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(digest(self.image), after)
        self.journal.write_bytes(saved)
        run(BIN, "--recover", self.journal, self.image)

    def test_existing_journal_not_overwritten(self):
        before = digest(self.image)
        self.journal.write_bytes(b"existing journal")
        result = run(BIN, "--journal", self.journal, self.image, "/tree", ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.journal.read_bytes(), b"existing journal")
        self.assertEqual(digest(self.image), before)

    def test_quota_filesystem_rejected(self):
        # Test the unsupported-feature gate even on kernels without XFS quota.
        run("xfs_db", "-x", "-c", "sb 0", "-c", "write qflags 1", self.image)
        before = digest(self.image)
        result = run(BIN, "--journal", self.journal, self.image, "/tree", ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(digest(self.image), before)

    def test_4k_sectors(self):
        run("mkfs.xfs", "-f", "-s", "size=4096", self.image)
        self.attach(self.image)
        try:
            (self.mount / "tree").mkdir()
            (self.mount / "keep").write_bytes(b"survivor")
            for i in range(256):
                (self.mount / "tree" / str(i)).write_bytes(b"payload" * 1024)
        finally:
            self.detach()
        before = digest(self.image)
        result = run(BIN, "--journal", self.journal, self.image, "/tree", ok=False,
                     env={**os.environ, "XFS_DELTREE_TEST_CRASH_AFTER": "5"})
        self.assertEqual(result.returncode, 99, result.stderr)
        run(BIN, "--recover", self.journal, self.image)
        self.assertEqual(digest(self.image), before)
        self.journal.unlink()
        run(BIN, "--journal", self.journal, self.image, "/tree")
        run("xfs_repair", "-n", self.image)

    def test_journal_wrong_image_rejected(self):
        result = run(BIN, "--journal", self.journal, self.image, "/tree", ok=False,
                     env={**os.environ, "XFS_DELTREE_TEST_CRASH_AFTER": "0"})
        self.assertEqual(result.returncode, 99, result.stderr)
        before = digest(self.master)
        result = run(BIN, "--recover", self.journal, self.master, ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(digest(self.master), before)

    def make_multibatch_tree(self):
        run("mkfs.xfs", "-f", "-s", "size=4096", "-m", "rmapbt=1,reflink=1", self.image)
        self.attach(self.image)
        try:
            tree = self.mount / "tree"
            tree.mkdir()
            (self.mount / "survivor").write_bytes(b"outside")
            for i in range(40000):
                (tree / str(i)).write_bytes(b"batch payload")
            # Links far apart in directory order, both internal and external.
            for i in range(0, 40000, 2000):
                os.link(tree / str(i), tree / ("link-" + str(i)))
            os.link(tree / "0", self.mount / "outside-link")
        finally:
            self.detach()

    def test_multiple_checkpoints_and_dry_run_memory(self):
        self.make_multibatch_tree()
        before = digest(self.image)
        dry = run(BIN, "--dry-run", self.image, "/tree")
        self.assertEqual(digest(self.image), before)
        self.assertGreater(int(re.search(rb"checkpoints=(\d+)", dry.stderr)[1]), 1)
        self.assertLess(max(float(x) for x in re.findall(rb"peak-RSS=([0-9.]+)", dry.stderr)), 128)
        applied = run(BIN, "--journal", self.journal, self.image, "/tree")
        self.assertGreater(int(re.search(rb"checkpoints=(\d+)", applied.stderr)[1]), 1)
        self.assertLess(max(float(x) for x in re.findall(rb"peak-RSS=([0-9.]+)", applied.stderr)), 128)
        self.assertFalse(Path(str(self.journal) + ".next").exists())
        run("xfs_repair", "-n", self.image)
        self.attach(self.image)
        try:
            self.assertFalse((self.mount / "tree").exists())
            self.assertEqual((self.mount / "outside-link").read_bytes(), b"batch payload")
            self.assertEqual((self.mount / "outside-link").stat().st_nlink, 1)
        finally:
            self.detach()

    def test_checkpoint_interruption_and_resume(self):
        self.make_multibatch_tree()
        fixture = self.base / "multi-master.img"
        run("cp", "--reflink=auto", self.image, fixture)
        try:
            for point in ("prepared", "installed", "mid", "data", "superblock", "committed"):
                with self.subTest(point=point):
                    run("cp", "--reflink=auto", fixture, self.image)
                    self.journal.unlink(missing_ok=True)
                    Path(str(self.journal) + ".next").unlink(missing_ok=True)
                    result = run(BIN, "--journal", self.journal, self.image, "/tree", ok=False,
                                 env={**os.environ, "XFS_DELTREE_TEST_CRASH_BATCH": "2",
                                      "XFS_DELTREE_TEST_CRASH_POINT": point,
                                      "XFS_DELTREE_TEST_CRASH_AFTER": "7" if point == "mid" else "18446744073709551615"})
                    self.assertEqual(result.returncode, 99, result.stderr)
                    recovery = run(BIN, "--recover", self.journal, self.image, ok=False)
                    if point in ("prepared", "committed"):
                        self.assertIn(b"already committed", recovery.stderr)
                    else:
                        self.assertEqual(recovery.returncode, 0, recovery.stderr)
                    run("xfs_repair", "-n", self.image)
                    self.attach(self.image)
                    try:
                        tree = self.mount / "tree"
                        remaining = len(list(tree.iterdir())) if tree.exists() else 0
                        self.assertLess(remaining, 40020)  # Previous checkpoint stays deleted.
                        if point != "committed":
                            self.assertGreater(remaining, 0)
                        self.assertEqual((self.mount / "survivor").read_bytes(), b"outside")
                        self.assertEqual((self.mount / "outside-link").read_bytes(), b"batch payload")
                    finally:
                        self.detach()
                    # Restart scans the still-reachable remainder, using a new journal.
                    if remaining:
                        resumed = self.base / "resume.undo"
                        run(BIN, "--journal", resumed, self.image, "/tree")
                        resumed.unlink()
                        run("xfs_repair", "-n", self.image)
            Path(str(self.journal) + ".next").unlink(missing_ok=True)
        finally:
            fixture.unlink()

    def test_inode_buffer_geometry_and_neighbors(self):
        for block, inode in ((4096, 1024), (4096, 2048), (1024, 512)):
            with self.subTest(block=block, inode=inode):
                # Start with a new image so mkfs does not probe the copied
                # fixture's 4 KiB sectors when testing 1 KiB blocks.
                size = self.image.stat().st_size
                self.image.unlink()
                run("mkfs.xfs", "-f", "-d", f"file=1,name={self.image},size={size}",
                    "-s", "size=512", "-b", f"size={block}", "-i", f"size={inode}",
                    "-m", "rmapbt=1,reflink=0")
                self.attach(self.image)
                try:
                    tree = self.mount / "tree"
                    tree.mkdir()
                    keep = self.mount / "keep"
                    keep.mkdir()
                    for i in range(257):
                        (tree / str(i)).write_bytes(b"delete" * 700)
                        (keep / str(i)).write_bytes(str(i).encode() * 1100)
                    os.link(keep / "0", tree / "outside-link")
                finally:
                    self.detach()
                before = digest(self.image)
                run(BIN, "--dry-run", self.image, "/tree")
                self.assertEqual(digest(self.image), before)
                result = run(BIN, "--journal", self.journal, self.image, "/tree", ok=False,
                             env={**os.environ, "XFS_DELTREE_TEST_CRASH_AFTER": "7"})
                self.assertEqual(result.returncode, 99, result.stderr)
                run(BIN, "--recover", self.journal, self.image)
                self.assertEqual(digest(self.image), before)
                self.journal.unlink()
                run(BIN, "--journal", self.journal, self.image, "/tree")
                run("xfs_repair", "-n", self.image)
                self.attach(self.image)
                try:
                    self.assertFalse((self.mount / "tree").exists())
                    for i in range(257):
                        self.assertEqual((keep / str(i)).read_bytes(), str(i).encode() * 1100)
                    self.assertEqual((keep / "0").stat().st_nlink, 1)
                    (self.mount / "allocated-after").write_bytes(b"reuse" * 10000)
                finally:
                    self.detach()
                self.journal.unlink()

    def test_fragmented_inode_bound_before_writes(self):
        self.attach(self.image)
        try:
            with open(self.mount / "tree" / "too-fragmented", "wb") as f:
                for i in range(8193):
                    f.seek(i * 8192)
                    f.write(b"x" * 4096)
        finally:
            self.detach()
        before = digest(self.image)
        result = run(BIN, "--journal", self.journal, self.image, "/tree", ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"extent map exceeds compile-time bound", result.stderr)
        self.assertEqual(digest(self.image), before)

    def test_invalid_memory_limit(self):
        # Runtime memory configuration was deliberately removed.
        before = digest(self.image)
        result = run(BIN, "--memory-mb", "128", self.image, "/tree", ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(digest(self.image), before)


if __name__ == "__main__":
    if os.environ.get("XFS_DELTREE_TEST_NAMESPACE") != "1":
        os.execvpe("unshare", ["unshare", "--mount", "--propagation", "private", sys.executable, __file__, *sys.argv[1:]],
                  {**os.environ, "XFS_DELTREE_TEST_NAMESPACE": "1"})
    unittest.main(verbosity=2)
