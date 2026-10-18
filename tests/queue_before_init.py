#!/usr/bin/env -S uv run --script
"""Link the real browser objects and reject a task before CEF initialization."""
import argparse
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--build-dir", type=Path, required=True)
parser.add_argument("--output-dir", type=Path, required=True)
args = parser.parse_args()
build = args.build_dir.resolve()
output = args.output_dir.resolve()
output.mkdir(parents=True, exist_ok=True)
commands = subprocess.check_output(["ninja", "-C", str(build), "-t", "commands", "obs-browser"], text=True)
link = None
for line in commands.splitlines():
    parts = shlex.split(line)
    if "-shared" not in parts or "-o" not in parts:
        continue
    position = parts.index("-o")
    if Path(parts[position + 1]).name != "obs-browser.so":
        continue
    start = max((i + 1 for i, part in enumerate(parts[:position]) if part == "&&"), default=0)
    parts = parts[start:]
    if "&&" in parts:
        parts = parts[:parts.index("&&")]
    link = [part.replace("\\$", "$") for part in parts]
if link is None:
    raise SystemExit("no Linux browser module link command found; build obs-browser with CMake/Ninja first")
link.remove("-shared")
executable = output / "queue-before-init"
link[link.index("-o") + 1] = str(executable)
link.insert(1, str(Path(__file__).with_name("queue-before-init.cpp").resolve()))
subprocess.run(link, cwd=build, check=True)
result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
print(result.stdout, end="")
print(result.stderr, end="")
if result.returncode:
    raise SystemExit(f"pre-initialization task check failed: exit {result.returncode}")
