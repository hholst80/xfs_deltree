#!/usr/bin/env python3
"""Warm-cache comparison on disposable loop-mounted XFS images."""
import argparse
import json
import os
from pathlib import Path
import random
import statistics
import sys
import tempfile
import time
from test_offline import ROOT, BIN, run, detach_loop


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--files", type=int, default=20000)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--bytes-per-file", type=int, default=0)
    parser.add_argument("--shape", choices=("flat", "branching", "both"), default="both")
    parser.add_argument("--sector-size", type=int, choices=(512, 4096), default=512)
    parser.add_argument("--save-fixture", type=Path, help="keep an unmounted fixture image for repeated experiments")
    parser.add_argument("--reuse-fixture", type=Path, help="clone an existing fixture instead of populating another")
    args = parser.parse_args()
    if (args.save_fixture or args.reuse_fixture) and args.shape == "both":
        parser.error("saving/reusing fixtures requires one --shape")
    if args.save_fixture and args.save_fixture.exists():
        parser.error("saved fixture path must be new")
    if args.reuse_fixture and not args.reuse_fixture.is_file():
        parser.error("reused fixture must be a regular image file")
    shapes = ("flat", "branching") if args.shape == "both" else (args.shape,)
    if not 0 <= args.bytes_per_file <= 65536:
        parser.error("bytes-per-file must be 0..65536")
    if not 1 <= args.files <= 1000000 or not 1 <= args.runs <= 20:
        parser.error("files must be 1..1000000; runs must be 1..20")
    results = []
    with tempfile.TemporaryDirectory(prefix=".xfs-deltree-bench-", dir=ROOT) as temp:
        base = Path(temp)
        mount = base / "mnt"
        mount.mkdir()

        def attach(image):
            loop = run("losetup", "--find", "--show", image).stdout.decode().strip()
            try:
                run("mount", "-o", "nouuid", loop, mount)
            except BaseException:
                detach_loop(loop)
                raise
            return loop

        def detach(loop):
            run("umount", mount)
            detach_loop(loop)

        for shape in shapes:
            master = base / f"{shape}.img"
            if args.reuse_fixture:
                run("cp", "--reflink=auto", args.reuse_fixture, master)
                print(json.dumps(dict(fixture=shape, reused=str(args.reuse_fixture))), flush=True)
            else:
                with open(master, "wb") as f:
                    f.truncate(max(512 * 1024 * 1024, args.files * (8192 + args.bytes_per_file)))
                geometry = run("mkfs.xfs", "-f", "-s", f"size={args.sector_size}", master).stdout.decode()
                print(json.dumps(dict(fixture=shape, geometry=geometry)), flush=True)
                loop = attach(master)
                try:
                    tree = mount / "tree"
                    tree.mkdir()
                    (mount / "survivor").write_bytes(b"must survive deletion")
                    parents = [tree]
                    if shape == "branching":
                        parents = [tree / str(i) for i in range(128)]
                        for p in parents:
                            p.mkdir()
                    for i in range(args.files):
                        (parents[i % len(parents)] / f"file-{i:08d}").write_bytes(b"x" * args.bytes_per_file)
                finally:
                    detach(loop)
            if args.save_fixture:
                # Exclusive creation prevents accidental replacement of an existing file.
                with args.save_fixture.open("xb"):
                    pass
                run("cp", "--reflink=auto", master, args.save_fixture)
                print(json.dumps(dict(saved_fixture=str(args.save_fixture))), flush=True)
            cases = ["offline", "rm"] * args.runs
            random.Random(42).shuffle(cases)
            for index, method in enumerate(cases):
                image = base / "run.img"
                journal = base / "run.undo"
                run("cp", "--reflink=auto", master, image)
                if method == "offline":
                    start = time.perf_counter()
                    result = run(BIN, "--journal", journal, image, "/tree", timeout=600)
                    elapsed = time.perf_counter() - start
                    result_data = dict(shape=shape, files=args.files, bytes_per_file=args.bytes_per_file, sector_size=args.sector_size, method=method, seconds=elapsed,
                                       detail=result.stderr.decode().strip())
                    journal.unlink()
                else:
                    loop = attach(image)
                    try:
                        start = time.perf_counter()
                        run("rm", "-rf", "--", mount / "tree", timeout=600)
                        returned = time.perf_counter() - start
                        run("sync", "-f", mount)
                        elapsed = time.perf_counter() - start
                        result_data = dict(shape=shape, files=args.files, bytes_per_file=args.bytes_per_file, sector_size=args.sector_size, method=method, seconds=elapsed,
                                           command_seconds=returned)
                    finally:
                        detach(loop)
                # Validation is outside the measured deletion interval.
                run("xfs_repair", "-n", image)
                loop = attach(image)
                try:
                    assert not (mount / "tree").exists()
                    assert (mount / "survivor").read_bytes() == b"must survive deletion"
                finally:
                    detach(loop)
                results.append(result_data)
                print(json.dumps(result_data), flush=True)
                image.unlink()
            master.unlink()
    summary = {}
    for shape in shapes:
        for method in ("offline", "rm"):
            summary[f"{shape}/{method}"] = statistics.median(
                r["seconds"] for r in results if r["shape"] == shape and r["method"] == method)
        rm_command = statistics.median(r["command_seconds"] for r in results
                                       if r["shape"] == shape and r["method"] == "rm")
        summary[f"{shape}/rm_command"] = rm_command
        summary[f"{shape}/speedup_vs_rm_command"] = rm_command / summary[f"{shape}/offline"]
    print(json.dumps(dict(files=args.files, bytes_per_file=args.bytes_per_file, sector_size=args.sector_size,
                          runs=args.runs, median_seconds=summary)), flush=True)


if __name__ == "__main__":
    if os.environ.get("XFS_DELTREE_TEST_NAMESPACE") != "1":
        os.execvpe("unshare", ["unshare", "--mount", "--propagation", "private", sys.executable, __file__, *sys.argv[1:]],
                  {**os.environ, "XFS_DELTREE_TEST_NAMESPACE": "1"})
    main()
