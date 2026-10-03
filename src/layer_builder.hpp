#pragma once
#include "motion_groups.hpp"
#include "elements.hpp"
#include <numeric>

// Layer cleave and lattice matching, based on the installed MS Layer
// Builder help. Atom/bond work is linear or disjoint-set near-linear, executed
// only on Build. The default atomic path keeps its compact image buffer.
namespace atomx::layers {
struct V {
    double x=0,y=0,z=0;
    V operator+(V b) const { return {x+b.x,y+b.y,z+b.z}; }
    V operator-(V b) const { return {x-b.x,y-b.y,z-b.z}; }
    V operator*(double s) const { return {x*s,y*s,z*s}; }
};
inline double dot(V a,V b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline V cross(V a,V b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
inline double length(V a) { return std::sqrt(dot(a,a)); }
inline bool finite(V v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
inline V vector(Vec3 v) { return {v.x,v.y,v.z}; }
inline Vec3 compact(V v) {
    if (!finite(v) || std::max({std::abs(v.x),std::abs(v.y),std::abs(v.z)})>std::numeric_limits<float>::max())
        throw std::invalid_argument("叠层坐标超出范围");
    return {float(v.x),float(v.y),float(v.z)};
}
struct Frame {
    V a,b,c,x,y,n;
    double al=0,bl=0,gamma=0,area=0,height=0,cu=0,cv=0;
};
inline Frame frame(const Dataset &d) {
    Frame f;
    f.a={d.cell[0],d.cell[1],d.cell[2]}; f.b={d.cell[3],d.cell[4],d.cell[5]}; f.c={d.cell[6],d.cell[7],d.cell[8]};
    f.al=length(f.a); f.bl=length(f.b); f.area=length(cross(f.a,f.b));
    if (!finite(f.a)||!finite(f.b)||!finite(f.c)||!finite(vector(d.origin)) ||
        f.al<1e-6 || f.bl<1e-6 || f.area<1e-8 || !d.pbc[0] || !d.pbc[1])
        throw std::invalid_argument("层来源需要有效的 a、b 周期晶格");
    f.x=f.a*(1/f.al); f.n=cross(f.a,f.b)*(1/f.area); f.y=cross(f.n,f.x);
    f.height=dot(f.c,f.n);
    if (f.height<1e-6) throw std::invalid_argument("层来源需要右手晶胞和正的法向高度");
    f.gamma=std::acos(std::clamp(dot(f.a,f.b)/(f.al*f.bl),-1.0,1.0))*180/authoring::kPi;
    f.cv=dot(f.c,f.y)/dot(f.b,f.y); f.cu=(dot(f.c,f.x)-f.cv*dot(f.b,f.x))/f.al;
    return f;
}
enum class Cleave { Atomic, Molecular };
enum class Flip { No, A, B };
struct Detail {
    std::string name; double gap=3,offsetA=0,offsetB=0;
    Cleave cleave=Cleave::Atomic; Flip flip=Flip::No;
};
inline std::array<int,3> signs(Flip flip) {
    if (flip==Flip::No) return {1,1,1};
    if (flip==Flip::A) return {1,-1,-1};
    if (flip==Flip::B) return {-1,1,-1};
    throw std::invalid_argument("层翻转选项无效");
}
inline Frame flipped(Frame f,Flip flip) {
    if (flip==Flip::No) return f;
    const auto s=signs(flip);
    f.a=f.a*s[0]; f.b=f.b*s[1]; f.c=f.c*s[2];
    f.x=f.x*s[0]; f.n=f.n*(-1); f.y=cross(f.n,f.x);
    f.gamma=180-f.gamma;
    f.cu*=s[2]*s[0]; f.cv*=s[2]*s[1];
    return f;
}
struct Options {
    int matching=-1; // -1 = arithmetic average of a,b,gamma; otherwise layer index.
    int orientation=0;
    bool constantVolume=true,surface=false;
    double warningPercent=5;
    size_t atomLimit=2000000;
};
struct Match {
    V a,b,n;
    double al=0,bl=0,gamma=0,area=0;
    std::vector<double> mismatch;
};
inline Match match(const std::vector<const Dataset *> &inputs,const Options &o,const std::vector<Detail> &details={}) {
    if (inputs.size()<2 || inputs.size()>3 || o.matching<-1 || o.matching>=int(inputs.size()) ||
        o.orientation<0 || o.orientation>=int(inputs.size()) || !std::isfinite(o.warningPercent) || o.warningPercent<0)
        throw std::invalid_argument("请选择两至三层和有效的晶格匹配 / 朝向");
    if (!details.empty() && details.size()!=inputs.size()) throw std::invalid_argument("层参数数目不匹配");
    std::vector<Frame> frames; for (size_t i=0;i<inputs.size();++i) {
        const auto *d=inputs[i];
        if (!d || d->atoms.empty() || d->sampled()) throw std::invalid_argument("请使用完整、非空的周期体系");
        frames.push_back(flipped(frame(*d),details.empty()?Flip::No:details[i].flip));
    }
    Match m;
    for (size_t i=0;i<frames.size();++i) if (o.matching<0 || o.matching==int(i)) {
        m.al+=frames[i].al; m.bl+=frames[i].bl; m.gamma+=frames[i].gamma;
    }
    if (o.matching<0) { m.al/=frames.size(); m.bl/=frames.size(); m.gamma/=frames.size(); }
    const auto orient=frame(*inputs[size_t(o.orientation)]); const double angle=m.gamma*authoring::kPi/180;
    m.a=orient.x*m.al; m.b=orient.x*(m.bl*std::cos(angle))+orient.y*(m.bl*std::sin(angle));
    m.n=orient.n; m.area=m.al*m.bl*std::sin(angle);
    if (!std::isfinite(m.area)||m.area<1e-8) throw std::invalid_argument("匹配后的面内晶格退化");
    for (const auto &f:frames) {
        const double oldAngle=f.gamma*authoring::kPi/180;
        const double db=std::hypot(m.bl*std::cos(angle)-f.bl*std::cos(oldAngle),
                                  m.bl*std::sin(angle)-f.bl*std::sin(oldAngle))/f.bl;
        m.mismatch.push_back(100*std::max(std::abs(m.al/f.al-1),db));
    }
    return m;
}
using Image=std::array<int64_t,3>;
// Unwrap finite explicit-bond components with a weighted disjoint set. Storage
// is O(N), including dense bond networks: never duplicate the entire bond list.
// A nonzero winding cycle is an infinite network, not a finite molecule.
inline std::vector<Image> molecularImages(const Dataset &d,const Frame &f,Flip flip) {
    std::vector<uint32_t> parents(d.atoms.size()); std::iota(parents.begin(),parents.end(),0);
    std::vector<uint8_t> rank(d.atoms.size()); std::vector<Image> images(d.atoms.size());
    auto root=[&](auto &&self,uint32_t i)->uint32_t {
        if (parents[i]==i) return i;
        const auto p=parents[i],r=self(self,p);
        for (int axis=0;axis<3;++axis) {
            images[i][axis]+=images[p][axis];
            if (std::abs(images[i][axis])>100000000) throw std::invalid_argument("分子周期像超出范围");
        }
        return parents[i]=r;
    };
    for (const auto &b:d.bonds) {
        if (b.a>=d.atoms.size() || b.b>=d.atoms.size()) throw std::invalid_argument("层来源键索引无效");
        const auto a=root(root,b.a),bb=root(root,b.b); Image link{};
        for (int axis=0;axis<3;++axis) {
            link[axis]=int64_t(b.image[axis])+images[b.a][axis]-images[b.b][axis];
            if (std::abs(link[axis])>100000000) throw std::invalid_argument("分子周期像超出范围");
        }
        if (a==bb) {
            if (link!=Image{}) throw std::invalid_argument("完整分子切割遇到无限周期键网络；请改用原子切割");
        } else if (rank[a]<rank[bb]) {
            parents[a]=bb; for (int axis=0;axis<3;++axis) images[a][axis]=-link[axis];
        } else {
            parents[bb]=a; images[bb]=link; if (rank[a]==rank[bb]) ++rank[a];
        }
    }
    const auto mass=d.scalarProperties.find("Mass");
    if (mass!=d.scalarProperties.end() && mass->second.size()!=d.atoms.size())
        throw std::invalid_argument("Mass 属性长度不匹配");
    std::vector<double> weights(d.species.size());
    for (size_t t=0;t<weights.size();++t) if (const auto *e=elements::find(d.species[t])) weights[t]=elements::atomicMass(*e);
    std::vector<V> centers(d.atoms.size()); std::vector<double> totals(d.atoms.size());
    const auto s=signs(flip);
    for (size_t i=0;i<d.atoms.size();++i) {
        const auto r=root(root,uint32_t(i)); const auto &atom=d.atoms[i];
        if (atom.type>=weights.size()) throw std::invalid_argument("层来源元素索引无效");
        const double w=mass==d.scalarProperties.end()?weights[atom.type]:mass->second[i];
        if (!std::isfinite(w) || w<=0) throw std::invalid_argument("完整分子需要有效原子质量；请提供正的 Mass 属性或改用原子切割");
        const auto &im=images[i]; const V p=V{atom.x,atom.y,atom.z}-vector(d.origin)+f.a*double(im[0])+f.b*double(im[1])+f.c*double(im[2]);
        if (!finite(p)) throw std::invalid_argument("层来源原子坐标无效");
        centers[r]=centers[r]+p*w; totals[r]+=w;
    }
    for (size_t r=0;r<d.atoms.size();++r) if (parents[r]==r) {
        const V center=centers[r]*(1/totals[r]); const double fc=dot(center,f.n)/f.height;
        const V plane=center-f.c*fc; const double fb=dot(plane,f.y)/dot(f.b,f.y),fa=(dot(plane,f.x)-fb*dot(f.b,f.x))/f.al;
        const double frac[3]={fa,fb,fc}; double wrap[3]{};
        for (int axis=0;axis<3;++axis) {
            const double v=std::floor(frac[axis]*s[axis]);
            if (!std::isfinite(v) || std::abs(v)>100000000) throw std::invalid_argument("分子质心离晶胞过远");
            wrap[axis]=v*s[axis];
        }
        centers[r]={wrap[0],wrap[1],wrap[2]};
    }
    for (size_t i=0;i<d.atoms.size();++i) {
        const auto wrap=centers[parents[i]];
        images[i][0]-=int64_t(wrap.x); images[i][1]-=int64_t(wrap.y); images[i][2]-=int64_t(wrap.z);
    }
    return images;
}
struct Built { Dataset data; size_t cutBonds=0; std::vector<double> mismatch; };
inline Built build(const std::vector<const Dataset *> &inputs,const std::vector<Detail> &details,const Options &o) {
    const auto m=match(inputs,o,details);
    if (details.size()!=inputs.size()) throw std::invalid_argument("层参数数目不匹配");
    size_t count=0;
    for (size_t i=0;i<inputs.size();++i) {
        const auto &detail=details[i];
        if ((detail.cleave!=Cleave::Atomic && detail.cleave!=Cleave::Molecular) || detail.name.empty() || detail.name.size()>256 || !std::isfinite(detail.gap) || detail.gap<0 ||
            !std::isfinite(detail.offsetA) || !std::isfinite(detail.offsetB) ||
            std::abs(detail.offsetA)>1000000 || std::abs(detail.offsetB)>1000000)
            throw std::invalid_argument("层名称、非负真空或面内偏移无效");
        if (inputs[i]->atoms.size()>o.atomLimit || count>o.atomLimit-inputs[i]->atoms.size())
            throw std::invalid_argument("叠层原子数超过当前完整载入上限");
        count+=inputs[i]->atoms.size();
    }
    if (count>UINT32_MAX) throw std::invalid_argument("叠层原子数超出索引范围");
    Built result; result.mismatch=m.mismatch; auto &out=result.data;
    out.atoms.reserve(count); out.pbc={true,true,!o.surface}; out.bondStyle=inputs[0]->bondStyle;
    const double nan=std::numeric_limits<double>::quiet_NaN();
    const float fnan=std::numeric_limits<float>::quiet_NaN();
    bool allColors=true;
    for (const auto *d:inputs) {
        allColors=allColors && d->particleColors.size()==d->atoms.size();
        for (const auto &[name,values]:d->scalarProperties) {
            if (values.size()!=d->atoms.size()) throw std::invalid_argument("层来源的原子属性长度不匹配");
            if (name!="AtomX.Layer" && name!=motion::property) out.scalarProperties.try_emplace(name,count,nan);
        }
        for (const auto &[name,values]:d->vectorProperties) {
            if (values.size()!=d->atoms.size()) throw std::invalid_argument("层来源的矢量属性长度不匹配");
            out.vectorProperties.try_emplace(name,count,Vec3{fnan,fnan,fnan});
        }
    }
    // Values retain their source semantics. The per-layer source/target basis
    // table makes the vector reference frames explicit; no force/velocity units
    // or strain transformation is guessed for arbitrary property names.
    auto &layerIds=out.scalarProperties["AtomX.Layer"]; layerIds.resize(count);
    DataTable layerTable{"AtomX.Layers",{"ID","Name","FirstAtom","Count","GapAbove","OffsetA","OffsetB","Height","MismatchPercent","Cleave","Flip"},{}};
    DataTable basisTable{"AtomX.LayerBases",{"ID","SourceA","SourceB","SourceC","TargetA","TargetB","TargetC"},{}};
    auto formatVector=[](V v) { return formatDataNumber(v.x)+" "+formatDataNumber(v.y)+" "+formatDataNumber(v.z); };
    std::map<int,std::string> groupNames; int nextGroup=1; V base{}; size_t start=0;
    for (size_t li=0;li<inputs.size();++li) {
        const auto &d=*inputs[li]; const auto &detail=details[li]; const auto original=frame(d),f=flipped(original,detail.flip);
        const bool crystal=d.pbc[2];
        const bool molecular=crystal && detail.cleave==Cleave::Molecular;
        const bool envelope=molecular || !crystal;
        const auto s=signs(detail.flip);
        const double heightScale=crystal && o.constantVolume?f.area/m.area:1;
        const V targetC=m.a*f.cu+m.b*f.cv+m.n*(f.height*heightScale);
        double minHeight=std::numeric_limits<double>::infinity(),maxHeight=-minHeight;
        const auto molecule=molecular?molecularImages(d,original,detail.flip):std::vector<Image>{};
        std::vector<std::array<int32_t,3>> shifts(d.atoms.size());
        std::vector<V> points(envelope?d.atoms.size():0);
        std::vector<double> radii(d.species.size());
        if (molecular) for (size_t t=0;t<radii.size();++t) {
            const auto *e=elements::find(d.species[t]);
            if (e) radii[t]=e->vdw;
        }
        std::vector<uint32_t> types(d.species.size());
        for (size_t t=0;t<types.size();++t) types[t]=authoring::speciesIndex(out,d.species[t]);
        for (size_t i=0;i<d.atoms.size();++i) {
            const auto &atom=d.atoms[i];
            if (atom.type>=types.size()) throw std::invalid_argument("层来源元素索引无效");
            if (molecular && radii[atom.type]<=0) throw std::invalid_argument("完整分子切割缺少元素的范德华半径；请改用原子切割");
            V p=V{atom.x,atom.y,atom.z}-vector(d.origin);
            if (molecular) { const auto &im=molecule[i]; p=p+original.a*double(im[0])+original.b*double(im[1])+original.c*double(im[2]); }
            if (!finite(p)) throw std::invalid_argument("层来源原子坐标无效");
            const double fc=dot(p,f.n)/f.height;
            const V plane=crystal?p-f.c*fc:p;
            double fb=dot(plane,f.y)/dot(f.b,f.y),fa=(dot(plane,f.x)-fb*dot(f.b,f.x))/f.al;
            fa+=detail.offsetA; fb+=detail.offsetB;
            const double wa=molecular?0:std::floor(fa),wb=molecular?0:std::floor(fb),wc=crystal && !molecular?std::floor(fc):0;
            if (std::max({std::abs(wa),std::abs(wb),std::abs(wc)})>100000000)
                throw std::invalid_argument("层来源坐标离晶胞过远");
            shifts[i]={int32_t(wa),int32_t(wb),int32_t(wc)};
            if (molecular) for (int axis=0;axis<3;++axis) {
                const int64_t shift=int64_t(shifts[i][axis])-molecule[i][axis]*s[axis];
                if (std::abs(shift)>INT32_MAX) throw std::invalid_argument("分子周期像超出范围");
                shifts[i][axis]=int32_t(shift);
            }
            const V point=m.a*(fa-wa)+m.b*(fb-wb)+(crystal?targetC*(fc-wc):m.n*dot(p,f.n));
            if (envelope) {
                points[i]=point;
                const double h=dot(point,m.n),radius=molecular?radii[atom.type]:0;
                minHeight=std::min(minHeight,h-radius); maxHeight=std::max(maxHeight,h+radius);
            } else {
                const auto at=compact(point+base); out.atoms.push_back({at.x,at.y,at.z,types[atom.type]});
            }
            layerIds[start+i]=double(li+1);
        }
        const double height=envelope?maxHeight-minHeight:f.height*heightScale;
        const V advance=crystal?targetC*(height/(f.height*heightScale)):m.n*height;
        const V translation=base-(envelope?(crystal?targetC*(minHeight/(f.height*heightScale)):m.n*minHeight):V{});
        if (envelope) for (size_t i=0;i<d.atoms.size();++i) {
            const auto &atom=d.atoms[i]; const auto at=compact(points[i]+translation);
            out.atoms.push_back({at.x,at.y,at.z,types[atom.type]});
        }
        for (const auto &b:d.bonds) {
            if (b.a>=d.atoms.size() || b.b>=d.atoms.size()) throw std::invalid_argument("层来源键索引无效");
            Bond next{uint32_t(start+b.a),uint32_t(start+b.b),{},b.order};
            for (int axis=0;axis<3;++axis) {
                const int64_t image=int64_t(b.image[size_t(axis)])*s[axis]+shifts[b.b][size_t(axis)]-shifts[b.a][size_t(axis)];
                if (std::abs(image)>INT32_MAX) throw std::invalid_argument("层来源键周期像超出范围");
                next.image[size_t(axis)]=int32_t(image);
            }
            if (next.image[2]) { ++result.cutBonds; continue; } // Each source is a cleaved slab.
            out.bonds.push_back(next);
        }
        for (const auto &[name,values]:d.scalarProperties) if (name!="AtomX.Layer" && name!=motion::property)
            std::copy(values.begin(),values.end(),out.scalarProperties.at(name).begin()+start);
        for (const auto &[name,values]:d.vectorProperties)
            std::copy(values.begin(),values.end(),out.vectorProperties.at(name).begin()+start);
        for (const auto &[name,component]:d.propertyComponents) {
            const auto existing=out.propertyComponents.find(name);
            if (existing!=out.propertyComponents.end() && existing->second!=component)
                throw std::invalid_argument("不同层的属性分量定义不一致");
            out.propertyComponents[name]=component;
        }
        if (allColors) out.particleColors.insert(out.particleColors.end(),d.particleColors.begin(),d.particleColors.end());
        std::map<int,int> groupMap;
        for (const auto &group:motion::catalog(d)) {
            if (groupNames.size()>=motion::groupLimit) throw std::invalid_argument("合并后的运动分组超过 512 组");
            groupMap[group.id]=nextGroup; groupNames[nextGroup++]=detail.name+" / "+group.name;
            if (groupNames.rbegin()->second.size()>256) throw std::invalid_argument("合并后的分组名称过长");
        }
        if (const auto *values=motion::memberships(d)) {
            auto &destination=out.scalarProperties[motion::property]; if (destination.empty()) destination.resize(count,0);
            for (size_t i=0;i<values->size();++i) if (const int key=motion::id((*values)[i])) destination[start+i]=groupMap.at(key);
        }
        const std::string prefix="Layer"+std::to_string(li+1)+".";
        for (const auto &[name,value]:d.globalAttributes) out.globalAttributes[prefix+name]=value;
        for (auto table:d.tables) if (table.name!=motion::tableName) { table.name=prefix+table.name; out.tables.push_back(std::move(table)); }
        layerTable.rows.push_back({std::to_string(li+1),detail.name,std::to_string(start),std::to_string(d.atoms.size()),
            formatDataNumber(o.surface && li+1==inputs.size()?0:detail.gap),formatDataNumber(detail.offsetA),
            formatDataNumber(detail.offsetB),formatDataNumber(height),formatDataNumber(m.mismatch[li]),
            molecular?"Molecular":"Atomic",detail.flip==Flip::No?"No":detail.flip==Flip::A?"A":"B"});
        basisTable.rows.push_back({std::to_string(li+1),formatVector(original.a),formatVector(original.b),formatVector(original.c),
            formatVector(m.a*s[0]),formatVector(m.b*s[1]),formatVector(targetC*s[2])});
        const double gap=o.surface && li+1==inputs.size()?0:detail.gap;
        base=base+(o.surface || !crystal?m.n*height:advance)+m.n*gap; start+=d.atoms.size();
    }
    if (dot(base,m.n)<1e-6) throw std::invalid_argument("总法向厚度为零，请增加层间真空");
    out.cell={m.a.x,m.a.y,m.a.z,m.b.x,m.b.y,m.b.z,base.x,base.y,base.z};
    motion::setNames(out,groupNames); out.tables.push_back(std::move(layerTable)); out.tables.push_back(std::move(basisTable));
    out.sourceCount=out.atoms.size(); out.bounds(); out.comment="Layer Builder · "+std::to_string(inputs.size())+" layers · "+(o.surface?"2D surface":"P1 crystal");
    out.globalAttributes["AtomX.LayerCount"]=double(inputs.size());
    out.globalAttributes["AtomX.LayerCutBonds"]=double(result.cutBonds);
    return result;
}
} // namespace atomx::layers
