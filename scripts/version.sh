#!/bin/sh
# Semantic versioning helper. The version lives in VERSION; plugin/pyproject.toml follows it.
#   scripts/version.sh show            print the version
#   scripts/version.sh check [TAG]     fail unless VERSION is valid semver, pyproject.toml matches, and (when
#                                      given) TAG is v<VERSION> and CHANGELOG.md has a section for it
#   scripts/version.sh bump X.Y.Z[-pre]  write VERSION and pyproject.toml (you commit and tag yourself)
#   scripts/version.sh notes           print this version's CHANGELOG section (for the release notes)
set -eu
cd "$(dirname "$0")/.."
SEMVER='^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$'
version() { head -n 1 VERSION | tr -d ' \r\n'; }
py_version() { sed -n 's/^version = "\(.*\)"/\1/p' plugin/pyproject.toml | head -n 1; }
# PEP 440 spelling of a semver: 1.2.3-rc.1 -> 1.2.3rc1 (plain releases are identical).
py_of() { echo "$1" | sed -e 's/-rc\.\{0,1\}/rc/' -e 's/-alpha\.\{0,1\}/a/' -e 's/-beta\.\{0,1\}/b/'; }
case "${1:-show}" in
show) version ;;
check)
  v=$(version)
  echo "$v" | grep -Eq "$SEMVER" || { echo "VERSION '$v' is not semver (X.Y.Z or X.Y.Z-pre)" >&2; exit 1; }
  [ "$(py_version)" = "$(py_of "$v")" ] || { echo "plugin/pyproject.toml has $(py_version), expected $(py_of "$v")" >&2; exit 1; }
  if [ -n "${2:-}" ]; then
    [ "$2" = "v$v" ] || { echo "tag $2 does not match VERSION v$v" >&2; exit 1; }
    grep -q "^## \[$v\]" CHANGELOG.md || { echo "CHANGELOG.md has no '## [$v]' section" >&2; exit 1; }
  fi
  echo "version ok: $v" ;;
bump)
  v="${2:?usage: version.sh bump X.Y.Z}"
  echo "$v" | grep -Eq "$SEMVER" || { echo "'$v' is not semver" >&2; exit 1; }
  echo "$v" > VERSION
  sed -i.bak "s/^version = \".*\"/version = \"$(py_of "$v")\"/" plugin/pyproject.toml && rm plugin/pyproject.toml.bak
  echo "now $v. Add a '## [$v]' section to CHANGELOG.md, commit, then: git tag v$v && git push origin v$v" ;;
notes)
  v=$(version)
  awk -v h="## [$v]" 'index($0,h)==1{on=1;next} /^## \[/{on=0} on' CHANGELOG.md ;;
*) echo "usage: version.sh show|check [TAG]|bump X.Y.Z|notes" >&2; exit 2 ;;
esac
