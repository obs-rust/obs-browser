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
