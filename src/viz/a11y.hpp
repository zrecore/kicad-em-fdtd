// System accessibility preferences the viewer respects: reduce motion, increase contrast, reduce
// transparency. Each can also be forced on or off by a setting (Follow the system, On, Off).
#pragma once
#include <string>

#include "viz/colormap.hpp"

namespace viz {

struct SystemAccess {
  bool reduceMotion = false, highContrast = false, reduceTransparency = false;
};

// What the operating system says right now. macOS: com.apple.universalaccess (reduceMotion, increaseContrast,
// reduceTransparency). Windows: client-area animations and the high-contrast flag. Linux (GNOME): gsettings
// enable-animations and high-contrast. Anything unreadable counts as "off". Only the macOS path has been
// exercised.
SystemAccess systemAccess();

enum class Tri { Follow, On, Off };
Tri parseTri(const std::string& text); // "on", "off"; anything else: Follow

// The effective value of a preference given the user's choice and the system's flag.
inline bool resolve(Tri choice, bool system) {
  return choice == Tri::On || (choice == Tri::Follow && system);
}

// The system's text-size preference as a factor (1 = default). macOS: the body font's preferred size against
// the 13 pt default (a11y_mac.mm). Windows: HKCU\\SOFTWARE\\Microsoft\\Accessibility TextScaleFactor (100-225
// %). GNOME: org.gnome.desktop.interface text-scaling-factor. Unreadable counts as 1. Only macOS is
// exercised.
double systemTextScale();

// The effective text factor: the user's choice ("follow", or a percentage such as "150%") against the
// system's. Clamped to [0.75, 3] so a bad value cannot make the window unusable.
double resolveTextScale(const std::string& choice, double system);

// WCAG 2.x contrast ratio between two sRGB colours (1 to 21). 4.5 is the AA minimum for text, 7 is AAA.
double contrastRatio(Rgb a, Rgb b);

} // namespace viz
