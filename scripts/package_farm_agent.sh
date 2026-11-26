#!/bin/sh
# Builds the farm worker bundle: everything a machine needs to join the GPU farm, without this repository.
#   scripts/package_farm_agent.sh        writes out/dist/emfdtd-farm-agent-<VERSION>-<YYYYMMDD>.tar.gz (and .zip if
#                                        `zip` exists) plus its SHA256SUMS, then checks the bundle by unpacking it
#                                        and running it on its own. Exits non-zero if anything is wrong.
# The bundle holds only the Python standard-library farm package, the launchers, the docs and the licence.
set -eu
cd "$(dirname "$0")/.."
VERSION=$(head -n 1 VERSION | tr -d ' \r\n')
STAMP=$(date +%Y%m%d)
NAME=emfdtd-farm-agent-$VERSION-$STAMP
DIST=out/dist
STAGE=$DIST/stage/$NAME
fail() { echo "package_farm_agent: FAILED: $*" >&2; exit 1; }
[ -n "$VERSION" ] || fail "VERSION is empty"
mkdir -p "$DIST"
rm -rf "${DIST:?}/stage" "${DIST:?}/verify" "${DIST:?}/verify-home"
mkdir -p "$STAGE/src/emfdtd_plugin/farm" "$STAGE/tools" "$STAGE/docs"

# --- what goes in -------------------------------------------------------------------------------------------
cp plugin/src/emfdtd_plugin/__init__.py "$STAGE/src/emfdtd_plugin/"
for f in plugin/src/emfdtd_plugin/farm/*.py; do cp "$f" "$STAGE/src/emfdtd_plugin/farm/"; done
mkdir -p "$STAGE/src/emfdtd_plugin/farm/gui"
for f in plugin/src/emfdtd_plugin/farm/gui/*.py; do cp "$f" "$STAGE/src/emfdtd_plugin/farm/gui/"; done
cp plugin/tools/emfdtd_farm.py plugin/tools/emfdtd-farm "$STAGE/tools/"
# The Windows launcher needs CRLF line endings (cmd.exe mishandles labels in LF-only files).
sed 's/\r$//; s/$/\r/' plugin/tools/emfdtd-farm.cmd >"$STAGE/tools/emfdtd-farm.cmd"
cp docs/farm.md docs/farm-setup.md "$STAGE/docs/"
cp LICENSE "$STAGE/"
cat >"$STAGE/emfdtd-farm" <<'SH'
#!/bin/sh
exec "$(cd "$(dirname "$0")" && pwd)/tools/emfdtd-farm" "$@"
SH
printf '@echo off\r\ncall "%%~dp0tools\\emfdtd-farm.cmd" %%*\r\nexit /b %%ERRORLEVEL%%\r\n' >"$STAGE/emfdtd-farm.cmd"
chmod +x "$STAGE/emfdtd-farm" "$STAGE/tools/emfdtd-farm"
cat >"$STAGE/README-FIRST.txt" <<EOF
emfdtd GPU farm worker, version $VERSION ($STAMP)

This folder is everything a machine needs to take part in an emfdtd GPU farm: the farm program (plain
Python, no packages to install), the launchers, and the guide.

  1. Read docs/farm-setup.md. It lists what you need (Python with TLS 1.3, the emfdtd program for your
     machine, a Vulkan GPU) and every step, in order, with what to expect on screen.
  2. Check the files arrived intact: compare them with SHA256SUMS.
  3. Smoke test:  ./emfdtd-farm status        (Windows: emfdtd-farm.cmd status)

The farm is off until you turn it on. Nothing here runs by itself and nothing here opens a port.
EOF

# --- checksums, archives ------------------------------------------------------------------------------------
if command -v sha256sum >/dev/null 2>&1; then SUM="sha256sum"; else SUM="shasum -a 256"; fi
(cd "$STAGE" && find . -type f ! -name SHA256SUMS | sed 's#^\./##' | LC_ALL=C sort | while read -r f; do $SUM "$f"; done >SHA256SUMS)
rm -f "${DIST:?}/${NAME:?}.tar.gz" "${DIST:?}/${NAME:?}.zip" "${DIST:?}/${NAME:?}.SHA256SUMS"
(cd "$DIST/stage" && tar -czf "../$NAME.tar.gz" "$NAME")
if command -v zip >/dev/null 2>&1; then (cd "$DIST/stage" && zip -r -q "../$NAME.zip" "$NAME"); fi
(cd "$DIST" && for f in "$NAME".tar.gz "$NAME".zip; do if [ -f "$f" ]; then $SUM "$f"; fi; done >"$NAME.SHA256SUMS")

# --- verify: unpack it somewhere else and run it on its own -----------------------------------------------
VERIFY=$DIST/verify
mkdir -p "$VERIFY" "$DIST/verify-home"
tar -xzf "$DIST/$NAME.tar.gz" -C "$VERIFY"
B=$(cd "$VERIFY/$NAME" && pwd)
HOME_DIR=$(cd "$DIST/verify-home" && pwd)

(cd "$B" && if command -v sha256sum >/dev/null 2>&1; then sha256sum -c SHA256SUMS >/dev/null; else shasum -a 256 -c SHA256SUMS >/dev/null; fi) \
  || fail "SHA256SUMS does not match the unpacked files"
# only the intended files, nothing else
EXTRA=$(cd "$B" && find . -type f | sed 's#^\./##' | grep -v -E '^(SHA256SUMS|LICENSE|README-FIRST\.txt|emfdtd-farm|emfdtd-farm\.cmd|docs/farm\.md|docs/farm-setup\.md|tools/emfdtd_farm\.py|tools/emfdtd-farm|tools/emfdtd-farm\.cmd|src/emfdtd_plugin/__init__\.py|src/emfdtd_plugin/farm/[a-z_]+\.py|src/emfdtd_plugin/farm/gui/[a-z_]+\.py)$' || true)
[ -z "$EXTRA" ] || fail "unexpected files in the bundle: $EXTRA"
if find "$B" -name __pycache__ | grep -q .; then fail "the bundle contains __pycache__"; fi

PY=$(env -u PYTHONPATH "$B/tools/emfdtd-farm" --which-python) || fail "no Python with TLS 1.3 found for the check (see the launcher's message)"
# Importing the package alone: -I ignores PYTHONPATH and the user site; only the bundle's src is added.
env -u PYTHONPATH EMFDTD_FARM_HOME="$HOME_DIR" "$PY" -I -c "
import os, sys, sysconfig
before = set(sys.modules)  # what the interpreter itself loaded at start-up (site .pth files): not our business
sys.path.insert(0, r'$B/src')
import emfdtd_plugin.farm, emfdtd_plugin.farm.cli, emfdtd_plugin.farm.worker, emfdtd_plugin.farm.coordinator
bundle = os.path.realpath(r'$B')
stdlib = {os.path.realpath(sysconfig.get_paths()[k]) for k in ('stdlib', 'platstdlib')}
outside = []
for name, mod in sorted(sys.modules.items()):
    if name in before:
        continue
    f = getattr(mod, '__file__', None)
    if not f:
        continue
    f = os.path.realpath(f)
    if name.split('.')[0] == 'emfdtd_plugin':
        if not f.startswith(bundle + os.sep):
            outside.append((name, f))
    elif not any(f.startswith(s + os.sep) for s in stdlib) and 'lib-dynload' not in f:
        outside.append((name, f))
if outside:
    print('imported from outside the bundle:', outside)
    sys.exit(1)
print('imports ok: only the standard library and the bundle')
" || fail "the farm package imports something from outside the bundle"
OUT=$(env -u PYTHONPATH EMFDTD_FARM_HOME="$HOME_DIR" "$B/emfdtd-farm" status) || fail "emfdtd-farm status failed"
echo "$OUT" | grep -q "farm state folder: $HOME_DIR" || fail "status did not use the scratch farm folder: $OUT"
echo "$OUT" | grep -q "worker .* off" || fail "status does not show the worker role off: $OUT"
head -c 10 "$B/emfdtd-farm.cmd" | grep -q "@echo off" || fail "the root Windows launcher is damaged"
for cmdfile in "$B/tools/emfdtd-farm.cmd" "$B/emfdtd-farm.cmd"; do
  [ "$(grep -c -v "$(printf '\r')\$" "$cmdfile" || true)" = 0 ] || fail "every line of $cmdfile must end in CRLF"
done
if [ -f "$DIST/$NAME.zip" ] && command -v unzip >/dev/null 2>&1; then
  unzip -l "$DIST/$NAME.zip" | grep -q "$NAME/tools/emfdtd-farm.cmd" || fail "the zip lacks the Windows launcher"
fi
rm -rf "${DIST:?}/verify" "${DIST:?}/verify-home" "${DIST:?}/stage"
echo "package_farm_agent: ok"
ls -l "$DIST/$NAME".* | awk '{print "  " $5 " bytes  " $NF}'
