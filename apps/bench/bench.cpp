// Measures what the performance estimates in docs/PLAN.md wait on: achievable copy bandwidth
// (BabelStream-style) and the submit-to-fence floor. Warm runs, medians, JSON to --out.
// Run under the GPU lock:  with_gpu_lock.sh nice -n 10 build/release/apps/bench/bench
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "fdtd_vk/gpu_solver.hpp"
#include "viz_vk/gpu_volume.hpp"
#include "vkcore/compute.hpp"

namespace {

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

} // namespace

int main(int argc, char** argv) {
  std::string outPath = "out/bench.json";
  std::size_t mib = 512; // per buffer
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--out"))
      outPath = argv[i + 1];
    if (!std::strcmp(argv[i], "--mib"))
      mib = std::stoul(argv[i + 1]);
  }

  vkcore::Context ctx;
  const std::string shaders = FDTD_SHADER_DIR;

  // Submit floor: one workgroup of a trivial kernel.
  vkcore::Buffer tiny(ctx, 4096);
  vkcore::ComputePipeline hello(ctx, shaders + "/hello.comp.spv", 1, sizeof(std::uint32_t));
  vkcore::Buffer* tinyBufs[] = {&tiny};
  const std::uint32_t n64 = 64;
  std::vector<double> floorMs;
  for (int i = 0; i < 5; ++i)
    hello.run(tinyBufs, &n64, 1); // warm up (pipeline and first-use costs)
  for (int i = 0; i < 21; ++i)
    floorMs.push_back(hello.run(tinyBufs, &n64, 1).wallMs);

  // Copy bandwidth: c = a over `mib` MiB, a few passes per submit so each submit stays ~tens of ms.
  const std::size_t bytes = mib << 20;
  const std::uint32_t elems = static_cast<std::uint32_t>(bytes / 16); // vec4 elements
  vkcore::Buffer a(ctx, bytes), c(ctx, bytes);
  std::memset(a.as<unsigned char>().data(), 1, bytes);
  std::memset(c.as<unsigned char>().data(), 0, bytes);
  vkcore::ComputePipeline copy(ctx, shaders + "/copy.comp.spv", 2, sizeof(std::uint32_t));
  vkcore::Buffer* bufs[] = {&a, &c};
  const std::uint32_t passes = 3, groups = 8192;
  for (int i = 0; i < 3; ++i)
    copy.run(bufs, &elems, groups, passes);
  bool stamps = true; // false: the driver gives no device timestamps, times below are wall times
  std::vector<double> gbps, wallMs;
  for (int i = 0; i < 9; ++i) {
    const auto t = copy.run(bufs, &elems, groups, passes);
    stamps = stamps && t.timestamps;
    gbps.push_back(2.0 * static_cast<double>(bytes) * passes / (t.gpuMs * 1e-3) / 1e9); // read + write
    wallMs.push_back(t.wallMs);
  }
  const bool correct = std::memcmp(a.as<unsigned char>().data(), c.as<unsigned char>().data(), bytes) == 0;

  // FDTD kernel throughput on an N^3-cell PEC cavity (no sources: pure update cost). Warm, median of 7.
  const std::size_t n = 160;
  fdtd::Solver<float> init(fdtd::Grid::uniform({n, n, n}, {0.16, 0.16, 0.16}), 0.95);
  init.mutableField(fdtd::Kind::E, fdtd::Z)[init.grid().nodeIndex(n / 2, n / 2, n / 2)] = 1.0f;
  fdtd_vk::GpuSolver gpu(ctx, init);
  const std::uint32_t stepsPerSubmit = 10;
  for (int i = 0; i < 3; ++i)
    gpu.step(stepsPerSubmit);
  std::vector<double> mcells, fdtdMs;
  for (int i = 0; i < 7; ++i) {
    const auto t = gpu.step(stepsPerSubmit);
    stamps = stamps && t.timestamps;
    mcells.push_back(static_cast<double>(n * n * n) * stepsPerSubmit / (t.gpuMs * 1e-3) / 1e6);
    fdtdMs.push_back(t.gpuMs);
  }
  const double mcellsMedian = median(mcells);

  // The phasor accumulator's cost next to the step kernel's (docs/PLAN.md, Next steps 13): the display volume
  // (64 x 64 x 32 voxels, 13 frequencies) and a far-field box (150 000 channels, 5 frequencies), one
  // accumulate call each, wall time (the call submits and waits), warm, median of 15. The share is what the
  // accumulator adds to a run that samples every 16 steps.
  struct AccumCase {
    const char* name;
    std::size_t channels, freqs;
    double ms = 0, share = 0;
  };
  std::vector<AccumCase> cases{{"display volume 64x64x32 x 13 f", 64 * 64 * 32, 13},
                               {"far-field box 150k channels x 5 f", 150000, 5}};
  const double stepMs = median(fdtdMs) / stepsPerSubmit;
  for (auto& ac : cases) {
    std::vector<fdtd::PhasorChannel> ch;
    const std::size_t nodes = init.grid().nodeCount();
    for (std::size_t i = 0; i < ac.channels; ++i)
      ch.push_back({fdtd::Kind::E, fdtd::Z, static_cast<std::uint32_t>((i * 7919) % nodes)});
    std::vector<double> fr(ac.freqs);
    for (std::size_t i = 0; i < fr.size(); ++i)
      fr[i] = 1e9 + 1e8 * static_cast<double>(i);
    const std::size_t list = gpu.addPhasorList(ch, fr);
    for (int i = 0; i < 3; ++i)
      gpu.accumulate(list, 1e-12);
    std::vector<double> ms;
    for (int i = 0; i < 15; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      gpu.accumulate(list, 1e-12);
      ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    ac.ms = median(ms);
    ac.share = ac.ms / (16 * stepMs + ac.ms);
  }

  // Volume view: one frame of the 3D ray march at three sizes over a 128 x 128 x 64 display volume holding
  // two lobes, CPU reference (all cores) against the GPU renderer. Wall time of render() including the
  // readback to RGB8, warm, median of 7.
  struct VolumeCase {
    std::uint32_t w, h;
    double cpuMs = 0, gpuMs = 0;
  };
  std::vector<VolumeCase> vcases{{640, 400}, {1280, 800}, {1920, 1080}};
  {
    viz::DisplayVolume dv;
    dv.dims = {128, 128, 64};
    dv.extent = {0.128, 0.128, 0.064};
    std::vector<float> re(dv.voxels()), im(dv.voxels());
    for (std::size_t k = 0; k < 64; ++k)
      for (std::size_t j = 0; j < 128; ++j)
        for (std::size_t i = 0; i < 128; ++i) {
          const double x1 = static_cast<double>(i) - 40, y1 = static_cast<double>(j) - 60,
                       z = static_cast<double>(k) - 30, x2 = static_cast<double>(i) - 90,
                       y2 = static_cast<double>(j) - 70;
          re[dv.index(i, j, k)] = static_cast<float>(std::exp(-(x1 * x1 + y1 * y1 + z * z) / 400.0) -
                                                     0.7 * std::exp(-(x2 * x2 + y2 * y2 + z * z) / 300.0));
          im[dv.index(i, j, k)] = 0.3f * re[dv.index(i, j, k)];
        }
    auto cpu = viz::makeCpuVolumeRenderer(dv);
    auto gpuView = viz::makeGpuVolumeRenderer(ctx, dv);
    cpu->setPhasors(re, im);
    gpuView->setPhasors(re, im);
    viz::Camera cam;
    for (auto& vc : vcases) {
      viz::RenderSpec rs;
      rs.width = vc.w;
      rs.height = vc.h;
      rs.colourScale = 0.8;
      rs.logRangeDb = 28;
      rs.threshold = 0.08;
      for (auto* r : {cpu.get(), gpuView.get()}) {
        std::vector<double> ms;
        for (int i = 0; i < 9; ++i) {
          const auto t0 = std::chrono::steady_clock::now();
          r->render(cam, rs, 0.1 * i, false);
          ms.push_back(
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        ms.erase(ms.begin(), ms.begin() + 2); // warm-up
        (r == cpu.get() ? vc.cpuMs : vc.gpuMs) = median(ms);
      }
    }
  }

  const auto& p = ctx.properties();
  std::ofstream f(outPath);
  f << "{\n  \"device\": \"" << p.deviceName << "\",\n  \"vendorId\": " << p.vendorID
    << ",\n  \"deviceId\": " << p.deviceID << ",\n  \"buffer_mib\": " << mib
    << ",\n  \"copy_gb_per_s_median\": " << median(gbps)
    << ",\n  \"copy_submit_wall_ms_median\": " << median(wallMs)
    << ",\n  \"submit_floor_ms_median\": " << median(floorMs)
    << ",\n  \"buffer_memory_flags\": " << a.memoryFlags()
    << ",\n  \"memory_note\": \"host-visible: unified (real) on Apple silicon; system RAM over PCIe on a "
       "discrete GPU\""
    << ",\n  \"fdtd_cells\": " << n * n * n << ",\n  \"fdtd_mcells_per_s_median\": " << mcellsMedian
    << ",\n  \"fdtd_effective_gb_per_s_at_48B\": " << mcellsMedian * 48e-3
    << ",\n  \"fdtd_submit_gpu_ms_median\": " << median(fdtdMs) << ",\n  \"fdtd_step_ms\": " << stepMs
    << ",\n  \"accumulate\": [";
  for (std::size_t i = 0; i < cases.size(); ++i)
    f << (i ? "," : "") << "\n    {\"case\": \"" << cases[i].name << "\", \"wall_ms_median\": " << cases[i].ms
      << ", \"share_of_run_sampling_every_16_steps\": " << cases[i].share << "}";
  f << "\n  ],\n  \"volume_render\": [";
  for (std::size_t i = 0; i < vcases.size(); ++i)
    f << (i ? "," : "") << "\n    {\"size\": \"" << vcases[i].w << "x" << vcases[i].h
      << "\", \"cpu_ms_median\": " << vcases[i].cpuMs << ", \"gpu_ms_median\": " << vcases[i].gpuMs << "}";
  f << "\n  ],\n  \"copy_verified\": " << (correct ? "true" : "false") << "\n}\n";
  std::printf(
      "%s: copy %.1f GB/s (median of 9, %zu MiB x2, %u passes/submit), submit floor %.3f ms, copy %s\n",
      p.deviceName, median(gbps), mib, passes, median(floorMs), correct ? "verified" : "WRONG");
  std::printf(
      "fdtd kernel: %.0f Mcells/s on %zu^3 cells (%.1f GB/s if 48 B/cell-step), %.1f ms per %u-step submit\n",
      mcellsMedian, n, mcellsMedian * 48e-3, median(fdtdMs), stepsPerSubmit);
  if (!stamps)
    std::printf("note: no device timestamps on this driver, so the times above are wall times (a lower bound "
                "on speed)\n");
  for (const auto& ac : cases)
    std::printf("accumulate %s: %.2f ms per call (wall), %.1f %% of a run that samples every 16 steps (step "
                "%.2f ms)\n",
                ac.name, ac.ms, 100 * ac.share, stepMs);
  for (const auto& vc : vcases)
    std::printf("volume frame %ux%u: CPU %.1f ms, GPU %.1f ms (wall, with readback)\n", vc.w, vc.h, vc.cpuMs,
                vc.gpuMs);
  return correct ? 0 : 1;
}
