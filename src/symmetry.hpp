#pragma once
// Crystallographic symmetry and primitive-cell conversion via Spglib 2.7.0.
// Spglib uses column lattice vectors; AtomX stores each vector as a row.
#include "authoring.hpp"
#include "../third_party/spglib/include/spglib.h"
#include <memory>
#include <optional>

namespace atomx::symmetry {
inline constexpr size_t interactiveAtomLimit = 512;
inline constexpr double defaultTolerance = 1e-3; // Angstrom

struct Info {
    int number = 0;
    std::string symbol;
    std::string hall;
    int operations = 0;
    int primitiveAtoms = 0;
};

struct GroupChoice {
    int hall = 0;
    std::string symbol;
};

inline std::optional<GroupChoice> defaultSetting(int number) {
    if (number<1 || number>230) return std::nullopt;
    for (int hall=1;hall<=530;++hall) {
        const auto group=spg_get_spacegroup_type(hall);
        if (group.number==number) return GroupChoice{hall,group.international_short};
    }
    return std::nullopt;
}

struct Site {
    std::string element;
    std::array<double,3> fractional{};
};

inline std::optional<Dataset> expandAsymmetricUnit(const Dataset &cell,
                                                    const std::vector<Site> &sites,
                                                    int spaceGroupNumber) {
    const auto setting=defaultSetting(spaceGroupNumber);
    if (!setting || sites.empty() || sites.size()>32 ||
        authoring::latticeOf(cell).volume<1e-8) return std::nullopt;
    int rotations[192][3][3]{};
    double translations[192][3]{};
    const int count=spg_get_symmetry_from_database(rotations,translations,setting->hall);
    if (count<1 || count>192) return std::nullopt;
    const auto axes=authoring::axes(cell.cell);
    const Vec3 basis[3]{axes.a,axes.b,axes.c};
    double metric[3][3]{};
    for (int i=0;i<3;++i)
        for (int j=0;j<3;++j) metric[i][j]=authoring::dot(basis[i],basis[j]);
    double maxMetric=0;
    for (const auto &row:metric)
        for (double value:row) maxMetric=std::max(maxMetric,std::abs(value));
    // A space-group operation must preserve the cell metric. Reject a cubic
    // group paired with a distorted cell instead of creating false symmetry.
    for (int op=0;op<count;++op)
        for (int i=0;i<3;++i)
            for (int j=0;j<3;++j) {
                double transformed=0;
                for (int p=0;p<3;++p)
                    for (int q=0;q<3;++q)
                        transformed+=rotations[op][p][i]*metric[p][q]*rotations[op][q][j];
                if (std::abs(transformed-metric[i][j])>std::max(1e-6,maxMetric*1e-4))
                    return std::nullopt;
            }
    Dataset result=cell;
    result.atoms.clear();
    result.species.clear();
    struct Placed { uint32_t type; std::array<double,3> f; };
    std::vector<Placed> placed;
    for (const auto &site:sites) {
        if (site.element.empty()) return std::nullopt;
        for (double value:site.fractional) if (!std::isfinite(value)) return std::nullopt;
        const auto type=authoring::speciesIndex(result,site.element);
        for (int op=0;op<count;++op) {
            std::array<double,3> f{};
            for (int axis=0;axis<3;++axis) {
                double value=translations[op][axis];
                for (int component=0;component<3;++component)
                    value+=rotations[op][axis][component]*site.fractional[size_t(component)];
                f[size_t(axis)]=value-std::floor(value);
            }
            const bool duplicate=std::any_of(placed.begin(),placed.end(),[&](const Placed &item){
                if (item.type!=type) return false;
                for (int axis=0;axis<3;++axis) {
                    const double distance=std::abs(item.f[size_t(axis)]-f[size_t(axis)]);
                    if (std::min(distance,1-distance)>1e-5) return false;
                }
                return true;
            });
            if (duplicate) continue;
            if (placed.size()>=4096) return std::nullopt;
            placed.push_back({type,f});
            const Vec3 p=authoring::cartesian(result,f[0],f[1],f[2]);
            result.atoms.push_back({p.x,p.y,p.z,type});
        }
    }
    result.pbc={true,true,true};
    authoring::finish(result,"Built space group "+std::to_string(spaceGroupNumber));
    return result;
}

inline bool prepared(const Dataset &data) {
    return !data.atoms.empty() && data.atoms.size() <= interactiveAtomLimit &&
           !data.sampled() && std::all_of(data.pbc.begin(),data.pbc.end(),[](bool x){return x;}) &&
           authoring::latticeOf(data).volume > 1e-8;
}

inline bool input(const Dataset &data, double lattice[3][3],
                  double (*positions)[3], int *types) {
    if (!prepared(data)) return false;
    for (int axis=0;axis<3;++axis)
        for (int coordinate=0;coordinate<3;++coordinate)
            lattice[coordinate][axis]=data.cell[size_t(axis*3+coordinate)];
    for (size_t i=0;i<data.atoms.size();++i) {
        const auto &atom=data.atoms[i];
        double f[3]{};
        if (!authoring::fractional(data,{atom.x,atom.y,atom.z},f[0],f[1],f[2])) return false;
        for (int axis=0;axis<3;++axis) {
            if (!std::isfinite(f[axis])) return false;
            positions[i][axis]=f[axis]-std::floor(f[axis]);
        }
        types[i]=int(atom.type)+1;
    }
    return true;
}

inline std::optional<Info> analyze(const Dataset &data,double tolerance=defaultTolerance) {
    if (!prepared(data) || !std::isfinite(tolerance) || tolerance<=0) return std::nullopt;
    const int n=int(data.atoms.size());
    auto positions=std::unique_ptr<double[][3]>(new double[size_t(n)][3]);
    std::vector<int> types(size_t(n),0);
    double lattice[3][3]{};
    if (!input(data,lattice,positions.get(),types.data())) return std::nullopt;
    SpglibDataset *raw=spg_get_dataset(lattice,positions.get(),types.data(),n,tolerance);
    if (!raw) return std::nullopt;
    Info info;
    info.number=raw->spacegroup_number;
    info.symbol=raw->international_symbol;
    info.hall=raw->hall_symbol;
    info.operations=raw->n_operations;
    info.primitiveAtoms=0;
    if (raw->mapping_to_primitive) {
        std::vector<int> unique;
        unique.reserve(size_t(n));
        for (int i=0;i<n;++i) unique.push_back(raw->mapping_to_primitive[i]);
        std::sort(unique.begin(),unique.end());
        info.primitiveAtoms=int(std::unique(unique.begin(),unique.end())-unique.begin());
    }
    spg_free_dataset(raw);
    return info;
}

inline std::optional<Dataset> primitive(const Dataset &source,
                                        double tolerance=defaultTolerance) {
    if (!prepared(source) || !std::isfinite(tolerance) || tolerance<=0) return std::nullopt;
    const int n=int(source.atoms.size());
    const size_t capacity=size_t(n)*4;
    auto positions=std::unique_ptr<double[][3]>(new double[capacity][3]);
    std::vector<int> types(capacity,0);
    double lattice[3][3]{};
    if (!input(source,lattice,positions.get(),types.data())) return std::nullopt;
    const int result=spg_standardize_cell(lattice,positions.get(),types.data(),n,1,1,tolerance);
    if (result<=0 || size_t(result)>capacity) return std::nullopt;
    Dataset data=source;
    for (int axis=0;axis<3;++axis)
        for (int coordinate=0;coordinate<3;++coordinate)
            data.cell[size_t(axis*3+coordinate)]=lattice[coordinate][axis];
    data.atoms.clear();
    data.atoms.reserve(size_t(result));
    for (int i=0;i<result;++i) {
        if (types[size_t(i)]<=0 || size_t(types[size_t(i)])>data.species.size()) return std::nullopt;
        const Vec3 p=authoring::cartesian(data,positions[size_t(i)][0],
            positions[size_t(i)][1],positions[size_t(i)][2]);
        data.atoms.push_back({p.x,p.y,p.z,uint32_t(types[size_t(i)]-1)});
    }
    authoring::finish(data,"Primitive cell");
    return data;
}
} // namespace atomx::symmetry
