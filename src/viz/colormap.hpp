// Diverging colour map for signed fields: blue (negative) - near-white/dark neutral - red (positive).
// Perceptual and colour-blind-safe in intent: lightness is symmetric about zero, so sign is carried by hue
// and magnitude by lightness distance from the neutral.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace viz {

struct Rgb {
  std::uint8_t r, g, b;
};

// value in [-1, 1] (clamped), neutral colour for 0 chosen for a dark background.
inline Rgb diverging(float value) {
  const float t = std::clamp(value, -1.0f, 1.0f);
  const float a = std::abs(t);
  // dark neutral (20,20,24) towards saturated blue (66,135,245) or orange-red (245,98,66)
  const std::array<float, 3> neutral{20, 20, 24}, pos{245, 98, 66}, neg{66, 135, 245};
  const auto& end = t >= 0 ? pos : neg;
  auto mix = [&](int i) { return static_cast<std::uint8_t>(neutral[i] + (end[i] - neutral[i]) * a + 0.5f); };
  return {mix(0), mix(1), mix(2)};
}

// The default field colours per theme (data/ui_defaults.json holds the same values; these are the fallbacks).
inline constexpr Rgb kDarkNegative{86, 180, 233}, kDarkPositive{240, 228, 66}; // Okabe-Ito sky blue, yellow
inline constexpr Rgb kLightNegative{33, 80, 170}, kLightPositive{178, 24, 43}; // blue, red

namespace detail {
inline float lerp(float a, float b, float t) {
  return a + (b - a) * t;
}
inline std::uint8_t byte(float v) {
  return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
}
} // namespace detail

// White-centred diverging map for light panels (the look of published field plots: zero is white, strong
// fields reach the full colour, so weak lobes stay legible and the figure prints well). `negative` and
// `positive` are the strong-field colours (defaults: traditional blue and red). `gamma` lifts weak fields.
inline Rgb divergingLight(float value, float gamma = 0.6f, Rgb negative = kLightNegative,
                          Rgb positive = kLightPositive) {
  const float t = std::clamp(value, -1.0f, 1.0f);
  const float a = std::pow(std::abs(t), gamma);
  const Rgb end = t >= 0 ? positive : negative;
  const std::array<float, 3> e{static_cast<float>(end.r), static_cast<float>(end.g),
                               static_cast<float>(end.b)};
  auto mix = [&](int i) {
    const float mid = detail::lerp(250.0f, e[i], 0.5f); // halfway from white to the colour
    return detail::byte(a < 0.5f ? detail::lerp(250.0f, mid, a / 0.5f)
                                 : detail::lerp(mid, e[i], (a - 0.5f) / 0.5f));
  };
  return {mix(0), mix(1), mix(2)};
}

// Dark-background diverging map: near-black at zero; the colours glow and brighten as the field strengthens,
// so sign is also carried by lightness when the two colours differ in it. Defaults: Okabe-Ito sky blue
// (negative) and yellow (positive), a colour-blind-safe pair; yellow is clearly lighter, so sign also shows
// in lightness.
inline Rgb divergingDark(float value, float gamma = 0.6f, Rgb negative = kDarkNegative,
                         Rgb positive = kDarkPositive) {
  const float t = std::clamp(value, -1.0f, 1.0f);
  const float a = std::pow(std::abs(t), gamma);
  const Rgb base = t >= 0 ? positive : negative;
  const std::array<float, 3> b{static_cast<float>(base.r), static_cast<float>(base.g),
                               static_cast<float>(base.b)};
  auto mix = [&](int i) {
    const float zero = i == 2 ? 14.0f : 8.0f, mid = b[i] * 0.75f, end = detail::lerp(b[i], 255.0f, 0.4f);
    return detail::byte(a < 0.5f ? detail::lerp(zero, mid, a / 0.5f)
                                 : detail::lerp(mid, end, (a - 0.5f) / 0.5f));
  };
  return {mix(0), mix(1), mix(2)};
}

// Luminous variant for the 3D volume on a dark background: strong fields glow (towards white), weak fields
// stay transparent instead of drawing dark. Uses the dark theme's colours.
inline Rgb luminous(float value, Rgb negative = kDarkNegative, Rgb positive = kDarkPositive) {
  const float t = std::clamp(value, -1.0f, 1.0f);
  const float a = std::abs(t);
  const Rgb base = t >= 0 ? positive : negative;
  const std::array<float, 3> b{static_cast<float>(base.r), static_cast<float>(base.g),
                               static_cast<float>(base.b)};
  auto mix = [&](int i) { return detail::byte(detail::lerp(b[i], detail::lerp(b[i], 255.0f, 0.7f), a * a)); };
  return {mix(0), mix(1), mix(2)};
}

// Volume colour on a light background: absorption (colour over white), not glow. Weak field is a pale tint of
// the colour, strong field the full colour, slightly deepened.
inline Rgb volumeLight(float value, Rgb negative = kLightNegative, Rgb positive = kLightPositive) {
  const float t = std::clamp(value, -1.0f, 1.0f);
  const float a = std::abs(t);
  const Rgb base = t >= 0 ? positive : negative;
  const std::array<float, 3> b{static_cast<float>(base.r), static_cast<float>(base.g),
                               static_cast<float>(base.b)};
  auto mix = [&](int i) {
    return detail::byte(detail::lerp(detail::lerp(250.0f, b[i], 0.5f), b[i] * 0.9f, a));
  };
  return {mix(0), mix(1), mix(2)};
}

} // namespace viz
