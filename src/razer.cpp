#include "razer.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>
#ifdef AMBIENT_HIDAPI
#include <hidapi.h>
#if defined(Q_OS_MACOS) && !defined(AMBIENT_FAKE_HIDAPI) && __has_include(<hidapi_darwin.h>)
#include <hidapi_darwin.h>
#define AMBIENT_RAZER_DARWIN
#endif
#endif

namespace {
// Protocol facts: OpenRazer driver/razercommon.{h,c}, razerchromacommon.c,
// razerkbd_driver.{h,c}. Original implementation; no daemon or kernel module required.
[[maybe_unused]] bool supported(quint16 product) {
    return product == 0x0203 || product == 0x0209 || product == 0x0216 || product == 0x021a;
}
[[noreturn]] void error(const char *message) {
    throw std::runtime_error(message);
}
#ifdef AMBIENT_HIDAPI
using Packet = std::array<unsigned char, 91>;
unsigned char checksum(const Packet &packet) {
    unsigned char value{};
    for (unsigned i = 3; i < 89; ++i)
        value ^= packet[i];
    return value;
}
class Razer final : public ambient::LightingOutput {
    hid_device *handle_{};
    std::array<unsigned char, 2> mode_{};
    unsigned char brightness_{};
    bool changed_{};
    Packet exchange(unsigned char group, unsigned char command, std::span<const unsigned char> args) {
        Packet packet{};
        packet[2] = 0xff; // Transaction ID; report ID is zero and is not in vendor payload.
        packet[6] = static_cast<unsigned char>(args.size());
        packet[7] = group;
        packet[8] = command;
        std::copy(args.begin(), args.end(), packet.begin() + 9);
        packet[89] = checksum(packet);
        if (hid_send_feature_report(handle_, packet.data(), packet.size()) != static_cast<int>(packet.size()))
            error("Razer feature report write failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Packet reply{};
        if (hid_get_feature_report(handle_, reply.data(), reply.size()) != static_cast<int>(reply.size()))
            error("Razer feature report response has invalid length");
        if (reply[0] != 0 || reply[2] != packet[2] || reply[3] != 0 || reply[4] != 0 || reply[5] != 0 ||
            reply[6] > 80 || reply[7] != group || reply[8] != command || reply[89] != checksum(reply))
            error("Razer feature response is malformed or belongs to another request");
        if (reply[1] != 2)
            error("Razer device rejected command or is busy; close other lighting software");
        return reply;
    }
    Packet lighting(unsigned char command, unsigned char value = 0) {
        const std::array<unsigned char, 3> args{0, 5, value}; // Volatile backlight settings.
        return exchange(3, command, args);
    }

  public:
    explicit Razer(const Device &device) {
        if (device.vendorId != 0x1532 || !supported(device.productId) || device.interfaceNumber != 1 ||
            device.path.isEmpty())
            error("Unsupported Razer profile; only listed wired BlackWidow Chroma interfaces are supported");
        if (hid_init() != 0)
            error("HIDAPI initialization failed");
#ifdef AMBIENT_RAZER_DARWIN
        hid_darwin_set_open_exclusive(0);
#endif
        handle_ = hid_open_path(device.path.constData());
        if (!handle_)
            error("Cannot open Razer HID interface; check permissions and close other lighting software");
        try {
            const std::array<unsigned char, 2> query{0, 0};
            const auto mode = exchange(0, 0x84, query);
            if (mode[6] != 2 || (mode[9] != 0 && mode[9] != 3) || mode[10] != 0)
                error("Unsupported Razer device mode snapshot");
            mode_ = {mode[9], mode[10]};
            const auto brightness = lighting(0x83);
            if (brightness[6] != 3 || brightness[9] != 0 || brightness[10] != 5)
                error("Invalid Razer brightness snapshot");
            brightness_ = brightness[11];
        } catch (...) {
            hid_close(handle_);
            handle_ = nullptr;
            throw;
        }
    }
    void write(std::span<const ambient::Rgb> zones, float brightness) override {
        if (zones.empty())
            return;
        ambient::Rgb color{};
        for (const auto &zone : zones) {
            color.r += zone.r;
            color.g += zone.g;
            color.b += zone.b;
        }
        if (!std::isfinite(brightness) || !std::isfinite(color.r) || !std::isfinite(color.g) ||
            !std::isfinite(color.b))
            error("Razer color and brightness must be finite");
        auto channel = [&](float value) {
            return static_cast<unsigned char>(std::lround(
                std::clamp(value / static_cast<float>(zones.size()) * std::clamp(brightness, 0.f, 1.f), 0.f,
                           1.f) *
                255));
        };
        if (!changed_) {
            changed_ = true; // Device may accept a write whose acknowledgement is lost.
            const std::array<unsigned char, 2> driverMode{3, 0};
            exchange(0, 4, driverMode);
            lighting(3, 255);
        }
        const std::array<unsigned char, 4> args{6, channel(color.r), channel(color.g), channel(color.b)};
        exchange(3, 0x0a, args);
    }
    void restore() override {
        if (!changed_)
            return;
        // Old matrix animation cannot be queried by this protocol. Restore only mode/brightness.
        bool failed = false;
        try {
            lighting(3, brightness_);
        } catch (...) {
            failed = true;
        }
        try {
            exchange(0, 4, mode_);
        } catch (...) {
            failed = true;
        }
        if (failed)
            error("Could not restore Razer brightness/device mode");
        changed_ = false;
    }
    ~Razer() override {
        if (handle_) {
            try {
                restore();
            } catch (...) {
            }
            hid_close(handle_);
        }
    }
};
#endif
} // namespace

QList<Device> razerDevices() {
    QList<Device> devices;
#ifdef AMBIENT_HIDAPI
    if (hid_init() != 0)
        error("HIDAPI initialization failed");
    auto *list = hid_enumerate(0x1532, 0);
    for (const auto *info = list; info; info = info->next) {
        if (info->vendor_id != 0x1532 || !supported(info->product_id) || info->interface_number != 1 ||
            !info->path)
            continue;
        Device device;
        device.kind = DeviceKind::Razer;
        device.path = info->path;
        device.vendorId = info->vendor_id;
        device.productId = info->product_id;
        device.interfaceNumber = info->interface_number;
        const auto name =
            info->product_string ? QString::fromWCharArray(info->product_string) : "BlackWidow Chroma";
        const auto serial = info->serial_number ? QString::fromWCharArray(info->serial_number) : QString{};
        device.label = name + " — Razer USB";
        device.key = QString("razer:%1:%2").arg(info->product_id).arg(serial);
        device.details = "Razer wired Chroma · whole-keyboard color · hardware unverified · restores "
                         "mode/brightness only, not previous animation";
        devices.push_back(device);
    }
    hid_free_enumeration(list);
#endif
    return devices;
}
std::unique_ptr<ambient::LightingOutput> openRazer(const Device &device) {
#ifdef AMBIENT_HIDAPI
    return std::make_unique<Razer>(device);
#else
    Q_UNUSED(device);
    error("Built without HIDAPI; Razer driver unavailable");
#endif
}
