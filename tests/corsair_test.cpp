#include "corsair.hpp"
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
using Report = std::array<unsigned char, 65>;
struct Fake {
    bool initializationFailure{}, openFailure{}, enumerate{}, invalidReply{}, timeout{};
    bool featureFailure{}, featureReplyFailure{};
    unsigned closes{}, frees{}, featureWrites{};
    int rejectWrite{-1};
    std::vector<Report> writes;
} fake;
hid_device_ handle;
Device keyboard(unsigned short product = 0x1b13) {
    Device d;
    d.vendorId = 0x1b1c;
    d.productId = product;
    d.interfaceNumber = 1;
    d.path = "corsair";
    d.kind = DeviceKind::Corsair;
    return d;
}
bool released(const Report &p) {
    return p[1] == 7 && p[2] == 4 && p[3] == 1;
}
void fillReply(unsigned char *data, bool feature) {
    std::fill_n(data, 65, 0);
    const unsigned offset = feature ? 1 : 0;
    data[offset] = 0x0e;
    data[offset + 1] = 1;
    data[offset + 0x14] = fake.invalidReply ? 0 : 0xc0;
}
} // namespace
extern "C" {
int hid_init() {
    return fake.initializationFailure ? -1 : 0;
}
hid_device_info *hid_enumerate(unsigned short vendor, unsigned short product) {
    EXPECT_EQ(vendor, 0x1b1c);
    EXPECT_EQ(product, 0);
    if (!fake.enumerate)
        return nullptr;
    static char path[] = "corsair";
    static wchar_t serial[] = L"serial";
    static hid_device_info entries[5];
    entries[0] = {path, 0x1b1c, 0x1b13, serial, nullptr, 0xffc2, 1, 1, &entries[1]};
    entries[1] = {path, 0x1b1c, 0x1b13, serial, nullptr, 0xffc2, 1, 0, &entries[2]};
    entries[2] = {path, 0x1b1c, 0x1b49, serial, nullptr, 0xffc2, 1, 1, &entries[3]};
    entries[3] = {path, 0x1b1c, 0x1b11, serial, nullptr, 0xffc2, 1, 1, &entries[4]};
    entries[4] = {path, 0x1b1c, 0x1b13, serial, nullptr, 1, 6, 1, nullptr};
    return entries;
}
void hid_free_enumeration(hid_device_info *) {
    ++fake.frees;
}
hid_device *hid_open_path(const char *path) {
    EXPECT_STREQ(path, "corsair");
    return fake.openFailure ? nullptr : &handle;
}
int hid_write(hid_device *, const unsigned char *data, std::size_t size) {
    EXPECT_EQ(size, 65u);
    if (size != 65)
        return -1;
    Report packet{};
    std::copy_n(data, size, packet.begin());
    fake.writes.push_back(packet);
    if (fake.rejectWrite == static_cast<int>(fake.writes.size()) - 1) {
        fake.rejectWrite = -1;
        return -1;
    }
    return 65;
}
int hid_read_timeout(hid_device *, unsigned char *data, std::size_t size, int timeout) {
    EXPECT_EQ(size, 65u);
    EXPECT_GT(timeout, 0);
    if (fake.timeout)
        return 0;
    fillReply(data, false);
    return 64;
}
int hid_send_feature_report(hid_device *, const unsigned char *data, std::size_t size) {
    EXPECT_EQ(size, 65u);
    EXPECT_EQ(data[1], 0x0e);
    EXPECT_EQ(data[2], 1);
    ++fake.featureWrites;
    return fake.featureFailure ? -1 : 65;
}
int hid_get_feature_report(hid_device *, unsigned char *data, std::size_t size) {
    EXPECT_EQ(size, 65u);
    fillReply(data, true);
    return fake.featureReplyFailure ? -1 : 65;
}
void hid_close(hid_device *) {
    ++fake.closes;
}
}
class CorsairProtocol : public testing::Test {
    void SetUp() override {
        fake = {};
    }
};
TEST_F(CorsairProtocol, DiscoversOnlyExplicitUsbModelsAndControlInterfacesWithoutOpening) {
    fake.enumerate = true;
    const auto devices = corsairDevices();
    ASSERT_EQ(devices.size(), 2);
    EXPECT_EQ(devices[0].productId, 0x1b13);
    EXPECT_EQ(devices[1].productId, 0x1b11);
    EXPECT_NE(devices[0].key, devices[1].key);
    EXPECT_FALSE(devices[0].verifiedProfile);
    EXPECT_TRUE(devices[0].details.contains("hardware unverified"));
    EXPECT_TRUE(fake.writes.empty());
    EXPECT_EQ(fake.frees, 1u);
}
TEST_F(CorsairProtocol, InitializationPrecedesColorAndHardwareReleaseIsIdempotent) {
    {
        auto output = openCorsair(keyboard());
        ASSERT_EQ(fake.writes.size(), 2u);
        EXPECT_EQ(fake.writes[0][1], 0x0e);
        EXPECT_EQ(fake.writes[1][2], 5);
        EXPECT_EQ(fake.writes[1][3], 2);
        EXPECT_EQ(fake.writes[1][5], 3);
        output->restore();
        EXPECT_TRUE(released(fake.writes.back()));
        const auto count = fake.writes.size();
        output->restore();
        output->write({}, 1);
        EXPECT_EQ(fake.writes.size(), count);
    }
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(CorsairProtocol, EncodesThreePlanarChannelsAndCommitsOnlyLastChannel) {
    auto output = openCorsair(keyboard());
    const ambient::Rgb colors[] = {{1, 0, 0}, {0, 1, 0}};
    output->write(colors, 1);
    ASSERT_EQ(fake.writes.size(), 14u);
    for (unsigned channel = 0; channel < 3; ++channel) {
        for (unsigned part = 0; part < 3; ++part) {
            const auto &packet = fake.writes[2 + channel * 4 + part];
            const unsigned size = part == 2 ? 24 : 60;
            EXPECT_EQ(packet[0], 0);
            EXPECT_EQ(packet[1], 0x7f);
            EXPECT_EQ(packet[2], part + 1);
            EXPECT_EQ(packet[3], size);
            for (unsigned i = 0; i < size; ++i)
                EXPECT_EQ(packet[5 + i], channel == 2 ? 0 : 128);
        }
        const auto &submit = fake.writes[5 + channel * 4];
        EXPECT_EQ(submit[1], 7);
        EXPECT_EQ(submit[2], 0x28);
        EXPECT_EQ(submit[3], channel + 1);
        EXPECT_EQ(submit[4], 3);
        EXPECT_EQ(submit[5], channel == 2 ? 2 : 1);
    }
}
TEST_F(CorsairProtocol, K95UsesAll168SlotsAndBrightnessIsClamped) {
    auto output = openCorsair(keyboard(0x1b11));
    const ambient::Rgb white[] = {{1, 1, 1}};
    output->write(white, 2);
    EXPECT_EQ(fake.writes[4][3], 48);
    EXPECT_EQ(fake.writes[4][52], 255);
    output->write(white, -1);
    EXPECT_EQ(fake.writes[14][5], 0);
}
TEST_F(CorsairProtocol, DestructorReleasesHardwareLighting) {
    { auto output = openCorsair(keyboard()); }
    EXPECT_TRUE(released(fake.writes.back()));
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(CorsairProtocol, UnsupportedDeviceIsRejectedBeforeOpening) {
    EXPECT_THROW(openCorsair(keyboard(0x1b49)), std::runtime_error);
    auto d = keyboard();
    d.interfaceNumber = 0;
    EXPECT_THROW(openCorsair(d), std::runtime_error);
    EXPECT_TRUE(fake.writes.empty());
    EXPECT_EQ(fake.closes, 0u);
}
TEST_F(CorsairProtocol, InvalidFirmwareDoesNotAcquireSoftwareControl) {
    fake.invalidReply = true;
    EXPECT_THROW(openCorsair(keyboard()), std::runtime_error);
    ASSERT_EQ(fake.writes.size(), 1u);
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(CorsairProtocol, TimeoutUsesFeatureReportFallback) {
    fake.timeout = true;
    { auto output = openCorsair(keyboard()); }
    EXPECT_EQ(fake.featureWrites, 1u);
    EXPECT_TRUE(released(fake.writes.back()));
}
TEST_F(CorsairProtocol, FeatureTransportFailureDoesNotAcquireControl) {
    fake.timeout = true;
    fake.featureFailure = true;
    EXPECT_THROW(openCorsair(keyboard()), std::runtime_error);
    ASSERT_EQ(fake.writes.size(), 1u);
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(CorsairProtocol, FeatureReplyFailureDoesNotAcquireControl) {
    fake.timeout = true;
    fake.featureReplyFailure = true;
    EXPECT_THROW(openCorsair(keyboard()), std::runtime_error);
    ASSERT_EQ(fake.writes.size(), 1u);
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(CorsairProtocol, FailedAcquireAttemptsHardwareReleaseAndCloses) {
    fake.rejectWrite = 1;
    EXPECT_THROW(openCorsair(keyboard()), std::runtime_error);
    EXPECT_TRUE(released(fake.writes.back()));
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(CorsairProtocol, FailedColorWriteSurfacesAndStillReleasesOnDestruction) {
    {
        auto output = openCorsair(keyboard());
        fake.rejectWrite = 3;
        const ambient::Rgb red[] = {{1, 0, 0}};
        EXPECT_THROW(output->write(red, 1), std::runtime_error);
    }
    EXPECT_TRUE(released(fake.writes.back()));
    EXPECT_EQ(fake.closes, 1u);
}
TEST_F(CorsairProtocol, FailedRestoreCanBeRetried) {
    auto output = openCorsair(keyboard());
    fake.rejectWrite = 2;
    EXPECT_THROW(output->restore(), std::runtime_error);
    EXPECT_NO_THROW(output->restore());
    EXPECT_TRUE(released(fake.writes.back()));
}
TEST_F(CorsairProtocol, InitializationAndOpenErrorsDoNotWrite) {
    fake.initializationFailure = true;
    EXPECT_THROW(openCorsair(keyboard()), std::runtime_error);
    EXPECT_THROW(corsairDevices(), std::runtime_error);
    fake.initializationFailure = false;
    fake.openFailure = true;
    EXPECT_THROW(openCorsair(keyboard()), std::runtime_error);
    EXPECT_TRUE(fake.writes.empty());
}

TEST_F(CorsairProtocol, NonFiniteFrameDoesNotSendColorPackets) {
    auto output = openCorsair(keyboard());
    const auto before = fake.writes.size();
    const ambient::Rgb invalid[] = {{std::numeric_limits<float>::quiet_NaN(), 0, 0}};
    EXPECT_THROW(output->write(invalid, 1), std::runtime_error);
    const ambient::Rgb white[] = {{1, 1, 1}};
    EXPECT_THROW(output->write(white, std::numeric_limits<float>::infinity()), std::runtime_error);
    EXPECT_EQ(fake.writes.size(), before);
}
