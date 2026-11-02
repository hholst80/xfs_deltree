#!/usr/bin/env python3
"""Destructive tests operate only in a newly created temporary directory."""
import os
import pathlib
import resource
import signal
import subprocess
import sys
import tempfile
import unittest

BINARY = os.path.abspath(sys.argv.pop(1))


class DeltreeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="xfs-deltree-test-")
        self.base = pathlib.Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def run_delete(self, path, jobs=4, **kwargs):
        return subprocess.run([BINARY, "-j", str(jobs), "--stats", "--", str(path)],
                              capture_output=True, timeout=30, **kwargs)

    def assert_ok(self, result):
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))

    def test_parallel_and_serial_tree(self):
        for jobs in (1, 2, 4, 16):
            tree = self.base / f"tree-{jobs}"
            for i in range(64):
                leaf = tree / str(i) / "a" / "b"
                leaf.mkdir(parents=True)
                for j in range(12):
                    (leaf / str(j)).write_bytes(b"payload")
            self.assert_ok(self.run_delete(tree, jobs))
            self.assertFalse(tree.exists())

    def test_links_and_special_names(self):
        tree = self.base / "-tree"
        tree.mkdir()
        outside = self.base / "outside"
        outside.mkdir()
        survivor = outside / "keep"
        survivor.write_bytes(b"preserve me")
        os.link(survivor, tree / "hard-link")
        (tree / "directory-link").symlink_to(outside, target_is_directory=True)
        (tree / "broken").symlink_to("missing")
        (tree / "loop").symlink_to(".")
        for name in ["newline\nfile", "space file", "'quote", "back\\slash", "-f"]:
            (tree / name).touch()
        raw = os.fsencode(tree) + b"/nonutf8-\xff"
        fd = os.open(raw, os.O_CREAT | os.O_WRONLY, 0o600)
        os.close(fd)
        os.mkfifo(tree / "pipe")
        self.assert_ok(self.run_delete(tree))
        self.assertEqual(survivor.read_bytes(), b"preserve me")
        self.assertEqual(survivor.stat().st_nlink, 1)
        self.assertFalse(tree.exists())

    def test_refusals(self):
        tree = self.base / "tree"
        tree.mkdir()
        marker = tree / "keep"
        marker.touch()
        link = self.base / "link"
        link.symlink_to(tree, target_is_directory=True)
        for operand in ["/", "////", str(tree) + "/.", str(tree) + "/..",
                        str(link), str(link) + "/", str(marker), str(link / "keep")]:
            result = self.run_delete(operand)
            self.assertNotEqual(result.returncode, 0, operand)
            self.assertTrue(marker.exists())
        self.assertNotEqual(self.run_delete(self.base / "missing").returncode, 0)

    def test_trailing_slashes_and_relative_path(self):
        (self.base / "tree").mkdir()
        self.assert_ok(self.run_delete("tree///", cwd=self.base))
        self.assertFalse((self.base / "tree").exists())

    def test_deep_tree_beyond_path_max(self):
        tree = self.base / "deep"
        tree.mkdir()
        fd = os.open(tree, os.O_RDONLY | os.O_DIRECTORY)
        try:
            for i in range(240):
                name = f"level-{i:04d}-abcdefghijklmnop"
                os.mkdir(name, dir_fd=fd)
                child = os.open(name, os.O_RDONLY | os.O_DIRECTORY, dir_fd=fd)
                os.close(fd)
                fd = child
            file_fd = os.open("leaf", os.O_CREAT | os.O_WRONLY, 0o600, dir_fd=fd)
            os.close(file_fd)
        finally:
            os.close(fd)
        self.assert_ok(self.run_delete(tree, 4))
        self.assertFalse(tree.exists())

    def test_fd_exhaustion_reports_incomplete(self):
        tree = self.base / "deep"
        current = tree
        for _ in range(80):
            current.mkdir()
            current = current / "d"

        def limit():
            resource.setrlimit(resource.RLIMIT_NOFILE, (32, 32))

        result = self.run_delete(tree, 1, preexec_fn=limit)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(tree.exists())
        self.assertIn(b"Too many open files", result.stderr)
        self.assert_ok(self.run_delete(tree, 1))

    def test_permissions_as_unprivileged_user(self):
        tree = self.base / "tree"
        locked = tree / "locked"
        locked.mkdir(parents=True)
        (locked / "keep").touch()
        (tree / "removable").touch()
        if os.geteuid() == 0:
            for path in [self.base, tree, locked, locked / "keep", tree / "removable"]:
                os.chown(path, 65534, 65534)
            # /root is not searchable by nobody; execute an accessible copy.
            import shutil
            executable = self.base / "command"
            shutil.copy2(BINARY, executable)
            os.chmod(self.base, 0o755)

            def drop():
                os.setgroups([])
                os.setgid(65534)
                os.setuid(65534)

            command = [str(executable), "--", str(tree)]
            preexec = drop
        else:
            command = [BINARY, "--", str(tree)]
            preexec = None
        locked.chmod(0)
        try:
            result = subprocess.run(command, capture_output=True, timeout=30, preexec_fn=preexec)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertFalse((tree / "removable").exists())
        finally:
            locked.chmod(0o700)
        self.assertTrue((locked / "keep").exists())

    def test_mount_boundaries_in_private_namespace(self):
        # Mounts never enter the host namespace. Skip without CAP_SYS_ADMIN.
        probe = subprocess.run(["unshare", "-m", "true"], capture_output=True)
        if probe.returncode:
            self.skipTest("mount namespace unavailable")
        tree = self.base / "tree"
        target = tree / "mounted"
        target.mkdir(parents=True)
        outside = self.base / "outside"
        outside.mkdir()
        (outside / "keep").touch()
        script = '''
set -eu
mount --make-rprivate /
mount --bind "$1" "$2"
set +e
"$3" -- "$2"
root_status=$?
"$3" -- "$4"
tree_status=$?
set -e
test "$root_status" -ne 0
test "$tree_status" -ne 0
test -f "$1/keep"
umount "$2"
'''
        result = subprocess.run(["unshare", "-m", "sh", "-c", script, "test",
                                 str(outside), str(target), BINARY, str(tree)],
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((outside / "keep").exists())


if __name__ == "__main__":
    unittest.main()
