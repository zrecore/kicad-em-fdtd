#!/bin/sh
# Install the KiCAD plugin into KiCAD's user plugin folder.
#   plugin/tools/install.sh --link [KICAD_PLUGIN_DIR]   symlink to this checkout (edits show up after a Refresh Plugins)
#   plugin/tools/install.sh [KICAD_PLUGIN_DIR]          copy (re-run after every change)
# A symlink works: KiCAD follows it, and the plugin finds the solver and the defaults through the real path. The
# earlier "plugin not found" problem was missing toolbar icons, not the link.
set -eu
here="$(cd "$(dirname "$0")/.." && pwd)"
repo="$(cd "$here/.." && pwd)"
mode=copy
if [ "${1:-}" = "--link" ]; then mode=link; shift; fi
dir="${1:-$HOME/Documents/KiCad/10.0/plugins}"
dest="$dir/org.kicad-em-fdtd.simulate"
mkdir -p "$dir"
if [ "$mode" = link ]; then
  rm -rf "$dest"
  ln -s "$here" "$dest"
  echo "linked $dest -> $here"
else
  [ -L "$dest" ] && rm "$dest"
  mkdir -p "$dest"
  rsync -a --delete --exclude .venv --exclude __pycache__ --exclude tests --exclude probe --exclude .pytest_cache --exclude .ruff_cache "$here/" "$dest/"
  mkdir -p "$dest/data"
  cp "$repo/data/fab_profiles.json" "$dest/data/fab_profiles.json" # single source of material and fab defaults
  printf '%s\n' "$repo/build/release/apps/emfdtd/emfdtd" > "$dest/solver_path.txt"
  echo "copied to $dest (solver: $(cat "$dest/solver_path.txt"))"
fi
