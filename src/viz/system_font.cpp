#include "viz/system_font.hpp"

#include <filesystem>

namespace viz {

std::string firstExistingFile(std::initializer_list<const char*> paths) {
  std::error_code ec;
  for (const char* p : paths)
    if (std::filesystem::is_regular_file(p, ec))
      return p;
  return {};
}

} // namespace viz
