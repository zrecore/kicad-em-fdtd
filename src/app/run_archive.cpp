#include "app/run_archive.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace app {
namespace {

static_assert(std::endian::native == std::endian::little, "the phasor file is little-endian");

constexpr char kMagic[4] = {'E', 'M', 'F', 'P'};
constexpr std::uint32_t kVersion = 1;

std::uint16_t toHalf(float f) { // round to nearest even; |f| <= 1 here, so no overflow, subnormals handled
  std::uint32_t x = std::bit_cast<std::uint32_t>(f);
  const std::uint32_t sign = (x >> 16) & 0x8000u;
  x &= 0x7fffffffu;
  if (x >= 0x7f800000u) // inf or nan
    return static_cast<std::uint16_t>(sign | 0x7c00u | (x > 0x7f800000u ? 0x200u : 0u));
  const int exp = static_cast<int>(x >> 23) - 127 + 15;
  if (exp >= 31)
    return static_cast<std::uint16_t>(sign | 0x7c00u);
  if (exp <= 0) { // subnormal half (or zero)
    if (exp < -10)
      return static_cast<std::uint16_t>(sign);
    const std::uint32_t mant = (x & 0x7fffffu) | 0x800000u;
    const int shift = 14 - exp;
    std::uint32_t h = mant >> shift;
    const std::uint32_t rem = mant & ((1u << shift) - 1), half = 1u << (shift - 1);
    if (rem > half || (rem == half && (h & 1u)))
      ++h;
    return static_cast<std::uint16_t>(sign | h);
  }
  std::uint32_t h = (static_cast<std::uint32_t>(exp) << 10) | ((x & 0x7fffffu) >> 13);
  const std::uint32_t rem = x & 0x1fffu;
  if (rem > 0x1000u || (rem == 0x1000u && (h & 1u)))
    ++h; // may carry into the exponent, which is correct
  return static_cast<std::uint16_t>(sign | h);
}

float fromHalf(std::uint16_t h) {
  const std::uint32_t sign = (static_cast<std::uint32_t>(h) & 0x8000u) << 16;
  const std::uint32_t exp = (h >> 10) & 0x1fu, mant = h & 0x3ffu;
  std::uint32_t bits;
  if (exp == 0) {
    if (mant == 0) {
      bits = sign;
    } else { // subnormal: normalise
      int e = -1;
      std::uint32_t m = mant;
      do {
        ++e;
        m <<= 1;
      } while (!(m & 0x400u));
      bits = sign | static_cast<std::uint32_t>(127 - 15 - e) << 23 | ((m & 0x3ffu) << 13);
    }
  } else if (exp == 31) {
    bits = sign | 0x7f800000u | (mant << 13);
  } else {
    bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
  }
  return std::bit_cast<float>(bits);
}

template <class T> void put(std::ofstream& out, const T& v) {
  out.write(reinterpret_cast<const char*>(&v), sizeof v);
}
template <class T> void get(std::ifstream& in, T& v, const char* what) {
  in.read(reinterpret_cast<char*>(&v), sizeof v);
  if (!in)
    throw std::runtime_error(std::string("phasor file: truncated at ") + what);
}

} // namespace

void savePhasors(const std::filesystem::path& file, const viz::PhasorAccumulator& acc) {
  std::ofstream out(file, std::ios::binary);
  if (!out)
    throw std::runtime_error("cannot write " + file.string());
  out.write(kMagic, 4);
  put(out, kVersion);
  put(out, static_cast<std::uint64_t>(acc.frequencies()));
  put(out, static_cast<std::uint64_t>(acc.voxels()));
  for (std::size_t k = 0; k < acc.frequencies(); ++k)
    put(out, acc.frequencyHz(k));
  std::vector<std::uint16_t> buf(acc.voxels());
  for (std::size_t k = 0; k < acc.frequencies(); ++k) {
    float peak = 0;
    for (float v : acc.re(k))
      peak = std::max(peak, std::abs(v));
    for (float v : acc.im(k))
      peak = std::max(peak, std::abs(v));
    put(out, peak);
    for (int part = 0; part < 2; ++part) {
      const auto src = part == 0 ? acc.re(k) : acc.im(k);
      for (std::size_t v = 0; v < src.size(); ++v)
        buf[v] = peak > 0 ? toHalf(src[v] / peak) : std::uint16_t{0};
      out.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size() * sizeof(std::uint16_t)));
    }
  }
  if (!out)
    throw std::runtime_error("write failed: " + file.string());
}

std::unique_ptr<viz::PhasorAccumulator> loadPhasors(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot read " + file.string());
  char magic[4];
  in.read(magic, 4);
  if (!in || std::memcmp(magic, kMagic, 4) != 0)
    throw std::runtime_error("not a phasor file: " + file.string());
  std::uint32_t version;
  std::uint64_t nf, nv;
  get(in, version, "the version");
  if (version > kVersion)
    throw std::runtime_error("phasor file written by a newer version: " + file.string());
  get(in, nf, "the frequency count");
  get(in, nv, "the voxel count");
  if (nf == 0 || nf > 4096 || nv == 0 || nv > (1ull << 31))
    throw std::runtime_error("phasor file: implausible sizes in " + file.string());
  std::vector<double> freqs(nf);
  for (double& f : freqs)
    get(in, f, "the frequencies");
  std::vector<float> re(nf * nv), im(nf * nv);
  std::vector<std::uint16_t> buf(nv);
  for (std::size_t k = 0; k < nf; ++k) {
    float peak;
    get(in, peak, "a scale");
    for (int part = 0; part < 2; ++part) {
      in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(nv * sizeof(std::uint16_t)));
      if (!in)
        throw std::runtime_error("phasor file: truncated in the data of " + file.string());
      float* dst = (part == 0 ? re.data() : im.data()) + k * nv;
      for (std::size_t v = 0; v < nv; ++v)
        dst[v] = fromHalf(buf[v]) * peak;
    }
  }
  auto acc = std::make_unique<viz::PhasorAccumulator>(std::move(freqs), nv);
  acc->load(re, im);
  return acc;
}

} // namespace app
