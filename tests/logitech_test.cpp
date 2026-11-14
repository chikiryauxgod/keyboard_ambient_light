#include "logitech.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <hidapi.h>
#include <limits>
#include <stdexcept>
#include <vector>

struct hid_device_ {};
namespace {
struct Fake {
    bool initFailure{}, openFailure{}, shortWrite{}, enumerate{}, paddedWrite{};
    unsigned opens{}, closes{}, frees{};
    std::vector<std::vector<unsigned char>> packets;
} fake;
hid_device_ handle;
Device keyboard(unsigned short product = 0xc331) {
    Device device;
    device.kind = DeviceKind::Logitech;
    device.path = "logitech-test";
    device.vendorId = 0x046d;
    device.productId = product;
    device.interfaceNumber = 1;
    return device;
}
class LogitechProtocol : public testing::Test {
    void SetUp() override {
        fake = {};
    }
};
} // namespace
extern "C" {
int hid_init() {
    return fake.initFailure ? -1 : 0;
}
hid_device_info *hid_enumerate(unsigned short, unsigned short) {
    if (!fake.enumerate)
        return nullptr;
    static char path[] = "logitech-test", wrongPath[] = "wrong";
    static wchar_t serial[] = L"SERIAL", name[] = L"Logitech";
    static hid_device_info entries[5];
    entries[0] = {wrongPath, 0x046d, 0xc547, serial, name, 0xff00, 1, 1, &entries[1]}; // Receiver.
    entries[1] = {wrongPath, 0x046d, 0xc331, serial, name, 1, 6, 0, &entries[2]};      // Boot keyboard.
    entries[2] = {path, 0x046d, 0xc331, serial, name, 0xff00, 1, 1, &entries[3]};
    entries[3] = {path, 0x046d, 0xc331, serial, name, 0xff00, 1, 1, &entries[4]};  // Duplicate collection.
    entries[4] = {wrongPath, 0x046d, 0xc33a, serial, name, 0xff00, 1, 1, nullptr}; // Single-color G413.
    return entries;
}
void hid_free_enumeration(hid_device_info *) {
    ++fake.frees;
}
hid_device *hid_open_path(const char *path) {
    if (fake.openFailure || std::strcmp(path, "logitech-test"))
        return nullptr;
    ++fake.opens;
    return &handle;
}
void hid_close(hid_device *) {
    ++fake.closes;
}
int hid_write(hid_device *, const unsigned char *packet, std::size_t size) {
    fake.packets.emplace_back(packet, packet + size);
    return fake.shortWrite ? static_cast<int>(size) - 1 : fake.paddedWrite ? 64 : static_cast<int>(size);
}
int hid_read_timeout(hid_device *, unsigned char *, std::size_t, int) {
    return 0;
}
}
TEST_F(LogitechProtocol, DiscoverySelectsLightingInterfaceAndDeduplicatesWithoutWrites) {
    fake.enumerate = true;
    const auto devices = logitechDevices();
    ASSERT_EQ(devices.size(), 1);
    EXPECT_EQ(devices.front().kind, DeviceKind::Logitech);
    EXPECT_EQ(devices.front().interfaceNumber, 1);
    EXPECT_FALSE(devices.front().verifiedProfile);
    EXPECT_TRUE(devices.front().details.contains("Stop retains last color"));
    EXPECT_TRUE(fake.packets.empty());
    EXPECT_EQ(fake.opens, 0u);
    EXPECT_EQ(fake.frees, 1u);
}
TEST_F(LogitechProtocol, DiscoveryHandlesNoDevices) {
    EXPECT_TRUE(logitechDevices().isEmpty());
    EXPECT_EQ(fake.frees, 1u);
}
TEST_F(LogitechProtocol, InitializationFailuresAreReported) {
    fake.initFailure = true;
    EXPECT_THROW(logitechDevices(), std::runtime_error);
    EXPECT_THROW(openLogitech(keyboard()), std::runtime_error);
    EXPECT_EQ(fake.opens, 0u);
}
TEST_F(LogitechProtocol, OpenDoesNotModifyLightingAndDestructorCloses) {
    {
        auto output = openLogitech(keyboard());
        EXPECT_TRUE(fake.packets.empty());
        output->restore();
        output->write({}, 1);
        EXPECT_TRUE(fake.packets.empty());
    }
    EXPECT_EQ(fake.opens, 1u);
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(LogitechProtocol, ZoneAverageBrightnessAndNumberedReport) {
    auto output = openLogitech(keyboard());
    const ambient::Rgb zones[] = {{1, 0, 0}, {0, 1, 0}};
    output->write(zones, .5f);
    ASSERT_EQ(fake.packets.size(), 1u);
    const auto &packet = fake.packets.front();
    ASSERT_EQ(packet.size(), 20u);
    EXPECT_EQ(packet[0], 0x11);
    EXPECT_EQ(packet[1], 0xff);
    EXPECT_EQ(packet[2], 0x0d);
    EXPECT_EQ(packet[3], 0x3c);
    EXPECT_EQ(packet[4], 0);
    EXPECT_EQ(packet[5], 1);
    EXPECT_EQ(packet[6], 64);
    EXPECT_EQ(packet[7], 64);
    EXPECT_EQ(packet[8], 0);
    EXPECT_EQ(packet[16], 0); // Never store a persistent user lighting profile.
}
TEST_F(LogitechProtocol, BrightnessIsClampedAndStopDoesNotInventAReplacementEffect) {
    auto output = openLogitech(keyboard());
    const ambient::Rgb white[] = {{1, 1, 1}};
    output->write(white, 2);
    EXPECT_EQ(fake.packets.back()[6], 255);
    output->write(white, -1);
    EXPECT_EQ(fake.packets.back()[6], 0);
    output->restore();
    output->restore();
    EXPECT_EQ(fake.packets.size(), 2u);
}
TEST_F(LogitechProtocol, TransportErrorsAreReportedAndHandleIsClosed) {
    fake.openFailure = true;
    EXPECT_THROW(openLogitech(keyboard()), std::runtime_error);
    EXPECT_EQ(fake.closes, 0u);
    fake.openFailure = false;
    {
        auto output = openLogitech(keyboard());
        fake.shortWrite = true;
        const ambient::Rgb green[] = {{0, 1, 0}};
        EXPECT_THROW(output->write(green, 1), std::runtime_error);
    }
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(LogitechProtocol, UnknownDeviceAndWrongInterfaceAreRejectedBeforeOpening) {
    EXPECT_THROW(openLogitech(keyboard(0xc547)), std::runtime_error);
    auto wrong = keyboard();
    wrong.interfaceNumber = 0;
    EXPECT_THROW(openLogitech(wrong), std::runtime_error);
    wrong = keyboard();
    wrong.vendorId = 0x1532;
    EXPECT_THROW(openLogitech(wrong), std::runtime_error);
    EXPECT_EQ(fake.opens, 0u);
    EXPECT_TRUE(fake.packets.empty());
}
TEST_F(LogitechProtocol, AllAllowlistedProfilesUseTheirDocumentedFeature) {
    const std::array<std::pair<unsigned short, unsigned char>, 9> profiles{{{0xc336, 0x0c},
                                                                            {0xc330, 0x0d},
                                                                            {0xc342, 0x0d},
                                                                            {0xc33c, 0x0d},
                                                                            {0xc331, 0x0d},
                                                                            {0xc337, 0x0d},
                                                                            {0xc32b, 0x10},
                                                                            {0xc335, 0x10},
                                                                            {0xc339, 0x0d}}};
    const ambient::Rgb blue[] = {{0, 0, 1}};
    for (const auto &[product, feature] : profiles) {
        auto output = openLogitech(keyboard(product));
        output->write(blue, 1);
        EXPECT_EQ(fake.packets.back()[2], feature) << product;
        EXPECT_EQ(fake.packets.back()[8], 255);
    }
}
TEST_F(LogitechProtocol, G815IsExcludedBecauseReferenceUsesPersistentStorage) {
    EXPECT_THROW(openLogitech(keyboard(0xc33f)), std::runtime_error);
    EXPECT_EQ(fake.opens, 0u);
    EXPECT_TRUE(fake.packets.empty());
}
TEST_F(LogitechProtocol, NonFiniteChannelsProduceBoundedBlackInsteadOfUndefinedConversion) {
    auto output = openLogitech(keyboard());
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const ambient::Rgb invalid[] = {{nan, std::numeric_limits<float>::infinity(), -1}};
    output->write(invalid, 1);
    ASSERT_EQ(fake.packets.size(), 1u);
    EXPECT_EQ(fake.packets.front()[6], 0);
    EXPECT_EQ(fake.packets.front()[7], 0);
    EXPECT_EQ(fake.packets.front()[8], 0);
}

TEST_F(LogitechProtocol, WindowsPaddedOutputLengthIsNotAnError) {
    fake.paddedWrite = true;
    auto output = openLogitech(keyboard());
    const ambient::Rgb red[] = {{1, 0, 0}};
    EXPECT_NO_THROW(output->write(red, 1));
    ASSERT_EQ(fake.packets.size(), 1u);
    EXPECT_EQ(fake.packets.front().size(), 20u);
    EXPECT_EQ(fake.packets.front()[6], 255);
}
