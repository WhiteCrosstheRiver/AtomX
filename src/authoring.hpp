#pragma once
// Direct structure editing for AtomX 1.0. Positions stay Cartesian angstroms,
// matching the rest of the pipeline. Operations rewrite a Dataset in place so
// Creation Mode and the modeling menus share one path.
#include "core.hpp"
#include "elements.hpp"
#include <cmath>
#include <string>

namespace atomx::authoring {
inline constexpr double kPi = 3.14159265358979323846;

struct CellVectors {
    Vec3 a, b, c;
};
inline CellVectors axes(const std::array<double, 9> &cell) {
    return {{float(cell[0]), float(cell[1]), float(cell[2])},
            {float(cell[3]), float(cell[4]), float(cell[5])},
            {float(cell[6]), float(cell[7]), float(cell[8])}};
}
inline double dot(Vec3 u, Vec3 v) { return double(u.x) * v.x + double(u.y) * v.y + double(u.z) * v.z; }
inline Vec3 cross(Vec3 u, Vec3 v) {
    return {float(double(u.y) * v.z - double(u.z) * v.y),
            float(double(u.z) * v.x - double(u.x) * v.z),
            float(double(u.x) * v.y - double(u.y) * v.x)};
}
inline double length(Vec3 v) { return std::sqrt(dot(v, v)); }
inline Vec3 scale(Vec3 v, double s) { return {float(v.x * s), float(v.y * s), float(v.z * s)}; }
inline Vec3 add(Vec3 u, Vec3 v) { return {u.x + v.x, u.y + v.y, u.z + v.z}; }
inline Vec3 sub(Vec3 u, Vec3 v) { return {u.x - v.x, u.y - v.y, u.z - v.z}; }

struct LatticeParameters {
    double a = 0, b = 0, c = 0;
    double alpha = 90, beta = 90, gamma = 90;
    double volume = 0;
    const char *system = "No cell";
};
inline double angleDegrees(Vec3 u, Vec3 v) {
    double lu = length(u), lv = length(v);
    if (lu < 1e-8 || lv < 1e-8) return 0;
    double c = std::clamp(dot(u, v) / (lu * lv), -1.0, 1.0);
    return std::acos(c) * 180.0 / kPi;
}
inline LatticeParameters latticeOf(const Dataset &data) {
    LatticeParameters p;
    auto cell = axes(data.cell);
    p.a = length(cell.a);
    p.b = length(cell.b);
    p.c = length(cell.c);
    if (p.a < 1e-6 && p.b < 1e-6 && p.c < 1e-6) return p;
    p.alpha = angleDegrees(cell.b, cell.c);
    p.beta = angleDegrees(cell.a, cell.c);
    p.gamma = angleDegrees(cell.a, cell.b);
    p.volume = std::abs(dot(cell.a, cross(cell.b, cell.c)));
    auto nearEq = [](double v, double target) { return std::abs(v - target) < 0.8; };
    bool right = nearEq(p.alpha, 90) && nearEq(p.beta, 90) && nearEq(p.gamma, 90);
    bool edges = std::abs(p.a - p.b) < 0.05 * std::max(p.a, 1.0) &&
                 std::abs(p.a - p.c) < 0.05 * std::max(p.a, 1.0);
    bool ab = std::abs(p.a - p.b) < 0.05 * std::max(p.a, 1.0);
    if (right && edges) p.system = "Cubic";
    else if (right && ab) p.system = "Tetragonal";
    else if (right) p.system = "Orthorhombic";
    else if (ab && nearEq(p.alpha, 90) && nearEq(p.beta, 90) && near(p.gamma, 120))
        p.system = "Hexagonal";
    else if (std::abs(p.a - p.b) < 0.05 * std::max(p.a, 1.0) &&
             std::abs(p.a - p.c) < 0.05 * std::max(p.a, 1.0) &&
             std::abs(p.alpha - p.beta) < 0.8 && std::abs(p.alpha - p.gamma) < 0.8)
        p.system = "Rhombohedral";
    else if (ab && nearEq(p.alpha, 90) && nearEq(p.gamma, 90)) p.system = "Monoclinic";
    else p.system = "Triclinic";
    return p;
}

inline bool fractional(const Dataset &data, Vec3 cartesian, double &fa, double &fb, double &fc) {
    auto cell = axes(data.cell);
    Vec3 n = cross(cell.a, cell.b);
    double det = dot(n, cell.c);
    if (std::abs(det) < 1e-10) return false;
    Vec3 d = sub(cartesian, data.origin);
    fa = dot(cross(d, cell.b), cell.c) / det;
    fb = dot(cross(cell.a, d), cell.c) / det;
    fc = dot(n, d) / det;
    return true;
}
inline Vec3 cartesian(const Dataset &data, double fa, double fb, double fc) {
    auto cell = axes(data.cell);
    return add(data.origin, add(scale(cell.a, fa), add(scale(cell.b, fb), scale(cell.c, fc))));
}

inline void finish(Dataset &data, const std::string &comment) {
    data.comment = comment;
    data.sourceCount = data.atoms.size();
    data.stride = 1;
    data.bonds.clear();
    data.scalarProperties.clear();
    data.vectorProperties.clear();
    data.particleColors.clear();
    data.tables.clear();
    data.bounds();
}
inline uint32_t speciesIndex(Dataset &data, const std::string &symbol) {
    for (size_t i = 0; i < data.species.size(); ++i)
        if (data.species[i] == symbol) return uint32_t(i);
    data.species.push_back(symbol);
    return uint32_t(data.species.size() - 1);
}

inline Dataset orthogonalCell(double a, double b, double c, const std::string &element) {
    Dataset data;
    data.pbc = {true, true, true};
    data.cell = {a, 0, 0, 0, b, 0, 0, 0, c};
    uint32_t type = speciesIndex(data, element.empty() ? "C" : element);
    Vec3 p = cartesian(data, 0.5, 0.5, 0.5);
    data.atoms.push_back({p.x, p.y, p.z, type});
    finish(data, "Orthogonal cell");
    return data;
}
inline Dataset triclinicCell(double a, double b, double c, double alpha, double beta, double gamma,
                             const std::string &element) {
    double ar = alpha * kPi / 180, br = beta * kPi / 180, gr = gamma * kPi / 180;
    double sg = std::sin(gr);
    if (std::abs(sg) < 1e-4) sg = 1e-4;
    double cx = c * std::cos(br);
    double cy = c * (std::cos(ar) - std::cos(br) * std::cos(gr)) / sg;
    double cz2 = c * c - cx * cx - cy * cy;
    double cz = std::sqrt(std::max(0.0, cz2));
    Dataset data;
    data.pbc = {true, true, true};
    data.cell = {a, 0, 0, b * std::cos(gr), b * sg, 0, cx, cy, cz};
    uint32_t type = speciesIndex(data, element.empty() ? "C" : element);
    Vec3 p = cartesian(data, 0, 0, 0);
    data.atoms.push_back({p.x, p.y, p.z, type});
    finish(data, "Custom crystal cell");
    return data;
}

inline Dataset replicate(const Dataset &input, int nx, int ny, int nz) {
    nx = std::max(1, nx);
    ny = std::max(1, ny);
    nz = std::max(1, nz);
    Dataset data = input;
    data.atoms.clear();
    auto cell = axes(input.cell);
    data.atoms.reserve(input.atoms.size() * size_t(nx) * ny * nz);
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                Vec3 shift = add(scale(cell.a, i), add(scale(cell.b, j), scale(cell.c, k)));
                for (const auto &atom : input.atoms)
                    data.atoms.push_back({atom.x + shift.x, atom.y + shift.y, atom.z + shift.z, atom.type});
            }
    data.cell[0] *= nx; data.cell[1] *= nx; data.cell[2] *= nx;
    data.cell[3] *= ny; data.cell[4] *= ny; data.cell[5] *= ny;
    data.cell[6] *= nz; data.cell[7] *= nz; data.cell[8] *= nz;
    finish(data, "Supercell " + std::to_string(nx) + "x" + std::to_string(ny) + "x" + std::to_string(nz));
    return data;
}

inline Dataset addVacuum(const Dataset &input, int axis, double angstrom) {
    Dataset data = input;
    int o = std::clamp(axis, 0, 2) * 3;
    Vec3 v{float(data.cell[o]), float(data.cell[o + 1]), float(data.cell[o + 2])};
    double len = length(v);
    if (len < 1e-6) {
        if (axis == 0) data.cell[0] = angstrom;
        else if (axis == 1) data.cell[4] = angstrom;
        else data.cell[8] = angstrom;
    } else {
        double scale = (len + angstrom) / len;
        data.cell[o] *= scale;
        data.cell[o + 1] *= scale;
        data.cell[o + 2] *= scale;
    }
    finish(data, "Vacuum slab");
    data.bonds.clear();
    return data;
}

// Keep the lower fraction of the cell along one axis, then open vacuum above it.
inline Dataset cleaveAndVacuum(const Dataset &input, int axis, double keepFraction, double vacuum) {
    Dataset data = input;
    data.atoms.clear();
    axis = std::clamp(axis, 0, 2);
    keepFraction = std::clamp(keepFraction, 0.05, 0.95);
    for (const auto &atom : input.atoms) {
        double fa, fb, fc;
        if (!fractional(input, {atom.x, atom.y, atom.z}, fa, fb, fc)) continue;
        double f = axis == 0 ? fa : axis == 1 ? fb : fc;
        if (f >= 0 && f <= keepFraction) data.atoms.push_back(atom);
    }
    int o = axis * 3;
    data.cell[o] *= keepFraction;
    data.cell[o + 1] *= keepFraction;
    data.cell[o + 2] *= keepFraction;
    finish(data, "Cleaved surface");
    return addVacuum(data, axis, vacuum);
}

inline Dataset stackLayers(const Dataset &input, double gapAngstrom) {
    Dataset data = replicate(input, 1, 1, 2);
    auto cell = axes(input.cell);
    double len = length(cell.c);
    if (len < 1e-6) return data;
    Vec3 shift = scale(cell.c, gapAngstrom / len);
    size_t half = input.atoms.size();
    for (size_t i = half; i < data.atoms.size(); ++i) {
        data.atoms[i].x += shift.x;
        data.atoms[i].y += shift.y;
        data.atoms[i].z += shift.z;
    }
    double scaleC = (len * 2 + gapAngstrom) / (len * 2);
    data.cell[6] *= scaleC;
    data.cell[7] *= scaleC;
    data.cell[8] *= scaleC;
    finish(data, "Layer stack with vacuum gap");
    return data;
}

inline Dataset grapheneSheet(int nx, int ny) {
    nx = std::max(1, nx);
    ny = std::max(1, ny);
    const double bond = 1.42;
    const double a = bond * std::sqrt(3.0);
    Dataset data;
    data.pbc = {true, true, false};
    uint32_t carbon = speciesIndex(data, "C");
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            double x = i * a + (j % 2) * a * 0.5;
            double y = j * bond * 1.5;
            data.atoms.push_back({float(x), float(y), 0.f, carbon});
            data.atoms.push_back({float(x + bond * 0.5), float(y + bond * 0.86602540378), 0.f, carbon});
        }
    data.cell = {nx * a, 0, 0, 0, ny * bond * 1.5, 0, 0, 0, 20};
    finish(data, "Graphene sheet");
    return data;
}

// Zigzag (n,0) tube. Circumference n * 2.46 A, axial repeat about 4.26 A.
inline Dataset zigzagNanotube(int n, int rings) {
    n = std::max(4, n);
    rings = std::max(1, rings);
    const double bond = 1.42;
    const double circumference = n * bond * std::sqrt(3.0);
    const double radius = circumference / (2 * kPi);
    const double axial = bond * 3.0;
    Dataset data;
    data.pbc = {false, false, true};
    uint32_t carbon = speciesIndex(data, "C");
    for (int ring = 0; ring < rings; ++ring) {
        for (int i = 0; i < n; ++i) {
            double theta = 2 * kPi * i / n;
            double z0 = ring * axial;
            data.atoms.push_back({float(radius * std::cos(theta)), float(radius * std::sin(theta)),
                                  float(z0), carbon});
            double theta2 = theta + kPi / n;
            data.atoms.push_back(
                {float(radius * std::cos(theta2)), float(radius * std::sin(theta2)),
                 float(z0 + bond), carbon});
        }
    }
    double span = rings * axial + 2;
    data.cell = {radius * 2 + 8, 0, 0, 0, radius * 2 + 8, 0, 0, 0, span};
    data.origin = {float(-radius - 4), float(-radius - 4), 0};
    finish(data, "Zigzag carbon nanotube");
    return data;
}

inline Dataset fccNanoparticle(const std::string &element, double radius) {
    radius = std::clamp(radius, 2.0, 30.0);
    const double lattice = 3.615;
    int n = int(std::ceil(radius * 2 / lattice)) + 1;
    Dataset data;
    data.pbc = {false, false, false};
    uint32_t type = speciesIndex(data, element.empty() ? "Cu" : element);
    const double basis[4][3] = {{0, 0, 0}, {0, .5, .5}, {.5, 0, .5}, {.5, .5, 0}};
    Vec3 center{float(n * lattice * 0.5), float(n * lattice * 0.5), float(n * lattice * 0.5)};
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                for (auto &b : basis) {
                    Vec3 p{float((x + b[0]) * lattice), float((y + b[1]) * lattice),
                           float((z + b[2]) * lattice)};
                    if (length(sub(p, center)) <= radius) data.atoms.push_back({p.x, p.y, p.z, type});
                }
    data.cell = {n * lattice, 0, 0, 0, n * lattice, 0, 0, 0, n * lattice};
    finish(data, "FCC nanoparticle");
    return data;
}

inline float covalentOf(const Dataset &data, uint32_t type) {
    if (type < data.species.size())
        if (const auto *e = elements::find(data.species[type])) return e->covalent;
    return 0.75f;
}
inline int valenceOf(const Dataset &data, uint32_t type) {
    if (type >= data.species.size()) return 0;
    const auto *e = elements::find(data.species[type]);
    if (!e) return 0;
    switch (e->z) {
    case 1: return 1;
    case 6: return 4;
    case 7: return 3;
    case 8: return 2;
    case 9: return 1;
    case 14: return 4;
    case 15: return 3;
    case 16: return 2;
    default: return 0;
    }
}

inline int addHydrogens(Dataset &data) {
    if (data.atoms.size() > 2500) return -1;
    const uint32_t hydrogen = speciesIndex(data, "H");
    const size_t original = data.atoms.size();
    std::vector<Atom> extra;
    for (size_t i = 0; i < original; ++i) {
        int valence = valenceOf(data, data.atoms[i].type);
        if (valence <= 0 || data.atoms[i].type == hydrogen) continue;
        float ri = covalentOf(data, data.atoms[i].type);
        int neighbors = 0;
        Vec3 away{};
        for (size_t j = 0; j < original; ++j) {
            if (i == j) continue;
            Vec3 d = sub({data.atoms[j].x, data.atoms[j].y, data.atoms[j].z},
                         {data.atoms[i].x, data.atoms[i].y, data.atoms[i].z});
            double dist = length(d);
            float rj = covalentOf(data, data.atoms[j].type);
            if (dist < 1.25 * (ri + rj) && dist > 0.3) {
                ++neighbors;
                away = sub(away, scale(d, 1.0 / std::max(dist, 1e-3)));
            }
        }
        int missing = valence - neighbors;
        if (missing <= 0) continue;
        if (length(away) < 1e-3) away = {0, 0, 1};
        double norm = length(away);
        away = scale(away, 1.1 / norm);
        for (int h = 0; h < missing && h < 4; ++h) {
            Vec3 offset = away;
            if (h == 1) offset = {away.y, away.z, away.x};
            if (h == 2) offset = {away.z, away.x, away.y};
            if (h == 3) offset = scale(away, -1);
            extra.push_back({data.atoms[i].x + offset.x, data.atoms[i].y + offset.y,
                             data.atoms[i].z + offset.z, hydrogen});
        }
    }
    data.atoms.insert(data.atoms.end(), extra.begin(), extra.end());
    finish(data, data.comment.empty() ? "Hydrogens added" : data.comment);
    return int(extra.size());
}

inline bool cleanGeometry(Dataset &data) {
    const int passes = 8;
    const size_t n = data.atoms.size();
    if (n > 2500) return false;
    for (int pass = 0; pass < passes; ++pass) {
        std::vector<Vec3> delta(n);
        for (size_t i = 0; i < n; ++i) {
            float ri = covalentOf(data, data.atoms[i].type);
            for (size_t j = i + 1; j < n; ++j) {
                Vec3 d = sub({data.atoms[j].x, data.atoms[j].y, data.atoms[j].z},
                             {data.atoms[i].x, data.atoms[i].y, data.atoms[i].z});
                double dist = length(d);
                float target = covalentOf(data, data.atoms[j].type) + ri;
                if (dist < 0.35 || dist > target * 1.2) continue;
                Vec3 step = scale(d, 0.15 * (target - dist) / dist);
                delta[i] = sub(delta[i], step);
                delta[j] = add(delta[j], step);
            }
        }
        for (size_t i = 0; i < n; ++i) {
            data.atoms[i].x += delta[i].x;
            data.atoms[i].y += delta[i].y;
            data.atoms[i].z += delta[i].z;
        }
    }
    finish(data, data.comment.empty() ? "Geometry cleaned" : data.comment);
    return true;
}

inline Dataset placeWater(const Dataset &input, double height) {
    Dataset data = input;
    float top = data.atoms.empty() ? 0.f : data.atoms[0].z;
    double cx = 0, cy = 0;
    for (const auto &atom : data.atoms) {
        top = std::max(top, atom.z);
        cx += atom.x;
        cy += atom.y;
    }
    if (!data.atoms.empty()) {
        cx /= double(data.atoms.size());
        cy /= double(data.atoms.size());
    }
    float z = top + float(height);
    uint32_t oxygen = speciesIndex(data, "O");
    uint32_t hydrogen = speciesIndex(data, "H");
    data.atoms.push_back({float(cx), float(cy), z, oxygen});
    data.atoms.push_back({float(cx + 0.96), float(cy), z + 0.2f, hydrogen});
    data.atoms.push_back({float(cx - 0.24), float(cy + 0.93), z + 0.2f, hydrogen});
    finish(data, "Water placed above the structure");
    return data;
}

inline double distance(const Dataset &data, int i, int j) {
    if (i < 0 || j < 0 || size_t(i) >= data.atoms.size() || size_t(j) >= data.atoms.size()) return 0;
    return length(sub({data.atoms[i].x, data.atoms[i].y, data.atoms[i].z},
                      {data.atoms[j].x, data.atoms[j].y, data.atoms[j].z}));
}
inline double bondAngle(const Dataset &data, int i, int j, int k) {
    if (i < 0 || j < 0 || k < 0) return 0;
    if (size_t(std::max({i, j, k})) >= data.atoms.size()) return 0;
    Vec3 a = sub({data.atoms[i].x, data.atoms[i].y, data.atoms[i].z},
                 {data.atoms[j].x, data.atoms[j].y, data.atoms[j].z});
    Vec3 b = sub({data.atoms[k].x, data.atoms[k].y, data.atoms[k].z},
                 {data.atoms[j].x, data.atoms[j].y, data.atoms[j].z});
    return angleDegrees(a, b);
}
// Signed dihedral of atoms i-j-k-l, degrees. Zero when any index is invalid.
inline double dihedralAngle(const Dataset &data, int i, int j, int k, int l) {
    if (i < 0 || j < 0 || k < 0 || l < 0) return 0;
    if (size_t(std::max({i, j, k, l})) >= data.atoms.size()) return 0;
    auto at = [&](int n) { return Vec3{data.atoms[n].x, data.atoms[n].y, data.atoms[n].z}; };
    Vec3 b1 = sub(at(j), at(i));
    Vec3 b2 = sub(at(k), at(j));
    Vec3 b3 = sub(at(l), at(k));
    Vec3 n1 = cross(b1, b2);
    Vec3 n2 = cross(b2, b3);
    double x = length(b2) * dot(b1, n2);
    double y = dot(n1, n2);
    return std::atan2(x, y) * 180.0 / kPi;
}
} // namespace atomx::authoring
