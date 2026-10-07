// Included inside atomx::io after the shared parsing helpers.
// Specifications: BIOVIA CTfile 2020 (V2000); XCrySDen XSF; wwPDB format 3.3.
#pragma once
inline std::string fixedLine(std::istream &f) {
    std::string s;
    if (!std::getline(f, s)) throw std::runtime_error("Truncated structure file");
    if (!s.empty() && s.back() == '\r') s.pop_back();
    return s;
}
inline const std::vector<std::string> &chemicalSymbols() {
    static const auto symbols = tokens("H He Li Be B C N O F Ne Na Mg Al Si P S Cl Ar K Ca Sc Ti V Cr Mn Fe Co Ni Cu Zn Ga Ge As Se Br Kr Rb Sr Y Zr Nb Mo Tc Ru Rh Pd Ag Cd In Sn Sb Te I Xe Cs Ba La Ce Pr Nd Pm Sm Eu Gd Tb Dy Ho Er Tm Yb Lu Hf Ta W Re Os Ir Pt Au Hg Tl Pb Bi Po At Rn Fr Ra Ac Th Pa U Np Pu Am Cm Bk Cf Es Fm Md No Lr Rf Db Sg Bh Hs Mt Ds Rg Cn Nh Fl Mc Lv Ts Og");
    return symbols;
}
inline std::string chemicalSymbol(std::string name) {
    while (!name.empty() && (name.back() == '+' || name.back() == '-' || std::isdigit(static_cast<unsigned char>(name.back())))) name.pop_back();
    for (const auto &symbol : chemicalSymbols()) {
        if (name.size() != symbol.size()) continue;
        bool equal = true;
        for (size_t k = 0; k < name.size(); ++k) equal &= std::tolower(static_cast<unsigned char>(name[k])) == std::tolower(static_cast<unsigned char>(symbol[k]));
        if (equal) return symbol;
    }
    throw std::runtime_error("Format requires a chemical element symbol (query / pseudo atoms are unsupported)");
}
inline std::filesystem::path defaultExportPath(const std::filesystem::path &source,
                                               Format fmt,
                                               const std::filesystem::path &directory = {}) {
    auto folder = directory.empty() ? source.parent_path() : directory;
    auto name = source.empty() ? std::filesystem::path(L"structure") : source.stem();
    if (name.empty()) name = L"structure";
    auto safeName = name.wstring();
    for (auto &c : safeName) if (c < 32 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) c = L'_';
    while (!safeName.empty() && (safeName.back() == L'.' || safeName.back() == L' ')) safeName.pop_back();
    name = safeName.empty() ? L"structure" : safeName;
    if (fmt == Format::POSCAR) return folder / L"POSCAR";
    // Preserve the input when exporting back into its folder.
    auto target = folder / (name.wstring() + L"." + std::filesystem::path(info(fmt).extension).wstring());
    if (target == source) target = folder / (name.wstring() + L"-export." + std::filesystem::path(info(fmt).extension).wstring());
    return target;
}
inline int smallInteger(const std::string &s, int low, int high) {
    double value = number(trim(s));
    if (value != std::floor(value) || value < low || value > high)
        throw std::runtime_error("Integer outside format limits");
    return int(value);
}
inline std::filesystem::path sequenceExportPath(const std::filesystem::path &destination,
                                                Format fmt, int frame) {
    std::wostringstream number; number << std::setw(6) << std::setfill(L'0') << frame;
    if (fmt == Format::POSCAR && destination.extension().empty())
        return destination.parent_path() / (L"frame_" + number.str()) / destination.filename();
    return destination.parent_path() / (destination.stem().wstring() + L"_" + number.str() + destination.extension().wstring());
}
inline void finishExchange(Dataset &d) {
    if (d.atoms.empty()) throw std::runtime_error("No atoms in structure");
    d.sourceCount = d.atoms.size(); d.bounds();
}
inline Dataset readMolRecord(std::istream &f, std::atomic<bool> *cancel) {
    Dataset d; d.comment = line(f); line(f); line(f);
    auto s = fixedLine(f);
    if (s.size() < 39 || s.substr(34, 5) != "V2000")
        throw std::runtime_error("MOL/SDF currently requires V2000; convert V3000 first");
    int n = smallInteger(s.substr(0, 3), 1, 999), nb = smallInteger(s.substr(3, 3), 0, 999);
    if (smallInteger(s.substr(12, 3), 0, 1)) throw std::runtime_error("MOL chirality is unsupported");
    auto &charges = d.scalarProperties["FormalCharge"]; charges.resize(size_t(n));
    for (int i = 0; i < n; ++i) {
        checkpoint(cancel); s = fixedLine(f);
        if (s.size() < 39) throw std::runtime_error("Truncated MOL atom row");
        const auto symbol = chemicalSymbol(trim(s.substr(31, 3)));
        atom(d, number(trim(s.substr(0, 10))), number(trim(s.substr(10, 10))),
             number(trim(s.substr(20, 10))), symbol);
        int q = smallInteger(s.substr(36, 3), 0, 7);
        if (q == 4) throw std::runtime_error("MOL radicals are unsupported");
        if (s.size() >= 42 && smallInteger(s.substr(39, 3), 0, 3))
            throw std::runtime_error("MOL atom stereochemistry is unsupported");
        charges[size_t(i)] = q ? 4 - q : 0;
        if (s.size() >= 36 && smallInteger(s.substr(34, 2), -3, 4) != 0)
            throw std::runtime_error("MOL isotopes are unsupported");
    }
    for (int i = 0; i < nb; ++i) {
        checkpoint(cancel); s = fixedLine(f);
        if (s.size() < 12) throw std::runtime_error("Truncated MOL bond row");
        int a = smallInteger(s.substr(0, 3), 1, n), b = smallInteger(s.substr(3, 3), 1, n);
        int order = smallInteger(s.substr(6, 3), 1, 4);
        if (a == b) throw std::runtime_error("MOL self bond is unsupported");
        if (smallInteger(s.substr(9, 3), 0, 6) != 0)
            throw std::runtime_error("MOL bond stereochemistry is unsupported");
        d.bonds.push_back({uint32_t(a - 1), uint32_t(b - 1), {}, uint8_t(order)});
    }
    bool chargeBlock = false;
    while ((s = line(f)) != "M  END") {
        checkpoint(cancel);
        auto row = tokens(s);
        if (row.size() < 3 || row[0] != "M" || row[1] != "CHG")
            throw std::runtime_error("Unsupported MOL property block (isotopes, radicals, queries or groups)");
        int entries = smallInteger(row[2], 1, 8);
        if (row.size() != size_t(3 + 2 * entries)) throw std::runtime_error("Invalid MOL charge row");
        if (!chargeBlock) { std::fill(charges.begin(), charges.end(), 0); chargeBlock = true; }
        for (int j = 0; j < entries; ++j)
            charges[size_t(smallInteger(row[size_t(3 + j * 2)], 1, n) - 1)] =
                smallInteger(row[size_t(4 + j * 2)], -15, 15);
    }
    finishExchange(d); return d;
}
inline std::vector<Frame> indexSDF(const std::filesystem::path &path,
                                  std::atomic<float> *progress, std::atomic<bool> *cancel) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open SDF");
    std::vector<Frame> frames; const auto size = std::filesystem::file_size(path);
    while (f.peek() != EOF) {
        checkpoint(cancel); auto offset = f.tellg();
        auto name = line(f); line(f); line(f); const auto counts = fixedLine(f);
        if (counts.size() < 39 || counts.substr(34, 5) != "V2000")
            throw std::runtime_error("SDF currently requires V2000 records");
        int n = smallInteger(counts.substr(0, 3), 1, 999);
        bool ended = false; std::string s;
        while (std::getline(f, s)) { checkpoint(cancel); if (trim(s) == "$$$$") { ended = true; break; } }
        if (!ended) throw std::runtime_error("SDF record missing $$$$ delimiter");
        frames.push_back({uint64_t(n), offset, name});
        if (progress) *progress = float(double(f.tellg()) / std::max<uint64_t>(1, size));
    }
    if (frames.empty()) throw std::runtime_error("Empty SDF");
    if (progress) *progress = 1; return frames;
}
inline Dataset readMol(const std::filesystem::path &path, const Frame &frame, bool sdf,
                       std::atomic<bool> *cancel) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open MOL/SDF");
    if (sdf) f.seekg(frame.offset);
    auto d = readMolRecord(f, cancel); std::string s;
    bool ended = false;
    while (std::getline(f, s)) {
        checkpoint(cancel);
        if (sdf && trim(s) == "$$$$") { ended = true; break; }
        if (!sdf && !trim(s).empty()) throw std::runtime_error("Unexpected data after MOL record");
        // SD data fields are document-level text and not per-atom properties.
    }
    if (sdf && !ended) throw std::runtime_error("SDF record missing $$$$ delimiter");
    return d;
}
inline Dataset readXSF(const std::filesystem::path &path, std::atomic<bool> *cancel) {
    std::ifstream f(path); if (!f) throw std::runtime_error("Cannot open XSF");
    Dataset d; bool cell = false, coordinates = false; std::string s;
    auto next = [&]() {
        while (std::getline(f, s)) { checkpoint(cancel); s = trim(s);
            if (!s.empty() && s[0] != '#') return true; }
        return false;
    };
    auto coordinate = [&](const std::string &text) {
        auto row = tokens(text);
        if (row.size() != 4 && row.size() != 7) throw std::runtime_error("Invalid XSF atom row");
        std::string symbol = row[0];
        if (symbol.find_first_not_of("0123456789") == std::string::npos) {
            const int z = smallInteger(symbol, 1, 118); symbol = chemicalSymbols()[size_t(z - 1)];
        } else symbol = chemicalSymbol(symbol);
        atom(d, number(row[1]), number(row[2]), number(row[3]), symbol);
        if (row.size() == 7) d.vectorProperties["Force.HartreePerAngstrom"].push_back(
            {float(number(row[4])), float(number(row[5])), float(number(row[6]))});
        if (d.atoms.size() > 20000000) throw std::runtime_error("XSF reader limit: 20 million atoms");
    };
    while (next()) {
        if (s == "CRYSTAL") d.pbc = {true, true, true};
        else if (s == "SLAB") d.pbc = {true, true, false};
        else if (s == "POLYMER") d.pbc = {true, false, false};
        else if (s == "MOLECULE") d.pbc = {};
        else if (s == "PRIMVEC" || s == "CONVVEC") {
            bool primitive = s == "PRIMVEC";
            for (int k = 0; k < 3; ++k) {
                if (!next()) throw std::runtime_error("Truncated XSF cell");
                auto v = tokens(s); if (v.size() != 3) throw std::runtime_error("Invalid XSF cell");
                for (int j = 0; j < 3; ++j) { const double q = number(v[size_t(j)]); if (primitive) d.cell[size_t(k * 3 + j)] = q; }
            }
            cell |= primitive;
        } else if (s == "PRIMCOORD") {
            if (coordinates || !cell || !next()) throw std::runtime_error("Missing XSF cell or repeated coordinates");
            auto v = tokens(s);
            if (v.size() != 2 || v[1] != "1") throw std::runtime_error("Invalid XSF PRIMCOORD count");
            auto n = count(v[0]); if (!n || n > 20000000) throw std::runtime_error("XSF atom count outside reader limits");
            for (uint64_t i = 0; i < n; ++i) { if (!next()) throw std::runtime_error("Truncated XSF coordinates"); coordinate(s); }
            coordinates = true;
        } else if (s == "ATOMS") {
            if (coordinates) throw std::runtime_error("Multiple XSF coordinate blocks are unsupported");
            while (next()) {
                auto row = tokens(s);
                if (row.size() != 4 && row.size() != 7) throw std::runtime_error("XSF grids / animations are unsupported");
                coordinate(s);
            }
            coordinates = true;
        } else throw std::runtime_error("XSF grids / animations or unknown sections are unsupported");
    }
    if (cell && std::abs(determinant(d.cell)) < 1e-15) throw std::runtime_error("Degenerate XSF cell");
    for (const auto &[name, values] : d.vectorProperties)
        if (values.size() != d.atoms.size()) throw std::runtime_error("Inconsistent XSF force columns");
    finishExchange(d); d.comment = "XSF structure"; return d;
}
inline double formalCharge(const Dataset &d, size_t i) {
    for (const char *name : {"AtomX.FormalCharge", "FormalCharge"})
        if (auto it = d.scalarProperties.find(name); it != d.scalarProperties.end()) {
            if (it->second.size() != d.atoms.size() || !std::isfinite(it->second[i]))
                throw std::runtime_error("Invalid formal charge property");
            return it->second[i];
        }
    return 0;
}
inline void moleculeBonds(const Dataset &d, bool pdb) {
    for (const auto &b : d.bonds) {
        if (b.a >= d.atoms.size() || b.b >= d.atoms.size() || b.a == b.b ||
            b.image != std::array<int32_t, 3>{} || b.order < 1 || b.order > 4)
            throw std::runtime_error("Molecular formats require valid bonds without periodic images; save .atomx for periodic bonds");
        if (pdb && b.order != 1) throw std::runtime_error("PDB cannot preserve bond order; choose MOL/SDF or .atomx");
    }
}
inline void writeExchange(std::ostream &f, Format fmt, const Dataset &d, int precision) {
    if (d.atoms.empty()) throw std::runtime_error("Cannot export an empty structure");
    if (fmt == Format::MOL || fmt == Format::SDF) {
        moleculeBonds(d, false);
        if (d.atoms.size() > 999 || d.bonds.size() > 999)
            throw std::runtime_error("MOL/SDF V2000 supports at most 999 atoms and 999 bonds; use .atomx or XYZ for larger structures");
        f << "AtomX molecule\n  AtomX            3D\n\n" << std::setw(3) << d.atoms.size()
          << std::setw(3) << d.bonds.size() << "  0  0  0  0  0  0  0  0999 V2000\n";
        for (const auto &a : d.atoms) {
            const auto symbol = chemicalSymbol(d.species[a.type]);
            for (float v : {a.x, a.y, a.z}) if (v < -9999.9999 || v > 99999.9999)
                throw std::runtime_error("MOL coordinate exceeds fixed-width range");
            f << std::fixed << std::setprecision(4) << std::setw(10) << a.x << std::setw(10) << a.y
              << std::setw(10) << a.z << ' ' << std::left << std::setw(3) << symbol << std::right
              << " 0  0  0  0  0  0  0  0  0  0  0  0\n";
        }
        for (const auto &b : d.bonds)
            f << std::setw(3) << b.a + 1 << std::setw(3) << b.b + 1 << std::setw(3) << int(b.order) << "  0  0  0  0\n";
        for (size_t i = 0; i < d.atoms.size(); ++i) {
            double q = formalCharge(d, i);
            if (q != std::floor(q) || q < -15 || q > 15) throw std::runtime_error("MOL formal charge must be an integer between -15 and 15");
            if (q) f << "M  CHG  1" << std::setw(4) << i + 1 << std::setw(4) << int(q) << '\n';
        }
        f << "M  END\n"; if (fmt == Format::SDF) f << "$$$$\n";
    } else if (fmt == Format::XSF) {
        if (d.pbc != std::array<bool, 3>{} && d.pbc != std::array<bool, 3>{true, false, false} &&
            d.pbc != std::array<bool, 3>{true, true, false} && d.pbc != std::array<bool, 3>{true, true, true})
            throw std::runtime_error("XSF periodic axes must be X, XY or XYZ; use extended XYZ for arbitrary PBC flags");
        const int dimensions = int(d.pbc[0]) + int(d.pbc[1]) + int(d.pbc[2]);
        if (dimensions) {
            if (std::abs(determinant(d.cell)) < 1e-15) throw std::runtime_error("XSF requires a non-degenerate periodic cell");
            f << (dimensions == 3 ? "CRYSTAL\n" : dimensions == 2 ? "SLAB\n" : "POLYMER\n") << "PRIMVEC\n";
            for (int k = 0; k < 3; ++k) f << d.cell[k * 3] << ' ' << d.cell[k * 3 + 1] << ' ' << d.cell[k * 3 + 2] << '\n';
            f << "PRIMCOORD\n" << d.atoms.size() << " 1\n";
        } else f << "ATOMS\n";
        for (const auto &a : d.atoms) f << chemicalSymbol(d.species[a.type]) << ' ' << a.x - d.origin.x << ' '
            << a.y - d.origin.y << ' ' << a.z - d.origin.z << '\n';
    } else if (fmt == Format::PDB) {
        moleculeBonds(d, true);
        if (d.atoms.size() > 99999) throw std::runtime_error("PDB supports at most 99999 atoms; use XYZ or .atomx");
        f << "TITLE     AtomX coordinates\n";
        std::array<double, 9> conventional{};
        const bool periodic = d.pbc[0] || d.pbc[1] || d.pbc[2];
        if (periodic) {
            double lengths[3];
            for (int k = 0; k < 3; ++k) lengths[k] = std::hypot(d.cell[k * 3], d.cell[k * 3 + 1], d.cell[k * 3 + 2]);
            if (std::abs(determinant(d.cell)) < 1e-15) throw std::runtime_error("PDB requires a non-degenerate periodic cell");
            auto angle = [&](int a, int b) { double dot = 0; for (int k = 0; k < 3; ++k) dot += d.cell[a * 3 + k] * d.cell[b * 3 + k];
                return std::acos(std::clamp(dot / (lengths[a] * lengths[b]), -1., 1.)) * 180 / 3.141592653589793; };
            conventional = cellFromParameters(lengths[0], lengths[1], lengths[2], angle(1, 2), angle(0, 2), angle(0, 1));
            f << "CRYST1" << std::fixed << std::setprecision(3);
            for (double v : lengths) { if (v >= 99999.9995) throw std::runtime_error("PDB cell exceeds fixed-width range"); f << std::setw(9) << v; }
            f << std::setprecision(2) << std::setw(7) << angle(1, 2) << std::setw(7) << angle(0, 2) << std::setw(7) << angle(0, 1) << " P 1           1\n";
        }
        for (size_t i = 0; i < d.atoms.size(); ++i) {
            auto a = d.atoms[i]; const auto symbol = chemicalSymbol(d.species[a.type]);
            std::array<double, 3> v{a.x, a.y, a.z};
            if (periodic) { auto q = fractional(d, a); for (int k = 0; k < 3; ++k) v[size_t(k)] = q[0] * conventional[size_t(k)] + q[1] * conventional[size_t(3 + k)] + q[2] * conventional[size_t(6 + k)]; }
            for (double q : v) if (q <= -999.9995 || q >= 9999.9995) throw std::runtime_error("PDB coordinate exceeds fixed-width range");
            const double q = formalCharge(d, i);
            if (q != std::floor(q) || std::abs(q) > 9) throw std::runtime_error("PDB formal charge must be an integer between -9 and 9");
            std::string atomName = symbol.size() == 1 ? std::string(" ") + symbol : symbol;
            f << "HETATM" << std::setw(5) << i + 1 << ' ' << std::left << std::setw(4) << atomName << std::right
              << " MOL A   1    " << std::fixed << std::setprecision(3) << std::setw(8) << v[0] << std::setw(8) << v[1] << std::setw(8) << v[2]
              << "  1.00  0.00          " << std::setw(2) << symbol;
            if (q) f << int(std::abs(q)) << (q > 0 ? '+' : '-'); else f << "  "; f << '\n';
        }
        for (const auto &b : d.bonds) { f << "CONECT" << std::setw(5) << b.a + 1 << std::setw(5) << b.b + 1 << '\n';
            f << "CONECT" << std::setw(5) << b.b + 1 << std::setw(5) << b.a + 1 << '\n'; }
        f << "END\n";
    }
    f << std::defaultfloat << std::setprecision(precision);
}
