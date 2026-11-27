---
name: gpu-driver-check
description: Run this project's GPU tests, benchmark or window on both Apple Vulkan drivers (KosmicKrisp and MoltenVK) under the shared GPU lock, with optional validation. Use after any change to shaders, src/vkcore, src/viz_vk, the swapchain or GpuUi, or when asked to compare drivers or measure frame time.
---

# GPU driver check (this Mac: M1 Pro)

Both ICDs are installed system-wide by the LunarG SDK in `/usr/local/share/vulkan/icd.d`: `libkosmickrisp_icd.json` (the target; Khronos recommends it on Apple Silicon) and `MoltenVK_icd.json` (deprecated upstream, the fallback). Without `VK_ICD_FILENAMES` the loader sees both, so always force one.

**Every GPU run goes through the shared lock** (CLAUDE.md): `L=/Volumes/CrucialX9/Projects/mongo/poc/gpu/tools/with_gpu_lock.sh`, then `$L nice -n 10 <cmd>`. Keep holds short. Run from the repo root; keep output in git-ignored `out/` on the external volume (never /tmp).

1. Build: `cmake --build --preset release` (or `scripts/check.sh --gpu` for the whole health check, which takes the lock itself).
2. For each `icd` in `libkosmickrisp_icd MoltenVK_icd`, with `export VK_ICD_FILENAMES=/usr/local/share/vulkan/icd.d/$icd.json`:
   - Tests: `$L nice -n 10 build/release/tests/tests_gpu "[gpu]"` (filters: `[volume]`, `[slice]`).
   - Benchmark: `$L nice -n 10 build/release/apps/bench/bench` (writes `out/bench.json`; copy it to `out/drivers/bench-$icd.json`). KosmicKrisp returns zero device timestamps, so its times are wall times (the bench prints a note); do not read that as a regression.
   - Window frame cost: `$L nice -n 10 build/release/apps/emfdtd/emfdtd out/samples/patch_2g45.json --ui --ui-frames 120` (add `--cpu-ui` for the CPU rasterizer). The window path is where drivers differ.
   - View cost without a window: `... emfdtd BOARD --gpu --ui-screenshot out/drivers/x.png --ui-run --ui-tab "3D volume" --ui-bench 20 --ui-size 1920x1080` (tabs: "Field slice", "3D volume").
3. Validation (finds misuse that one driver tolerates and the other crashes on; KosmicKrisp tolerated a null descriptor set that segfaulted MoltenVK): `VK_LAYER_PATH=/usr/local/share/vulkan/explicit_layer.d EMFDTD_VALIDATE=1` on the window command. Expect no `[vulkan validation]` lines.
4. Record numbers in `docs/PLAN.md` ("Measured so far": one line, driver, size, wall or device time, one run or median of how many). One run each means differences of a few percent are not established; say so.

Do not install or change system Vulkan software without the user's go-ahead. A window screenshot (`screencapture -x out/drivers/x.png`) also captures the rest of the screen: keep it in `out/` and say so.
