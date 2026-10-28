#include "btwin.h"

#include <chrono>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

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

// System.Devices.Aep.IsPresent is a boolean that flips false->true when Windows
// actually detects the device on-air (as opposed to it merely being a cached
// association endpoint). Returns nullopt when the key is absent so callers can
// distinguish "known not present" from "unknown". unbox_value_or keeps a malformed
// or null box from throwing inside a watcher callback.
std::optional<bool> getBtDeviceIsPresent(
    const Foundation::Collections::IMapView<winrt::hstring, Foundation::IInspectable> &properties) {
    if (properties.HasKey(L"System.Devices.Aep.IsPresent")) {
        if (const auto value = properties.Lookup(L"System.Devices.Aep.IsPresent")) {
            return std::optional(winrt::unbox_value_or<bool>(value, false));
        }
    }
    return std::optional<bool>();
}

// System.ItemNameDisplay can resolve later than the initial Added event, arriving
// in an Updated payload. Returns nullopt when the key is absent.
std::optional<std::string> getBtDeviceName(
    const Foundation::Collections::IMapView<winrt::hstring, Foundation::IInspectable> &properties) {
    if (properties.HasKey(L"System.ItemNameDisplay")) {
        if (const auto value = properties.Lookup(L"System.ItemNameDisplay")) {
            return std::optional(winrt::to_string(winrt::unbox_value_or<winrt::hstring>(value, L"")));
        }
    }
    return std::optional<std::string>();
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

    // Per-endpoint state, keyed by DeviceInformation.Id(). An Updated event carries
    // only the Id plus the changed property subset, so name/mac/standard captured at
    // Added time must be stashed here to be correlated with later presence changes.
    struct TrackedDevice {
        std::string   mac{};
        std::string   name{};
        bt_standard_t standard{BT_UNKNOWN};
        bool          is_present{false}; // last known System.Devices.Aep.IsPresent
        // is_present value last handed to on_device; nullopt until the first report.
        std::optional<bool> reported_present{};

        // Report when the presence differs from what the caller last saw (or it has not
        // seen this device yet). A device without a MAC is held until one arrives.
        bool needs_report() const {
            return !mac.empty() && reported_present != is_present;
        }
    };
    std::unordered_map<std::string, TrackedDevice> devices{};

    // Build a bt_device_t from a tracked entry and hand it to the user callback.
    // MUST be called WITHOUT holding `lock` — the callback runs arbitrary user code.
    void report_device(const TrackedDevice &d) const {
        bt_device_t device = {};
        // copy at most sizeof-1 so the zero-initialized final byte stays a NUL terminator
        d.mac.copy(device.mac, sizeof(device.mac) - 1);
        device.name_len = static_cast<uint32_t>(d.name.copy(device.name, sizeof(device.name)));
        device.standard = d.standard;
        device.is_present = d.is_present ? 1 : 0;
        bt_log(BT_LOG_INFO, "reporting device: {} {} (standard {}, present {})",
               d.mac, d.name, static_cast<int>(d.standard), d.is_present);
        if (on_device) {
            on_device(&device, user_data);
        }
    }

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
                L"System.Devices.Aep.IsPresent",
            },
            Devices::Enumeration::DeviceInformationKind::AssociationEndpoint
        );

        // Added reflects Windows' cached association-endpoint list: a device usually
        // shows up here with IsPresent=false and only becomes present later (Updated).
        // Report it right away with its current presence in bt_device_t.is_present;
        // whether a not-present device is wanted is the caller's decision.
        w.Added([this](const auto &watcher, const auto &info) {
            auto id = winrt::to_string(info.Id());
            auto device_mac = getBtDeviceMacAddress(info.Properties());
            auto standard = getBtDeviceStandard(info.Properties());
            auto device_name = winrt::to_string(info.Name());
            auto present = getBtDeviceIsPresent(info.Properties());

            bt_log(BT_LOG_INFO, "device added: {} {} (standard {}, present {})",
                   device_mac.value_or("?"), device_name, static_cast<int>(standard),
                   present.has_value() ? (present.value() ? "true" : "false") : "unknown");

            if (!device_mac.has_value()) {
                bt_log(BT_LOG_WARN, "device '{}': no MAC address yet", device_name);
            }

            std::optional<TrackedDevice> to_report;
            {
                auto g = std::lock_guard(lock);
                auto &entry = devices[id]; // value-inits reported_present=nullopt on first insert
                entry.mac = device_mac.value_or("");
                entry.name = device_name;
                entry.standard = standard;
                entry.is_present = present.value_or(false);

                if (entry.needs_report()) {
                    entry.reported_present = entry.is_present;
                    to_report = entry;
                }
            }
            if (to_report) {
                report_device(*to_report);
            }
        });

        // Updated carries only the Id plus the changed property subset. Correlate it
        // with the Added record and report again whenever the presence changes (most
        // importantly the false->true transition: the device is on-air now).
        // Note: never call getBtDeviceStandard here — ProtocolId is absent from an
        // Updated payload and the unguarded unbox would throw.
        w.Updated([this](const auto &watcher, const Devices::Enumeration::DeviceInformationUpdate &update) {
            auto id = winrt::to_string(update.Id());
            auto props = update.Properties();
            auto present = getBtDeviceIsPresent(props);
            auto name = getBtDeviceName(props);
            auto mac = getBtDeviceMacAddress(props);

            std::optional<TrackedDevice> to_report;
            {
                auto g = std::lock_guard(lock);
                auto it = devices.find(id);
                if (it == devices.end()) {
                    // Updated before its Added — nothing to correlate against.
                    return;
                }
                auto &entry = it->second;

                if (present.has_value()) {
                    entry.is_present = present.value();
                }
                if (name.has_value()) {
                    entry.name = name.value();
                }
                if (mac.has_value() && entry.mac.empty()) {
                    entry.mac = mac.value();
                }

                if (entry.needs_report()) {
                    entry.reported_present = entry.is_present;
                    to_report = entry;
                }
            }
            if (to_report) {
                report_device(*to_report);
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

    // Start() is only legal from Created/Stopped/Aborted; calling it while the watcher
    // is Started/EnumerationCompleted/Stopping throws. Guard on Status() so a repeat
    // start is a no-op, and swallow any WinRT error so it never crosses the C ABI.
    int start() const {
        using Status = Devices::Enumeration::DeviceWatcherStatus;
        const auto status = watcher.Status();
        if (status == Status::Started || status == Status::EnumerationCompleted || status == Status::Stopping) {
            bt_log(BT_LOG_DEBUG, "watcher already running (status {}); start is a no-op", static_cast<int>(status));
            return 1;
        }
        bt_log(BT_LOG_DEBUG, "watcher starting");
        try {
            watcher.Start();
        } catch (const winrt::hresult_error &e) {
            bt_log(BT_LOG_ERROR, "watcher start failed: {}", winrt::to_string(e.message()));
            return 0;
        }
        return 1;
    }

    // Stop() is only legal from Started/EnumerationCompleted; in any other state it is
    // a no-op. Swallow any WinRT error so it never crosses the C ABI.
    int stop() {
        using Status = Devices::Enumeration::DeviceWatcherStatus;
        const auto status = watcher.Status();
        if (status != Status::Started && status != Status::EnumerationCompleted) {
            bt_log(BT_LOG_DEBUG, "watcher not running (status {}); stop is a no-op", static_cast<int>(status));
            return 1;
        }
        bt_log(BT_LOG_DEBUG, "watcher stopping");
        try {
            watcher.Stop();
        } catch (const winrt::hresult_error &e) {
            bt_log(BT_LOG_ERROR, "watcher stop failed: {}", winrt::to_string(e.message()));
            return 0;
        }
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
