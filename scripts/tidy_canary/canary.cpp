// Canary for scripts/tidy.sh: NOT built and not part of the sources. It has a known problem (an if without
// braces) that readability-braces-around-statements reports, and so does the header it includes. If clang-tidy
// says nothing about either file the check or the header filter is not working and tidy.sh fails.
// Do not "fix" these files.
#include "canary_header.hpp"

int canary(int x) {
  if (x > 0) return canaryHeader(x);
  return 0;
}
