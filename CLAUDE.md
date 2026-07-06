# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`btwin` scans for nearby Bluetooth devices on Windows and reports them through a small C ABI.
- `src/btwin.cpp` — C++/WinRT implementation. Uses `DeviceWatcher` with an AQS filter on the
  Bluetooth Classic and LE `ProtocolId` GUIDs to enumerate devices, and maps each hit into a plain
  `bt_device_t`. Built as a **shared library** named `btwin`.
  Devices are **streamed as they appear**: the watcher tracks each endpoint by
  `DeviceInformation.Id()` and reports a device the moment `System.Devices.Aep.IsPresent` becomes
  `true` (via either the `Added` or `Updated` callback), rather than dumping the cached endpoint
  list at the end of the scan. See "Conventions / gotchas" for the exact semantics.
- `src/btwin.h` — the C ABI surface. Everything is wrapped in `extern "C"` and marked
  `__declspec(dllexport)`. `btwin_t` is an opaque handle (really a `BtWatcher*`). This is the
  boundary: callers stay in C, all WinRT lives behind it.
- `src/main.c` — C executable that links `btwin`, allocates a watcher with callbacks, starts it,
  and blocks in `btwin_join` until enumeration completes.

The C/C++ split is deliberate: `main.c` is compiled as C and must only depend on `btwin.h`.

## Build

vcpkg is a **git submodule**, so a fresh clone needs:
```
git submodule update --init --recursive
```

Dependencies (`fmt`, `cppwinrt`) are declared in `vcpkg.json` and installed automatically by the
vcpkg toolchain during CMake configure.

**This project must be built natively on Windows with MSVC.** The WinRT headers it depends on
(`windows.h`, `winrt/*`) are only available in the MSVC toolchain, so use the `windows-x64` preset
(Visual Studio 2026 generator, output in `build/windows-x64/`):
```
cmake --preset windows-x64
cmake --build build/windows-x64 --config Debug
```

`CMakePresets.json` also defines a `vcpkg` preset that cross-compiles from Linux with MinGW-w64
(`mingw-w64-x86_64.cmake`). **This does not currently work** — MinGW lacks the WinRT headers — so it
is not a usable build path; keep it only as a reference.

There is no test suite and no linter configured.

## Conventions / gotchas

- **Device streaming / presence:** the watcher requests `System.Devices.Aep.IsPresent` and wires
  both the `Added` and `Updated` callbacks. `Added` reflects Windows' cached endpoint list (often
  `IsPresent=false`); the real "device is nearby now" signal is the `false→true` presence
  transition, which typically arrives via `Updated`. Since `Updated` carries only the changed
  property subset plus `Id()`, each endpoint's name/mac/standard is stashed in a map keyed by
  `DeviceInformation.Id()` at `Added` time so an `Updated` can be correlated back. A device is
  reported through `on_device` **exactly once**, on the first confirmed presence (`reported` latch).
  A device that never reports `IsPresent=true` is never reported — this is deliberate. Do **not**
  call `getBtDeviceStandard` on an `Updated` payload: `ProtocolId` is absent there and the unguarded
  unbox would throw.
- `btwin_join` blocks on a `std::condition_variable` that is signaled from the WinRT
  `EnumerationCompleted` callback — i.e. it returns once the initial device scan finishes, not on a
  timer. Presence transitions that occur during the scan are streamed to `on_device` before join
  returns.
- `btwin_start`/`btwin_stop` guard on `watcher.Status()` and swallow WinRT errors: a repeated or
  out-of-order start/stop is a logged no-op, and no exception crosses the C ABI. Their start/stop
  chatter logs at `BT_LOG_DEBUG` (level 4, below the default `BT_LOG_INFO` threshold, so off by
  default).
- `btwin_adapter_exists`/`btwin_adapter_is_on` run their WinRT queries on a dedicated worker thread
  (`run_on_mta_thread` in `btwin.cpp`) that owns its own MTA, so they are safe to call regardless of
  the caller's apartment.
- WinRT GUIDs identify the radio standard: Classic = `{e0cbf06c-...f974}`, LE = `{bb7bb05e-...4d49}`.
