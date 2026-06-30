#include <stdio.h>
#include <threads.h>

#include "watcher.h"

static const char* bt_standard_strings[] = {
    "BT_UNKNOWN",
    "BT_CLASSIC",
    "BT_LE",
};

void my_bt_device_callback(bt_device_t device) {
    const int len = (int)device.name_len;
    if (device.standard == BT_CLASSIC) {
        printf("found: %s %s %.*s\n", bt_standard_strings[device.standard], device.mac, len, device.name);
    }
}

void my_bt_end() {
    printf("my_bt_end\n");
}

int main(int argc, char **argv) {
    // runBtTest();
    // return 0;
    printf("hello\n");
    // runWatcher();

    const watcher_params_t params = {
        .callback = my_bt_device_callback,
        .on_end = my_bt_end,
    };

    watcher_t watcher = watcher_alloc(&params);

    printf("start\n");
    watcher_start(watcher);

    printf("join\n");
    watcher_join(watcher);

    // struct timespec duration = { .tv_sec = 1, .tv_nsec = 0 };
    // thrd_sleep(&duration, NULL);

    // printf("sleep\n");
    // struct timespec duratio2 = { .tv_sec = 120, .tv_nsec = 0 };
    // thrd_sleep(&duratio2, NULL);

    printf("free\n");
    watcher_free(watcher);
    return 0;
}
