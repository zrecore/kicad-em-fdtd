// Plain byte streams for run state (checkpoints): fixed-width values and arrays of trivially copyable
// elements, with bounds-checked reads that throw instead of reading past the end of a damaged file. The byte
// order is the machine's own: a checkpoint is only resumed on the machine that wrote it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace core {

class ByteWriter {
public:
  template <class T> void put(const T& v) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    bytes_.insert(bytes_.end(), p, p + sizeof(T));
  }
  // An element count (64 bit) followed by the elements.
  template <class T> void putArray(std::span<const T> a) {
    static_assert(std::is_trivially_copyable_v<T>);
    put<std::uint64_t>(a.size());
    const auto* p = reinterpret_cast<const std::uint8_t*>(a.data());
    bytes_.insert(bytes_.end(), p, p + a.size_bytes());
  }
  template <class T> void putArray(const std::vector<T>& a) { putArray<T>(std::span<const T>(a)); }
  std::vector<std::uint8_t>& bytes() { return bytes_; }
  const std::vector<std::uint8_t>& bytes() const { return bytes_; }

private:
  std::vector<std::uint8_t> bytes_;
};

class ByteReader {
public:
  explicit ByteReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
  template <class T> T get() {
    static_assert(std::is_trivially_copyable_v<T>);
    need(sizeof(T));
    T v;
    std::memcpy(&v, bytes_.data() + pos_, sizeof(T));
    pos_ += sizeof(T);
    return v;
  }
  // Reads an array written by putArray. `expected` (if not npos) is the only acceptable length.
  template <class T> void getArray(std::vector<T>& out, std::size_t expected = npos) {
    static_assert(std::is_trivially_copyable_v<T>);
    const std::uint64_t n = get<std::uint64_t>();
    if (expected != npos && n != expected)
      throw std::runtime_error("the checkpoint holds an array of a different size than this run needs");
    if (n > (bytes_.size() - pos_) / sizeof(T))
      throw std::runtime_error("the checkpoint is truncated");
    out.resize(static_cast<std::size_t>(n));
    std::memcpy(out.data(), bytes_.data() + pos_, out.size() * sizeof(T));
    pos_ += out.size() * sizeof(T);
  }
  bool atEnd() const { return pos_ == bytes_.size(); }
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);

private:
  void need(std::size_t n) const {
    if (n > bytes_.size() - pos_)
      throw std::runtime_error("the checkpoint is truncated");
  }
  std::span<const std::uint8_t> bytes_;
  std::size_t pos_ = 0;
};

} // namespace core
