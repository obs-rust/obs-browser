#!/usr/bin/env -S uv run --script
"""Test the actual startup handshake without CEF or OBS development libraries."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output-dir", type=Path, required=True)
parser.add_argument("--compiler", default="c++")
args = parser.parse_args()
args.output_dir.mkdir(parents=True, exist_ok=True)
executable = args.output_dir.resolve() / "startup-state"
subprocess.run([args.compiler, "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
                str(Path(__file__).with_name("startup-state.cpp")), "-o", str(executable)], check=True)
for _ in range(100):
    subprocess.run([str(executable)], check=True, timeout=2)
print("startup handshake: 100 runs passed (failure, success, slow recovery, late return after timeout)")
