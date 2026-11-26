// Checkpoint files (docs/checkpoint.md): a run's state on disk, so a run that was stopped (a time limit, a
// pause, a crash, a restart of the farm worker) continues instead of starting over. Same machine, same
// device and driver, same binary, same inputs: anything else is refused with the reason.
//
// A file is the magic, a format version and three sections (META, HOST, ENGN), each with its length and a
// CRC-32C, so a damaged or cut-short file is recognized. A store keeps two generations (ckpt.a, ckpt.b),
// written to a temporary file, flushed and renamed, alternately: a crash while writing leaves the older one.
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "board/voxelize.hpp"
#include "sim/simulation.hpp"

namespace app::checkpoint {

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// CRC-32C (Castagnoli), the checksum of every section.
std::uint32_t crc32c(std::span<const std::uint8_t> bytes);

// A 128-bit change detector (two FNV variants; not a security hash) over the post-voxelizer model and the
// resolved run options: a checkpoint written for other inputs never matches. 32 lowercase hex digits.
std::string hashRun(const board::Model& model, const sim::RunOptions& options);

// What a checkpoint must match to be resumed (all but runDir), plus where the run's results go.
struct Identity {
  std::string runHash;
  std::string backend; // "cpu" or "gpu"
  std::string deviceUuid, deviceName, driver;
  std::string binaryVersion;
  std::array<std::uint64_t, 3> nodes{}; // grid nodes per axis
  double dt = 0;
  std::string runDir; // the results folder of the run (kept by a resume)
};

struct Loaded {
  Identity identity;
  sim::Checkpoint state;
  char generation = 'a';
};

class Store {
public:
  explicit Store(std::filesystem::path dir);
  const std::filesystem::path& dir() const { return dir_; }
  std::filesystem::path path(char generation) const;
  // Deletes both generations (a fresh run starts without the files of an earlier one).
  void clear() const;
  // Writes the other generation than the newest one, atomically. Throws Error if the disk refuses.
  void save(const Identity& identity, const sim::Checkpoint& state);
  // The newest generation that is intact and matches `expect`. Throws Error naming why each generation was
  // not usable (damaged, written by another version, for another device, for other inputs, missing).
  Loaded load(const Identity& expect);

private:
  std::filesystem::path dir_;
  char next_ = 'a'; // the generation the next save overwrites
};

} // namespace app::checkpoint
