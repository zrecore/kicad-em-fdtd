// Phasor accumulation on the GPU: see GpuSolver::addPhasorList.
#include <cmath>
#include <numbers>
#include <stdexcept>

#include "core/resources.hpp"
#include "fdtd_vk/gpu_solver.hpp"

namespace fdtd_vk {

std::size_t GpuSolver::addPhasorList(std::vector<PhasorChannel> channels, std::vector<double> frequenciesHz) {
  if (channels.empty() || frequenciesHz.empty())
    throw std::invalid_argument("a phasor list needs channels and frequencies");
  auto l = std::make_unique<PhasorList>();
  l->channels = channels.size();
  l->frequencies = std::move(frequenciesHz);
  const std::size_t n = l->frequencies.size();
  l->list = std::make_unique<vkcore::Buffer>(ctx_, channels.size() * 2 * sizeof(std::uint32_t),
                                             vkcore::Residency::Device);
  auto words = l->list->as<std::uint32_t>();
  for (std::size_t c = 0; c < channels.size(); ++c) {
    if (channels[c].node >= nodeCount_)
      throw std::out_of_range("phasor channel outside the grid");
    words[2 * c] =
        static_cast<std::uint32_t>((channels[c].kind == fdtd::Kind::E ? 0 : 3) + channels[c].component);
    words[2 * c + 1] = channels[c].node;
  }
  l->list->upload();
  // re, im and their Kahan compensations, each N * channels floats
  l->acc = std::make_unique<vkcore::Buffer>(ctx_, 4 * n * channels.size() * sizeof(float),
                                            vkcore::Residency::Device);
  std::fill(l->acc->as<float>().begin(), l->acc->as<float>().end(), 0.0f);
  l->acc->upload();
  l->trig = std::make_unique<vkcore::Buffer>(ctx_, 2 * n * 2 * sizeof(float), vkcore::Residency::Device);
  if (!accumPipeline_)
    accumPipeline_ = std::make_unique<vkcore::ComputePipeline>(
        ctx_, core::resourceDir("shaders", FDTD_SHADER_DIR) + "/phasor_accum.comp.spv", 9,
        sizeof(fdtd_gpu::AccumParams));
  lists_.push_back(std::move(l));
  return lists_.size() - 1;
}

void GpuSolver::accumulate(std::size_t list, double weight) {
  PhasorList& l = *lists_.at(list);
  const std::size_t n = l.frequencies.size();
  auto trig = l.trig->as<float>();
  const double tE = static_cast<double>(stepsDone_) * dt_;
  for (std::size_t k = 0; k < n; ++k)
    for (int kind = 0; kind < 2; ++kind) {
      const double w = 2 * std::numbers::pi * l.frequencies[k] * (kind == 0 ? tE : tE - 0.5 * dt_);
      trig[2 * (kind * n + k)] = static_cast<float>(std::cos(w) * weight);
      trig[2 * (kind * n + k) + 1] = static_cast<float>(std::sin(w) * weight);
    }
  l.trig->upload();
  l.stale = true;
  std::vector<vkcore::Buffer*> bound;
  for (int i = 0; i < 6; ++i)
    bound.push_back(buffers_[i].get());
  bound.push_back(l.list.get());
  bound.push_back(l.acc.get());
  bound.push_back(l.trig.get());
  const fdtd_gpu::AccumParams params{static_cast<std::uint32_t>(l.channels), static_cast<std::uint32_t>(n)};
  const std::uint32_t groups = static_cast<std::uint32_t>((l.channels + 255) / 256);
  accumPipeline_->run(bound, &params, groups);
}

std::complex<double> GpuSolver::phasor(std::size_t list, std::size_t frequency, std::size_t channel) const {
  const PhasorList& l = *lists_.at(list);
  const std::size_t n = l.frequencies.size(), at = frequency * l.channels + channel;
  if (frequency >= n || channel >= l.channels)
    throw std::out_of_range("phasor index");
  if (l.stale) {
    l.acc->download();
    l.stale = false;
  }
  const auto acc = l.acc->as<float>();
  return {acc[at], acc[n * l.channels + at]};
}

} // namespace fdtd_vk

namespace fdtd_vk {

void GpuSolver::copyPhasors(std::size_t list, std::span<float> re, std::span<float> im) const {
  const PhasorList& l = *lists_.at(list);
  const std::size_t total = l.frequencies.size() * l.channels;
  if (re.size() != total || im.size() != total)
    throw std::invalid_argument("phasor copy: wrong size");
  if (l.stale) {
    l.acc->download();
    l.stale = false;
  }
  const auto acc = l.acc->as<float>();
  std::copy(acc.begin(), acc.begin() + static_cast<std::ptrdiff_t>(total), re.begin());
  std::copy(acc.begin() + static_cast<std::ptrdiff_t>(total),
            acc.begin() + static_cast<std::ptrdiff_t>(2 * total), im.begin());
}

} // namespace fdtd_vk
