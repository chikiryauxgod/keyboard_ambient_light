#include "corsair.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#ifdef AMBIENT_HIDAPI
#include <hidapi.h>
#if defined(Q_OS_MACOS) && !defined(AMBIENT_FAKE_HIDAPI) && __has_include(<hidapi_darwin.h>)
#include <hidapi_darwin.h>
#define AMBIENT_CORSAIR_DARWIN
#endif
#endif

namespace {
// Protocol reference:
// https://github.com/CalcProgrammer1/OpenRGB/tree/master/Controllers/CorsairPeripheralController Legacy USB
// protocol facts documented by OpenRGB's CorsairPeripheralController. This is deliberately separate from
// Corsair's newer peripheral V2 protocol.
struct Profile {
    unsigned short product;
    const char *name;
    unsigned leds;
};
constexpr std::array profiles{
    Profile{0x1b17, "Corsair K65 RGB", 144},           Profile{0x1b37, "Corsair K65 LUX RGB", 144},
    Profile{0x1b39, "Corsair K65 RGB RAPIDFIRE", 144}, Profile{0x1b13, "Corsair K70 RGB", 144},
    Profile{0x1b11, "Corsair K95 RGB", 168},
};
[[maybe_unused]] const Profile *profile(unsigned short vendor, unsigned short product) {
    if (vendor != 0x1b1c)
        return nullptr;
    const auto it = std::find_if(profiles.begin(), profiles.end(),
                                 [product](const auto &p) { return p.product == product; });
    return it == profiles.end() ? nullptr : &*it;
}
[[noreturn]] void corsairError(const char *message) {
    throw std::runtime_error(message);
}
#ifdef AMBIENT_HIDAPI
using Report = std::array<unsigned char, 65>;
class CorsairOutput final : public ambient::LightingOutput {
    hid_device *handle_{};
    unsigned leds_{};
    bool controlled_{};
    void send(const Report &packet) {
        if (hid_write(handle_, packet.data(), packet.size()) < static_cast<int>(packet.size()))
            corsairError("Corsair HID write failed; check connection and close other lighting software");
    }
    void identify() {
        Report query{};
        query[1] = 0x0e;
        query[2] = 1;
        send(query);
        Report reply{};
        auto received = hid_read_timeout(handle_, reply.data(), reply.size(), 500);
        unsigned offset = 0;
        if (received == 0) {
            if (hid_send_feature_report(handle_, query.data(), query.size()) !=
                static_cast<int>(query.size()))
                corsairError("Corsair firmware query failed");
            received = hid_get_feature_report(handle_, reply.data(), reply.size());
            offset = 1;
        }
        // Firmware-info responses identify a keyboard with device type 0xc0.
        if (received < static_cast<int>(0x15 + offset) || reply[offset] != 0x0e || reply[1 + offset] != 1 ||
            reply[0x14 + offset] != 0xc0)
            corsairError("Invalid Corsair keyboard firmware reply or timeout");
    }

  public:
    explicit CorsairOutput(const Device &device) {
        const auto *p = profile(device.vendorId, device.productId);
        if (!p || device.interfaceNumber != 1 || device.path.isEmpty())
            corsairError("Unsupported Corsair model or USB control interface");
        leds_ = p->leds;
        if (hid_init() != 0)
            corsairError("HIDAPI initialization failed");
#ifdef AMBIENT_CORSAIR_DARWIN
        hid_darwin_set_open_exclusive(0);
#endif
        handle_ = hid_open_path(device.path.constData());
        if (!handle_)
            corsairError("Cannot open Corsair HID device; check USB access permissions");
        try {
            identify();
            Report control{};
            control[1] = 7;
            control[2] = 5;
            control[3] = 2;
            control[5] = 3;
            controlled_ = true;
            send(control);
        } catch (...) {
            try {
                restore();
            } catch (...) {
            }
            hid_close(handle_);
            handle_ = nullptr;
            throw;
        }
    }
    void write(std::span<const ambient::Rgb> zones, float brightness) override {
        if (zones.empty())
            return;
        ambient::Rgb average{};
        for (const auto color : zones) {
            average.r += color.r;
            average.g += color.g;
            average.b += color.b;
        }
        if (!std::isfinite(brightness) || !std::isfinite(average.r) || !std::isfinite(average.g) ||
            !std::isfinite(average.b))
            corsairError("Corsair color and brightness must be finite");
        const auto scale = std::clamp(brightness, 0.f, 1.f) / static_cast<float>(zones.size());
        const std::array components{average.r, average.g, average.b};
        // Every physical LED receives the same color, so no layout mapping is required.
        for (unsigned channel = 0; channel < components.size(); ++channel) {
            const auto value = static_cast<unsigned char>(
                std::lround(std::clamp(components[channel] * scale, 0.f, 1.f) * 255.f));
            for (unsigned part = 0; part < 3; ++part) {
                Report packet{};
                packet[1] = 0x7f;
                packet[2] = static_cast<unsigned char>(part + 1);
                packet[3] = static_cast<unsigned char>(std::min(60u, leds_ - part * 60));
                std::fill_n(packet.begin() + 5, packet[3], value);
                send(packet);
            }
            Report submit{};
            submit[1] = 7;
            submit[2] = 0x28;
            submit[3] = static_cast<unsigned char>(channel + 1);
            submit[4] = 3;
            submit[5] = static_cast<unsigned char>(channel == 2 ? 2 : 1);
            send(submit);
        }
    }
    void restore() override {
        if (!controlled_)
            return;
        Report hardware{};
        hardware[1] = 7;
        hardware[2] = 4;
        hardware[3] = 1;
        send(hardware);
        controlled_ = false;
    }
    ~CorsairOutput() override {
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

QList<Device> corsairDevices() {
    QList<Device> result;
#ifdef AMBIENT_HIDAPI
    if (hid_init() != 0)
        corsairError("HIDAPI initialization failed");
    auto *list = hid_enumerate(0x1b1c, 0);
    for (auto *entry = list; entry; entry = entry->next) {
        const auto *p = profile(entry->vendor_id, entry->product_id);
        if (!p || entry->interface_number != 1 || entry->usage_page != 0xffc2 || !entry->path)
            continue;
        Device device;
        device.kind = DeviceKind::Corsair;
        device.path = entry->path;
        device.label = QString::fromLatin1(p->name) + " — Corsair USB";
        device.vendorId = entry->vendor_id;
        device.productId = entry->product_id;
        device.interfaceNumber = entry->interface_number;
        const auto serial = entry->serial_number ? QString::fromWCharArray(entry->serial_number) : QString{};
        device.key = QString("corsair:%1:%2:%3").arg(device.vendorId).arg(device.productId).arg(serial);
        device.details = "Corsair legacy USB · whole-keyboard color · hardware unverified · "
                         "Stop resumes hardware lighting; previous software colors cannot be restored";
        result.push_back(device);
    }
    hid_free_enumeration(list);
#endif
    return result;
}
std::unique_ptr<ambient::LightingOutput> openCorsair(const Device &device) {
#ifdef AMBIENT_HIDAPI
    return std::make_unique<CorsairOutput>(device);
#else
    Q_UNUSED(device);
    corsairError("Built without HIDAPI; install HIDAPI and rebuild");
#endif
}
