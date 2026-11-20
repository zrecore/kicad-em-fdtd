#include "viz/volume.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

namespace viz {
namespace {

std::size_t nearestNode(const fdtd::Grid& g, fdtd::Axis a, double x) {
  std::size_t lo = 0, hi = g.nodes(a) - 1;
  while (hi - lo > 1) {
    const std::size_t mid = (lo + hi) / 2;
    (g.edge(a, mid) <= x ? lo : hi) = mid;
  }
  return std::abs(g.edge(a, lo) - x) <= std::abs(g.edge(a, hi) - x) ? lo : hi;
}

struct Vec {
  double x, y, z;
};
Vec operator+(Vec a, Vec b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec operator-(Vec a, Vec b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec operator*(Vec a, double s) {
  return {a.x * s, a.y * s, a.z * s};
}
double dot(Vec a, Vec b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec cross(Vec a, Vec b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec norm(Vec a) {
  return a * (1.0 / std::sqrt(dot(a, a)));
}

// The box is drawn centred at the origin with its longest side 1, so the camera distance is scale-free.
struct View {
  Vec size, pos, forward, right, up;
};
View makeView(const DisplayVolume& dv, const Camera& c) {
  const double longest = std::max({dv.extent[0], dv.extent[1], dv.extent[2]});
  View v;
  v.size = {dv.extent[0] / longest, dv.extent[1] / longest, dv.extent[2] / longest};
  const Vec target{c.target[0], c.target[1], c.target[2]}; // the point the camera looks at, orbits and pans
  const Vec dir{std::cos(c.elevation) * std::cos(c.azimuth), std::cos(c.elevation) * std::sin(c.azimuth),
                std::sin(c.elevation)};
  v.pos = target + dir * c.distance;
  v.forward = norm(Vec{0, 0, 0} - dir);
  v.right = norm(cross(v.forward, Vec{0, 0, 1}));
  v.up = cross(v.right, v.forward);
  return v;
}

// Soft clip of the accumulated glow: lifts weak fields, and strong ones approach their full colour without
// saturating to white (so the hue survives).
double toneMap(double value, double gain) {
  return 255.0 * (1.0 - std::exp(-gain * value / 255.0));
}

} // namespace

DisplayVolume makeDisplayVolume(const fdtd::Grid& g, std::size_t maxDim, const VolumeBox& box) {
  DisplayVolume dv;
  const bool whole = !(box.hi[0] > box.lo[0] && box.hi[1] > box.lo[1] && box.hi[2] > box.lo[2]);
  double longest = 0;
  for (int a = 0; a < 3; ++a) {
    dv.origin[a] = whole ? 0 : std::max(0.0, box.lo[a]);
    const double hi = whole ? g.length(static_cast<fdtd::Axis>(a))
                            : std::min(g.length(static_cast<fdtd::Axis>(a)), box.hi[a]);
    dv.extent[a] = hi - dv.origin[a];
    longest = std::max(longest, dv.extent[a]);
  }
  for (int a = 0; a < 3; ++a) {
    dv.dims[a] = std::max<std::size_t>(
        2, static_cast<std::size_t>(std::lround(static_cast<double>(maxDim) * dv.extent[a] / longest)));
    dv.nodeOf[a].resize(dv.dims[a]);
    for (std::size_t i = 0; i < dv.dims[a]; ++i)
      dv.nodeOf[a][i] = nearestNode(g, static_cast<fdtd::Axis>(a),
                                    dv.origin[a] + (static_cast<double>(i) + 0.5) /
                                                       static_cast<double>(dv.dims[a]) * dv.extent[a]);
  }
  return dv;
}

void sampleField(const fdtd::Grid& g, const DisplayVolume& dv, std::span<const float> field,
                 std::vector<float>& out) {
  out.resize(dv.voxels());
  for (std::size_t k = 0; k < dv.dims[2]; ++k)
    for (std::size_t j = 0; j < dv.dims[1]; ++j) {
      const std::size_t row = g.nodeIndex(0, dv.nodeOf[1][j], dv.nodeOf[2][k]);
      float* dst = &out[dv.index(0, j, k)];
      for (std::size_t i = 0; i < dv.dims[0]; ++i)
        dst[i] = field[row + dv.nodeOf[0][i]];
    }
}

void reconstruct(const PhasorAccumulator& acc, std::size_t k, double phi, std::vector<float>& out) {
  out.resize(acc.voxels());
  const float c = static_cast<float>(std::cos(phi)), s = static_cast<float>(std::sin(phi));
  for (std::size_t v = 0; v < out.size(); ++v) {
    const auto x = acc.phasor(k, v);
    out[v] = x.real() * c - x.imag() * s;
  }
}

void fitCamera(const DisplayVolume& dv, const std::vector<float>& values, double colourScale,
               double logRangeDb, Camera& cam, double margin) {
  const double longest = std::max({dv.extent[0], dv.extent[1], dv.extent[2]});
  const std::array<double, 3> size{dv.extent[0] / longest, dv.extent[1] / longest, dv.extent[2] / longest};
  const double thr = colourScale * (logRangeDb > 0 ? std::pow(10.0, -logRangeDb * 0.85 / 20) : 0.01);
  std::array<std::size_t, 3> lo = dv.dims, hi{0, 0, 0};
  bool any = false;
  for (std::size_t k = 0; k < dv.dims[2]; ++k)
    for (std::size_t j = 0; j < dv.dims[1]; ++j)
      for (std::size_t i = 0; i < dv.dims[0]; ++i)
        if (std::abs(values[dv.index(i, j, k)]) >= thr) {
          any = true;
          const std::array<std::size_t, 3> idx{i, j, k};
          for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], idx[a]);
            hi[a] = std::max(hi[a], idx[a]);
          }
        }
  if (!any)
    return;
  double radius2 = 0;
  for (int a = 0; a < 3; ++a) {
    const double a0 = (static_cast<double>(lo[a]) / static_cast<double>(dv.dims[a]) - 0.5) * size[a];
    const double a1 = (static_cast<double>(hi[a] + 1) / static_cast<double>(dv.dims[a]) - 0.5) * size[a];
    cam.target[a] = 0.5 * (a0 + a1);
    radius2 += 0.25 * (a1 - a0) * (a1 - a0);
  }
  const double radius = std::max(std::sqrt(radius2), 0.05);
  cam.distance = std::clamp(margin * radius / std::tan(0.5 * cam.fovY), 0.25, 6.0);
}

void panCamera(const DisplayVolume& dv, Camera& cam, double dxPixels, double dyPixels, double heightPixels) {
  const View v = makeView(dv, cam);
  // One pixel of the view height spans 2 tan(fov/2) * distance at the target; dragging moves the scene with
  // the pointer, so the camera goes the other way.
  const double perPixel = 2.0 * std::tan(0.5 * cam.fovY) * cam.distance / std::max(heightPixels, 1.0);
  const Vec move = v.right * (-dxPixels * perPixel) + v.up * (dyPixels * perPixel);
  cam.target[0] += move.x;
  cam.target[1] += move.y;
  cam.target[2] += move.z;
}

bool projectPoint(const DisplayVolume& dv, const Camera& cam, std::uint32_t width, std::uint32_t height,
                  const std::array<double, 3>& p, float& px, float& py) {
  const View v = makeView(dv, cam);
  const Vec w{(p[0] - dv.origin[0]) / dv.extent[0] - 0.5, (p[1] - dv.origin[1]) / dv.extent[1] - 0.5,
              (p[2] - dv.origin[2]) / dv.extent[2] - 0.5};
  const Vec world{w.x * v.size.x, w.y * v.size.y, w.z * v.size.z};
  const Vec d = world - v.pos;
  const double depth = dot(d, v.forward);
  if (depth <= 1e-6)
    return false;
  const double tanY = std::tan(0.5 * cam.fovY), aspect = static_cast<double>(width) / height;
  px = static_cast<float>((0.5 + 0.5 * dot(d, v.right) / (depth * tanY * aspect)) * width);
  py = static_cast<float>((0.5 - 0.5 * dot(d, v.up) / (depth * tanY)) * height);
  return true;
}

std::vector<std::uint8_t> renderVolume(const DisplayVolume& dv, const std::vector<float>& vals,
                                       const Camera& cam, const RenderSpec& spec) {
  const View view = makeView(dv, cam);
  const double tanY = std::tan(0.5 * cam.fovY), aspect = static_cast<double>(spec.width) / spec.height;
  const std::size_t nx = dv.dims[0], ny = dv.dims[1], nz = dv.dims[2];
  constexpr std::size_t kBrick = 4;
  const std::size_t bx = (nx + kBrick - 1) / kBrick, by = (ny + kBrick - 1) / kBrick,
                    bz = (nz + kBrick - 1) / kBrick;
  std::vector<std::uint8_t> brickFull(bx * by * bz, 1);
  if (spec.skipEmpty) { // a brick is empty if every voxel in it (and its neighbours, for interpolation) is
                        // below threshold
    // In dB mode a voxel counts as empty when it maps below `threshold` of the dB range.
    const double thr =
        spec.colourScale *
        (spec.logRangeDb > 0 ? std::pow(10.0, -spec.logRangeDb * (1 - spec.threshold) / 20) : spec.threshold);
    std::fill(brickFull.begin(), brickFull.end(), 0);
    for (std::size_t k = 0; k < nz; ++k)
      for (std::size_t j = 0; j < ny; ++j)
        for (std::size_t i = 0; i < nx; ++i)
          if (std::abs(vals[dv.index(i, j, k)]) >= thr) {
            for (long dk = -1; dk <= 1; ++dk)
              for (long dj = -1; dj <= 1; ++dj)
                for (long di = -1; di <= 1; ++di) {
                  const long ii = static_cast<long>(i) + di * static_cast<long>(kBrick) / 2 * 2,
                             jj = static_cast<long>(j) + dj * static_cast<long>(kBrick) / 2 * 2,
                             kk = static_cast<long>(k) + dk * static_cast<long>(kBrick) / 2 * 2;
                  if (ii < 0 || jj < 0 || kk < 0 || ii >= static_cast<long>(nx) ||
                      jj >= static_cast<long>(ny) || kk >= static_cast<long>(nz))
                    continue;
                  brickFull[(static_cast<std::size_t>(ii) / kBrick) +
                            bx * ((static_cast<std::size_t>(jj) / kBrick) +
                                  by * (static_cast<std::size_t>(kk) / kBrick))] = 1;
                }
          }
  }
  const double voxel = std::min({view.size.x / nx, view.size.y / ny, view.size.z / nz});
  const double step = 0.6 * voxel;

  auto sample = [&](double gx, double gy, double gz) { // trilinear, voxel centres at +0.5
    gx = std::clamp(gx - 0.5, 0.0, nx - 1.0);
    gy = std::clamp(gy - 0.5, 0.0, ny - 1.0);
    gz = std::clamp(gz - 0.5, 0.0, nz - 1.0);
    const std::size_t i = std::min<std::size_t>(static_cast<std::size_t>(gx), nx - 2),
                      j = std::min<std::size_t>(static_cast<std::size_t>(gy), ny - 2),
                      k = std::min<std::size_t>(static_cast<std::size_t>(gz), nz - 2);
    const double fx = gx - i, fy = gy - j, fz = gz - k;
    auto at = [&](std::size_t a, std::size_t b, std::size_t c) {
      return static_cast<double>(vals[dv.index(a, b, c)]);
    };
    const double c00 = at(i, j, k) * (1 - fx) + at(i + 1, j, k) * fx,
                 c10 = at(i, j + 1, k) * (1 - fx) + at(i + 1, j + 1, k) * fx;
    const double c01 = at(i, j, k + 1) * (1 - fx) + at(i + 1, j, k + 1) * fx,
                 c11 = at(i, j + 1, k + 1) * (1 - fx) + at(i + 1, j + 1, k + 1) * fx;
    return (c00 * (1 - fy) + c10 * fy) * (1 - fz) + (c01 * (1 - fy) + c11 * fy) * fz;
  };

  const Rgb bg = spec.lightTheme ? Rgb{238, 238, 241} : kBackground;
  std::vector<std::uint8_t> img(static_cast<std::size_t>(spec.width) * spec.height * 3);
  auto renderRows = [&](std::uint32_t y0, std::uint32_t y1) {
    for (std::uint32_t y = y0; y < y1; ++y)
      for (std::uint32_t x = 0; x < spec.width; ++x) {
        const double u = ((x + 0.5) / spec.width * 2 - 1) * tanY * aspect,
                     w = (1 - (y + 0.5) / spec.height * 2) * tanY;
        const Vec dir = norm(view.forward + view.right * u + view.up * w);
        // ray / box intersection (box [-size/2, size/2])
        double t0 = 0, t1 = 1e30;
        const double o[3] = {view.pos.x, view.pos.y, view.pos.z}, d[3] = {dir.x, dir.y, dir.z},
                     h[3] = {view.size.x / 2, view.size.y / 2, view.size.z / 2};
        bool hit = true;
        for (int a = 0; a < 3 && hit; ++a) {
          if (std::abs(d[a]) < 1e-12) {
            hit = std::abs(o[a]) <= h[a];
          } else {
            double ta = (-h[a] - o[a]) / d[a], tb = (h[a] - o[a]) / d[a];
            if (ta > tb)
              std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            hit = t0 < t1;
          }
        }
        double r = 0, g = 0, b = 0, alpha = 0;
        if (hit)
          for (double t = t0 + 0.5 * step; t < t1 && alpha < 0.98; t += step) {
            const double px = o[0] + d[0] * t, py = o[1] + d[1] * t, pz = o[2] + d[2] * t;
            const double gx = (px / view.size.x + 0.5) * nx, gy = (py / view.size.y + 0.5) * ny,
                         gz = (pz / view.size.z + 0.5) * nz;
            if (spec.skipEmpty) {
              const std::size_t ib = std::min<std::size_t>(
                                    bx - 1, static_cast<std::size_t>(std::max(0.0, gx)) / kBrick),
                                jb = std::min<std::size_t>(
                                    by - 1, static_cast<std::size_t>(std::max(0.0, gy)) / kBrick),
                                kb = std::min<std::size_t>(
                                    bz - 1, static_cast<std::size_t>(std::max(0.0, gz)) / kBrick);
              if (!brickFull[ib + bx * (jb + by * kb)]) {
                t += step *
                     (kBrick - 1); // empty brick: leap ahead (the brick flags include a neighbour margin)
                continue;
              }
            }
            double s = sample(gx, gy, gz) / spec.colourScale;
            if (spec.logRangeDb > 0) { // map [-range, 0] dB to [0, 1], keeping the sign
              const double db = 20 * std::log10(std::max(std::abs(s), 1e-12));
              s = std::copysign(std::clamp(1 + db / spec.logRangeDb, 0.0, 1.0), s);
            }
            const double a = std::abs(s);
            if (a < spec.threshold)
              continue;
            const double shaped = std::copysign(std::pow(std::min(a, 1.0), spec.gamma), s);
            // opacity per voxel crossed (not per unit length): a thin sheet of field must still show from
            // above
            const double opacity = 1 - std::exp(-0.25 * spec.density * std::min(a, 1.0) * step / voxel);
            const Rgb c = spec.lightTheme
                              ? volumeLight(static_cast<float>(shaped), spec.negative, spec.positive)
                              : luminous(static_cast<float>(shaped), spec.negative, spec.positive);
            const double wgt = (1 - alpha) * opacity;
            r += wgt * c.r;
            g += wgt * c.g;
            b += wgt * c.b;
            alpha += wgt;
          }
        std::uint8_t* px = &img[(static_cast<std::size_t>(y) * spec.width + x) * 3];
        px[0] = static_cast<std::uint8_t>(
            std::clamp((spec.lightTheme ? r : toneMap(r, spec.emission)) + (1 - alpha) * bg.r, 0.0, 255.0));
        px[1] = static_cast<std::uint8_t>(
            std::clamp((spec.lightTheme ? g : toneMap(g, spec.emission)) + (1 - alpha) * bg.g, 0.0, 255.0));
        px[2] = static_cast<std::uint8_t>(
            std::clamp((spec.lightTheme ? b : toneMap(b, spec.emission)) + (1 - alpha) * bg.b, 0.0, 255.0));
      }
  };
  const unsigned n = spec.threads > 0 ? static_cast<unsigned>(spec.threads)
                                      : std::max(1u, std::thread::hardware_concurrency());
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < n; ++t)
    pool.emplace_back(renderRows, spec.height * t / n, spec.height * (t + 1) / n);
  for (auto& th : pool)
    th.join();
  return img;
}

} // namespace viz
