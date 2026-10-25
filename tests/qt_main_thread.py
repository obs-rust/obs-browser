#!/usr/bin/env -S uv run --script
"""Compile the Qt main-thread hand-over with its test against Qt Core and run it."""
import argparse
import platform
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--output-dir", type=Path, required=True)
parser.add_argument("--qt-lib-dir", type=Path, help="macOS: the directory with QtCore.framework")
parser.add_argument("--cxx", default="c++")
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
output = args.output_dir.resolve()
output.mkdir(parents=True, exist_ok=True)
executable = output / "qt-main-thread"
if platform.system() == "Darwin":
    if not args.qt_lib_dir:
        raise SystemExit("--qt-lib-dir is required on macOS")
    qt = [f"-F{args.qt_lib_dir}", f"-I{args.qt_lib_dir}/QtCore.framework/Headers", "-framework", "QtCore",
          f"-Wl,-rpath,{args.qt_lib_dir}"]
else:
    qt = subprocess.run(["pkg-config", "--cflags", "--libs", "Qt6Core"], capture_output=True, text=True,
                        check=True).stdout.split()
subprocess.run(
    [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-fPIC", f"-I{root}",
     str(root / "tests" / "qt-main-thread.cpp"), "-o", str(executable), *qt],
    check=True,
)
result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
print(result.stdout, end="")
print(result.stderr, end="")
if result.returncode:
    raise SystemExit(f"Qt main-thread check failed: exit {result.returncode}")
