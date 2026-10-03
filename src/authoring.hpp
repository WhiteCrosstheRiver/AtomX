#pragma once
// Direct structure editing for AtomX 1.0. Positions stay Cartesian angstroms,
// matching the rest of the pipeline. Operations rewrite a Dataset in place so
// Creation Mode and the modeling menus share one path.
#include "core.hpp"
#include "elements.hpp"
#include <cmath>
#include <numeric>
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

inline int directBondIndex(const Dataset &data,int a,int b) {
    for (size_t index=0;index<data.bonds.size();++index) {
        const auto &bond=data.bonds[index];
        if (bond.image!=std::array<int32_t,3>{}) continue;
        if ((bond.a==uint32_t(a) && bond.b==uint32_t(b)) ||
            (bond.a==uint32_t(b) && bond.b==uint32_t(a))) return int(index);
    }
    return -1;
}
inline bool setDirectBond(Dataset &data,int a,int b,int order) {
    if (a<0 || b<0 || a==b || size_t(a)>=data.atoms.size() || size_t(b)>=data.atoms.size() ||
        order<0 || order>4) throw std::invalid_argument("连键需要两个不同原子，键级为 1-3 或芳香键");
    const int existing=directBondIndex(data,a,b);
    if (!order) {
        if (existing<0) return false;
        data.bonds.erase(data.bonds.begin()+existing); return true;
    }
    if (existing>=0) {
        if (data.bonds[size_t(existing)].order==order) return false;
        data.bonds[size_t(existing)].order=uint8_t(order); return true;
    }
    const auto &first=data.atoms[size_t(a)], &last=data.atoms[size_t(b)];
    const auto delta=sub({first.x,first.y,first.z},{last.x,last.y,last.z});
    if (!std::isfinite(length(delta)) || length(delta)<1e-6)
        throw std::invalid_argument("重合或无效坐标的原子不能连键");
    if (data.bonds.size()>=interactiveBondBudget) throw std::invalid_argument("键数已达到交互显示上限");
    data.bonds.push_back({uint32_t(a),uint32_t(b),{},uint8_t(order)}); return true;
}
inline double sketchBondLength(const Dataset &data,int anchor,const std::string &element) {
    const auto *next=elements::find(element);
    const auto *start=anchor>=0 && size_t(anchor)<data.atoms.size() &&
        data.atoms[size_t(anchor)].type<data.species.size()?
        elements::find(data.species[data.atoms[size_t(anchor)].type]):nullptr;
    return (start?start->covalent:.76)+(next?next->covalent:.76);
}
inline Vec3 sketchPosition(Vec3 anchor,Vec3 plane,Vec3 towardViewer,double bondLength,bool behind,bool stretch) {
    Vec3 delta=sub(plane,anchor);
    if (stretch) return plane;
    const double distance=length(delta);
    if (distance>bondLength) delta=scale(delta,bondLength/distance);
    const double depth=std::sqrt(std::max(0.0,bondLength*bondLength-dot(delta,delta)));
    return add(anchor,add(delta,scale(towardViewer,(behind?-1:1)*depth)));
}
// Preserve every surviving explicit bond, its periodic image and its order.
inline void eraseAtoms(Dataset &data,const std::vector<int> &removed) {
    const size_t count=data.atoms.size();
    std::vector<int> remap(count,-1); size_t cursor=0,out=0;
    for (size_t i=0;i<count;++i) {
        if (cursor<removed.size() && size_t(removed[cursor])==i) { ++cursor; continue; }
        remap[i]=int(out); data.atoms[out++]=data.atoms[i];
    }
    data.atoms.resize(out);
    data.bonds.erase(std::remove_if(data.bonds.begin(),data.bonds.end(),[&](Bond &bond) {
        if (bond.a>=count || bond.b>=count || remap[bond.a]<0 || remap[bond.b]<0) return true;
        bond.a=uint32_t(remap[bond.a]); bond.b=uint32_t(remap[bond.b]); return false;
    }),data.bonds.end());
    auto compact=[&](auto &values) {
        if (values.size()!=count) { values.clear(); return; }
        for (size_t i=0;i<count;++i) if (remap[i]>=0) values[size_t(remap[i])]=values[i];
        values.resize(out);
    };
    compact(data.particleColors);
    for (auto &[name,values]:data.scalarProperties) { (void)name; compact(values); }
    for (auto &[name,values]:data.vectorProperties) { (void)name; compact(values); }
}

// Connected component of explicit bond topology. No distance guessing or
// all-pairs search: large structures take O(atoms + bonds) on demand only.
inline std::vector<int> fragment(const Dataset &data, int seed) {
    if (seed < 0 || size_t(seed) >= data.atoms.size()) return {};
    std::vector<size_t> offsets(data.atoms.size()+1,0);
    for (const auto &bond:data.bonds)
        if (bond.a<data.atoms.size() && bond.b<data.atoms.size()) {
            ++offsets[bond.a+1]; ++offsets[bond.b+1];
        }
    std::partial_sum(offsets.begin(),offsets.end(),offsets.begin());
    auto cursor=offsets;
    std::vector<uint32_t> neighbors(offsets.back());
    for (const auto &bond:data.bonds)
        if (bond.a<data.atoms.size() && bond.b<data.atoms.size()) {
            neighbors[cursor[bond.a]++]=bond.b;
            neighbors[cursor[bond.b]++]=bond.a;
        }
    std::vector<uint8_t> visited(data.atoms.size(),0);
    std::vector<int> found{seed}; visited[size_t(seed)]=1;
    for (size_t i=0;i<found.size();++i)
        for (size_t j=offsets[size_t(found[i])];j<offsets[size_t(found[i])+1];++j)
            if (!visited[neighbors[j]]) {
                visited[neighbors[j]]=1; found.push_back(int(neighbors[j]));
            }
    return found;
}
inline Vec3 rotatedPoint(Vec3 point, Vec3 center, Vec3 axis, double radians) {
    const double norm=length(axis);
    if (norm<1e-10) return point;
    axis=scale(axis,1/norm);
    const Vec3 v=sub(point,center);
    return add(center,add(add(scale(v,std::cos(radians)),
        scale(cross(axis,v),std::sin(radians))),
        scale(axis,dot(axis,v)*(1-std::cos(radians)))));
}

// Validate and prepare a rigid transform before the caller records history.
// The input is untouched, including on overflow or malformed selections.
inline std::vector<std::pair<int,Vec3>> transformedSelection(const Dataset &data,
        std::vector<int> selected, Vec3 translation, Vec3 axis, double degrees, std::optional<Vec3> pivot={}) {
    auto finite=[](Vec3 v) {
        return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);
    };
    if (!finite(translation)||!finite(axis)||!std::isfinite(degrees))
        throw std::invalid_argument("位移与角度必须为有限数值");
    if (degrees!=0 && length(axis)<1e-10)
        throw std::invalid_argument("旋转轴不能为零");
    std::sort(selected.begin(),selected.end());
    selected.erase(std::unique(selected.begin(),selected.end()),selected.end());
    double cx=0,cy=0,cz=0;
    for (int index:selected) {
        if (index<0 || size_t(index)>=data.atoms.size())
            throw std::invalid_argument("选中原子已改变，请重新选择");
        const auto &a=data.atoms[size_t(index)];
        if (!finite({a.x,a.y,a.z})) throw std::invalid_argument("原子坐标无效");
        cx+=a.x; cy+=a.y; cz+=a.z;
    }
    if (selected.empty() || (degrees==0 && length(translation)==0)) return {};
    const Vec3 center=pivot.value_or(Vec3{float(cx/selected.size()),float(cy/selected.size()),float(cz/selected.size())});
    if (!finite(center)) throw std::invalid_argument("旋转中心必须为有限数值");
    std::vector<std::pair<int,Vec3>> positions;
    positions.reserve(selected.size());
    bool changed=false;
    for (int index:selected) {
        const auto &a=data.atoms[size_t(index)];
        const Vec3 original{a.x,a.y,a.z};
        const Vec3 rotated=degrees==0?original:rotatedPoint(original,center,axis,degrees*kPi/180);
        const Vec3 at=add(rotated,translation);
        if (!finite(at)) throw std::invalid_argument("位移超出坐标范围");
        changed=changed || at.x!=original.x || at.y!=original.y || at.z!=original.z;
        positions.push_back({index,at});
    }
    if (!changed) return {};
    return positions;
}

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
    else if (ab && nearEq(p.alpha, 90) && nearEq(p.beta, 90) && nearEq(p.gamma, 120))
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
inline bool setAtomPosition(Dataset &data, int index, Vec3 position, bool useFractional) {
    if (index<0 || size_t(index)>=data.atoms.size() || !std::isfinite(position.x) ||
        !std::isfinite(position.y) || !std::isfinite(position.z)) return false;
    if (useFractional) {
        double a,b,c;
        if (!fractional(data,{},a,b,c)) return false;
        position=cartesian(data,position.x,position.y,position.z);
    }
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
        return false;
    auto &atom=data.atoms[size_t(index)];
    atom.x=position.x; atom.y=position.y; atom.z=position.z;
    data.bounds();
    return true;
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

// Ring sketches are small value objects: preview never copies the structure.
// Order 4 denotes aromatic (1.5), not a quadruple bond.
struct RingSketch {
    int size=6, anchor=-1, bond=-1;
    bool aromatic=false;
    std::array<Vec3,6> points{};
    Vec3 center{}, rotationAxis{};
};
inline RingSketch ringSketch(const Dataset &data,int size,Vec3 at,Vec3 right,Vec3 normal,
                             int anchor=-1,int bond=-1,bool aromatic=false) {
    if (size<4 || size>6 || length(normal)<1e-8 || length(right)<1e-8)
        throw std::invalid_argument("环需要 4、5 或 6 个顶点和有效绘制平面");
    normal=scale(normal,1/length(normal));
    right=sub(right,scale(normal,dot(right,normal)));
    if (length(right)<1e-8) throw std::invalid_argument("绘制平面无效");
    right=scale(right,1/length(right));
    RingSketch ring; ring.size=size; ring.anchor=anchor; ring.bond=bond; ring.aromatic=aromatic;
    const double turn=2*kPi/size;
    if (bond>=0) {
        if (size_t(bond)>=data.bonds.size()) throw std::invalid_argument("原有键已改变");
        const auto &b=data.bonds[size_t(bond)];
        if (b.image!=std::array<int32_t,3>{} || b.a>=data.atoms.size() || b.b>=data.atoms.size())
            throw std::invalid_argument("只能在直接键上接环");
        const auto &a=data.atoms[b.a],&z=data.atoms[b.b];
        at={a.x,a.y,a.z}; const Vec3 end{z.x,z.y,z.z};
        const Vec3 edge=sub(end,at); const double len=length(edge);
        if (len<1e-6 || !std::isfinite(len)) throw std::invalid_argument("原有键长无效");
        const Vec3 direction=scale(edge,1/len);
        Vec3 inward=cross(normal,direction);
        if (length(inward)<1e-5) inward=sub(right,scale(direction,dot(right,direction)));
        if (length(inward)<1e-5) inward=cross(direction,std::abs(direction.z)<.8?Vec3{0,0,1}:Vec3{0,1,0});
        inward=scale(inward,1/length(inward));
        normal=cross(direction,inward);
        ring.center=add(scale(add(at,end),.5),scale(inward,len/(2*std::tan(kPi/size))));
        ring.rotationAxis=edge;
        for (int i=0;i<size;++i) ring.points[size_t(i)]=rotatedPoint(at,ring.center,normal,i*turn);
        ring.points[0]=at; ring.points[1]=end; // Exact shared endpoints.
    } else {
        if (anchor>=0) {
            if (size_t(anchor)>=data.atoms.size()) throw std::invalid_argument("原有原子已改变");
            const auto &a=data.atoms[size_t(anchor)]; at={a.x,a.y,a.z};
            // Orient away from existing direct neighbors, without guessing valence.
            Vec3 occupied{};
            for (const auto &b:data.bonds) if (b.image==std::array<int32_t,3>{}) {
                const int next=b.a==uint32_t(anchor)?int(b.b):b.b==uint32_t(anchor)?int(b.a):-1;
                if (next>=0 && size_t(next)<data.atoms.size()) {
                    const auto &n=data.atoms[size_t(next)];
                    const auto d=sub({n.x,n.y,n.z},at);
                    if (length(d)>1e-6) occupied=add(occupied,scale(d,1/length(d)));
                }
            }
            occupied=sub(occupied,scale(normal,dot(occupied,normal)));
            if (length(occupied)>1e-5) right=scale(occupied,-1/length(occupied));
        }
        const double radius=(aromatic?1.40:1.52)/(2*std::sin(kPi/size));
        ring.center=add(at,scale(right,radius));
        ring.rotationAxis=anchor>=0?right:normal;
        for (int i=0;i<size;++i) ring.points[size_t(i)]=rotatedPoint(at,ring.center,normal,i*turn);
        ring.points[0]=at;
    }
    return ring;
}
inline RingSketch rotatedRing(RingSketch ring,double radians) {
    const Vec3 pivot=ring.points[0];
    for (int i=1;i<ring.size;++i) ring.points[size_t(i)]=rotatedPoint(ring.points[size_t(i)],pivot,ring.rotationAxis,radians);
    ring.center=rotatedPoint(ring.center,pivot,ring.rotationAxis,radians);
    return ring;
}
struct RingEdit {
    std::vector<Atom> atoms;
    std::vector<Bond> bonds;
    std::vector<int> selection;
    std::vector<int> aromaticUpdates;
    bool changed() const { return !atoms.empty() || !bonds.empty() || !aromaticUpdates.empty(); }
};
// Preflight before the caller records history. Coincident vertices merge at
// 0.001 A; hidden coincident atoms reject placement rather than duplicate atoms.
inline RingEdit prepareRing(const Dataset &data,const RingSketch &ring,const std::vector<uint8_t> &hidden={}) {
    if (ring.size<4 || ring.size>6 || data.atoms.size()>size_t(INT32_MAX)-6)
        throw std::invalid_argument("环大小或原子数量无效");
    RingEdit edit; edit.selection.resize(size_t(ring.size),-1);
    for (int i=0;i<ring.size;++i) {
        const Vec3 at=ring.points[size_t(i)];
        if (!std::isfinite(at.x)||!std::isfinite(at.y)||!std::isfinite(at.z))
            throw std::invalid_argument("环坐标无效");
    }
    // One linear scan on commit, never during preview.
    for (size_t index=0;index<data.atoms.size();++index) {
        const auto &a=data.atoms[index];
        for (int i=0;i<ring.size;++i) if (dot(sub({a.x,a.y,a.z},ring.points[size_t(i)]),sub({a.x,a.y,a.z},ring.points[size_t(i)]))<1e-6) {
            if (index<hidden.size() && hidden[index]) throw std::invalid_argument("环与隐藏原子重合，请先显示原子");
            if (edit.selection[size_t(i)]<0) edit.selection[size_t(i)]=int(index);
        }
    }
    if (ring.bond>=0) {
        if (size_t(ring.bond)>=data.bonds.size()) throw std::invalid_argument("原有键已改变");
        const auto &b=data.bonds[size_t(ring.bond)];
        if (b.image!=std::array<int32_t,3>{} || b.a>=data.atoms.size() || b.b>=data.atoms.size())
            throw std::invalid_argument("只能在直接键上接环");
        edit.selection[0]=int(b.a); edit.selection[1]=int(b.b);
    } else if (ring.anchor>=0) {
        if (size_t(ring.anchor)>=data.atoms.size()) throw std::invalid_argument("原有原子已改变");
        edit.selection[0]=ring.anchor;
    }
    const int pinned=ring.bond>=0?2:ring.anchor>=0?1:0;
    for (int i=0;i<pinned;++i) {
        const int index=edit.selection[size_t(i)]; const auto &a=data.atoms[size_t(index)];
        if ((size_t(index)<hidden.size() && hidden[size_t(index)]) ||
            length(sub({a.x,a.y,a.z},ring.points[size_t(i)]))>=.001)
            throw std::invalid_argument("接环起点已改变，请重新绘制");
    }
    for (int i=0;i<ring.size;++i) if (edit.selection[size_t(i)]<0) {
        const auto at=ring.points[size_t(i)];
        edit.selection[size_t(i)]=int(data.atoms.size()+edit.atoms.size());
        edit.atoms.push_back({at.x,at.y,at.z,0});
    }
    for (int i=0;i<ring.size;++i) for (int j=i+1;j<ring.size;++j)
        if (edit.selection[size_t(i)]==edit.selection[size_t(j)] ||
            length(sub(ring.points[size_t(i)],ring.points[size_t(j)]))<.001)
            throw std::invalid_argument("环顶点重合，请调整朝向");
    for (int i=0;i<ring.size;++i) {
        const int a=edit.selection[size_t(i)], b=edit.selection[size_t((i+1)%ring.size)];
        const int existing=directBondIndex(data,a,b);
        if (existing<0) edit.bonds.push_back({uint32_t(a),uint32_t(b),{},uint8_t(ring.aromatic?4:1)});
        else if (ring.aromatic && data.bonds[size_t(existing)].order!=4) edit.aromaticUpdates.push_back(existing);
    }
    if (data.bonds.size()+edit.bonds.size()>interactiveBondBudget) throw std::invalid_argument("键数已达到交互显示上限");
    return edit;
}
inline void applyRing(Dataset &data,const RingEdit &edit) {
    const size_t old=data.atoms.size();
    const uint32_t carbon=edit.atoms.empty()?0:speciesIndex(data,"C");
    data.atoms.reserve(old+edit.atoms.size()); data.bonds.reserve(data.bonds.size()+edit.bonds.size());
    for (auto atom:edit.atoms) { atom.type=carbon; data.atoms.push_back(atom); }
    for (auto bond:edit.bonds) data.bonds.push_back(bond);
    for (auto index:edit.aromaticUpdates) data.bonds[size_t(index)].order=4;
    if (data.particleColors.size()==old && !data.particleColors.empty()) data.particleColors.resize(data.atoms.size(),{-1,-1,-1});
    for (auto &[name,values]:data.scalarProperties) {
        if(values.size()==old)values.resize(data.atoms.size(),name=="AtomX.FormalCharge" || name=="FormalCharge" ||
            name=="AtomX.Hybridization" || name=="AtomX.MotionGroup"?0:NAN);
    }
    for (auto &[name,values]:data.vectorProperties) { (void)name; if (values.size()==old) values.resize(data.atoms.size(),{NAN,NAN,NAN}); }
    data.sourceCount=data.atoms.size(); data.bounds();
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

inline bool validCellParameters(double a, double b, double c, double alpha, double beta, double gamma) {
    if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c) ||
        !std::isfinite(alpha) || !std::isfinite(beta) || !std::isfinite(gamma) ||
        a <= 1e-5 || b <= 1e-5 || c <= 1e-5 ||
        alpha <= 0 || alpha >= 180 || beta <= 0 || beta >= 180 || gamma <= 0 || gamma >= 180)
        return false;
    const double ar=alpha*kPi/180, br=beta*kPi/180, gr=gamma*kPi/180;
    const double ca=std::cos(ar), cb=std::cos(br), cg=std::cos(gr);
    const double shape=1+2*ca*cb*cg-ca*ca-cb*cb-cg*cg;
    return shape>1e-10;
}

inline bool setCellParameters(Dataset &data, double a, double b, double c,
                              double alpha, double beta, double gamma,
                              Vec3 origin, std::array<bool,3> pbc, bool preserveFractional) {
    if (!validCellParameters(a,b,c,alpha,beta,gamma) ||
        !std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z)) return false;
    std::vector<std::array<double,3>> fractionalPositions;
    if (preserveFractional) {
        fractionalPositions.reserve(data.atoms.size());
        for (const auto &atom : data.atoms) {
            double fa=0, fb=0, fc=0;
            if (!fractional(data,{atom.x,atom.y,atom.z},fa,fb,fc)) return false;
            fractionalPositions.push_back({fa,fb,fc});
        }
    }
    const Dataset shape=triclinicCell(a,b,c,alpha,beta,gamma,"C");
    data.cell=shape.cell;
    data.origin=origin;
    data.pbc=pbc;
    if (preserveFractional) {
        for (size_t i=0;i<data.atoms.size();++i) {
            const auto &f=fractionalPositions[i];
            const Vec3 p=cartesian(data,f[0],f[1],f[2]);
            data.atoms[i].x=p.x; data.atoms[i].y=p.y; data.atoms[i].z=p.z;
        }
    }
    data.bonds.clear();
    data.bounds();
    return true;
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

inline std::pair<int,int> bezout(int a,int b) {
    const int signA=a<0?-1:1, signB=b<0?-1:1;
    int oldR=std::abs(a),r=std::abs(b),oldS=1,s=0,oldT=0,t=1;
    while (r) {
        const int q=oldR/r;
        const int nextR=oldR-q*r; oldR=r; r=nextR;
        const int nextS=oldS-q*s; oldS=s; s=nextS;
        const int nextT=oldT-q*t; oldT=t; t=nextT;
    }
    return {oldS*signA,oldT*signB};
}

// Re-express the lattice in a unimodular basis with a and b parallel to (hkl).
// This retains all atoms, builds `layers` repeats, and adds vacuum perpendicular
// to the surface. The out-of-plane boundary becomes nonperiodic.
inline std::optional<Dataset> millerSurface(const Dataset &input,int h,int k,int l,
                                             int layers,double vacuum) {
    if ((!h&&!k&&!l) || layers<1 || layers>12 || !std::isfinite(vacuum) || vacuum<0 ||
        input.atoms.empty() || input.atoms.size()*size_t(layers)>60000 ||
        !std::all_of(input.pbc.begin(),input.pbc.end(),[](bool x){return x;}) ||
        latticeOf(input).volume<1e-8) return std::nullopt;
    const int divisor=std::gcd(std::gcd(std::abs(h),std::abs(k)),std::abs(l));
    h/=divisor; k/=divisor; l/=divisor;
    int u[3]{},v[3]{},w[3]{};
    if (!h&&!k) {
        const int sign=l<0?-1:1;
        u[0]=1; v[1]=sign; w[2]=sign;
    } else {
        const int g=std::gcd(std::abs(h),std::abs(k));
        const auto [p,q]=bezout(h,k);
        const auto [r,s]=bezout(g,l);
        u[0]=-k/g; u[1]=h/g;
        v[0]=-p*l; v[1]=-q*l; v[2]=g;
        w[0]=r*p; w[1]=r*q; w[2]=s;
    }
    Dataset data=input;
    for (int coordinate=0;coordinate<3;++coordinate) {
        const double a=input.cell[size_t(coordinate)];
        const double b=input.cell[size_t(3+coordinate)];
        const double c=input.cell[size_t(6+coordinate)];
        data.cell[size_t(coordinate)]=u[0]*a+u[1]*b+u[2]*c;
        data.cell[size_t(3+coordinate)]=v[0]*a+v[1]*b+v[2]*c;
        data.cell[size_t(6+coordinate)]=w[0]*a+w[1]*b+w[2]*c;
    }
    for (auto &atom:data.atoms) {
        double fa=0,fb=0,fc=0;
        if (!fractional(data,{atom.x,atom.y,atom.z},fa,fb,fc)) return std::nullopt;
        const auto wrap=[](double value){value-=std::floor(value); return value;};
        const Vec3 p=cartesian(data,wrap(fa),wrap(fb),wrap(fc));
        atom.x=p.x; atom.y=p.y; atom.z=p.z;
    }
    data=replicate(data,1,1,layers);
    const auto axesNow=axes(data.cell);
    Vec3 normal=cross(axesNow.a,axesNow.b);
    const double normalLength=length(normal);
    if (normalLength<1e-8) return std::nullopt;
    normal=scale(normal,1.0/normalLength);
    if (dot(normal,axesNow.c)<0) normal=scale(normal,-1);
    for (int coordinate=0;coordinate<3;++coordinate)
        data.cell[size_t(6+coordinate)]+=vacuum*(coordinate==0?normal.x:
                                               coordinate==1?normal.y:normal.z);
    const Vec3 offset=scale(normal,vacuum*.5);
    for (auto &atom:data.atoms) {
        atom.x+=offset.x; atom.y+=offset.y; atom.z+=offset.z;
    }
    data.pbc={true,true,false};
    finish(data,"Miller surface ("+std::to_string(h)+" "+std::to_string(k)+" "+
        std::to_string(l)+")");
    return data;
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
