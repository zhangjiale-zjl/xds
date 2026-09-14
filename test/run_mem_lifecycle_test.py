#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Compile real mem.c with userspace mocks; never load a module or issue disk I/O."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def run(command, **kwargs):
    return subprocess.run(command, check=True, timeout=30, **kwargs)


def prepare_headers(directory):
    linux = directory / "linux"
    linux.mkdir()
    names = (
        "errno completion jiffies mm module mutex overflow slab vmalloc workqueue "
        "types blk-mq blkdev device fs nvme version blk_types"
    )
    for name in names.split():
        content = "#include_next <linux/errno.h>\n" if name == "errno" else ""
        (linux / f"{name}.h").write_text(content)
    (directory / "ascend_kernel_hal.h").write_text("")


def check_version_guards(compiler, directory, source):
    for major, minor, patch, accepted in (
        (5, 4, 0, False), (5, 10, 0, False), (5, 14, 0, False),
        (5, 15, 0, True), (5, 15, 152, True), (5, 16, 0, False),
        (5, 19, 0, False), (6, 6, 0, True),
    ):
        version = (major << 16) | (minor << 8) | patch
        command = compiler + [
            "-E", "-x", "c", "-I", str(directory),
            "-DKERNEL_VERSION(a,b,c)=(((a)<<16)+((b)<<8)+(c))",
            f"-DLINUX_VERSION_CODE={version}", str(source / "compat.h"),
        ]
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        assert (result.returncode == 0) == accepted, result.stderr
        if not accepted:
            assert "XDS legacy NVMe request ABI requires Linux 5.15.x" in result.stderr
        print(f"PASS version-guard {major}.{minor}.{patch} accepted={accepted}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--no-sanitize", action="store_true")
    args = parser.parse_args()
    compiler = shlex.split(args.cc)
    source = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="xds-mem-test-") as name:
        directory = Path(name)
        prepare_headers(directory)
        binary = directory / "mem_lifecycle_test"
        flags = ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread"]
        if not args.no_sanitize:
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        run(compiler + flags + ["-I", str(directory), "-o", str(binary),
                               str(source / "test" / "mem_lifecycle_test.c")])
        for case in (
            "normal", "init-failure", "hal-get-failure", "prepare-invalidation",
            "invalid-table", "copy-allocation-failure", "active-invalidation", "retry-unload",
        ):
            run([str(binary), case])
        check_version_guards(compiler, directory, source)


if __name__ == "__main__":
    main()
