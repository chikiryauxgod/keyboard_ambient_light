#include "backends.hpp"
#include <QCoreApplication>
#include <algorithm>
#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <hidapi.h>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

struct hid_device_ {};
namespace {
using Report = std::array<unsigned char, 33>;
struct Fake {
    unsigned version{12};
    bool vial{}, solid{true};
    unsigned char vialVersion{1}, maximum{128};
    std::array<unsigned char, 6> vialMode{6, 0, 90, 42, 180, 100};
    unsigned char channel{3};
    std::array<unsigned char, 2> brightness{102, 0}, effect{25, 0}, color{42, 180};
    std::vector<Report> writes;
    Report pending{};
    bool initFailure{}, openFailure{}, shortReply{}, closed{}, enumerateDevices{};
    int rejectGet{-1}, rejectSetOnce{-1};
    unsigned opens{}, closes{}, freedEnumerations{};
} fake;
hid_device_ handle;
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
Device device(unsigned char channel = 3) {
    Device d;
    d.path = "fake";
    d.channel = channel;
    return d;
}
bool hasWrites() {
    return std::any_of(fake.writes.begin(), fake.writes.end(), [](const auto &r) { return r[1] == 7; });
}
void lifecycle(unsigned version, unsigned char channel) {
    fake = {};
    fake.version = version;
    fake.channel = channel;
    {
        auto output = openVia(device(channel));
        check(fake.writes.size() == 5, "Must read version and all three settings before changing lighting");
        for (unsigned i = 0; i < 4; ++i)
            check(fake.writes[i][1] != 7, "Snapshot must precede first write");
        const ambient::Rgb green[] = {{0, 1, 0}};
        output->write(green, .5f);
        check(fake.color[0] == 85 && fake.color[1] == 255 && fake.brightness[0] == 128,
              "HSV frame and brightness encoding");
        output->restore();
        check(fake.brightness[0] == 102 && fake.effect[0] == 25 &&
                  fake.color == std::array<unsigned char, 2>{42, 180},
              "Snapshot restoration");
        auto count = fake.writes.size();
        output->restore();
        check(fake.writes.size() == count, "Restore must be idempotent");
        output->write({}, 1);
        check(fake.writes.size() == count, "Empty frame must not write");
    }
    check(fake.opens == 1 && fake.closes == 1, "HID handle lifecycle");
    for (const auto &r : fake.writes) {
        check(r[0] == 0, "Zero Report ID prefix required");
        check(r[1] == 1 || r[1] == 7 || r[1] == 8, "No save, reset or bootloader commands allowed");
        if (r[1] != 1)
            check(r[2] == channel, "Do not change another lighting channel");
    }
}
} // namespace
extern "C" {
int hid_init() {
    return fake.initFailure ? -1 : 0;
}
hid_device_info *hid_enumerate(unsigned short, unsigned short) {
    if (!fake.enumerateDevices)
        return nullptr;
    static char path[] = "fake";
    static wchar_t name[] = L"Test keyboard", serial[] = L"SERIAL";
    static hid_device_info devices[3];
    devices[0] = {path, 0x1111, 0x2222, serial, name, 1, 6, 0, &devices[1]};
    devices[1] = {path, 0x1111, 0x2222, serial, name, 0xff60, 0x61, 2, &devices[2]};
    devices[2] = {path, 0x342d, 0xe506, serial, name, 0xff60, 0x61, 2, nullptr};
    return devices;
}
void hid_free_enumeration(hid_device_info *) {
    ++fake.freedEnumerations;
}
hid_device *hid_open_path(const char *path) {
    if (fake.openFailure || std::strcmp(path, "fake"))
        return nullptr;
    ++fake.opens;
    return &handle;
}
void hid_close(hid_device *) {
    fake.closed = true;
    ++fake.closes;
}
int hid_write(hid_device *, const unsigned char *data, std::size_t size) {
    check(size == 33, "Expected Report ID plus 32-byte VIA packet");
    Report report{};
    std::copy_n(data, 33, report.begin());
    fake.writes.push_back(report);
    fake.pending = report;
    return 33;
}
int hid_read_timeout(hid_device *, unsigned char *data, std::size_t size, int timeout) {
    check(size == 32 && timeout > 0, "Bounded 32-byte HID read required");
    if (fake.shortReply)
        return 2;
    const auto p = fake.pending;
    std::array<unsigned char, 32> reply{};
    std::copy_n(p.begin() + 1, 32, reply.begin());
    if (fake.vial) {
        if (p[2] == 0x40) {
            reply[2] = fake.vialVersion;
            reply[3] = 0;
            reply[4] = fake.maximum;
        } else if (p[2] == 0x42) {
            reply[2] = fake.solid ? 2 : 0xff;
            reply[3] = fake.solid ? 0 : 0xff;
        } else if (p[2] == 0x41) {
            if (p[1] == 7)
                std::copy_n(p.begin() + 3, 6, fake.vialMode.begin());
            std::copy(fake.vialMode.begin(), fake.vialMode.end(), reply.begin() + 2);
        }
    } else if (p[1] == 1) {
        reply[1] = 0;
        reply[2] = static_cast<unsigned char>(fake.version);
    } else if (p[2] != fake.channel || (p[1] == 8 && p[3] == fake.rejectGet))
        reply[0] = 0xff;
    else if (p[1] == 7 && p[3] == fake.rejectSetOnce) {
        fake.rejectSetOnce = -1;
        reply[0] = 0xff;
    } else {
        auto *value = p[3] == 1 ? &fake.brightness : p[3] == 2 ? &fake.effect : &fake.color;
        if (p[1] == 7)
            *value = {p[4], p[5]};
        reply[3] = (*value)[0];
        reply[4] = (*value)[1];
    }
    std::copy(reply.begin(), reply.end(), data);
    return 32;
}
}
class ViaProtocol : public testing::TestWithParam<unsigned> {};
TEST_P(ViaProtocol, SnapshotFrameAndIdempotentRestoration) {
    EXPECT_NO_THROW(lifecycle(GetParam() / 10, static_cast<unsigned char>(GetParam() % 10)));
}
INSTANTIATE_TEST_SUITE_P(StandardVersionsAndChannels, ViaProtocol, testing::Values(122u, 123u, 132u, 133u));
class ViaFailure : public testing::Test {
    void SetUp() override {
        fake = {};
    }
};
TEST_F(ViaFailure, UnsupportedVersionDoesNotWrite) {
    fake.version = 11;
    EXPECT_THROW(openVia(device()), std::runtime_error);
    EXPECT_FALSE(hasWrites());
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(ViaFailure, IncompleteSnapshotDoesNotWrite) {
    fake.rejectGet = 4;
    EXPECT_THROW(openVia(device()), std::runtime_error);
    EXPECT_FALSE(hasWrites());
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(ViaFailure, ShortReplyDoesNotWrite) {
    fake.shortReply = true;
    EXPECT_THROW(openVia(device()), std::runtime_error);
    EXPECT_FALSE(hasWrites());
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(ViaFailure, InitializationFailureDoesNotOpenDevice) {
    fake.initFailure = true;
    EXPECT_THROW(openVia(device()), std::runtime_error);
    EXPECT_EQ(fake.opens, 0u);
}
TEST_F(ViaFailure, AccessFailureDoesNotWrite) {
    fake.openFailure = true;
    EXPECT_THROW(openVia(device()), std::runtime_error);
    EXPECT_TRUE(fake.writes.empty());
}
TEST_F(ViaFailure, InvalidChannelDoesNotOpenDevice) {
    EXPECT_THROW(openVia(device(1)), std::runtime_error);
    EXPECT_EQ(fake.opens, 0u);
}
TEST_F(ViaFailure, FailedActivationRestoresSnapshotAndClosesHandle) {
    fake.rejectSetOnce = 2;
    EXPECT_THROW(openVia(device()), std::runtime_error);
    EXPECT_EQ(fake.effect[0], 25);
    EXPECT_EQ(fake.brightness[0], 102);
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(ViaFailure, RestoreAttemptsRemainingSettingsAfterOneFailure) {
    {
        auto output = openVia(device());
        fake.rejectSetOnce = 4;
        EXPECT_THROW(output->restore(), std::runtime_error);
        EXPECT_EQ(fake.effect[0], 25);
        EXPECT_EQ(fake.brightness[0], 102);
    }
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(ViaFailure, DiscoveryFiltersInterfacesAndBuildsStandardProfilesWithoutWrites) {
    fake.enumerateDevices = true;
    const auto devices = viaDevices();
    ASSERT_EQ(devices.size(), 5);
    EXPECT_EQ(devices[0].channel, 3);
    EXPECT_EQ(devices[1].channel, 2);
    EXPECT_FALSE(devices[0].verifiedProfile);
    EXPECT_TRUE(devices[3].verifiedProfile);
    EXPECT_EQ(devices[2].kind, DeviceKind::VialRgb);
    EXPECT_EQ(devices[4].kind, DeviceKind::VialRgb);
    EXPECT_FALSE(devices[2].verifiedProfile);
    EXPECT_EQ(devices[0].vendorId, 0x1111);
    EXPECT_EQ(devices[0].interfaceNumber, 2);
    EXPECT_NE(devices[0].key, devices[1].key);
    EXPECT_NE(devices[0].key, devices[2].key);
    EXPECT_TRUE(fake.writes.empty());
    EXPECT_EQ(fake.opens, 0u);
    EXPECT_EQ(fake.freedEnumerations, 1u);
}
TEST_F(ViaFailure, DiscoveryHandlesNoDevices) {
    EXPECT_TRUE(viaDevices().isEmpty());
    EXPECT_EQ(fake.freedEnumerations, 1u);
    EXPECT_TRUE(fake.writes.empty());
}

class VialProtocol : public testing::Test {
    void SetUp() override {
        fake = {};
        fake.vial = true;
    }
};
TEST_F(VialProtocol, SnapshotColorBrightnessAndRestoration) {
    const auto original = fake.vialMode;
    {
        auto output = openVialRgb(device());
        ASSERT_EQ(fake.writes.size(), 3u);
        EXPECT_FALSE(hasWrites());
        const ambient::Rgb green[] = {{0, 1, 0}};
        output->write(green, .5f);
        EXPECT_EQ(fake.vialMode, (std::array<unsigned char, 6>{2, 0, 90, 85, 255, 64}));
        output->restore();
        EXPECT_EQ(fake.vialMode, original);
        const auto count = fake.writes.size();
        output->restore();
        output->write({}, 1);
        EXPECT_EQ(fake.writes.size(), count);
    }
    EXPECT_EQ(fake.closes, 1u);
    for (const auto &packet : fake.writes)
        EXPECT_TRUE(packet[1] == 7 || packet[1] == 8); // No EEPROM save commands.
}
TEST_F(VialProtocol, DestructorRestoresAndClampsBrightness) {
    const auto original = fake.vialMode;
    {
        auto output = openVialRgb(device());
        const ambient::Rgb white[] = {{1, 1, 1}};
        output->write(white, 2);
        EXPECT_EQ(fake.vialMode[5], 128);
        output->write(white, -1);
        EXPECT_EQ(fake.vialMode[5], 0);
    }
    EXPECT_EQ(fake.vialMode, original);
}
TEST_F(VialProtocol, UnsupportedProtocolDoesNotModifyLighting) {
    fake.vialVersion = 2;
    EXPECT_THROW(openVialRgb(device()), std::runtime_error);
    EXPECT_FALSE(hasWrites());
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(VialProtocol, MissingSolidEffectDoesNotModifyLighting) {
    fake.solid = false;
    EXPECT_THROW(openVialRgb(device()), std::runtime_error);
    EXPECT_FALSE(hasWrites());
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(VialProtocol, ShortReplyClosesHandleWithoutChangingLighting) {
    fake.shortReply = true;
    EXPECT_THROW(openVialRgb(device()), std::runtime_error);
    EXPECT_FALSE(hasWrites());
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(VialProtocol, OpenFailureDoesNotWrite) {
    fake.openFailure = true;
    EXPECT_THROW(openVialRgb(device()), std::runtime_error);
    EXPECT_TRUE(fake.writes.empty());
    EXPECT_EQ(fake.closes, 0u);
}

TEST_F(VialProtocol, InvalidBrightnessCapabilityDoesNotModifyLighting) {
    fake.maximum = 0;
    EXPECT_THROW(openVialRgb(device()), std::runtime_error);
    EXPECT_FALSE(hasWrites());
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(VialProtocol, FailedAcknowledgementStillRestoresPossiblyAcceptedFrame) {
    const auto original = fake.vialMode;
    auto output = openVialRgb(device());
    const ambient::Rgb red[] = {{1, 0, 0}};
    fake.shortReply = true;
    EXPECT_THROW(output->write(red, 1), std::runtime_error);
    EXPECT_THROW(output->restore(), std::runtime_error);
    fake.shortReply = false;
    EXPECT_NO_THROW(output->restore());
    EXPECT_EQ(fake.vialMode, original);
}
TEST_F(VialProtocol, NonFiniteBrightnessIsRejectedBeforeChangingLighting) {
    auto output = openVialRgb(device());
    const auto before = fake.writes.size();
    const ambient::Rgb white[] = {{1, 1, 1}};
    EXPECT_THROW(output->write(white, std::numeric_limits<float>::quiet_NaN()), std::runtime_error);
    EXPECT_EQ(fake.writes.size(), before);
}
