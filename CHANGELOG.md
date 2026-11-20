# Changelog

All notable changes are listed here. The project follows [Semantic Versioning](https://semver.org): while the
major version is 0, anything may change in a minor release. Format: [Keep a Changelog](https://keepachangelog.com).

## [Unreleased]

### Added (credits)
- `CREDITS.md`; release archives ship it with the licence texts of ImGui, GLFW, nlohmann/json and AccessKit (`share/doc/emfdtd/licenses/`).

### Changed
- The Settings panel starts collapsed (the button reads "Settings >>"; open it to read "<< Hide settings"). For screenshots, `--ui-hide-settings` became `--ui-show-settings`.
- 3D volume: right-drag, middle-drag or Shift+drag pans; double-click refits the view. The camera's look-at point is now honoured by the renderer (auto-fit follows the field's activity).

### Fixed
- The 3D view of a board with hundreds of copper shapes drew huge grey triangles and stray lines over the window. Two causes: ImGui draw lists use 16-bit indices and our backend never enabled vertex offsets (indices wrapped past 65 535 vertices), and the CPU rasteriser's clip rectangle ignored the display scale on its left and top edges (Retina). Copper fills now use our own ear-clipping triangulation instead of ImGui's concave fill.

### Added
- PCB properties are settings (group PCB): choose the board's own stackup or a fab profile (OSH Park 2, 4, 6 layers; PCBWay 2, 4 layers; ENIG FR4-class) and edit each dielectric layer's thickness, permittivity and loss tangent. Values the fab does not publish are marked assumed in `data/fab_profiles.json` and in the run notes.

## [0.1.0]

First tagged version: a working prototype on macOS (Apple silicon).

### Added
- FDTD solver (CPU reference and Vulkan GPU) with CPML absorbing boundary and lumped ports; S11 (Touchstone) output.
- Board snapshot format, voxelizer with a graded grid, component selection, radio and fab profiles.
- Standalone window (`emfdtd --ui`): setup, settings, run, results, field slice and 3D volume views.
- Theme (Auto, Light, Dark) with configurable field colours; system Reduce Motion, Increase Contrast, Reduce
  Transparency and text size respected.
- Screen reader support on macOS (AccessKit) and keyboard navigation.
- KiCAD IPC plugin entry points (untested inside KiCAD).
- `emfdtd --version`; release archives built by GitHub Actions for macOS arm64.
