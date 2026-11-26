// The operating system's own UI font, found at run time (never bundled, so no font licence comes with the
// program). Each platform has one file: system_font_mac.mm, system_font_win.cpp, system_font_linux.cpp.
#pragma once

#include <initializer_list>
#include <optional>
#include <string>

namespace viz {

struct SystemFont {
  std::string path; // a TrueType or OpenType file (or a collection) the font atlas can load
  int index = 0;    // the face inside a collection (.ttc), 0 for a plain file
  std::string name; // what the system calls it, for logs and the accessibility notes
};

// The system UI font as a file, or nothing if there is none this program can load (the window then keeps its
// built-in font). Candidates are checked for existence, not for rendering quality.
std::optional<SystemFont> systemUiFont();

// Platform-neutral helper: the first existing path of `paths` (empty if none).
std::string firstExistingFile(std::initializer_list<const char*> paths);

} // namespace viz
