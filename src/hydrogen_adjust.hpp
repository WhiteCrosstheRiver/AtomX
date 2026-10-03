#pragma once
#include "authoring.hpp"
#include <cstring>

namespace atomx::hydrogens {
// Explicit chemical topology only. Neither distance cutoffs nor partial charges
// are evidence of bond order/formal charge. Metals and ambiguous sites are left alone.
struct Options {
    bool all=true, onlyAdd=false;
    std::vector<int> selection;
    int hybridization=0; // 0: keep/automatic, 1: SP, 2: SP2, 3: SP3, 4: reset automatic
    bool setCharge=false;
    int charge=0;
};
struct Move { int index; Vec3 position,from,to; };
struct Addition { int parent; Vec3 position; };
struct Setting { int index,charge,hybridization; };
struct Plan {
    uint64_t fingerprint=0,visibility=0;
    size_t atomCount=0,bondCount=0,sites=0,unsupported=0,ambiguous=0,invalid=0,periodic=0,hidden=0;
    std::vector<int> removed;
    std::vector<Move> moved;
    std::vector<Addition> added;
    std::vector<Setting> settings;
    bool changes() const { return !removed.empty() || !moved.empty() || !added.empty() || !settings.empty(); }
};
inline bool finite(Vec3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
inline Vec3 unit(Vec3 v) {
    const double n=authoring::length(v);
    if (!finite(v) || n<1e-6) throw std::invalid_argument("局部键方向退化");
    return authoring::scale(v,1/n);
}
inline Vec3 point(const Dataset &d,int i) { const auto &a=d.atoms.at(size_t(i)); return {a.x,a.y,a.z}; }
inline int atomicNumber(const Dataset &d,int i) {
    const auto t=d.atoms[size_t(i)].type;
    const auto *e=t<d.species.size()?elements::find(d.species[t]):nullptr;
    return e?e->z:0;
}
inline uint64_t visibilityHash(const std::vector<uint8_t> &hidden) {
    uint64_t h=14695981039346656037ull;
    for(size_t i=0;i<hidden.size();++i) if(hidden[i]) { h^=i+1; h*=1099511628211ull; }
    return h;
}
inline uint64_t fingerprint(const Dataset &d) {
    uint64_t h=14695981039346656037ull;
    auto mix=[&](const auto &v) { const auto *bytes=reinterpret_cast<const unsigned char*>(&v);
        for(size_t i=0;i<sizeof(v);++i) { h^=bytes[i];h*=1099511628211ull; } };
    mix(d.atoms.size()); mix(d.bonds.size());
    for(const auto &a:d.atoms) {mix(a.x);mix(a.y);mix(a.z);mix(a.type);}
    for(const auto &b:d.bonds) {mix(b.a);mix(b.b);mix(b.order);for(auto x:b.image)mix(x);}
    for(auto x:d.cell)mix(x); for(auto x:d.pbc)mix(x);
    for(const auto &s:d.species) {mix(s.size());for(char c:s)mix(c);}
    for(const char *name:{"AtomX.FormalCharge","FormalCharge","AtomX.Hybridization"}) {
        auto it=d.scalarProperties.find(name); const bool present=it!=d.scalarProperties.end();mix(present);
        if(present) {mix(it->second.size());for(double x:it->second)mix(x);}
    }
    return h;
}
inline double property(const Dataset &d,const char *name,size_t i,double fallback) {
    const auto it=d.scalarProperties.find(name);
    if(it==d.scalarProperties.end())return fallback;
    if(it->second.size()!=d.atoms.size())throw std::invalid_argument("原子属性行数不一致");
    return it->second[i];
}
inline void validateRows(const Dataset &d) {
    for(const auto &[name,rows]:d.scalarProperties){(void)name;if(rows.size()!=d.atoms.size())throw std::invalid_argument("标量属性行数不一致");}
    for(const auto &[name,rows]:d.vectorProperties){(void)name;if(rows.size()!=d.atoms.size())throw std::invalid_argument("向量属性行数不一致");}
    if(!d.particleColors.empty() && d.particleColors.size()!=d.atoms.size())throw std::invalid_argument("原子颜色行数不一致");
}
// Newly sketched atoms have missing measured science, neutral formal charge and
// automatic hybridization. Existing values are never rewritten here.
inline void extendRows(Dataset &d,size_t oldCount) {
    for(auto &[name,rows]:d.scalarProperties) {
        if(rows.size()!=oldCount)throw std::invalid_argument("标量属性行数不一致");
        rows.resize(d.atoms.size(),constraintScalarDefault(name,name=="AtomX.FormalCharge" || name=="FormalCharge" ||
            name=="AtomX.Hybridization" || name=="AtomX.MotionGroup"?0:NAN));
    }
    for(auto &[name,rows]:d.vectorProperties) {
        (void)name;if(rows.size()!=oldCount)throw std::invalid_argument("向量属性行数不一致");
        rows.resize(d.atoms.size(),constraintVectorDefault(name));
    }
    if(!d.particleColors.empty()) {
        if(d.particleColors.size()!=oldCount)throw std::invalid_argument("原子颜色行数不一致");
        d.particleColors.resize(d.atoms.size(),{-1,-1,-1});
    }
}
inline int remapIndex(int index,const std::vector<int> &removed) {
    if(index<0)return -1;
    const auto it=std::lower_bound(removed.begin(),removed.end(),index);
    return it!=removed.end() && *it==index?-1:index-int(it-removed.begin());
}
inline int valence(int z,int charge) {
    switch(z) {
    case 5: return charge==0?3:charge==-1?4:-1;
    case 6: return charge==0?4:std::abs(charge)==1?3:-1;
    case 7: case 15: case 33: return charge==0?3:charge==1?4:charge==-1?2:-1;
    case 8: case 16: case 34: case 52: return charge==0?2:charge==1?3:charge==-1?1:-1;
    case 9: case 17: case 35: case 53: return charge==0?1:charge==-1?0:-1;
    case 14: return charge==0?4:-1;
    case 2: case 10: case 18: case 36: case 54: case 86: return charge==0?0:-1;
    default:return -1;
    }
}
inline Vec3 perpendicular(Vec3 axis,Vec3 hint) {
    using namespace authoring;
    Vec3 v=sub(hint,scale(axis,dot(hint,axis)));
    if(length(v)<1e-6) {const Vec3 basis=std::abs(axis.z)<.8?Vec3{0,0,1}:Vec3{0,1,0};v=sub(basis,scale(axis,dot(basis,axis)));}
    return unit(v);
}
inline Vec3 rotatedVector(Vec3 value,Vec3 from,Vec3 to) {
    using namespace authoring;
    const double cosine=std::clamp(dot(from,to),-1.,1.);
    Vec3 axis=cross(from,to);const double sine=length(axis);
    if(sine<1e-6) return cosine>0?value:rotatedPoint(value,{},perpendicular(from,{}),kPi);
    return rotatedPoint(value,{},scale(axis,1/sine),std::atan2(sine,cosine));
}
inline std::vector<Vec3> slots(int hybrid,const std::vector<Vec3> &fixed,const std::vector<Vec3> &existing,Vec3 planeHint={}) {
    using namespace authoring;
    const int capacity=hybrid==1?2:hybrid==2?3:4;
    if(fixed.size()>size_t(capacity))throw std::invalid_argument("键数超出指定杂化的配位数");
    if(fixed.size()==size_t(capacity))return {};
    if(fixed.empty()) {
        std::vector<Vec3> out=hybrid==1?std::vector<Vec3>{{0,0,1},{0,0,-1}}:
            hybrid==2?std::vector<Vec3>{{0,0,1},{float(std::sqrt(3.)/2),0,-.5f},{float(-std::sqrt(3.)/2),0,-.5f}}:
            std::vector<Vec3>{{0,0,1},{float(2*std::sqrt(2.)/3),0,-1.f/3},
                {float(-std::sqrt(2.)/3),float(std::sqrt(2./3)), -1.f/3},
                {float(-std::sqrt(2.)/3),float(-std::sqrt(2./3)), -1.f/3}};
        if(!existing.empty()) {
            for(auto &v:out)v=rotatedVector(v,{0,0,1},existing[0]);
            if(existing.size()>1 && hybrid!=1) {
                const Vec3 a=perpendicular(existing[0],out[1]),b=perpendicular(existing[0],existing[1]);
                const double angle=std::atan2(dot(existing[0],cross(a,b)),dot(a,b));
                for(auto &v:out)v=rotatedPoint(v,{},existing[0],angle);
            }
        }
        return out;
    }
    const auto v=fixed[0];
    if(fixed.size()==1) {
        if(hybrid==1)return {scale(v,-1)};
        const auto u=perpendicular(v,existing.empty()?planeHint:existing[0]),w=cross(v,u);
        std::vector<Vec3> out;
        const double axial=hybrid==2?-.5:-1./3,radial=std::sqrt(1-axial*axial);
        for(int i=0;i<capacity-1;++i) {
            const double angle=2*kPi*i/(capacity-1);
            out.push_back(add(scale(v,axial),add(scale(u,radial*std::cos(angle)),scale(w,radial*std::sin(angle)))));
        }
        return out;
    }
    Vec3 sum{};for(auto direction:fixed)sum=add(sum,direction);
    if(fixed.size()==2) {
        const Vec3 away=scale(unit(sum),-1);
        if(hybrid==2)return {away};
        Vec3 normal=unit(cross(fixed[0],fixed[1]));
        if(!existing.empty() && dot(normal,existing[0])<0)normal=scale(normal,-1);
        return {add(scale(away,std::sqrt(1./3)),scale(normal,std::sqrt(2./3))),
                sub(scale(away,std::sqrt(1./3)),scale(normal,std::sqrt(2./3)))};
    }
    if(length(sum)>1e-6)return {scale(unit(sum),-1)};
    Vec3 normal=unit(cross(fixed[0],fixed[1]));
    if(!existing.empty() && dot(normal,existing[0])<0)normal=scale(normal,-1);
    return {normal};
}
inline Plan prepare(const Dataset &d,const Options &options,const std::vector<uint8_t> &hidden={}) {
    using namespace authoring;
    if(d.sampled())throw std::invalid_argument("采样体系不能调整化学拓扑");
    if(d.atoms.size()>size_t(INT32_MAX) || d.bonds.size()>size_t(UINT32_MAX) || options.hybridization<0 || options.hybridization>4 ||
       (options.setCharge && (options.charge < -4 || options.charge > 4)))
        throw std::invalid_argument("调整参数或原子数超出支持范围");
    validateRows(d);
    Plan p;p.fingerprint=fingerprint(d);p.visibility=visibilityHash(hidden);p.atomCount=d.atoms.size();p.bondCount=d.bonds.size();
    std::vector<size_t> offsets(d.atoms.size()+1);
    for(const auto &b:d.bonds) {
        if(b.a>=d.atoms.size() || b.b>=d.atoms.size() || b.order<1 || b.order>4)throw std::invalid_argument("显式键端点或键级无效");
        ++offsets[b.a+1];++offsets[b.b+1];
    }
    std::partial_sum(offsets.begin(),offsets.end(),offsets.begin());auto cursor=offsets;
    std::vector<uint32_t> edges(offsets.back());
    for(size_t i=0;i<d.bonds.size();++i){const auto &b=d.bonds[i];edges[cursor[b.a]++]=uint32_t(i);edges[cursor[b.b]++]=uint32_t(i);}
    std::vector<uint8_t> scope(d.atoms.size(),options.all?1:0);
    if(!options.all) for(int i:options.selection) if(i>=0 && size_t(i)<d.atoms.size()) {
        if(atomicNumber(d,i)!=1)scope[size_t(i)]=1;
        else if(offsets[size_t(i)+1]-offsets[size_t(i)]==1) {
            const auto &b=d.bonds[edges[offsets[size_t(i)]]];const auto other=b.a==uint32_t(i)?b.b:b.a;
            if(b.order==1 && b.image==std::array<int32_t,3>{} && atomicNumber(d,int(other))!=1)scope[other]=1;
        }
    }
    auto invisible=[&](size_t i){return i<hidden.size() && hidden[i];};
    for(size_t i=0;i<d.atoms.size();++i) if(scope[i] && atomicNumber(d,int(i))!=1) {
        if(invisible(i)) {++p.hidden;continue;}
        const int z=atomicNumber(d,int(i));
        double formal=options.setCharge?options.charge:property(d,"AtomX.FormalCharge",i,property(d,"FormalCharge",i,0));
        if(!std::isfinite(formal) || std::abs(formal)>4 || std::floor(formal)!=formal) {++p.ambiguous;continue;}
        const int charge=int(formal),target=valence(z,charge);
        if(target<0) {++p.unsupported;continue;}
        if(!finite(point(d,int(i)))) {++p.invalid;continue;}
        if(offsets[i+1]-offsets[i]>size_t(8)) {++p.invalid;continue;}
        std::vector<Vec3> fixed,existing;std::vector<int> terminal;
        double heavyOrder=0;bool invalid=false,periodic=false,hiddenH=false,aromatic=false,hasDouble=false,hasTriple=false;
        int doubles=0;
        Vec3 planeHint{};
        for(size_t k=offsets[i];k<offsets[i+1];++k) {
            const auto &b=d.bonds[edges[k]];const uint32_t other=b.a==i?b.b:b.a;
            if(b.image!=std::array<int32_t,3>{}){periodic=true;continue;}
            const Vec3 delta=sub(point(d,int(other)),point(d,int(i)));
            if(other==i || !finite(delta) || length(delta)<1e-6){invalid=true;continue;}
            if(atomicNumber(d,int(other))==1) {
                if(b.order!=1 || offsets[other+1]-offsets[other]!=1){invalid=true;continue;}
                if(invisible(other))hiddenH=true;
                terminal.push_back(int(other));existing.push_back(unit(delta));
            } else {
                fixed.push_back(unit(delta));heavyOrder+=b.order==4?1.5:b.order;
                aromatic|=b.order==4;hasDouble|=b.order==2;hasTriple|=b.order==3;doubles+=b.order==2;
                // Double-bond H directions use the neighbouring heavy-atom plane.
                if(b.order==2) for(size_t j=offsets[other];j<offsets[other+1];++j) {
                    const auto &next=d.bonds[edges[j]];const auto third=next.a==other?next.b:next.a;
                    if(third!=i && next.image==std::array<int32_t,3>{}) {
                        planeHint=sub(point(d,int(third)),point(d,int(other)));break;
                    }
                }
            }
        }
        if(periodic){++p.periodic;continue;}
        if(hiddenH){++p.hidden;continue;}
        if(invalid){++p.invalid;continue;}
        const double missing=target-heavyOrder;
        // Aromatic N has distinct pyridine/pyrrole chemistry not encoded by
        // numeric aromatic order alone. Require explicit charge/single-double topology.
        if((aromatic && z==7) || missing<0 || missing>4 || std::abs(missing-std::round(missing))>1e-6) {++p.ambiguous;continue;}
        const int desired=int(std::round(missing));
        double setting=property(d,"AtomX.Hybridization",i,0);
        if(!std::isfinite(setting) || setting<0 || setting>3 || std::floor(setting)!=setting){++p.ambiguous;continue;}
        const int automatic=hasTriple||doubles>=2?1:hasDouble||aromatic||(z==5 && charge==0)||(z==6 && charge==1)?2:3;
        const int requested=options.hybridization==4?0:options.hybridization;
        const int hybrid=options.hybridization?requested?requested:automatic:setting>0?int(setting):automatic;
        if(fixed.size()+size_t(desired)>size_t(hybrid==1?2:hybrid==2?3:4)) {++p.ambiguous;continue;}
        std::vector<Vec3> positions;
        try { if(desired)positions=slots(hybrid,fixed,existing,planeHint); }
        catch(const std::invalid_argument &){++p.invalid;continue;}
        const double bondLength=sketchBondLength(d,int(i),"H");
        if(!(bondLength>0) || !std::isfinite(bondLength)){++p.invalid;continue;}
        const int keep=std::min(desired,int(terminal.size()));
        // Greedy assignment is bounded by four H slots, and preserves atom identity.
        std::vector<uint8_t> used(positions.size());
        std::vector<Move> moves;std::vector<Addition> additions;
        bool badPosition=false;
        for(int h=0;h<keep;++h) {
            size_t best=positions.size();double score=-2;
            for(size_t s=0;s<positions.size();++s) if(!used[s] && dot(existing[size_t(h)],positions[s])>score) {best=s;score=dot(existing[size_t(h)],positions[s]);}
            if(best==positions.size()){badPosition=true;break;}used[best]=1;
            const Vec3 pos=add(point(d,int(i)),scale(positions[best],bondLength));
            if(!finite(pos)){badPosition=true;break;}
            if(!options.onlyAdd && length(sub(pos,point(d,terminal[size_t(h)])))>1e-4)
                moves.push_back({terminal[size_t(h)],pos,existing[size_t(h)],unit(positions[best])});
        }
        for(int h=int(terminal.size());h<desired;++h) {
            const size_t s=size_t(std::find(used.begin(),used.end(),uint8_t(0))-used.begin());
            if(s==used.size()){badPosition=true;break;}used[s]=1;
            const Vec3 pos=add(point(d,int(i)),scale(positions[s],bondLength));
            if(!finite(pos)){badPosition=true;break;}
            if(options.onlyAdd)for(int hydrogen:terminal)
                if(length(sub(pos,point(d,hydrogen)))<.35)badPosition=true;
            if(badPosition)break;
            additions.push_back({int(i),pos});
        }
        if(badPosition){++p.invalid;continue;}
        ++p.sites;
        p.moved.insert(p.moved.end(),moves.begin(),moves.end());p.added.insert(p.added.end(),additions.begin(),additions.end());
        if(!options.onlyAdd)for(size_t h=size_t(keep);h<terminal.size();++h)p.removed.push_back(terminal[h]);
        const bool changeCharge=options.setCharge && property(d,"AtomX.FormalCharge",i,property(d,"FormalCharge",i,0))!=options.charge;
        const bool changeHybrid=options.hybridization && setting!=requested;
        if(changeCharge||changeHybrid)p.settings.push_back({int(i),options.setCharge?options.charge:charge,options.hybridization?requested:int(setting)});
    }
    std::sort(p.removed.begin(),p.removed.end());
    if(d.atoms.size()-p.removed.size()+p.added.size()>size_t(INT32_MAX) ||
       (p.added.size()>p.removed.size() && d.bonds.size()-p.removed.size()+p.added.size()>interactiveBondBudget))
        throw std::invalid_argument("调整后的原子数或键数超过交互上限；请缩小选择范围");
    return p;
}
inline void preflight(const Dataset &d,const Plan &p,const std::vector<uint8_t> &hidden={}) {
    validateRows(d);
    if(d.atoms.size()!=p.atomCount || d.bonds.size()!=p.bondCount || fingerprint(d)!=p.fingerprint || visibilityHash(hidden)!=p.visibility)
        throw std::invalid_argument("体系、键级或可见性已改变，请重新计算预览");
}
inline std::vector<int> apply(Dataset &d,const Plan &p) {
    using namespace authoring;
    // Visibility is checked by the UI before its history entry is recorded.
    if(d.atoms.size()!=p.atomCount || d.bonds.size()!=p.bondCount || fingerprint(d)!=p.fingerprint)
        throw std::invalid_argument("体系已改变，请重新计算预览");
    validateRows(d);
    for(const auto &s:p.settings) {
        auto &charges=d.scalarProperties["AtomX.FormalCharge"],&hybrids=d.scalarProperties["AtomX.Hybridization"];
        if(charges.empty()) {charges.resize(d.atoms.size());for(size_t i=0;i<charges.size();++i)charges[i]=property(d,"FormalCharge",i,0);}
        if(hybrids.empty())hybrids.resize(d.atoms.size());
        charges[size_t(s.index)]=s.charge;hybrids[size_t(s.index)]=s.hybridization;
    }
    for(const auto &m:p.moved) {
        auto &a=d.atoms[size_t(m.index)];a.x=m.position.x;a.y=m.position.y;a.z=m.position.z;
        for(auto &[name,rows]:d.vectorProperties) { if(spatialVectorProperty(name) && finite(rows[size_t(m.index)]))rows[size_t(m.index)]=rotatedVector(rows[size_t(m.index)],m.from,m.to); }
    }
    auto remap=[&](int i){return i-int(std::lower_bound(p.removed.begin(),p.removed.end(),i)-p.removed.begin());};
    if(!p.removed.empty())eraseAtoms(d,p.removed);
    std::vector<int> changed;for(const auto &m:p.moved)changed.push_back(remap(m.index));
    if(!p.added.empty()) {
        const uint32_t type=speciesIndex(d,"H");const size_t old=d.atoms.size(),count=old+p.added.size();
        d.atoms.reserve(count);d.bonds.reserve(d.bonds.size()+p.added.size());
        for(const auto &a:p.added) {
            const int index=int(d.atoms.size());d.atoms.push_back({a.position.x,a.position.y,a.position.z,type});
            d.bonds.push_back({uint32_t(remap(a.parent)),uint32_t(index),{},1});changed.push_back(index);
        }
        for(auto &[name,rows]:d.scalarProperties) rows.resize(count,constraintScalarDefault(name,
            name=="AtomX.MotionGroup"||name=="AtomX.FormalCharge"||name=="AtomX.Hybridization"?0:NAN));
        for(auto &[name,rows]:d.vectorProperties)rows.resize(count,constraintVectorDefault(name));
        if(!d.particleColors.empty())d.particleColors.resize(count,{-1,-1,-1});
    }
    d.sourceCount=d.atoms.size();d.bounds();return changed;
}
} // namespace atomx::hydrogens
