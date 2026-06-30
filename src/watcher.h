#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WATCHER_EXPORT __declspec(dllexport)

typedef enum {
    BT_UNKNOWN,
    BT_CLASSIC,
    BT_LE,
} bt_standard_t;

typedef struct {
    uint32_t name_len;
    char name[128];
    char mac[18];
    bt_standard_t standard;
} bt_device_t;

typedef void (*bt_device_callback_t)(bt_device_t device, void *user_data);

typedef void (*on_start_t)(bt_device_t device, void *user_data);

typedef void (*on_end_t)(void *user_data);

typedef struct {
    bt_device_callback_t callback;
    on_start_t on_start;
    on_end_t on_end;
} watcher_params_t;

typedef void *watcher_t;

WATCHER_EXPORT watcher_t watcher_alloc(const watcher_params_t *params, void* user_data);

WATCHER_EXPORT void watcher_free(watcher_t watcher);

WATCHER_EXPORT int watcher_start(watcher_t watcher);

WATCHER_EXPORT int watcher_stop(watcher_t watcher);

WATCHER_EXPORT void watcher_join(watcher_t watcher);

WATCHER_EXPORT int runWatcher();

WATCHER_EXPORT int runBtTest();

#ifdef __cplusplus
}
#endif
