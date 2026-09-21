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
#include <iomanip>

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
    // Scalar/vector particle properties published by modifiers and readers.
    // Positions and type remain in the compact Atom buffer for the renderer;
    // auxiliary properties are kept separately so adding analysis columns does
    // not change the GPU ABI.
    std::unordered_map<std::string, std::vector<double>> scalarProperties;
    std::unordered_map<std::string, std::vector<Vec3>> vectorProperties;
    std::unordered_map<std::string, std::string> propertyComponents;
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

struct DataObject {
    enum class Kind { Particles, Bonds, Cell, Surface, Dislocations, VoxelGrid, Table, Labels };
    Kind kind = Kind::Particles;
    std::string name;
    bool visible = true;
    size_t sourceNode = 0;
};

struct ModifierNode {
    std::string id;
    std::string displayName;
    std::string category;
    bool enabled = true;
    bool dirty = true;
    bool running = false;
    std::string error;
    std::vector<DataObject> outputs;
};

// Lightweight pipeline contract used by the UI and by asynchronous workers.
// Concrete algorithms can remain in headers while sharing ordering, enabled
// state, diagnostics and output-object publication.
struct PipelineGraph {
    std::vector<ModifierNode> nodes;
    size_t selected = 0;
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
inline std::string lowerExtension(const std::filesystem::path &p) {
    auto s = p.extension().string();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
inline Dataset readPOSCAR(const std::filesystem::path &path) {
    std::ifstream f(path); if (!f) throw std::runtime_error("Cannot open POSCAR");
    Dataset d; std::string line; std::getline(f,d.comment); if (!std::getline(f,line)) throw std::runtime_error("Invalid POSCAR");
    double scale=std::stod(line); for (int r=0;r<3;r++) { if(!std::getline(f,line)) throw std::runtime_error("Invalid POSCAR lattice"); std::istringstream ss(line); for(int c=0;c<3;c++) ss>>d.cell[r*3+c]; }
    if (scale < 0) throw std::runtime_error("Negative POSCAR scale is not supported"); for(auto& v:d.cell)v*=scale;
    if(!std::getline(f,line)) throw std::runtime_error("Invalid POSCAR species"); std::istringstream names(line); std::string s; while(names>>s)d.species.push_back(s);
    if(!std::getline(f,line)) throw std::runtime_error("Invalid POSCAR counts"); std::istringstream counts(line); std::vector<int> nums; int n; while(counts>>n)nums.push_back(n);
    if(nums.size()!=d.species.size()) { // VASP 4: the first line is counts and species are synthetic.
        std::istringstream maybe(line); nums.clear(); while(maybe>>n)nums.push_back(n); d.species.clear(); for(size_t i=0;i<nums.size();i++)d.species.push_back("X"+std::to_string(i+1));
    }
    if(!std::getline(f,line)) throw std::runtime_error("Invalid POSCAR coordinate mode");
    if(!line.empty()&&(line[0]=='S'||line[0]=='s')) { if(!std::getline(f,line)) throw std::runtime_error("Invalid POSCAR selective mode"); }
    bool direct=line.find_first_of("Dd")!=std::string::npos; uint64_t total=0; for(int x:nums)total+=x;
    auto basis=[&](double a,double b,double c){ return Vec3{float(a*d.cell[0]+b*d.cell[3]+c*d.cell[6]),float(a*d.cell[1]+b*d.cell[4]+c*d.cell[7]),float(a*d.cell[2]+b*d.cell[5]+c*d.cell[8])}; };
    for(size_t type=0;type<nums.size();type++) for(int i=0;i<nums[type];i++){ if(!std::getline(f,line))throw std::runtime_error("Truncated POSCAR"); std::istringstream ss(line); double a,b,c;ss>>a>>b>>c;Vec3 p=direct?basis(a,b,c):Vec3{float(a),float(b),float(c)};d.atoms.push_back({p.x,p.y,p.z,uint32_t(type)}); }
    d.sourceCount=d.atoms.size(); d.pbc={true,true,true}; d.bounds(); return d;
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
    auto ext=lowerExtension(path); if(ext==".poscar"||ext==".contcar"||ext==".vasp")return readPOSCAR(path); if(ext==".cif")return readCIF(path); if(ext==".data"||ext==".lmp")return readLammpsData(path);
    auto frames=indexXYZ(path,progress,cancel); return readXYZ(path,frames.front(),budget,progress,cancel);
}
inline void writePOSCAR(const std::filesystem::path& p,const Dataset& d){std::ofstream f(p);if(!f)throw std::runtime_error("Cannot write POSCAR");f<<"AtomX export\n1.0\n";for(int r=0;r<3;r++)f<<d.cell[r*3]<<' '<<d.cell[r*3+1]<<' '<<d.cell[r*3+2]<<'\n';for(auto&s:d.species)f<<s<<' ';f<<"\n";for(size_t i=0;i<d.species.size();i++){size_t n=std::count_if(d.atoms.begin(),d.atoms.end(),[&](auto&a){return a.type==i;});f<<n<<' ';}f<<"\nDirect\n";for(auto&a:d.atoms){double det=d.cell[0]*(d.cell[4]*d.cell[8]-d.cell[5]*d.cell[7])-d.cell[1]*(d.cell[3]*d.cell[8]-d.cell[5]*d.cell[6])+d.cell[2]*(d.cell[3]*d.cell[7]-d.cell[4]*d.cell[6]);double x=(a.x*(d.cell[4]*d.cell[8]-d.cell[5]*d.cell[7])+a.y*(d.cell[2]*d.cell[7]-d.cell[1]*d.cell[8])+a.z*(d.cell[1]*d.cell[5]-d.cell[2]*d.cell[4]))/det;double y=(a.x*(d.cell[5]*d.cell[6]-d.cell[3]*d.cell[8])+a.y*(d.cell[0]*d.cell[8]-d.cell[2]*d.cell[6])+a.z*(d.cell[2]*d.cell[3]-d.cell[0]*d.cell[5]))/det;double z=(a.x*(d.cell[3]*d.cell[7]-d.cell[4]*d.cell[6])+a.y*(d.cell[1]*d.cell[6]-d.cell[0]*d.cell[7])+a.z*(d.cell[0]*d.cell[4]-d.cell[1]*d.cell[3]))/det;f<<std::setprecision(10)<<x<<' '<<y<<' '<<z<<'\n';}}
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
