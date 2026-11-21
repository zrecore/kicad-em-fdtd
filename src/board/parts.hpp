// Reading a part's value field: resistance in ohm from the forms found on schematics. Pure text, tested.
#pragma once
#include <cctype>
#include <cmath>
#include <optional>
#include <string>

namespace board {

// "0R", "10R", "4k7", "4.7k", "1M", "47", "220 ohm", "0R0", "10 Ω" (case of the unit letter ignored).
// R, k, M, G double as the decimal point (RKM code: 4k7 = 4.7 kohm, 2R2 = 2.2 ohm). A word after the number
// and a space ("10k 1%") is ignored. nullopt if it is not a plain resistance.
inline std::optional<double> parseResistance(const std::string& text) {
  // first word, plus a unit word right after it ("220 ohm"); anything later ("1%", "0603") is ignored
  std::string t;
  {
    std::size_t i = 0;
    while (i < text.size() && text[i] == ' ')
      ++i;
    while (i < text.size() && text[i] != ' ')
      t += text[i++];
    while (i < text.size() && text[i] == ' ')
      ++i;
    const std::string rest = text.substr(i);
    for (const char* unit : {"ohms", "ohm", "\u03a9", "\u2126"})
      if (rest.rfind(unit, 0) == 0 &&
          (rest.size() == std::string(unit).size() || rest[std::string(unit).size()] == ' '))
        t += unit;
  }
  for (const char* unit : {"ohms", "ohm", "Ω", "Ω"}) {
    const std::string u = unit;
    if (t.size() > u.size() && t.compare(t.size() - u.size(), u.size(), u) == 0) {
      t.resize(t.size() - u.size());
      break;
    }
  }
  if (t.empty() || !std::isdigit(static_cast<unsigned char>(t[0])))
    return std::nullopt;
  double scale = 1;
  std::string num;
  bool seenMark = false;
  for (char ch : t) {
    const char u = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (std::isdigit(static_cast<unsigned char>(ch)) ||
        (ch == '.' && !seenMark && num.find('.') == std::string::npos)) {
      num += ch;
    } else if ((u == 'R' || u == 'K' || u == 'M' || u == 'G') && !seenMark) {
      seenMark = true;
      scale = u == 'R' ? 1.0 : u == 'K' ? 1e3 : u == 'M' ? 1e6 : 1e9;
      if (num.find('.') == std::string::npos)
        num += '.'; // 4k7: the mark is the decimal point
    } else {
      return std::nullopt;
    }
  }
  if (num.empty() || num == ".")
    return std::nullopt;
  try {
    const double v = std::stod(num);
    return std::isfinite(v) ? std::optional<double>(v * scale) : std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

// "1pF", "100nF", "0.1u", "4p7", "2n2", "10 pF" in farad (RKM-style p, n, u as the decimal point too). A bare
// number, an H (inductance), an R or an unknown word gives nullopt. The prefix letter (p, n, u, \u00b5) is
// required: "100" alone could be anything.
inline std::optional<double> parseCapacitance(const std::string& text) {
  std::string t;
  {
    std::size_t i = 0;
    while (i < text.size() && text[i] == ' ')
      ++i;
    while (i < text.size() && text[i] != ' ')
      t += text[i++];
    while (i < text.size() && text[i] == ' ')
      ++i;
    const std::string rest = text.substr(i);
    const std::size_t word = rest.find(' ');
    const std::string next = rest.substr(0, word);
    if (!next.empty() &&
        (next[0] == 'p' || next[0] == 'P' || next[0] == 'n' || next[0] == 'N' || next[0] == 'u' ||
         next[0] == 'U' || next.rfind("\u00b5", 0) == 0) &&
        std::isdigit(static_cast<unsigned char>(t.back())))
      t += next;
  }
  if (!t.empty() && (t.back() == 'F' || t.back() == 'f'))
    t.pop_back();
  if (t.empty() || !std::isdigit(static_cast<unsigned char>(t[0])))
    return std::nullopt;
  double scale = 0;
  std::string num;
  for (std::size_t i = 0; i < t.size(); ++i) {
    const char ch = t[i];
    if (std::isdigit(static_cast<unsigned char>(ch)) ||
        (ch == '.' && num.find('.') == std::string::npos && scale == 0)) {
      num += ch;
      continue;
    }
    if (scale != 0)
      return std::nullopt;
    if (ch == 'p' || ch == 'P')
      scale = 1e-12;
    else if (ch == 'n' || ch == 'N')
      scale = 1e-9;
    else if (ch == 'u' || ch == 'U')
      scale = 1e-6;
    else if (t.compare(i, 2, "\u00b5") == 0) {
      scale = 1e-6;
      ++i;
    } else
      return std::nullopt;
    if (num.find('.') == std::string::npos)
      num += '.';
  }
  if (scale == 0 || num.empty() || num == ".")
    return std::nullopt;
  try {
    const double v = std::stod(num);
    return std::isfinite(v) ? std::optional<double>(v * scale) : std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

// True if the value field itself says "resistance": an R, k, M or G mark (4k7, 0R), an ohm word or sign, or
// a plain zero. A bare number ("47") says nothing without the reference prefix, so it is not enough alone.
// The value wins over the reference: an inductor footprint (L3) with the value 0R is a 0 ohm link.
inline bool valueSaysResistance(const std::string& text) {
  const auto v = parseResistance(text);
  if (!v)
    return false;
  if (*v == 0)
    return true;
  for (char ch : text.substr(0, text.find(' '))) {
    const char u = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (u == 'R' || u == 'K' || u == 'M' || u == 'G')
      return true;
  }
  return text.find("ohm") != std::string::npos || text.find("\u03a9") != std::string::npos ||
         text.find("\u2126") != std::string::npos;
}

// The letters at the start of a reference designator: "R12" -> "R", "FB3" -> "FB", "JP1" -> "JP".
inline std::string refPrefix(const std::string& ref) {
  std::string p;
  for (char ch : ref) {
    if (!std::isalpha(static_cast<unsigned char>(ch)))
      break;
    p += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  return p;
}

} // namespace board
