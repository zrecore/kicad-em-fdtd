// Where the program's read-only files (data/*.json, compiled shaders) are. A release package keeps them in
// <prefix>/share/emfdtd/{data,shaders} next to <prefix>/bin/emfdtd; a build tree has them at the absolute
// paths baked in at configure time. The installed copy wins when it exists, so a package works wherever it is
// unpacked and a build tree keeps working untouched.
#pragma once
#include <filesystem>
#include <string>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace core {

inline std::filesystem::path executablePath() {
  namespace fs = std::filesystem;
  std::error_code ec;
#if defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buf(size, '\0');
  if (_NSGetExecutablePath(buf.data(), &size) != 0)
    return {};
  return fs::canonical(fs::path(buf.c_str()), ec);
#elif defined(_WIN32)
  std::wstring buf(MAX_PATH, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  buf.resize(n);
  return fs::path(buf);
#else
  return fs::canonical("/proc/self/exe", ec);
#endif
}

// `sub` is "data" or "shaders"; `buildTreeDir` is the configure-time absolute path.
inline std::string resourceDir(const char* sub, const char* buildTreeDir) {
  namespace fs = std::filesystem;
  const fs::path exe = executablePath();
  if (!exe.empty()) {
    const fs::path installed = exe.parent_path().parent_path() / "share" / "emfdtd" / sub;
    std::error_code ec;
    if (fs::is_directory(installed, ec))
      return installed.string();
  }
  return buildTreeDir;
}

} // namespace core
