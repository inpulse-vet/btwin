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

#define BT_NAME_MAX 248   // Bluetooth GAP maximum device-name length, in octets

typedef struct {
    uint32_t name_len;
    char name[BT_NAME_MAX];
    char mac[18];
    bt_standard_t standard;
} bt_device_t;

typedef void (*bt_device_callback_t)(const bt_device_t *device, void *user_data);

typedef void (*on_end_t)(void *user_data);

typedef struct {
    bt_device_callback_t callback;
    on_end_t on_end;
} btwin_params_t;

typedef void *btwin_t;

typedef enum {
    BT_LOG_OFF = 0,   // nothing is emitted, not even the stderr fallback
    BT_LOG_ERROR,     // 1
    BT_LOG_WARN,      // 2
    BT_LOG_INFO,      // 3
} bt_log_level_t;

// level = severity of this message; message is only valid for the duration of the
// call (copy it if you need to retain it). May be invoked from multiple threads.
typedef void (*bt_log_callback_t)(bt_log_level_t level, const char *message, void *user_data);

// Register the process-wide log sink. Pass callback = NULL to unregister and fall
// back to stderr for errors. Intended to be called once at init, before starting a
// watcher; safe to call at any time (guarded internally).
BTWIN_EXPORT void btwin_set_log_callback(bt_log_callback_t callback, void *user_data);

// Messages with severity above this level are dropped. Default: BT_LOG_INFO.
BTWIN_EXPORT void btwin_set_log_level(bt_log_level_t level);

BTWIN_EXPORT btwin_t btwin_alloc(const btwin_params_t *params, void* user_data);

BTWIN_EXPORT void btwin_free(btwin_t watcher);

BTWIN_EXPORT int btwin_start(btwin_t watcher);

BTWIN_EXPORT int btwin_stop(btwin_t watcher);

BTWIN_EXPORT void btwin_join(btwin_t watcher);

// 1 if a default Bluetooth adapter is present, 0 otherwise.
BTWIN_EXPORT int btwin_adapter_exists(void);

// 1 = adapter present and radio On; 0 = present but Off; -1 = no adapter / unknown / error.
BTWIN_EXPORT int btwin_adapter_is_on(void);

#ifdef __cplusplus
}
#endif
