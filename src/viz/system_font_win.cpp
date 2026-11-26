// The Windows system UI font: Segoe UI from the Fonts folder (Windows 10 and later), else Tahoma or Arial.
// Not built or run on the machine this was written on; it is plain Win32 and std::filesystem.
#include <windows.h>

#include <string>

#include "viz/system_font.hpp"

namespace viz {

std::optional<SystemFont> systemUiFont() {
  wchar_t dir[MAX_PATH];
  const UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
  if (n == 0 || n >= MAX_PATH)
    return std::nullopt;
  std::string base;
  for (wchar_t c : std::wstring(dir, n)) // the Windows folder is ASCII on every supported setup; skip if not
    if (c < 128)
      base += static_cast<char>(c);
    else
      return std::nullopt;
  base += "\\Fonts\\";
  struct Candidate {
    const char* file;
    const char* name;
  };
  for (const Candidate& c : {Candidate{"segoeui.ttf", "Segoe UI"}, Candidate{"tahoma.ttf", "Tahoma"},
                             Candidate{"arial.ttf", "Arial"}}) {
    const std::string path = base + c.file;
    if (!firstExistingFile({path.c_str()}).empty())
      return SystemFont{path, 0, c.name};
  }
  return std::nullopt;
}

} // namespace viz
