#include "btwin.h"

#include <chrono>
#include <iostream>
#include <mutex>
#include <condition_variable>
#include <string>
#include <algorithm>
#include <thread>

#define WINDOWS_LEAN_AND_MEAN
#include <windows.h>
// #include <winsock2.h>
#include <ws2bth.h>

#include <fmt/base.h>
#include <fmt/core.h>
#include <fmt/format.h>
#include <fmt/chrono.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Globalization.DateTimeFormatting.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
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

template<typename V>
std::string print_map(Foundation::Collections::IMapView<winrt::hstring, V> map) {
    std::stringstream ss;
    for (auto const &pair: map) {
        auto key = pair.Key();
        auto value = pair.Value();
        ss << winrt::to_string(key);
        ss << ": ";
        if (value != nullptr) {
            Foundation::IStringable stringable = value.try_as<Foundation::IStringable>();
            if (stringable) {
                ss << winrt::to_string(stringable.ToString());
            } else {
                winrt::hstring className = winrt::get_class_name(value);
                ss << winrt::to_string(className);
            }
        } else {
            ss << "null";
        }
        ss << ";" << std::endl;
    }
    return ss.str();
}

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


template<typename... T>
void log(fmt::v12::format_string<T...> fmt, T &&... args) {
    auto now = winrt::clock::now();
    fmt::print("[{}] ", now);
    fmt::println(fmt, std::forward<T>(args)...);
}

struct BtWatcher {

    bt_device_callback_t on_device;
    on_start_t on_start;
    on_end_t on_end;
    void* user_data;

    Devices::Enumeration::DeviceWatcher watcher;

    std::mutex lock{};
    std::condition_variable cond{};

    explicit BtWatcher(const btwin_params_t *params, void *user_data): user_data(user_data), watcher(nullptr) {

        on_device = params->callback;
        on_start = params->on_start;
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
            auto id = winrt::to_string(info.Id());

            // log("Added: {} {} {}", id, device_mac.value_or("nomac"), device_name);

            if (!device_mac.has_value()) {
                return;
            }

            const auto &mac = device_mac.value();

            bt_device_t device = {};
            mac.copy(device.mac, sizeof(device.mac));
            device_name.copy(device.name, sizeof(device.name));
            device.name_len = device_name.length();
            device.standard = standard;

            if (this->on_device) {
                this->on_device(device, this->user_data);
            }
        });


        w.EnumerationCompleted([this](const auto &watcher, const auto &info) {
            if (this->on_end) {
                this->on_end(this->user_data);
            }
            auto a = std::lock_guard(lock);
            cond.notify_all();
        });

        w.Stopped([&](const auto &watcher, const auto &info) {
            // log("Stopped");
        });

        w.Updated([](const auto &watcher, const auto &info) {
            auto properties = info.Properties();
            auto ma = print_map(properties);
            // log("Updated: {} props {}", winrt::to_string(info.Id()), ma);
        });

        w.Removed([&](const auto &watcher, const auto &info) {
            // log("Removed: {}", winrt::to_string(info.Id()));
        });

        watcher = std::move(w);
    }

    int start() const {
        watcher.Start();
        return 1;
    }

    int stop() {
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
            log("adapter query error: {}", winrt::to_string(e.message()));
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


int runWatcher() {
    std::mutex lock{};
    std::condition_variable cond{};

    auto bt_filter = winrt::to_hstring("System.Devices.Aep.ProtocolId:=\"{e0cbf06c-cd8b-4647-bb8a-263b43f0f974}\"");

    auto watcher = Devices::Enumeration::DeviceInformation::CreateWatcher(
        bt_filter,
        {
            L"System.Devices.Aep.ProtocolId",
            L"System.Devices.Aep.DeviceAddress",
            L"System.ItemNameDisplay",
        },
        Devices::Enumeration::DeviceInformationKind::AssociationEndpoint
    );

    // auto watcher = Devices::Enumeration::DeviceInformation::CreateWatcher();

    watcher.Added([&](const auto &watcher, const auto &info) {
        auto props = print_map(info.Properties());
        auto mac = getBtDeviceMacAddress(info.Properties());
        // log("Added: {} {} {}", winrt::to_string(info.Name()), mac.value_or("nomac"), props);
    });

    watcher.Stopped([&](const auto &watcher, const auto &info) {
        // log("Stopped");
    });

    watcher.Updated([](const auto &watcher, const auto &info) {
        auto properties = info.Properties();
        auto ma = print_map(properties);
        // log("Updated: {} props {}", winrt::to_string(info.Id()), ma);
    });

    watcher.Removed([&](const auto &watcher, const auto &info) {
        log("Removed: {}", winrt::to_string(info.Id()));
    });

    watcher.EnumerationCompleted([&](const auto &watcher, const auto &info) {
        log("EnumerationCompleted");
        {
            std::lock_guard l(lock);
            cond.notify_all();
        }
    });

    watcher.Start();
    log("Started");

    {
        std::unique_lock l(lock);
        cond.wait(l);
    }

    watcher.Stop();
    log("Stopped");

    // auto col = Devices::Enumeration::DeviceInformation::FindAllAsync(bt_filter).get();
    // for (auto re : col) {
    //     fmt::print("{}\n", winrt::to_string(re.Name()));
    // }
    return 0;
}

int runBtTest() {
    auto adapter = Devices::Bluetooth::BluetoothAdapter::GetDefaultAsync().get();

    auto radio = adapter.GetRadioAsync().get();
    auto state = radio.State();
    log("radio state {}", static_cast<int>(state));

    if (state != Devices::Radios::RadioState::On) {
        return 1;
    }

    const auto id = L"Bluetooth#Bluetooth00:1a:7d:da:71:13-88:6b:0f:ad:b9:b5";
    log("get async");
    try {
        // auto device = Devices::Bluetooth::BluetoothDevice::FromIdAsync(id).get();
        auto device = Devices::Bluetooth::BluetoothDevice::FromBluetoothAddressAsync(0x886b0fadb9b5).get();

        auto hostname = device.HostName();
        log("hostname {}", winrt::to_string(hostname.ToString()));

        auto address = device.BluetoothAddress();
        log("address {:x}", address);

        auto aqs = Devices::Bluetooth::Rfcomm::RfcommDeviceService::GetDeviceSelectorForBluetoothDevice(device);
        auto rfcomm = Devices::Enumeration::DeviceInformation::FindAllAsync(aqs).get();

        for (auto device_information : rfcomm) {
            log("rfcomm id {}", winrt::to_string(device_information.Id()));
        }

    } catch (const winrt::hresult_error &e) {
        log("error: {}", winrt::to_string(e.message()));
    }
    log("after");
    return 0;
}
