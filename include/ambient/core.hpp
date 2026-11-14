#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ambient {
struct Rgb {
    float r{}, g{}, b{};
}; // sRGB, normalized to [0, 1]
struct Hsv {
    std::uint8_t h{}, s{}, v{};
};
struct Pixel {
    std::uint8_t r{}, g{}, b{}, a{255};
};
enum class Sampling { Average, Dominant };
struct SamplingOptions {
    Sampling method{Sampling::Dominant};
    float blackThreshold{.04f};
};
struct Calibration {
    float red{1}, green{1}, blue{1};
    float saturation{1}, gamma{1}, hueDegrees{};
};
// Samples are RGBA, row-major. Horizontal zones preserve left-to-right order.
std::vector<Rgb> sample(std::span<const Pixel> pixels, unsigned width, unsigned height, unsigned zones,
                        unsigned stride = 4);
std::vector<Rgb> sample(std::span<const Pixel> pixels, unsigned width, unsigned height, unsigned zones,
                        SamplingOptions options, unsigned stride = 4);
Rgb calibrate(Rgb rgb, const Calibration &calibration);
Hsv toHsv(Rgb rgb);
class Smoother {
  public:
    std::vector<Rgb> update(std::span<const Rgb> target, float seconds, float tau);
    void reset() {
        previous_.clear();
    }

  private:
    std::vector<Rgb> previous_;
};
// Drivers own their connections and snapshot/restore lifecycle on one worker.
class LightingOutput {
  public:
    virtual ~LightingOutput() = default;
    virtual void write(std::span<const Rgb> zones, float brightness) = 0;
    virtual void restore() = 0;
};
} // namespace ambient
