// Minimal PNG writer (8-bit RGB, zlib "stored" blocks: no compression, no dependency). For test frames and
// headless renders; the interactive window does not use it.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace viz {

// rgb has width*height*3 bytes, row-major from the top. Throws std::runtime_error if the file cannot be
// written.
void writePng(const std::string& path, std::uint32_t width, std::uint32_t height,
              const std::vector<std::uint8_t>& rgb);

} // namespace viz
