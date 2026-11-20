#include "viz/a11y.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace viz {

Tri parseTri(const std::string& text) {
  return text == "on" ? Tri::On : text == "off" ? Tri::Off : Tri::Follow;
}

double contrastRatio(Rgb a, Rgb b) {
  auto lum = [](Rgb c) {
    auto lin = [](double v) {
      v /= 255.0;
      return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
  };
  const double la = lum(a), lb = lum(b);
  return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

#if !defined(__APPLE__) && !defined(_WIN32)
namespace {
// Output of `gsettings get <schema> <key>`, or empty when gsettings is missing.
std::string gsettings(const char* command) {
  std::string out;
  if (FILE* p = popen(command, "r")) {
    char buf[128] = {0};
    if (std::fgets(buf, sizeof buf, p))
      out = buf;
    pclose(p);
  }
  return out;
}
} // namespace
#endif

double resolveTextScale(const std::string& choice, double system) {
  double v = system;
  if (choice != "follow" && !choice.empty()) {
    const double pct = std::atof(choice.c_str());
    if (pct > 0)
      v = pct / 100.0;
  }
  return std::clamp(v, 0.75, 3.0);
}

#if defined(__APPLE__)
double macTextScale(); // a11y_mac.mm
#endif

double systemTextScale() {
#if defined(__APPLE__)
  return macTextScale();
#elif defined(_WIN32)
  DWORD pct = 100, size = sizeof pct;
  if (RegGetValueW(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Accessibility", L"TextScaleFactor",
                   RRF_RT_REG_DWORD, nullptr, &pct, &size) != ERROR_SUCCESS)
    pct = 100;
  return pct / 100.0;
#else
  const double v = std::atof(
      gsettings("gsettings get org.gnome.desktop.interface text-scaling-factor 2>/dev/null").c_str());
  return v > 0 ? v : 1.0;
#endif
}

SystemAccess systemAccess() {
  SystemAccess a;
#if defined(__APPLE__)
  auto flag = [](CFStringRef key) {
    CFPreferencesAppSynchronize(CFSTR("com.apple.universalaccess")); // pick up changes made while running
    CFTypeRef v = CFPreferencesCopyAppValue(key, CFSTR("com.apple.universalaccess"));
    bool on = false;
    if (v) {
      if (CFGetTypeID(v) == CFBooleanGetTypeID())
        on = CFBooleanGetValue(static_cast<CFBooleanRef>(v));
      else if (CFGetTypeID(v) == CFNumberGetTypeID()) {
        int n = 0;
        CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberIntType, &n);
        on = n != 0;
      }
      CFRelease(v);
    }
    return on;
  };
  a.reduceMotion = flag(CFSTR("reduceMotion"));
  a.highContrast = flag(CFSTR("increaseContrast"));
  a.reduceTransparency = flag(CFSTR("reduceTransparency"));
#elif defined(_WIN32)
  BOOL animations = TRUE;
  if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0))
    a.reduceMotion = !animations;
  HIGHCONTRASTW hc{sizeof hc, 0, nullptr};
  if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0))
    a.highContrast = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
  a.reduceTransparency = a.highContrast;
#else
  a.reduceMotion =
      gsettings("gsettings get org.gnome.desktop.interface enable-animations 2>/dev/null").find("false") !=
      std::string::npos;
  a.highContrast =
      gsettings("gsettings get org.gnome.desktop.a11y.interface high-contrast 2>/dev/null").find("true") !=
      std::string::npos;
  a.reduceTransparency = a.highContrast;
#endif
  return a;
}

} // namespace viz
