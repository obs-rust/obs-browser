#!/usr/bin/env -S uv run --script
"""Browser source WebAudio after Chromium's audio service dies (Linux, real CEF).

Each scenario starts the real module in a fresh process and profile, with
browser sources that play a local WebAudio page with "Control audio via
OBS". Once audio packets flow, it SIGKILLs only this run's Chromium audio
service process (found through the harness's process tree) and requires
the packets to flow again within 5 seconds.
"""
import argparse
import os
from pathlib import Path
import queue
import signal
import subprocess
import threading
import time

AUDIO_SERVICE = b"--utility-sub-type=audio.mojom.AudioService"
RECOVERY_SECONDS = 5.0


class Failure(Exception):
    pass

parser = argparse.ArgumentParser()
parser.add_argument("--include-dir", type=Path, action="append", required=True)
parser.add_argument("--library-dir", type=Path, action="append", required=True)
parser.add_argument("--module", type=Path, required=True)
parser.add_argument("--data", type=Path, required=True)
parser.add_argument("--libobs-data", type=Path, required=True)
parser.add_argument("--output-dir", type=Path, required=True)
parser.add_argument("--compiler", default="cc")
parser.add_argument("--scenario", action="append", help="run only these scenarios")
args = parser.parse_args()
output = args.output_dir.resolve()
output.mkdir(parents=True, exist_ok=True)
executable = output / "audio-recovery"
here = Path(__file__).resolve().parent
command = [args.compiler, "-DHAVE_OBSCONFIG_H"]
for directory in args.include_dir:
    command.append("-I" + str(directory.resolve()))
command.append(str(here / "audio-recovery.c"))
for directory in args.library_dir:
    command.extend(["-L" + str(directory.resolve()), "-Wl,-rpath," + str(directory.resolve())])
command.extend(["-lobs", "-lX11", "-lpthread", "-o", str(executable)])
subprocess.run(command, check=True)


def descendants(root):
    children = {}
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text()
        except OSError:
            continue
        parent = int(stat.rsplit(")", 1)[1].split()[1])
        children.setdefault(parent, []).append(int(entry.name))
    found, pending = [], [root]
    while pending:
        for child in children.get(pending.pop(), []):
            found.append(child)
            pending.append(child)
    return found


def audio_services(root):
    services = []
    for pid in descendants(root):
        try:
            if AUDIO_SERVICE in Path(f"/proc/{pid}/cmdline").read_bytes():
                services.append(pid)
        except OSError:
            pass
    return services


class Run:
    def __init__(self, name, mode, sources):
        self.name = name
        self.sources = sources
        page = output / (name + ".html")
        page.write_text((here / "audio-recovery.html").read_text().replace("@MODE@", mode))
        config = output / (name + "-config")
        config.mkdir()  # Each process needs a fresh CEF profile.
        self.log = open(output / (name + ".log"), "w")
        self.lines = queue.Queue()
        self.samples = []  # (monotonic time, cumulative packets per source)
        self.states = []  # (monotonic time, page state line)
        self.compat = None
        self.process = subprocess.Popen(
            [str(executable), str(args.module.resolve()), str(args.data.resolve()),
             str(args.libobs_data.resolve()) + "/", str(config), str(page), str(sources)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for raw in self.process.stdout:
            now = time.monotonic()
            line = raw.decode(errors="replace").rstrip("\n")
            if line.startswith("COUNTS "):
                self.samples.append((now, [int(n) for n in line.split()[1:]]))
                continue
            self.log.write(f"[{now:.3f}] {line}\n")
            if "audio-test state " in line:
                self.states.append((now, line.split("audio-test state ", 1)[1].split(" (")[0]))
            if "audio-test compat " in line:
                self.compat = line.split("audio-test compat ", 1)[1].split(" (")[0]
        self.lines.put(None)

    def packets(self, start, end):
        """Packets per source that arrived between two monotonic times."""
        def at(moment):
            before = [counts for when, counts in self.samples if when <= moment]
            return before[-1] if before else [0] * self.sources
        first, last = at(start), at(end)
        return [b - a for a, b in zip(first, last)]

    def first_packet(self, after):
        """Time of the first sample after `after` in which every source got packets."""
        previous = None
        for when, counts in self.samples:
            if when > after and previous and all(b > a for a, b in zip(previous, counts)):
                return when
            previous = counts
        return float("nan")

    def wait(self, condition, seconds, what):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.fail(f"harness exited ({self.process.returncode}) while waiting for {what}")
            if condition():
                return
            time.sleep(0.1)
        self.fail(f"timed out after {seconds} s waiting for {what}")

    def flowing(self):
        now = time.monotonic()
        return all(n > 0 for n in self.packets(now - 1.0, now))

    def kill_audio_service(self):
        self.wait(lambda: audio_services(self.process.pid), 10, "the audio service process")
        pids = audio_services(self.process.pid)
        for pid in pids:
            os.kill(pid, signal.SIGKILL)
        self.log.write(f"[{time.monotonic():.3f}] TEST killed audio service {pids}\n")
        return time.monotonic()

    def fail(self, message):
        self.stop()
        raise Failure(f"{message} (log: {self.log.name})")

    def stop(self):
        if self.process.poll() is None:
            try:
                self.process.stdin.write(b"q\n")
                self.process.stdin.close()
                self.process.wait(timeout=30)
            except (OSError, subprocess.TimeoutExpired):
                self.process.kill()
                self.process.wait()
        self.lines.get(timeout=10)
        self.log.close()


def recover(run, kills):
    for attempt in range(1, kills + 1):
        run.wait(run.flowing, 30, "audio packets from every source")
        killed = run.kill_audio_service()
        time.sleep(RECOVERY_SECONDS)
        late = run.packets(killed + RECOVERY_SECONDS - 1.0, killed + RECOVERY_SECONDS)
        if not all(n > 0 for n in late):
            run.fail(f"kill {attempt}: packets per source in the 5th second after the kill: {late}")
        print(f"{run.name}: kill {attempt}: audio resumed, packets in the 5th second: {late}, "
              f"first packet {run.first_packet(killed + 1.0) - killed:.1f} s after the kill")
    return killed


def plain():
    run = Run("plain", "plain", 1)
    run.wait(lambda: run.compat is not None, 30, "the compatibility report")
    if run.compat != "ok":
        run.fail("page sees a changed AudioContext: " + run.compat)
    recover(run, 1)
    run.stop()


def repeated():
    run = Run("repeated", "hostile", 1)
    recover(run, 3)
    run.stop()


def page_suspend():
    run = Run("page-suspend", "suspend", 1)
    run.wait(lambda: run.states and run.states[-1][1] == "running suspended", 30,
             "the page to suspend its second context")
    killed = recover(run, 1)
    after = [state for when, state in run.states if when >= killed]
    if any(state.split()[1] != "suspended" for state in after):
        run.fail("the context the page suspended was resumed: " + ", ".join(after))
    if run.states[-1][1] != "running suspended":
        run.fail("states after the kill: " + run.states[-1][1])
    run.stop()


def two_sources():
    run = Run("two-sources", "plain", 2)
    recover(run, 1)
    run.stop()


scenarios = {"plain": plain, "repeated": repeated, "page-suspend": page_suspend, "two-sources": two_sources}
failed = []
for name in args.scenario or scenarios:
    try:
        scenarios[name]()
        print(name + ": passed")
    except Failure as failure:
        print(f"{name}: FAILED: {failure}")
        failed.append(name)
if failed:
    raise SystemExit("failed: " + ", ".join(failed))
