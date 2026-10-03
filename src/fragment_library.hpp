#pragma once
#include "authoring.hpp"
#include "creation_document.hpp"

// Small, explicit molecular templates. Geometry is generated here, not copied
// from any third-party fragment files; preview never clones the user's system.
namespace atomx::fragments {
inline constexpr size_t atomLimit=512, bondLimit=2048;
struct Template {
    std::string key,name,category;
    Dataset data;
    int connector=1; // A terminal leaving atom; omitted when attached.
};
inline Vec3 at(const Dataset &d,int index) {
    const auto &a=d.atoms.at(size_t(index)); return {a.x,a.y,a.z};
}
inline int terminalNeighbor(const Dataset &d,int index) {
    if (index<0 || size_t(index)>=d.atoms.size()) return -1;
    int neighbor=-1,degree=0;
    for (const auto &b:d.bonds) if (b.a==uint32_t(index) || b.b==uint32_t(index)) {
        if (b.image!=std::array<int32_t,3>{}) return -1;
        neighbor=int(b.a==uint32_t(index)?b.b:b.a); ++degree;
    }
    return degree==1?neighbor:-1;
}
inline void validate(const Template &t) {
    if (t.name.empty() || t.name.size()>256 || t.category.size()>128 ||
        t.data.atoms.size()<2 || t.data.atoms.size()>atomLimit || t.data.bonds.size()>bondLimit ||
        terminalNeighbor(t.data,t.connector)<0)
        throw std::invalid_argument("片段需要 2–512 个原子和一个末端连接点");
    document::validate(t.data,{});
    for (const auto &[name,rows]:t.data.scalarProperties) { (void)name;
        if (rows.size()!=t.data.atoms.size()) throw std::invalid_argument("片段标量属性长度不匹配");
    }
    for (const auto &[name,rows]:t.data.vectorProperties) { (void)name;
        if (rows.size()!=t.data.atoms.size()) throw std::invalid_argument("片段向量属性长度不匹配");
    }
    std::set<std::pair<uint32_t,uint32_t>> edges;
    for (const auto &b:t.data.bonds) {
        if (b.a==b.b || b.image!=std::array<int32_t,3>{} ||
            !edges.emplace(std::min(b.a,b.b),std::max(b.a,b.b)).second ||
            authoring::length(authoring::sub(at(t.data,int(b.a)),at(t.data,int(b.b))))<1e-5)
            throw std::invalid_argument("片段需要有效的直接键，不支持周期连通网络");
    }
    if (authoring::fragment(t.data,t.connector).size()!=t.data.atoms.size())
        throw std::invalid_argument("片段原子必须通过显式键连接");
    for (const auto &s:t.data.species) if (!elements::find(s))
        throw std::invalid_argument("片段包含未知元素");
}
inline std::vector<Template> builtins() {
    using namespace authoring;
    std::vector<Template> library;
    auto make=[&](const char *key,const char *name,const char *category,const char *center) -> Template & {
        Template t; t.key=std::string("builtin/")+key; t.name=name; t.category=category;
        t.data.species={center,"H"}; t.data.atoms={{0,0,0,0}};
        library.push_back(std::move(t)); return library.back();
    };
    auto atom=[](Template &t,Vec3 p,const char *symbol,int parent,int order=1) {
        const int index=int(t.data.atoms.size());
        t.data.atoms.push_back({p.x,p.y,p.z,speciesIndex(t.data,symbol)});
        if (parent>=0) t.data.bonds.push_back({uint32_t(parent),uint32_t(index),{},uint8_t(order)});
        return index;
    };
    const Vec3 tetra[4]={{-1,0,0},{1.f/3,.942809f,0},{1.f/3,-.471405f,.816497f},{1.f/3,-.471405f,-.816497f}};
    auto &methyl=make("methyl","甲基 / Methyl","烃基","C");
    for (auto v:tetra) atom(methyl,scale(v,1.09),"H",0);
    auto &ethyl=make("ethyl","乙基 / Ethyl","烃基","C");
    atom(ethyl,scale(tetra[0],1.09),"H",0);
    atom(ethyl,scale(tetra[2],1.09),"H",0); atom(ethyl,scale(tetra[3],1.09),"H",0);
    const Vec3 c2=scale(tetra[1],1.52); const int next=atom(ethyl,c2,"C",0);
    const Vec3 direction=scale(tetra[1],-1),u{0,0,1},v=cross(direction,u);
    for (int i=0;i<3;++i) atom(ethyl,add(c2,scale(add(scale(direction,-1.0/3),
        add(scale(u,.942809*std::cos(i*2*kPi/3)),scale(v,.942809*std::sin(i*2*kPi/3)))),1.09)),"H",next);
    auto &oh=make("hydroxyl","羟基 / Hydroxyl","官能团","O");
    atom(oh,{-0.96f,0,0},"H",0); atom(oh,{.240f,.9295f,0},"H",0);
    auto &nh=make("amino","氨基 / Amino","官能团","N");
    atom(nh,{-1.01f,0,0},"H",0); atom(nh,{.292f,.967f,0},"H",0); atom(nh,{.292f,-.397f,.884f},"H",0);
    auto &formyl=make("formyl","甲酰基 / Formyl","官能团","C");
    atom(formyl,{-1.09f,0,0},"H",0); atom(formyl,{.605f,1.048f,0},"O",0,2); atom(formyl,{.545f,-.944f,0},"H",0);
    auto &carboxyl=make("carboxyl","羧基 / Carboxyl","官能团","C");
    atom(carboxyl,{-1.09f,0,0},"H",0); atom(carboxyl,{.605f,1.048f,0},"O",0,2);
    const int oxygen=atom(carboxyl,{.665f,-1.152f,0},"O",0); atom(carboxyl,{1.625f,-1.152f,0},"H",oxygen);
    auto &phenyl=make("phenyl","苯基 / Phenyl","环","C");
    for (int i=1;i<6;++i) atom(phenyl,{float(1.40*(1-std::cos(i*kPi/3))),float(1.40*std::sin(i*kPi/3)),0},"C",-1);
    for (int i=0;i<6;++i) {
        phenyl.data.bonds.push_back({uint32_t(i),uint32_t((i+1)%6),{},4});
        atom(phenyl,{float(1.40-2.49*std::cos(i*kPi/3)),float(2.49*std::sin(i*kPi/3)),0},"H",i);
    }
    phenyl.connector=6;
    for (const char *symbol:{"F","Cl","Br","I"}) {
        auto &halogen=make(symbol,symbol,"卤素",symbol);
        atom(halogen,{float(-(elements::find(symbol)->covalent+.31)),0,0},"H",0);
    }
    for (auto &t:library) {
        t.data.sourceCount=t.data.atoms.size(); t.data.bounds(); t.data.bondStyle.radius=.12f;
        t.data.bondStyle.colorByType=true; validate(t);
    }
    return library;
}
inline Template define(const Dataset &source,int connector,std::string name,std::string category) {
    if (terminalNeighbor(source,connector)<0) throw std::invalid_argument("请选中一个末端原子作为连接点");
    auto selected=authoring::fragment(source,connector);
    if (selected.size()>atomLimit) throw std::invalid_argument("自定义片段最多 512 个原子");
    std::sort(selected.begin(),selected.end());
    Template t; t.name=std::move(name); t.category=std::move(category); t.data.species=source.species;
    t.data.bondStyle=source.bondStyle;
    std::unordered_map<int,int> remap;
    const Vec3 origin=at(source,terminalNeighbor(source,connector));
    for (int i:selected) {
        remap[i]=int(t.data.atoms.size()); auto a=source.atoms[size_t(i)];
        const auto p=authoring::sub({a.x,a.y,a.z},origin); a.x=p.x; a.y=p.y; a.z=p.z;
        t.data.atoms.push_back(a);
        if (source.particleColors.size()==source.atoms.size()) t.data.particleColors.push_back(source.particleColors[size_t(i)]);
    }
    for (auto b:source.bonds) if (remap.contains(int(b.a)) && remap.contains(int(b.b))) {
        b.a=uint32_t(remap.at(int(b.a))); b.b=uint32_t(remap.at(int(b.b))); t.data.bonds.push_back(b);
    }
    for (const auto &[nameIn,rows]:source.scalarProperties) if (nameIn!="AtomX.MotionGroup" && rows.size()==source.atoms.size()) {
        auto &out=t.data.scalarProperties[nameIn]; for (int i:selected) out.push_back(rows[size_t(i)]);
    }
    for (const auto &[nameIn,rows]:source.vectorProperties) if (rows.size()==source.atoms.size()) {
        auto &out=t.data.vectorProperties[nameIn]; for (int i:selected) out.push_back(rows[size_t(i)]);
    }
    t.data.propertyComponents=source.propertyComponents;
    t.connector=remap.at(connector); t.data.sourceCount=t.data.atoms.size(); t.data.bounds(); validate(t);
    return t;
}
struct Placement {
    std::vector<Vec3> points;
    int anchor=-1,removed=-1,connector=-1,root=-1;
    Vec3 pivot{},axis{},anchorPosition{},removedPosition{};
    std::array<Vec3,3> vectorAxes{};
};
inline Placement place(const Dataset &source,const Template &t,int connector,Vec3 position,Vec3 right,Vec3 normal,int hit=-1) {
    using namespace authoring;
    const int root=terminalNeighbor(t.data,connector);
    if (root<0 || length(right)<1e-6 || length(normal)<1e-6) throw std::invalid_argument("片段连接点或绘制平面无效");
    Placement p; p.root=root; p.connector=connector;
    Vec3 direction=scale(right,1/length(right));
    double bondLength=length(sub(at(t.data,root),at(t.data,connector)));
    if (hit>=0) {
        if (size_t(hit)>=source.atoms.size()) throw std::invalid_argument("接枝原子已改变");
        int degree=0,neighbor=-1;
        for (const auto &b:source.bonds) if (b.a==uint32_t(hit) || b.b==uint32_t(hit)) {
            if (b.image!=std::array<int32_t,3>{}) throw std::invalid_argument("不能在周期边界键上接枝");
            ++degree; neighbor=int(b.a==uint32_t(hit)?b.b:b.a);
        }
        if (degree>1) throw std::invalid_argument("请点击孤立原子或末端原子接枝");
        p.anchor=hit; position=at(source,hit);
        const auto type=source.atoms[size_t(hit)].type;
        if (degree==1 && type<source.species.size() && source.species[type]=="H") {
            p.removed=hit; p.removedPosition=position; p.anchor=neighbor; position=at(source,neighbor);
            direction=sub(at(source,hit),position);
        } else if (neighbor>=0) direction=sub(position,at(source,neighbor));
        if (length(direction)<1e-6) throw std::invalid_argument("接枝方向无效");
        direction=scale(direction,1/length(direction));
        p.anchorPosition=position;
        bondLength=sketchBondLength(source,p.anchor,t.data.species[t.data.atoms[size_t(root)].type]);
        position=add(position,scale(direction,bondLength));
    }
    Vec3 u=cross(normal,direction);
    if (length(u)<1e-6) u=cross(std::abs(direction.z)<.8?Vec3{0,0,1}:Vec3{0,1,0},direction);
    u=scale(u,1/length(u)); const Vec3 v=cross(direction,u);
    const Vec3 localOrigin=at(t.data,root);
    Vec3 localX=sub(localOrigin,at(t.data,connector)); localX=scale(localX,1/length(localX));
    Vec3 localY=cross(std::abs(localX.z)<.8?Vec3{0,0,1}:Vec3{0,1,0},localX); localY=scale(localY,1/length(localY));
    const Vec3 localZ=cross(localX,localY);
    for (int j=0;j<3;++j) {
        const Vec3 unit=j==0?Vec3{1,0,0}:j==1?Vec3{0,1,0}:Vec3{0,0,1};
        p.vectorAxes[size_t(j)]=add(scale(direction,dot(unit,localX)),add(scale(u,dot(unit,localY)),scale(v,dot(unit,localZ))));
    }
    for (const auto &a:t.data.atoms) {
        const Vec3 d=sub({a.x,a.y,a.z},localOrigin);
        p.points.push_back(add(position,add(scale(direction,dot(d,localX)),add(scale(u,dot(d,localY)),scale(v,dot(d,localZ))))));
    }
    p.pivot=position; p.axis=hit>=0?direction:normal;
    return p;
}
inline Placement rotated(Placement p,double radians) {
    for (auto &v:p.points) v=authoring::rotatedPoint(v,p.pivot,p.axis,radians);
    for (auto &v:p.vectorAxes) v=authoring::rotatedPoint(v,{},p.axis,radians);
    return p;
}
inline void preflight(const Dataset &source,const Template &t,const Placement &p,const std::vector<uint8_t> &hidden={}) {
    validate(t);
    if (p.points.size()!=t.data.atoms.size() || p.root<0 || p.root!=terminalNeighbor(t.data,p.connector) ||
        (p.removed>=0 && p.anchor<0) ||
        source.atoms.size()>size_t(INT32_MAX)-p.points.size()) throw std::invalid_argument("片段放置状态已改变");
    for (auto v:p.points) if (!document::finite(v)) throw std::invalid_argument("片段坐标无效");
    for (int index:{p.anchor,p.removed}) if (index>=0 &&
        (size_t(index)>=source.atoms.size() || (size_t(index)<hidden.size() && hidden[size_t(index)])))
        throw std::invalid_argument("接枝起点已改变或被隐藏");
    if (p.anchor>=0 && authoring::length(authoring::sub(at(source,p.anchor),p.anchorPosition))>.001)
        throw std::invalid_argument("接枝起点已改变");
    if (p.removed>=0 && (terminalNeighbor(source,p.removed)!=p.anchor ||
        authoring::length(authoring::sub(at(source,p.removed),p.removedPosition))>.001))
        throw std::invalid_argument("被替换的末端原子已改变");
    if (source.bonds.size()+t.data.bonds.size()+1>interactiveBondBudget)
        throw std::invalid_argument("键数已达到交互显示上限");
}
// Returns only added atoms, so selection/display identity can be remapped by
// the caller before replacing a terminal H. Existing bond orders are retained.
inline std::vector<int> apply(Dataset &source,const Template &t,const Placement &p) {
    using namespace authoring;
    int anchor=p.anchor;
    if (p.removed>=0) { eraseAtoms(source,{p.removed}); if (anchor>p.removed) --anchor; }
    const size_t old=source.atoms.size();
    for (const auto &[name,rows]:t.data.scalarProperties) if (name!="AtomX.MotionGroup" && rows.size()==t.data.atoms.size() && !source.scalarProperties.contains(name))
        source.scalarProperties[name]=std::vector<double>(old,NAN);
    for (const auto &[name,rows]:t.data.vectorProperties) if (rows.size()==t.data.atoms.size() && !source.vectorProperties.contains(name))
        source.vectorProperties[name]=std::vector<Vec3>(old,{NAN,NAN,NAN});
    for (const auto &[name,value]:t.data.propertyComponents) source.propertyComponents.try_emplace(name,value);
    if (source.particleColors.empty() && t.data.particleColors.size()==t.data.atoms.size())
        source.particleColors.resize(old,{-1,-1,-1});
    std::vector<int> remap(t.data.atoms.size(),-1),added;
    for (size_t i=0;i<t.data.atoms.size();++i) {
        if (p.anchor>=0 && int(i)==p.connector) continue;
        const auto &a=t.data.atoms[i]; const auto v=p.points[i];
        remap[i]=int(source.atoms.size()); added.push_back(remap[i]);
        source.atoms.push_back({v.x,v.y,v.z,speciesIndex(source,t.data.species[a.type])});
    }
    for (auto b:t.data.bonds) if (remap[b.a]>=0 && remap[b.b]>=0) {
        b.a=uint32_t(remap[b.a]); b.b=uint32_t(remap[b.b]); source.bonds.push_back(b);
    }
    if (anchor>=0) source.bonds.push_back({uint32_t(anchor),uint32_t(remap[size_t(p.root)]),{},1});
    if (!source.particleColors.empty() && source.particleColors.size()==old) {
        for (size_t i=0;i<remap.size();++i) if (remap[i]>=0) source.particleColors.push_back(
            t.data.particleColors.size()==remap.size()?t.data.particleColors[i]:Vec3{-1,-1,-1});
    }
    for (auto &[name,rows]:source.scalarProperties) if (rows.size()==old) {
        auto found=t.data.scalarProperties.find(name);
        for (size_t i=0;i<remap.size();++i) if (remap[i]>=0)
            rows.push_back(name=="AtomX.MotionGroup"?0:found!=t.data.scalarProperties.end()?found->second[i]:NAN);
    }
    for (auto &[name,rows]:source.vectorProperties) if (rows.size()==old) {
        auto found=t.data.vectorProperties.find(name);
        for (size_t i=0;i<remap.size();++i) if (remap[i]>=0) {
            Vec3 value{NAN,NAN,NAN};
            if (found!=t.data.vectorProperties.end()) {
                const auto v=found->second[i];
                value=add(scale(p.vectorAxes[0],v.x),add(scale(p.vectorAxes[1],v.y),scale(p.vectorAxes[2],v.z)));
            }
            rows.push_back(value);
        }
    }
    source.sourceCount=source.atoms.size(); source.bounds(); return added;
}
// Library entries are ordinary .atomx documents with one selected
// terminal connector. Files can also be opened directly as structures.
inline void save(const std::filesystem::path &path,const Template &t) {
    validate(t); document::View view; view.title=t.name; view.basedOn=t.category;
    view.selection={t.connector}; view.cell=false;
    std::ostringstream buffer(std::ios::binary); document::write(buffer,t.data,view);
    const auto content=buffer.str();
    if (content.size()>1024*1024) throw std::invalid_argument("片段文件超过 1 MiB");
    // New unique files only: failed writes cannot overwrite a library entry.
    if (std::filesystem::exists(path)) throw std::runtime_error("片段文件已存在，请使用新名称");
    auto staging=path; staging+=L".tmp";
    if (std::filesystem::exists(staging)) throw std::runtime_error("片段暂存文件已存在");
    try {
        std::ofstream file(staging,std::ios::binary); file.write(content.data(),std::streamsize(content.size())); file.flush();
        if (!file) throw std::runtime_error("无法保存片段文件");
        file.close(); std::filesystem::rename(staging,path);
    } catch (...) { std::error_code error; std::filesystem::remove(staging,error); throw; }
}
inline Template load(const std::filesystem::path &path) {
    if (std::filesystem::file_size(path)>1024*1024) throw std::runtime_error("片段文件超过 1 MiB");
    auto content=document::read(path,atomLimit);
    if (content.view.selection.size()!=1) throw std::runtime_error("片段文件缺少唯一连接点");
    Template t{"user/"+path.stem().string(),content.view.title,content.view.basedOn,
        std::move(content.data),content.view.selection[0]}; validate(t); return t;
}
} // namespace atomx::fragments
