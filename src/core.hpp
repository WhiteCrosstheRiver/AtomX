#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <iomanip>
#include <cwctype>

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
    Vec3 origin{};
    Vec3 lo{}, hi{};
    uint64_t sourceCount = 0, stride = 1;
    std::string comment;
    // Scalar/vector particle properties published by modifiers and readers.
    // Positions and type remain in the compact Atom buffer for the renderer;
    // auxiliary properties are kept separately so adding analysis columns does
    // not change the GPU ABI.
    std::unordered_map<std::string, std::vector<double>> scalarProperties;
    std::unordered_map<std::string, std::vector<Vec3>> vectorProperties;
    std::unordered_map<std::string, std::string> propertyComponents;
    // Explicit pair topology published by bond-producing modifiers.
    std::vector<std::array<uint32_t, 2>> bonds;
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
    std::istringstream origin(attribute(fr.comment, "Origin"));
    origin >> d.origin.x >> d.origin.y >> d.origin.z;
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
    struct Column {
        std::string name;
        int offset, width;
    };
    std::vector<Column> columns;
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
            else if (name != "species" && name != "type" && (type == "R" || type == "I") &&
                     (width == 1 || width == 3))
                columns.push_back({name, col, width});
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
        for (const auto &c : columns) {
            if (tok.size() < size_t(c.offset + c.width))
                throw std::runtime_error("Missing XYZ property: " + c.name);
            auto number = [&](int offset) {
                size_t end = 0;
                double v = std::stod(tok[offset], &end);
                if (end != tok[offset].size() || !std::isfinite(v))
                    throw std::runtime_error("Invalid XYZ property: " + c.name);
                return v;
            };
            if (c.width == 1)
                d.scalarProperties[c.name].push_back(number(c.offset));
            else
                d.vectorProperties[c.name].push_back({float(number(c.offset)),
                                                      float(number(c.offset + 1)),
                                                      float(number(c.offset + 2))});
        }
    }
    d.bounds();
    if (progress)
        *progress = 1;
    return d;
}
inline std::string lowerExtension(const std::filesystem::path &p) {
    auto s = p.extension().string();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
inline bool isPOSCAR(const std::filesystem::path &path) {
    auto name = path.filename().wstring();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](wchar_t c) { return wchar_t(std::towlower(c)); });
    auto ext = lowerExtension(path);
    return name == L"poscar" || name == L"contcar" || ext == ".poscar" || ext == ".contcar" ||
           ext == ".vasp";
}
inline Dataset readPOSCAR(const std::filesystem::path &path) {
    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("Cannot open POSCAR");
    Dataset d;
    auto line = [&]() {
        std::string s;
        if (!std::getline(f, s))
            throw std::runtime_error("Truncated POSCAR");
        return s;
    };
    d.comment = line();
    double scale;
    std::istringstream scaling(line());
    std::string extra;
    if (!(scaling >> scale) || !std::isfinite(scale) || scale <= 0 || (scaling >> extra))
        throw std::runtime_error(
            "POSCAR requires one positive scale (volume/three-axis scaling unsupported)");
    for (int row = 0; row < 3; ++row) {
        std::istringstream values(line());
        for (int col = 0; col < 3; ++col) {
            double v;
            if (!(values >> v) || !std::isfinite(v * scale))
                throw std::runtime_error("Invalid POSCAR lattice");
            d.cell[row * 3 + col] = v * scale;
        }
    }
    auto header = line();
    std::istringstream symbols(header);
    std::string token;
    while (symbols >> token)
        d.species.push_back(token);
    if (d.species.empty())
        throw std::runtime_error("Missing POSCAR species/counts");
    bool v4 = d.species.front().find_first_not_of("0123456789") == std::string::npos;
    std::istringstream counts(v4 ? header : line());
    std::vector<size_t> numbers;
    uint64_t total = 0;
    while (counts >> token) {
        if (token.front() == '!')
            break;
        if (token.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("Invalid POSCAR count");
        auto n = std::stoull(token);
        if (n > 20000000 || total > 20000000 - n)
            throw std::runtime_error("POSCAR exceeds 20 million atom reader limit");
        numbers.push_back(size_t(n));
        total += n;
    }
    if (v4) {
        d.species.clear();
        for (size_t i = 0; i < numbers.size(); ++i)
            d.species.push_back("X" + std::to_string(i + 1));
    }
    if (numbers.empty() || numbers.size() != d.species.size())
        throw std::runtime_error("POSCAR species/count mismatch");
    auto mode = line();
    auto first = mode.find_first_not_of(" \t\r");
    bool selective = first != std::string::npos && (mode[first] == 'S' || mode[first] == 's');
    if (selective) {
        mode = line();
        first = mode.find_first_not_of(" \t\r");
    }
    if (first == std::string::npos)
        throw std::runtime_error("Missing POSCAR coordinate mode");
    char m = char(std::tolower(static_cast<unsigned char>(mode[first])));
    if (m != 'd' && m != 'c' && m != 'k')
        throw std::runtime_error("Invalid POSCAR coordinate mode");
    d.atoms.reserve(size_t(total));
    for (size_t type = 0; type < numbers.size(); ++type) {
        for (size_t i = 0; i < numbers[type]; ++i) {
            double a, b, c;
            std::istringstream row(line());
            if (!(row >> a >> b >> c))
                throw std::runtime_error("Invalid POSCAR coordinate row");
            if (selective) {
                Vec3 mask;
                for (int k = 0; k < 3; ++k) {
                    std::string flag;
                    if (!(row >> flag) ||
                        (flag != "T" && flag != "F" && flag != "t" && flag != "f"))
                        throw std::runtime_error("Invalid POSCAR selective-dynamics flag");
                    float v = (flag == "T" || flag == "t") ? 1.f : 0.f;
                    if (k == 0)
                        mask.x = v;
                    else if (k == 1)
                        mask.y = v;
                    else
                        mask.z = v;
                }
                d.vectorProperties["MoveMask"].push_back(mask);
            }
            double x = m == 'd' ? a * d.cell[0] + b * d.cell[3] + c * d.cell[6] : a * scale;
            double y = m == 'd' ? a * d.cell[1] + b * d.cell[4] + c * d.cell[7] : b * scale;
            double z = m == 'd' ? a * d.cell[2] + b * d.cell[5] + c * d.cell[8] : c * scale;
            Atom atom{float(x), float(y), float(z), uint32_t(type)};
            if (!std::isfinite(atom.x) || !std::isfinite(atom.y) || !std::isfinite(atom.z))
                throw std::runtime_error("Non-finite POSCAR coordinate");
            d.atoms.push_back(atom);
        }
    }
    d.sourceCount = d.atoms.size();
    d.pbc = {true, true, true};
    d.bounds();
    return d;
}
inline Dataset readCIF(const std::filesystem::path &path) {
    std::ifstream f(path); if(!f)throw std::runtime_error("Cannot open CIF"); Dataset d; std::string line; double a=0,b=0,c=0,alpha=90,beta=90,gamma=90; std::vector<std::array<std::string,4>> rows; bool loop=false;
    while(std::getline(f,line)){std::istringstream ss(line);std::string key;ss>>key;if(key=="_cell_length_a")ss>>a;else if(key=="_cell_length_b")ss>>b;else if(key=="_cell_length_c")ss>>c;else if(key=="_cell_angle_alpha")ss>>alpha;else if(key=="_cell_angle_beta")ss>>beta;else if(key=="_cell_angle_gamma")ss>>gamma;else if(key=="loop_")loop=true;else if(loop&&!line.empty()&&line[0]!='_'){std::array<std::string,4> r{};ss>>r[0]>>r[1]>>r[2]>>r[3];if(!r[0].empty())rows.push_back(r);}}
    if(a<=0||b<=0||c<=0)throw std::runtime_error("CIF cell lengths are missing"); double pi=3.141592653589793/180, ca=std::cos(alpha*pi),cb=std::cos(beta*pi),cg=std::cos(gamma*pi),sg=std::sin(gamma*pi); d.cell={a,b*cg,c*cb,0,b*sg,c*(ca-cb*cg)/std::max(sg,1e-12),0,0,c*std::sqrt(std::max(0.0,1-cb*cb-std::pow((ca-cb*cg)/std::max(sg,1e-12),2)))};std::unordered_map<std::string,uint32_t> types;
    for(auto&r:rows){
        try { std::string symbol=r[0]; double x,y,z; if(r[3].empty()){symbol="X";x=std::stod(r[0]);y=std::stod(r[1]);z=std::stod(r[2]);}else{x=std::stod(r[1]);y=std::stod(r[2]);z=std::stod(r[3]);} auto[it,ins]=types.emplace(symbol,uint32_t(types.size())); if(ins)d.species.push_back(symbol); Vec3 p{float(x*d.cell[0]+y*d.cell[3]+z*d.cell[6]),float(x*d.cell[1]+y*d.cell[4]+z*d.cell[7]),float(x*d.cell[2]+y*d.cell[5]+z*d.cell[8])}; d.atoms.push_back({p.x,p.y,p.z,it->second}); }
        catch(const std::exception&) { if(r[0].find("_atom_site") == 0 || r[0] == "loop_") continue; throw std::runtime_error("Invalid CIF atom coordinate: " + r[0] + " " + r[1] + " " + r[2] + " " + r[3]); }
    }
    d.sourceCount=d.atoms.size();d.pbc={true,true,true};d.comment="CIF import";d.bounds();return d;
}
inline Dataset readLammpsData(const std::filesystem::path &path) {
    std::ifstream f(path);if(!f)throw std::runtime_error("Cannot open LAMMPS data");Dataset d;std::string line;uint64_t natoms=0;double loX=0,hiX=0,loY=0,hiY=0,loZ=0,hiZ=0;bool atoms=false;
    while(std::getline(f,line)){std::istringstream ss(line);double x,y,z;std::string a,b;if(atoms){std::istringstream row(line);int id,type; if(row>>id>>type>>x>>y>>z){if(size_t(type)>d.species.size())d.species.resize(type,"X");d.species[type-1]=d.species[type-1]==""?"X"+std::to_string(type):d.species[type-1];d.atoms.push_back({float(x),float(y),float(z),uint32_t(type-1)});}continue;}if(ss>>natoms>>a&&a=="atoms")continue;if(ss.clear(),ss.str(line),ss>>loX>>hiX>>a>>b&&a=="xlo"&&b=="xhi")continue;if(ss.clear(),ss.str(line),ss>>loY>>hiY>>a>>b&&a=="ylo"&&b=="yhi")continue;if(ss.clear(),ss.str(line),ss>>loZ>>hiZ>>a>>b&&a=="zlo"&&b=="zhi")continue;if(line.find("Atoms")!=std::string::npos){atoms=true;std::getline(f,line);continue;}}
    if(d.atoms.empty())throw std::runtime_error("No atomic coordinates found in LAMMPS data");d.cell={hiX-loX,0,0,0,hiY-loY,0,0,0,hiZ-loZ};d.pbc={true,true,true};d.sourceCount=d.atoms.size();d.bounds();return d;
}
inline Dataset readInput(const std::filesystem::path& path, uint64_t budget=2000000, std::atomic<float>* progress=nullptr, std::atomic<bool>* cancel=nullptr) {
    auto ext = lowerExtension(path);
    if (isPOSCAR(path))
        return readPOSCAR(path);
    if (ext == ".cif")
        return readCIF(path);
    if (ext == ".data" || ext == ".lmp")
        return readLammpsData(path);
    auto frames=indexXYZ(path,progress,cancel); return readXYZ(path,frames.front(),budget,progress,cancel);
}
inline void writePOSCAR(const std::filesystem::path &p, const Dataset &d) {
    const auto &c = d.cell;
    double det = c[0] * (c[4] * c[8] - c[5] * c[7]) - c[1] * (c[3] * c[8] - c[5] * c[6]) +
                 c[2] * (c[3] * c[7] - c[4] * c[6]);
    if (!std::isfinite(det) || std::abs(det) < 1e-15)
        throw std::runtime_error("POSCAR export requires a nonsingular cell");
    if (d.species.empty())
        throw std::runtime_error("POSCAR export requires particle types");
    std::vector<size_t> counts(d.species.size());
    for (const auto &a : d.atoms) {
        if (a.type >= counts.size() || !std::isfinite(a.x) || !std::isfinite(a.y) ||
            !std::isfinite(a.z))
            throw std::runtime_error("Invalid POSCAR export particle");
        ++counts[a.type];
    }
    std::ofstream f(p);
    if (!f)
        throw std::runtime_error("Cannot write POSCAR");
    f << std::setprecision(17) << "AtomX export\n1.0\n";
    for (int r = 0; r < 3; ++r)
        f << c[r * 3] << ' ' << c[r * 3 + 1] << ' ' << c[r * 3 + 2] << '\n';
    for (const auto &s : d.species)
        f << s << ' ';
    f << '\n';
    for (auto n : counts)
        f << n << ' ';
    f << "\nCartesian\n";
    // POSCAR counts define contiguous type blocks: group positions accordingly.
    for (size_t type = 0; type < counts.size(); ++type)
        for (const auto &a : d.atoms)
            if (a.type == type)
                f << a.x << ' ' << a.y << ' ' << a.z << '\n';
    f.flush();
    if (!f)
        throw std::runtime_error("POSCAR write failed");
}
inline void writeCIF(const std::filesystem::path&p,const Dataset&d){std::ofstream f(p);if(!f)throw std::runtime_error("Cannot write CIF");f<<"data_atomx\n_cell_length_a "<<d.cell[0]<<"\n_cell_length_b "<<d.cell[4]<<"\n_cell_length_c "<<d.cell[8]<<"\n_cell_angle_alpha 90\n_cell_angle_beta 90\n_cell_angle_gamma 90\nloop_\n_atom_site_type_symbol\n_atom_site_fract_x\n_atom_site_fract_y\n_atom_site_fract_z\n";for(auto&a:d.atoms)f<<d.species[a.type]<<' '<<a.x/d.cell[0]<<' '<<a.y/d.cell[4]<<' '<<a.z/d.cell[8]<<'\n';}
inline void writeLammpsData(const std::filesystem::path&p,const Dataset&d){std::ofstream f(p);if(!f)throw std::runtime_error("Cannot write LAMMPS data");f<<d.atoms.size()<<" atoms\n"<<d.species.size()<<" atom types\n\n0 "<<d.cell[0]<<" xlo xhi\n0 "<<d.cell[4]<<" ylo yhi\n0 "<<d.cell[8]<<" zlo zhi\n\nMasses\n\n";for(size_t i=0;i<d.species.size();i++)f<<i+1<<" 1.0 # "<<d.species[i]<<'\n';f<<"\nAtoms # atomic\n\n";for(size_t i=0;i<d.atoms.size();i++)f<<i+1<<' '<<d.atoms[i].type+1<<' '<<d.atoms[i].x<<' '<<d.atoms[i].y<<' '<<d.atoms[i].z<<'\n';}
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
    ,ColorCoding
    ,CommonNeighborAnalysis
    ,CreateBonds
    ,RemoveProperty
    ,ExpandSelection
    ,SelectOverlapping
    ,ExpressionSelect
    ,ComputeProperty
};
struct Modifier {
    Op op;
    bool enabled = true;
    float value = 0;
    int axis = 2;
    int type = 0;
    float upper = 1;
    std::string property = "Position.X";
    bool adaptive = false;
    std::string outputProperty = "Computed property";
};
struct DataObject {
    enum class Kind { Particles, Bonds, Cell, Surface, Dislocations, VoxelGrid, Table, Labels };
    Kind kind = Kind::Particles;
    std::string name;
    bool visible = true;
    size_t sourceNode = 0;
};

// A pipeline node owns the executable modifier parameters as well as the UI,
// diagnostic and output-object state associated with that stage.
struct ModifierNode : Modifier {
    std::string id;
    std::string displayName;
    std::string category;
    bool dirty = true;
    bool running = false;
    std::string error;
    std::vector<DataObject> outputs;
    ModifierNode() = default;
    ModifierNode(const Modifier &modifier) : Modifier(modifier) {}
    ModifierNode(Op operation, bool active = true, float parameter = 0, int direction = 2,
                 int typeIndex = 0, float upperBound = 1,
                 std::string propertyName = "Position.X", bool useAdaptive = false)
        : Modifier{operation, active, parameter, direction, typeIndex, upperBound,
                   std::move(propertyName), useAdaptive} {}
};

// OVITO's pipeline is a linear dataflow graph. Nodes store real executable
// Modifier values; changing their order or state marks dependent stages dirty.
struct PipelineGraph {
    std::vector<ModifierNode> nodes;
    size_t selected = 0;
    PipelineGraph() {}
    void markDirtyFrom(size_t index) {
        for (size_t i = index; i < nodes.size(); ++i) nodes[i].dirty = true;
    }
    void insert(ModifierNode node, size_t at = SIZE_MAX) {
        at = std::min(at, nodes.size());
        nodes.insert(nodes.begin() + at, std::move(node));
        markDirtyFrom(at);
        selected = at;
    }
    void erase(size_t at) {
        if (at >= nodes.size()) return;
        nodes.erase(nodes.begin() + at);
        selected = nodes.empty() ? 0 : std::min(selected, nodes.size() - 1);
        markDirtyFrom(selected);
    }
    void move(size_t from, size_t to) {
        if (from >= nodes.size() || to >= nodes.size() || from == to) return;
        auto node = std::move(nodes[from]);
        nodes.erase(nodes.begin() + from);
        nodes.insert(nodes.begin() + to, std::move(node));
        selected = to;
        markDirtyFrom(std::min(from, to));
    }
};
struct NeighborBin {
    int64_t x = 0, y = 0, z = 0;
    bool operator==(const NeighborBin &) const = default;
};
inline int64_t &binComponent(NeighborBin &b, int axis) {
    return axis == 0 ? b.x : axis == 1 ? b.y : b.z;
}
struct NeighborBinHash {
    size_t operator()(NeighborBin b) const {
        return std::hash<int64_t>{}(b.x) ^ (std::hash<int64_t>{}(b.y) * 19349663u) ^
               (std::hash<int64_t>{}(b.z) * 83492791u);
    }
};

// Visits each cutoff pair once using linked spatial bins. Scientific neighbor
// operations reject sampled previews and currently require orthogonal PBC.
template <typename Callback>
inline void forEachNeighborPair(const Dataset &d, double cutoff, Callback &&callback,
                                std::atomic<bool> *cancel = nullptr) {
    if (d.sampled())
        throw std::runtime_error("Neighbor analysis requires full data; increase the import budget.");
    if (!(cutoff > 0) || !std::isfinite(cutoff))
        throw std::runtime_error("Cutoff must be finite and positive");
    if (d.atoms.size() > 2000000)
        throw std::runtime_error("Neighbor analysis currently limited to 2 million atoms");
    if (std::any_of(d.pbc.begin(), d.pbc.end(), [](bool b) { return b; }) &&
        (d.cell[1] || d.cell[2] || d.cell[3] || d.cell[5] || d.cell[6] || d.cell[7]))
        throw std::runtime_error("Periodic analysis currently requires an orthogonal cell");

    std::array<int64_t, 3> periodicBins{};
    std::array<double, 3> widths{cutoff, cutoff, cutoff};
    for (int axis = 0; axis < 3; ++axis)
        if (d.pbc[axis]) {
            double length = d.cell[axis * 4];
            if (!(length >= 2 * cutoff))
                throw std::runtime_error("Periodic cell must be at least twice the cutoff");
            periodicBins[axis] = std::max<int64_t>(1, int64_t(length / cutoff));
            widths[axis] = length / periodicBins[axis];
        }
    auto binOf = [&](const Atom &atom) {
        NeighborBin bin;
        for (int axis = 0; axis < 3; ++axis) {
            double origin = axis == 0 ? d.origin.x : axis == 1 ? d.origin.y : d.origin.z;
            double value = coordinate(atom, axis) - origin;
            if (d.pbc[axis])
                value -= std::floor(value / d.cell[axis * 4]) * d.cell[axis * 4];
            double q = std::floor(value / widths[axis]);
            if (!std::isfinite(q) || std::abs(q) > 1e15)
                throw std::runtime_error("Coordinates exceed spatial index range");
            binComponent(bin, axis) = int64_t(q);
        }
        return bin;
    };
    std::unordered_map<NeighborBin, std::vector<uint32_t>, NeighborBinHash> grid;
    grid.reserve(d.atoms.size());
    for (size_t i = 0; i < d.atoms.size(); ++i) {
        if (cancel && *cancel) throw std::runtime_error("Cancelled");
        grid[binOf(d.atoms[i])].push_back(uint32_t(i));
    }
    uint64_t comparisons = 0;
    const double cutoff2 = cutoff * cutoff;
    for (uint32_t i = 0; i < d.atoms.size(); ++i) {
        if (cancel && *cancel) throw std::runtime_error("Cancelled");
        auto center = binOf(d.atoms[i]);
        std::array<NeighborBin, 27> visited{};
        size_t visitedCount = 0;
        for (int z = -1; z <= 1; ++z)
            for (int y = -1; y <= 1; ++y)
                for (int x = -1; x <= 1; ++x) {
                    NeighborBin neighbor{center.x + x, center.y + y, center.z + z};
                    for (int axis = 0; axis < 3; ++axis)
                        if (periodicBins[axis]) {
                            auto &v = binComponent(neighbor, axis);
                            v = (v % periodicBins[axis] + periodicBins[axis]) % periodicBins[axis];
                        }
                    if (std::find(visited.begin(), visited.begin() + visitedCount, neighbor) !=
                        visited.begin() + visitedCount)
                        continue;
                    visited[visitedCount++] = neighbor;
                    auto found = grid.find(neighbor);
                    if (found == grid.end()) continue;
                    for (uint32_t j : found->second) {
                        if (j <= i) continue;
                        if (++comparisons > 300000000)
                            throw std::runtime_error("Neighbor comparison budget exceeded; reduce cutoff");
                        double distance2 = 0;
                        for (int axis = 0; axis < 3; ++axis) {
                            double delta = double(coordinate(d.atoms[i], axis)) - coordinate(d.atoms[j], axis);
                            if (d.pbc[axis])
                                delta -= std::round(delta / d.cell[axis * 4]) * d.cell[axis * 4];
                            distance2 += delta * delta;
                        }
                        if (distance2 <= cutoff2)
                            callback(i, j, distance2);
                    }
                }
    }
}
class ParticleExpression {
    const Dataset &d; size_t atom; std::string_view text; size_t p = 0;
    void ws() { while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p; }
    [[noreturn]] void error(const std::string &s) const {
        throw std::runtime_error("Expression column " + std::to_string(p + 1) + ": " + s);
    }
    bool take(std::string_view token) { ws(); if (text.substr(p, token.size()) != token) return false; p += token.size(); return true; }
    bool word(std::string_view token) {
        ws(); if (text.substr(p, token.size()) != token) return false;
        size_t e = p + token.size();
        auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        if ((p && ident(text[p-1])) || (e < text.size() && ident(text[e]))) return false;
        p = e; return true;
    }
    std::string name() {
        ws(); size_t b = p;
        while (p < text.size()) { char c = text[p]; if (std::isalnum(static_cast<unsigned char>(c)) || c=='_' || c=='.') ++p; else break; }
        if (p == b) error("Expected a property or function name");
        return std::string(text.substr(b,p-b));
    }
    double property(const std::string &n) {
        const Atom &a = d.atoms.at(atom);
        if (n == "x" || n == "Position.X") return a.x;
        if (n == "y" || n == "Position.Y") return a.y;
        if (n == "z" || n == "Position.Z") return a.z;
        if (n == "type" || n == "Particle Type") return a.type;
        auto it = d.scalarProperties.find(n);
        if (it == d.scalarProperties.end()) error("Unknown scalar property '" + n + "'");
        if (it->second.size() != d.atoms.size()) error("Property length does not match particle count");
        if (!std::isfinite(it->second[atom])) error("Property is non-finite");
        return it->second[atom];
    }
    double primary() {
        ws();
        if (take("(")) { double v=exprOr(); if(!take(")")) error("Expected ')'"); return v; }
        if (p < text.size() && (std::isdigit(static_cast<unsigned char>(text[p])) || text[p]=='.')) {
            const char *b=text.data()+p; char *e=nullptr; double v=std::strtod(b,&e);
            if(e==b || !std::isfinite(v)) error("Invalid number"); p += size_t(e-b); return v;
        }
        auto n=name();
        if (take("(")) {
            double v=exprOr(); if(!take(")")) error("Expected ')' after function argument");
            if(n=="abs") return std::abs(v);
            if(n=="sqrt") { if(v<0) error("sqrt requires a nonnegative input"); return std::sqrt(v); }
            if(n=="isfinite") return std::isfinite(v) ? 1.0 : 0.0;
            error("Function is not allowed: '"+n+"'");
        }
        return property(n);
    }
    double unary() { if(take("!")) return unary()==0; if(take("-")) return -unary(); if(take("+")) return unary(); return primary(); }
    double mul() { double v=unary(); for(;;){if(take("*"))v*=unary();else if(take("/")){double q=unary();if(q==0)error("Division by zero");v/=q;}else break;}return v; }
    double add() { double v=mul(); for(;;){if(take("+"))v+=mul();else if(take("-"))v-=mul();else break;}return v; }
    double compare() {
        double a=add();
        if(take("=="))return a==add(); if(take("!="))return a!=add();
        if(take("<="))return a<=add(); if(take(">="))return a>=add();
        if(take("<"))return a<add(); if(take(">"))return a>add(); return a;
    }
    double exprAnd() { double v=compare(); for(;;){if(take("&&")||word("and")){double q=compare();v=(v!=0&&q!=0);}else break;}return v; }
    double exprOr() { double v=exprAnd(); for(;;){if(take("||")||word("or")){double q=exprAnd();v=(v!=0||q!=0);}else break;}return v; }
public:
    ParticleExpression(const Dataset &data, size_t i, std::string_view expression) : d(data), atom(i), text(expression) {}
    double evaluateNumber() { if(text.empty())error("Expression is empty");double v=exprOr();ws();if(p!=text.size())error("Unexpected token");if(!std::isfinite(v))error("Result is non-finite");return v; }
    bool evaluate() { return evaluateNumber()!=0; }
};

struct CNAResult {
    std::vector<uint8_t> structure;
    std::unordered_map<std::string, uint64_t> counts;
    std::map<std::tuple<int,int,int>, uint64_t> bondSignatureCounts;
};

inline CNAResult analyzeCommonNeighbors(const Dataset &d, double cutoff,
                                        std::atomic<bool> *cancel = nullptr) {
    if (d.sampled()) throw std::runtime_error("CNA requires full data, not a sampled preview");
    std::vector<std::vector<uint32_t>> adjacency(d.atoms.size());
    forEachNeighborPair(d, cutoff, [&](uint32_t a, uint32_t b, double) {
        adjacency[a].push_back(b); adjacency[b].push_back(a);
    }, cancel);
    for (auto &neighbors : adjacency) {
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
        if (neighbors.size() > 128)
            throw std::runtime_error("CNA cutoff produced more than 128 neighbors per atom");
    }
    CNAResult result; result.structure.resize(d.atoms.size());
    result.counts = {{"Other",0},{"FCC",0},{"HCP",0},{"BCC",0},{"Icosahedral",0}};
    for (uint32_t center=0; center<d.atoms.size(); ++center) {
        if (cancel && *cancel) throw std::runtime_error("Cancelled");
        std::map<std::tuple<int,int,int>,uint32_t> signatures;
        const auto &neighbors = adjacency[center];
        for (uint32_t other : neighbors) {
            const auto &otherNeighbors = adjacency[other];
            std::vector<uint32_t> common;
            std::set_intersection(neighbors.begin(), neighbors.end(), otherNeighbors.begin(), otherNeighbors.end(),
                                  std::back_inserter(common));
            if (common.empty()) continue;
            int edges = 0;
            std::vector<std::vector<uint8_t>> graph(common.size(), std::vector<uint8_t>(common.size()));
            for (size_t i=0;i<common.size();++i)
                for (size_t j=i+1;j<common.size();++j)
                    if (std::binary_search(adjacency[common[i]].begin(), adjacency[common[i]].end(), common[j])) {
                        graph[i][j]=graph[j][i]=1; ++edges;
                    }
            int chain = 0;
            if (common.size() <= 12) {
                size_t searchStates = 0;
                bool capped = false;
                auto visit = [&](auto &&self, size_t node, uint16_t visited, int length) -> void {
                    if (capped || ++searchStates > 2000) { capped = true; return; }
                    chain = std::max(chain, length);
                    for (size_t next=0; next<common.size(); ++next)
                        if (graph[node][next] && !(visited & (uint16_t(1) << next)))
                            self(self,next,uint16_t(visited | (uint16_t(1)<<next)),length+1);
                };
                for (size_t start=0; start<common.size(); ++start)
                    visit(visit,start,uint16_t(1)<<start,0);
                if (capped) chain = -1; // fail closed: never classify an incomplete signature
            }
            ++signatures[{int(common.size()),edges,chain}];
            ++result.bondSignatureCounts[{int(common.size()),edges,chain}];
        }
        int coordination = int(neighbors.size());
        uint8_t id = 0;
        if (coordination == 12 && signatures[{4,2,1}] == 12) id = 1; // FCC
        else if (coordination == 12 && signatures[{4,2,1}] == 6 && signatures[{4,2,2}] == 6) id = 2; // HCP
        else if (coordination == 14 && signatures[{4,4,3}] == 6 && signatures[{6,6,5}] == 8) id = 3; // BCC
        else if (coordination == 12 && signatures[{5,5,4}] == 12) id = 4; // Icosahedral
        result.structure[center] = id;
        ++result.counts[id==1?"FCC":id==2?"HCP":id==3?"BCC":id==4?"Icosahedral":"Other"];
    }
    return result;
}

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
    case Op::ColorCoding: return "Color coding";
    case Op::CommonNeighborAnalysis: return "Common neighbor analysis";
    case Op::RemoveProperty: return "Remove property";
    case Op::CreateBonds: return "Create bonds";
    case Op::ExpandSelection: return "Expand selection";
    case Op::SelectOverlapping: return "Find overlapping particles";
    case Op::ExpressionSelect: return "Expression selection";
    case Op::ComputeProperty: return "Compute property";
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
            if ((m.op == Op::CommonNeighborAnalysis || m.op == Op::CreateBonds) &&
                (!(m.value > 0) || !std::isfinite(m.value)))
                throw std::runtime_error("Cutoff must be finite and positive");
            if ((m.op == Op::ExpandSelection || m.op == Op::SelectOverlapping) &&
                (!(m.value > 0) || !std::isfinite(m.value)))
                throw std::runtime_error("Cutoff must be finite and positive");
            if (m.op == Op::ExpandSelection && (m.type < 1 || m.type > 64))
                throw std::runtime_error("Expansion steps must be between 1 and 64");
            if (m.op == Op::ColorCoding && m.property.empty())
                throw std::runtime_error("Color coding requires a particle property");
            if (m.op == Op::ExpressionSelect && m.property.empty())
                throw std::runtime_error("Expression selection requires an expression");
            if (m.op == Op::ComputeProperty && (m.property.empty() || m.outputProperty.empty()))
                throw std::runtime_error("Computed property requires an expression and output name");
            if (m.op == Op::RemoveProperty) {
                if (m.property.empty() || m.property == "Position" || m.property == "Particle Type")
                    throw std::runtime_error("Choose an auxiliary particle property to remove");
                auto removed = r.data.scalarProperties.erase(m.property);
                removed += r.data.vectorProperties.erase(m.property);
                if (!removed) throw std::runtime_error("Unknown particle property: " + m.property);
                r.data.propertyComponents.erase(m.property);
                continue;
            }
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
                for (auto &[name, values] : r.data.scalarProperties) {
                    auto original = values;
                    for (int copy = 1; copy < m.type; ++copy)
                        values.insert(values.end(), original.begin(), original.end());
                }
                for (auto &[name, values] : r.data.vectorProperties) {
                    auto original = values;
                    for (int copy = 1; copy < m.type; ++copy)
                        values.insert(values.end(), original.begin(), original.end());
                }
                continue;
            }
            if (m.op == Op::Wrap &&
                (r.data.cell[1] != 0 || r.data.cell[2] != 0 || r.data.cell[3] != 0 ||
                 r.data.cell[5] != 0 || r.data.cell[6] != 0 || r.data.cell[7] != 0))
                throw std::runtime_error("Wrap currently requires an orthogonal cell");
            if (m.op == Op::ColorCoding) {
                std::vector<double> values;
                if (m.property == "Position.X" || m.property == "Position.Y" || m.property == "Position.Z") {
                    int axis = m.property.back() - 'X';
                    values.reserve(r.data.atoms.size());
                    for (const auto &a : r.data.atoms) values.push_back(coordinate(a, axis));
                } else {
                    auto it = r.data.scalarProperties.find(m.property);
                    if (it == r.data.scalarProperties.end())
                        throw std::runtime_error("Unknown particle property: " + m.property);
                    values = it->second;
                }
                if (values.size() != r.data.atoms.size())
                    throw std::runtime_error("Particle property length mismatch: " + m.property);
                r.data.scalarProperties["Color coding"] = std::move(values);
                continue;
            }
            if (m.op == Op::ExpandSelection) {
                for (int step = 0; step < m.type; ++step) {
                    auto expanded = r.selected;
                    forEachNeighborPair(r.data, m.value, [&](uint32_t i, uint32_t j, double) {
                        if (r.selected[i]) expanded[j] = 1;
                        if (r.selected[j]) expanded[i] = 1;
                    });
                    r.selected = std::move(expanded);
                }
                continue;
            }
            if (m.op == Op::SelectOverlapping) {
                r.selected.assign(r.data.atoms.size(), 0);
                forEachNeighborPair(r.data, m.value, [&](uint32_t i, uint32_t j, double) {
                    r.selected[i] = r.selected[j] = 1;
                });
                continue;
            }
            if (m.op == Op::ExpressionSelect) {
                r.selected.resize(r.data.atoms.size());
                for (size_t i=0; i<r.data.atoms.size(); ++i)
                    r.selected[i] = ParticleExpression(r.data, i, m.property).evaluate() ? 1 : 0;
                continue;
            }
            if (m.op == Op::ComputeProperty) {
                std::vector<double> values(r.data.atoms.size());
                for (size_t i=0; i<r.data.atoms.size(); ++i)
                    values[i] = ParticleExpression(r.data, i, m.property).evaluateNumber();
                r.data.scalarProperties[m.outputProperty] = std::move(values);
                continue;
            }
            if (m.op == Op::CommonNeighborAnalysis || m.op == Op::CreateBonds) {
                r.data.bonds.clear();
                if (m.op == Op::CreateBonds)
                    forEachNeighborPair(r.data, m.value, [&](uint32_t i, uint32_t j, double) { r.data.bonds.push_back({i,j}); });
                else {
                    auto cna = analyzeCommonNeighbors(r.data, m.value);
                    std::vector<double> structure(cna.structure.begin(), cna.structure.end());
                    r.data.scalarProperties["CNA Structure"] = std::move(structure);
                    r.data.bonds.clear();
                }
                continue;
            }
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
                                float(std::floor((coordinate(a, k) - (k == 0   ? r.data.origin.x
                                                                      : k == 1 ? r.data.origin.y
                                                                               : r.data.origin.z)) /
                                                 r.data.cell[k * 4]) *
                                      r.data.cell[k * 4]);
                    break;
                default:
                    break;
                }
                if (keep) {
                    for (auto &[name, values] : r.data.scalarProperties) {
                        if (values.size() != r.data.atoms.size())
                            throw std::runtime_error("Particle property length mismatch: " + name);
                        values[out] = values[i];
                    }
                    for (auto &[name, values] : r.data.vectorProperties) {
                        if (values.size() != r.data.atoms.size())
                            throw std::runtime_error("Particle property length mismatch: " + name);
                        values[out] = values[i];
                    }
                    r.data.atoms[out] = a;
                    r.selected[out++] = sel;
                }
            }
            r.data.atoms.resize(out);
            for (auto &[name, values] : r.data.scalarProperties)
                values.resize(out);
            for (auto &[name, values] : r.data.vectorProperties)
                values.resize(out);
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
            if (m.op == Op::Scale) {
                r.data.origin.x *= m.value;
                r.data.origin.y *= m.value;
                r.data.origin.z *= m.value;
            }
            if (m.op == Op::Rotate) {
                float angle = m.value * .0174532925199433f;
                Atom origin{r.data.origin.x, r.data.origin.y, r.data.origin.z, 0};
                int u = (m.axis + 1) % 3, v = (m.axis + 2) % 3;
                float x = coordinate(origin, u), y = coordinate(origin, v);
                coordinate(origin, u) = x * std::cos(angle) - y * std::sin(angle);
                coordinate(origin, v) = x * std::sin(angle) + y * std::cos(angle);
                r.data.origin = {origin.x, origin.y, origin.z};
            }
        }
    r.data.bounds();
    return r;
}
inline PipelineResult evaluate(const Dataset &source, const PipelineGraph &graph) {
    std::vector<Modifier> executable;
    executable.reserve(graph.nodes.size());
    for (const auto &node : graph.nodes)
        executable.push_back(static_cast<const Modifier &>(node));
    return evaluate(source, executable);
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
