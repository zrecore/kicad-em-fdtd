// Reading a part's value field: resistance in ohm from the forms found on schematics. Pure text, tested.
#pragma once
#include <cctype>
#include <cmath>
#include <optional>
#include <string>

#include "board/snapshot.hpp"

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

// Capacitance in farad or inductance in henry from a value field: "1pF", "100nF", "0.1u", "4p7", "2n2",
// "10 pF" and "2.2nH", "10 nH", "1uH", "4n7H" (RKM-style p, n, u as the decimal point too; m is milli for
// henry only). The prefix letter is required: "100" alone could be anything. A capacitance may leave the F
// off ("22p"); an inductance must keep the H ("10n" is a capacitance, never an inductance). Anything else
// (a bare number, the other unit, a resistance, a part number) gives nullopt.
inline std::optional<double> parseSI(const std::string& text, char unit, bool unitRequired, bool allowMilli) {
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
    if (!next.empty() && !t.empty() && std::isdigit(static_cast<unsigned char>(t.back())) &&
        (next[0] == 'p' || next[0] == 'P' || next[0] == 'n' || next[0] == 'N' || next[0] == 'u' ||
         next[0] == 'U' || (allowMilli && next[0] == 'm') || next.rfind("\u00b5", 0) == 0))
      t += next;
  }
  if (!t.empty() && std::toupper(static_cast<unsigned char>(t.back())) == unit)
    t.pop_back();
  else if (unitRequired)
    return std::nullopt;
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
    else if (allowMilli && ch == 'm')
      scale = 1e-3;
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
inline std::optional<double> parseCapacitance(const std::string& text) {
  return parseSI(text, 'F', false, false);
}
inline std::optional<double> parseInductance(const std::string& text) {
  return parseSI(text, 'H', true, true);
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

// What to put between the two pads of a two-terminal part. Auto reads the part's value field (a resistance,
// a capacitance or an inductance; the value wins over the reference letter) and leaves a do-not-populate
// part open. The other states are the user's: Open (not fitted), Short (a 0 ohm link) or Values (a parallel
// R, L, C with the given values, any subset: a ferrite bead is all three). fitDnp fits a DNP part by value.
struct PartTreatment {
  enum class State { Auto, Open, Short, Values } state = State::Auto;
  bool fitDnp = false;
  std::optional<double> r, l, c; // ohm, henry, farad (Values only)
};

// What the model puts between a two-terminal part's pads: nothing (skip: the part is not modellable and is
// reported as not modelled), an open circuit on purpose, or R, L, C in parallel (a short for 0 ohm).
struct PartModel {
  enum class Kind { Unmodelled, Open, Lumped } kind = Kind::Unmodelled;
  std::optional<double> r, l, c;
  bool shortIt = false;
  std::string asRead; // "resistance", "capacitance", "inductance", "short, set by the user", ...
  std::string note;   // a note for the run summary, or empty
};

inline bool isTwoTerminal(const Component& c) {
  return c.pads.size() == 2 && c.pads[0].net != c.pads[1].net;
}

// Decide a two-terminal part's treatment from the part and the user's PartTreatment (the value wins over the
// reference letter; a do-not-populate part is open unless fitDnp or an explicit state says otherwise).
inline PartModel modelPart(const Component& c, const PartTreatment& t) {
  using State = PartTreatment::State;
  PartModel m;
  if (t.state == State::Open) {
    m.kind = PartModel::Kind::Open;
    m.note = "component " + c.ref + ": left open as set by the user";
    return m;
  }
  if (t.state == State::Auto && c.dnp && !t.fitDnp) {
    m.kind = PartModel::Kind::Open; // reported by the caller as do not populate
    return m;
  }
  if (t.state == State::Short) {
    m.kind = PartModel::Kind::Lumped;
    m.shortIt = true;
    m.asRead = "short, set by the user";
    return m;
  }
  if (t.state == State::Values) {
    m.r = t.r, m.l = t.l, m.c = t.c;
    if (!m.r && !m.l && !m.c) {
      m.kind = PartModel::Kind::Open;
      m.note = "component " + c.ref + ": the user gave no value, left open";
      return m;
    }
    m.kind = PartModel::Kind::Lumped;
    m.asRead = "values set by the user";
    m.shortIt = m.r && *m.r == 0 && !m.l && !m.c;
    return m;
  }
  const bool byValue = valueSaysResistance(c.value), byPrefix = refPrefix(c.ref) == "R";
  if (byValue) {
    m.r = parseResistance(c.value);
    m.asRead = "resistance";
  } else if ((m.c = parseCapacitance(c.value))) {
    m.asRead = "capacitance";
  } else if ((m.l = parseInductance(c.value))) {
    m.asRead = "inductance";
  } else if (byPrefix) { // a bare number on an R reference is ohms
    m.r = parseResistance(c.value);
    if (!m.r) {
      m.kind = PartModel::Kind::Open;
      m.note = "component " + c.ref + ": value '" + c.value + "' is not a resistance, left open";
      return m;
    }
    m.asRead = "resistance";
  } else {
    return m; // a bead, a part number or an unreadable value
  }
  m.kind = PartModel::Kind::Lumped;
  m.shortIt = m.r && *m.r == 0;
  const std::string want = m.asRead == "capacitance" ? "C" : m.asRead == "inductance" ? "L" : "R";
  if (refPrefix(c.ref) != want)
    m.note = "component " + c.ref + ": the reference says " + refPrefix(c.ref) + " but the value '" +
             c.value + "' is a " + m.asRead + ", so it is modelled as that";
  return m;
}

} // namespace board
