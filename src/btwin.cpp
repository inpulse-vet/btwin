#include "btwin.h"

#include <chrono>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <string>
#include <thread>

#define WINDOWS_LEAN_AND_MEAN
#include <windows.h>

#include <fmt/base.h>
#include <fmt/core.h>
#include <fmt/format.h>
#include <fmt/chrono.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Radios.h>

using namespace winrt::Windows;

template<>
struct fmt::formatter<Foundation::DateTime> {
    constexpr auto parse(format_parse_context &ctx) {
        return ctx.begin(); // Return the end of the parsed range
    }

    template<typename FormatContext>
    auto format(const Foundation::DateTime &dt, FormatContext &ctx) const {
        std::chrono::system_clock::time_point sys_now{dt.time_since_epoch()};
        return fmt::format_to(ctx.out(), "{:%FT%TZ}", sys_now);
    }
};

std::optional<std::string> getBtDeviceMacAddress(
    const Foundation::Collections::IMapView<winrt::hstring, Foundation::IInspectable> &properties) {
    if (properties.HasKey(L"System.Devices.Aep.DeviceAddress")) {
        if (const auto mac = properties.Lookup(L"System.Devices.Aep.DeviceAddress")) {
            const auto addr = winrt::unbox_value<winrt::hstring>(mac);
            return std::optional(winrt::to_string(addr));
        }
    }
    return std::optional<std::string>();
}

bt_standard_t getBtDeviceStandard(
    const Foundation::Collections::IMapView<winrt::hstring, Foundation::IInspectable> &properties
) {
    static auto bt_classic_guid = winrt::guid(L"{e0cbf06c-cd8b-4647-bb8a-263b43f0f974}");
    static auto bt_le_guid = winrt::guid(L"{bb7bb05e-5972-42b5-94fc-76eaa7084d49}");

    auto id = properties.Lookup(L"System.Devices.Aep.ProtocolId");
    auto guid = winrt::unbox_value<winrt::guid>(id);
    bt_standard_t standard = BT_UNKNOWN;
    if (guid == bt_classic_guid) {
        standard = BT_CLASSIC;
    } else if (guid == bt_le_guid) {
        standard = BT_LE;
    }
    return standard;
}


static std::mutex        g_log_lock;
static bt_log_callback_t g_log_cb    = nullptr;
static void*             g_log_user  = nullptr;
static bt_log_level_t    g_log_level = BT_LOG_INFO;

template<typename... T>
static void bt_log(bt_log_level_t level, fmt::format_string<T...> f, T &&... args) {
    bt_log_callback_t cb;
    void*             ud;
    bt_log_level_t    threshold;
    {
        std::lock_guard g(g_log_lock);
        cb = g_log_cb;
        ud = g_log_user;
        threshold = g_log_level;
    }

    if (threshold == BT_LOG_OFF || level > threshold) {
        return;
    }

    auto msg = fmt::format(f, std::forward<T>(args)...);
    if (cb) {
        cb(level, msg.c_str(), ud);
    } else if (level == BT_LOG_ERROR) {
        // stderr fallback: critical errors must not vanish when no sink is set
        fmt::print(stderr, "[{}] btwin ERROR: {}\n", winrt::clock::now(), msg);
    }
}

void btwin_set_log_callback(bt_log_callback_t cb, void* ud) {
    std::lock_guard g(g_log_lock);
    g_log_cb = cb;
    g_log_user = ud;
}

void btwin_set_log_level(bt_log_level_t level) {
    std::lock_guard g(g_log_lock);
    g_log_level = level;
}

struct BtWatcher {

    bt_device_callback_t on_device;
    on_end_t on_end;
    void* user_data;

    Devices::Enumeration::DeviceWatcher watcher;

    std::mutex lock{};
    std::condition_variable cond{};

    explicit BtWatcher(const btwin_params_t *params, void *user_data): user_data(user_data), watcher(nullptr) {

        on_device = params->callback;
        on_end = params->on_end;

        auto bt_filter = winrt::to_hstring(
            R"((System.Devices.Aep.ProtocolId:="{e0cbf06c-cd8b-4647-bb8a-263b43f0f974}" OR System.Devices.Aep.ProtocolId:="{bb7bb05e-5972-42b5-94fc-76eaa7084d49}"))"
            );

        auto w = Devices::Enumeration::DeviceInformation::CreateWatcher(
            bt_filter,
            {
                L"System.Devices.Aep.ProtocolId",
                L"System.Devices.Aep.DeviceAddress",
                L"System.ItemNameDisplay",
            },
            Devices::Enumeration::DeviceInformationKind::AssociationEndpoint
        );

        w.Added([this](const auto &watcher, const auto &info) {
            auto device_mac = getBtDeviceMacAddress(info.Properties());
            auto standard = getBtDeviceStandard(info.Properties());
            auto device_name = winrt::to_string(info.Name());

            if (!device_mac.has_value()) {
                bt_log(BT_LOG_WARN, "skipping device '{}': no MAC address", device_name);
                return;
            }

            const auto &mac = device_mac.value();

            bt_device_t device = {};
            mac.copy(device.mac, sizeof(device.mac));
            device.name_len = static_cast<uint32_t>(device_name.copy(device.name, sizeof(device.name)));
            device.standard = standard;

            bt_log(BT_LOG_INFO, "device added: {} {} (standard {})", mac, device_name, static_cast<int>(standard));

            if (this->on_device) {
                this->on_device(&device, this->user_data);
            }
        });


        w.EnumerationCompleted([this](const auto &watcher, const auto &info) {
            bt_log(BT_LOG_INFO, "enumeration completed");
            if (this->on_end) {
                this->on_end(this->user_data);
            }
            auto a = std::lock_guard(lock);
            cond.notify_all();
        });

        watcher = std::move(w);
    }

    int start() const {
        bt_log(BT_LOG_INFO, "watcher starting");
        watcher.Start();
        return 1;
    }

    int stop() {
        bt_log(BT_LOG_INFO, "watcher stopping");
        watcher.Stop();
        return 1;
    }

};

btwin_t btwin_alloc(const btwin_params_t* params, void* user_data) {
    return new BtWatcher(params, user_data);
}

void btwin_free(btwin_t watcher) {
    const auto watcher_ptr = static_cast<BtWatcher *>(watcher);
    delete watcher_ptr;
}

int btwin_start(btwin_t watcher) {
    const auto w = static_cast<BtWatcher*>(watcher);
    return w->start();
}

int btwin_stop(btwin_t watcher) {
    const auto w = static_cast<BtWatcher*>(watcher);
    return w->stop();
}

void btwin_join(btwin_t watcher) {
    const auto w = static_cast<BtWatcher*>(watcher);
    auto l = std::unique_lock(w->lock);
    w->cond.wait(l);
}

// Run a WinRT query on a dedicated worker thread that owns its own MTA, so the
// result is correct regardless of the caller's apartment and the caller's thread
// is never mutated. A fresh thread has no apartment, so init_apartment cannot throw
// RPC_E_CHANGED_MODE; it is kept outside the try so uninit_apartment only runs after
// a successful init. join() is an ordinary thread join (not a WinRT .get()), so it is
// legal even from an STA caller and cannot deadlock.
template <typename F>
static int run_on_mta_thread(F &&query, int on_error) {
    int result = on_error;
    std::thread t([&]() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            result = query();
        } catch (const winrt::hresult_error &e) {
            bt_log(BT_LOG_ERROR, "adapter query error: {}", winrt::to_string(e.message()));
            result = on_error;
        }
        winrt::uninit_apartment();
    });
    t.join();
    return result;
}

int btwin_adapter_exists() {
    return run_on_mta_thread([] {
        auto adapter = Devices::Bluetooth::BluetoothAdapter::GetDefaultAsync().get();
        return adapter != nullptr ? 1 : 0;
    }, 0);
}

int btwin_adapter_is_on() {
    return run_on_mta_thread([] {
        auto adapter = Devices::Bluetooth::BluetoothAdapter::GetDefaultAsync().get();
        if (adapter == nullptr) {
            return -1;
        }
        auto radio = adapter.GetRadioAsync().get();
        if (radio == nullptr) {
            return -1;
        }
        return radio.State() == Devices::Radios::RadioState::On ? 1 : 0;
    }, -1);
}
