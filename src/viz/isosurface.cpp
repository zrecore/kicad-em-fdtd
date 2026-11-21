#include "viz/isosurface.hpp"

#include <cmath>
#include <cstdio>
#include <ostream>

namespace viz {
namespace {

// The cube's corners in the order 0 (0,0,0), 1 (1,0,0), 2 (1,1,0), 3 (0,1,0), 4 (0,0,1), 5 (1,0,1), 6
// (1,1,1), 7 (0,1,1), and the six tetrahedra around the diagonal 0-6.
constexpr int kCorner[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                               {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
constexpr int kTets[6][4] = {{0, 5, 1, 6}, {0, 1, 2, 6}, {0, 2, 3, 6},
                             {0, 3, 7, 6}, {0, 7, 4, 6}, {0, 4, 5, 6}};

using P3 = std::array<float, 3>;

P3 cross(const P3& a, const P3& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

} // namespace

std::vector<Triangle> isosurface(const DisplayVolume& dv, const std::vector<float>& field, float level) {
  std::vector<Triangle> mesh;
  std::array<double, 3> h{};
  for (int a = 0; a < 3; ++a)
    h[a] = dv.extent[a] / static_cast<double>(dv.dims[a]);
  auto position = [&](std::size_t i, std::size_t j, std::size_t k) {
    return P3{static_cast<float>(dv.origin[0] + (static_cast<double>(i) + 0.5) * h[0]),
              static_cast<float>(dv.origin[1] + (static_cast<double>(j) + 0.5) * h[1]),
              static_cast<float>(dv.origin[2] + (static_cast<double>(k) + 0.5) * h[2])};
  };
  for (std::size_t k = 0; k + 1 < dv.dims[2]; ++k)
    for (std::size_t j = 0; j + 1 < dv.dims[1]; ++j)
      for (std::size_t i = 0; i + 1 < dv.dims[0]; ++i) {
        float val[8];
        P3 pos[8];
        bool any = false, all = true;
        for (int c = 0; c < 8; ++c) {
          const std::size_t ci = i + static_cast<std::size_t>(kCorner[c][0]),
                            cj = j + static_cast<std::size_t>(kCorner[c][1]),
                            ck = k + static_cast<std::size_t>(kCorner[c][2]);
          val[c] = field[dv.index(ci, cj, ck)];
          pos[c] = position(ci, cj, ck);
          any = any || val[c] >= level;
          all = all && val[c] >= level;
        }
        if (!any || all)
          continue;
        for (const auto& tet : kTets) {
          int in[4], out[4], nIn = 0, nOut = 0;
          for (int t = 0; t < 4; ++t)
            (val[tet[t]] >= level ? in[nIn++] : out[nOut++]) = tet[t];
          if (nIn == 0 || nIn == 4)
            continue;
          auto cut = [&](int a, int b) { // where the edge a-b crosses the level
            const float t = (level - val[a]) / (val[b] - val[a]);
            return P3{pos[a][0] + t * (pos[b][0] - pos[a][0]), pos[a][1] + t * (pos[b][1] - pos[a][1]),
                      pos[a][2] + t * (pos[b][2] - pos[a][2])};
          };
          if (nIn == 1 || nIn == 3) { // one vertex on its own side: one triangle
            const int lone = nIn == 1 ? in[0] : out[0];
            const int* others = nIn == 1 ? out : in;
            mesh.push_back({{cut(lone, others[0]), cut(lone, others[1]), cut(lone, others[2])}});
          } else { // two and two: a quad, as two triangles
            const P3 a = cut(in[0], out[0]), b = cut(in[0], out[1]), c = cut(in[1], out[1]),
                     d = cut(in[1], out[0]);
            mesh.push_back({{a, b, c}});
            mesh.push_back({{a, c, d}});
          }
        }
      }
  return mesh;
}

double surfaceArea(const std::vector<Triangle>& mesh) {
  double area = 0;
  for (const Triangle& t : mesh) {
    const P3 e1{t.v[1][0] - t.v[0][0], t.v[1][1] - t.v[0][1], t.v[1][2] - t.v[0][2]},
        e2{t.v[2][0] - t.v[0][0], t.v[2][1] - t.v[0][1], t.v[2][2] - t.v[0][2]};
    const P3 n = cross(e1, e2);
    area += 0.5 * std::sqrt(static_cast<double>(n[0]) * n[0] + static_cast<double>(n[1]) * n[1] +
                            static_cast<double>(n[2]) * n[2]);
  }
  return area;
}

void writeObj(std::ostream& out, const std::vector<Triangle>& mesh, std::array<double, 3> shift) {
  out << "# kicad-em-fdtd isosurface, metres\n";
  char buf[128];
  for (const Triangle& t : mesh)
    for (const auto& v : t.v) {
      std::snprintf(buf, sizeof buf, "v %.9g %.9g %.9g\n", v[0] + shift[0], v[1] + shift[1], v[2] + shift[2]);
      out << buf;
    }
  for (std::size_t n = 0; n < mesh.size(); ++n)
    out << "f " << 3 * n + 1 << ' ' << 3 * n + 2 << ' ' << 3 * n + 3 << "\n";
}

} // namespace viz
