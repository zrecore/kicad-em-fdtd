# KiCAD plugin (IPC API)

Adds a **Simulate (EM FDTD)** button to the PCB editor. It exports the open board (read-only), defines the port from the two selected pads (first selected = positive), and starts the solver window (`emfdtd`) on its preview screen. Results go to `<project>/em-fdtd/results/run-<time>/`. The export follows the board outline as drawn on Edge.Cuts (arcs, cutouts, via spans); when the board sits inside a panel, the exporter also finds the board between the slots and breakaway tabs and writes it as `board_proper`, which the window models by default (setting "Board outline").

Two more toolbar buttons: **EM FDTD window** opens the solver window on the preview without a run, and **EM FDTD farm** opens it on the Farm tab (GPU farm: off until you turn it on there; see `docs/farm.md`).

## Install (macOS, KiCAD 10)
1. `plugin/tools/install.sh --link` in Terminal (recommended while developing: a symlink, so edits show up after Tools, External Plugins, Refresh Plugins). Or `plugin/tools/install.sh` without `--link`, which copies the plugin to `~/Documents/KiCad/10.0/plugins/org.kicad-em-fdtd.simulate` and writes `solver_path.txt` (the solver binary). With a copy, re-run it after every change. Terminal needs access to the Documents folder (System Settings, Privacy and Security, Files and Folders). A symbolic link does work: the earlier failure was missing toolbar icons, not the link.
2. Build the solver first: `cmake --preset release && cmake --build --preset release`.
3. Start KiCAD, open the board in the PCB editor, then **Tools, External Plugins, Refresh Plugins**. The button appears at the end of the top toolbar (the toolbar's "IPC/Scripting plugins" section). **Preferences, PCB Editor, Plugins** has a "Show Button" box per action.
4. Select two pads (for the W3008 test board: J1 pad 1 RF and pad 2 GND), click the button.

## What we learned (2026-11-19, KiCAD 10.0.7)
- KiCAD draws a plugin's toolbar button **only if the action has icons** (`icons-light` / `icons-dark` PNGs in `plugin.json`); without them the action loads and no button appears (`AddApiPluginTools` skips it). `tools/make_icons.py` generates them (24 and 48 pixels; a 32 pixel bitmap renders too large).
- Plugins are loaded from `~/Documents/KiCad/10.0/plugins/<identifier>/plugin.json` (user path). `3rdparty` is for the package manager.
- If a plugin does not show up, start KiCAD with tracing and read the output: `KICAD_ENABLE_WXTRACE=1 WXTRACE=KICAD_API /Applications/KiCad/KiCad.app/Contents/MacOS/kicad PROJECT.kicad_pro`. The trace shows scanning, schema validation, "loaded action" and "marking ... as ready".
- A forum report says plugins may not be found when KiCAD is started from the app icon on macOS (10.0.6); starting it from Terminal worked for us. Not yet re-tested from the Dock after the icons were added.
- `plugin.json` is checked against `KiCad.app/Contents/SharedSupport/schemas/api.v1.schema.json`; the identifier must be reverse-DNS with at least two parts after the first (`org.kicad-em-fdtd.simulate`).

## Tests
`cd plugin && .venv/bin/python -m pytest` (geometry and snapshot checks, no KiCAD needed); the exporter and the action itself are exercised against a running KiCAD.

The venv must be built on a Python with TLS 1.3 (here Homebrew `python@3.12`; the Xcode Python is linked to LibreSSL and has none): the job farm in `src/emfdtd_plugin/farm/` requires TLS 1.3 and its tests fail, not skip, without it. They also need the `openssl` command (the coordinator's certificate tool). The plugin itself stays Python 3.9 compatible (ruff lints for 3.9). The farm and its command line (`tools/emfdtd_farm.py`) are optional and off by default: see `docs/farm.md`.
