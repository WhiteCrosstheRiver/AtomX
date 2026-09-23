#pragma once
#include "core.hpp"
namespace atomx {
struct MeshTriangle {
    std::array<float, 4> a, b, c;
};
inline std::vector<MeshTriangle> readParticleOBJ(const std::filesystem::path &path) {
    std::ifstream file(path);
    if (!file)
        throw std::runtime_error("Cannot open particle OBJ");
    std::vector<Vec3> vertices;
    std::vector<std::array<size_t, 3>> faces;
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream row(line);
        std::string tag;
        row >> tag;
        if (tag == "v") {
            Vec3 v;
            if (!(row >> v.x >> v.y >> v.z) || !std::isfinite(v.x) || !std::isfinite(v.y) ||
                !std::isfinite(v.z))
                throw std::runtime_error("Invalid OBJ vertex");
            vertices.push_back(v);
            if (vertices.size() > 100000)
                throw std::runtime_error("Particle OBJ has too many vertices");
        }
        if (tag == "f") {
            std::vector<size_t> polygon;
            std::string token;
            while (row >> token) {
                if (token[0] == '#')
                    break;
                auto id = token.substr(0, token.find('/'));
                size_t used = 0;
                long long value = std::stoll(id, &used);
                if (used != id.size() || value == 0)
                    throw std::runtime_error("Invalid OBJ vertex index");
                long long index =
                    value > 0 ? value - 1 : static_cast<long long>(vertices.size()) + value;
                if (index < 0 || size_t(index) >= vertices.size())
                    throw std::runtime_error("OBJ face index out of range");
                polygon.push_back(size_t(index));
            }
            // Triangle-only input prevents incorrect triangulation of concave polygons.
            if (polygon.size() != 3)
                throw std::runtime_error("Particle OBJ must be triangulated before import");
            faces.push_back({polygon[0], polygon[1], polygon[2]});
            if (faces.size() > 256)
                throw std::runtime_error(
                    "Particle mesh renderer limit is 256 triangles; simplify the OBJ first");
        }
    }
    if (vertices.empty() || faces.empty())
        throw std::runtime_error("OBJ has no triangles");
    Vec3 lo = vertices.front(), hi = lo;
    for (auto v : vertices) {
        lo.x = std::min(lo.x, v.x);
        lo.y = std::min(lo.y, v.y);
        lo.z = std::min(lo.z, v.z);
        hi.x = std::max(hi.x, v.x);
        hi.y = std::max(hi.y, v.y);
        hi.z = std::max(hi.z, v.z);
    }
    Vec3 center{(lo.x + hi.x) * .5f, (lo.y + hi.y) * .5f, (lo.z + hi.z) * .5f};
    float extent = 0;
    for (auto &v : vertices) {
        v.x -= center.x;
        v.y -= center.y;
        v.z -= center.z;
        extent = std::max(extent, std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z));
    }
    if (!(extent > 0) && std::isfinite(extent))
        throw std::runtime_error("Degenerate OBJ");
    if (!std::isfinite(extent))
        throw std::runtime_error("OBJ coordinate range too large");
    for (auto &v : vertices) {
        v.x /= extent;
        v.y /= extent;
        v.z /= extent;
    }
    std::vector<MeshTriangle> result;
    for (auto f : faces) {
        auto a = vertices[f[0]], b = vertices[f[1]], c = vertices[f[2]];
        Vec3 u{b.x - a.x, b.y - a.y, b.z - a.z}, v{c.x - a.x, c.y - a.y, c.z - a.z};
        Vec3 n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
        if (n.x * n.x + n.y * n.y + n.z * n.z < 1e-16f)
            throw std::runtime_error("OBJ contains a degenerate triangle");
        result.push_back({{a.x, a.y, a.z, 0}, {b.x, b.y, b.z, 0}, {c.x, c.y, c.z, 0}});
    }
    return result;
}
} // namespace atomx
