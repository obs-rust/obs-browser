#!/usr/bin/env -S uv run --script
"""Force real CefInitialize failure by holding its profile in another process."""
import argparse
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("--executable", type=Path, required=True)
parser.add_argument("--module", type=Path, required=True)
parser.add_argument("--data", type=Path, required=True)
parser.add_argument("--output-dir", type=Path, required=True)
args = parser.parse_args()
output = args.output_dir.resolve()
output.mkdir(parents=True, exist_ok=True)
config = output / "shared-config"
config.mkdir()
command = [str(args.executable.resolve()), str(args.module.resolve()), str(args.data.resolve()), str(config)]
with (output / "holder.log").open("w") as log:
    holder = subprocess.Popen(command + ["hold", "--password-store=basic"], stdin=subprocess.PIPE,
                              stdout=log, stderr=subprocess.STDOUT, text=True)
    try:
        deadline = time.monotonic() + 15
        while not any(config.rglob("SingletonLock")):
            if holder.poll() is not None:
                raise SystemExit(f"profile holder exited: {holder.returncode}; see {output / 'holder.log'}")
            if time.monotonic() >= deadline:
                raise SystemExit("CEF did not acquire its real profile singleton lock")
            time.sleep(0.05)
        with (output / "failure.log").open("w") as failure_log:
            failed = subprocess.Popen(command + ["hold", "--password-store=basic"], stdin=subprocess.PIPE,
                                      stdout=failure_log, stderr=subprocess.STDOUT, text=True)
            try:
                deadline = time.monotonic() + 20
                while "CEF failed to initialize" not in (output / "failure.log").read_text():
                    if failed.poll() is not None:
                        raise SystemExit(f"second process exited before observing initialization failure: {failed.returncode}")
                    if time.monotonic() >= deadline:
                        raise SystemExit("second process did not exercise the actual CEF initialization-failure branch")
                    time.sleep(0.05)
                # Release shutdown only after observing the real failure. This
                # avoids confusing it with the separate pre-initialization race.
                try:
                    failed.communicate("\n", timeout=20)
                except subprocess.TimeoutExpired as error:
                    raise SystemExit("unload hung after real CEF initialization failure") from error
                if failed.returncode:
                    raise SystemExit(f"initialization-failure process exited {failed.returncode}")
            finally:
                if failed.poll() is None:
                    failed.kill()
                    failed.wait(timeout=5)
        evidence = (output / "failure.log").read_text()
        if "browser startup and immediate shutdown completed" not in evidence:
            raise SystemExit("second process did not complete OBS shutdown")
        print("real CEF initialization failure followed by OBS shutdown completed")
    finally:
        if holder.poll() is None:
            try:
                holder.communicate("\n", timeout=20)
            except subprocess.TimeoutExpired:
                holder.kill()
                holder.wait(timeout=5)
        if holder.returncode:
            raise SystemExit(f"profile holder failed to shut down: {holder.returncode}")
