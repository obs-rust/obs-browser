#!/usr/bin/env -S uv run --script
"""Run immediate shutdown against a real Linux OBS/CEF module, without stubs."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--include-dir", type=Path, action="append", required=True)
parser.add_argument("--library-dir", type=Path, action="append", required=True)
parser.add_argument("--module", type=Path, required=True)
parser.add_argument("--data", type=Path, required=True)
parser.add_argument("--output-dir", type=Path, required=True)
parser.add_argument("--compiler", default="cc")
args = parser.parse_args()
output = args.output_dir.resolve()
output.mkdir(parents=True, exist_ok=True)
executable = output / "startup-shutdown"
command = [args.compiler, "-DHAVE_OBSCONFIG_H"]
for directory in args.include_dir:
    command.append("-I" + str(directory.resolve()))
command.append(str(Path(__file__).with_name("startup-shutdown.c").resolve()))
for directory in args.library_dir:
    command.extend(["-L" + str(directory.resolve()), "-Wl,-rpath," + str(directory.resolve())])
command.extend(["-lobs", "-ldl", "-o", str(executable)])
subprocess.run(command, check=True)
for mode in ["initialize", "source"]:
    config = output / (mode + "-config")
    config.mkdir()  # Each process needs a fresh CEF profile.
    command = [str(executable), str(args.module.resolve()), str(args.data.resolve()), str(config), mode]
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired as error:
        log = (error.stdout or b"") + (error.stderr or b"")
        (output / (mode + ".log")).write_bytes(log)
        raise SystemExit(f"{mode}: startup/shutdown timed out after 30 seconds") from error
    (output / (mode + ".log")).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit(f"{mode}: startup/shutdown failed: exit {result.returncode}")
    print(mode + ": real browser startup and immediate shutdown completed")
