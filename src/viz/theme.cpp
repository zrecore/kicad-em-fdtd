#include "viz/theme.hpp"

#include <cstdio>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace viz {

ThemeMode parseThemeMode(const std::string& text) {
  if (text == "light")
    return ThemeMode::Light;
  if (text == "dark")
    return ThemeMode::Dark;
  return ThemeMode::Auto;
}

bool systemPrefersDark() {
#if defined(__APPLE__)
  CFPreferencesAppSynchronize(kCFPreferencesAnyApplication); // pick up a change made while we are running
  CFTypeRef value = CFPreferencesCopyValue(CFSTR("AppleInterfaceStyle"), kCFPreferencesAnyApplication,
                                           kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
  bool dark = false; // the key is absent in light mode
  if (value) {
    if (CFGetTypeID(value) == CFStringGetTypeID())
      dark = CFStringCompare(static_cast<CFStringRef>(value), CFSTR("Dark"), 0) == kCFCompareEqualTo;
    CFRelease(value);
  }
  return dark;
#elif defined(_WIN32)
  DWORD light = 1, size = sizeof light;
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                   L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) == ERROR_SUCCESS)
    return light == 0;
  return true;
#else
  if (FILE* p = popen("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null", "r")) {
    char buf[128] = {0};
    const bool read = std::fgets(buf, sizeof buf, p) != nullptr;
    pclose(p);
    if (read)
      return std::string(buf).find("prefer-light") == std::string::npos &&
             std::string(buf).find("default") == std::string::npos;
  }
  return true;
#endif
}

} // namespace viz
