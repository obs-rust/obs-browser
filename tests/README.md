# CEF startup regression

On Linux, build `obs-browser` with CMake and Ninja, then run:

```sh
uv run --no-project python tests/queue_before_init.py --build-dir /path/to/build --output-dir /path/to/output
```

Provide the same CEF/OBS runtime library paths used by that build. The test
relinks its actual browser objects into a small executable; it does not stub
CEF or rewrite the plugin implementation. It must reject a queued task before
CEF initialization without invoking CEF. With the CEF 7871 SDK, the original
implementation aborts with `CefTask_0_CToCpp called with invalid version -1`.
This focused check does not establish browser rendering or frontend parity.
Artifacts are retained in the requested output directory.

## Immediate unload

`startup_shutdown.py` compiles a C harness against the actual libobs and loads
the requested browser module. One process initializes CEF then immediately
shuts down OBS; another creates and releases a browser source before shutdown.
The original CEF 7871 module aborts in both modes with API version `-1`.
Provide an X display (for example an owned Xvfb), the module's staged CEF helper
and resources, and its runtime library search paths. Example:

```sh
uv run --no-project python tests/startup_shutdown.py \
  --include-dir /path/to/obs/libobs --include-dir /path/to/generated/config \
  --include-dir /path/to/simde/include --library-dir /path/to/libobs \
  --module /path/to/staged/obs-browser.so --data /path/to/obs-browser/data \
  --output-dir /path/to/new-output
```

Each output directory must be fresh; logs and profiles are retained. This
covers real successful initialization concurrent with unload. It does not
inject a CEF initialization failure or prove absence of leaks, and it does not
replace full frontend/rendering tests.

## Real initialization failure

After compiling the lifecycle harness above, run the Linux CEF singleton case:

```sh
uv run --no-project python tests/initialization_failure.py \
  --executable /path/to/output/startup-shutdown \
  --module /path/to/staged/obs-browser.so --data /path/to/obs-browser/data \
  --output-dir /path/to/new-failure-output
```

It holds a fresh CEF profile in one real module process, then starts another
against that locked profile. It requires CEF itself to report initialization
failure and the second process to complete OBS shutdown. It releases the holder
through stdin and bounds both process lifetimes. No CEF API is stubbed. Basic
password storage is selected only for these fresh disposable test profiles.

This case requires a Chrome-runtime CEF profile singleton (validated with CEF
7871). The second process waits for the initialization-failure log before stdin
releases shutdown, separating this path from the pre-initialization task race.
It passes against both the original and fixed modules: this is characterization
coverage for preserved failure behavior, not an additional unique RED case.

## Sandbox decision

`sandbox_decision.py` compiles `browser-sandbox.cpp` with
`sandbox-decision.cpp` and runs every case of the decision whether the CEF
sandbox can work (obs-rust/obs-studio#1092): root, user namespaces, and the
state of the `chrome-sandbox` helper. It also checks the fallback warning
(obs-rust/obs-studio#2218): only the fallback warns, with the message for its
cause, and both locale keys exist in `data/locale/en-US.ini`. The decision is
a pure function, so the test needs no CEF, no libobs and no display, and runs
on any platform:

```sh
uv run --no-project python tests/sandbox_decision.py --output-dir /path/to/new-output
```

## Sandbox probe

`sandbox_probe.py` compiles `browser-sandbox.cpp` with `sandbox-probe.cpp` and
prints what `ProbeSandbox` finds in this environment and the decision. Run it
as the user and in the setup to check; a command prefix after `--` runs the
probe through it:

```sh
uv run --no-project python tests/sandbox_probe.py --output-dir /tmp/out --cef-dir /path/to/cef/dir \
  -- systemd-run --pipe --wait --quiet -p User=someone -p RestrictNamespaces=~user --
```

Linux x86_64 (Debian 13 LXC container, kernel 7.0.14, CEF 7871
`chrome-sandbox`): as root, off for every helper state; as a normal user, the
namespace sandbox for every helper state; as a normal user with
`RestrictNamespaces=~user`, off without a helper or with a helper that is not
setuid, and the setuid helper with a helper that is setuid root.

## WebAudio after the audio service dies

`audio_recovery.py` compiles `audio-recovery.c` against the actual libobs and
plays `audio-recovery.html` (a WebAudio tone) through real browser sources
with "Control audio via OBS". Once packets flow it SIGKILLs only this run's
Chromium audio service process, found through the harness's process tree,
and requires packets again in the fifth second after the kill. Scenarios:
`plain` (also checks that the page sees an unchanged `AudioContext`),
`repeated` (three kills, on a page that replaces and freezes the globals a
recovery script might use), `page-suspend` (a context the page suspended
itself stays suspended) and `two-sources`. Run it with an owned Xvfb and
dbus session, no audio server and a fresh HOME, for example inside
`unshare -rn` with `PULSE_SERVER=unix:/nonexistent`:

```sh
uv run --no-project python tests/audio_recovery.py \
  --include-dir /path/to/obs/libobs --include-dir /path/to/generated/config \
  --library-dir /path/to/staged/lib --module /path/to/staged/obs-browser.so \
  --data /path/to/obs-browser/data --libobs-data /path/to/staged/libobs/data \
  --output-dir /path/to/new-output [--scenario plain ...]
```

Linux only; it needs CEF and kills a process, so CI does not run it.

## Audio buffering under load

`audio_buffering.py` compiles `audio-buffering.c` against the actual libobs
and plays `audio-buffering.html` through a real browser source with "Control
audio via OBS": by default a 1 s WebAudio tone every 5 s, as an alert box
plays (`--mode continuous` plays one endless tone). After 20 quiet seconds it
loads every CPU in bursts (busy processes it starts and stops by PID; 4 s on,
6 s off) until the run ends (150 s by default). It requires that OBS's global
audio buffering stays at the value it had before the load
(obs-rust/obs-studio#2335: a stall in the capture under load used to raise it
for good, up to 960 ms), that every alert period has packets, and that each
tone starts in the mix within one audio block (1024 frames, plus 1 ms for
CEF's millisecond timestamps) of its capture timestamp, on the clock video
uses. Run it with an owned Xvfb and dbus session, no audio server and a fresh
HOME, for example inside `unshare -rn` with `PULSE_SERVER=unix:/nonexistent`:

```sh
uv run --no-project python tests/audio_buffering.py \
  --include-dir /path/to/obs/libobs --include-dir /path/to/generated/config \
  --library-dir /path/to/libobs \
  --module /path/to/staged/obs-browser.so --data /path/to/obs-browser/data \
  --libobs-data /path/to/libobs/data --output-dir /path/to/new-output
```

The load makes the result depend on the machine: on a machine with many idle
cores the bursts may not stall the capture at all, and the test then passes
on any build. Check that the output shows late packets (`arrival after the
timestamp ... max` on a build without the fix, `late audio dropped` with it),
or raise `--burners`.

## First browser source off the main thread (macOS)

`first_source_off_main_thread.py` starts a built `OBS-Studio-Rust.app` with a
fresh, seeded configuration and obs-websocket on a test port, and requires a
browser source showing `first-source.html` (a page that turns its background
green) to render (obs-rust/obs-studio#2387). On macOS obs-browser runs CEF on
Qt's main loop, and the first browser source initializes CEF. Scenarios:
`websocket-first` (obs-websocket creates the first source, from its IO
thread) and `collection` (the scene collection has it, so OBS creates it on
the main thread while it loads; the control):

```sh
uv run tests/first_source_off_main_thread.py \
  --app /path/to/build_macos/frontend/RelWithDebInfo/OBS-Studio-Rust.app \
  --output-dir /path/to/new-output [--scenario websocket-first ...]
```

The app keeps its configuration in `~/Library/Application Support/obs-studio-rust`.
The test refuses to run when that folder exists, and moves the folder it made
into the output directory afterwards. macOS only; it needs the real app with
CEF, so CI does not run it.

## Hand-over to the Qt main thread

`qt_main_thread.py` compiles `qt-main-thread.cpp` with
`browser-qt-main-thread.hpp` against Qt Core and checks
`RunOnQtMainThread()` (obs-rust/obs-studio#2391): while Qt's main loop runs,
a worker thread's call runs on the main thread; once the loop has finished,
the call returns at once without running, so a first browser source created
off the main thread while OBS quits cannot hang it. It needs no CEF and no
display:

```sh
uv run --no-project python tests/qt_main_thread.py --output-dir /path/to/new-output \
  [--qt-lib-dir /path/to/lib-with-QtCore.framework]   # macOS
```

On Linux it finds Qt Core through `pkg-config Qt6Core`.
