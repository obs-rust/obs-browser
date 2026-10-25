#!/usr/bin/env -S uv run --script
"""Browser source audio under load must not raise OBS's audio buffering (Linux, real CEF).

Runs the real module in a fresh process and profile with a browser source
that plays a local WebAudio page with "Control audio via OBS": by default a
1 s tone every 5 s, as an alert box does. After a quiet start it loads every
CPU in bursts (busy processes this script starts and stops by PID). Load
does not stall CEF's audio delivery on every machine or run, so the harness
also stalls it a few times, as load does: the packets captured during the
stall then arrive at once, late, with their capture timestamps. It requires
that:
- OBS's global audio buffering stays at the value it had before the load;
- packets keep flowing: every alert reaches the source and the mix;
- each tone starts in the mix within one audio block (plus CEF's 1 ms
  timestamp resolution) of its capture timestamp, on the clock video uses.
"""
import argparse
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import threading
import time

BLOCK_NS = 1024 * 1_000_000_000 // 48000  # one OBS audio block (AUDIO_OUTPUT_FRAMES)
# CEF's packet timestamps are whole milliseconds.
SYNC_NS = BLOCK_NS + 1_000_000
STALL = re.compile(r"TEST stalling audio delivery")
BUFFERING = re.compile(r"total audio buffering is now (\d+) milliseconds")
DROPPED = re.compile(r"Dropped ([\d.]+) ms of audio that arrived too late")

parser = argparse.ArgumentParser()
parser.add_argument("--include-dir", type=Path, action="append", required=True)
parser.add_argument("--library-dir", type=Path, action="append", required=True)
parser.add_argument("--module", type=Path, required=True)
parser.add_argument("--data", type=Path, required=True)
parser.add_argument("--libobs-data", type=Path, required=True)
parser.add_argument("--output-dir", type=Path, required=True)
parser.add_argument("--compiler", default="cc")
parser.add_argument("--mode", choices=["alerts", "continuous"], default="alerts")
parser.add_argument("--seconds", type=int, default=150, help="total run time")
parser.add_argument("--quiet-seconds", type=int, default=20, help="run time before the load starts")
parser.add_argument("--burst", type=float, default=4.0, help="seconds of each load burst")
parser.add_argument("--pause", type=float, default=6.0, help="seconds between load bursts")
parser.add_argument("--burners", type=int, default=2 * (os.cpu_count() or 1), help="busy processes per burst")
parser.add_argument("--stalls", help="delivery stalls, alert:delay_s:length_s,... (alerts counted from 1, "
                    "a continuous tone is alert 1); empty for none")
args = parser.parse_args()
if args.stalls is None:
    args.stalls = "6:0.3:0.5,12:0.3:0.9,18:0.1:0.3" if args.mode == "alerts" else "1:25.3:0.5,1:55.3:0.9,1:85.1:0.3"
output = args.output_dir.resolve()
output.mkdir(parents=True, exist_ok=True)
executable = output / "audio-buffering"
here = Path(__file__).resolve().parent
command = [args.compiler, "-DHAVE_OBSCONFIG_H"]
for directory in args.include_dir:
    command.append("-I" + str(directory.resolve()))
command.append(str(here / "audio-buffering.c"))
for directory in args.library_dir:
    command.extend(["-L" + str(directory.resolve()), "-Wl,-rpath," + str(directory.resolve())])
command.extend(["-rdynamic", "-lobs", "-ldl", "-lX11", "-lm", "-lpthread", "-o", str(executable)])
subprocess.run(command, check=True)

page = output / "page.html"
page.write_text((here / "audio-buffering.html").read_text().replace("@MODE@", args.mode))
config = output / "config"
config.mkdir()  # The process needs a fresh CEF profile.

packets = []  # (arrival ns, timestamp ns, frames)
source_onsets, mix_onsets = [], []
buffering = []  # (monotonic ns when logged, total ms)
dropped = []  # ms of late audio the module dropped (after the fix)
stalled = []  # stalls the harness made
log = open(output / "harness.log", "w")
process = subprocess.Popen(
    [str(executable), str(args.module.resolve()), str(args.data.resolve()),
     str(args.libobs_data.resolve()) + "/", str(config), str(page)],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    env=dict(os.environ, STALLS=args.stalls))


def read():
    for raw in process.stdout:
        line = raw.decode(errors="replace").rstrip("\n")
        log.write(line + "\n")
        kind, _, rest = line.partition(" ")
        if kind == "PKT":
            packets.append(tuple(int(n) for n in rest.split()))
        elif kind == "SRC_ONSET":
            source_onsets.append(int(rest))
        elif kind == "MIX_ONSET":
            mix_onsets.append(int(rest))
        elif kind == "LOG":
            match = BUFFERING.search(rest)
            if match:
                buffering.append((time.monotonic_ns(), int(match.group(1))))
            if STALL.search(rest):
                stalled.append(rest)
            match = DROPPED.search(rest)
            if match:
                dropped.append(float(match.group(1)))


reader = threading.Thread(target=read, daemon=True)
reader.start()
burners = []


def stop_burners():
    for burner in burners:
        burner.kill()
        burner.wait()
    burners.clear()


start = time.monotonic_ns()
load_start = start + args.quiet_seconds * 1_000_000_000
end = start + args.seconds * 1_000_000_000
bursts = 0
try:
    time.sleep(args.quiet_seconds)
    while time.monotonic_ns() < end and process.poll() is None:
        for _ in range(args.burners):
            burners.append(subprocess.Popen([sys.executable, "-I", "-c", "while True: pass"]))
        bursts += 1
        time.sleep(args.burst)
        stop_burners()
        time.sleep(max(0.0, min(args.pause, (end - time.monotonic_ns()) / 1e9)))
finally:
    stop_burners()
    if process.poll() is None:
        try:
            process.stdin.write(b"q\n")
            process.stdin.close()
            process.wait(timeout=60)
        except (OSError, subprocess.TimeoutExpired):
            process.send_signal(signal.SIGKILL)
            process.wait()
    reader.join(timeout=10)
    log.close()

failures = []
if process.returncode:
    failures.append(f"harness exited {process.returncode}")

# The audio clock is os_gettime_ns(): CLOCK_MONOTONIC, as time.monotonic_ns().
before = [ms for when, ms in buffering if when < load_start]
startup = max(before) if before else None
peak = max((ms for _, ms in buffering), default=None)
print(f"audio buffering: {startup} ms before the load, {peak} ms at most; "
      f"{bursts} load bursts of {args.burners} busy processes")
if startup is None:
    failures.append("no audio buffering was logged before the load (no audio?)")
elif peak != startup:
    raised = [f"{(when - start) / 1e9:.1f} s: {ms} ms" for when, ms in buffering if ms > startup]
    failures.append(f"audio buffering rose from {startup} ms: " + ", ".join(raised))

expected_stalls = len([s for s in args.stalls.split(",") if s])
print(f"{len(stalled)} of {expected_stalls} delivery stalls made")
if len(stalled) != expected_stalls:
    failures.append(f"only {len(stalled)} of {expected_stalls} delivery stalls were made")
if dropped:
    print(f"late audio dropped {len(dropped)} times, {sum(dropped):.0f} ms in all, at most {max(dropped):.0f} ms")
lateness = sorted(arrival - timestamp for arrival, timestamp, _ in packets if arrival >= load_start)
if lateness:
    print(f"{len(packets)} packets; under load, arrival after the timestamp: "
          f"median {lateness[len(lateness) // 2] / 1e6:.1f} ms, max {lateness[-1] / 1e6:.1f} ms")

# Packets keep flowing: every period of the page has packets.
period = 5_000_000_000 if args.mode == "alerts" else 1_000_000_000
first = start + 5_000_000_000
silent = []
for window in range(first, end - period, period):
    if not any(window <= arrival < window + period for arrival, _, _ in packets):
        silent.append(f"{(window - start) / 1e9:.0f} s")
if silent:
    failures.append("no packets in the periods starting at " + ", ".join(silent))

# Sync: each tone starts in the mix where its capture timestamp puts it.
checked = 0
if args.mode == "alerts":
    expected = (end - first) // period - 2
    if len(source_onsets) < expected:
        failures.append(f"only {len(source_onsets)} tones reached the source, expected at least {expected}")
    for onset in source_onsets:
        if onset < first or onset > end - 3_000_000_000:
            continue
        nearest = min(mix_onsets, key=lambda mixed: abs(mixed - onset), default=None)
        checked += 1
        if nearest is None or abs(nearest - onset) > 1_000_000_000:
            failures.append(f"the tone captured at {(onset - start) / 1e9:.1f} s is missing from the mix")
        elif abs(nearest - onset) > SYNC_NS:
            failures.append(f"the tone captured at {(onset - start) / 1e9:.1f} s starts in the mix "
                            f"{(nearest - onset) / 1e6:+.1f} ms off")
    print(f"{checked} tones checked against the mix")

if failures:
    for failure in failures:
        print("FAIL: " + failure)
    raise SystemExit(f"failed (log: {output / 'harness.log'})")
print("passed")
