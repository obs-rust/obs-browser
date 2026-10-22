#!/usr/bin/env -S uv run --script
"""Compile the sandbox probe and print what it finds in this environment."""
import argparse
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--output-dir", type=Path, required=True)
parser.add_argument("--cef-dir", type=Path, required=True, help="directory to look for chrome-sandbox in")
parser.add_argument("--cxx", default="c++")
# After "--": a command prefix to run the probe with, e.g. -- runuser -u someone --
argv = sys.argv[1:]
run_as = argv[argv.index("--") + 1:] if "--" in argv else []
args = parser.parse_args(argv[:argv.index("--")] if "--" in argv else argv)
root = Path(__file__).resolve().parent.parent
output = args.output_dir.resolve()
output.mkdir(parents=True, exist_ok=True)
executable = output / "sandbox-probe"
subprocess.run(
    [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", f"-I{root}",
     str(root / "tests" / "sandbox-probe.cpp"), str(root / "browser-sandbox.cpp"), "-o", str(executable)],
    check=True,
)
executable.chmod(0o755)
result = subprocess.run([*run_as, str(executable), str(args.cef_dir.resolve())], capture_output=True, text=True,
                        timeout=30)
print(result.stdout, end="")
print(result.stderr, end="")
if result.returncode:
    raise SystemExit(f"sandbox probe failed: exit {result.returncode}")
