#include "viz/png.hpp"

#include <array>
#include <fstream>
#include <stdexcept>

namespace viz {
namespace {

std::uint32_t crc32(const std::uint8_t* data, std::size_t n, std::uint32_t crc = 0xFFFFFFFFu) {
  static const auto table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k)
        c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  for (std::size_t i = 0; i < n; ++i)
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}

void be32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  for (int s = 24; s >= 0; s -= 8)
    v.push_back(static_cast<std::uint8_t>(x >> s));
}

void chunk(std::ofstream& f, const char type[4], const std::vector<std::uint8_t>& payload) {
  std::vector<std::uint8_t> out;
  be32(out, static_cast<std::uint32_t>(payload.size()));
  std::vector<std::uint8_t> body(type, type + 4);
  body.insert(body.end(), payload.begin(), payload.end());
  out.insert(out.end(), body.begin(), body.end());
  be32(out, crc32(body.data(), body.size()) ^ 0xFFFFFFFFu);
  f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}

} // namespace

void writePng(const std::string& path, std::uint32_t width, std::uint32_t height,
              const std::vector<std::uint8_t>& rgb) {
  if (rgb.size() != static_cast<std::size_t>(width) * height * 3)
    throw std::invalid_argument("writePng: rgb size does not match width*height*3");
  std::ofstream f(path, std::ios::binary);
  if (!f)
    throw std::runtime_error("cannot write " + path);
  static const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  f.write(reinterpret_cast<const char*>(sig), 8);
  std::vector<std::uint8_t> ihdr;
  be32(ihdr, width);
  be32(ihdr, height);
  ihdr.insert(ihdr.end(),
              {8, 2, 0, 0, 0}); // 8-bit, truecolour, deflate, no filter method variants, no interlace
  chunk(f, "IHDR", ihdr);

  std::vector<std::uint8_t> raw; // each scanline prefixed with filter type 0
  raw.reserve(rgb.size() + height);
  for (std::uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb.begin() + static_cast<std::ptrdiff_t>(y) * width * 3,
               rgb.begin() + static_cast<std::ptrdiff_t>(y + 1) * width * 3);
  }
  std::vector<std::uint8_t> z{0x78, 0x01}; // zlib header; then stored deflate blocks of at most 65535 bytes
  for (std::size_t pos = 0; pos < raw.size();) {
    const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
    z.push_back(pos + n == raw.size() ? 1 : 0);
    z.push_back(static_cast<std::uint8_t>(n & 0xFF));
    z.push_back(static_cast<std::uint8_t>(n >> 8));
    z.push_back(static_cast<std::uint8_t>(~n & 0xFF));
    z.push_back(static_cast<std::uint8_t>((~n >> 8) & 0xFF));
    z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
             raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
    pos += n;
  }
  std::uint32_t a = 1, b = 0; // Adler-32 of the uncompressed data
  for (std::uint8_t byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  be32(z, (b << 16) | a);
  chunk(f, "IDAT", z);
  chunk(f, "IEND", {});
}

} // namespace viz
