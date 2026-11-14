#include "ambient/core.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ambient {
namespace {
float linear(float x) {
    return x <= .04045f ? x / 12.92f : std::pow((x + .055f) / 1.055f, 2.4f);
}
float srgb(float x) {
    return x <= .0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.f / 2.4f) - .055f;
}
std::uint8_t byte(float x) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(x, 0.f, 1.f) * 255.f));
}
void finiteColor(Rgb c) {
    if (!std::isfinite(c.r) || !std::isfinite(c.g) || !std::isfinite(c.b))
        throw std::invalid_argument("Non-finite RGB color");
}
} // namespace
std::vector<Rgb> sample(std::span<const Pixel> p, unsigned w, unsigned h, unsigned zones, unsigned stride) {
    return sample(p, w, h, zones, {Sampling::Average, 0}, stride);
}
std::vector<Rgb> sample(std::span<const Pixel> p, unsigned w, unsigned h, unsigned zones,
                        SamplingOptions options, unsigned stride) {
    if (!w || !h || !zones || zones > w || !stride || p.size() / w < h)
        throw std::invalid_argument("Invalid image dimensions or zone count");
    if (!std::isfinite(options.blackThreshold) || options.blackThreshold < 0 || options.blackThreshold > 1)
        throw std::invalid_argument("Black threshold must be in [0, 1]");
    std::vector<Rgb> result(zones);
    for (unsigned z = 0; z < zones; ++z) {
        const unsigned left = static_cast<unsigned>(static_cast<std::uint64_t>(z) * w / zones);
        const unsigned right = static_cast<unsigned>(static_cast<std::uint64_t>(z + 1) * w / zones);
        double r = 0, g = 0, b = 0;
        unsigned count = 0;
        struct Bin {
            double r{}, g{}, b{}, weight{};
            unsigned count{};
        };
        std::array<Bin, 24> bins{};
        for (unsigned y = 0; y < h; y += stride)
            for (unsigned x = left; x < right; x += stride) {
                const auto c = p[static_cast<std::size_t>(y) * w + x];
                const float hi = std::max({c.r, c.g, c.b}) / 255.f;
                if (hi < options.blackThreshold)
                    continue;
                r += linear(c.r / 255.f);
                g += linear(c.g / 255.f);
                b += linear(c.b / 255.f);
                ++count;
                if (options.method == Sampling::Dominant) {
                    const Rgb color{c.r / 255.f, c.g / 255.f, c.b / 255.f};
                    const auto hsv = toHsv(color);
                    // Neutral UI backgrounds must not drown out a colored scene.
                    if (hsv.s < 32)
                        continue;
                    auto &bin = bins[std::min(23u, static_cast<unsigned>(hsv.h) * 24 / 256)];
                    bin.r += linear(color.r);
                    bin.g += linear(color.g);
                    bin.b += linear(color.b);
                    bin.weight += .25 + .75 * hsv.s / 255.;
                    ++bin.count;
                }
            }
        if (options.method == Sampling::Dominant && count) {
            const auto best = std::max_element(
                bins.begin(), bins.end(), [](const auto &a, const auto &b) { return a.weight < b.weight; });
            if (best->count >= std::max(1u, count / 20)) {
                r = best->r;
                g = best->g;
                b = best->b;
                count = best->count;
            }
        }
        if (!count)
            continue;
        result[z] = {srgb(static_cast<float>(r / count)), srgb(static_cast<float>(g / count)),
                     srgb(static_cast<float>(b / count))};
    }
    return result;
}
Rgb calibrate(Rgb c, const Calibration &k) {
    finiteColor(c);
    if (!std::isfinite(k.gamma) || k.gamma <= 0 || !std::isfinite(k.hueDegrees) ||
        !std::isfinite(k.saturation) || !std::isfinite(k.red) || !std::isfinite(k.green) ||
        !std::isfinite(k.blue) || k.red < 0 || k.green < 0 || k.blue < 0 || k.saturation < 0)
        throw std::invalid_argument("Invalid color calibration");
    const float hi = std::max({c.r, c.g, c.b}), lo = std::min({c.r, c.g, c.b}), d = hi - lo;
    float hue = 0;
    if (d > 0) {
        if (hi == c.r)
            hue = std::fmod((c.g - c.b) / d, 6.f);
        else if (hi == c.g)
            hue = (c.b - c.r) / d + 2;
        else
            hue = (c.r - c.g) / d + 4;
    }
    hue = std::fmod(hue + k.hueDegrees / 60.f, 6.f);
    if (hue < 0)
        hue += 6.f;
    const float saturation = std::clamp(hi > 0 ? d / hi * k.saturation : 0, 0.f, 1.f);
    const float chroma = hi * saturation, x = chroma * (1 - std::abs(std::fmod(hue, 2.f) - 1)),
                m = hi - chroma;
    Rgb out;
    if (hue < 1)
        out = {chroma, x, 0};
    else if (hue < 2)
        out = {x, chroma, 0};
    else if (hue < 3)
        out = {0, chroma, x};
    else if (hue < 4)
        out = {0, x, chroma};
    else if (hue < 5)
        out = {x, 0, chroma};
    else
        out = {chroma, 0, x};
    auto channel = [&](float v, float gain) {
        return std::pow(std::clamp((v + m) * gain, 0.f, 1.f), k.gamma);
    };
    return {channel(out.r, k.red), channel(out.g, k.green), channel(out.b, k.blue)};
}
Hsv toHsv(Rgb c) {
    finiteColor(c);
    c = {std::clamp(c.r, 0.f, 1.f), std::clamp(c.g, 0.f, 1.f), std::clamp(c.b, 0.f, 1.f)};
    const float hi = std::max({c.r, c.g, c.b}), lo = std::min({c.r, c.g, c.b}), delta = hi - lo;
    float hue = 0;
    if (delta > 0) {
        if (hi == c.r)
            hue = std::fmod((c.g - c.b) / delta, 6.f);
        else if (hi == c.g)
            hue = (c.b - c.r) / delta + 2.f;
        else
            hue = (c.r - c.g) / delta + 4.f;
        hue /= 6.f;
        if (hue < 0)
            hue += 1;
    }
    return {byte(hue), byte(hi ? delta / hi : 0), byte(hi)};
}
std::vector<Rgb> Smoother::update(std::span<const Rgb> target, float dt, float tau) {
    if (dt < 0 || tau < 0 || !std::isfinite(dt) || !std::isfinite(tau))
        throw std::invalid_argument("Invalid smoothing interval");
    for (auto color : target)
        finiteColor(color);
    if (previous_.size() != target.size())
        previous_.assign(target.begin(), target.end());
    const float alpha = tau == 0 ? 1 : -std::expm1(-dt / tau);
    for (std::size_t i = 0; i < target.size(); ++i) {
        auto &p = previous_[i];
        const auto t = target[i];
        p.r += alpha * (t.r - p.r);
        p.g += alpha * (t.g - p.g);
        p.b += alpha * (t.b - p.b);
    }
    return previous_;
}
} // namespace ambient
