#include "razer.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <hidapi.h>
#include <limits>
#include <vector>

struct hid_device_ {};
namespace {
using Packet = std::array<unsigned char, 91>;
struct Fake {
    bool initError{}, openError{}, shortWrite{}, shortRead{}, badChecksum{}, badIdentity{}, busy{},
        discovered{};
    unsigned char mode{}, brightness{102};
    unsigned opens{}, closes{}, frees{};
    std::vector<Packet> writes;
} fake;
hid_device_ handle;
unsigned char crc(const Packet &packet) {
    unsigned char value{};
    for (unsigned i = 3; i <= 88; ++i)
        value ^= packet[i];
    return value;
}
Device profile() {
    Device device;
    device.kind = DeviceKind::Razer;
    device.path = "razer-test";
    device.vendorId = 0x1532;
    device.productId = 0x0203;
    device.interfaceNumber = 1;
    return device;
}
} // namespace
extern "C" {
int hid_init() {
    return fake.initError ? -1 : 0;
}
hid_device *hid_open_path(const char *path) {
    if (fake.openError || std::strcmp(path, "razer-test"))
        return nullptr;
    ++fake.opens;
    return &handle;
}
void hid_close(hid_device *) {
    ++fake.closes;
}
hid_device_info *hid_enumerate(unsigned short vendor, unsigned short) {
    EXPECT_EQ(vendor, 0x1532);
    if (!fake.discovered)
        return nullptr;
    static char path[] = "razer-test";
    static wchar_t serial[] = L"123", name[] = L"BlackWidow Chroma";
    static hid_device_info info[4];
    info[0] = {path, 0x1532, 0x0203, serial, name, 1, 6, 0, &info[1]};
    info[1] = {path, 0x1532, 0x0203, serial, name, 1, 6, 1, &info[2]};
    info[2] = {path, 0x1532, 0xffff, serial, name, 1, 6, 1, &info[3]};
    info[3] = {path, 0x1532, 0x0209, serial, name, 1, 6, 1, nullptr};
    return info;
}
void hid_free_enumeration(hid_device_info *) {
    ++fake.frees;
}
int hid_send_feature_report(hid_device *, const unsigned char *data, std::size_t size) {
    EXPECT_EQ(size, 91u);
    Packet packet{};
    std::copy_n(data, std::min(size, packet.size()), packet.begin());
    EXPECT_EQ(packet[0], 0);
    EXPECT_EQ(packet[89], crc(packet));
    fake.writes.push_back(packet);
    if (packet[7] == 0 && packet[8] == 4)
        fake.mode = packet[9];
    if (packet[7] == 3 && packet[8] == 3)
        fake.brightness = packet[11];
    return fake.shortWrite ? 1 : 91;
}
int hid_get_feature_report(hid_device *, unsigned char *data, std::size_t size) {
    EXPECT_EQ(size, 91u);
    auto reply = fake.writes.back();
    reply[1] = fake.busy ? 1 : 2;
    if (reply[7] == 0 && reply[8] == 0x84) {
        reply[9] = fake.mode;
        reply[10] = 0;
    }
    if (reply[7] == 3 && reply[8] == 0x83)
        reply[11] = fake.brightness;
    if (fake.badIdentity)
        reply[8] ^= 1;
    reply[89] = crc(reply);
    if (fake.badChecksum)
        reply[89] ^= 1;
    std::copy(reply.begin(), reply.end(), data);
    return fake.shortRead ? 12 : 91;
}
}
class RazerProtocol : public testing::Test {
    void SetUp() override {
        fake = {};
    }
};
TEST_F(RazerProtocol, SnapshotFrameAndRestoration) {
    auto output = openRazer(profile());
    ASSERT_EQ(fake.writes.size(), 2u);
    EXPECT_EQ(fake.writes[0][8], 0x84);
    EXPECT_EQ(fake.writes[1][8], 0x83);
    const ambient::Rgb green[] = {{0, 1, 0}};
    output->write(green, .5f);
    ASSERT_EQ(fake.writes.size(), 5u);
    const auto &frame = fake.writes.back();
    EXPECT_EQ(frame[7], 3);
    EXPECT_EQ(frame[8], 0x0a);
    EXPECT_EQ(frame[6], 4);
    EXPECT_EQ(frame[9], 6);
    EXPECT_EQ(frame[10], 0);
    EXPECT_EQ(frame[11], 128);
    EXPECT_EQ(frame[12], 0);
    EXPECT_EQ(fake.mode, 3);
    EXPECT_EQ(fake.brightness, 255);
    output->restore();
    EXPECT_EQ(fake.mode, 0);
    EXPECT_EQ(fake.brightness, 102);
    const auto count = fake.writes.size();
    output->restore();
    output->write({}, 1);
    EXPECT_EQ(fake.writes.size(), count);
    output.reset();
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(RazerProtocol, DestructorRestoresAndFramesDoNotReinitialize) {
    {
        auto output = openRazer(profile());
        const ambient::Rgb white[] = {{1, 1, 1}, {0, 0, 0}};
        output->write(white, 1);
        const auto count = fake.writes.size();
        output->write(white, 1);
        EXPECT_EQ(fake.writes.size(), count + 1);
        EXPECT_EQ(fake.writes.back()[10], 128);
    }
    EXPECT_EQ(fake.mode, 0);
    EXPECT_EQ(fake.brightness, 102);
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(RazerProtocol, RejectsUnsupportedProfileBeforeOpening) {
    auto device = profile();
    device.productId = 0xffff;
    EXPECT_THROW(openRazer(device), std::runtime_error);
    device = profile();
    device.interfaceNumber = 0;
    EXPECT_THROW(openRazer(device), std::runtime_error);
    EXPECT_EQ(fake.opens, 0u);
    EXPECT_TRUE(fake.writes.empty());
}
TEST_F(RazerProtocol, DiscoveryFiltersModelsAndInterfacesWithoutWrites) {
    fake.discovered = true;
    const auto devices = razerDevices();
    ASSERT_EQ(devices.size(), 2);
    EXPECT_EQ(devices[0].kind, DeviceKind::Razer);
    EXPECT_FALSE(devices[0].verifiedProfile);
    EXPECT_NE(devices[0].key, devices[1].key);
    EXPECT_TRUE(devices[0].details.contains("not previous animation"));
    EXPECT_EQ(fake.frees, 1u);
    EXPECT_EQ(fake.opens, 0u);
    EXPECT_TRUE(fake.writes.empty());
}
TEST_F(RazerProtocol, EmptyDiscoveryAndInitializationFailure) {
    EXPECT_TRUE(razerDevices().isEmpty());
    fake.initError = true;
    EXPECT_THROW(razerDevices(), std::runtime_error);
    EXPECT_THROW(openRazer(profile()), std::runtime_error);
    EXPECT_TRUE(fake.writes.empty());
}
TEST_F(RazerProtocol, PermissionFailureDoesNotWrite) {
    fake.openError = true;
    EXPECT_THROW(openRazer(profile()), std::runtime_error);
    EXPECT_TRUE(fake.writes.empty());
    EXPECT_EQ(fake.closes, 0u);
}
TEST_F(RazerProtocol, ShortWriteClosesHandle) {
    fake.shortWrite = true;
    EXPECT_THROW(openRazer(profile()), std::runtime_error);
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(RazerProtocol, ShortReplyClosesHandle) {
    fake.shortRead = true;
    EXPECT_THROW(openRazer(profile()), std::runtime_error);
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(RazerProtocol, CorruptChecksumClosesHandle) {
    fake.badChecksum = true;
    EXPECT_THROW(openRazer(profile()), std::runtime_error);
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(RazerProtocol, MismatchedReplyClosesHandle) {
    fake.badIdentity = true;
    EXPECT_THROW(openRazer(profile()), std::runtime_error);
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(RazerProtocol, BusyResponseIsNotReportedAsApplied) {
    fake.busy = true;
    EXPECT_THROW(openRazer(profile()), std::runtime_error);
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(RazerProtocol, RestoreAttemptsBothSettingsAndCanRetry) {
    auto output = openRazer(profile());
    const ambient::Rgb white[] = {{1, 1, 1}};
    output->write(white, 1);
    fake.busy = true;
    const auto count = fake.writes.size();
    EXPECT_THROW(output->restore(), std::runtime_error);
    EXPECT_EQ(fake.writes.size(), count + 2);
    fake.busy = false;
    EXPECT_NO_THROW(output->restore());
    EXPECT_EQ(fake.mode, 0);
    EXPECT_EQ(fake.brightness, 102);
}

TEST_F(RazerProtocol, NonFiniteFrameIsRejectedBeforeChangingLighting) {
    auto output = openRazer(profile());
    const auto before = fake.writes.size();
    const ambient::Rgb invalid[] = {{std::numeric_limits<float>::quiet_NaN(), 0, 0}};
    EXPECT_THROW(output->write(invalid, 1), std::runtime_error);
    const ambient::Rgb white[] = {{1, 1, 1}};
    EXPECT_THROW(output->write(white, std::numeric_limits<float>::infinity()), std::runtime_error);
    EXPECT_EQ(fake.writes.size(), before);
}
