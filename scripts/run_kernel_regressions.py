#!/usr/bin/env python3
"""Run shell regression suites against a built kernel, using disposable disks.

Example: python3 scripts/run_kernel_regressions.py rustvfstest mmaptest testsig
No build is performed. --kernel can select an older binary for an A/B run.
Only the QEMU process started by this script is ever terminated.
"""

import argparse
import os
from pathlib import Path
import re
import selectors
import shutil
import socket
import subprocess
import tempfile
import threading
import time


CASES = {
    "rustvfstest": ("rustvfstest", r"rustvfstest: ALL TESTS PASSED"),
    "rustnettest": ("rustnettest", r"rustnettest: ALL TESTS PASSED \(64 UDP echoes\)"),
    "mmaptest": ("mmaptest", r"mmaptest: all tests passed"),
    "testsig": ("testsig", r"ALL TESTS PASSED \(21/21\)"),
    "cowtest": ("cowtest", r"ALL COW TESTS PASSED"),
    "symlinktest": ("symlinktest", r"test concurrent symlinks: ok"),
    "vforktest": ("vforktest", r"All vfork tests passed!"),
    "clonetest": ("clonetest", r"All clone tests passed!"),
    "devtest": ("devtest", r"devtest: all tests passed!"),
    "usertests": ("usertests -q", r"ALL TESTS PASSED"),
    "usermem": ("usertests mem", r"ALL TESTS PASSED"),
    "forkstress": ("usertests forkforkfork", r"ALL TESTS PASSED"),
    "stressfs": ("stressfs", r"stressfs: ALL TESTS PASSED \(33 workers\)"),
}
PROMPT = re.compile(rb"(?:^|\n)/ \$ ")
FAILURE = re.compile(r"(?:\bFAIL(?:ED)?\b|TESTS FAILED|ASSERTION_FAILURE|IPI_REASON_CRASH|"
                     r"\[Core: \d+\] (?:In thread|No thread context)|"
                     r"(?i:kernel panic|panic:|spin_lock reentry|deadlock detected|exception preempted interrupt))")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cases", nargs="*", choices=list(CASES))
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--boot-timeout", type=float, default=45)
    parser.add_argument("--boot-marker", action="append", default=[],
                        help="literal in-kernel test success marker required before shell tests; repeatable")
    parser.add_argument("--memory", default="1024M", help="QEMU guest RAM")
    parser.add_argument("--log", type=Path, default=Path("build/rustify-regressions.log"))
    args = parser.parse_args()
    selected = args.cases or [case for case in CASES if case not in ("usermem", "forkstress")]
    build = args.build_dir.resolve()
    kernel = (args.kernel or build / "kernel/xv6.bin").resolve()
    # -snapshot isolates guest writes, but its backing files still reflect
    # host rebuilds. Copy every artifact so ongoing tests use one build.
    with tempfile.TemporaryDirectory(prefix="xv6-regressions-") as directory:
        images = Path(directory)
        shutil.copyfile(kernel, images / "xv6.bin")
        for name in ("fs.img", "fs0.img"):
            shutil.copyfile(build / name, images / name)
        run(args, selected, images)


def run(args, selected, images):
    command = [
        "qemu-system-riscv64", "-machine", "virt", "-bios", "default",
        "-kernel", str(images / "xv6.bin"), "-initrd", str(images / "fs.img"),
        "-m", args.memory, "-smp", "2", "-nographic", "-snapshot",
        "-global", "virtio-mmio.force-legacy=false",
        "-drive", f"file={images / 'fs.img'},if=none,format=raw,id=x0",
        "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
        "-drive", f"file={images / 'fs0.img'},if=none,format=raw,id=x1",
        "-device", "virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1",
        "-netdev", "user,id=net0",
        "-device", "e1000,netdev=net0,bus=pcie.0",
    ]
    args.log.parent.mkdir(parents=True, exist_ok=True)
    echo = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    echo.bind(("127.0.0.1", 0))
    echo.settimeout(0.5)
    stop_echo = threading.Event()

    def echo_packets():
        while not stop_echo.is_set():
            try:
                payload, address = echo.recvfrom(65535)
                echo.sendto(payload, address)
            except socket.timeout:
                continue

    echo_thread = threading.Thread(target=echo_packets, daemon=True)
    with args.log.open("wb") as log:
        proc = subprocess.Popen(command, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        echo_thread.start()
        selector = selectors.DefaultSelector()
        selector.register(proc.stdout, selectors.EVENT_READ)

        def until_prompt(timeout, required=()):
            output = bytearray()
            deadline = time.monotonic() + timeout
            failure_at = None
            while time.monotonic() < deadline:
                for key, _ in selector.select(min(1, max(0, deadline - time.monotonic()))):
                    chunk = os.read(key.fd, 65536)
                    if not chunk:
                        raise RuntimeError("QEMU exited before the shell prompt")
                    log.write(chunk)
                    log.flush()
                    output.extend(chunk)
                clean = bytes(output).replace(b"\r", b"")
                decoded = clean.decode(errors="replace")
                failure = FAILURE.search(decoded)
                if failure and "\n" in decoded[failure.start():]:
                    # Panic prints its context/backtrace before the reason.
                    # Keep collecting briefly so the first line cannot hide
                    # the diagnostic that identifies the actual kernel bug.
                    if failure_at is None:
                        failure_at = time.monotonic()
                    if time.monotonic() - failure_at >= 0.5:
                        raise RuntimeError(f"kernel/test failure:\n{clean[-5000:].decode(errors='replace')}")
                if (failure_at is None and PROMPT.search(clean)
                        and all(marker in decoded for marker in required)):
                    return clean.decode(errors="replace")
            raise RuntimeError(f"shell/test markers timed out after {timeout:g}s")

        try:
            boot = until_prompt(args.boot_timeout, args.boot_marker)
            # Concurrent in-kernel tests can interleave their output with
            # init's character-at-a-time message. Their exact success markers
            # plus a live shell prompt establish boot in that configuration.
            if (not args.boot_marker and boot.count("init: starting sh") != 1) or FAILURE.search(boot):
                raise RuntimeError("boot gate failed")
            print("PASS boot", flush=True)
            for marker in args.boot_marker:
                print(f"PASS {marker}", flush=True)
            for case in selected:
                shell_command, expected = CASES[case]
                if case == "rustnettest":
                    shell_command += f" {echo.getsockname()[1]}"
                proc.stdin.write((shell_command + "\n").encode())
                proc.stdin.flush()
                output = until_prompt(args.timeout)
                if not re.search(expected, output) or FAILURE.search(output):
                    raise RuntimeError(f"{case} failed:\n{output[-5000:]}")
                print(f"PASS {case}", flush=True)
        finally:
            selector.close()
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            proc.stdin.close()
            proc.stdout.close()
            stop_echo.set()
            echo_thread.join(timeout=1)
            echo.close()
    print(f"Console log: {args.log}")


if __name__ == "__main__":
    main()
