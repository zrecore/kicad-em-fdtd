#include "viz/vtk_export.hpp"

#include <cctype>
#include <cstdio>
#include <ostream>
#include <stdexcept>

namespace viz {

void writeVtkVolume(std::ostream& out, const DisplayVolume& dv, const std::vector<VtkScalar>& scalars,
                    const std::string& title, std::array<double, 3> shift) {
  for (const VtkScalar& s : scalars)
    if (s.values.size() != dv.voxels())
      throw std::invalid_argument("VTK scalar '" + s.name + "' has " + std::to_string(s.values.size()) +
                                  " values, the volume has " + std::to_string(dv.voxels()));
  std::string line = title.substr(0, title.find('\n'));
  line = line.substr(0, 250);
  char buf[160];
  out << "# vtk DataFile Version 3.0\n" << line << "\nASCII\nDATASET STRUCTURED_POINTS\n";
  std::snprintf(buf, sizeof buf, "DIMENSIONS %zu %zu %zu\n", dv.dims[0], dv.dims[1], dv.dims[2]);
  out << buf;
  std::array<double, 3> spacing{}, origin{};
  for (int a = 0; a < 3; ++a) {
    spacing[a] = dv.extent[a] / static_cast<double>(dv.dims[a]);
    origin[a] = dv.origin[a] + 0.5 * spacing[a] + shift[a]; // the first voxel centre
  }
  std::snprintf(buf, sizeof buf, "ORIGIN %.12g %.12g %.12g\nSPACING %.12g %.12g %.12g\n", origin[0],
                origin[1], origin[2], spacing[0], spacing[1], spacing[2]);
  out << buf;
  out << "POINT_DATA " << dv.voxels() << "\n";
  for (const VtkScalar& s : scalars) {
    std::string name = s.name;
    for (char& c : name)
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
        c = '_';
    out << "SCALARS " << name << " float 1\nLOOKUP_TABLE default\n";
    for (std::size_t i = 0; i < s.values.size(); ++i) {
      std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(s.values[i]));
      out << buf << ((i + 1) % 6 == 0 ? '\n' : ' ');
    }
    out << "\n";
  }
}

} // namespace viz
