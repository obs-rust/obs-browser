#!/usr/bin/env -S uv run --script
# /// script
# dependencies = ["obsws-python", "pillow"]
# ///
"""The first browser source renders when it is created off the main thread (macOS, real app).

On macOS obs-browser runs CEF on Qt's main loop (ENABLE_BROWSER_QT_LOOP), and
the first browser source initializes CEF. When obs-websocket creates that
source, the call comes from its IO thread (obs-rust/obs-studio#2387).

Each scenario starts the built OBS-Studio-Rust.app with a fresh, seeded
configuration and obs-websocket on a test port, and requires a browser
source showing first-source.html (a page that turns its background green) to
render within the timeout:

- websocket-first: no browser source at startup; obs-websocket creates it.
- collection: the scene collection has it, so OBS creates it on the main
  thread while it loads (control).

The app keeps its configuration in ~/Library/Application Support/
obs-studio-rust. The test refuses to run when that folder exists, and moves
the folder it made into the output directory afterwards.
"""
import argparse
import base64
import io
import json
import logging
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

import obsws_python as obs
from PIL import Image

GREEN = (18, 171, 52)
CONFIG = Path.home() / "Library/Application Support/obs-studio-rust"


class Failure(Exception):
    pass


# obs-websocket answers 207 (NotReady) until OBS has loaded; when_ready()
# retries those and says so once, so obsws_python's own error log of each
# retried request is left out.
logging.getLogger("obsws_python").setLevel(logging.CRITICAL)


parser = argparse.ArgumentParser()
parser.add_argument("--app", type=Path, required=True, help="the built OBS-Studio-Rust.app")
parser.add_argument("--output-dir", type=Path, required=True)
parser.add_argument("--port", type=int, default=4460)
parser.add_argument("--timeout", type=float, default=30.0)
parser.add_argument("--scenario", action="append", choices=("websocket-first", "collection"))
args = parser.parse_args()
page = (Path(__file__).resolve().parent / "first-source.html").as_posix()
output = args.output_dir.resolve()
if output.exists():
    sys.exit(f"{output} exists; use a new output directory")
output.mkdir(parents=True)


def seed(collection: bool) -> None:
    (CONFIG / "plugin_config/obs-websocket").mkdir(parents=True)
    (CONFIG / "user.ini").write_text("[General]\nFirstRun=true\n[Basic]\nSceneCollection=Test\nSceneCollectionFile=Test\n")
    # No modal startup dialogs: the macOS permissions dialog and the update
    # channel notice (obs-rust/obs-studio#2388) would wait for a click.
    (CONFIG / "global.ini").write_text("[General]\nMacOSPermissionsDialogLastShown=1\nAutoBetaOptIn=true\n")
    (CONFIG / "plugin_config/obs-websocket/config.json").write_text(json.dumps(
        {"first_load": False, "server_enabled": True, "server_port": args.port, "alerts_enabled": False,
         "auth_required": False}))
    items, sources = [], []
    if collection:
        items = [{"name": "bst", "source_uuid": "00000000-0000-4000-8000-000000000002", "visible": True}]
        sources = [{"id": "browser_source", "versioned_id": "browser_source", "name": "bst",
                    "uuid": "00000000-0000-4000-8000-000000000002",
                    "settings": {"width": 320, "height": 180, "is_local_file": True, "local_file": page}}]
    scene = {"id": "scene", "versioned_id": "scene", "name": "Scene", "uuid": "00000000-0000-4000-8000-000000000001",
             "settings": {"items": items, "id_counter": len(items)}}
    (CONFIG / "basic/scenes").mkdir(parents=True)
    (CONFIG / "basic/scenes/Test.json").write_text(json.dumps(
        {"name": "Test", "current_scene": "Scene", "current_program_scene": "Scene",
         "scene_order": [{"name": "Scene"}], "sources": [scene, *sources]}))


def wait_for_port(deadline: float) -> None:
    while time.monotonic() < deadline:
        try:
            socket.create_connection(("127.0.0.1", args.port), timeout=0.5).close()
            return
        except OSError:
            time.sleep(0.5)
    raise Failure("obs-websocket did not come up")


def when_ready(request, deadline: float):
    """obs-websocket answers 207 (NotReady) until OBS has loaded; retry until the deadline."""
    while True:
        try:
            return request()
        except obs.error.OBSSDKRequestError as e:
            if e.code != 207 or time.monotonic() >= deadline:
                raise
            if not getattr(when_ready, "waited", False):
                print("     waiting: OBS is not ready yet (obs-websocket 207)")
                when_ready.waited = True
            time.sleep(0.5)


def rendered(client: obs.ReqClient) -> tuple[int, int, int]:
    shot = client.get_source_screenshot("bst", "png", 32, 18, -1).image_data
    return Image.open(io.BytesIO(base64.b64decode(shot.split(",", 1)[1]))).convert("RGB").getpixel((16, 9))


def run(scenario: str) -> None:
    when_ready.waited = False
    out = output / scenario
    out.mkdir()
    if CONFIG.exists():
        raise Failure(f"{CONFIG} exists; it is not this test's")
    seed(collection=scenario == "collection")
    log = (out / "stdout.log").open("wb")
    executable = args.app / "Contents/MacOS" / args.app.stem
    proc = subprocess.Popen([executable, "--multi", "--disable-shutdown-check", "--disable-missing-files-check"],
                            stdout=log, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + args.timeout
        wait_for_port(deadline)
        client = None
        while client is None and time.monotonic() < deadline:
            try:
                client = obs.ReqClient(host="127.0.0.1", port=args.port, timeout=10)
            except OSError:
                time.sleep(0.5)
        if client is None:
            raise Failure("could not connect to obs-websocket")
        if scenario == "websocket-first":
            settings = {"width": 320, "height": 180, "is_local_file": True, "local_file": page}
            scene = when_ready(client.get_current_program_scene, deadline).scene_name
            when_ready(lambda: client.create_input(scene, "bst", "browser_source", settings, True), deadline)
        pixel = None
        while time.monotonic() < deadline:
            pixel = when_ready(lambda: rendered(client), deadline)
            if pixel == GREEN:
                print(f"ok   {scenario}: the page rendered")
                return
            time.sleep(1)
        raise Failure(f"{scenario}: the page did not render within {args.timeout:.0f} s (pixel {pixel})")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
        time.sleep(1)
        if CONFIG.exists():
            shutil.move(CONFIG, out / "config")


failures = 0
for scenario in args.scenario or ("collection", "websocket-first"):
    try:
        run(scenario)
    except Failure as e:
        print(f"FAIL {e}")
        failures += 1
sys.exit(1 if failures else 0)
