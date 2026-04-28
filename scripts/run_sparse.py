#!/usr/bin/env python3
"""Run sparse over kernel compile_commands.json.

This is intentionally a developer-time checker.  It reuses the kernel CMake
compile database, strips GCC-only options that sparse does not understand, and
adds __CHECKER__ so lock/context annotations in compiler.h become active.
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path


KEEP_PREFIXES = (
    "-D",
    "-U",
    "-I",
    "-isystem",
    "-iquote",
    "-idirafter",
    "-include",
    "-std=",
)

TAKES_VALUE = {
    "-D",
    "-U",
    "-I",
    "-isystem",
    "-iquote",
    "-idirafter",
    "-include",
    "-std",
}

DROP_WITH_VALUE = {"-o", "-MF", "-MT", "-MQ", "-x"}

DROP_PREFIXES = (
    "-O",
    "-g",
    "-W",
    "-f",
    "-m",
    "-MMD",
    "-MP",
    "-pipe",
    "-nostdlib",
    "-nostartfiles",
    "-nodefaultlibs",
    "-no-pie",
    "-static",
)

DEFAULT_SPARSE_FLAGS = [
    "-D__CHECKER__",
    "-Wbitwise",
    "-Wcontext",
    "-Wno-decl",
    "-Wno-undef",
]

DEFAULT_EXCLUDES = (
    "/kernel/kernel/lwip/",
    "/kernel/kernel/lwip_port/",
    "/kernel/kernel/lwext4/",
    "/kernel/kernel/lwext4_port/",
)


def load_command(entry: dict) -> list[str]:
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"])


def keep_arg(arg: str) -> bool:
    if arg in ("-c",):
        return False
    if arg.startswith(DROP_PREFIXES):
        return False
    if arg.startswith(KEEP_PREFIXES):
        return True
    return False


def sparse_args(entry: dict, sparse: str, extra_flags: list[str]) -> list[str]:
    original = load_command(entry)
    source = str(Path(entry["file"]).resolve())
    out = [sparse, *DEFAULT_SPARSE_FLAGS, *extra_flags]

    i = 1  # skip compiler executable
    while i < len(original):
        arg = original[i]

        if arg in DROP_WITH_VALUE:
            i += 2
            continue
        if arg in TAKES_VALUE:
            if i + 1 < len(original):
                out.extend([arg, original[i + 1]])
            i += 2
            continue
        if arg == source or Path(arg).is_absolute() and Path(arg) == Path(source):
            i += 1
            continue
        if keep_arg(arg):
            out.append(arg)
        i += 1

    out.append(source)
    return out


def should_check(entry: dict, paths: list[str], excludes: list[str]) -> bool:
    source = str(Path(entry["file"]).resolve())
    if not source.endswith(".c"):
        return False
    if any(excl in source for excl in excludes):
        return False
    if not paths:
        return True
    return any(source.startswith(str(Path(path).resolve())) for path in paths)


def run_one(entry: dict, sparse: str, extra_flags: list[str]) -> tuple[str, int, str]:
    cmd = sparse_args(entry, sparse, extra_flags)
    proc = subprocess.run(
        cmd,
        cwd=entry.get("directory") or None,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return entry["file"], proc.returncode, proc.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--compile-commands",
        required=True,
        help="Path to kernel compile_commands.json",
    )
    parser.add_argument("--sparse", default=os.environ.get("SPARSE", "sparse"))
    parser.add_argument(
        "--jobs",
        type=int,
        default=max(1, min(8, os.cpu_count() or 1)),
        help="Parallel sparse jobs",
    )
    parser.add_argument(
        "--path",
        action="append",
        default=[],
        help="Only analyze sources under this path. May be repeated.",
    )
    parser.add_argument(
        "--exclude",
        action="append",
        default=[],
        help="Substring path to exclude. May be repeated.",
    )
    parser.add_argument(
        "--no-fail",
        action="store_true",
        help="Print sparse warnings but exit 0.",
    )
    parser.add_argument(
        "--all-output",
        action="store_true",
        help="Print every sparse diagnostic instead of only high-signal ones.",
    )
    args = parser.parse_args()

    sparse = shutil.which(args.sparse)
    if sparse is None:
        print(
            f"error: sparse executable not found: {args.sparse}\n"
            "Install the host package named 'sparse' or set SPARSE=/path/to/sparse.",
            file=sys.stderr,
        )
        return 127

    compile_commands = Path(args.compile_commands)
    if not compile_commands.exists():
        print(f"error: missing compile database: {compile_commands}", file=sys.stderr)
        return 2

    extra_flags = shlex.split(os.environ.get("SPARSEFLAGS", ""))
    excludes = [*DEFAULT_EXCLUDES, *args.exclude]
    with compile_commands.open() as f:
        entries = [
            entry
            for entry in json.load(f)
            if should_check(entry, args.path, excludes)
        ]

    if not entries:
        print("sparse: no matching kernel C files")
        return 0

    failures = 0
    error_lines = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(run_one, entry, sparse, extra_flags) for entry in entries]
        for fut in as_completed(futures):
            source, rc, output = fut.result()
            if output:
                error_lines += output.count(": error:")
                if args.all_output:
                    shown = output
                else:
                    shown = "\n".join(
                        line
                        for line in output.splitlines()
                        if ": error:" in line
                        or "context imbalance" in line
                        or "context problem" in line
                        or "incorrect type" in line
                        or "restricted " in line
                    )
                if shown:
                    print(shown, end="" if shown.endswith("\n") else "\n")
            if rc != 0:
                failures += 1
                print(f"sparse: {source}: exited with {rc}", file=sys.stderr)

    print(
        f"sparse: checked {len(entries)} file(s), "
        f"failures={failures}, errors={error_lines}"
    )
    if (failures or error_lines) and not args.no_fail:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
