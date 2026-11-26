#!/bin/sh
# The window with the operating system's UI font (the default; ui_farm_check.sh pins the built-in font so its click
# positions do not depend on the machine): the Preview and Farm tabs must render at 720x520 and 720x900 at 100 % and
# 150 % text size, every control must have a name (emfdtd exits 1 otherwise), and a font that is wider than the
# built-in one must not push the screenshots out of their size. Looking at the pictures is still a human job:
# they are kept in out/ui-font/.
#   scripts/ui_font_check.sh [path/to/emfdtd]
set -eu
cd "$(dirname "$0")/.."
BIN=${1:-build/release/apps/emfdtd/emfdtd}
OUT=out/ui-font
rm -rf "$OUT/farm-home" "$OUT/project"
mkdir -p "$OUT/project" "$OUT/farm-home"
python3 plugin/tools/make_patch_snapshot.py "$OUT/patch.json" >/dev/null
export EMFDTD_FARM_COMMAND="$PWD/tests/farm_stub/farm_ui_stub.sh"
export EMFDTD_FARM_HOME="$PWD/$OUT/farm-home"
echo '{"enabled":{"coordinator":true,"worker":false,"client":true}}' > "$OUT/farm-home/farm.json"
export EMFDTD_UI_FONT=system
run() { # name tab size extra-args...
  name=$1; tab=$2; size=$3; shift 3
  "$BIN" "$OUT/patch.json" --project-dir "$OUT/project" --profile wifi --set mesh_preset=coarse --cpu \
    --ui-tab "$tab" --ui-size "$size" --ui-screenshot "$OUT/$name.png" --ui-a11y-dump "$OUT/$name.txt" "$@" >/dev/null
  size_of "$OUT/$name.png" "${size%x*}" "$name"
}
size_of() { # png width name: the picture must be as wide as the window asked for
  w=$(python3 -c "import struct,sys; d=open(sys.argv[1],'rb').read(24); print(struct.unpack('>I', d[16:20])[0])" "$1")
  [ "$w" = "$2" ] || { echo "ui_font_check: $3 is $w pixels wide, not $2" >&2; exit 1; }
}
need() { # file pattern what
  grep -q "$2" "$OUT/$1.txt" || { echo "ui_font_check: missing in the accessibility dump of $1: $3" >&2; exit 1; }
}
for scale in 100% 150%; do
  tag=$(echo "$scale" | tr -d %)
  run "preview_$tag" Preview 720x520 --set "text_scale=$scale"
  need "preview_$tag" 'button "Run simulation"' "the Run button at $scale"
  need "preview_$tag" 'tab "Preview"' "the tabs at $scale"
  run "farm_$tag" Farm 720x900 --set "text_scale=$scale"
  need "farm_$tag" 'tab "Farm"' "the Farm tab at $scale"
  need "farm_$tag" 'checkbox \[x\] "Coordinator"' "the role switches at $scale"
  need "farm_$tag" 'button "Also use this computer.s GPU for the farm"' "the one-click option at $scale"
done
# The setting itself: the built-in font renders too, and the two differ.
EMFDTD_UI_FONT=builtin run preview_builtin Preview 720x520
if cmp -s "$OUT/preview_100.png" "$OUT/preview_builtin.png"; then
  echo "ui_font_check: the system font and the built-in font gave the same picture" >&2
  exit 1
fi
echo "ui_font_check: ok"
