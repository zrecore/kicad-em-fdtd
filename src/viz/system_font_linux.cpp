// The desktop's UI font on Linux and other Unix systems: the GNOME interface font if gsettings says one,
// asked of fontconfig for its file; else fontconfig's "sans". Not exercised on the machine this was written
// on.
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "viz/system_font.hpp"

namespace viz {
namespace {

std::string run(const std::string& command) {
  std::string out;
  if (FILE* p = popen(command.c_str(), "r")) {
    char buf[512];
    while (std::fgets(buf, sizeof buf, p))
      out += buf;
    pclose(p);
  }
  return out;
}

// "'Cantarell 11'" (gsettings) to "Cantarell"; empty if it is not a plain family name.
std::string familyFromGsettings(std::string s) {
  const auto a = s.find('\''), b = s.rfind('\'');
  if (a == std::string::npos || b <= a)
    return {};
  s = s.substr(a + 1, b - a - 1);
  while (!s.empty() &&
         (std::isdigit(static_cast<unsigned char>(s.back())) || s.back() == ' ' || s.back() == '.'))
    s.pop_back();
  for (char c : s)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != ' ' && c != '-')
      return {}; // never hand a surprising string to a shell
  return s;
}

} // namespace

std::optional<SystemFont> systemUiFont() {
  std::string family =
      familyFromGsettings(run("gsettings get org.gnome.desktop.interface font-name 2>/dev/null"));
  if (family.empty())
    family = "sans";
  const std::string reply = run("fc-match -f '%{file}\\n%{index}\\n' '" + family + "' 2>/dev/null");
  const auto nl = reply.find('\n');
  if (nl == std::string::npos || nl == 0)
    return std::nullopt;
  SystemFont font;
  font.path = reply.substr(0, nl);
  font.index = std::atoi(reply.c_str() + nl + 1);
  font.name = family;
  if (firstExistingFile({font.path.c_str()}).empty())
    return std::nullopt;
  return font;
}

} // namespace viz
