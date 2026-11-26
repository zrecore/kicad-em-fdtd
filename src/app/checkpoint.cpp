#include "app/checkpoint.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>

#include "core/bytes.hpp"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace app::checkpoint {
namespace {

constexpr char kMagic[8] = {'E', 'M', 'F', 'D', 'C', 'K', 'P', 'T'};
constexpr std::uint32_t kFormat = 1;
constexpr std::uint32_t kMeta = 0x4154454D; // "META"
constexpr std::uint32_t kHost = 0x54534F48; // "HOST"
constexpr std::uint32_t kEngn = 0x4E474E45; // "ENGN"

std::array<std::uint32_t, 256> makeCrcTable() {
  std::array<std::uint32_t, 256> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k)
      c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1; // reflected Castagnoli polynomial
    t[i] = c;
  }
  return t;
}

void putString(core::ByteWriter& w, const std::string& s) {
  w.putArray<char>(std::span<const char>(s.data(), s.size()));
}
std::string getString(core::ByteReader& r) {
  std::vector<char> v;
  r.getArray(v);
  return std::string(v.begin(), v.end());
}

std::vector<std::uint8_t> metaBytes(const Identity& id, std::size_t steps) {
  core::ByteWriter w;
  putString(w, id.runHash);
  putString(w, id.backend);
  putString(w, id.deviceUuid);
  putString(w, id.deviceName);
  putString(w, id.driver);
  putString(w, id.binaryVersion);
  putString(w, id.runDir);
  for (std::uint64_t n : id.nodes)
    w.put(n);
  w.put(id.dt);
  w.put<std::uint64_t>(steps);
  return std::move(w.bytes());
}

Identity parseMeta(std::span<const std::uint8_t> bytes, std::size_t& steps) {
  core::ByteReader r(bytes);
  Identity id;
  id.runHash = getString(r);
  id.backend = getString(r);
  id.deviceUuid = getString(r);
  id.deviceName = getString(r);
  id.driver = getString(r);
  id.binaryVersion = getString(r);
  id.runDir = getString(r);
  for (auto& n : id.nodes)
    n = r.get<std::uint64_t>();
  id.dt = r.get<double>();
  steps = static_cast<std::size_t>(r.get<std::uint64_t>());
  return id;
}

void appendSection(std::vector<std::uint8_t>& out, std::uint32_t tag,
                   const std::vector<std::uint8_t>& bytes) {
  core::ByteWriter w;
  w.put(tag);
  w.put<std::uint64_t>(bytes.size());
  w.put(crc32c(bytes));
  out.insert(out.end(), w.bytes().begin(), w.bytes().end());
  out.insert(out.end(), bytes.begin(), bytes.end());
}

// Writes `parts` to `file` and makes sure the bytes are on the disk before the caller renames it.
void writeDurably(const std::filesystem::path& file,
                  const std::vector<const std::vector<std::uint8_t>*>& parts) {
  std::FILE* f = std::fopen(file.string().c_str(), "wb");
  if (!f)
    throw Error("cannot write the checkpoint " + file.string());
  bool ok = true;
  for (const auto* p : parts)
    ok = ok && (p->empty() || std::fwrite(p->data(), 1, p->size(), f) == p->size());
  ok = ok && std::fflush(f) == 0;
#ifdef _WIN32
  ok = ok && _commit(_fileno(f)) == 0;
#else
  ok = ok && fsync(fileno(f)) == 0;
#endif
  ok = (std::fclose(f) == 0) && ok;
  if (!ok) {
    std::error_code ec;
    std::filesystem::remove(file, ec);
    throw Error("the disk refused the checkpoint " + file.string() + " (full?)");
  }
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary | std::ios::ate);
  if (!in)
    throw Error("is missing");
  const std::streamsize n = in.tellg();
  in.seekg(0);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(n));
  if (n > 0)
    in.read(reinterpret_cast<char*>(bytes.data()), n);
  if (!in)
    throw Error("could not be read");
  return bytes;
}

struct Parsed {
  Identity id;
  sim::Checkpoint state;
};

// Splits and checks a file. Throws Error("is damaged: ...") for anything that is not a whole, intact file.
Parsed parse(const std::vector<std::uint8_t>& bytes) {
  const std::string damaged = "is damaged: ";
  if (bytes.size() < sizeof kMagic + 4 || std::memcmp(bytes.data(), kMagic, sizeof kMagic) != 0)
    throw Error(damaged + "not a checkpoint file");
  core::ByteReader r(std::span<const std::uint8_t>(bytes).subspan(sizeof kMagic));
  try {
    if (r.get<std::uint32_t>() != kFormat)
      throw Error("was written by another checkpoint format version");
    std::array<std::vector<std::uint8_t>, 3> sections;
    const std::array<std::uint32_t, 3> tags{kMeta, kHost, kEngn};
    std::size_t offset = sizeof kMagic + 4;
    for (std::size_t i = 0; i < 3; ++i) {
      core::ByteReader h(std::span<const std::uint8_t>(bytes).subspan(offset));
      const auto tag = h.get<std::uint32_t>();
      const auto len = h.get<std::uint64_t>();
      const auto crc = h.get<std::uint32_t>();
      offset += 16;
      if (tag != tags[i])
        throw Error(damaged + "a section is out of order");
      if (len > bytes.size() - offset)
        throw Error(damaged + "the file is cut short");
      sections[i].assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                         bytes.begin() + static_cast<std::ptrdiff_t>(offset + len));
      if (crc32c(sections[i]) != crc)
        throw Error(damaged + "a checksum does not match");
      offset += static_cast<std::size_t>(len);
    }
    Parsed p;
    p.id = parseMeta(sections[0], p.state.steps);
    p.state.host = std::move(sections[1]);
    p.state.engine = std::move(sections[2]);
    return p;
  } catch (const Error&) {
    throw;
  } catch (const std::runtime_error&) {
    throw Error(damaged + "the file is cut short");
  }
}

// Why `have` cannot be resumed as `want` ("" if it can).
std::string mismatch(const Identity& have, const Identity& want) {
  if (have.binaryVersion != want.binaryVersion)
    return "was written by emfdtd " + have.binaryVersion + ", this is " + want.binaryVersion;
  if (have.backend != want.backend)
    return "was written by the " + have.backend + " backend, this run uses the " + want.backend;
  if (have.deviceUuid != want.deviceUuid || have.deviceName != want.deviceName)
    return "was written on " + have.deviceName + " (" + have.deviceUuid + "), this run uses " +
           want.deviceName + " (" + want.deviceUuid +
           "): a checkpoint resumes only on the device that wrote it";
  if (have.driver != want.driver)
    return "was written with the driver " + have.driver + ", this run uses " + want.driver;
  if (have.nodes != want.nodes || std::memcmp(&have.dt, &want.dt, sizeof(double)) != 0 ||
      have.runHash != want.runHash)
    return "was written for other inputs (the board, the settings or the options differ)";
  return "";
}

} // namespace

std::uint32_t crc32c(std::span<const std::uint8_t> bytes) {
  static const std::array<std::uint32_t, 256> table = makeCrcTable();
  std::uint32_t c = 0xFFFFFFFFu;
  for (std::uint8_t b : bytes)
    c = table[(c ^ b) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

namespace {
class Hasher {
public:
  void add(const void* data, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i) {
      a_ = (a_ ^ p[i]) * 0x100000001B3ull;       // FNV-1a
      b_ = (b_ * 0x100000001B3ull) ^ (p[i] + 1); // FNV-1 with another basis and a shifted byte
    }
  }
  template <class T> void value(const T& v) { add(&v, sizeof v); }
  void text(const std::string& s) {
    value<std::uint64_t>(s.size());
    add(s.data(), s.size());
  }
  std::string hex() const {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%016llx%016llx", static_cast<unsigned long long>(a_),
                  static_cast<unsigned long long>(b_));
    return buf;
  }

private:
  std::uint64_t a_ = 0xCBF29CE484222325ull, b_ = 0x84222325CBF29CE4ull;
};
} // namespace

std::string hashRun(const board::Model& m, const sim::RunOptions& o) {
  Hasher h;
  for (fdtd::Axis a : {fdtd::X, fdtd::Y, fdtd::Z})
    for (std::size_t i = 0; i < m.grid.nodes(a); ++i)
      h.value(m.grid.edge(a, i));
  for (const fdtd::Material& mat : m.materials) {
    h.value(mat.epsR);
    h.value(mat.sigma);
    h.value(mat.pec);
  }
  h.value<std::uint64_t>(m.cellMaterial.size());
  h.add(m.cellMaterial.data(), m.cellMaterial.size());
  for (const fdtd::PecEdge& e : m.pecEdges) {
    h.value(e.axis);
    h.value(e.node);
  }
  for (const board::PortPlan& p : m.ports) {
    h.value(p.impedance);
    h.value(p.series);
    h.value(p.parallel);
    h.value(p.lumped);
    for (const board::PortEdge& e : p.edges) {
      h.value(e.edge.i);
      h.value(e.edge.j);
      h.value(e.edge.k);
      h.value(e.edge.component);
      h.value(e.edge.resistance);
      h.value(e.edge.capacitance);
      h.value(e.edge.inductance);
      h.value(e.strip);
      h.value(e.share);
      h.value(e.sign);
    }
  }
  for (double d : {o.fMinHz, o.excitationMinHz, o.excitationMaxHz, o.courant, o.decay, o.fMaxHz})
    h.value(d);
  for (std::size_t n : {o.points, o.cpmlCells, o.maxSteps, o.sampleEvery})
    h.value(n);
  return h.hex();
}

Store::Store(std::filesystem::path dir) : dir_(std::move(dir)) {}

std::filesystem::path Store::path(char generation) const {
  return dir_ / (std::string("ckpt.") + generation);
}

void Store::clear() const {
  std::error_code ec;
  for (const char g : {'a', 'b'})
    std::filesystem::remove(path(g), ec);
  std::filesystem::remove(dir_ / "ckpt.tmp", ec);
  std::filesystem::remove(dir_ / "stop", ec);
}

void Store::save(const Identity& id, const sim::Checkpoint& state) {
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  std::vector<std::uint8_t> head(kMagic, kMagic + sizeof kMagic), sections;
  core::ByteWriter fv;
  fv.put(kFormat);
  head.insert(head.end(), fv.bytes().begin(), fv.bytes().end());
  appendSection(sections, kMeta, metaBytes(id, state.steps));
  // The big sections are written from their own buffers: only their small headers are built here.
  const auto headerFor = [](std::uint32_t tag, const std::vector<std::uint8_t>& bytes) {
    std::vector<std::uint8_t> h;
    core::ByteWriter w;
    w.put(tag);
    w.put<std::uint64_t>(bytes.size());
    w.put(crc32c(bytes));
    h = std::move(w.bytes());
    return h;
  };
  const auto hostHead = headerFor(kHost, state.host), engnHead = headerFor(kEngn, state.engine);
  const std::filesystem::path tmp = dir_ / "ckpt.tmp", target = path(next_);
  writeDurably(tmp, {&head, &sections, &hostHead, &state.host, &engnHead, &state.engine});
  std::filesystem::rename(tmp, target, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    throw Error("could not replace " + target.string() + ": " + ec.message());
  }
  next_ = next_ == 'a' ? 'b' : 'a';
}

Loaded Store::load(const Identity& expect) {
  std::optional<Loaded> best;
  std::string why;
  for (const char g : {'a', 'b'}) {
    const std::string name = path(g).filename().string();
    try {
      Parsed p = parse(readFile(path(g)));
      if (const std::string bad = mismatch(p.id, expect); !bad.empty()) {
        why += (why.empty() ? "" : "; ") + name + " " + bad;
        continue;
      }
      if (!best || p.state.steps > best->state.steps)
        best = Loaded{std::move(p.id), std::move(p.state), g};
    } catch (const Error& e) {
      why += (why.empty() ? "" : "; ") + name + " " + e.what();
    }
  }
  if (!best)
    throw Error("no checkpoint can be resumed in " + dir_.string() + ": " + why);
  next_ = best->generation == 'a' ? 'b' : 'a'; // the next save replaces the older generation
  return std::move(*best);
}

} // namespace app::checkpoint
