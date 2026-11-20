// Application theme: Auto follows the operating system's light/dark appearance, Light and Dark force one.
#pragma once
#include <string>

namespace viz {

enum class ThemeMode { Auto, Light, Dark };

ThemeMode parseThemeMode(const std::string& text); // "auto", "light", "dark" (anything else: Auto)

// Whether the system is in dark mode right now: macOS (AppleInterfaceStyle), Windows (AppsUseLightTheme),
// Linux (GNOME color-scheme through gsettings). Defaults to dark where it cannot tell. Only the macOS path
// has been exercised so far.
bool systemPrefersDark();

// The theme to draw with, given the user's choice and what the system reports.
inline bool useDark(ThemeMode mode, bool systemDark) {
  return mode == ThemeMode::Dark || (mode == ThemeMode::Auto && systemDark);
}

} // namespace viz
