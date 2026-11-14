#include "logitech.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#ifdef AMBIENT_HIDAPI
#include <hidapi.h>
#if defined(Q_OS_MACOS) && !defined(AMBIENT_FAKE_HIDAPI) && __has_include(<hidapi_darwin.h>)
#include <hidapi_darwin.h>
#define AMBIENT_LOGITECH_DARWIN
#endif
#endif

namespace {
struct Profile {
    unsigned short product;
    const char *name;
    unsigned char feature;
};
// Protocol facts: MatMoul/g810-led Keyboard.h model IDs and Keyboard.cpp native effects.
// G413/G610 are single-color models; receivers and undocumented newer protocols are excluded.
// G815 is excluded: the reference forces storage=1, unsuitable for continuous ambient updates.
constexpr std::array profiles{
    Profile{0xc336, "G213", 0x0c}, Profile{0xc330, "G410", 0x0d}, Profile{0xc342, "G512", 0x0d},
    Profile{0xc33c, "G513", 0x0d}, Profile{0xc331, "G810", 0x0d}, Profile{0xc337, "G810", 0x0d},
    Profile{0xc32b, "G910", 0x10}, Profile{0xc335, "G910", 0x10}, Profile{0xc339, "G Pro", 0x0d}};
const Profile *profile(unsigned short vendor, unsigned short product) {
    if (vendor != 0x046d)
        return nullptr;
    const auto found = std::find_if(profiles.begin(), profiles.end(),
                                    [product](const Profile &p) { return p.product == product; });
    return found == profiles.end() ? nullptr : &*found;
}
[[noreturn]] void fail(const char *message) {
    throw std::runtime_error(message);
}
#ifdef AMBIENT_HIDAPI
class Logitech final : public ambient::LightingOutput {
    hid_device *handle_{};
    Profile profile_;
    void send(const std::array<unsigned char, 20> &packet) {
        // HIDAPI expects the actual numbered report ID (0x11), without an extra zero prefix.
        // Windows may pad to the longest output report and return that larger length.
        if (hid_write(handle_, packet.data(), packet.size()) < static_cast<int>(packet.size()))
            fail("Logitech HID write failed; check the USB connection and close other lighting software");
    }

  public:
    Logitech(const Device &device, const Profile &selected) : profile_(selected) {
        if (hid_init() != 0)
            fail("HIDAPI initialization failed");
#ifdef AMBIENT_LOGITECH_DARWIN
        hid_darwin_set_open_exclusive(0);
#endif
        handle_ = hid_open_path(device.path.constData());
        if (!handle_)
            fail("Cannot open Logitech HID interface; check USB access permissions");
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
        const auto scale = std::clamp(brightness, 0.f, 1.f) / static_cast<float>(zones.size());
        const auto byte = [scale](float value) {
            const auto scaled = value * scale;
            return static_cast<unsigned char>(
                std::lround(std::isfinite(scaled) ? std::clamp(scaled, 0.f, 1.f) * 255.f : 0.f));
        };
        std::array<unsigned char, 20> packet{};
        packet[0] = 0x11;
        packet[1] = 0xff; // Direct USB device, not a receiver subdevice.
        packet[2] = profile_.feature;
        packet[3] = 0x3c;
        packet[4] = 0; // Keys only; preserve logo and indicators.
        packet[5] = 1; // Static color effect.
        packet[6] = byte(color.r);
        packet[7] = byte(color.g);
        packet[8] = byte(color.b);
        packet[14] = 0x64;
        packet[16] = 0; // Do not store the effect in persistent user lighting memory.
        send(packet);
    }
    void restore() override {
        // These profiles have no implemented reliable effect readback. Retain the last volatile
        // color rather than replacing the user's previous settings with an invented default.
    }
    ~Logitech() override {
        if (handle_)
            hid_close(handle_);
    }
};
#endif
} // namespace

QList<Device> logitechDevices() {
    QList<Device> devices;
#ifdef AMBIENT_HIDAPI
    if (hid_init() != 0)
        fail("HIDAPI initialization failed");
    auto *list = hid_enumerate(0x046d, 0);
    for (const auto *entry = list; entry; entry = entry->next) {
        const auto *selected = profile(entry->vendor_id, entry->product_id);
        // The documented lighting reports belong to USB interface 1. In particular, never open
        // a boot-keyboard interface, a mouse, or an arbitrary Logitech receiver.
        if (!selected || entry->interface_number != 1 || !entry->path)
            continue;
        Device device;
        device.kind = DeviceKind::Logitech;
        device.path = QByteArray(entry->path);
        device.vendorId = entry->vendor_id;
        device.productId = entry->product_id;
        device.interfaceNumber = entry->interface_number;
        device.label = QString("Logitech %1 — direct USB").arg(selected->name);
        const auto serial = entry->serial_number ? QString::fromWCharArray(entry->serial_number) : QString{};
        device.key =
            QString("logitech:%1:%2:%3").arg(device.productId).arg(serial).arg(device.interfaceNumber);
        device.details = "Experimental Logitech USB · whole-keyboard color · hardware not verified · "
                         "Stop retains last color (previous effect cannot be read) · no Bluetooth/receiver";
        if (std::none_of(devices.begin(), devices.end(),
                         [&](const Device &d) { return d.path == device.path; }))
            devices.push_back(device);
    }
    hid_free_enumeration(list);
#endif
    return devices;
}
std::unique_ptr<ambient::LightingOutput> openLogitech(const Device &device) {
    const auto *selected = profile(device.vendorId, device.productId);
    if (!selected || device.interfaceNumber != 1 || device.path.isEmpty())
        fail("Unsupported Logitech device; select an allowlisted USB keyboard lighting interface");
#ifdef AMBIENT_HIDAPI
    return std::make_unique<Logitech>(device, *selected);
#else
    fail("Built without HIDAPI; install HIDAPI and rebuild");
#endif
}
