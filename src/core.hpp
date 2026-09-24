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
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
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
struct Bond {
    uint32_t a = 0, b = 0;
    // Integer translation of particle b by the simulation cell vectors.
    // For a non-periodic bond this is {0,0,0}.
    std::array<int32_t, 3> image{};
    bool operator==(const Bond &) const = default;
};
struct DataTable {
    std::string name;
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
};
inline std::string formatDataNumber(double value) {
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return stream.str();
}
inline void writeDataTableCsv(const std::filesystem::path &path, const DataTable &table) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("Cannot open data table CSV output");
    auto field = [&](const std::string &value) {
        output << '"';
        for (char c : value) { if (c == '"') output << '"'; output << c; }
        output << '"';
    };
    for (size_t column=0;column<table.columns.size();++column) {
        if (column) output << ',';
        field(table.columns[column]);
    }
    output << "\r\n";
    for (const auto &row : table.rows) {
        for (size_t column=0;column<table.columns.size();++column) {
            if (column) output << ',';
            field(column<row.size()?row[column]:std::string{});
        }
        output << "\r\n";
    }
    output.flush();
    if (!output) throw std::runtime_error("Data table CSV write failed");
}
struct BondStyle {
    bool visible = true;
    float width = 1.5f;
    // A positive radius selects a shaded world-space cylinder; zero keeps screen-space lines.
    float radius = 0;
    std::array<float, 4> color{.72f,.78f,.86f,1.f};
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
    std::unordered_map<std::string, double> globalAttributes;
    std::vector<DataTable> tables;
    // Explicit pair topology published by bond-producing modifiers.
    std::vector<Bond> bonds;
    BondStyle bondStyle;
    std::vector<Vec3> particleColors;
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

struct NumericParticlePropertyView {
    const std::vector<double> *scalar = nullptr;
    const std::vector<Vec3> *vector = nullptr;
    int component = 0;

    double value(const Dataset &data, size_t index) const {
        if (scalar) return (*scalar)[index];
        if (vector) {
            const auto &v=(*vector)[index];
            return component==0 ? v.x : component==1 ? v.y : v.z;
        }
        return coordinate(data.atoms[index],component);
    }
};
inline NumericParticlePropertyView numericParticlePropertyView(const Dataset &data,
                                                               const std::string &property) {
    if (property=="Position.X" || property=="Position.Y" || property=="Position.Z")
        return {nullptr,nullptr,property.back()=='X'?0:property.back()=='Y'?1:2};
    if (auto found=data.scalarProperties.find(property);found!=data.scalarProperties.end()) {
        if (found->second.size()!=data.atoms.size())
            throw std::runtime_error("Particle property length mismatch: " + property);
        return {&found->second,nullptr,0};
    }
    if (property.size()>2 && property[property.size()-2]=='.') {
        const char suffix=property.back();
        const int component=suffix=='X'?0:suffix=='Y'?1:suffix=='Z'?2:-1;
        if (component>=0) {
            const auto base=property.substr(0,property.size()-2);
            if (auto found=data.vectorProperties.find(base);found!=data.vectorProperties.end()) {
                if (found->second.size()!=data.atoms.size())
                    throw std::runtime_error("Particle property length mismatch: " + property);
                return {nullptr,&found->second,component};
            }
        }
    }
    throw std::runtime_error("Unknown numeric particle property: " + property);
}

inline std::array<double,3> bondVector(const Dataset &data, const Bond &bond) {
    if (bond.a >= data.atoms.size() || bond.b >= data.atoms.size())
        throw std::runtime_error("Bond endpoint is outside the particle array");
    for (int axis=0;axis<3;++axis) if (bond.image[axis]!=0) {
        if (!data.pbc[axis])
            throw std::runtime_error("Bond image shift uses a non-periodic axis");
        const double length=std::hypot(data.cell[axis*3],data.cell[axis*3+1],data.cell[axis*3+2]);
        if (!(length>0) || !std::isfinite(length))
            throw std::runtime_error("Bond image shift uses an invalid periodic cell vector");
    }
    const auto &a=data.atoms[bond.a], &b=data.atoms[bond.b];
    std::array<double,3> vector{double(b.x)-a.x,double(b.y)-a.y,double(b.z)-a.z};
    for (int axis=0;axis<3;++axis)
        for (int component=0;component<3;++component)
            vector[component]+=double(bond.image[axis])*data.cell[axis*3+component];
    if (!std::all_of(vector.begin(),vector.end(),[](double value){return std::isfinite(value);}))
        throw std::runtime_error("Bond has non-finite geometry");
    if (!(std::hypot(vector[0],vector[1],vector[2])>0))
        throw std::runtime_error("Bond has zero-length geometry");
    return vector;
}

inline std::vector<double> particlePropertyValues(const Dataset &data,
                                                  const std::string &property,
                                                  std::atomic<bool> *cancel = nullptr) {
    auto checkCancelled=[&](size_t index) {
        if ((index & 65535)==0 && cancel && *cancel)
            throw std::runtime_error("Cancelled");
    };
    if (property == "Position.X" || property == "Position.Y" || property == "Position.Z") {
        const int axis = property.back() == 'X' ? 0 : property.back() == 'Y' ? 1 : 2;
        std::vector<double> values;
        values.reserve(data.atoms.size());
        for (size_t i=0;i<data.atoms.size();++i) {
            checkCancelled(i);
            values.push_back(coordinate(data.atoms[i], axis));
        }
        return values;
    }
    if (auto scalar = data.scalarProperties.find(property); scalar != data.scalarProperties.end()) {
        if (scalar->second.size() != data.atoms.size())
            throw std::runtime_error("Particle property length mismatch: " + property);
        std::vector<double> values;
        values.reserve(scalar->second.size());
        for (size_t i=0;i<scalar->second.size();++i) {
            checkCancelled(i);
            values.push_back(scalar->second[i]);
        }
        return values;
    }
    if (property.size() > 2 && property[property.size() - 2] == '.') {
        const char component = property.back();
        const int axis = component == 'X' ? 0 : component == 'Y' ? 1 : component == 'Z' ? 2 : -1;
        if (axis >= 0) {
            const auto name = property.substr(0, property.size() - 2);
            if (auto vector = data.vectorProperties.find(name); vector != data.vectorProperties.end()) {
                if (vector->second.size() != data.atoms.size())
                    throw std::runtime_error("Particle property length mismatch: " + property);
                std::vector<double> values;
                values.reserve(vector->second.size());
                for (size_t i=0;i<vector->second.size();++i) {
                    checkCancelled(i);
                    const auto &value=vector->second[i];
                    values.push_back(axis == 0 ? value.x : axis == 1 ? value.y : value.z);
                }
                return values;
            }
        }
    }
    throw std::runtime_error("Unknown particle property: " + property);
}

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
    // OVITO parity: an XYZ frame without an explicit pbc attribute is treated
    // as fully periodic whenever a Lattice is present (the extended-XYZ file
    // format has no standard default, and OVITO assumes a periodic cell).
    const bool explicitPbc = !attribute(fr.comment, "pbc").empty();
    std::istringstream periodic(attribute(fr.comment, "pbc"));
    for (auto &b : d.pbc) {
        std::string v;
        periodic >> v;
        b = v == "T" || v == "1" || v == "true";
    }
    if (!explicitPbc &&
        std::any_of(d.cell.begin(), d.cell.end(), [](double v) { return v != 0; }))
        d.pbc = {true, true, true};
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
    ,CoordinationAnalysis
    ,ClusterAnalysis
    ,RadialDistribution
    ,Histogram
    ,ReduceProperty
    ,AssignColor
    ,ManualSelection
    ,EditCell
    ,AffineTransform
    ,BondLengthDistribution
    ,BondAngleDistribution
    ,ScatterPlot
    ,CentrosymmetryParameter
    ,DisplacementVectors
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
    bool discardExistingBonds = false;
    bool bondTypeCutoffsEnabled = false;
    bool bondCylinders = false;
    std::vector<float> bondTypeCutoffs;
    int colorGradient = 0;
    bool colorAutoRange = true, colorSymmetricRange = false, colorReverse = false;
    bool colorAllFramesRange = false;
    bool colorDiscrete = false, colorSelectedOnly = false, colorKeepSelection = false;
    bool colorLegend = true;
    float colorMin = 0, colorMax = 1;
    int reduceOperation = 2; // min, max, mean, sum
    std::string scatterXProperty = "Position.X", scatterYProperty = "Position.Y";
    bool scatterSelectedOnly = false;
    bool bondsVisible = true;
    float bondWidth = 1.5f;
    float bondRadius = .08f;
    std::array<float,4> bondColor{.72f,.78f,.86f,1.f};
    std::array<float,3> assignColor{1.f,.15f,.12f};
    std::vector<uint32_t> manualSelection;
    std::array<double, 9> editedCell{};
    Vec3 editedOrigin{};
    std::array<bool, 3> editedPbc{};
    bool transformCoordinatesWithCell = false;
    bool overlapUseRadii = false;
    bool transformVectorProperties = false;
    bool clusterByBonds = false;
    bool clusterOnlySelected = false;
    bool clusterSortBySize = false;
    std::array<double, 12> affineTransform{1,0,0,0, 0,1,0,0, 0,0,1,0};
    int histogramNormalization = 0; // counts, relative frequency, probability density
    int rdfBins = 128;
    bool neighborOnlySelected = false;
    bool histogramSelectedOnly = false;
    bool histogramSelectRange = false;
    double histogramRangeStart = 0, histogramRangeEnd = 1;
    float sliceNormal[3]{1, 0, 0};
    double sliceDistance = 0;
    double sliceWidth = 0;
    bool sliceInvert = false;
    bool sliceShowPlane = false;
    bool sliceCreateSelection = false;
    bool sliceApplySelectionOnly = false;
    bool sliceOperateOnParticles = true;
    int replicateN[3]{1, 1, 1};
    bool replicateAdjustBox = true;
    int cspNeighbors = 12;
    int cspMode = 0; // 0 = Conventional CSP, 1 = Minimum-weight matching CSP
    bool cspOnlySelected = false;
    // Calculate displacements: reference frame within the same pipeline
    // (absolute number or offset relative to the current frame), optional
    // affine mapping of the simulation cell and the minimum image convention.
    int displacementFrame = 0;         // absolute reference frame number
    bool displacementRelative = false; // use offset relative to current frame
    int displacementOffset = -1;       // relative frame offset (default: previous frame)
    int displacementCellMapping = 0;   // 0 = off, 1 = to reference, 2 = to current
    bool displacementMinimumImage = true;
    bool affineReducedCoords = false;
    bool affineOnlySelected = false;
    int expandMode = 0; // 0 cutoff range, 1 N nearest, 2 bonded, 3 same molecule
    int expandNeighbors = 10;
    // Particle types checked in the Select type panel; empty falls back to the
    // legacy single-type field (m.type < 0 selects nothing).
    std::vector<uint32_t> selectedTypes;
};
// Returns properties visible at a node's input without evaluating particle
// operations. This is the schema counterpart to evaluatePrefix(): particle
// filters preserve property names, while property-producing/removing nodes
// update the schema in pipeline order.
template<class ModifierRange>
inline std::vector<std::string> pipelineInputPropertyChoices(
    const Dataset &source, const ModifierRange &modifiers,
    size_t nodeIndex, bool includeVectorComponents = false) {
    std::set<std::string> scalar, vector;
    for (const auto &[name, values] : source.scalarProperties)
        if (values.size() == source.atoms.size()) scalar.insert(name);
    for (const auto &[name, values] : source.vectorProperties)
        if (values.size() == source.atoms.size()) vector.insert(name);
    nodeIndex = std::min(nodeIndex, modifiers.size());
    for (size_t i = 0; i < nodeIndex; ++i) {
        const auto &modifier = modifiers[i];
        if (!modifier.enabled) continue;
        switch (modifier.op) {
        case Op::RemoveProperty:
            scalar.erase(modifier.property);
            vector.erase(modifier.property);
            break;
        case Op::ComputeProperty:
            scalar.insert(modifier.outputProperty);
            break;
        case Op::ColorCoding:
            scalar.insert("Color coding");
            break;
        case Op::ColorType:
            scalar.erase("Color coding");
            break;
        case Op::CoordinationAnalysis:
            scalar.insert("Coordination");
            break;
        case Op::ClusterAnalysis:
            scalar.insert("Cluster");
            break;
        case Op::CommonNeighborAnalysis:
            scalar.insert("Structure Type");
            break;
        case Op::CentrosymmetryParameter:
            scalar.insert("Centrosymmetry");
            break;
        case Op::DisplacementVectors:
            scalar.insert("Displacement Magnitude");
            vector.insert("Displacement");
            break;
        default:
            break;
        }
    }
    std::vector<std::string> choices{"Position.X", "Position.Y", "Position.Z"};
    choices.insert(choices.end(), scalar.begin(), scalar.end());
    if (includeVectorComponents)
        for (const auto &name : vector)
            for (const char *axis : {"X", "Y", "Z"})
                choices.push_back(name + "." + axis);
    std::sort(choices.begin() + 3, choices.end());
    return choices;
}
struct DataObject {
    enum class Kind { Particles, Bonds, Cell, Surface, Dislocations, VoxelGrid, Table, Labels, GlobalAttributes };
    Kind kind = Kind::Particles;
    std::string name;
    bool visible = true;
    size_t sourceNode = 0;
};
inline const char *opName(Op op);
inline std::vector<DataObject> modifierOutputs(const Modifier &modifier, size_t sourceNode) {
    using Kind=DataObject::Kind;
    std::vector<DataObject> outputs;
    auto add=[&](Kind kind,std::string name){outputs.push_back({kind,std::move(name),true,sourceNode});};
    if (modifier.op==Op::CreateBonds) add(Kind::Bonds,"Bonds");
    if (modifier.op==Op::EditCell) add(Kind::Cell,"Simulation cell");
    if (modifier.op==Op::ManualSelection) add(Kind::Particles,"Manual selection");
    if (modifier.op==Op::Histogram && modifier.histogramSelectRange)
        add(Kind::Particles,"Value-range selection");
    if (modifier.op==Op::ColorCoding || modifier.op==Op::ColorType || modifier.op==Op::AssignColor)
        add(Kind::Particles,"Particle colors");
    if (modifier.op==Op::ComputeProperty) add(Kind::Particles,modifier.outputProperty);
    if (modifier.op==Op::CommonNeighborAnalysis) {
        add(Kind::Particles,"CNA structure types");
        add(Kind::GlobalAttributes,"Structure counts");
        add(Kind::Table,"Common neighbor analysis");
    }
    if (modifier.op==Op::CoordinationAnalysis) {
        add(Kind::Particles,"Coordination");
        add(Kind::GlobalAttributes,"Coordination statistics");
        add(Kind::Table,"Coordination number distribution");
    }
    if (modifier.op==Op::CentrosymmetryParameter) {
        add(Kind::Particles,"Centrosymmetry");
        add(Kind::GlobalAttributes,"Centrosymmetry statistics");
    }
    if (modifier.op==Op::DisplacementVectors) {
        add(Kind::Particles,"Displacement");
        add(Kind::Particles,"Displacement Magnitude");
    }
    if (modifier.op==Op::ClusterAnalysis) {
        add(Kind::Particles,"Cluster IDs");
        add(Kind::GlobalAttributes,"Cluster statistics");
        add(Kind::Table,"Cluster analysis");
    }
    if (modifier.op==Op::RadialDistribution || modifier.op==Op::Histogram ||
        modifier.op==Op::BondLengthDistribution || modifier.op==Op::BondAngleDistribution ||
        modifier.op==Op::ScatterPlot)
        add(Kind::Table,opName(modifier.op));
    if (modifier.op==Op::RadialDistribution || modifier.op==Op::Histogram ||
        modifier.op==Op::ReduceProperty ||
        modifier.op==Op::BondLengthDistribution || modifier.op==Op::BondAngleDistribution ||
        modifier.op==Op::ScatterPlot)
        add(Kind::GlobalAttributes,opName(modifier.op)+std::string(" statistics"));
    return outputs;
}

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
inline bool applyModifierHistory(std::vector<ModifierNode> &current,
                                 std::vector<std::vector<ModifierNode>> &undo,
                                 std::vector<std::vector<ModifierNode>> &redo,
                                 bool forward) {
    auto &source = forward ? redo : undo;
    auto &destination = forward ? undo : redo;
    if (source.empty()) return false;
    destination.push_back(current);
    current = std::move(source.back());
    source.pop_back();
    return true;
}
struct InteractiveEditCheckpoint {
    bool mouseGestureCheckpointed = false;
    void beginFrame(bool leftMouseDown) {
        if (!leftMouseDown) mouseGestureCheckpointed = false;
    }
    bool shouldRecord(bool leftMouseDown) {
        if (!leftMouseDown) return true;
        if (mouseGestureCheckpointed) return false;
        mouseGestureCheckpointed = true;
        return true;
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

inline double cellDeterminant(const std::array<double, 9> &c) {
    return c[0] * (c[4]*c[8] - c[5]*c[7]) -
           c[1] * (c[3]*c[8] - c[5]*c[6]) +
           c[2] * (c[3]*c[7] - c[4]*c[6]);
}
inline std::array<std::array<double, 3>, 3> cellInverse(const std::array<double, 9> &c) {
    const double determinant = cellDeterminant(c);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12)
        throw std::runtime_error("Simulation cell is singular");
    // The stored vectors are consecutive rows; convert the vector matrix to
    // the Cartesian-from-fractional column convention before inversion.
    const double a = c[0], b = c[3], cc = c[6];
    const double d = c[1], e = c[4], f = c[7];
    const double g = c[2], h = c[5], i = c[8];
    const double det = a*(e*i-f*h)-b*(d*i-f*g)+cc*(d*h-e*g);
    std::array<std::array<double,3>,3> inv{{
        {{(e*i-f*h)/det, (cc*h-b*i)/det, (b*f-cc*e)/det}},
        {{(f*g-d*i)/det, (a*i-cc*g)/det, (cc*d-a*f)/det}},
        {{(d*h-e*g)/det, (b*g-a*h)/det, (a*e-b*d)/det}}
    }};
    return inv;
}
inline std::array<double, 3> fractionalPosition(const Dataset &d, const Atom &atom,
                                                const std::array<std::array<double,3>,3> &inverse) {
    const double x = double(atom.x) - d.origin.x;
    const double y = double(atom.y) - d.origin.y;
    const double z = double(atom.z) - d.origin.z;
    return {inverse[0][0]*x + inverse[0][1]*y + inverse[0][2]*z,
            inverse[1][0]*x + inverse[1][1]*y + inverse[1][2]*z,
            inverse[2][0]*x + inverse[2][1]*y + inverse[2][2]*z};
}
inline Atom wrapAtomInCell(const Dataset &data, Atom atom) {
    std::array<int, 3> periodicAxes{};
    int periodicCount = 0;
    for (int axis = 0; axis < 3; ++axis)
        if (data.pbc[axis]) periodicAxes[periodicCount++] = axis;
    if (periodicCount == 0) return atom;

    const std::array<double, 3> position{atom.x - data.origin.x,
                                         atom.y - data.origin.y,
                                         atom.z - data.origin.z};
    double augmented[3][4]{};
    double matrixScale = 0;
    for (int row = 0; row < periodicCount; ++row) {
        const int a = periodicAxes[row];
        for (int col = 0; col < periodicCount; ++col) {
            const int b = periodicAxes[col];
            for (int xyz = 0; xyz < 3; ++xyz)
                augmented[row][col] += data.cell[a * 3 + xyz] * data.cell[b * 3 + xyz];
        }
        for (int xyz = 0; xyz < 3; ++xyz)
            augmented[row][periodicCount] += data.cell[a * 3 + xyz] * position[xyz];
        matrixScale = std::max(matrixScale, augmented[row][row]);
    }
    if (!(matrixScale > 0) || !std::isfinite(matrixScale))
        throw std::runtime_error("Periodic cell vectors must be nonzero and finite");
    for (int column = 0; column < periodicCount; ++column) {
        int pivot = column;
        for (int row = column + 1; row < periodicCount; ++row)
            if (std::abs(augmented[row][column]) > std::abs(augmented[pivot][column])) pivot = row;
        if (std::abs(augmented[pivot][column]) <= matrixScale * 1e-12)
            throw std::runtime_error("Periodic cell vectors are linearly dependent");
        if (pivot != column)
            for (int col = column; col <= periodicCount; ++col)
                std::swap(augmented[pivot][col], augmented[column][col]);
        for (int row = column + 1; row < periodicCount; ++row) {
            const double factor = augmented[row][column] / augmented[column][column];
            for (int col = column; col <= periodicCount; ++col)
                augmented[row][col] -= factor * augmented[column][col];
        }
    }
    std::array<double, 3> fractional{};
    for (int row = periodicCount - 1; row >= 0; --row) {
        double rhs = augmented[row][periodicCount];
        for (int col = row + 1; col < periodicCount; ++col)
            rhs -= augmented[row][col] * fractional[col];
        fractional[row] = rhs / augmented[row][row];
        if (!std::isfinite(fractional[row]))
            throw std::runtime_error("Particle position cannot be wrapped in this periodic cell");
    }
    std::array<double, 3> wrapped{atom.x, atom.y, atom.z};
    for (int component = 0; component < 3; ++component)
        for (int i = 0; i < periodicCount; ++i) {
            const int axis = periodicAxes[i];
            wrapped[component] -= std::floor(fractional[i]) * data.cell[axis * 3 + component];
        }
    atom.x = float(wrapped[0]); atom.y = float(wrapped[1]); atom.z = float(wrapped[2]);
    if (!std::isfinite(atom.x) || !std::isfinite(atom.y) || !std::isfinite(atom.z))
        throw std::runtime_error("Wrapped particle position is non-finite");
    return atom;
}
inline std::array<double,9> neighborSearchCell(const Dataset &d) {
    auto basis=d.cell;
    std::vector<std::array<double,3>> orthonormal;
    double scale=0;
    for (int axis=0;axis<3;++axis) if (d.pbc[axis]) {
        std::array<double,3> vector{d.cell[axis*3],d.cell[axis*3+1],d.cell[axis*3+2]};
        const double length=std::sqrt(vector[0]*vector[0]+vector[1]*vector[1]+vector[2]*vector[2]);
        if (!(length>0) || !std::isfinite(length))
            throw std::runtime_error("Periodic simulation cell vector is zero or non-finite");
        scale=std::max(scale,length);
        for (const auto &q:orthonormal) {
            const double projection=vector[0]*q[0]+vector[1]*q[1]+vector[2]*q[2];
            for (int xyz=0;xyz<3;++xyz) vector[xyz]-=projection*q[xyz];
        }
        const double residual=std::sqrt(vector[0]*vector[0]+vector[1]*vector[1]+vector[2]*vector[2]);
        if (!(residual>length*1e-12))
            throw std::runtime_error("Periodic cell vectors are linearly dependent");
        for (double &component:vector) component/=residual;
        orthonormal.push_back(vector);
    }
    if (scale==0) scale=1;
    for (int axis=0;axis<3;++axis) if (!d.pbc[axis]) {
        std::array<double,3> best{};
        double bestLength=0;
        for (int candidate=0;candidate<3;++candidate) {
            std::array<double,3> residual{};
            residual[candidate]=1;
            for (const auto &q:orthonormal) {
                const double projection=residual[0]*q[0]+residual[1]*q[1]+residual[2]*q[2];
                for (int xyz=0;xyz<3;++xyz) residual[xyz]-=projection*q[xyz];
            }
            const double length=std::sqrt(residual[0]*residual[0]+residual[1]*residual[1]+residual[2]*residual[2]);
            if (length>bestLength) { bestLength=length; best=residual; }
        }
        if (!(bestLength>1e-10))
            throw std::runtime_error("Could not construct a nonsingular neighbor-search basis");
        for (int xyz=0;xyz<3;++xyz) {
            best[xyz]*=scale/bestLength;
            basis[axis*3+xyz]=best[xyz];
            best[xyz]/=scale;
        }
        orthonormal.push_back(best);
    }
    return basis;
}
template <typename Callback>
inline void forEachTriclinicNeighborPair(const Dataset &d, double cutoff, Callback &&callback,
                                         std::atomic<bool> *cancel) {
    // Non-periodic cell vectors are irrelevant to minimum-image distances.
    // Complete missing/degenerate non-periodic axes so slab and wire cells can
    // still use exact fractional-space bins without requiring a 3D cell volume.
    const auto searchCell=neighborSearchCell(d);
    const auto inverse = cellInverse(searchCell);
    std::array<double,3> reach{}, width{};
    std::array<int64_t,3> periodicBins{};
    for (int axis=0; axis<3; ++axis) {
        double reciprocalNorm = std::sqrt(inverse[axis][0]*inverse[axis][0] +
                                          inverse[axis][1]*inverse[axis][1] +
                                          inverse[axis][2]*inverse[axis][2]);
        reach[axis] = cutoff * reciprocalNorm;
        if (d.pbc[axis]) {
            const int64_t bins = std::max<int64_t>(1, int64_t(std::floor(1.0 / reach[axis])));
            periodicBins[axis] = bins;
            width[axis] = 1.0 / bins;
        } else width[axis] = reach[axis];
    }
    double shortestSquared = std::numeric_limits<double>::infinity();
    std::array<int64_t,3> shortestLo{},shortestHi{};
    uint64_t shortestStates=1;
    for (int axis=0;axis<3;++axis) {
        if (!d.pbc[axis]) { shortestLo[axis]=shortestHi[axis]=0; continue; }
        const double length2=d.cell[axis*3]*d.cell[axis*3]+d.cell[axis*3+1]*d.cell[axis*3+1]+d.cell[axis*3+2]*d.cell[axis*3+2];
        shortestSquared=std::min(shortestSquared,length2);
    }
    if (!(shortestSquared>0)) throw std::runtime_error("Periodic cell has no nonzero lattice vector");
    for (int axis=0;axis<3;++axis) if (d.pbc[axis]) {
        const double reciprocalNorm=std::sqrt(inverse[axis][0]*inverse[axis][0]+
                                              inverse[axis][1]*inverse[axis][1]+
                                              inverse[axis][2]*inverse[axis][2]);
        const double radiusValue=std::ceil(std::sqrt(shortestSquared)*reciprocalNorm);
        if (!std::isfinite(radiusValue) || radiusValue>32)
            throw std::runtime_error("Simulation cell basis is too skewed for exact periodic analysis");
        const int64_t radius=int64_t(radiusValue);
        shortestLo[axis]=-radius; shortestHi[axis]=radius;
        const uint64_t axisStates=uint64_t(2*radius+1);
        if (shortestStates>65536/axisStates) throw std::runtime_error("Simulation cell basis is too skewed for exact periodic analysis");
        shortestStates*=axisStates;
    }
    for (int64_t a=shortestLo[0];a<=shortestHi[0];++a)
        for (int64_t b=shortestLo[1];b<=shortestHi[1];++b)
            for (int64_t c=shortestLo[2];c<=shortestHi[2];++c) {
        const int64_t n[3]{a,b,c};
        if (!(a||b||c)) continue;
        std::array<double,3> v{};
        for (int k=0;k<3;++k) for (int xyz=0;xyz<3;++xyz) v[xyz]+=n[k]*d.cell[k*3+xyz];
        shortestSquared=std::min(shortestSquared,v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
    }
    if (!(shortestSquared > 0) || cutoff > .5 * std::sqrt(shortestSquared) + 1e-10)
        throw std::runtime_error("Periodic cutoff must not exceed half the shortest cell translation");

    std::vector<std::array<double,3>> fractional(d.atoms.size());
    std::unordered_map<NeighborBin, std::vector<uint32_t>, NeighborBinHash> grid;
    grid.reserve(d.atoms.size());
    auto binOf = [&](const std::array<double,3> &position) {
        NeighborBin bin;
        for (int axis=0;axis<3;++axis) {
            double q = position[axis];
            if (d.pbc[axis]) q -= std::floor(q);
            const double scaled = std::floor(q / width[axis]);
            if (!std::isfinite(scaled) || std::abs(scaled) > 1e15)
                throw std::runtime_error("Coordinates exceed spatial index range");
            binComponent(bin,axis)=int64_t(scaled);
        }
        return bin;
    };
    for (size_t i=0;i<d.atoms.size();++i) {
        if (cancel && *cancel) throw std::runtime_error("Cancelled");
        fractional[i]=fractionalPosition(d,d.atoms[i],inverse);
        grid[binOf(fractional[i])].push_back(uint32_t(i));
    }
    uint64_t comparisons=0;
    const double cutoff2=cutoff*cutoff;
    for (uint32_t i=0;i<d.atoms.size();++i) {
        if (cancel && *cancel) throw std::runtime_error("Cancelled");
        const auto center=binOf(fractional[i]);
        std::unordered_set<NeighborBin,NeighborBinHash> visited;
        const double radiusX=std::ceil(reach[0]/width[0])+1;
        const double radiusY=std::ceil(reach[1]/width[1])+1;
        const double radiusZ=std::ceil(reach[2]/width[2])+1;
        if (std::max({radiusX,radiusY,radiusZ})>64)
            throw std::runtime_error("Simulation cell basis is too skewed for exact spatial indexing");
        const int rx=int(radiusX), ry=int(radiusY), rz=int(radiusZ);
        for (int z=-rz;z<=rz;++z) for (int y=-ry;y<=ry;++y) for (int x=-rx;x<=rx;++x) {
            NeighborBin bin{center.x+x,center.y+y,center.z+z};
            for (int axis=0;axis<3;++axis) if (periodicBins[axis]) {
                auto &v=binComponent(bin,axis);
                v=(v%periodicBins[axis]+periodicBins[axis])%periodicBins[axis];
            }
            if (!visited.insert(bin).second) continue;
            auto found=grid.find(bin);
            if (found==grid.end()) continue;
            for (uint32_t j : found->second) {
                if (j<=i) continue;
                if (++comparisons>300000000) throw std::runtime_error("Neighbor comparison budget exceeded; reduce cutoff");
                std::array<double,3> delta{fractional[i][0]-fractional[j][0],
                                           fractional[i][1]-fractional[j][1],
                                           fractional[i][2]-fractional[j][2]};
                std::array<int64_t,3> shift{};
                for (int axis=0;axis<3;++axis) if (d.pbc[axis]) {
                    if (std::abs(delta[axis])>1e9) throw std::runtime_error("Periodic image index exceeds supported range");
                    shift[axis]=int64_t(std::round(delta[axis]));
                }
                auto distanceSquared = [&](const std::array<int64_t,3> &translation) {
                    std::array<double,3> cart{};
                    for (int xyz=0;xyz<3;++xyz)
                        for (int axis=0;axis<3;++axis)
                            cart[xyz]+=(delta[axis]-translation[axis])*searchCell[axis*3+xyz];
                    return cart[0]*cart[0]+cart[1]*cart[1]+cart[2]*cart[2];
                };
                double best=distanceSquared(shift);
                std::array<int64_t,3> lower{},upper{};
                uint64_t imageStates=1;
                for (int axis=0;axis<3;++axis) {
                    if (!d.pbc[axis]) { lower[axis]=upper[axis]=0; continue; }
                    const double reciprocalNorm=std::sqrt(inverse[axis][0]*inverse[axis][0]+
                                                          inverse[axis][1]*inverse[axis][1]+
                                                          inverse[axis][2]*inverse[axis][2]);
                    const double bound=std::sqrt(best)*reciprocalNorm+1e-12;
                    if (!std::isfinite(bound) || bound>1e9) throw std::runtime_error("Periodic image search exceeds supported range");
                    lower[axis]=int64_t(std::ceil(delta[axis]-bound));
                    upper[axis]=int64_t(std::floor(delta[axis]+bound));
                    const uint64_t axisStates=uint64_t(std::max<int64_t>(1,upper[axis]-lower[axis]+1));
                    if (imageStates>65536/axisStates) throw std::runtime_error("Periodic image search exceeded the skew-cell limit");
                    imageStates*=axisStates;
                }
                for (int64_t iz=lower[2];iz<=upper[2];++iz)
                    for (int64_t iy=lower[1];iy<=upper[1];++iy)
                        for (int64_t ix=lower[0];ix<=upper[0];++ix) {
                    const std::array<int64_t,3> translation{ix,iy,iz};
                    const double distance2=distanceSquared(translation);
                    if (distance2<best) { best=distance2; shift=translation; }
                }
                if (best<=cutoff2) {
                    std::array<int32_t,3> bestImage{};
                    for (int axis=0;axis<3;++axis) {
                        if (shift[axis] < -INT32_MAX || shift[axis] > INT32_MAX)
                            throw std::runtime_error("Periodic image index exceeds supported range");
                        bestImage[axis]=int32_t(-shift[axis]);
                    }
                    if constexpr (std::is_invocable_v<Callback,uint32_t,uint32_t,double,
                                                      std::array<int32_t,3>>)
                        callback(i,j,best,bestImage);
                    else callback(i,j,best);
                }
            }
        }
    }
}

// Visits each cutoff pair once using linked spatial bins. Sampled previews are
// rejected, while exact minimum-image distances support orthogonal and triclinic PBC.
inline void validateNeighborAnalysisInput(const Dataset &d) {
    if (d.sampled())
        throw std::runtime_error("Neighbor analysis requires full data; increase the import budget.");
    if (d.atoms.size()>2000000)
        throw std::runtime_error("Neighbor analysis currently limited to 2 million atoms");
}
template <typename Callback>
inline void forEachNeighborPair(const Dataset &d, double cutoff, Callback &&callback,
                                std::atomic<bool> *cancel = nullptr) {
    validateNeighborAnalysisInput(d);
    if (!(cutoff > 0) || !std::isfinite(cutoff))
        throw std::runtime_error("Cutoff must be finite and positive");
    const bool hasPeriodic = std::any_of(d.pbc.begin(), d.pbc.end(), [](bool b) { return b; });
    const bool triclinic = d.cell[1] || d.cell[2] || d.cell[3] || d.cell[5] || d.cell[6] || d.cell[7];
    if (hasPeriodic && triclinic) {
        if (std::none_of(d.cell.begin(),d.cell.end(),[](double v){return v!=0;}))
            throw std::runtime_error("Periodic analysis requires a valid simulation cell");
        forEachTriclinicNeighborPair(d,cutoff,std::forward<Callback>(callback),cancel);
        return;
    }

    std::array<int64_t, 3> periodicBins{};
    std::array<double, 3> widths{cutoff, cutoff, cutoff};
    for (int axis = 0; axis < 3; ++axis)
        if (d.pbc[axis]) {
            double length = d.cell[axis * 4];
            if (!(length > 0) || !std::isfinite(length))
                throw std::runtime_error("Periodic cell length must be finite and positive");
            if (!(length >= 2 * cutoff))
                throw std::runtime_error("Periodic cell must be at least twice the cutoff");
            const double binCount = length / cutoff;
            if (!std::isfinite(binCount) || binCount >= 1e15)
                throw std::runtime_error("Periodic cell-to-cutoff ratio exceeds the exact spatial-index range");
            periodicBins[axis] = std::max<int64_t>(1, int64_t(binCount));
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
                        std::array<int32_t, 3> image{};
                        for (int axis = 0; axis < 3; ++axis) {
                            double delta = double(coordinate(d.atoms[i], axis)) - coordinate(d.atoms[j], axis);
                            if (d.pbc[axis]) {
                                const double crossings = std::round(delta / d.cell[axis * 4]);
                                delta -= crossings * d.cell[axis * 4];
                                image[axis] = int32_t(-crossings);
                            }
                            distance2 += delta * delta;
                        }
                        if (distance2 <= cutoff2) {
                            if constexpr (std::is_invocable_v<Callback, uint32_t, uint32_t, double,
                                                               std::array<int32_t, 3>>)
                                callback(i, j, distance2, image);
                            else
                                callback(i, j, distance2);
                        }
                    }
                }
    }
}

struct NeighborAnalysis {
    std::vector<uint32_t> coordination, cluster;
    uint32_t clusters = 0;
    uint64_t bonds = 0;
    double meanCoordination = 0;
    std::vector<uint64_t> pairHistogram;
    std::vector<double> rdf;
    float cutoff = 0;
    bool rdfValid = false;
};
inline NeighborAnalysis neighbors(const Dataset &d, float cutoff,
                                  std::atomic<bool> *cancel = nullptr,
                                  const std::vector<uint8_t> *clusterSelection=nullptr,
                                  size_t radialBins=128,
                                  const std::vector<uint8_t> *analysisSelection=nullptr) {
    validateNeighborAnalysisInput(d);
    if (radialBins==0 || radialBins>4096)
        throw std::runtime_error("Radial distribution bin count must be between 1 and 4096");
    if (clusterSelection && clusterSelection->size()!=d.atoms.size())
        throw std::runtime_error("Cluster selection length does not match particle count");
    if (analysisSelection && analysisSelection->size()!=d.atoms.size())
        throw std::runtime_error("Analysis selection length does not match particle count");
    NeighborAnalysis result;
    result.cutoff = cutoff;
    result.pairHistogram.resize(radialBins);
    result.rdf.resize(radialBins);
    result.coordination.resize(d.atoms.size());
    result.cluster.resize(d.atoms.size());
    size_t analysisParticleCount=d.atoms.size();
    if (analysisSelection) {
        analysisParticleCount=0;
        for (size_t i=0;i<analysisSelection->size();++i) {
            if ((i&65535)==0 && cancel && *cancel) throw std::runtime_error("Cancelled");
            if ((*analysisSelection)[i]) ++analysisParticleCount;
        }
    }
    for (uint32_t i=0;i<result.cluster.size();++i) result.cluster[i]=i;
    auto root=[&](uint32_t i) {
        while (i!=result.cluster[i]) {
            result.cluster[i]=result.cluster[result.cluster[i]];
            i=result.cluster[i];
        }
        return i;
    };
    forEachNeighborPair(d,cutoff,[&](uint32_t i,uint32_t j,double distanceSquared) {
        if (analysisSelection && (!(*analysisSelection)[i] || !(*analysisSelection)[j])) return;
        ++result.coordination[i]; ++result.coordination[j]; ++result.bonds;
        const size_t bin=std::min(radialBins-1,
            size_t(std::sqrt(distanceSquared)/cutoff*radialBins));
        result.pairHistogram[bin]++;
        if (!clusterSelection || ((*clusterSelection)[i] && (*clusterSelection)[j])) {
            const auto ri=root(i), rj=root(j);
            if (ri!=rj) result.cluster[std::max(ri,rj)]=std::min(ri,rj);
        }
    },cancel);
    std::unordered_map<uint32_t,uint32_t> ids;
    for (uint32_t i=0;i<result.cluster.size();++i) {
        if (clusterSelection && !(*clusterSelection)[i]) {
            result.cluster[i]=0;
            continue;
        }
        const auto rt=root(i);
        auto [it,inserted]=ids.emplace(rt,uint32_t(ids.size())+1);
        result.cluster[i]=rt;
    }
    for (uint32_t i=0;i<result.cluster.size();++i)
        if (!clusterSelection || (*clusterSelection)[i]) result.cluster[i]=ids.at(result.cluster[i]);
    result.clusters=uint32_t(ids.size());
    result.meanCoordination=analysisParticleCount==0?0:2.0*result.bonds/analysisParticleCount;
    const double volume=std::abs(cellDeterminant(d.cell));
    result.rdfValid=d.pbc[0]&&d.pbc[1]&&d.pbc[2]&&volume>0&&analysisParticleCount>0;
    if (result.rdfValid) {
        const double density=analysisParticleCount/volume;
        for (size_t bin=0;bin<radialBins;++bin) {
            const double lo=double(cutoff)*bin/radialBins, hi=double(cutoff)*(bin+1)/radialBins;
            const double shell=(4.0/3.0)*3.141592653589793*(hi*hi*hi-lo*lo*lo);
            result.rdf[bin]=2.0*result.pairHistogram[bin]/(analysisParticleCount*density*shell);
        }
    }
    return result;
}
inline NeighborAnalysis clustersFromBonds(const Dataset &data,std::atomic<bool> *cancel=nullptr,
                                           const std::vector<uint8_t> *clusterSelection=nullptr) {
    if (data.sampled())
        throw std::runtime_error("Bond-based cluster analysis requires complete, unsampled particle data");
    if (data.atoms.size()>20'000'000)
        throw std::runtime_error("Bond-based cluster analysis is limited to 20 million particles");
    if (data.bonds.size()>20'000'000)
        throw std::runtime_error("Bond-based cluster analysis is limited to 20 million bonds");
    if (clusterSelection && clusterSelection->size()!=data.atoms.size())
        throw std::runtime_error("Cluster selection length does not match particle count");
    NeighborAnalysis result;
    result.cluster.resize(data.atoms.size());
    for (uint32_t i=0;i<result.cluster.size();++i) result.cluster[i]=i;
    auto root=[&](uint32_t i) {
        while (i!=result.cluster[i]) {
            result.cluster[i]=result.cluster[result.cluster[i]];
            i=result.cluster[i];
        }
        return i;
    };
    for (size_t i=0;i<data.bonds.size();++i) {
        if ((i&4095)==0 && cancel && *cancel) throw std::runtime_error("Cancelled");
        const auto &bond=data.bonds[i];
        if (bond.a>=data.atoms.size() || bond.b>=data.atoms.size())
            throw std::runtime_error("Bond endpoint is outside the particle array");
        if (clusterSelection && (!(*clusterSelection)[bond.a] || !(*clusterSelection)[bond.b]))
            continue;
        const uint32_t a=root(bond.a), b=root(bond.b);
        if (a!=b) result.cluster[std::max(a,b)]=std::min(a,b);
    }
    std::unordered_map<uint32_t,uint32_t> ids;
    ids.reserve(result.cluster.size());
    std::vector<uint32_t> representatives(result.cluster.size());
    for (uint32_t i=0;i<result.cluster.size();++i) {
        if ((i&65535)==0 && cancel && *cancel) throw std::runtime_error("Cancelled");
        if (clusterSelection && !(*clusterSelection)[i]) {
            result.cluster[i]=0;
            continue;
        }
        representatives[i]=root(i);
        if (!ids.contains(representatives[i]))
            ids.emplace(representatives[i],uint32_t(ids.size())+1);
    }
    for (size_t i=0;i<result.cluster.size();++i) {
        if ((i&65535)==0 && cancel && *cancel) throw std::runtime_error("Cancelled");
        if (!clusterSelection || (*clusterSelection)[i])
            result.cluster[i]=ids.at(representatives[i]);
    }
    result.clusters=uint32_t(ids.size());
    return result;
}
// OVITO Expand selection, "N nearest neighbors" mode: for every selected
// particle, mark its N nearest minimum-image neighbors. The exact shared
// neighbor kernel runs with an adaptively grown cutoff until every selected
// center sees at least N candidates (bounded by half the shortest periodic
// cell translation, mirroring the CSP neighbor search).
inline void expandSelectionNearestNeighbors(const Dataset &data, int neighborCount,
                                            const std::vector<uint8_t> &selected,
                                            std::vector<uint8_t> &expanded,
                                            std::atomic<bool> *cancel = nullptr) {
    validateNeighborAnalysisInput(data);
    if (neighborCount < 1)
        throw std::runtime_error("Neighbor count must be positive");
    if (selected.size()!=data.atoms.size() || expanded.size()!=data.atoms.size())
        throw std::runtime_error("Selection length does not match particle count");
    struct Entry { uint32_t index; double distanceSquared; };
    std::vector<std::vector<Entry>> adjacency(data.atoms.size());
    double cutoff = 1.0;
    const double volume = std::abs(cellDeterminant(data.cell));
    if (volume > 0 && data.atoms.size() > 1) {
        const double density = double(data.atoms.size()) / volume;
        cutoff = 2.0 * std::cbrt(3.0 * double(neighborCount) /
                                 (4.0 * 3.14159265358979323846 * density));
    }
    if (!(cutoff > 0) || !std::isfinite(cutoff)) cutoff = 1.0;
    double maxCutoff = std::numeric_limits<double>::infinity();
    for (int axis = 0; axis < 3; ++axis)
        if (data.pbc[axis])
            maxCutoff = std::min(maxCutoff, .5 * std::sqrt(
                data.cell[axis*3]*data.cell[axis*3] + data.cell[axis*3+1]*data.cell[axis*3+1] +
                data.cell[axis*3+2]*data.cell[axis*3+2]));
    bool found = false;
    for (int attempt = 0; attempt < 64 && !found; ++attempt) {
        if (cutoff > maxCutoff) cutoff = maxCutoff;
        for (auto &list : adjacency) list.clear();
        try {
            forEachNeighborPair(data, cutoff, [&](uint32_t i, uint32_t j, double distanceSquared) {
                adjacency[i].push_back({j, distanceSquared});
                adjacency[j].push_back({i, distanceSquared});
            }, cancel);
        } catch (const std::exception &e) {
            if (std::string(e.what()) == "Cancelled") throw;
            if (attempt == 0) throw;
            break; // keep the last completed pass for under-coordinated centers
        }
        found = true;
        for (size_t i = 0; i < data.atoms.size(); ++i)
            if (selected[i] && adjacency[i].size() < size_t(neighborCount)) { found = false; break; }
        if (!found) {
            if (cutoff >= maxCutoff) break;
            cutoff = std::min(cutoff * 1.3, maxCutoff);
        }
    }
    for (size_t i = 0; i < data.atoms.size(); ++i) {
        if ((i & 65535) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
        if (!selected[i]) continue;
        auto &list = adjacency[i];
        const size_t take = std::min(list.size(), size_t(neighborCount));
        if (!take) continue;
        std::nth_element(list.begin(), list.begin() + (take - 1), list.end(),
                         [](const Entry &a, const Entry &b) {
                             return a.distanceSquared < b.distanceSquared;
                         });
        for (size_t k = 0; k < take; ++k) expanded[list[k].index] = 1;
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
        ws();
        if (p < text.size() && text[p] == '`') {
            ++p;
            std::string result;
            while (p < text.size()) {
                if (text[p] == '`') {
                    if (p + 1 < text.size() && text[p + 1] == '`') {
                        result.push_back('`');
                        p += 2;
                        continue;
                    }
                    ++p;
                    if (result.empty()) error("Quoted property name is empty");
                    return result;
                }
                result.push_back(text[p++]);
            }
            error("Expected closing backtick for quoted property name");
        }
        size_t b = p;
        while (p < text.size()) { char c = text[p]; if (std::isalnum(static_cast<unsigned char>(c)) || c=='_' || c=='.') ++p; else break; }
        if (p == b) error("Expected a property or function name");
        return std::string(text.substr(b,p-b));
    }
    double property(const std::string &n) {
        const Atom *a = atom < d.atoms.size() ? &d.atoms[atom] : nullptr;
        if (n == "x" || n == "Position.X") return a ? a->x : 0;
        if (n == "y" || n == "Position.Y") return a ? a->y : 0;
        if (n == "z" || n == "Position.Z") return a ? a->z : 0;
        if (n == "type" || n == "Particle Type") return a ? a->type : 0;
        auto it = d.scalarProperties.find(n);
        if (it == d.scalarProperties.end()) error("Unknown scalar property '" + n + "'");
        if (it->second.size() != d.atoms.size()) error("Property length does not match particle count");
        if (!a) return 0;
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
    double evaluateNumber() { if(text.empty())error("Expression is empty");if(text.size()>511)error("Expression exceeds 511 characters");double v=exprOr();ws();if(p!=text.size())error("Unexpected token");if(!std::isfinite(v))error("Result is non-finite");return v; }
    void validate() { (void)evaluateNumber(); }
    bool evaluate() { return evaluateNumber()!=0; }
};

struct CNAResult {
    std::vector<uint8_t> structure;
    std::unordered_map<std::string, uint64_t> counts;
    std::map<std::tuple<int,int,int>, uint64_t> bondSignatureCounts;
};

inline CNAResult analyzeCommonNeighbors(const Dataset &d, double cutoff,
                                        std::atomic<bool> *cancel = nullptr) {
    validateNeighborAnalysisInput(d);
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
            if (common.size() > 12)
                throw std::runtime_error("CNA common-neighbor graph exceeds the exact-search limit");
            int edges = 0;
            std::vector<std::vector<uint8_t>> graph(common.size(), std::vector<uint8_t>(common.size()));
            for (size_t i=0;i<common.size();++i)
                for (size_t j=i+1;j<common.size();++j)
                    if (std::binary_search(adjacency[common[i]].begin(), adjacency[common[i]].end(), common[j])) {
                        graph[i][j]=graph[j][i]=1; ++edges;
                    }
            int chain = 0;
            {
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
                if (capped)
                    throw std::runtime_error("CNA signature search limit exceeded; reduce cutoff");
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

// Kelchner–Plimpton–Hamilton centrosymmetry parameter (OVITO S02). For every
// analyzed center, the N nearest minimum-image neighbor vectors are paired up;
// the CSP is the sum of the N/2 pair resultant weights |r_i + r_j|^2.
// Conventional mode greedily keeps the N/2 lightest pairs of all N(N-1)/2
// (LAMMPS compute centro/atom); matching mode solves the minimum-weight
// perfect pairing exactly (Larsen, arXiv:2003.08879).
struct CentrosymmetryResult {
    std::vector<double> values;
    double maxValue = 0;
    // Centers with fewer than N neighbors that could not even produce a valid
    // even pairing (>= 4 remaining); they report a CSP of 0 (OVITO behavior
    // for such systems is undocumented; see docs/parity/reference/S02).
    size_t underCoordinated = 0;
};
inline double centrosymmetryPairWeight(const std::array<double,3> &a, const std::array<double,3> &b) {
    const double x = a[0] + b[0], y = a[1] + b[1], z = a[2] + b[2];
    return x * x + y * y + z * z;
}
inline double centrosymmetryGreedy(const std::vector<std::array<double,3>> &v) {
    const size_t n = v.size();
    std::vector<double> weights;
    weights.reserve(n * (n - 1) / 2);
    for (size_t a = 0; a < n; ++a)
        for (size_t b = a + 1; b < n; ++b)
            weights.push_back(centrosymmetryPairWeight(v[a], v[b]));
    const size_t half = n / 2;
    std::nth_element(weights.begin(), weights.begin() + half, weights.end());
    double sum = 0;
    for (size_t i = 0; i < half; ++i) sum += weights[i];
    return sum;
}
inline double centrosymmetryMatching(const std::vector<std::array<double,3>> &v) {
    const size_t n = v.size();
    std::vector<double> dp(size_t(1) << n, std::numeric_limits<double>::infinity());
    dp[0] = 0;
    for (size_t mask = 0; mask + 1 < dp.size(); ++mask) {
        if (!std::isfinite(dp[mask])) continue;
        size_t first = 0;
        while (mask & (size_t(1) << first)) ++first;
        for (size_t b = first + 1; b < n; ++b) {
            if (mask & (size_t(1) << b)) continue;
            double &target = dp[mask | (size_t(1) << first) | (size_t(1) << b)];
            target = std::min(target, dp[mask] + centrosymmetryPairWeight(v[first], v[b]));
        }
    }
    return dp.back();
}
inline CentrosymmetryResult centrosymmetry(const Dataset &d, int neighborCount, int mode,
                                           const std::vector<uint8_t> *selection = nullptr,
                                           std::atomic<bool> *cancel = nullptr) {
    validateNeighborAnalysisInput(d);
    if (neighborCount < 2 || neighborCount > 64 || neighborCount % 2)
        throw std::runtime_error("Number of neighbors must be a positive, even integer");
    if (mode != 0 && mode != 1)
        throw std::runtime_error("Centrosymmetry mode must be conventional or minimum-weight matching");
    if (mode == 1 && neighborCount > 20)
        throw std::runtime_error("Minimum-weight matching supports at most 20 neighbors");
    if (selection && selection->size() != d.atoms.size())
        throw std::runtime_error("Selection length does not match particle count");
    CentrosymmetryResult result;
    result.values.assign(d.atoms.size(), 0.0);
    if (d.atoms.empty()) return result;
    const auto searchCell = neighborSearchCell(d);
    const auto inverse = cellInverse(searchCell);
    std::vector<std::array<double,3>> fractional(d.atoms.size());
    for (size_t i = 0; i < d.atoms.size(); ++i) {
        if (cancel && (i & 65535) == 0 && *cancel) throw std::runtime_error("Cancelled");
        fractional[i] = fractionalPosition(d, d.atoms[i], inverse);
    }
    struct NeighborEntry { uint32_t index; double distanceSquared; std::array<int32_t,3> image; };
    std::vector<std::vector<NeighborEntry>> adjacency(d.atoms.size());
    // Adaptive cutoff: grow until every analyzed center sees at least N distinct
    // minimum-image neighbors, but never past half the shortest periodic cell
    // translation, which is the exact-search limit of the shared kernel.
    const double volume = std::abs(cellDeterminant(d.cell));
    double cutoff = 1.0;
    if (volume > 0 && d.atoms.size() > 1) {
        const double density = double(d.atoms.size()) / volume;
        cutoff = 2.0 * std::cbrt(3.0 * double(neighborCount) /
                                 (4.0 * 3.14159265358979323846 * density));
    }
    if (!(cutoff > 0) || !std::isfinite(cutoff)) cutoff = 1.0;
    double maxCutoff = std::numeric_limits<double>::infinity();
    for (int axis = 0; axis < 3; ++axis)
        if (d.pbc[axis]) {
            const double length = std::sqrt(d.cell[axis*3] * d.cell[axis*3] +
                                            d.cell[axis*3+1] * d.cell[axis*3+1] +
                                            d.cell[axis*3+2] * d.cell[axis*3+2]);
            maxCutoff = std::min(maxCutoff, 0.5 * length);
        }
    bool found = false;
    for (int attempt = 0; attempt < 24 && !found; ++attempt) {
        if (cutoff > maxCutoff) cutoff = maxCutoff;
        for (auto &list : adjacency) list.clear();
        try {
            forEachNeighborPair(d, cutoff, [&](uint32_t i, uint32_t j, double distanceSquared,
                                               std::array<int32_t,3> image) {
                adjacency[i].push_back({j, distanceSquared, image});
                adjacency[j].push_back({i, distanceSquared,
                                        {int32_t(-image[0]), int32_t(-image[1]), int32_t(-image[2])}});
            }, cancel);
        } catch (const std::exception &e) {
            if (std::string(e.what()) == "Cancelled") throw;
            if (attempt == 0)
                throw std::runtime_error(
                    "The simulation cell is too small to resolve " + std::to_string(neighborCount) +
                    " nearest neighbors; reduce the neighbor count or enlarge the cell");
            break; // keep the previous pass for the under-coordination fallback
        }
        found = true;
        for (size_t i = 0; i < d.atoms.size() && found; ++i) {
            if (cancel && (i & 65535) == 0 && *cancel) throw std::runtime_error("Cancelled");
            if (selection && !(*selection)[i]) continue;
            if (adjacency[i].size() < size_t(neighborCount)) found = false;
        }
        if (!found) {
            if (cutoff >= maxCutoff) break;
            cutoff = std::min(cutoff * 1.3, maxCutoff);
        }
    }
    for (size_t i = 0; i < d.atoms.size(); ++i) {
        if (cancel && (i & 65535) == 0 && *cancel) throw std::runtime_error("Cancelled");
        if (selection && !(*selection)[i]) continue; // OVITO: unselected centers report 0
        auto &list = adjacency[i];
        // Guarantee N neighbors where possible; centers the grown cutoff still
        // under-coordinates fall back to the found neighbors when they form a
        // valid even pairing (>= 4), and to a CSP of 0 otherwise.
        size_t take = std::min(list.size(), size_t(neighborCount));
        if (list.size() < size_t(neighborCount)) {
            if (list.size() >= 4 && list.size() % 2 == 0)
                take = list.size();
            else {
                ++result.underCoordinated;
                continue;
            }
        }
        std::nth_element(list.begin(), list.begin() + (take - 1), list.end(),
                         [](const NeighborEntry &a, const NeighborEntry &b) {
                             return a.distanceSquared < b.distanceSquared;
                         });
        std::vector<std::array<double,3>> vectors;
        vectors.reserve(take);
        for (size_t k = 0; k < take; ++k) {
            const auto &entry = list[k];
            std::array<double,3> vector{};
            for (int axis = 0; axis < 3; ++axis) {
                const double f = fractional[entry.index][axis] - fractional[i][axis] -
                                 double(entry.image[axis]);
                for (int xyz = 0; xyz < 3; ++xyz) vector[xyz] += f * searchCell[axis * 3 + xyz];
            }
            vectors.push_back(vector);
        }
        const double csp = mode == 0 ? centrosymmetryGreedy(vectors)
                                     : centrosymmetryMatching(vectors);
        result.values[i] = csp;
        result.maxValue = std::max(result.maxValue, csp);
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
        return "Select particle by index";
    case Op::ManualSelection:
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
    case Op::CoordinationAnalysis: return "Coordination analysis";
    case Op::ClusterAnalysis: return "Cluster analysis";
    case Op::RadialDistribution: return "Radial distribution function (RDF)";
    case Op::Histogram: return "Histogram";
    case Op::ReduceProperty: return "Reduce property";
    case Op::AssignColor: return "Assign color";
    case Op::EditCell: return "Edit simulation cell";
    case Op::AffineTransform: return "Affine transformation";
    case Op::BondLengthDistribution: return "Bond length distribution";
    case Op::BondAngleDistribution: return "Bond angle distribution";
    case Op::ScatterPlot: return "Scatter plot";
    case Op::CentrosymmetryParameter: return "Centrosymmetry parameter";
    case Op::DisplacementVectors: return "Displacement vectors";
    default:
        return "Color by type";
    }
}
struct PipelineResult {
    Dataset data;
    std::vector<uint8_t> selected;
    std::vector<uint8_t> colorSelected;
};
inline bool pipelineResultWithinCacheBudget(const PipelineResult &result,
                                            size_t budgetBytes=64u*1024u*1024u) {
    size_t used=0;
    auto add=[&](size_t count,size_t elementSize) {
        if (elementSize && count>budgetBytes/elementSize) return false;
        const size_t bytes=count*elementSize;
        if (bytes>budgetBytes-used) return false;
        used+=bytes;
        return true;
    };
    const auto &data=result.data;
    if (!add(data.atoms.size(),sizeof(Atom)) || !add(result.selected.size(),sizeof(uint8_t)) ||
        !add(result.colorSelected.size(),sizeof(uint8_t)) || !add(data.bonds.size(),sizeof(Bond)) ||
        !add(data.particleColors.size(),sizeof(Vec3)) || !add(data.species.size(),sizeof(std::string)) ||
        !add(data.comment.size(),sizeof(char)) ||
        !add(data.propertyComponents.size(),sizeof(std::pair<std::string,std::string>)))
        return false;
    for (const auto &name:data.species) if (!add(name.size(),sizeof(char))) return false;
    for (const auto &[name,components]:data.propertyComponents)
        if (!add(name.size(),sizeof(char)) || !add(components.size(),sizeof(char))) return false;
    for (const auto &[name,values]:data.scalarProperties)
        if (!add(sizeof(name)+name.size(),1) || !add(values.size(),sizeof(double))) return false;
    for (const auto &[name,values]:data.vectorProperties)
        if (!add(sizeof(name)+name.size(),1) || !add(values.size(),sizeof(Vec3))) return false;
    if (!add(data.globalAttributes.size(),sizeof(std::pair<std::string,double>))) return false;
    for (const auto &entry:data.globalAttributes)
        if (!add(entry.first.size(),sizeof(char))) return false;
    for (const auto &table:data.tables) {
        if (!add(table.name.size(),sizeof(char)) || !add(table.columns.size(),sizeof(std::string))) return false;
        for (const auto &column:table.columns) if (!add(column.size(),sizeof(char))) return false;
        for (const auto &row:table.rows) {
            if (!add(row.size(),sizeof(std::string))) return false;
            for (const auto &field:row) if (!add(field.size(),sizeof(char))) return false;
        }
    }
    return true;
}
struct ModifierExecutionError : std::runtime_error {
    size_t nodeIndex;
    ModifierExecutionError(size_t node, const std::string &message)
        : std::runtime_error(message), nodeIndex(node) {}
};
// Resolves the reference frame requested by a Calculate displacements node:
// an absolute animation frame number, or the current frame plus an offset.
// Both are clamped into the valid frame range.
inline int resolveDisplacementReferenceFrame(const Modifier &m, int currentFrame,
                                             int frameCount) {
    if (frameCount <= 0)
        throw std::runtime_error("Displacement vectors requires a loaded trajectory for its reference configuration");
    const int requested = m.displacementRelative ? currentFrame + m.displacementOffset
                                                 : m.displacementFrame;
    return std::clamp(requested, 0, frameCount - 1);
}
// Calculate displacements core: matches particles between the reference and
// the current configuration, optionally maps positions into the reference or
// the current simulation cell, applies the minimum image convention on
// periodic axes and publishes the Displacement vector plus the Displacement
// Magnitude scalar particle properties.
inline void computeDisplacements(Dataset &data, const Dataset &reference,
                                 const Modifier &m, std::atomic<bool> *cancel = nullptr) {
    const size_t count = data.atoms.size();
    const size_t referenceCount = reference.atoms.size();
    // Per-current-particle index of the matching reference particle. Equal
    // particle counts pair by atom index; otherwise a Particle Identifier
    // property on both sides establishes the correspondence, and without one
    // the OVITO count-mismatch error is raised.
    std::vector<uint32_t> mapping(count);
    if (referenceCount == count) {
        for (size_t i = 0; i < count; ++i) mapping[i] = uint32_t(i);
    } else {
        const auto referenceIds = reference.scalarProperties.find("Particle Identifier");
        const auto currentIds = data.scalarProperties.find("Particle Identifier");
        if (referenceIds == reference.scalarProperties.end() ||
            currentIds == data.scalarProperties.end() ||
            referenceIds->second.size() != referenceCount ||
            currentIds->second.size() != count)
            throw std::runtime_error(
                "Particle counts of the reference and current configuration do not match.");
        std::unordered_map<double, uint32_t> byIdentifier;
        byIdentifier.reserve(referenceCount * 2);
        for (size_t i = 0; i < referenceCount; ++i) {
            if ((i & 65535) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
            byIdentifier.emplace(referenceIds->second[i], uint32_t(i));
        }
        for (size_t i = 0; i < count; ++i) {
            if ((i & 65535) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
            const auto found = byIdentifier.find(currentIds->second[i]);
            if (found == byIdentifier.end())
                throw std::runtime_error(
                    "Particle counts of the reference and current configuration do not match.");
            mapping[i] = found->second;
        }
    }
    // Cell mapping option: wrap current positions into the reference cell, or
    // reference positions into the current cell, before differencing.
    Dataset referenceCellSource;
    referenceCellSource.cell = reference.cell;
    referenceCellSource.origin = reference.origin;
    referenceCellSource.pbc = reference.pbc;
    const bool anyPeriodic =
        std::any_of(data.pbc.begin(), data.pbc.end(), [](bool periodic) { return periodic; });
    std::array<std::array<double, 3>, 3> cellInverseCurrent{};
    if (m.displacementMinimumImage && anyPeriodic && count)
        cellInverseCurrent = cellInverse(data.cell);
    auto &displacement = data.vectorProperties["Displacement"];
    auto &magnitude = data.scalarProperties["Displacement Magnitude"];
    displacement.resize(count);
    magnitude.resize(count);
    data.propertyComponents["Displacement"] = "XYZ";
    for (size_t i = 0; i < count; ++i) {
        if ((i & 65535) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
        const Atom &referenceAtom = reference.atoms[mapping[i]];
        double current3[3]{data.atoms[i].x, data.atoms[i].y, data.atoms[i].z};
        double reference3[3]{referenceAtom.x, referenceAtom.y, referenceAtom.z};
        if (m.displacementCellMapping == 1) {
            const Atom wrapped =
                wrapAtomInCell(referenceCellSource, data.atoms[i]);
            current3[0] = wrapped.x; current3[1] = wrapped.y; current3[2] = wrapped.z;
        } else if (m.displacementCellMapping == 2) {
            const Atom wrapped = wrapAtomInCell(data, referenceAtom);
            reference3[0] = wrapped.x; reference3[1] = wrapped.y; reference3[2] = wrapped.z;
        }
        double delta[3]{current3[0] - reference3[0],
                        current3[1] - reference3[1],
                        current3[2] - reference3[2]};
        if (m.displacementMinimumImage && anyPeriodic) {
            // Fractional delta against the current cell; only periodic axes
            // are rounded to the nearest periodic image.
            double fractional[3];
            for (int axis = 0; axis < 3; ++axis)
                fractional[axis] = cellInverseCurrent[axis][0] * delta[0] +
                                   cellInverseCurrent[axis][1] * delta[1] +
                                   cellInverseCurrent[axis][2] * delta[2];
            for (int axis = 0; axis < 3; ++axis) {
                if (!data.pbc[axis]) continue;
                const double image = std::round(fractional[axis]);
                for (int component = 0; component < 3; ++component)
                    delta[component] -= image * data.cell[axis * 3 + component];
            }
        }
        const double magnitudeValue = std::hypot(delta[0], delta[1], delta[2]);
        if (!std::isfinite(magnitudeValue))
            throw std::runtime_error("Displacement computation produced a non-finite value");
        displacement[i] = {float(delta[0]), float(delta[1]), float(delta[2])};
        magnitude[i] = magnitudeValue;
    }
}
inline PipelineResult evaluateFrom(PipelineResult r,const std::vector<Modifier> &mods,
                                   size_t firstNode=0,std::atomic<bool> *cancel=nullptr,
                                   std::atomic<size_t> *activeNode=nullptr,
                                   size_t checkpointNode=SIZE_MAX,
                                   const std::function<void(size_t,const PipelineResult&)> &checkpoint={},
                                   const std::function<const Dataset &(size_t)> &referenceProvider={}) {
    if (firstNode>mods.size())
        throw std::runtime_error("Pipeline checkpoint starts after the final node");
    for (size_t modifierIndex = firstNode; modifierIndex < mods.size(); ++modifierIndex) {
        if (activeNode) *activeNode = modifierIndex + 1;
        if (modifierIndex==checkpointNode && checkpoint)
            checkpoint(modifierIndex,r);
        const auto &m = mods[modifierIndex];
        if (m.enabled) {
          try {
            if (cancel && *cancel) throw std::runtime_error("Cancelled");
            if (m.axis < 0 || m.axis > 2 || !std::isfinite(m.value) || !std::isfinite(m.upper))
                throw std::runtime_error("Invalid modifier parameters");
            if (m.op == Op::Scale && m.value <= 0)
                throw std::runtime_error("Scale must be positive");
            if (m.op == Op::AffineTransform) {
                for (double value : m.affineTransform)
                    if (!std::isfinite(value))
                        throw std::runtime_error("Affine transformation entries must be finite");
                const std::array<double, 9> linear{
                    m.affineTransform[0], m.affineTransform[1], m.affineTransform[2],
                    m.affineTransform[4], m.affineTransform[5], m.affineTransform[6],
                    m.affineTransform[8], m.affineTransform[9], m.affineTransform[10]};
                double scale = 0;
                for (int row = 0; row < 3; ++row) {
                    double lengthSquared = 0;
                    for (int column = 0; column < 3; ++column)
                        lengthSquared += linear[row * 3 + column] * linear[row * 3 + column];
                    scale = std::max(scale, std::sqrt(lengthSquared));
                }
                const double determinant = cellDeterminant(linear);
                if (!(scale > 0) || !std::isfinite(determinant) ||
                    std::abs(determinant) <= scale * scale * scale * 1e-12)
                    throw std::runtime_error("Affine transformation linear part must be non-degenerate");
            }
            if (m.op == Op::EditCell) {
                double scale = 0;
                for (int axis = 0; axis < 3; ++axis) {
                    double lengthSquared = 0;
                    for (int component = 0; component < 3; ++component) {
                        const double value = m.editedCell[axis * 3 + component];
                        if (!std::isfinite(value))
                            throw std::runtime_error("Cell vectors must contain finite values");
                        lengthSquared += value * value;
                    }
                    scale = std::max(scale, std::sqrt(lengthSquared));
                }
                if (!std::isfinite(m.editedOrigin.x) || !std::isfinite(m.editedOrigin.y) ||
                    !std::isfinite(m.editedOrigin.z))
                    throw std::runtime_error("Cell origin must contain finite values");
                const double determinant = cellDeterminant(m.editedCell);
                if (!(scale > 0) || !std::isfinite(determinant) ||
                    std::abs(determinant) <= scale * scale * scale * 1e-12)
                    throw std::runtime_error("Simulation cell vectors must form a non-degenerate cell");
            }
            // Slice plane parameters, with the normal pre-normalized once for the
            // per-particle loop below.
            double sliceNormalUnit[3] = {};
            if (m.op == Op::Slice) {
                const double nx = m.sliceNormal[0], ny = m.sliceNormal[1], nz = m.sliceNormal[2];
                if (!std::isfinite(m.sliceDistance) || !std::isfinite(m.sliceWidth) ||
                    !std::isfinite(nx) || !std::isfinite(ny) || !std::isfinite(nz))
                    throw std::runtime_error("Slice distance, width and normal must be finite");
                if (m.sliceWidth < 0)
                    throw std::runtime_error("Slice width must not be negative");
                const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (!(length > 0))
                    throw std::runtime_error("Slice normal vector must be non-zero");
                sliceNormalUnit[0] = nx / length;
                sliceNormalUnit[1] = ny / length;
                sliceNormalUnit[2] = nz / length;
            }
            // Shared strict slice-plane predicate: half-space keep is h < d
            // (reverse: h > d); slab keep is the closed |h - d| <= w/2 (reverse:
            // strictly outside). No epsilon: float particle storage against the
            // double plane distance reproduces OVITO's boundary-layer counts.
            const auto slicePasses = [&](const Atom &particle) {
                const double h = sliceNormalUnit[0] * particle.x +
                                 sliceNormalUnit[1] * particle.y +
                                 sliceNormalUnit[2] * particle.z;
                if (m.sliceWidth > 0) {
                    const double offset = std::abs(h - m.sliceDistance);
                    return m.sliceInvert ? offset > m.sliceWidth * .5
                                         : offset <= m.sliceWidth * .5;
                }
                return m.sliceInvert ? h > m.sliceDistance : h < m.sliceDistance;
            };
            if (m.op == Op::Slice && m.sliceOperateOnParticles && m.sliceCreateSelection) {
                size_t kept = 0;
                for (size_t i = 0; i < r.data.atoms.size(); ++i) {
                    if (cancel && (i & 65535) == 0 && *cancel)
                        throw std::runtime_error("Cancelled");
                    const bool passes =
                        (!m.sliceApplySelectionOnly || r.selected[i]) &&
                        slicePasses(r.data.atoms[i]);
                    r.selected[i] = uint8_t(passes);
                    kept += passes;
                }
                r.data.globalAttributes["Slice.input_particles"] = double(r.data.atoms.size());
                r.data.globalAttributes["Slice.particles_deleted"] = 0;
                r.data.globalAttributes["Slice.particles_remaining"] = double(r.data.atoms.size());
                r.data.globalAttributes["Slice.particles_selected"] = double(kept);
                continue;
            }
            if (m.op == Op::EditType && (m.type < 0 || size_t(m.type) >= r.data.species.size()))
                throw std::runtime_error("Particle type is out of range");
            if (m.op == Op::SelectRange && m.upper < m.value)
                throw std::runtime_error("Upper bound must be at least the lower bound");
            if ((m.op == Op::CommonNeighborAnalysis || (m.op == Op::CreateBonds && !m.bondTypeCutoffsEnabled) ||
                 m.op == Op::CoordinationAnalysis || (m.op == Op::ClusterAnalysis && !m.clusterByBonds) ||
                m.op == Op::RadialDistribution) &&
                (!(m.value > 0) || !std::isfinite(m.value)))
                throw std::runtime_error("Cutoff must be finite and positive");
            if (m.op==Op::RadialDistribution && (m.rdfBins<1 || m.rdfBins>4096))
                throw std::runtime_error("RDF bins must be between 1 and 4096");
            if (m.op == Op::CreateBonds && m.bondTypeCutoffsEnabled) {
                const size_t typeCount=r.data.species.size();
                if (typeCount==0 || typeCount>32 || m.bondTypeCutoffs.size()!=typeCount*typeCount)
                    throw std::runtime_error("Type-pair bond cutoffs do not match the particle type table");
                for (float cutoff : m.bondTypeCutoffs)
                    if (!std::isfinite(cutoff) || cutoff<0 || cutoff>100000.f)
                        throw std::runtime_error("Type-pair bond cutoffs must be finite values between 0 and 100000");
                for (size_t a=0;a<typeCount;++a)
                    for (size_t b=a+1;b<typeCount;++b)
                        if (m.bondTypeCutoffs[a*typeCount+b]!=m.bondTypeCutoffs[b*typeCount+a])
                            throw std::runtime_error("Type-pair bond cutoff matrix must be symmetric");
            }
            if (m.op == Op::ExpandSelection) {
                if (m.expandMode < 0 || m.expandMode > 3)
                    throw std::runtime_error("Invalid expansion mode");
                if (m.expandMode == 0 && (!(m.value > 0) || !std::isfinite(m.value)))
                    throw std::runtime_error("Cutoff must be finite and positive");
                if (m.expandMode == 1 && (m.expandNeighbors < 1 || m.expandNeighbors > 100000))
                    throw std::runtime_error("Neighbor count must be a positive value");
                if (m.expandMode >= 2 && r.data.bonds.empty())
                    throw std::runtime_error("Expand selection requires an existing bond topology");
            }
            if (m.op == Op::SelectOverlapping) {
                if (!m.overlapUseRadii && (!(m.value > 0) || !std::isfinite(m.value)))
                    throw std::runtime_error("Pair cutoff must be finite and positive");
                if (m.overlapUseRadii) {
                    auto radius = r.data.scalarProperties.find(m.property);
                    if (m.property.empty() || radius == r.data.scalarProperties.end() ||
                        radius->second.size() != r.data.atoms.size())
                        throw std::runtime_error("Choose a scalar particle-radius property with one value per particle");
                    for (double value : radius->second)
                        if (!std::isfinite(value) || value < 0 || value > 100000)
                            throw std::runtime_error("Particle radii must be finite and between 0 and 100000");
                }
            }
            if (m.op == Op::ExpandSelection && (m.type < 1 || m.type > 64))
                throw std::runtime_error("Expansion steps must be between 1 and 64");
            if (m.op == Op::ColorCoding) {
                if (m.property.empty())
                    throw std::runtime_error("Color coding requires a particle property");
                if (m.colorGradient < 0 || m.colorGradient > 9)
                    throw std::runtime_error("Choose a supported color gradient");
                if (!m.colorAutoRange &&
                    (!std::isfinite(m.colorMin) || !std::isfinite(m.colorMax) ||
                     !(m.colorMax > m.colorMin)))
                    throw std::runtime_error("Manual color range must have finite, increasing endpoints");
            }
            if (m.op == Op::AssignColor && !std::all_of(m.assignColor.begin(),m.assignColor.end(),
                    [](float value){return std::isfinite(value)&&value>=0&&value<=1;}))
                throw std::runtime_error("Assigned color channels must be finite values between 0 and 1");
            if (m.op == Op::ExpressionSelect && m.property.empty())
                throw std::runtime_error("Expression selection requires an expression");
            if (m.op == Op::ComputeProperty && (m.property.empty() || m.outputProperty.empty()))
                throw std::runtime_error("Computed property requires an expression and output name");
            if (m.op == Op::Histogram && (m.type < 1 || m.type > 4096))
                throw std::runtime_error("Histogram bins must be between 1 and 4096");
            if (m.op == Op::Histogram && (m.histogramNormalization < 0 || m.histogramNormalization > 2))
                throw std::runtime_error("Choose a supported histogram normalization mode");
            if (m.op == Op::Histogram && m.histogramSelectRange &&
                (!std::isfinite(m.histogramRangeStart) || !std::isfinite(m.histogramRangeEnd) ||
                 m.histogramRangeEnd < m.histogramRangeStart))
                throw std::runtime_error("Histogram selection range must be finite and non-decreasing");
            if (m.op == Op::BondLengthDistribution && (m.type < 1 || m.type > 4096))
                throw std::runtime_error("Histogram bins must be between 1 and 4096");
            if (m.op == Op::BondAngleDistribution && (m.type < 1 || m.type > 4096))
                throw std::runtime_error("Histogram bins must be between 1 and 4096");
            if ((m.op == Op::BondLengthDistribution || m.op == Op::BondAngleDistribution) &&
                (m.histogramNormalization < 0 || m.histogramNormalization > 2))
                throw std::runtime_error("Choose a supported bond-distribution normalization mode");
            if (m.op == Op::ReduceProperty && (m.property.empty() || m.reduceOperation < 0 || m.reduceOperation > 3))
                throw std::runtime_error("Choose a property and a valid reduction operation");
            if (m.op==Op::ScatterPlot && (m.scatterXProperty.empty() || m.scatterYProperty.empty()))
                throw std::runtime_error("Choose both scatter-plot properties");
            if (m.op == Op::Histogram || m.op == Op::ReduceProperty)
                (void)numericParticlePropertyView(r.data,m.property);
            if (m.op == Op::RemoveProperty) {
                if (m.property.empty() || m.property == "Position" || m.property == "Particle Type")
                    throw std::runtime_error("Choose an auxiliary particle property to remove");
                auto removed = r.data.scalarProperties.erase(m.property);
                removed += r.data.vectorProperties.erase(m.property);
                if (!removed) throw std::runtime_error("Unknown particle property: " + m.property);
                r.data.propertyComponents.erase(m.property);
                continue;
            }
            if (m.op == Op::AffineTransform) {
                // Point data (positions, cell origin) receives the translation
                // column; vector data and cell vectors only the linear part.
                std::array<double, 3> pointTranslation{
                    m.affineTransform[3], m.affineTransform[7], m.affineTransform[11]};
                if (m.affineReducedCoords) {
                    // x' = M*(x + H*t): the fractional shift H*t is resolved
                    // against the input cell, then premultiplied by M.
                    const double t[3]{m.affineTransform[3], m.affineTransform[7],
                                      m.affineTransform[11]};
                    std::array<double, 3> cellShift{};
                    for (int axis = 0; axis < 3; ++axis)
                        for (int component = 0; component < 3; ++component)
                            cellShift[component] += t[axis] * r.data.cell[axis * 3 + component];
                    for (int row = 0; row < 3; ++row)
                        pointTranslation[row] = m.affineTransform[row * 4] * cellShift[0] +
                                                m.affineTransform[row * 4 + 1] * cellShift[1] +
                                                m.affineTransform[row * 4 + 2] * cellShift[2];
                }
                auto transformPoint = [&](double x, double y, double z, bool point) {
                    std::array<double, 3> result{};
                    for (int row = 0; row < 3; ++row) {
                        result[row] = m.affineTransform[row * 4] * x +
                                      m.affineTransform[row * 4 + 1] * y +
                                      m.affineTransform[row * 4 + 2] * z;
                        if (point) result[row] += pointTranslation[row];
                        if (!std::isfinite(result[row]))
                            throw std::runtime_error("Affine transformation produced a non-finite coordinate");
                    }
                    return result;
                };
                for (size_t index = 0; index < r.data.atoms.size(); ++index) {
                    if (cancel && (index & 65535) == 0 && *cancel)
                        throw std::runtime_error("Cancelled");
                    if (m.affineOnlySelected && !r.selected[index]) continue;
                    auto &atom = r.data.atoms[index];
                    const auto transformed = transformPoint(atom.x, atom.y, atom.z, true);
                    atom.x = float(transformed[0]); atom.y = float(transformed[1]);
                    atom.z = float(transformed[2]);
                    if (!std::isfinite(atom.x) || !std::isfinite(atom.y) || !std::isfinite(atom.z))
                        throw std::runtime_error("Affine transformation exceeds particle coordinate range");
                }
                if (m.transformVectorProperties) {
                    for (auto &[name, values] : r.data.vectorProperties) {
                        if (values.size() != r.data.atoms.size())
                            throw std::runtime_error(
                                "Vector property length does not match particle count: " + name);
                        for (auto &value : values) {
                            const auto transformed = transformPoint(value.x, value.y, value.z, false);
                            for (double component : transformed)
                                if (std::abs(component) > std::numeric_limits<float>::max())
                                    throw std::runtime_error(
                                        "Affine transformation exceeds vector property range: " + name);
                            value = {float(transformed[0]), float(transformed[1]),
                                     float(transformed[2])};
                        }
                    }
                }
                for (int vector = 0; vector < 3; ++vector) {
                    const int offset = vector * 3;
                    const auto transformed = transformPoint(r.data.cell[offset],
                        r.data.cell[offset + 1], r.data.cell[offset + 2], false);
                    for (int component = 0; component < 3; ++component)
                        r.data.cell[offset + component] = transformed[component];
                }
                const auto transformedOrigin = transformPoint(r.data.origin.x, r.data.origin.y,
                                                               r.data.origin.z, true);
                r.data.origin = {float(transformedOrigin[0]), float(transformedOrigin[1]),
                                 float(transformedOrigin[2])};
                if (!std::isfinite(r.data.origin.x) || !std::isfinite(r.data.origin.y) ||
                    !std::isfinite(r.data.origin.z))
                    throw std::runtime_error("Affine transformation exceeds cell origin range");
                continue;
            }
            if (m.op == Op::Replicate) {
                const int copies[3]{m.replicateN[0], m.replicateN[1], m.replicateN[2]};
                for (int count : copies)
                    if (count < 1 || count > 32)
                        throw std::runtime_error("Replication copies must be between 1 and 32 along each cell vector");
                const size_t total = size_t(copies[0]) * size_t(copies[1]) * size_t(copies[2]);
                if (r.data.atoms.size() > 20000000 / total)
                    throw std::runtime_error("Replication exceeds the 20 million atom budget");
                for (int axis = 0; axis < 3; ++axis)
                    if (copies[axis] > 1 && r.data.cell[axis * 3] == 0 &&
                        r.data.cell[axis * 3 + 1] == 0 && r.data.cell[axis * 3 + 2] == 0)
                        throw std::runtime_error("Replication requires a nonzero simulation cell vector");
                const size_t n = r.data.atoms.size();
                const auto originalBonds = r.data.bonds;
                // Image (i,j,k) lives at particle slot i + j*Na + k*Na*Nb so the
                // copy order runs along a, then b, then c.
                const size_t copyStride[3]{1, size_t(copies[0]), size_t(copies[0]) * size_t(copies[1])};
                r.data.atoms.reserve(n * total); r.selected.reserve(n * total);
                r.colorSelected.reserve(n * total);
                for (int cz = 0; cz < copies[2]; ++cz)
                    for (int cy = 0; cy < copies[1]; ++cy)
                        for (int cx = 0; cx < copies[0]; ++cx) {
                            if (!cx && !cy && !cz) continue;
                            const double shift[3]{
                                cx * r.data.cell[0] + cy * r.data.cell[3] + cz * r.data.cell[6],
                                cx * r.data.cell[1] + cy * r.data.cell[4] + cz * r.data.cell[7],
                                cx * r.data.cell[2] + cy * r.data.cell[5] + cz * r.data.cell[8]};
                            for (size_t i = 0; i < n; ++i) {
                                auto a = r.data.atoms[i];
                                a.x += float(shift[0]);
                                a.y += float(shift[1]);
                                a.z += float(shift[2]);
                                r.data.atoms.push_back(a); r.selected.push_back(r.selected[i]);
                                r.colorSelected.push_back(r.colorSelected[i]);
                            }
                        }
                if (m.replicateAdjustBox)
                    for (int axis = 0; axis < 3; ++axis)
                        for (int component = 0; component < 3; ++component)
                            r.data.cell[axis * 3 + component] *= copies[axis];
                for (auto &[name, values] : r.data.scalarProperties) {
                    auto original = values;
                    for (size_t copy = 1; copy < total; ++copy)
                        values.insert(values.end(), original.begin(), original.end());
                }
                for (auto &[name, values] : r.data.vectorProperties) {
                    auto original = values;
                    for (size_t copy = 1; copy < total; ++copy)
                        values.insert(values.end(), original.begin(), original.end());
                }
                if (!r.data.particleColors.empty()) {
                    auto original=r.data.particleColors;
                    for (size_t copy=1;copy<total;++copy)
                        r.data.particleColors.insert(r.data.particleColors.end(),original.begin(),original.end());
                }
                r.data.bonds.clear();
                r.data.bonds.reserve(originalBonds.size() * total);
                for (int cz = 0; cz < copies[2]; ++cz)
                    for (int cy = 0; cy < copies[1]; ++cy)
                        for (int cx = 0; cx < copies[0]; ++cx) {
                            const int image[3]{cx, cy, cz};
                            for (auto bond : originalBonds) {
                                if (bond.a >= n || bond.b >= n)
                                    throw std::runtime_error("Cannot replicate invalid bond topology");
                                uint32_t target = bond.b;
                                for (int axis = 0; axis < 3; ++axis) {
                                    const int64_t unwrapped = int64_t(image[axis]) + bond.image[axis];
                                    const int64_t targetCopy =
                                        (unwrapped % copies[axis] + copies[axis]) % copies[axis];
                                    bond.image[axis] = int32_t((unwrapped - targetCopy) / copies[axis]);
                                    target += uint32_t(targetCopy * copyStride[axis] * n);
                                }
                                bond.b = target;
                                bond.a += uint32_t((size_t(cx) * copyStride[0] + size_t(cy) * copyStride[1] +
                                                   size_t(cz) * copyStride[2]) * n);
                                r.data.bonds.push_back(bond);
                            }
                        }
                continue;
            }
            if (m.op == Op::Wrap) {
                for (size_t i = 0; i < r.data.atoms.size(); ++i) {
                    if (cancel && (i & 65535) == 0 && *cancel)
                        throw std::runtime_error("Cancelled");
                    r.data.atoms[i] = wrapAtomInCell(r.data, r.data.atoms[i]);
                }
                continue;
            }
            if (m.op == Op::ColorCoding) {
                r.data.particleColors.clear();
                auto values = particlePropertyValues(r.data, m.property, cancel);
                bool hasFiniteValue = false;
                for (double value : values) {
                    if (!std::isfinite(value)) continue;
                    hasFiniteValue = true;
                    if (std::abs(value) > std::numeric_limits<float>::max())
                        throw std::runtime_error("Color coding values must fit the renderer's finite single-precision range");
                }
                if (!hasFiniteValue)
                    throw std::runtime_error("Color coding property has no finite values");
                r.data.scalarProperties["Color coding"] = std::move(values);
                if (m.colorSelectedOnly) {
                    r.colorSelected = r.selected;
                    if (!m.colorKeepSelection)
                        std::fill(r.selected.begin(), r.selected.end(), uint8_t(0));
                } else {
                    r.colorSelected.assign(r.data.atoms.size(), uint8_t(1));
                }
                continue;
            }
            if (m.op == Op::AssignColor) {
                if (r.data.particleColors.empty())
                    r.data.particleColors.assign(r.data.atoms.size(),Vec3{-1,-1,-1});
                if (r.data.particleColors.size()!=r.data.atoms.size())
                    throw std::runtime_error("Particle color property length mismatch");
                const bool hasSelection=std::any_of(r.selected.begin(),r.selected.end(),[](uint8_t value){return value!=0;});
                for (size_t i=0;i<r.data.atoms.size();++i) {
                    if (cancel && (i&65535)==0 && *cancel) throw std::runtime_error("Cancelled");
                    if (!hasSelection || r.selected[i])
                        r.data.particleColors[i]={m.assignColor[0],m.assignColor[1],m.assignColor[2]};
                }
                continue;
            }
            if (m.op == Op::ColorType) {
                r.data.particleColors.clear();
                r.data.scalarProperties.erase("Color coding");
                continue;
            }
            if (m.op == Op::ExpandSelection) {
                if (r.selected.size() != r.data.atoms.size())
                    throw std::runtime_error("Selection length does not match the particle count");
                if (std::find(r.selected.begin(), r.selected.end(), uint8_t(1)) == r.selected.end())
                    throw std::runtime_error("This operation requires an input particles selection.");
                for (int step = 0; step < m.type; ++step) {
                    auto expanded = r.selected;
                    switch (m.expandMode) {
                    case 0:
                        forEachNeighborPair(r.data, m.value, [&](uint32_t i, uint32_t j, double) {
                            if (r.selected[i]) expanded[j] = 1;
                            if (r.selected[j]) expanded[i] = 1;
                        }, cancel);
                        break;
                    case 1:
                        expandSelectionNearestNeighbors(r.data, m.expandNeighbors, r.selected,
                                                        expanded, cancel);
                        break;
                    case 2:
                        for (const auto &bond : r.data.bonds) {
                            if (r.selected[bond.a]) expanded[bond.b] = 1;
                            if (r.selected[bond.b]) expanded[bond.a] = 1;
                        }
                        break;
                    default: {
                        const auto clusters = clustersFromBonds(r.data, cancel);
                        std::unordered_set<uint32_t> molecules;
                        for (size_t i = 0; i < r.selected.size(); ++i)
                            if (r.selected[i]) molecules.insert(clusters.cluster[i]);
                        for (size_t i = 0; i < expanded.size(); ++i)
                            if (molecules.count(clusters.cluster[i])) expanded[i] = 1;
                        break;
                    }
                    }
                    r.selected = std::move(expanded);
                }
                continue;
            }
            if (m.op == Op::SelectOverlapping) {
                r.selected.assign(r.data.atoms.size(), 0);
                if (m.overlapUseRadii) {
                    const auto &radii = r.data.scalarProperties.at(m.property);
                    const double maxRadius = radii.empty() ? 0 : *std::max_element(radii.begin(), radii.end());
                    if (maxRadius > 0) {
                        const double searchCutoff = 2 * maxRadius;
                        if (!std::isfinite(searchCutoff))
                            throw std::runtime_error("Particle-radius search cutoff is not finite");
                        forEachNeighborPair(r.data, searchCutoff,
                            [&](uint32_t i, uint32_t j, double distanceSquared) {
                                const double pairRadius = radii[i] + radii[j];
                                if (distanceSquared < pairRadius * pairRadius)
                                    r.selected[i] = r.selected[j] = 1;
                            }, cancel);
                    }
                } else {
                    forEachNeighborPair(r.data, m.value, [&](uint32_t i, uint32_t j, double) {
                        r.selected[i] = r.selected[j] = 1;
                    }, cancel);
                }
                continue;
            }
            if (m.op == Op::ExpressionSelect) {
                ParticleExpression(r.data, 0, m.property).validate();
                r.selected.resize(r.data.atoms.size());
                for (size_t i=0; i<r.data.atoms.size(); ++i) {
                    if (cancel && (i & 4095) == 0 && *cancel) throw std::runtime_error("Cancelled");
                    r.selected[i] = ParticleExpression(r.data, i, m.property).evaluate() ? 1 : 0;
                }
                continue;
            }
            if (m.op == Op::ManualSelection) {
                r.selected.assign(r.data.atoms.size(), 0);
                for (uint32_t index : m.manualSelection) {
                    if (index >= r.selected.size())
                        throw std::runtime_error("Manual selection contains an index outside the current particle data");
                    r.selected[index] = 1;
                }
                continue;
            }
            if (m.op == Op::ComputeProperty) {
                ParticleExpression(r.data, 0, m.property).validate();
                std::vector<double> values(r.data.atoms.size());
                for (size_t i=0; i<r.data.atoms.size(); ++i) {
                    if (cancel && (i & 4095) == 0 && *cancel) throw std::runtime_error("Cancelled");
                    values[i] = ParticleExpression(r.data, i, m.property).evaluateNumber();
                }
                r.data.scalarProperties[m.outputProperty] = std::move(values);
                continue;
            }
            if (m.op == Op::EditCell) {
                if (m.transformCoordinatesWithCell) {
                    const auto oldInverse = cellInverse(r.data.cell);
                    for (auto &atom : r.data.atoms) {
                        const auto fractional = fractionalPosition(r.data, atom, oldInverse);
                        atom.x = float(m.editedOrigin.x + fractional[0] * m.editedCell[0] +
                                       fractional[1] * m.editedCell[3] + fractional[2] * m.editedCell[6]);
                        atom.y = float(m.editedOrigin.y + fractional[0] * m.editedCell[1] +
                                       fractional[1] * m.editedCell[4] + fractional[2] * m.editedCell[7]);
                        atom.z = float(m.editedOrigin.z + fractional[0] * m.editedCell[2] +
                                       fractional[1] * m.editedCell[5] + fractional[2] * m.editedCell[8]);
                    }
                }
                r.data.cell = m.editedCell;
                r.data.origin = m.editedOrigin;
                r.data.pbc = m.editedPbc;
                continue;
            }
            if (m.op == Op::CentrosymmetryParameter) {
                auto csp = centrosymmetry(r.data, m.cspNeighbors, m.cspMode,
                                          m.cspOnlySelected ? &r.selected : nullptr, cancel);
                r.data.scalarProperties["Centrosymmetry"] = std::move(csp.values);
                r.data.globalAttributes["Centrosymmetry.max"] = csp.maxValue;
                r.data.globalAttributes["Centrosymmetry.neighbor_count"] = double(m.cspNeighbors);
                r.data.globalAttributes["Centrosymmetry.undercoordinated_particles"] =
                    double(csp.underCoordinated);
                continue;
            }
            if (m.op == Op::DisplacementVectors) {
                if (!referenceProvider)
                    throw std::runtime_error(
                        "Displacement vectors has no reference configuration provider");
                const Dataset &reference = referenceProvider(modifierIndex);
                computeDisplacements(r.data, reference, m, cancel);
                continue;
            }
            if (m.op == Op::CoordinationAnalysis || m.op == Op::ClusterAnalysis ||
                m.op == Op::RadialDistribution) {
                if (r.data.sampled())
                    throw std::runtime_error("Neighbor analysis requires complete, unsampled particle data");
                const auto *clusterSelection=m.clusterOnlySelected ? &r.selected : nullptr;
                const auto *analysisSelection = m.neighborOnlySelected ? &r.selected : nullptr;
                auto analysis = m.op==Op::ClusterAnalysis && m.clusterByBonds
                    ? clustersFromBonds(r.data,cancel,clusterSelection)
                    : neighbors(r.data,m.value,cancel,
                                m.op==Op::ClusterAnalysis ? clusterSelection : nullptr,
                                m.op==Op::RadialDistribution ? size_t(m.rdfBins) : 128,
                                analysisSelection);
                if (m.op == Op::CoordinationAnalysis) {
                    std::vector<double> values(analysis.coordination.begin(), analysis.coordination.end());
                    r.data.scalarProperties["Coordination"] = std::move(values);
                    r.data.globalAttributes["CoordinationAnalysis.mean"] = analysis.meanCoordination;
                    r.data.globalAttributes["CoordinationAnalysis.neighbor_pairs"] = double(analysis.bonds);
                    std::map<uint32_t,uint64_t> counts;
                    size_t population=0;
                    for (size_t i=0;i<analysis.coordination.size();++i) {
                        if ((i&65535)==0 && cancel && *cancel) throw std::runtime_error("Cancelled");
                        if (analysisSelection && !(*analysisSelection)[i]) continue;
                        ++counts[analysis.coordination[i]];
                        ++population;
                    }
                    DataTable table; table.name="Coordination number distribution";
                    table.columns={"Coordination number","Particle count","Fraction"};
                    const double total=double(population);
                    for (const auto &[number,count]:counts)
                        table.rows.push_back({std::to_string(number),std::to_string(count),
                            total>0 ? formatDataNumber(double(count)/total) : "0"});
                    r.data.globalAttributes["CoordinationAnalysis.distinct_numbers"]=double(counts.size());
                    r.data.tables.push_back(std::move(table));
                } else if (m.op == Op::ClusterAnalysis) {
                    std::vector<uint64_t> counts(size_t(analysis.clusters)+1);
                    for (auto id:analysis.cluster) if (id && id<counts.size()) ++counts[id];
                    if (m.clusterSortBySize && analysis.clusters>1) {
                        std::vector<uint32_t> order(analysis.clusters);
                        for (uint32_t i=0;i<order.size();++i) order[i]=i+1;
                        std::stable_sort(order.begin(),order.end(),[&](uint32_t a,uint32_t b) {
                            return counts[a]>counts[b];
                        });
                        std::vector<uint32_t> remap(size_t(analysis.clusters)+1);
                        for (uint32_t i=0;i<order.size();++i) remap[order[i]]=i+1;
                        for (auto &id:analysis.cluster) if (id) id=remap[id];
                        std::vector<uint64_t> sortedCounts(counts.size());
                        for (uint32_t i=0;i<order.size();++i) sortedCounts[i+1]=counts[order[i]];
                        counts=std::move(sortedCounts);
                    }
                    std::vector<double> values(analysis.cluster.begin(), analysis.cluster.end());
                    r.data.scalarProperties["Cluster"] = std::move(values);
                    r.data.globalAttributes["ClusterAnalysis.count"] = analysis.clusters;
                    r.data.globalAttributes["ClusterAnalysis.largest_size"] =
                        counts.empty() ? 0.0 : double(*std::max_element(counts.begin(),counts.end()));
                    DataTable table; table.name = "Cluster analysis";
                    table.columns = {"Cluster", "Particle count"};
                    for (uint32_t id = 1; id < counts.size(); ++id)
                        table.rows.push_back({std::to_string(id), std::to_string(counts[id])});
                    r.data.tables.push_back(std::move(table));
                } else {
                    if (!analysis.rdfValid)
                        throw std::runtime_error("RDF requires particles in a valid fully periodic 3D simulation cell");
                    DataTable table; table.name = "Radial distribution function";
                    table.columns = {"r", "Pair count", "g(r)"};
                    for (size_t bin = 0; bin < analysis.rdf.size(); ++bin) {
                        const double radius = (bin + .5) * m.value / analysis.rdf.size();
                        table.rows.push_back({std::to_string(radius),
                            std::to_string(analysis.pairHistogram[bin]), std::to_string(analysis.rdf[bin])});
                    }
                    r.data.globalAttributes["RadialDistribution.cutoff"] = m.value;
                    r.data.tables.push_back(std::move(table));
                }
                continue;
            }
            if (m.op==Op::ScatterPlot) {
                constexpr size_t maxScatterPoints=250000;
                const auto xProperty=numericParticlePropertyView(r.data,m.scatterXProperty);
                const auto yProperty=numericParticlePropertyView(r.data,m.scatterYProperty);
                const size_t valueCount=r.data.atoms.size();
                if (m.scatterSelectedOnly && r.selected.size()!=valueCount)
                    throw std::runtime_error("Selection length does not match the particle count");
                auto selectedAt=[&](size_t i){return !m.scatterSelectedOnly || r.selected[i]!=0;};
                size_t validCount=0;
                for (size_t i=0;i<valueCount;++i) {
                    if ((i&4095)==0 && cancel && *cancel) throw std::runtime_error("Cancelled");
                    if (!selectedAt(i)) continue;
                    if (std::isfinite(xProperty.value(r.data,i)) &&
                        std::isfinite(yProperty.value(r.data,i))) ++validCount;
                }
                if (validCount==0)
                    throw std::runtime_error(m.scatterSelectedOnly
                        ? "The selected elements contain no finite scatter-plot pairs"
                        : "No finite property pairs are available for the scatter plot");
                const size_t plottedCount=std::min(validCount,maxScatterPoints);
                DataTable table;
                table.name="Scatter plot: "+m.scatterXProperty+" vs "+m.scatterYProperty;
                if (validCount>maxScatterPoints) table.name+=" (deterministic preview)";
                table.columns={m.scatterXProperty,m.scatterYProperty,"Particle index"};
                table.rows.reserve(plottedCount);
                size_t ordinal=0;
                size_t nextSampleOrdinal=0;
                for (size_t i=0;i<valueCount;++i) {
                    if ((i&4095)==0 && cancel && *cancel) throw std::runtime_error("Cancelled");
                    if (!selectedAt(i)) continue;
                    const double x=xProperty.value(r.data,i), y=yProperty.value(r.data,i);
                    if (!std::isfinite(x) || !std::isfinite(y)) continue;
                    if (ordinal==nextSampleOrdinal) {
                        table.rows.push_back({formatDataNumber(x),formatDataNumber(y),std::to_string(i)});
                        const size_t nextIndex=table.rows.size();
                        if (nextIndex<plottedCount)
                            nextSampleOrdinal=(nextIndex*validCount)/plottedCount;
                    }
                    ++ordinal;
                }
                r.data.globalAttributes["ScatterPlot.samples"]=double(validCount);
                r.data.globalAttributes["ScatterPlot.plotted"]=double(table.rows.size());
                r.data.tables.push_back(std::move(table));
                continue;
            }
            if (m.op == Op::Histogram || m.op == Op::ReduceProperty) {
                const auto propertyView=numericParticlePropertyView(r.data,m.property);
                const size_t valueCount = r.data.atoms.size();
                auto valueAt = [&](size_t index) { return propertyView.value(r.data,index); };
                auto checkCancelled = [&](size_t index) {
                    if (cancel && (index & 4095) == 0 && *cancel)
                        throw std::runtime_error("Cancelled");
                };
                if (m.op == Op::ReduceProperty) {
                    if (valueCount == 0) throw std::runtime_error("Cannot reduce a property of an empty dataset");
                    double reduced = m.reduceOperation == 0 ? std::numeric_limits<double>::infinity() :
                                     m.reduceOperation == 1 ? -std::numeric_limits<double>::infinity() : 0;
                    double scale = 0;
                    for (size_t i = 0; i < valueCount; ++i) {
                        checkCancelled(i);
                        const double value = valueAt(i);
                        if (!std::isfinite(value)) throw std::runtime_error("Property contains a non-finite value");
                        if (m.reduceOperation == 0) reduced = std::min(reduced, value);
                        else if (m.reduceOperation == 1) reduced = std::max(reduced, value);
                        else scale = std::max(scale, std::abs(value));
                    }
                    if (m.reduceOperation >= 2 && scale != 0) {
                        // Sum normalized inputs with compensation so a finite mean
                        // doesn't overflow merely because the unscaled sum does.
                        double sum = 0, correction = 0;
                        for (size_t i = 0; i < valueCount; ++i) {
                            checkCancelled(i);
                            const double value = valueAt(i);
                            const double normalized = value / scale;
                            const double adjusted = normalized - correction;
                            const double next = sum + adjusted;
                            correction = (next - sum) - adjusted;
                            sum = next;
                        }
                        const double normalizedResult = m.reduceOperation == 2 ?
                            std::clamp(sum / valueCount, -1.0, 1.0) : sum;
                        reduced = normalizedResult * scale;
                        if (!std::isfinite(reduced))
                            throw std::runtime_error("Reduction result is outside the finite numeric range");
                    }
                    r.data.globalAttributes["ReduceProperty." + m.property + "." +
                        (m.reduceOperation == 0 ? "min" : m.reduceOperation == 1 ? "max" : m.reduceOperation == 2 ? "mean" : "sum")] = reduced;
                } else {
                    DataTable table; table.name = "Histogram: " + m.property;
                    table.columns = {"Bin center", m.histogramNormalization == 0 ? "Count" :
                        m.histogramNormalization == 1 ? "Relative frequency" : "Probability density"};
                    if (m.histogramSelectedOnly && r.selected.size() != valueCount)
                        throw std::runtime_error("Selection length does not match the particle count");
                    double lo = 0, hi = 0;
                    bool first = true;
                    uint64_t sampleCount = 0;
                    for (size_t i = 0; i < valueCount; ++i) {
                        checkCancelled(i);
                        if (m.histogramSelectedOnly && !r.selected[i]) continue;
                        const double value = valueAt(i);
                        if (!std::isfinite(value)) continue;
                        ++sampleCount;
                        if (first) { lo = hi = value; first = false; }
                        else { lo = std::min(lo, value); hi = std::max(hi, value); }
                    }
                    if (first) throw std::runtime_error(m.histogramSelectedOnly
                        ? "The selected elements contain no finite property values"
                        : "Property has no finite values to histogram");
                    const bool constantRange = lo == hi;
                    if (constantRange) {
                        const double expandedLo = lo - .5, expandedHi = hi + .5;
                        if (std::isfinite(expandedLo) && std::isfinite(expandedHi) && expandedLo < expandedHi) {
                            lo = expandedLo;
                            hi = expandedHi;
                        }
                    }
                    int scaleExponent = 0;
                    const double magnitude = std::max(std::abs(lo), std::abs(hi));
                    if (magnitude > 0) std::frexp(magnitude, &scaleExponent);
                    const double scaledLo = std::scalbn(lo, -scaleExponent);
                    const double scaledHi = std::scalbn(hi, -scaleExponent);
                    const double scaledSpan = scaledHi - scaledLo;
                    const double densityScale = m.histogramNormalization == 2
                        ? std::scalbn(double(m.type) / scaledSpan, -scaleExponent) : 1.0;
                    if (m.histogramNormalization == 2 && (!std::isfinite(densityScale) || densityScale <= 0))
                        throw std::runtime_error("Histogram probability-density scale is outside the finite numeric range");
                    std::vector<uint64_t> counts(size_t(m.type));
                    for (size_t i = 0; i < valueCount; ++i) {
                        checkCancelled(i);
                        if (m.histogramSelectedOnly && !r.selected[i]) continue;
                        const double value = valueAt(i);
                        if (!std::isfinite(value)) continue;
                        const double fraction = constantRange ? .5 :
                            (std::scalbn(value, -scaleExponent) - scaledLo) / scaledSpan;
                        const double boundedFraction = std::clamp(fraction, 0.0, 1.0);
                        const auto bin = std::min(size_t(m.type - 1), size_t(boundedFraction * m.type));
                        ++counts[bin];
                    }
                    for (int bin = 0; bin < m.type; ++bin) {
                        const double center = constantRange && lo == hi ? lo :
                            std::scalbn(scaledLo + (bin + .5) * scaledSpan / m.type, scaleExponent);
                        double output = double(counts[bin]);
                        if (m.histogramNormalization == 1)
                            output = double(counts[bin]) / double(sampleCount);
                        else if (m.histogramNormalization == 2)
                            output = (double(counts[bin]) / double(sampleCount)) * densityScale;
                        if (!std::isfinite(output))
                            throw std::runtime_error("Histogram normalized value is outside the finite numeric range");
                        table.rows.push_back({std::to_string(center), m.histogramNormalization == 0
                            ? std::to_string(counts[bin]) : std::to_string(output)});
                    }
                    r.data.globalAttributes["Histogram.samples"] = double(sampleCount);
                    r.data.tables.push_back(std::move(table));
                    if (m.histogramSelectRange) {
                        r.selected.resize(valueCount);
                        for (size_t i = 0; i < valueCount; ++i) {
                            checkCancelled(i);
                            const double value = valueAt(i);
                            r.selected[i] = std::isfinite(value) &&
                                value >= m.histogramRangeStart && value <= m.histogramRangeEnd;
                        }
                    }
                }
                continue;
            }
            if (m.op == Op::BondLengthDistribution) {
                if (r.data.sampled())
                    throw std::runtime_error("Bond length distribution requires full data, not a sampled preview");
                if (r.data.bonds.empty())
                    throw std::runtime_error("Bond length distribution requires explicit bonds; add Create bonds first");
                if (r.data.bonds.size() > 20'000'000)
                    throw std::runtime_error("Bond length distribution is limited to 20 million bonds");
                std::vector<double> lengths;
                lengths.reserve(r.data.bonds.size());
                double lo=std::numeric_limits<double>::infinity(), hi=0;
                size_t bondIndex=0;
                for (const auto &bond : r.data.bonds) {
                    if ((bondIndex++ & 4095)==0 && cancel && *cancel)
                        throw std::runtime_error("Cancelled");
                    const auto vector=bondVector(r.data,bond);
                    const double length=std::hypot(vector[0],vector[1],vector[2]);
                    if (!std::isfinite(length))
                        throw std::runtime_error("Bond length is not finite");
                    lengths.push_back(length); lo=std::min(lo,length); hi=std::max(hi,length);
                }
                if (lo==hi) {
                    const double expandedLo=std::nextafter(lo,-std::numeric_limits<double>::infinity());
                    const double expandedHi=std::nextafter(hi,std::numeric_limits<double>::infinity());
                    if (std::isfinite(expandedLo)) lo=expandedLo;
                    if (std::isfinite(expandedHi)) hi=expandedHi;
                }
                int scaleExponent=0;
                const double magnitude=std::max(std::abs(lo),std::abs(hi));
                if (magnitude>0) std::frexp(magnitude,&scaleExponent);
                const double scaledLo=std::scalbn(lo,-scaleExponent);
                const double scaledHi=std::scalbn(hi,-scaleExponent);
                const double scaledSpan=scaledHi-scaledLo;
                if (!(scaledSpan>0) || !std::isfinite(scaledSpan))
                    throw std::runtime_error("Bond-length range is outside the finite histogram domain");
                const double densityScale=m.histogramNormalization==2
                    ? std::scalbn(double(m.type)/scaledSpan,-scaleExponent) : 1.0;
                if (m.histogramNormalization==2 && (!std::isfinite(densityScale) || densityScale<=0))
                    throw std::runtime_error("Bond-length probability-density scale is outside the finite numeric range");
                std::vector<uint64_t> counts(size_t(m.type));
                size_t sampleIndex=0;
                for (double length:lengths) {
                    if ((sampleIndex++ & 4095)==0 && cancel && *cancel)
                        throw std::runtime_error("Cancelled");
                    const double fraction=lo==hi ? .5 :
                        (std::scalbn(length,-scaleExponent)-scaledLo)/scaledSpan;
                    const auto bin=std::min(size_t(m.type-1),
                        size_t(std::clamp(fraction,0.0,1.0)*m.type));
                    ++counts[bin];
                }
                DataTable table; table.name="Bond length distribution";
                table.columns={"Bond length",m.histogramNormalization==0 ? "Bond count" :
                    m.histogramNormalization==1 ? "Relative frequency" : "Probability density (1/length)"};
                for (int bin=0;bin<m.type;++bin) {
                    const double center=std::scalbn(scaledLo+(bin+.5)*scaledSpan/m.type,scaleExponent);
                    double output=double(counts[bin]);
                    if (m.histogramNormalization==1) output/=double(lengths.size());
                    else if (m.histogramNormalization==2) output=(output/double(lengths.size()))*densityScale;
                    if (!std::isfinite(output))
                        throw std::runtime_error("Bond-length normalized value is outside the finite numeric range");
                    table.rows.push_back({formatDataNumber(center),m.histogramNormalization==0
                        ? std::to_string(counts[bin]) : formatDataNumber(output)});
                }
                r.data.globalAttributes["BondLengthDistribution.count"]=double(lengths.size());
                r.data.globalAttributes["BondLengthDistribution.minimum"]=*std::min_element(lengths.begin(),lengths.end());
                r.data.globalAttributes["BondLengthDistribution.maximum"]=*std::max_element(lengths.begin(),lengths.end());
                r.data.tables.push_back(std::move(table));
                continue;
            }
            if (m.op == Op::BondAngleDistribution) {
                if (r.data.sampled())
                    throw std::runtime_error("Bond angle distribution requires full data, not a sampled preview");
                if (r.data.bonds.empty())
                    throw std::runtime_error("Bond angle distribution requires explicit bonds; add Create bonds first");
                if (r.data.bonds.size() > 20'000'000)
                    throw std::runtime_error("Bond angle distribution is limited to 20 million bonds");
                using Vector=std::array<double,3>;
                std::vector<std::vector<Vector>> neighbors(r.data.atoms.size());
                size_t bondIndex=0;
                for (const auto &bond:r.data.bonds) {
                    if ((bondIndex++ & 4095)==0 && cancel && *cancel)
                        throw std::runtime_error("Cancelled");
                    const auto geometry=bondVector(r.data,bond);
                    Vector v{geometry[0],geometry[1],geometry[2]};
                    neighbors[bond.a].push_back(v);
                    for (double &component:v) component=-component;
                    neighbors[bond.b].push_back(v);
                }
                std::vector<double> angles;
                size_t centralIndex=0;
                for (const auto &incident:neighbors) {
                    if ((centralIndex++ & 4095)==0 && cancel && *cancel)
                        throw std::runtime_error("Cancelled");
                    if (incident.size()>1 && angles.size()+incident.size()*(incident.size()-1)/2>20'000'000)
                        throw std::runtime_error("Bond angle distribution exceeds 20 million angles");
                    for (size_t i=0;i<incident.size();++i) for (size_t j=i+1;j<incident.size();++j) {
                        if ((angles.size() & 4095)==0 && cancel && *cancel)
                            throw std::runtime_error("Cancelled");
                        const auto &a=incident[i], &b=incident[j];
                        const double la=std::hypot(a[0],a[1],a[2]);
                        const double lb=std::hypot(b[0],b[1],b[2]);
                        if (!(la>0) || !(lb>0) || !std::isfinite(la) || !std::isfinite(lb))
                            throw std::runtime_error("Bond angle requires finite, nonzero bond vectors");
                        const double cosine=std::clamp((a[0]/la)*(b[0]/lb)+
                                                       (a[1]/la)*(b[1]/lb)+
                                                       (a[2]/la)*(b[2]/lb),-1.0,1.0);
                        angles.push_back(std::acos(cosine)*180.0/3.14159265358979323846);
                    }
                }
                if (angles.empty())
                    throw std::runtime_error("Bond angle distribution requires at least one particle with two bonds");
                std::vector<uint64_t> counts(size_t(m.type));
                size_t angleIndex=0;
                for (double angle:angles) {
                    if ((angleIndex++ & 4095)==0 && cancel && *cancel)
                        throw std::runtime_error("Cancelled");
                    ++counts[std::min(size_t(m.type-1),size_t(angle/180*m.type))];
                }
                DataTable table; table.name="Bond angle distribution";
                table.columns={"Bond angle (degrees)",m.histogramNormalization==0 ? "Angle count" :
                    m.histogramNormalization==1 ? "Relative frequency" : "Probability density (1/degree)"};
                const double densityScale=double(m.type)/180.0;
                for (int bin=0;bin<m.type;++bin) {
                    double output=double(counts[bin]);
                    if (m.histogramNormalization==1) output/=double(angles.size());
                    else if (m.histogramNormalization==2) output=(output/double(angles.size()))*densityScale;
                    table.rows.push_back({formatDataNumber((bin+.5)*180.0/m.type),m.histogramNormalization==0
                        ? std::to_string(counts[bin]) : formatDataNumber(output)});
                }
                r.data.globalAttributes["BondAngleDistribution.count"]=double(angles.size());
                r.data.globalAttributes["BondAngleDistribution.minimum"]=*std::min_element(angles.begin(),angles.end());
                r.data.globalAttributes["BondAngleDistribution.maximum"]=*std::max_element(angles.begin(),angles.end());
                r.data.tables.push_back(std::move(table));
                continue;
            }
            if (m.op == Op::CommonNeighborAnalysis || m.op == Op::CreateBonds) {
                if (m.op == Op::CreateBonds) {
                    if (!std::all_of(m.bondColor.begin(),m.bondColor.end(),
                            [](float value){return std::isfinite(value)&&value>=0&&value<=1;}))
                        throw std::runtime_error("Bond color channels must be finite values between 0 and 1");
                    if (!m.bondCylinders &&
                        (!std::isfinite(m.bondWidth) || m.bondWidth < .5f || m.bondWidth > 12.f))
                        throw std::runtime_error("Bond line width must be between 0.5 and 12 pixels");
                    if (m.bondCylinders &&
                        (!std::isfinite(m.bondRadius) || m.bondRadius < .001f || m.bondRadius > 100.f))
                        throw std::runtime_error("Bond cylinder radius must be between 0.001 and 100 units");
                    r.data.bondStyle.visible = m.bondsVisible;
                    r.data.bondStyle.width = m.bondWidth;
                    r.data.bondStyle.radius = m.bondCylinders ? m.bondRadius : 0.f;
                    r.data.bondStyle.color = m.bondColor;
                    if (m.discardExistingBonds) r.data.bonds.clear();
                    double searchCutoff=m.value;
                    if (m.bondTypeCutoffsEnabled)
                        searchCutoff=*std::max_element(m.bondTypeCutoffs.begin(),m.bondTypeCutoffs.end());
                    if (searchCutoff>0) {
                        for (const auto &atom : r.data.atoms)
                            if (atom.type>=r.data.species.size())
                                throw std::runtime_error("Particle type is outside the type-pair cutoff table");
                        forEachNeighborPair(r.data, searchCutoff,
                                            [&](uint32_t i, uint32_t j, double distanceSquared,
                                                std::array<int32_t, 3> image) {
                                                if (m.bondTypeCutoffsEnabled) {
                                                    const size_t typeCount=r.data.species.size();
                                                    const size_t a=std::min(r.data.atoms[i].type,r.data.atoms[j].type);
                                                    const size_t b=std::max(r.data.atoms[i].type,r.data.atoms[j].type);
                                                    const double pairCutoff=m.bondTypeCutoffs[a*typeCount+b];
                                                    if (distanceSquared>pairCutoff*pairCutoff) return;
                                                }
                                                r.data.bonds.push_back({i, j, image});
                                            }, cancel);
                    }
                    auto key = [](Bond &bond) {
                        if (bond.a > bond.b) {
                            std::swap(bond.a, bond.b);
                            for (auto &v : bond.image) v = -v;
                        } else if (bond.a == bond.b) {
                            for (const auto v : bond.image) {
                                if (v == 0) continue;
                                if (v < 0)
                                    for (auto &component : bond.image) component = -component;
                                break;
                            }
                        }
                        return std::tuple{bond.a, bond.b, bond.image[0], bond.image[1], bond.image[2]};
                    };
                    std::set<std::tuple<uint32_t, uint32_t, int32_t, int32_t, int32_t>> seen;
                    std::vector<Bond> unique;
                    unique.reserve(r.data.bonds.size());
                    for (auto bond : r.data.bonds) {
                        (void)bondVector(r.data,bond);
                        if (seen.insert(key(bond)).second) unique.push_back(bond);
                    }
                    r.data.bonds = std::move(unique);
                } else {
                    auto cna = analyzeCommonNeighbors(r.data, m.value, cancel);
                    std::vector<double> structure(cna.structure.begin(), cna.structure.end());
                    r.data.scalarProperties["Structure Type"] = std::move(structure);
                    for (const auto &[name, count] : cna.counts)
                        r.data.globalAttributes["CommonNeighborAnalysis.counts." + name] = double(count);
                    DataTable table;
                    table.name = "Structure analysis results";
                    table.columns = {"Structure", "Count", "Fraction", "Id"};
                    const std::array<std::pair<const char *, uint8_t>, 5> kinds{{
                        {"Other",uint8_t(0)}, {"FCC",uint8_t(1)}, {"HCP",uint8_t(2)},
                        {"BCC",uint8_t(3)}, {"Icosahedral",uint8_t(4)}}};
                    for (const auto &[name, id] : kinds) {
                        const auto count = cna.counts.at(name);
                        table.rows.push_back({name, std::to_string(count),
                            r.data.atoms.empty() ? "0" : std::to_string(double(count) / r.data.atoms.size()),
                            std::to_string(id)});
                    }
                    r.data.tables.push_back(std::move(table));
                }
                continue;
            }
            size_t out = 0;
            std::vector<int64_t> particleMap(r.data.atoms.size(), -1);
            for (size_t i = 0; i < r.data.atoms.size(); ++i) {
                if (cancel && (i & 65535) == 0 && *cancel) throw std::runtime_error("Cancelled");
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
                    keep = !m.sliceOperateOnParticles ||
                           (m.sliceApplySelectionOnly && !r.selected[i]) ||
                           slicePasses(a);
                    break;
                case Op::SelectType:
                    if (m.selectedTypes.empty())
                        sel = m.type >= 0 && a.type == uint32_t(m.type);
                    else
                        sel = std::find(m.selectedTypes.begin(), m.selectedTypes.end(),
                                        a.type) != m.selectedTypes.end();
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
                default:
                    break;
                }
                if (keep) {
                    particleMap[i] = int64_t(out);
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
                    if (!r.data.particleColors.empty()) r.data.particleColors[out]=r.data.particleColors[i];
                    r.data.atoms[out] = a;
                    r.colorSelected[out] = r.colorSelected[i];
                    r.selected[out++] = sel;
                }
            }
            r.data.atoms.resize(out);
            for (auto &[name, values] : r.data.scalarProperties)
                values.resize(out);
            for (auto &[name, values] : r.data.vectorProperties)
                values.resize(out);
            if (!r.data.particleColors.empty()) r.data.particleColors.resize(out);
            r.selected.resize(out);
            r.colorSelected.resize(out);
            if (m.op == Op::Slice && m.sliceOperateOnParticles && !m.sliceCreateSelection) {
                r.data.globalAttributes["Slice.input_particles"] = double(particleMap.size());
                r.data.globalAttributes["Slice.particles_deleted"] = double(particleMap.size() - out);
                r.data.globalAttributes["Slice.particles_remaining"] = double(out);
            }
            if (m.op == Op::Delete || m.op == Op::Slice) {
                std::vector<Bond> remapped;
                remapped.reserve(r.data.bonds.size());
                for (auto bond : r.data.bonds) {
                    if (bond.a >= particleMap.size() || bond.b >= particleMap.size())
                        throw std::runtime_error("Invalid bond endpoint in particle topology");
                    const int64_t a = particleMap[bond.a], b = particleMap[bond.b];
                    if (a < 0 || b < 0) continue;
                    bond.a = uint32_t(a);
                    bond.b = uint32_t(b);
                    remapped.push_back(bond);
                }
                r.data.bonds = std::move(remapped);
            }
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
          } catch (const ModifierExecutionError &) {
              throw;
          } catch (const std::exception &e) {
              throw ModifierExecutionError(modifierIndex, e.what());
          }
        }
    }
    r.data.bounds();
    if (activeNode) *activeNode = mods.size();
    return r;
}
inline PipelineResult evaluate(Dataset source, const std::vector<Modifier> &mods,
                               std::atomic<bool> *cancel = nullptr,
                               std::atomic<size_t> *activeNode = nullptr,
                               const std::function<const Dataset &(size_t)> &referenceProvider = {}) {
    const size_t particleCount=source.atoms.size();
    PipelineResult initial{std::move(source),std::vector<uint8_t>(particleCount),
                           std::vector<uint8_t>(particleCount,1)};
    return evaluateFrom(std::move(initial),mods,0,cancel,activeNode,SIZE_MAX,{},referenceProvider);
}
inline PipelineResult evaluatePrefix(Dataset source, const std::vector<Modifier> &mods,
                                     size_t nodeCount, std::atomic<bool> *cancel = nullptr,
                                     std::atomic<size_t> *activeNode = nullptr) {
    if (nodeCount > mods.size())
        throw std::runtime_error("Pipeline inspection stage is out of range");
    std::vector<Modifier> prefix(mods.begin(), mods.begin() + nodeCount);
    return evaluate(std::move(source), prefix, cancel, activeNode);
}
inline std::pair<double, double> colorRangeAcrossFrames(
    size_t frameCount, const std::function<Dataset(size_t)> &readFrame,
    const std::vector<Modifier> &upstream, const std::string &property,
    std::atomic<bool> *cancel = nullptr, std::atomic<float> *progress = nullptr) {
    if (frameCount == 0 || !readFrame)
        throw std::runtime_error("No trajectory frames are available for color range calculation");
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    for (size_t frame = 0; frame < frameCount; ++frame) {
        if (cancel && *cancel) throw std::runtime_error("Cancelled");
        auto evaluated = evaluate(readFrame(frame), upstream, cancel);
        const auto &data = evaluated.data;
        auto values = particlePropertyValues(data, property, cancel);
        for (double value : values) {
            if (cancel && *cancel) throw std::runtime_error("Cancelled");
            if (std::isfinite(value)) { lo = std::min(lo, value); hi = std::max(hi, value); }
        }
        if (progress) *progress = float(frame + 1) / float(frameCount);
    }
    if (!std::isfinite(lo) || !std::isfinite(hi))
        throw std::runtime_error("No finite values were found across trajectory frames");
    if (lo == hi) { lo -= .5; hi += .5; }
    return {lo, hi};
}
inline PipelineResult evaluate(const Dataset &source, const PipelineGraph &graph,
                               std::atomic<bool> *cancel = nullptr) {
    std::vector<Modifier> executable;
    executable.reserve(graph.nodes.size());
    for (const auto &node : graph.nodes)
        executable.push_back(static_cast<const Modifier &>(node));
    return evaluate(source, executable, cancel);
}
inline PipelineResult evaluateNodes(const Dataset &source, const std::vector<ModifierNode> &nodes,
                                    std::atomic<bool> *cancel = nullptr) {
    PipelineGraph graph;
    graph.nodes = nodes;
    return evaluate(source, graph, cancel);
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
