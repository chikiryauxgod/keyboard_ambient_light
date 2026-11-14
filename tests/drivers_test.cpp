#include "backends.hpp"
#include <gtest/gtest.h>
#include <stdexcept>

namespace {
DeviceKind failing{DeviceKind::Preview};
QList<DeviceKind> scanned;
QList<Device> enumerate(DeviceKind kind) {
    scanned.append(kind);
    if (kind == failing)
        throw std::runtime_error("Transport unavailable");
    Device device;
    device.kind = kind;
    return {device};
}
} // namespace

QList<Device> viaDevices() {
    return enumerate(DeviceKind::Via);
}
QList<Device> logitechDevices() {
    return enumerate(DeviceKind::Logitech);
}
QList<Device> razerDevices() {
    return enumerate(DeviceKind::Razer);
}
QList<Device> corsairDevices() {
    return enumerate(DeviceKind::Corsair);
}

namespace {
class Noop final : public ambient::LightingOutput {
  public:
    void write(std::span<const ambient::Rgb>, float) override {}
    void restore() override {}
};
DeviceKind opened{DeviceKind::Preview};
QByteArray openedPath;
auto open(DeviceKind kind, const Device &device) {
    opened = kind;
    openedPath = device.path;
    return std::make_unique<Noop>();
}
} // namespace

std::unique_ptr<ambient::LightingOutput> openVia(const Device &device) {
    return open(DeviceKind::Via, device);
}
std::unique_ptr<ambient::LightingOutput> openVialRgb(const Device &device) {
    return open(DeviceKind::VialRgb, device);
}
std::unique_ptr<ambient::LightingOutput> openLogitech(const Device &device) {
    return open(DeviceKind::Logitech, device);
}
std::unique_ptr<ambient::LightingOutput> openRazer(const Device &device) {
    return open(DeviceKind::Razer, device);
}
std::unique_ptr<ambient::LightingOutput> openCorsair(const Device &device) {
    return open(DeviceKind::Corsair, device);
}

class DriverDiscovery : public testing::TestWithParam<DeviceKind> {};
TEST_P(DriverDiscovery, FailedDriverDoesNotHideOtherKeyboards) {
    failing = GetParam();
    scanned.clear();
    QStringList errors;
    const auto devices = directDevices(&errors);
    EXPECT_EQ(scanned.size(), 4);
    EXPECT_EQ(devices.size(), 3);
    ASSERT_EQ(errors.size(), 1);
    EXPECT_TRUE(errors.front().contains("Transport unavailable"));
    for (const auto &device : devices)
        EXPECT_NE(device.kind, failing);
    EXPECT_EQ(directDevices().size(), 3); // Diagnostics are optional.
}
INSTANTIATE_TEST_SUITE_P(AllTransports, DriverDiscovery,
                         testing::Values(DeviceKind::Via, DeviceKind::Logitech, DeviceKind::Razer,
                                         DeviceKind::Corsair));

TEST(DriverRegistry, RejectsUnavailableAndNonHidDevicesWithoutOpening) {
    opened = DeviceKind::Preview;
    Device device;
    device.available = false;
    EXPECT_THROW(openDirect(device), std::runtime_error);
    device.available = true;
    device.kind = DeviceKind::OpenRgb;
    EXPECT_THROW(openDirect(device), std::runtime_error);
    device.kind = DeviceKind::Preview;
    EXPECT_THROW(openDirect(device), std::runtime_error);
    EXPECT_EQ(opened, DeviceKind::Preview);
}

TEST(DriverRegistry, OpensSelectedDriverAndPreservesDevicePath) {
    for (const auto kind : {DeviceKind::Via, DeviceKind::VialRgb, DeviceKind::Logitech, DeviceKind::Razer,
                            DeviceKind::Corsair}) {
        Device device;
        device.kind = kind;
        device.path = "selected-device";
        EXPECT_NE(openDirect(device), nullptr);
        EXPECT_EQ(opened, kind);
        EXPECT_EQ(openedPath, device.path);
    }
}
