#pragma once
#include "core.hpp"
#include "color_maps.hpp"
inline std::array<float, 3> sampleColorGradientFormula(int gradient, float u) {
    u = std::clamp(u, 0.f, 1.f);
    auto mix = [](std::array<float,3> a, std::array<float,3> b, float t) {
        t = std::clamp(t, 0.f, 1.f);
        return std::array<float,3>{a[0]+(b[0]-a[0])*t, a[1]+(b[1]-a[1])*t, a[2]+(b[2]-a[2])*t};
    };
    if (gradient == 0) {
        const std::array<float,3> a{.10f,.15f,.85f}, b{.12f,.85f,.75f}, c{.98f,.88f,.08f}, d{.9f,.08f,.04f};
        return u < .5f ? mix(a,b,u*2) : u < .8f ? mix(b,c,(u-.5f)*3.3333333f) : mix(c,d,(u-.8f)*5);
    }
    if (gradient == 1)
        return u < .5f ? mix({.1f,.15f,.9f},{1,1,1},u*2) : mix({1,1,1},{.9f,.05f,.05f},(u-.5f)*2);
    if (gradient == 2)
        return {.5f+.5f*std::cos(6.2831853f*u), .5f+.5f*std::cos(6.2831853f*(u+.33f)), .5f+.5f*std::cos(6.2831853f*(u+.67f))};
    if (gradient == 3) return mix({.02f,.02f,.02f},{1,.95f,.1f},u);
    if (gradient == 4) return {u,u,u};
    if (gradient == 5) return mix({.02f,.02f,.15f},{1,.02f,0},u);
    if (gradient == 6) return {std::clamp(1.5f-std::abs(4*u-3),0.f,1.f), std::clamp(1.5f-std::abs(4*u-2),0.f,1.f), std::clamp(1.5f-std::abs(4*u-1),0.f,1.f)};
    if (gradient == 7 || gradient == 8 || gradient == 9) {
        const auto &lut = gradient == 7 ? atomx::color_maps::magma
                         : gradient == 8 ? atomx::color_maps::viridis
                                         : atomx::color_maps::plasma;
        const float position = u * float(atomx::color_maps::sampleCount - 1);
        const auto lower = size_t(position);
        const auto upper = std::min(lower + 1, size_t(atomx::color_maps::sampleCount - 1));
        const float blend = position - float(lower);
        std::array<float, 3> result{};
        for (int channel = 0; channel < 3; ++channel) {
            const float a = float(lut[lower][channel]) / 255.f;
            const float b = float(lut[upper][channel]) / 255.f;
            result[channel] = a + (b - a) * blend;
        }
        return result;
    }
    return mix({.02f,.02f,.02f},{1,.95f,.1f},u);
}
inline constexpr int colorGradientCount = 10;
inline const auto &colorGradientLut() {
    static const auto lut = [] {
        std::array<std::array<std::array<float, 3>, atomx::color_maps::sampleCount>, colorGradientCount> result{};
        for (int gradient = 0; gradient < colorGradientCount; ++gradient)
            for (int sample = 0; sample < atomx::color_maps::sampleCount; ++sample)
                result[gradient][sample] = sampleColorGradientFormula(
                    gradient, float(sample) / float(atomx::color_maps::sampleCount - 1));
        return result;
    }();
    return lut;
}
inline std::array<float, 3> sampleColorGradient(int gradient, float u) {
    gradient = std::clamp(gradient, 0, colorGradientCount - 1);
    u = std::clamp(u, 0.f, 1.f);
    const auto &lut = colorGradientLut()[gradient];
    const float position = u * float(atomx::color_maps::sampleCount - 1);
    const size_t lower = size_t(position);
    const size_t upper = std::min(lower + 1, size_t(atomx::color_maps::sampleCount - 1));
    const float blend = position - float(lower);
    std::array<float, 3> result{};
    for (int channel = 0; channel < 3; ++channel)
        result[channel] = lut[lower][channel] + (lut[upper][channel] - lut[lower][channel]) * blend;
    return result;
}
