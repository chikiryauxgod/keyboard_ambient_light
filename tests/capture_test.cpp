#include "capture.hpp"
#include <gtest/gtest.h>
TEST(CaptureRegion, DefaultBottomStripExcludesTaskbar) {
    EXPECT_EQ(regionRect({1920, 1080}, {0, 80, 100, 17}), QRect(0, 864, 1920, 183));
}
TEST(CaptureRegion, ClipsRegionToMonitor) {
    auto r = regionRect({1920, 1080}, {95, 95, 100, 100});
    EXPECT_LT(r.right(), 1920);
    EXPECT_LT(r.bottom(), 1080);
    EXPECT_FALSE(r.isEmpty());
}
TEST(CaptureRegion, NormalizedRegionScalesWithPhysicalFrameSize) {
    auto r = regionRect({3840, 2160}, {0, 80, 100, 17});
    EXPECT_EQ(r.x(), 0);
    EXPECT_EQ(r.y(), 1728);
    EXPECT_EQ(r.width(), 3840);
}
TEST(CaptureRegion, EmptyMonitorReturnsEmptyRegion) {
    EXPECT_TRUE(regionRect({}, {}).isEmpty());
}
TEST(CaptureRegion, InvalidPercentagesAreClamped) {
    EXPECT_EQ(regionRect({100, 100}, {-20, -20, 0, 0}), QRect(0, 0, 1, 1));
}
TEST(CaptureRegion, TinyMonitorNeverProducesEmptyValidRegion) {
    EXPECT_EQ(regionRect({1, 1}, {99, 99, 1, 1}), QRect(0, 0, 1, 1));
}
