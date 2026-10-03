#pragma once
#include "fragment_library.hpp"
#include <memory>

namespace atomx::fragments {
// Existing-fragment gestures cache topology once. Preview is a rigid transform,
// not a second Dataset, and has no template-library atom limit.
struct FusionSeed {
    int connector=-1,root=-1,removed=-1;
    size_t atomCount=0,bondCount=0;
    uint64_t topology=0;
    Vec3 origin{},direction{};
    std::vector<int> indices;
    std::vector<Vec3> originals;
    std::vector<uint32_t> types;
    std::vector<uint8_t> member;
    std::vector<std::array<int,2>> previewEdges;
};
struct Fusion {
    std::shared_ptr<const FusionSeed> seed;
    int target=-1,targetNeighbor=-1,anchor=-1,removed=-1;
    uint32_t targetType=0;
    Vec3 targetPosition{},neighborPosition{},anchorPosition{},pivot{},axis{};
    std::array<Vec3,3> axes{};
};
inline uint64_t fusionTopology(const Dataset &d) {
    uint64_t hash=14695981039346656037ull;
    auto mix=[&](uint64_t v) { hash^=v; hash*=1099511628211ull; };
    for (const auto &b:d.bonds) {
        mix(b.a); mix(b.b); mix(b.order);
        for (auto v:b.image) mix(uint32_t(v));
    }
    return hash;
}
inline bool isHydrogen(const Dataset &d,int index) {
    const auto type=d.atoms.at(size_t(index)).type;
    return type<d.species.size() && d.species[type]=="H";
}
inline FusionSeed prepareFusion(const Dataset &d,int connector) {
    using namespace authoring;
    const int neighbor=terminalNeighbor(d,connector);
    if (neighbor<0) throw std::invalid_argument("连接点必须是只有一条直接键的末端原子");
    FusionSeed s; s.connector=connector; s.atomCount=d.atoms.size(); s.bondCount=d.bonds.size();
    s.topology=fusionTopology(d); s.removed=isHydrogen(d,connector)?connector:-1;
    s.root=s.removed>=0?neighbor:connector; s.origin=at(d,s.root);
    s.direction=sub(at(d,neighbor),at(d,connector));
    if (!document::finite(s.direction) || length(s.direction)<1e-6)
        throw std::invalid_argument("连接点的键方向无效");
    s.direction=scale(s.direction,1/length(s.direction));
    s.indices=fragment(d,connector); std::sort(s.indices.begin(),s.indices.end());
    s.member.resize(d.atoms.size()); std::vector<int> local(d.atoms.size(),-1);
    for (int index:s.indices) {
        s.member[size_t(index)]=1; local[size_t(index)]=int(s.originals.size());
        const auto point=at(d,index);
        if (!document::finite(point)) throw std::invalid_argument("片段含无效坐标");
        s.originals.push_back(point); s.types.push_back(d.atoms[size_t(index)].type);
    }
    for (const auto &b:d.bonds) if (s.member[b.a] || s.member[b.b]) {
        if (b.image!=std::array<int32_t,3>{}) throw std::invalid_argument("不能对齐含跨周期键的移动片段");
        if (s.previewEdges.size()<4096 && int(b.a)!=s.removed && int(b.b)!=s.removed)
            s.previewEdges.push_back({local[b.a],local[b.b]});
    }
    return s;
}
inline Fusion alignFusion(const Dataset &d,std::shared_ptr<const FusionSeed> seed,int target,Vec3 normal) {
    using namespace authoring;
    if (!seed || seed->atomCount!=d.atoms.size() || seed->bondCount!=d.bonds.size())
        throw std::invalid_argument("片段已改变，请重新指定连接点");
    const int neighbor=terminalNeighbor(d,target);
    if (neighbor<0) throw std::invalid_argument("请点击另一片段的末端原子");
    if (seed->member[size_t(target)]) throw std::invalid_argument("连接点必须属于两个独立片段");
    Fusion f; f.seed=std::move(seed); f.target=target; f.targetPosition=at(d,target);
    f.targetNeighbor=neighbor; f.neighborPosition=at(d,neighbor); f.targetType=d.atoms[size_t(target)].type;
    f.removed=isHydrogen(d,target)?target:-1; f.anchor=f.removed>=0?neighbor:target;
    f.anchorPosition=at(d,f.anchor);
    Vec3 direction=sub(at(d,target),at(d,neighbor));
    if (!document::finite(direction) || length(direction)<1e-6 || !document::finite(normal) || length(normal)<1e-6)
        throw std::invalid_argument("目标键方向无效");
    direction=scale(direction,1/length(direction));
    f.axis=direction; f.pivot=add(f.anchorPosition,scale(direction,
        sketchBondLength(d,f.anchor,d.species.at(d.atoms[size_t(f.seed->root)].type))));
    Vec3 u=cross(normal,direction);
    if (length(u)<1e-6) u=cross(std::abs(direction.z)<.8?Vec3{0,0,1}:Vec3{0,1,0},direction);
    u=scale(u,1/length(u)); const Vec3 v=cross(direction,u);
    const Vec3 x=f.seed->direction;
    Vec3 y=cross(std::abs(x.z)<.8?Vec3{0,0,1}:Vec3{0,1,0},x); y=scale(y,1/length(y));
    const Vec3 z=cross(x,y);
    for (int j=0;j<3;++j) {
        const Vec3 unit=j==0?Vec3{1,0,0}:j==1?Vec3{0,1,0}:Vec3{0,0,1};
        f.axes[size_t(j)]=add(scale(direction,dot(unit,x)),add(scale(u,dot(unit,y)),scale(v,dot(unit,z))));
    }
    return f;
}
inline Vec3 fusionVector(const Fusion &f,Vec3 value) {
    using namespace authoring;
    return add(scale(f.axes[0],value.x),add(scale(f.axes[1],value.y),scale(f.axes[2],value.z)));
}
inline Vec3 fusionPoint(const Fusion &f,size_t local) {
    return authoring::add(f.pivot,fusionVector(f,authoring::sub(f.seed->originals.at(local),f.seed->origin)));
}
inline Fusion rotated(Fusion f,double radians) {
    if (!std::isfinite(radians)) throw std::invalid_argument("片段旋转角无效");
    for (auto &axis:f.axes) axis=authoring::rotatedPoint(axis,{},f.axis,radians);
    return f;
}
inline std::vector<int> fusionRemoved(const Fusion &f) {
    std::vector<int> removed;
    for (int i:{f.seed->removed,f.removed}) if (i>=0) removed.push_back(i);
    std::sort(removed.begin(),removed.end()); return removed;
}
inline void preflight(const Dataset &d,const Fusion &f,const std::vector<uint8_t> &hidden={}) {
    using namespace authoring;
    if (!f.seed || f.seed->atomCount!=d.atoms.size() || f.seed->bondCount!=d.bonds.size() ||
        f.seed->topology!=fusionTopology(d)) throw std::invalid_argument("片段拓扑已改变，请重新指定连接点");
    auto visible=[&](int index) { return index>=0 && size_t(index)<d.atoms.size() &&
        !(size_t(index)<hidden.size() && hidden[size_t(index)]); };
    for (int index:{f.seed->connector,f.seed->root,f.target,f.anchor})
        if (!visible(index)) throw std::invalid_argument("连接点已改变或被隐藏");
    if (terminalNeighbor(d,f.seed->connector)<0 || terminalNeighbor(d,f.target)!=f.targetNeighbor ||
        d.atoms[size_t(f.target)].type!=f.targetType ||
        length(sub(at(d,f.targetNeighbor),f.neighborPosition))>.001 ||
        length(sub(at(d,f.target),f.targetPosition))>.001 || length(sub(at(d,f.anchor),f.anchorPosition))>.001)
        throw std::invalid_argument("连接点已改变");
    if (!document::finite(f.pivot)) throw std::invalid_argument("连接位置无效");
    for (auto axis:f.axes) if (!document::finite(axis)) throw std::invalid_argument("连接朝向无效");
    for (size_t i=0;i<f.seed->indices.size();++i) {
        const int index=f.seed->indices[i];
        if (d.atoms[size_t(index)].type!=f.seed->types[i] ||
            length(sub(at(d,index),f.seed->originals[i]))>.001 || !document::finite(fusionPoint(f,i)))
            throw std::invalid_argument("移动片段已改变");
    }
    if (d.bonds.size()+1>interactiveBondBudget) throw std::invalid_argument("键数已达到交互显示上限");
}
// UI preflights before recording undo; this also preflights for non-UI callers.
inline std::vector<int> apply(Dataset &d,const Fusion &f) {
    preflight(d,f);
    const auto removed=fusionRemoved(f);
    auto remap=[&](int index) { return index-int(std::lower_bound(removed.begin(),removed.end(),index)-removed.begin()); };
    std::vector<int> selected;
    for (size_t i=0;i<f.seed->indices.size();++i) {
        const int index=f.seed->indices[i];
        if (index==f.seed->removed) continue;
        const auto point=fusionPoint(f,i); auto &a=d.atoms[size_t(index)]; a.x=point.x; a.y=point.y; a.z=point.z;
        for (auto &[name,rows]:d.vectorProperties) {
            if(!spatialVectorProperty(name))continue;
            if (rows.size()==d.atoms.size() && document::finite(rows[size_t(index)]))
                rows[size_t(index)]=fusionVector(f,rows[size_t(index)]);
        }
        selected.push_back(remap(index));
    }
    authoring::eraseAtoms(d,removed);
    d.bonds.push_back({uint32_t(remap(f.anchor)),uint32_t(remap(f.seed->root)),{},1});
    d.sourceCount=d.atoms.size(); d.bounds(); return selected;
}
} // namespace atomx::fragments
