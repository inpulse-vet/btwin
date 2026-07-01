#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BTWIN_EXPORT __declspec(dllexport)

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
} btwin_params_t;

typedef void *btwin_t;

BTWIN_EXPORT btwin_t btwin_alloc(const btwin_params_t *params, void* user_data);

BTWIN_EXPORT void btwin_free(btwin_t watcher);

BTWIN_EXPORT int btwin_start(btwin_t watcher);

BTWIN_EXPORT int btwin_stop(btwin_t watcher);

BTWIN_EXPORT void btwin_join(btwin_t watcher);

BTWIN_EXPORT int runWatcher();

BTWIN_EXPORT int runBtTest();

#ifdef __cplusplus
}
#endif
