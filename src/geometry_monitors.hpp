#pragma once
#include "authoring.hpp"
#include <numeric>

namespace atomx::geometry {
inline constexpr size_t monitorLimit=256;
struct Monitor {
    uint8_t count=2;
    std::array<int32_t,4> atoms{-1,-1,-1,-1};
    bool operator==(const Monitor &) const = default;
};
inline bool valid(const Monitor &m,size_t count) {
    if(m.count<2 || m.count>4) return false;
    for(size_t i=0;i<m.count;++i) {
        if(m.atoms[i]<0 || size_t(m.atoms[i])>=count) return false;
        for(size_t j=0;j<i;++j) if(m.atoms[i]==m.atoms[j]) return false;
    }
    for(size_t i=m.count;i<4;++i) if(m.atoms[i]!=-1) return false;
    return true;
}
inline Vec3 at(const Dataset &d,int i) { const auto &a=d.atoms.at(size_t(i)); return {a.x,a.y,a.z}; }
inline std::optional<double> value(const Dataset &d,const Monitor &m) {
    if(!valid(m,d.atoms.size())) return {};
    for(size_t i=0;i<m.count;++i) { const auto q=at(d,m.atoms[i]); if(!std::isfinite(q.x)||!std::isfinite(q.y)||!std::isfinite(q.z)) return {}; }
    const Vec3 a=authoring::sub(at(d,m.atoms[0]),at(d,m.atoms[1]));
    if(m.count==2) return authoring::length(a);
    const Vec3 b=authoring::sub(at(d,m.atoms[2]),at(d,m.atoms[1]));
    if(authoring::length(a)<1e-8 || authoring::length(b)<1e-8) return {};
    if(m.count==3) return authoring::bondAngle(d,m.atoms[0],m.atoms[1],m.atoms[2]);
    const Vec3 c=authoring::sub(at(d,m.atoms[3]),at(d,m.atoms[2]));
    if(authoring::length(authoring::cross(a,b))<1e-8 || authoring::length(authoring::cross(b,c))<1e-8) return {};
    return authoring::dihedralAngle(d,m.atoms[0],m.atoms[1],m.atoms[2],m.atoms[3]);
}
struct Plan {
    Monitor monitor;
    bool inverted=false;
    double initial=0;
    Vec3 pivot{},axis{};
    std::vector<std::pair<int,Vec3>> originals;
};
// Explicit operation only: O(N+B) adjacency, never a per-frame neighbor search.
// Removing the pivot edge must separate the moving branch from every anchor.
inline Plan prepare(const Dataset &d,const Monitor &m,bool inverted=false) {
    if(d.sampled()) throw std::invalid_argument("采样体系不能修改几何");
    const auto v=value(d,m); if(!v) throw std::invalid_argument("测量点重合或扭转角未定义");
    if(m.count==2 && *v<1e-8) throw std::invalid_argument("重合点不能调整距离");
    int cutA=m.atoms[0],cutB=m.atoms[1],seed=inverted?cutA:cutB;
    std::vector<int> fixed{inverted?cutB:cutA};
    if(m.count==3) {
        cutA=m.atoms[1]; cutB=m.atoms[inverted?0:2]; seed=cutB;
        fixed={m.atoms[1],m.atoms[inverted?2:0]};
    } else if(m.count==4) {
        cutA=m.atoms[1]; cutB=m.atoms[2]; seed=m.atoms[inverted?0:3];
        fixed={m.atoms[inverted?2:0],m.atoms[inverted?3:1]};
    }
    const size_t n=d.atoms.size();
    std::vector<size_t> offsets(n+1,0);
    std::vector<uint8_t> boundary(n,0);
    auto cut=[&](const Bond &b){return (int(b.a)==cutA && int(b.b)==cutB)||(int(b.a)==cutB && int(b.b)==cutA);};
    for(const auto &b:d.bonds) {
        if(b.a>=n || b.b>=n) throw std::invalid_argument("显式键索引无效");
        if(b.image!=std::array<int32_t,3>{}) boundary[b.a]=boundary[b.b]=1;
        if(!cut(b)) { ++offsets[b.a+1]; ++offsets[b.b+1]; }
    }
    std::partial_sum(offsets.begin(),offsets.end(),offsets.begin());
    std::vector<int> neighbors(offsets.back()); auto cursor=offsets;
    for(const auto &b:d.bonds) if(!cut(b)) { neighbors[cursor[b.a]++]=int(b.b); neighbors[cursor[b.b]++]=int(b.a); }
    std::vector<uint8_t> visited(n,0); std::vector<int> branch{seed}; visited[size_t(seed)]=1;
    for(size_t i=0;i<branch.size();++i) {
        const int index=branch[i];
        if(boundary[size_t(index)]) throw std::invalid_argument("含跨周期键的片段暂不能调整几何");
        if(std::find(fixed.begin(),fixed.end(),index)!=fixed.end()) throw std::invalid_argument("环或连通路径约束：无法独立移动这一侧");
        for(size_t j=offsets[size_t(index)];j<offsets[size_t(index)+1];++j)
            if(!visited[size_t(neighbors[j])]) { visited[size_t(neighbors[j])]=1; branch.push_back(neighbors[j]); }
    }
    Plan p; p.monitor=m; p.inverted=inverted; p.initial=*v;
    p.pivot=at(d,m.atoms[1]);
    const auto first=authoring::sub(at(d,m.atoms[0]),p.pivot);
    if(m.count==2) p.axis=authoring::scale(first,-1/authoring::length(first));
    else if(m.count==3) {
        const auto last=authoring::sub(at(d,m.atoms[2]),p.pivot);
        p.axis=authoring::cross(first,last);
        if(authoring::length(p.axis)<1e-8) p.axis=authoring::cross(first,std::abs(first.x)<std::abs(first.y)?Vec3{1,0,0}:Vec3{0,1,0});
    } else p.axis=authoring::sub(at(d,m.atoms[2]),p.pivot);
    for(int index:branch) p.originals.push_back({index,at(d,index)});
    return p;
}
inline double delta(const Plan &p,double target) {
    if(!std::isfinite(target) || (p.monitor.count==2 && target<=0) ||
       (p.monitor.count==3 && (target<0 || target>180)) ||
       (p.monitor.count==4 && (target<-180 || target>180))) throw std::invalid_argument("距离须大于零；角度 0–180°；扭转角 −180–180°");
    const double d=p.monitor.count==4?std::remainder(target-p.initial,360):target-p.initial;
    return p.inverted?-d:d;
}
inline std::vector<std::pair<int,Vec3>> positions(const Plan &p,double target) {
    const double d=delta(p,target); auto result=p.originals;
    for(auto &[index,at]:result) {
        (void)index;
        at=p.monitor.count==2?authoring::add(at,authoring::scale(p.axis,d)):
            authoring::rotatedPoint(at,p.pivot,p.axis,d*authoring::kPi/180);
        if(!std::isfinite(at.x)||!std::isfinite(at.y)||!std::isfinite(at.z)) throw std::invalid_argument("几何修改超出坐标范围");
    }
    return result;
}
inline void apply(Dataset &d,const Plan &p,double target) {
    const auto edits=positions(p,target); const double angle=delta(p,target)*authoring::kPi/180;
    for(const auto &[index,at]:edits) {
        auto &a=d.atoms.at(size_t(index)); a.x=at.x; a.y=at.y; a.z=at.z;
        if(p.monitor.count>2) for(auto &[name,values]:d.vectorProperties) {
            if(!spatialVectorProperty(name))continue;
            if(values.size()!=d.atoms.size()) continue;
            auto &v=values[size_t(index)];
            if(std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z)) v=authoring::rotatedPoint(v,{},p.axis,angle);
        }
    }
}
} // namespace atomx::geometry
