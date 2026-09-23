#pragma once
#include "core.hpp"
#include <cctype>
#include <locale>
#include <optional>

namespace atomx::io {
enum class Format { XYZ, POSCAR, CIF, LammpsData, LammpsDump, PDB, GRO };
struct FormatInfo {
    Format id;
    const char *name;
    const char *extension;
    bool trajectory;
    bool writable;
};
inline constexpr FormatInfo formats[] = {
    {Format::XYZ, "XYZ / Extended XYZ", "xyz", true, true},
    {Format::POSCAR, "VASP POSCAR / CONTCAR", "vasp", false, true},
    {Format::CIF, "CIF (P1 structure)", "cif", false, true},
    {Format::LammpsData, "LAMMPS data (atomic)", "data", false, true},
    {Format::LammpsDump, "LAMMPS text dump", "dump", true, true},
    {Format::PDB, "PDB coordinates", "pdb", false, false},
    {Format::GRO, "GROMACS GRO", "gro", false, true}};
inline std::string trim(std::string s) {
    auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
inline std::vector<std::string> tokens(const std::string &s) {
    std::istringstream f(s);
    f.imbue(std::locale::classic());
    std::vector<std::string> v;
    std::string t;
    while (f >> t)
        v.push_back(t);
    return v;
}
inline double number(const std::string &s) {
    std::istringstream f(s);
    f.imbue(std::locale::classic());
    double v;
    std::string extra;
    if (!(f >> v) || (f >> extra) || !std::isfinite(v))
        throw std::runtime_error("Invalid numeric value: " + s);
    return v;
}
inline uint64_t count(const std::string &s) {
    auto v = trim(s);
    if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("Invalid count: " + s);
    return std::stoull(v);
}
inline std::string line(std::istream &f) {
    std::string s;
    if (!std::getline(f, s))
        throw std::runtime_error("Truncated structure file");
    return trim(s);
}
inline void checkpoint(std::atomic<bool> *cancel) {
    if (cancel && *cancel)
        throw std::runtime_error("Cancelled");
}
inline uint32_t species(Dataset &d, const std::string &name) {
    auto it = std::find(d.species.begin(), d.species.end(), name);
    if (it != d.species.end())
        return uint32_t(it - d.species.begin());
    d.species.push_back(name);
    return uint32_t(d.species.size() - 1);
}
inline void atom(Dataset &d, double x, double y, double z, const std::string &symbol) {
    Atom a{float(x), float(y), float(z), species(d, symbol)};
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(a.z))
        throw std::runtime_error("Coordinates exceed float range");
    d.atoms.push_back(a);
}
inline double determinant(const std::array<double, 9> &c) {
    return c[0] * (c[4] * c[8] - c[5] * c[7]) - c[1] * (c[3] * c[8] - c[5] * c[6]) +
           c[2] * (c[3] * c[7] - c[4] * c[6]);
}
inline std::array<double, 3> fractional(const Dataset &d, const Atom &a) {
    const auto &c = d.cell;
    double det = determinant(c);
    if (!std::isfinite(det) || std::abs(det) < 1e-15)
        throw std::runtime_error("Format requires a non-degenerate simulation cell");
    double x = a.x - d.origin.x, y = a.y - d.origin.y, z = a.z - d.origin.z;
    return {(x * (c[4] * c[8] - c[5] * c[7]) + y * (c[5] * c[6] - c[3] * c[8]) +
             z * (c[3] * c[7] - c[4] * c[6])) /
                det,
            (x * (c[7] * c[2] - c[8] * c[1]) + y * (c[8] * c[0] - c[6] * c[2]) +
             z * (c[6] * c[1] - c[7] * c[0])) /
                det,
            (x * (c[1] * c[5] - c[2] * c[4]) + y * (c[2] * c[3] - c[0] * c[5]) +
             z * (c[0] * c[4] - c[1] * c[3])) /
                det};
}
inline std::array<double, 9> cellFromParameters(double a, double b, double c, double alpha,
                                                double beta, double gamma) {
    constexpr double rad = 3.14159265358979323846 / 180.;
    if (a <= 0 || b <= 0 || c <= 0 || alpha <= 0 || alpha >= 180 || beta <= 0 || beta >= 180 ||
        gamma <= 0 || gamma >= 180)
        throw std::runtime_error("Invalid cell parameters");
    double ca = std::cos(alpha * rad), cb = std::cos(beta * rad), cg = std::cos(gamma * rad),
           sg = std::sin(gamma * rad);
    double cy = (ca - cb * cg) / sg, zz = 1 - cb * cb - cy * cy;
    if (zz <= 0)
        throw std::runtime_error("Degenerate cell angles");
    return {a, 0, 0, b * cg, b * sg, 0, c * cb, c * cy, c * std::sqrt(zz)};
}
inline Format detect(const std::filesystem::path &path) {
    auto ext = lowerExtension(path);
    if (ext == ".gz" || ext == ".zst")
        throw std::runtime_error(
            "Compressed input is not supported yet; decompress the file first");
    if (isPOSCAR(path))
        return Format::POSCAR;
    if (ext == ".cif")
        return Format::CIF;
    if (ext == ".data" || ext == ".lmp")
        return Format::LammpsData;
    if (ext == ".dump" || ext == ".lammpstrj")
        return Format::LammpsDump;
    if (ext == ".pdb" || ext == ".ent")
        return Format::PDB;
    if (ext == ".gro")
        return Format::GRO;
    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("Cannot open input file");
    std::string s;
    std::getline(f, s);
    if (trim(s) == "ITEM: TIMESTEP")
        return Format::LammpsDump;
    if (ext == ".xyz" || ext == ".extxyz" ||
        (!trim(s).empty() && trim(s).find_first_not_of("0123456789") == std::string::npos))
        return Format::XYZ;
    throw std::runtime_error(
        "Unrecognized structure format; use XYZ, POSCAR, CIF, LAMMPS data/dump, PDB or GRO");
}
inline const FormatInfo &info(Format id) {
    for (const auto &f : formats)
        if (f.id == id)
            return f;
    throw std::runtime_error("Unknown format");
}

struct DumpHeader {
    uint64_t n;
    Dataset data;
    std::vector<std::string> columns;
};
inline DumpHeader dumpHeader(std::istream &f) {
    if (line(f) != "ITEM: TIMESTEP")
        throw std::runtime_error("Expected LAMMPS TIMESTEP");
    DumpHeader h;
    h.data.comment = "Timestep=" + line(f);
    if (line(f) != "ITEM: NUMBER OF ATOMS")
        throw std::runtime_error("Only LAMMPS atom dump is supported");
    h.n = count(line(f));
    auto bounds = tokens(line(f));
    if (bounds.size() < 6 || bounds[0] != "ITEM:" || bounds[1] != "BOX" || bounds[2] != "BOUNDS")
        throw std::runtime_error("Invalid LAMMPS box header");
    bool tilted = bounds[3] == "xy";
    if (bounds[3] == "abc")
        throw std::runtime_error("General triclinic dump is not supported yet");
    size_t start = tilted ? 6 : 3;
    if (bounds.size() != start + 3)
        throw std::runtime_error("Invalid LAMMPS boundary flags");
    for (int k = 0; k < 3; ++k)
        h.data.pbc[k] = bounds[start + k] == "pp";
    double lo[3], hi[3], tilt[3]{};
    for (int k = 0; k < 3; ++k) {
        auto v = tokens(line(f));
        if (v.size() != size_t(tilted ? 3 : 2))
            throw std::runtime_error("Invalid box bounds row");
        lo[k] = number(v[0]);
        hi[k] = number(v[1]);
        if (tilted)
            tilt[k] = number(v[2]);
    }
    double xy = tilt[0], xz = tilt[1], yz = tilt[2];
    lo[0] -= std::min({0., xy, xz, xy + xz});
    hi[0] -= std::max({0., xy, xz, xy + xz});
    lo[1] -= std::min(0., yz);
    hi[1] -= std::max(0., yz);
    for (int k = 0; k < 3; ++k)
        if (!(hi[k] > lo[k]))
            throw std::runtime_error("Invalid box extent");
    h.data.cell = {hi[0] - lo[0], 0, 0, xy, hi[1] - lo[1], 0, xz, yz, hi[2] - lo[2]};
    h.data.origin = {float(lo[0]), float(lo[1]), float(lo[2])};
    h.columns = tokens(line(f));
    if (h.columns.size() < 3 || h.columns[0] != "ITEM:" || h.columns[1] != "ATOMS")
        throw std::runtime_error("Expected LAMMPS atom columns");
    h.columns.erase(h.columns.begin(), h.columns.begin() + 2);
    return h;
}
inline std::vector<Frame> index(const std::filesystem::path &path,
                                std::atomic<float> *progress = nullptr,
                                std::atomic<bool> *cancel = nullptr) {
    auto fmt = detect(path);
    if (fmt == Format::XYZ)
        return indexXYZ(path, progress, cancel);
    if (fmt != Format::LammpsDump)
        return {Frame{0, 0, info(fmt).name}};
    std::ifstream f(path, std::ios::binary);
    std::vector<Frame> frames;
    auto size = std::filesystem::file_size(path);
    while (f.peek() != EOF) {
        checkpoint(cancel);
        auto offset = f.tellg();
        auto h = dumpHeader(f);
        for (uint64_t i = 0; i < h.n; ++i) {
            line(f);
            if ((i & 65535) == 0)
                checkpoint(cancel);
        }
        frames.push_back({h.n, offset, h.data.comment});
        if (progress)
            *progress = float(double(f.tellg()) / std::max<uint64_t>(size, 1));
        while (f.peek() == '\r' || f.peek() == '\n')
            f.get();
    }
    if (frames.empty())
        throw std::runtime_error("Empty trajectory");
    if (progress)
        *progress = 1;
    return frames;
}
inline Dataset readDump(const std::filesystem::path &path, const Frame &frame, uint64_t budget,
                        std::atomic<float> *progress, std::atomic<bool> *cancel) {
    if (!budget)
        throw std::runtime_error("Budget must be positive");
    std::ifstream f(path, std::ios::binary);
    f.seekg(frame.offset);
    auto h = dumpHeader(f);
    auto d = std::move(h.data);
    d.sourceCount = h.n;
    d.stride = std::max<uint64_t>(1, h.n / budget + (h.n % budget != 0));
    auto column = [&](const char *name) {
        auto it = std::find(h.columns.begin(), h.columns.end(), name);
        return it == h.columns.end() ? -1 : int(it - h.columns.begin());
    };
    int x = column("x"), y = column("y"), z = column("z");
    bool scaled = false;
    if (x < 0 || y < 0 || z < 0) {
        x = column("xu");
        y = column("yu");
        z = column("zu");
    }
    if (x < 0 || y < 0 || z < 0) {
        x = column("xs");
        y = column("ys");
        z = column("zs");
        scaled = true;
    }
    if (x < 0 || y < 0 || z < 0)
        throw std::runtime_error("Dump needs x/y/z, xu/yu/zu or xs/ys/zs");
    int t = column("type"), element = column("element");
    for (uint64_t i = 0; i < h.n; ++i) {
        auto row = tokens(line(f));
        if (row.size() != h.columns.size())
            throw std::runtime_error("Dump row width mismatch");
        if ((i & 65535) == 0) {
            checkpoint(cancel);
            if (progress)
                *progress = float(double(i) / std::max<uint64_t>(h.n, 1));
        }
        // Build a stable type list even when a row is not resident in the preview.
        std::string name = element >= 0 ? row[element] : t >= 0 ? "Type_" + row[t] : "X";
        species(d, name);
        if (i % d.stride)
            continue;
        double a = number(row[x]), b = number(row[y]), c = number(row[z]);
        if (scaled)
            atom(d, d.origin.x + a * d.cell[0] + b * d.cell[3] + c * d.cell[6],
                 d.origin.y + a * d.cell[1] + b * d.cell[4] + c * d.cell[7],
                 d.origin.z + a * d.cell[2] + b * d.cell[5] + c * d.cell[8], name);
        else
            atom(d, a, b, c, name);
        for (size_t k = 0; k < row.size(); ++k)
            if (int(k) != x && int(k) != y && int(k) != z && int(k) != element) {
                double value = number(row[k]);
                if (h.columns[k] == "id" &&
                    (value < 0 || value > 9007199254740991. || std::floor(value) != value))
                    throw std::runtime_error("Atom ID exceeds exact integer property range");
                d.scalarProperties[h.columns[k]].push_back(value);
            }
    }
    d.bounds();
    if (progress)
        *progress = 1;
    return d;
}
inline Dataset readGRO(const std::filesystem::path &path, std::atomic<bool> *cancel) {
    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("Cannot open GRO");
    Dataset d;
    d.comment = line(f);
    auto n = count(line(f));
    if (n > 20000000)
        throw std::runtime_error("GRO reader limit: 20 million atoms");
    bool velocities = false;
    for (uint64_t i = 0; i < n; ++i) {
        checkpoint(cancel);
        std::string s;
        if (!std::getline(f, s) || s.size() < 44)
            throw std::runtime_error("Invalid GRO fixed-width atom row");
        // GRO coordinates are nanometers; AtomX uses Angstroms.
        atom(d, 10 * number(s.substr(20, 8)), 10 * number(s.substr(28, 8)),
             10 * number(s.substr(36, 8)), trim(s.substr(10, 5)));
        bool has = s.size() >= 68 && !trim(s.substr(44)).empty();
        if (i == 0)
            velocities = has;
        if (has != velocities)
            throw std::runtime_error("Inconsistent GRO velocity columns");
        if (has)
            d.vectorProperties["Velocity"].push_back({float(10 * number(s.substr(44, 8))),
                                                      float(10 * number(s.substr(52, 8))),
                                                      float(10 * number(s.substr(60, 8)))});
    }
    auto box = tokens(line(f));
    if (box.size() != 3 && box.size() != 9)
        throw std::runtime_error("GRO box needs 3 or 9 values");
    d.cell[0] = number(box[0]) * 10;
    d.cell[4] = number(box[1]) * 10;
    d.cell[8] = number(box[2]) * 10;
    if (box.size() == 9) {
        int indices[] = {1, 2, 3, 5, 6, 7};
        for (int k = 0; k < 6; ++k)
            d.cell[indices[k]] = number(box[k + 3]) * 10;
    }
    d.pbc = {d.cell[0] != 0, d.cell[4] != 0, d.cell[8] != 0};
    std::string trailing;
    while (std::getline(f, trailing))
        if (!trim(trailing).empty())
            throw std::runtime_error("Concatenated GRO trajectories are not supported yet");
    d.sourceCount = n;
    d.bounds();
    return d;
}
inline Dataset readPDB(const std::filesystem::path &path, std::atomic<bool> *cancel) {
    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("Cannot open PDB");
    Dataset d;
    std::string s;
    int models = 0;
    while (std::getline(f, s)) {
        checkpoint(cancel);
        if (s.rfind("MODEL ", 0) == 0 && ++models > 1)
            throw std::runtime_error("PDB reader currently accepts one model per file");
        if (s.rfind("CRYST1", 0) == 0) {
            if (s.size() < 54)
                throw std::runtime_error("Truncated PDB CRYST1");
            d.cell =
                cellFromParameters(number(trim(s.substr(6, 9))), number(trim(s.substr(15, 9))),
                                   number(trim(s.substr(24, 9))), number(trim(s.substr(33, 7))),
                                   number(trim(s.substr(40, 7))), number(trim(s.substr(47, 7))));
            d.pbc = {true, true, true};
        }
        if (s.rfind("ATOM  ", 0) != 0 && s.rfind("HETATM", 0) != 0)
            continue;
        if (s.size() < 54)
            throw std::runtime_error("Truncated PDB coordinate");
        if (s[16] != ' ' && s[16] != 'A')
            continue; // primary alternate location
        std::string name = s.size() >= 78 ? trim(s.substr(76, 2)) : "";
        if (name.empty()) {
            name = trim(s.substr(12, 4));
            if (!name.empty() && std::isdigit(static_cast<unsigned char>(name.front())))
                name.erase(0, 1);
            name = name.substr(0, s[12] == ' ' ? 1 : 2);
        }
        if (name.empty())
            name = "X";
        atom(d, number(trim(s.substr(30, 8))), number(trim(s.substr(38, 8))),
             number(trim(s.substr(46, 8))), name);
        if (d.atoms.size() > 20000000)
            throw std::runtime_error("PDB reader limit: 20 million atoms");
    }
    if (d.atoms.empty())
        throw std::runtime_error("No PDB atoms");
    d.sourceCount = d.atoms.size();
    d.comment = "PDB coordinates (residue topology is not imported)";
    d.bounds();
    return d;
}
inline Dataset readAtomicData(const std::filesystem::path &path, std::atomic<bool> *cancel) {
    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("Cannot open LAMMPS data");
    Dataset d;
    d.comment = line(f);
    std::string s;
    uint64_t expected = 0;
    bool inAtoms = false;
    bool bounds[3]{};
    while (std::getline(f, s)) {
        checkpoint(cancel);
        s = trim(s);
        if (s.empty())
            continue;
        if (s.rfind("Atoms", 0) == 0) {
            auto hash = s.find('#');
            if (hash != std::string::npos && trim(s.substr(hash + 1)) != "atomic")
                throw std::runtime_error(
                    "LAMMPS data reader currently supports Atoms # atomic only");
            inAtoms = true;
            continue;
        }
        auto hash = s.find('#');
        if (hash != std::string::npos)
            s = s.substr(0, hash);
        auto v = tokens(s);
        if (v.empty())
            continue;
        if (inAtoms) {
            if (std::isalpha(static_cast<unsigned char>(v[0][0]))) {
                inAtoms = false;
                continue;
            }
            if (v.size() != 5 && v.size() != 8)
                throw std::runtime_error("Atomic LAMMPS row must have id type x y z [ix iy iz]");
            auto id = count(v[0]), type = count(v[1]);
            if (!type || id > 9007199254740991ULL)
                throw std::runtime_error("Invalid LAMMPS id/type");
            atom(d, number(v[2]), number(v[3]), number(v[4]), "Type_" + v[1]);
            d.scalarProperties["id"].push_back(double(id));
            continue;
        }
        if (v.size() == 2 && v[1] == "atoms") {
            expected = count(v[0]);
            if (expected > 20000000)
                throw std::runtime_error("LAMMPS data reader limit: 20 million atoms");
        }
        if (v.size() == 4) {
            for (int k = 0; k < 3; ++k) {
                std::string axis(1, "xyz"[k]);
                if (v[2] == axis + "lo" && v[3] == axis + "hi") {
                    double lo = number(v[0]), hi = number(v[1]);
                    if (hi <= lo)
                        throw std::runtime_error("Invalid LAMMPS cell");
                    if (k == 0)
                        d.origin.x = float(lo);
                    else if (k == 1)
                        d.origin.y = float(lo);
                    else
                        d.origin.z = float(lo);
                    d.cell[k * 4] = hi - lo;
                    bounds[k] = true;
                }
            }
        }
        if (v.size() == 6 && v[3] == "xy" && v[4] == "xz" && v[5] == "yz") {
            d.cell[3] = number(v[0]);
            d.cell[6] = number(v[1]);
            d.cell[7] = number(v[2]);
        }
    }
    if (d.atoms.size() != expected || !bounds[0] || !bounds[1] || !bounds[2])
        throw std::runtime_error("LAMMPS atom count or cell bounds missing/mismatched");
    // LAMMPS data has no boundary flags; do not invent periodicity.
    d.sourceCount = expected;
    d.bounds();
    return d;
}
inline Dataset readCifP1(const std::filesystem::path &path, std::atomic<bool> *cancel) {
    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("Cannot open CIF");
    // CIF lexical tokens: preserve quoted values and ignore comments outside quotes.
    std::vector<std::string> all;
    std::string s;
    while (std::getline(f, s)) {
        checkpoint(cancel);
        if (!s.empty() && s.front() == ';')
            throw std::runtime_error("CIF multiline fields are not supported by this reader");
        size_t i = 0;
        while (i < s.size()) {
            if (std::isspace(static_cast<unsigned char>(s[i]))) {
                ++i;
                continue;
            }
            if (s[i] == '#')
                break;
            size_t start = i;
            char q = s[i];
            if (q == '\'' || q == '\"') {
                start = ++i;
                while (i < s.size() && s[i] != q)
                    ++i;
                if (i == s.size())
                    throw std::runtime_error("Unterminated CIF quote");
                all.push_back(s.substr(start, i - start));
                ++i;
            } else {
                while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) &&
                       s[i] != '#')
                    ++i;
                all.push_back(s.substr(start, i - start));
            }
        }
    }
    std::map<std::string, std::string> fields;
    std::vector<std::string> headers, rows;
    auto control = [](const std::string &t) {
        return t == "loop_" || t.rfind("data_", 0) == 0 || (!t.empty() && t[0] == '_');
    };
    for (size_t i = 0; i < all.size();) {
        if (all[i] == "loop_") {
            ++i;
            std::vector<std::string> h;
            while (i < all.size() && !all[i].empty() && all[i][0] == '_')
                h.push_back(all[i++]);
            std::vector<std::string> r;
            while (i < all.size() && !control(all[i]))
                r.push_back(all[i++]);
            if (h.empty() || r.size() % h.size())
                throw std::runtime_error("CIF loop row mismatch");
            if (std::find(h.begin(), h.end(), "_atom_site_fract_x") != h.end()) {
                if (!headers.empty())
                    throw std::runtime_error("Multiple CIF atom blocks are not supported");
                headers = std::move(h);
                rows = std::move(r);
            } else
                for (size_t k = 0; k < h.size(); ++k)
                    if (h[k] == "_space_group_symop_operation_xyz" ||
                        h[k] == "_symmetry_equiv_pos_as_xyz")
                        for (size_t j = k; j < r.size(); j += h.size()) {
                            auto op = r[j];
                            op.erase(std::remove(op.begin(), op.end(), ' '), op.end());
                            if (op != "x,y,z")
                                throw std::runtime_error("CIF symmetry expansion is not supported; "
                                                         "export a P1 structure first");
                        }
        } else if (!all[i].empty() && all[i][0] == '_') {
            if (i + 1 == all.size())
                throw std::runtime_error("Missing CIF field value");
            fields[all[i]] = all[i + 1];
            i += 2;
        } else
            ++i;
    }
    for (const char *name : {"_space_group_IT_number", "_symmetry_Int_Tables_number"})
        if (fields.count(name) && fields[name] != "1")
            throw std::runtime_error("Only CIF P1 symmetry is supported");
    for (const char *name : {"_symmetry_space_group_name_H-M", "_space_group_name_H-M_alt"})
        if (fields.count(name)) {
            auto group = fields[name];
            group.erase(std::remove(group.begin(), group.end(), ' '), group.end());
            if (group != "P1")
                throw std::runtime_error("Only CIF P1 symmetry is supported");
        }
    auto numeric = [](std::string v) {
        auto p = v.find('(');
        if (p != std::string::npos)
            v = v.substr(0, p);
        return number(v);
    };
    Dataset d;
    d.cell = cellFromParameters(
        numeric(fields.at("_cell_length_a")), numeric(fields.at("_cell_length_b")),
        numeric(fields.at("_cell_length_c")),
        fields.count("_cell_angle_alpha") ? numeric(fields["_cell_angle_alpha"]) : 90,
        fields.count("_cell_angle_beta") ? numeric(fields["_cell_angle_beta"]) : 90,
        fields.count("_cell_angle_gamma") ? numeric(fields["_cell_angle_gamma"]) : 90);
    auto col = [&](const char *name) {
        auto it = std::find(headers.begin(), headers.end(), name);
        return it == headers.end() ? -1 : int(it - headers.begin());
    };
    int x = col("_atom_site_fract_x"), y = col("_atom_site_fract_y"), z = col("_atom_site_fract_z"),
        t = col("_atom_site_type_symbol"), label = col("_atom_site_label"),
        occ = col("_atom_site_occupancy");
    if (x < 0 || y < 0 || z < 0 || (t < 0 && label < 0))
        throw std::runtime_error("CIF needs fractional positions and type symbols/labels");
    for (size_t i = 0; i < rows.size(); i += headers.size()) {
        checkpoint(cancel);
        if (occ >= 0 && numeric(rows[i + occ]) != 1.)
            throw std::runtime_error("Partial CIF occupancies are not supported");
        auto symbol = rows[i + (t >= 0 ? t : label)];
        if (t < 0)
            symbol = symbol.substr(0, symbol.find_first_of("0123456789_"));
        double a = numeric(rows[i + x]), b = numeric(rows[i + y]), c = numeric(rows[i + z]);
        atom(d, a * d.cell[0] + b * d.cell[3] + c * d.cell[6],
             a * d.cell[1] + b * d.cell[4] + c * d.cell[7],
             a * d.cell[2] + b * d.cell[5] + c * d.cell[8], symbol);
    }
    d.sourceCount = d.atoms.size();
    d.pbc = {true, true, true};
    d.comment = "CIF P1";
    d.bounds();
    return d;
}
inline Dataset read(const std::filesystem::path &path, const Frame &frame,
                    uint64_t budget = 2000000, std::atomic<float> *progress = nullptr,
                    std::atomic<bool> *cancel = nullptr) {
    checkpoint(cancel);
    auto fmt = detect(path);
    if (fmt == Format::XYZ)
        return readXYZ(path, frame, budget, progress, cancel);
    if (fmt == Format::LammpsDump)
        return readDump(path, frame, budget, progress, cancel);
    Dataset d = fmt == Format::PDB          ? readPDB(path, cancel)
                : fmt == Format::GRO        ? readGRO(path, cancel)
                : fmt == Format::CIF        ? readCifP1(path, cancel)
                : fmt == Format::LammpsData ? readAtomicData(path, cancel)
                                            : readInput(path, budget, progress, cancel);
    if (!budget)
        throw std::runtime_error("Budget must be positive");
    d.stride = std::max<uint64_t>(1, d.atoms.size() / budget + (d.atoms.size() % budget != 0));
    if (d.stride > 1) {
        size_t out = 0;
        for (size_t i = 0; i < d.atoms.size(); i += size_t(d.stride)) {
            d.atoms[out] = d.atoms[i];
            for (auto &[name, v] : d.vectorProperties)
                v[out] = v[i];
            for (auto &[name, v] : d.scalarProperties)
                v[out] = v[i];
            ++out;
        }
        d.atoms.resize(out);
        for (auto &[name, v] : d.vectorProperties)
            v.resize(out);
        for (auto &[name, v] : d.scalarProperties)
            v.resize(out);
        d.bounds();
    }
    checkpoint(cancel);
    if (progress)
        *progress = 1;
    return d;
}
struct ExportOptions {
    int precision = 10;
    bool extendedXYZ = true;
    bool fractionalPOSCAR = false;
    bool constraints = true;
    std::vector<std::string> scalarProperties, vectorProperties;
};
inline void validate(const Dataset &d, const ExportOptions &o) {
    if (o.precision < 1 || o.precision > 17)
        throw std::runtime_error("Precision must be between 1 and 17");
    for (const auto &a : d.atoms)
        if (a.type >= d.species.size() || !std::isfinite(a.x) || !std::isfinite(a.y) ||
            !std::isfinite(a.z))
            throw std::runtime_error("Invalid particle for export");
    for (const auto &s : d.species)
        if (s.empty() || s.find_first_of(" \t\r\n\"'") != std::string::npos)
            throw std::runtime_error(
                "Format requires particle type names without whitespace or quotes");
    auto validName = [](const std::string &s) {
        return !s.empty() &&
               s.find_first_not_of(
                   "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") ==
                   std::string::npos &&
               s != "species" && s != "pos" && s != "type";
    };
    for (const auto &s : o.scalarProperties) {
        auto it = d.scalarProperties.find(s);
        if (!validName(s) || it == d.scalarProperties.end() || it->second.size() != d.atoms.size())
            throw std::runtime_error("Invalid export property: " + s);
        for (double v : it->second)
            if (!std::isfinite(v))
                throw std::runtime_error("Non-finite property: " + s);
    }
    for (const auto &s : o.vectorProperties) {
        auto it = d.vectorProperties.find(s);
        if (!validName(s) || it == d.vectorProperties.end() || it->second.size() != d.atoms.size())
            throw std::runtime_error("Invalid export vector: " + s);
        for (auto v : it->second)
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
                throw std::runtime_error("Non-finite vector: " + s);
    }
}
inline void writeFrame(std::ostream &f, Format fmt, const Dataset &d, const ExportOptions &o,
                       int frame = 0) {
    auto applicableOptions = o;
    if (fmt != Format::XYZ || !o.extendedXYZ) {
        applicableOptions.scalarProperties.clear();
        applicableOptions.vectorProperties.clear();
    }
    if (fmt == Format::GRO)
        applicableOptions.precision = 10; // GRO field precision is fixed by the writer.
    validate(d, applicableOptions);
    f.imbue(std::locale::classic());
    f << std::setprecision(o.precision);
    if (fmt == Format::XYZ) {
        f << d.atoms.size() << '\n';
        if (o.extendedXYZ) {
            f << "Lattice=\"";
            for (int k = 0; k < 9; ++k)
                f << d.cell[k] << (k == 8 ? "" : " ");
            f << "\" Properties=species:S:1:pos:R:3";
            for (const auto &s : o.scalarProperties)
                f << ':' << s << ":R:1";
            for (const auto &s : o.vectorProperties)
                f << ':' << s << ":R:3";
            f << " Origin=\"" << d.origin.x << ' ' << d.origin.y << ' ' << d.origin.z
              << "\" pbc=\"";
            for (int k = 0; k < 3; ++k)
                f << (d.pbc[k] ? "T" : "F") << (k == 2 ? "" : " ");
            f << "\"";
        } else
            f << "AtomX frame " << frame;
        f << '\n';
        for (size_t i = 0; i < d.atoms.size(); ++i) {
            auto a = d.atoms[i];
            f << d.species[a.type] << ' ' << a.x << ' ' << a.y << ' ' << a.z;
            if (o.extendedXYZ) {
                for (const auto &s : o.scalarProperties)
                    f << ' ' << d.scalarProperties.at(s)[i];
                for (const auto &s : o.vectorProperties) {
                    auto v = d.vectorProperties.at(s)[i];
                    f << ' ' << v.x << ' ' << v.y << ' ' << v.z;
                }
            }
            f << '\n';
        }
    } else if (fmt == Format::POSCAR) {
        if (std::abs(determinant(d.cell)) < 1e-15)
            throw std::runtime_error("POSCAR requires a non-degenerate cell");
        f << "AtomX export\n1.0\n";
        for (int k = 0; k < 3; ++k)
            f << d.cell[k * 3] << ' ' << d.cell[k * 3 + 1] << ' ' << d.cell[k * 3 + 2] << '\n';
        for (const auto &s : d.species)
            f << s << ' ';
        f << '\n';
        for (size_t t = 0; t < d.species.size(); ++t)
            f << std::count_if(d.atoms.begin(), d.atoms.end(), [&](const Atom &a) {
                return a.type == t;
            }) << ' ';
        bool constraints = o.constraints && d.vectorProperties.count("MoveMask");
        if (constraints && d.vectorProperties.at("MoveMask").size() != d.atoms.size())
            throw std::runtime_error("Selective dynamics property length mismatch");
        f << (constraints ? "\nSelective dynamics" : "")
          << (o.fractionalPOSCAR ? "\nDirect\n" : "\nCartesian\n");
        for (size_t t = 0; t < d.species.size(); ++t)
            for (size_t i = 0; i < d.atoms.size(); ++i) {
                const auto &a = d.atoms[i];
                if (a.type != t)
                    continue;
                auto v = o.fractionalPOSCAR
                             ? fractional(d, a)
                             : std::array<double, 3>{a.x - d.origin.x, a.y - d.origin.y,
                                                     a.z - d.origin.z};
                f << v[0] << ' ' << v[1] << ' ' << v[2];
                if (constraints) {
                    auto m = d.vectorProperties.at("MoveMask")[i];
                    for (float q : {m.x, m.y, m.z}) {
                        if (q != 0 && q != 1)
                            throw std::runtime_error("MoveMask must contain zero or one");
                        f << (q == 0 ? " F" : " T");
                    }
                }
                f << '\n';
            }
    } else if (fmt == Format::CIF) {
        double lengths[3];
        for (int k = 0; k < 3; ++k)
            lengths[k] = std::hypot(d.cell[k * 3], d.cell[k * 3 + 1], d.cell[k * 3 + 2]);
        if (std::abs(determinant(d.cell)) < 1e-15)
            throw std::runtime_error("CIF requires a non-degenerate cell");
        auto angle = [&](int a, int b) {
            double dot = 0;
            for (int k = 0; k < 3; ++k)
                dot += d.cell[a * 3 + k] * d.cell[b * 3 + k];
            return std::acos(std::clamp(dot / (lengths[a] * lengths[b]), -1., 1.)) * 180 /
                   3.141592653589793;
        };
        f << "data_atomx\n_symmetry_space_group_name_H-M 'P 1'\n_cell_length_a " << lengths[0]
          << "\n_cell_length_b " << lengths[1] << "\n_cell_length_c " << lengths[2]
          << "\n_cell_angle_alpha " << angle(1, 2) << "\n_cell_angle_beta " << angle(0, 2)
          << "\n_cell_angle_gamma " << angle(0, 1)
          << "\nloop_\n_atom_site_type_symbol\n_atom_site_fract_x\n_atom_site_fract_y\n_atom_site_"
             "fract_z\n";
        for (const auto &a : d.atoms) {
            auto v = fractional(d, a);
            f << d.species[a.type] << ' ' << v[0] << ' ' << v[1] << ' ' << v[2] << '\n';
        }
    } else if (fmt == Format::LammpsData || fmt == Format::LammpsDump) {
        if (d.cell[1] || d.cell[2] || d.cell[5] || d.cell[0] <= 0 || d.cell[4] <= 0 ||
            d.cell[8] <= 0)
            throw std::runtime_error(
                "LAMMPS export requires a restricted triclinic cell (a along X, b in XY)");
        if (fmt == Format::LammpsData) {
            f << "AtomX atomic structure\n\n"
              << d.atoms.size() << " atoms\n"
              << d.species.size() << " atom types\n\n"
              << d.origin.x << ' ' << d.origin.x + d.cell[0] << " xlo xhi\n"
              << d.origin.y << ' ' << d.origin.y + d.cell[4] << " ylo yhi\n"
              << d.origin.z << ' ' << d.origin.z + d.cell[8] << " zlo zhi\n"
              << d.cell[3] << ' ' << d.cell[6] << ' ' << d.cell[7]
              << " xy xz yz\n\nAtoms # atomic\n\n";
            for (size_t i = 0; i < d.atoms.size(); ++i) {
                auto a = d.atoms[i];
                f << i + 1 << ' ' << a.type + 1 << ' ' << a.x << ' ' << a.y << ' ' << a.z << '\n';
            }
        } else {
            double xy = d.cell[3], xz = d.cell[6], yz = d.cell[7];
            f << "ITEM: TIMESTEP\n"
              << frame << "\nITEM: NUMBER OF ATOMS\n"
              << d.atoms.size() << "\nITEM: BOX BOUNDS xy xz yz";
            for (bool p : d.pbc)
                f << (p ? " pp" : " ff");
            f << '\n'
              << d.origin.x + std::min({0., xy, xz, xy + xz}) << ' '
              << d.origin.x + d.cell[0] + std::max({0., xy, xz, xy + xz}) << ' ' << xy << '\n'
              << d.origin.y + std::min(0., yz) << ' ' << d.origin.y + d.cell[4] + std::max(0., yz)
              << ' ' << xz << '\n'
              << d.origin.z << ' ' << d.origin.z + d.cell[8] << ' ' << yz
              << "\nITEM: ATOMS id type element x y z\n";
            for (size_t i = 0; i < d.atoms.size(); ++i) {
                auto a = d.atoms[i];
                f << i + 1 << ' ' << a.type + 1 << ' ' << d.species[a.type] << ' ' << a.x << ' '
                  << a.y << ' ' << a.z << '\n';
            }
        }
    } else if (fmt == Format::GRO) {
        for (const auto &s : d.species)
            if (s.size() > 5)
                throw std::runtime_error("GRO atom names must be at most five characters");
        if (d.atoms.size() > 99999)
            throw std::runtime_error("GRO export supports at most 99999 atoms");
        f << "AtomX GRO (nm)\n" << d.atoms.size() << '\n';
        for (size_t i = 0; i < d.atoms.size(); ++i) {
            auto a = d.atoms[i];
            for (float v : {a.x, a.y, a.z})
                if (v < -9999.99f || v > 99999.99f)
                    throw std::runtime_error("GRO coordinate exceeds fixed-width range");
            f << std::setw(5) << 1 << std::left << std::setw(5) << "MOL" << std::right
              << std::setw(5) << d.species[a.type] << std::setw(5) << i + 1 << std::fixed
              << std::setprecision(3) << std::setw(8) << a.x / 10. << std::setw(8) << a.y / 10.
              << std::setw(8) << a.z / 10. << '\n';
        }
        f << std::fixed << std::setprecision(5);
        for (int k : {0, 4, 8, 1, 2, 3, 5, 6, 7})
            f << ' ' << d.cell[k] / 10.;
        f << '\n';
    } else
        throw std::runtime_error("Format is read-only");
    if (!f)
        throw std::runtime_error("Structure export failed");
}
inline void write(const std::filesystem::path &path, Format fmt, const Dataset &d,
                  const ExportOptions &o = {}) {
    // Validate and serialize before opening the destination, so invalid data does not truncate it.
    std::ostringstream buffer;
    writeFrame(buffer, fmt, d, o);
    std::ofstream out(path, std::ios::binary);
    if (!out)
        throw std::runtime_error("Cannot open export destination");
    out << buffer.str();
    out.flush();
    if (!out)
        throw std::runtime_error("Failed writing export file");
}
} // namespace atomx::io
