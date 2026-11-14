#include "backends.hpp"
#include "corsair.hpp"
#include "logitech.hpp"
#include "razer.hpp"
#include <stdexcept>

QList<Device> directDevices(QStringList *errors) {
    QList<Device> devices;
    struct Discovery {
        const char *name;
        QList<Device> (*enumerate)();
    };
    for (const auto &driver : {Discovery{"VIA / VialRGB", viaDevices}, Discovery{"Logitech", logitechDevices},
                               Discovery{"Razer", razerDevices}, Discovery{"Corsair", corsairDevices}}) {
        try {
            devices.append(driver.enumerate());
        } catch (const std::exception &e) {
            if (errors)
                errors->append(QString::fromLatin1(driver.name) + ": " + QString::fromUtf8(e.what()));
        }
    }
    return devices;
}

std::unique_ptr<ambient::LightingOutput> openDirect(const Device &device) {
    if (!device.available)
        throw std::runtime_error("Device profile is unavailable");
    switch (device.kind) {
    case DeviceKind::Via:
        return openVia(device);
    case DeviceKind::VialRgb:
        return openVialRgb(device);
    case DeviceKind::Logitech:
        return openLogitech(device);
    case DeviceKind::Razer:
        return openRazer(device);
    case DeviceKind::Corsair:
        return openCorsair(device);
    default:
        throw std::runtime_error("Select a directly connected HID device");
    }
}
