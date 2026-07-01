#include <stdio.h>

#include "btwin.h"

static const char* bt_standard_strings[] = {
    "BT_UNKNOWN",
    "BT_CLASSIC",
    "BT_LE",
};

void my_bt_device_callback(bt_device_t device, void *user_data) {
    const int len = (int)device.name_len;
    if (device.standard == BT_CLASSIC) {
        printf("found: %s %s %.*s\n", bt_standard_strings[device.standard], device.mac, len, device.name);
    }
}

void my_bt_end(void *user_data) {
    printf("my_bt_end\n");
}

int main(int argc, char **argv) {
    printf("hello\n");

    printf("adapter exists: %d\n", btwin_adapter_exists());
    printf("adapter is on:  %d\n", btwin_adapter_is_on());

    const btwin_params_t params = {
        .callback = my_bt_device_callback,
        .on_end = my_bt_end,
    };

    btwin_t watcher = btwin_alloc(&params, NULL);

    printf("start\n");
    btwin_start(watcher);

    printf("join\n");
    btwin_join(watcher);

    printf("free\n");
    btwin_free(watcher);
    return 0;
}
