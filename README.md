# btwin

Scan for nearby Bluetooth devices on Windows, exposed through a small C ABI.

`btwin` enumerates nearby Bluetooth Classic and Low Energy devices and reports each one — name, MAC
address, and radio standard — through a handful of `extern "C"` functions. All the Windows-specific
machinery (C++/WinRT, `DeviceWatcher`) lives behind a shared library, `btwin.dll`, so callers only
have to deal with plain C.

## Overview

The implementation is deliberately split across a C/C++ boundary:

- **`btwin.dll`** (built from `src/btwin.cpp`) is C++/WinRT. It creates a
  `DeviceWatcher` with an AQS filter on the Bluetooth Classic and LE `ProtocolId` GUIDs, and maps
  every device it finds into a plain [`bt_device_t`](src/btwin.h).
- **Callers stay in C** and depend only on `src/btwin.h`. The `btwin_t` handle is opaque, and no
  WinRT types ever cross the boundary.

Each device's radio standard is determined from its `System.Devices.Aep.ProtocolId` property:

| Standard      | GUID                                     |
| ------------- | ---------------------------------------- |
| `BT_CLASSIC`  | `{e0cbf06c-cd8b-4647-bb8a-263b43f0f974}` |
| `BT_LE`       | `{bb7bb05e-5972-42b5-94fc-76eaa7084d49}` |

## Requirements

- **Windows.** The WinRT headers this project uses (`windows.h`, `winrt/*`) ship only with the MSVC
  toolchain, so it must be built natively on Windows with MSVC.
- **CMake ≥ 3.10.**
- **vcpkg**, bundled as a git submodule. Its toolchain installs the dependencies (`fmt`, `cppwinrt`,
  declared in `vcpkg.json`) automatically during CMake configure.

## Building

vcpkg is a submodule, so a fresh clone needs it initialized first:

```sh
git submodule update --init --recursive
```

Then configure and build with the `windows-x64` preset (Visual Studio generator, output in
`build/windows-x64/`):

```sh
cmake --preset windows-x64
cmake --build build/windows-x64 --config Debug
# or, for an optimized build:
cmake --build build/windows-x64 --config Release
```

This produces `btwin.dll` (the library) and `BTWin.exe` (the example consumer built from
`src/main.c`).

> **Note:** `CMakePresets.json` also defines a `vcpkg` preset that cross-compiles from Linux with
> MinGW-w64. **This does not work** — MinGW lacks the WinRT headers — and is kept only as a
> reference. Build natively on Windows.

## Usage

Link against `btwin` and include `btwin.h`. Provide callbacks via `btwin_params_t`, then drive
the watcher through its lifecycle:

```c
#include <stdio.h>
#include "btwin.h"

void on_device(bt_device_t device, void *user_data) {
    printf("found %.*s  mac=%s  standard=%d\n",
           (int)device.name_len, device.name, device.mac, device.standard);
}

void on_end(void *user_data) {
    printf("initial scan complete\n");
}

int main(void) {
    const btwin_params_t params = {
        .callback = on_device,  // invoked once per discovered device
        .on_end   = on_end,     // invoked when the initial enumeration finishes
    };

    btwin_t watcher = btwin_alloc(&params, NULL);
    btwin_start(watcher);
    btwin_join(watcher);   // blocks until the initial scan completes
    btwin_free(watcher);
    return 0;
}
```

`btwin_join` does **not** wait on a timer — it blocks until the watcher's initial device
enumeration completes (signaled internally from the WinRT `EnumerationCompleted` callback). See
`src/main.c` for the example consumer this snippet is adapted from.

## C ABI reference

All functions are declared in [`src/btwin.h`](src/btwin.h), wrapped in `extern "C"` and marked
`__declspec(dllexport)`.

| Function                                                          | Description                                                                |
| ----------------------------------------------------------------- | -------------------------------------------------------------------------- |
| `btwin_t btwin_alloc(const btwin_params_t *params, void *user_data)` | Allocate a watcher with the given callbacks and user data.        |
| `int btwin_start(btwin_t watcher)`                               | Start enumerating Bluetooth devices.                                       |
| `int btwin_stop(btwin_t watcher)`                                | Stop the watcher.                                                          |
| `void btwin_join(btwin_t watcher)`                               | Block until the initial enumeration completes.                             |
| `void btwin_free(btwin_t watcher)`                               | Destroy a watcher.                                                         |

### Types

```c
typedef enum {
    BT_UNKNOWN,
    BT_CLASSIC,
    BT_LE,
} bt_standard_t;

typedef struct {
    uint32_t      name_len;   // length of `name` (not NUL-terminated)
    char          name[128];
    char          mac[18];
    bt_standard_t standard;
} bt_device_t;

typedef void (*bt_device_callback_t)(bt_device_t device, void *user_data);
typedef void (*on_end_t)(void *user_data);

typedef struct {
    bt_device_callback_t callback;   // called per discovered device
    on_end_t             on_end;      // called when enumeration completes
} btwin_params_t;

typedef void *btwin_t;               // opaque handle
```

## Prebuilt binaries

GitHub Actions (`.github/workflows/build.yml`) builds the library for **x64** and **ARM64** on every
`v*` tag and attaches the results to the GitHub Release as `btwin-<tag>-<arch>.zip`. Each archive
contains:

- `release/btwin.dll`, `release/btwin.lib`
- `debug/btwin.dll`, `debug/btwin.lib`, `debug/btwin.pdb`
- `btwin.h`

## Project layout

| Path                   | Description                                                          |
| ---------------------- | ------------------------------------------------------------------- |
| `src/btwin.h`          | The C ABI surface — the only file C consumers need.                 |
| `src/btwin.cpp`        | C++/WinRT implementation, built as the `btwin` shared library.       |
| `src/main.c`           | Example C consumer that links `btwin`.                              |
| `CMakeLists.txt`       | Build definition for the library and example executable.            |
| `CMakePresets.json`    | Configure/build presets (`windows-x64` is the supported one).       |
| `vcpkg.json`           | Dependency manifest (`fmt`, `cppwinrt`).                            |

There is no test suite and no linter configured.
