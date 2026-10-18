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
