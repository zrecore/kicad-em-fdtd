// Part of the canary for scripts/tidy.sh (see canary.cpp). A finding here must be reported: it proves that the
// header filter lets our own headers through. Not built; do not "fix" it.
#pragma once
inline int canaryHeader(int x) {
  if (x > 0) return 1;
  return 0;
}
