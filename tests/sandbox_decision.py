#!/usr/bin/env -S uv run --script
"""Compile the sandbox decision with its test and run every case."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--output-dir", type=Path, required=True)
parser.add_argument("--cxx", default="c++")
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
output = args.output_dir.resolve()
output.mkdir(parents=True, exist_ok=True)
executable = output / "sandbox-decision"
subprocess.run(
    [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", f"-I{root}",
     str(root / "tests" / "sandbox-decision.cpp"), str(root / "browser-sandbox.cpp"), "-o", str(executable)],
    check=True,
)
result = subprocess.run([str(executable), str(root / "data" / "locale" / "en-US.ini")], capture_output=True,
                        text=True, timeout=30)
print(result.stdout, end="")
print(result.stderr, end="")
if result.returncode:
    raise SystemExit(f"sandbox decision check failed: exit {result.returncode}")
