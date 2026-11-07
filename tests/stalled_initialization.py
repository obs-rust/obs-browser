#!/usr/bin/env -S uv run --script
"""Exercise real module shutdown with a stalled cef_initialize interposition.

Build startup-shutdown with startup_shutdown.py first. The shim blocks before
CEF starts, or after real CEF has started helpers. A pipe releases slow starts;
stalled cases never release it. No production timeout environment overrides.
"""
import argparse
import os
from pathlib import Path
import selectors
import signal
import subprocess
import tempfile
import time


def descendants(pid):
    found = set()
    for task in Path(f"/proc/{pid}/task").glob("*"):
        try:
            children = (task / "children").read_text().split()
        except FileNotFoundError:
            continue
        for child in children:
            child = int(child)
            if child not in found:
                found.add(child)
                found.update(descendants(child))
    return found

SHIM = r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
typedef int (*initialize_fn)(const void *, const void *, void *, void *);
int cef_initialize(const void *args, const void *settings, void *app, void *sandbox)
{
    initialize_fn real = (initialize_fn)dlsym(RTLD_NEXT, "cef_initialize");
    int result = 0;
    if (getenv("STALL_AFTER_INIT"))
        result = real(args, settings, app, sandbox);
    fprintf(stderr, "TEST: cef_initialize stalled (result=%d)\n", result);
    fflush(stderr);
    char release;
    if (read(atoi(getenv("STALL_RELEASE_FD")), &release, 1) != 1)
        _exit(90);
    if (!getenv("STALL_AFTER_INIT"))
        result = real(args, settings, app, sandbox);
    return result;
}
'''


def run_case(args, shim, output, name, after_init=False, release=False):
    config = output / name
    config.mkdir()
    read_fd, write_fd = os.pipe()
    env = dict(os.environ, LD_PRELOAD=str(shim), STALL_RELEASE_FD=str(read_fd))
    if after_init:
        env["STALL_AFTER_INIT"] = "1"
    command = [str(args.executable.resolve()), str(args.module.resolve()),
               str(args.data.resolve()), str(config), "hold", "--password-store=basic"]
    evidence = []
    process = subprocess.Popen(command, env=env, pass_fds=(read_fd,),
                               start_new_session=True,
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    os.close(read_fd)
    selector = selectors.DefaultSelector()
    helper_fds = []
    try:
        selector.register(process.stdout, selectors.EVENT_READ)
        deadline = time.monotonic() + 30
        stalled = False
        warning = False
        while time.monotonic() < deadline:
            for key, _ in selector.select(max(0, deadline - time.monotonic())):
                chunk = os.read(key.fd, 65536)
                if not chunk:
                    raise AssertionError(f"{name}: exited before startup observation")
                evidence.append(chunk)
            text = b"".join(evidence)
            stalled = b"TEST: cef_initialize stalled" in text
            warning = b"Browser sources and docks" in text
            if stalled and (release or warning):
                break
        assert stalled, f"{name}: interposition never reached"
        if release:
            os.write(write_fd, b"\n")
        else:
            assert warning, f"{name}: stalled CEF remained silent for 30 seconds"
        if after_init:
            assert b"result=1" in text, "real CEF initialization must succeed"
            helpers = descendants(process.pid)
            assert helpers, "real CEF must create helpers before the stall"
            for pid in helpers:
                try:
                    helper_fds.append(os.pidfd_open(pid))
                except ProcessLookupError:
                    pass
        started = time.monotonic()
        remaining, _ = process.communicate(b"\n", timeout=12)
        evidence.append(remaining)
        assert process.returncode == 0, f"{name}: exit {process.returncode}"
        assert time.monotonic() - started < 12, f"{name}: shutdown exceeded bound"
        text = b"".join(evidence)
        assert b"browser startup and immediate shutdown completed" in text, text.decode(errors="replace")
        if not release:
            assert b"CEF initialization timed out during shutdown" in text, text.decode(errors="replace")
        with selectors.DefaultSelector() as exited:
            for fd in helper_fds:
                exited.register(fd, selectors.EVENT_READ)
            deadline = time.monotonic() + 5
            while exited.get_map() and time.monotonic() < deadline:
                for key, _ in exited.select(max(0, deadline - time.monotonic())):
                    exited.unregister(key.fd)
            assert not exited.get_map(), f"{name}: CEF helpers survived parent exit"
        print(f"{name}: bounded shutdown completed")
    finally:
        selector.close()
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)
        for fd in helper_fds:
            os.close(fd)
        os.close(write_fd)
        (output / (name + ".log")).write_bytes(b"".join(evidence))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ["executable", "module", "data", "output-dir"]:
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--compiler", default="cc")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="stalled-cef-", dir=args.output_dir) as scratch:
        output = Path(scratch).resolve()
        source = output / "stall.c"
        source.write_text(SHIM)
        shim = output / "stall.so"
        subprocess.run([args.compiler, "-shared", "-fPIC", str(source), "-ldl", "-o", str(shim)], check=True)
        # Keep logs outside the scratch directory even when a case fails.
        for name, after, release in [("slow", False, True), ("stalled", False, False),
                                      ("stalled-with-helpers", True, False)]:
            run_case(args, shim, args.output_dir.resolve(), name, after, release)


if __name__ == "__main__":
    main()
