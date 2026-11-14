#include "ambient/core.hpp"
#include <gtest/gtest.h>
#include <limits>
#include <stdexcept>
using namespace ambient;
namespace {
constexpr float epsilon = .005f;
void colorNear(Rgb a, Rgb b) {
    EXPECT_NEAR(a.r, b.r, epsilon);
    EXPECT_NEAR(a.g, b.g, epsilon);
    EXPECT_NEAR(a.b, b.b, epsilon);
}
TEST(Sampling, HorizontalZonesPreservePhysicalOrder) {
    const Pixel p[] = {{255, 0, 0}, {255, 0, 0}, {0, 0, 255}, {0, 0, 255}};
    auto c = sample(p, 4, 1, 2, 1);
    ASSERT_EQ(c.size(), 2u);
    colorNear(c[0], {1, 0, 0});
    colorNear(c[1], {0, 0, 1});
}
TEST(Sampling, AverageIsComputedInLinearLight) {
    const Pixel p[] = {{0, 0, 0}, {255, 255, 255}};
    auto c = sample(p, 2, 1, 1, 1).front();
    colorNear(c, {.73536f, .73536f, .73536f});
}
TEST(Sampling, DominantHueSurvivesNeutralBackgrounds) {
    const Pixel p[] = {{255, 0, 0}, {255, 0, 0}, {0, 0, 255}, {240, 240, 240}, {0, 0, 0}};
    colorNear(sample(p, 5, 1, 1, {Sampling::Dominant, .04f}, 1).front(), {1, 0, 0});
}
TEST(Sampling, DarkSceneRemainsBlack) {
    const Pixel p[] = {{0, 0, 0}, {1, 1, 1}};
    colorNear(sample(p, 2, 1, 1, {Sampling::Dominant, .04f}, 1).front(), {});
}
TEST(Sampling, NeutralSceneRemainsNeutral) {
    const Pixel p[] = {{128, 128, 128}, {128, 128, 128}};
    colorNear(sample(p, 2, 1, 1, {Sampling::Dominant, .04f}, 1).front(),
              {128 / 255.f, 128 / 255.f, 128 / 255.f});
}
TEST(Sampling, SparseSamplingAndUnevenZonesHaveSamples) {
    const Pixel p[] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
    auto c = sample(p, 3, 1, 2, 50);
    ASSERT_EQ(c.size(), 2u);
    colorNear(c[0], {1, 0, 0});
    colorNear(c[1], {0, 1, 0});
}
TEST(Sampling, RejectsInvalidDimensionsStrideAndThreshold) {
    const Pixel p[] = {{255, 0, 0}};
    EXPECT_THROW(sample(p, 0, 1, 1), std::invalid_argument);
    EXPECT_THROW(sample(p, 1, 0, 1), std::invalid_argument);
    EXPECT_THROW(sample(p, 1, 1, 0), std::invalid_argument);
    EXPECT_THROW(sample(p, 1, 2, 1), std::invalid_argument);
    EXPECT_THROW(sample(p, 1, 1, 2), std::invalid_argument);
    EXPECT_THROW(sample(p, 1, 1, 1, 0), std::invalid_argument);
    for (float threshold : {-.1f, 1.1f, std::numeric_limits<float>::quiet_NaN()})
        EXPECT_THROW(sample(p, 1, 1, 1, {Sampling::Dominant, threshold}), std::invalid_argument);
}
TEST(Calibration, IdentityPreservesColor) {
    colorNear(calibrate({.2f, .5f, .8f}, {}), {.2f, .5f, .8f});
}
TEST(Calibration, GainsAdjustWhiteBalanceAndClampOutput) {
    Calibration k;
    k.red = .5f;
    k.blue = 2;
    colorNear(calibrate({1, 1, 1}, k), {.5f, 1, 1});
}
class HueRotation : public testing::TestWithParam<float> {};
TEST_P(HueRotation, WrapsInBothDirections) {
    Calibration k;
    k.hueDegrees = GetParam();
    colorNear(calibrate({1, 0, 0}, k), {0, 1, 0});
}
INSTANTIATE_TEST_SUITE_P(FullTurns, HueRotation, testing::Values(120.f, 480.f, -240.f, -960.f));
TEST(Calibration, SaturationCanProduceNeutralAndGammaControlsIntensity) {
    Calibration k;
    k.saturation = 0;
    colorNear(calibrate({1, .2f, 0}, k), {1, 1, 1});
    k = {};
    k.gamma = 2;
    colorNear(calibrate({.5f, .5f, .5f}, k), {.25f, .25f, .25f});
}
TEST(Calibration, RejectsInvalidControlsAndNonFiniteColors) {
    Calibration k;
    k.gamma = 0;
    EXPECT_THROW(calibrate({}, k), std::invalid_argument);
    k = {};
    k.red = -1;
    EXPECT_THROW(calibrate({}, k), std::invalid_argument);
    k = {};
    k.hueDegrees = std::numeric_limits<float>::infinity();
    EXPECT_THROW(calibrate({}, k), std::invalid_argument);
    EXPECT_THROW(calibrate({std::numeric_limits<float>::quiet_NaN(), 0, 0}, {}), std::invalid_argument);
}
struct HsvCase {
    Rgb rgb;
    int h, s, v;
};
class HsvConversion : public testing::TestWithParam<HsvCase> {};
TEST_P(HsvConversion, EncodesViaColor) {
    const auto c = GetParam();
    auto hsv = toHsv(c.rgb);
    EXPECT_EQ(hsv.h, c.h);
    EXPECT_EQ(hsv.s, c.s);
    EXPECT_EQ(hsv.v, c.v);
}
INSTANTIATE_TEST_SUITE_P(PrimaryAndNeutral, HsvConversion,
                         testing::Values(HsvCase{{1, 0, 0}, 0, 255, 255}, HsvCase{{0, 1, 0}, 85, 255, 255},
                                         HsvCase{{0, 0, 1}, 170, 255, 255}, HsvCase{{1, 1, 1}, 0, 0, 255},
                                         HsvCase{{0, 0, 0}, 0, 0, 0}, HsvCase{{1, 0, 1}, 213, 255, 255}));
TEST(Hsv, ClampsFiniteInputsAndRejectsNaN) {
    EXPECT_EQ(toHsv({2, -1, 0}).v, 255);
    EXPECT_THROW(toHsv({0, 0, std::numeric_limits<float>::quiet_NaN()}), std::invalid_argument);
}
TEST(Smoothing, IsIndependentOfFrameRate) {
    Smoother a, b;
    const Rgb black[] = {{}}, white[] = {{1, 1, 1}};
    a.update(black, 0, .2f);
    b.update(black, 0, .2f);
    auto once = a.update(white, .1f, .2f);
    b.update(white, .05f, .2f);
    auto twice = b.update(white, .05f, .2f);
    colorNear(once[0], twice[0]);
}
TEST(Smoothing, HandlesFirstFrameZeroTimeAndImmediateTransitions) {
    Smoother s;
    const Rgb black[] = {{}}, white[] = {{1, 1, 1}};
    colorNear(s.update(black, 0, .2f)[0], {});
    colorNear(s.update(white, 0, .2f)[0], {});
    colorNear(s.update(white, .1f, 0)[0], {1, 1, 1});
}
TEST(Smoothing, ResetAndChangedZoneCountStartFromCurrentTarget) {
    Smoother s;
    const Rgb black[] = {{}}, white[] = {{1, 1, 1}}, two[] = {{}, {1, 0, 0}};
    s.update(black, 0, .2f);
    s.reset();
    colorNear(s.update(white, 0, .2f)[0], {1, 1, 1});
    auto zones = s.update(two, 0, .2f);
    ASSERT_EQ(zones.size(), 2u);
    colorNear(zones[1], {1, 0, 0});
    EXPECT_TRUE(s.update({}, 0, .2f).empty());
}
TEST(Smoothing, RejectsInvalidTimesAndNonFiniteInput) {
    Smoother s;
    const Rgb white[] = {{1, 1, 1}}, bad[] = {{std::numeric_limits<float>::quiet_NaN(), 0, 0}};
    EXPECT_THROW(s.update(white, -1, .2f), std::invalid_argument);
    EXPECT_THROW(s.update(white, .1f, -1), std::invalid_argument);
    EXPECT_THROW(s.update(white, std::numeric_limits<float>::infinity(), .2f), std::invalid_argument);
    EXPECT_THROW(s.update(bad, .1f, .2f), std::invalid_argument);
}
} // namespace
