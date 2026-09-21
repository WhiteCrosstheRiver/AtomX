#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace atomx {
struct Vec3 {
    float x = 0, y = 0, z = 0;
};
struct Atom {
    float x, y, z;
    uint32_t type;
};
static_assert(sizeof(Atom) == 16);
inline float &coordinate(Atom &a, int axis) {
    return axis == 0 ? a.x : axis == 1 ? a.y : a.z;
}
inline float coordinate(const Atom &a, int axis) {
    return axis == 0 ? a.x : axis == 1 ? a.y : a.z;
}
struct Frame {
    uint64_t count = 0;
    std::streamoff offset = 0;
    std::string comment;
};
struct Dataset {
    std::vector<Atom> atoms;
    std::vector<std::string> species;
    std::array<double, 9> cell{};
    std::array<bool, 3> pbc{};
    Vec3 lo{}, hi{};
    uint64_t sourceCount = 0, stride = 1;
    std::string comment;
    bool sampled() const {
        return stride > 1;
    }
    void bounds() {
        if (atoms.empty()) {
            lo = {};
            hi = {1, 1, 1};
            return;
        }
        lo = hi = {atoms[0].x, atoms[0].y, atoms[0].z};
        for (auto a : atoms) {
            lo.x = std::min(lo.x, a.x);
            lo.y = std::min(lo.y, a.y);
            lo.z = std::min(lo.z, a.z);
            hi.x = std::max(hi.x, a.x);
            hi.y = std::max(hi.y, a.y);
            hi.z = std::max(hi.z, a.z);
        }
    }
};
inline std::string attribute(const std::string &s, const std::string &key) {
    auto p = s.find(key + "=");
    if (p == std::string::npos)
        return {};
    p += key.size() + 1;
    if (p < s.size() && s[p] == '"') {
        auto e = s.find('"', p + 1);
        return s.substr(p + 1, e - p - 1);
    }
    auto e = s.find(' ', p);
    return s.substr(p, e - p);
}
inline std::vector<Frame> indexXYZ(const std::filesystem::path &path,
                                   std::atomic<float> *progress = nullptr,
                                   std::atomic<bool> *cancel = nullptr) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot open XYZ file");
    auto size = std::filesystem::file_size(path);
    std::vector<Frame> frames;
    std::string line;
    while (std::getline(f, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos)
            continue;
        Frame fr;
        std::istringstream n(line);
        std::string extra;
        if (!(n >> fr.count) || fr.count > 100000000000ULL || (n >> extra))
            throw std::runtime_error("Invalid XYZ atom count");
        if (!std::getline(f, fr.comment))
            throw std::runtime_error("Missing XYZ comment");
        fr.offset = f.tellg();
        for (uint64_t i = 0; i < fr.count; ++i) {
            if (!std::getline(f, line))
                throw std::runtime_error("Truncated XYZ frame");
            if ((i & 65535) == 0) {
                if (cancel && *cancel)
                    throw std::runtime_error("Cancelled");
                if (progress)
                    *progress = float(double(f.tellg()) / std::max<uint64_t>(size, 1));
            }
        }
        frames.push_back(fr);
    }
    if (frames.empty())
        throw std::runtime_error("No XYZ frames found");
    if (progress)
        *progress = 1;
    return frames;
}
inline Dataset readXYZ(const std::filesystem::path &path, const Frame &fr,
                       uint64_t budget = 2000000, std::atomic<float> *progress = nullptr,
                       std::atomic<bool> *cancel = nullptr) {
    if (!budget)
        throw std::runtime_error("Preview budget must be positive");
    Dataset d;
    d.sourceCount = fr.count;
    d.stride = std::max<uint64_t>(1, (fr.count + budget - 1) / budget);
    d.comment = fr.comment;
    std::istringstream lattice(attribute(fr.comment, "Lattice"));
    for (auto &v : d.cell)
        lattice >> v;
    std::istringstream periodic(attribute(fr.comment, "pbc"));
    for (auto &b : d.pbc) {
        std::string v;
        periodic >> v;
        b = v == "T" || v == "1" || v == "true";
    }
    int speciesCol = 0, posCol = 1;
    auto props = attribute(fr.comment, "Properties");
    if (!props.empty()) {
        std::replace(props.begin(), props.end(), ':', ' ');
        std::istringstream schema(props);
        std::string name, type;
        int width, col = 0;
        speciesCol = -1;
        posCol = -1;
        while (schema >> name >> type >> width) {
            if (width < 1 || width > 256)
                throw std::runtime_error("Invalid XYZ schema");
            if (name == "species" || name == "type")
                speciesCol = col;
            if (name == "pos" && width == 3)
                posCol = col;
            col += width;
        }
        if (posCol < 0)
            throw std::runtime_error("Extended XYZ requires pos:R:3");
    }
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot open XYZ");
    f.seekg(fr.offset);
    d.atoms.reserve(size_t((fr.count + d.stride - 1) / d.stride));
    std::unordered_map<std::string, uint32_t> types;
    std::string line;
    for (uint64_t i = 0; i < fr.count; ++i) {
        if (!std::getline(f, line))
            throw std::runtime_error("Truncated XYZ frame");
        if ((i & 65535) == 0) {
            if (cancel && *cancel)
                throw std::runtime_error("Cancelled");
            if (progress)
                *progress = float(double(i) / std::max<uint64_t>(fr.count, 1));
        }
        if (i % d.stride)
            continue;
        std::istringstream row(line);
        std::vector<std::string> tok;
        std::string t;
        while (row >> t)
            tok.push_back(t);
        if (tok.size() <= size_t(posCol + 2) ||
            (speciesCol >= 0 && tok.size() <= size_t(speciesCol)))
            throw std::runtime_error("Invalid atom row " + std::to_string(i + 1));
        auto parse = [](const std::string &s) {
            size_t end;
            float v = std::stof(s, &end);
            if (end != s.size() || !std::isfinite(v))
                throw std::runtime_error("Invalid coordinate");
            return v;
        };
        std::string species = speciesCol < 0 ? "X" : tok[speciesCol];
        auto [it, inserted] = types.emplace(species, uint32_t(types.size()));
        if (inserted)
            d.species.push_back(species);
        d.atoms.push_back(
            {parse(tok[posCol]), parse(tok[posCol + 1]), parse(tok[posCol + 2]), it->second});
    }
    d.bounds();
    if (progress)
        *progress = 1;
    return d;
}
inline Dataset crystal(int n = 32) {
    Dataset d;
    d.species = {"Cu", "Ni"};
    d.cell = {double(n), 0, 0, 0, double(n), 0, 0, 0, double(n)};
    const float basis[4][3] = {{0, 0, 0}, {0, .5f, .5f}, {.5f, 0, .5f}, {.5f, .5f, 0}};
    for (int z = 0; z < n; z++)
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                for (auto &b : basis)
                    d.atoms.push_back({x + b[0], y + b[1], z + b[2],
                                       uint32_t(x > n * .52f && z > n * .3f ? 1 : 0)});
    d.sourceCount = d.atoms.size();
    d.bounds();
    d.comment = "Generated FCC Cu/Ni specimen";
    return d;
}
enum class Op {
    Slice,
    SelectType,
    SelectIndex,
    Invert,
    Clear,
    Delete,
    Translate,
    Scale,
    Wrap,
    ColorType,
    Rotate,
    Replicate,
    EditType,
    SelectRange
};
struct Modifier {
    Op op;
    bool enabled = true;
    float value = 0;
    int axis = 2;
    int type = 0;
    float upper = 1;
};
inline const char *opName(Op op) {
    switch (op) {
    case Op::Rotate: return "Rotate";
    case Op::Replicate: return "Replicate";
    case Op::EditType: return "Edit particle types";
    case Op::SelectRange: return "Coordinate range selection";
    case Op::Slice:
        return "Slice";
    case Op::SelectType:
        return "Select type";
    case Op::SelectIndex:
        return "Manual selection";
    case Op::Invert:
        return "Invert selection";
    case Op::Clear:
        return "Clear selection";
    case Op::Delete:
        return "Delete selected";
    case Op::Translate:
        return "Translate";
    case Op::Scale:
        return "Uniform scale";
    case Op::Wrap:
        return "Wrap at periodic boundaries";
    default:
        return "Color by type";
    }
}
struct PipelineResult {
    Dataset data;
    std::vector<uint8_t> selected;
};
inline PipelineResult evaluate(const Dataset &source, const std::vector<Modifier> &mods) {
    PipelineResult r{source, std::vector<uint8_t>(source.atoms.size())};
    for (auto m : mods)
        if (m.enabled) {
            if (m.axis < 0 || m.axis > 2 || !std::isfinite(m.value) || !std::isfinite(m.upper))
                throw std::runtime_error("Invalid modifier parameters");
            if (m.op == Op::Scale && m.value <= 0)
                throw std::runtime_error("Scale must be positive");
            if (m.op == Op::EditType && (m.type < 0 || size_t(m.type) >= r.data.species.size()))
                throw std::runtime_error("Particle type is out of range");
            if (m.op == Op::SelectRange && m.upper < m.value)
                throw std::runtime_error("Upper bound must be at least the lower bound");
            if (m.op == Op::Replicate) {
                if (m.type < 1 || m.type > 32 || r.data.atoms.size() > 20000000 / size_t(m.type))
                    throw std::runtime_error("Replication exceeds the 20 million atom budget");
                size_t n = r.data.atoms.size();
                auto offset = m.axis * 3;
                if (r.data.cell[offset] == 0 && r.data.cell[offset+1] == 0 && r.data.cell[offset+2] == 0)
                    throw std::runtime_error("Replication requires a nonzero simulation cell vector");
                r.data.atoms.reserve(n * m.type); r.selected.reserve(n * m.type);
                for (int copy = 1; copy < m.type; ++copy)
                    for (size_t i = 0; i < n; ++i) {
                        auto a = r.data.atoms[i];
                        a.x += float(copy * r.data.cell[offset]);
                        a.y += float(copy * r.data.cell[offset+1]);
                        a.z += float(copy * r.data.cell[offset+2]);
                        r.data.atoms.push_back(a); r.selected.push_back(r.selected[i]);
                    }
                for (int k=0;k<3;++k) r.data.cell[offset+k] *= m.type;
                continue;
            }
            if (m.op == Op::Wrap &&
                (r.data.cell[1] != 0 || r.data.cell[2] != 0 || r.data.cell[3] != 0 ||
                 r.data.cell[5] != 0 || r.data.cell[6] != 0 || r.data.cell[7] != 0))
                throw std::runtime_error("Wrap currently requires an orthogonal cell");
            size_t out = 0;
            for (size_t i = 0; i < r.data.atoms.size(); ++i) {
                auto a = r.data.atoms[i];
                bool sel = r.selected[i], keep = true;
                switch (m.op) {
                case Op::SelectRange:
                    sel = coordinate(a,m.axis) >= m.value && coordinate(a,m.axis) <= m.upper;
                    break;
                case Op::EditType:
                    if (sel) a.type = uint32_t(m.type);
                    break;
                case Op::Rotate: {
                    int u = (m.axis+1)%3, v = (m.axis+2)%3;
                    float angle = m.value * 0.0174532925199433f;
                    float x=coordinate(a,u), y=coordinate(a,v);
                    coordinate(a,u)=x*std::cos(angle)-y*std::sin(angle);
                    coordinate(a,v)=x*std::sin(angle)+y*std::cos(angle);
                    break;
                }
                case Op::Slice:
                    keep = coordinate(a, m.axis) <= m.value;
                    break;
                case Op::SelectType:
                    sel = a.type == uint32_t(m.type);
                    break;
                case Op::SelectIndex:
                    sel = i == size_t(m.type);
                    break;
                case Op::Invert:
                    sel = !sel;
                    break;
                case Op::Clear:
                    sel = false;
                    break;
                case Op::Delete:
                    keep = !sel;
                    break;
                case Op::Translate:
                    coordinate(a, m.axis) += m.value;
                    break;
                case Op::Scale:
                    a.x *= m.value;
                    a.y *= m.value;
                    a.z *= m.value;
                    break;
                case Op::Wrap:
                    for (int k = 0; k < 3; k++)
                        if (r.data.pbc[k] && r.data.cell[k * 4] > 0)
                            coordinate(a, k) -=
                                float(std::floor(coordinate(a, k) / r.data.cell[k * 4]) *
                                      r.data.cell[k * 4]);
                    break;
                default:
                    break;
                }
                if (keep) {
                    r.data.atoms[out] = a;
                    r.selected[out++] = sel;
                }
            }
            r.data.atoms.resize(out);
            r.selected.resize(out);
            if (m.op == Op::Rotate) {
                int u = (m.axis+1)%3, v = (m.axis+2)%3;
                double angle = m.value * 0.0174532925199433;
                for (int row=0;row<3;++row) {
                    double x=r.data.cell[row*3+u], y=r.data.cell[row*3+v];
                    r.data.cell[row*3+u]=x*std::cos(angle)-y*std::sin(angle);
                    r.data.cell[row*3+v]=x*std::sin(angle)+y*std::cos(angle);
                }
            }
            if (m.op == Op::Scale)
                for (auto &v : r.data.cell)
                    v *= m.value;
        }
    r.data.bounds();
    return r;
}
struct Statistics {
    double min = 0, max = 0, mean = 0;
    std::array<float, 64> histogram{};
};
inline Statistics statistics(const Dataset &d, int axis) {
    Statistics s;
    if (d.atoms.empty())
        return s;
    s.min = s.max = coordinate(d.atoms[0], axis);
    for (auto a : d.atoms) {
        double v = coordinate(a, axis);
        s.min = std::min(s.min, v);
        s.max = std::max(s.max, v);
        s.mean += v;
    }
    s.mean /= d.atoms.size();
    for (auto a : d.atoms) {
        int b = int((coordinate(a, axis) - s.min) / std::max(s.max - s.min, 1e-12) * 63);
        s.histogram[std::clamp(b, 0, 63)]++;
    }
    return s;
}
inline void writeXYZ(const std::filesystem::path &p, const Dataset &d) {
    std::ofstream f(p);
    if (!f)
        throw std::runtime_error("Cannot write XYZ");
    f << d.atoms.size() << "\nLattice=\"";
    for (int i = 0; i < 9; i++)
        f << d.cell[i] << (i == 8 ? "" : " ");
    f << "\" Properties=species:S:1:pos:R:3 pbc=\"";
    for (int i = 0; i < 3; i++)
        f << (d.pbc[i] ? "T" : "F") << (i == 2 ? "" : " ");
    f << "\"\n";
    f.precision(9);
    for (auto a : d.atoms)
        f << d.species.at(a.type) << ' ' << a.x << ' ' << a.y << ' ' << a.z << '\n';
    if (!f)
        throw std::runtime_error("XYZ write failed");
}
} // namespace atomx
