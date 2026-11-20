# kicad-em-fdtd

Real-time 3D electromagnetic wave simulator for KiCAD boards: an FDTD solver of Maxwell's equations (Vulkan GPU, MoltenVK on macOS, plus a CPU reference) with an animated field viewer, packaged as a cross-platform (macOS, Windows, Linux) KiCAD IPC plugin.

Status: working prototype on macOS (Apple silicon, KiCAD 10). A board snapshot goes through the voxelizer to the solver (CPML absorbing boundary, lumped ports) and comes out as S11 (Touchstone) plus an animated field view. The KiCAD plugin entry point exists but is untested inside KiCAD. Windows and Linux builds come later. See [docs/PLAN.md](docs/PLAN.md) for decisions and measurements and [docs/ux.md](docs/ux.md) for the interface.

## Build and run

```
scripts/check.sh                 # build, CPU tests, clang-format, ruff, pytest
cmake --preset release && cmake --build --preset release
build/release/apps/emfdtd/emfdtd SNAPSHOT.json --ui     # the window: setup, settings, run, results
build/release/apps/emfdtd/emfdtd --list-settings        # every setting, with help
```

PCB properties are editable in Settings, PCB: use the board's own stackup or start from a fab profile (OSH Park 2, 4, 6 layers; PCBWay 2, 4 layers) and change any layer's thickness, permittivity or loss tangent. Values a fab does not publish are marked as assumed.

Results go to `<KiCAD project>/em-fdtd/results/`. The plugin only reads the board.

## Versions and releases

Versions follow [Semantic Versioning](https://semver.org) (0.x: anything may change in a minor release). The version lives only in `VERSION`; `emfdtd --version` prints it, and `CHANGELOG.md` lists the changes.

- **CI** (`.github/workflows/ci.yml`): on every push to `main` and every pull request, macOS (Apple silicon) builds with warnings as errors and sanitizers, runs the CPU tests and the format check; Python lint and tests run on Python 3.9 (KiCAD's interpreter). No GPU on the runner, so GPU tests stay local (`scripts/check.sh --gpu`).
- **Release** (`.github/workflows/release.yml`): pushing a tag `vX.Y.Z` (or `vX.Y.Z-rc.1`, published as a pre-release) builds the release preset, runs the tests, packs `emfdtd-X.Y.Z-macos-arm64.tar.gz` with its SHA-256 and publishes a GitHub release whose notes are the changelog section.
- To cut a release: `scripts/version.sh bump 0.2.0`, add a `## [0.2.0]` section to `CHANGELOG.md`, commit, then `git tag v0.2.0 && git push origin v0.2.0`. The workflow refuses a tag that does not match `VERSION` or has no changelog section.
- The archive holds `bin/emfdtd`, `share/emfdtd/{data,shaders,plugin}` and licences; it works wherever it is unpacked. It is **not signed or notarised** and needs the Vulkan loader and MoltenVK (`brew install vulkan-loader molten-vk`). Windows and Linux archives come with those ports.

## Accessibility

The window respects the system's Reduce Motion, Increase Contrast, Reduce Transparency and text-size settings (macOS exercised; Windows and GNOME readers are written but untested). In Settings, View you can override them: Appearance (Auto, Light, Dark), Text size (follow the system or 100 % to 250 %), Reduce motion, High contrast, and the field colours.

**Screen readers.** The window is drawn by our own renderer, so it builds its own accessibility tree (role, name, value and help for every control) and hands it to the operating system through [AccessKit](https://github.com/AccessKit/accesskit-c). VoiceOver on macOS is wired up; Windows (Narrator, NVDA) and Linux (Orca) need only an adapter branch in `src/viz/screen_reader.cpp`. A screen reader can press controls and set setting values. Moving its cursor does not yet move keyboard focus, and sliders have no increment and decrement actions.

**Keyboard.** Keyboard navigation is on, using Dear ImGui's standard keys (not yet tried by hand in the window; please report anything that does not work):

| Key | Does |
|---|---|
| Arrow keys | move focus between neighbouring controls (Settings, tab strip, buttons) |
| Space or Enter | press a button, toggle a checkbox, open a drop-down (arrows pick, Enter confirms) |
| Esc | close a drop-down or leave a text field |
| Tab, Shift+Tab | step through text fields |
| Ctrl+Tab | cycle panels; with the tab strip focused, switch tabs |
| Ctrl + arrows, or Ctrl+click | adjust a focused slider, or type its value |

The field slice and 3D volume cannot be panned or rotated from the keyboard yet. With a pointer: drag rotates the 3D view, right-drag or Shift+drag pans it, the wheel zooms, double-click refits; in the field slice drag pans and the wheel zooms.

**Checking it.** `emfdtd SNAPSHOT.json --ui-screenshot out.png --ui-a11y-dump tree.txt` writes what a screen reader is told and exits 1 if any control has no name. Not yet tried with VoiceOver itself (Cmd+F5 on macOS).

License: MIT (see LICENSE). Dependency licences, including AccessKit, are listed in docs/PLAN.md.
