#pragma once
#include "creation_display.hpp"
#include <bit>

// AtomX document v5 (reads v1..v4): little-endian IEEE floats; explicit field order and
// length-prefixed UTF-8 strings. No C++ struct padding is persisted.
namespace atomx::document {
struct Style { std::array<float,4> color{},visual{},axes{}; };
struct View {
    bool creation=false,cell=true,particles=true,propertiesOpen=true;
    std::array<float,5> camera{.65f,.48f,1,0,0};
    int32_t cameraMode=7,shape=0,tool=0,order=1,propertyPage=0;
    int32_t ringSize=6;
    std::string fragmentKey="builtin/methyl";
    int32_t fragmentConnector=1;
    bool fitSelected=false,continuous=true;
    Vec3 fitLo{},fitHi{};
    float radius=.32f;
    std::string title,basedOn,element="C";
    std::vector<Style> styles;
    std::vector<int32_t> selection;
    creation::Display display;
};
struct Content { Dataset data; View view; };
inline constexpr char magic[]="ATOMXDOC";
static_assert(std::endian::native==std::endian::little && sizeof(float)==4 && sizeof(double)==8);
inline void checkpoint(std::atomic<bool> *cancel) {
    if (cancel && *cancel) throw std::runtime_error("Cancelled");
}
struct Writer {
    std::ostream &file; std::atomic<bool> *cancel=nullptr;
    void bytes(const void *data,size_t size) {
        auto p=static_cast<const char *>(data);
        while (size) {
            checkpoint(cancel); const size_t chunk=std::min<size_t>(size,1024*1024);
            file.write(p,std::streamsize(chunk));
            if (!file) throw std::runtime_error("AtomX document write failed");
            p+=chunk; size-=chunk;
        }
    }
    template<class T> void value(const T &v) {
        static_assert(std::is_arithmetic_v<T> && !std::is_same_v<T,bool>); bytes(&v,sizeof(v));
    }
    void flag(bool v) { value(uint8_t(v)); }
    void text(const std::string &s) { value(uint64_t(s.size())); bytes(s.data(),s.size()); }
    void vec(Vec3 v) { value(v.x); value(v.y); value(v.z); }
    template<class T> void array(const std::vector<T> &v) {
        static_assert(std::is_arithmetic_v<T> || std::is_same_v<T,Atom> || std::is_same_v<T,Vec3>);
        value(uint64_t(v.size())); bytes(v.data(),v.size()*sizeof(T));
    }
};
struct Reader {
    std::istream &file; uint64_t remaining; std::atomic<bool> *cancel=nullptr;
    void bytes(void *data,size_t size) {
        if (size>remaining) throw std::runtime_error("Truncated AtomX document");
        remaining-=size; auto p=static_cast<char *>(data);
        while (size) {
            checkpoint(cancel); const size_t chunk=std::min<size_t>(size,1024*1024);
            file.read(p,std::streamsize(chunk));
            if (!file) throw std::runtime_error("Truncated AtomX document");
            p+=chunk; size-=chunk;
        }
    }
    template<class T> T value() { T v{}; bytes(&v,sizeof(v)); return v; }
    bool flag() { const auto v=value<uint8_t>(); if (v>1) throw std::runtime_error("Invalid document flag"); return v!=0; }
    size_t count(size_t minimumBytes=1,uint64_t limit=SIZE_MAX) {
        const auto n=value<uint64_t>();
        if (n>limit || n>SIZE_MAX || n>remaining/std::max<size_t>(minimumBytes,1))
            throw std::runtime_error("Invalid AtomX document block length");
        return size_t(n);
    }
    std::string text() { std::string s(count(),'\0'); bytes(s.data(),s.size()); return s; }
    Vec3 vec() { return {value<float>(),value<float>(),value<float>()}; }
    template<class T> std::vector<T> array(uint64_t limit=SIZE_MAX) {
        std::vector<T> v(count(sizeof(T),limit)); bytes(v.data(),v.size()*sizeof(T)); return v;
    }
};
inline void valid(bool condition) {
    if (!condition) throw std::runtime_error("Invalid AtomX document data");
}
inline bool finite(Vec3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
inline void validate(const Dataset &d,const View &v,std::atomic<bool> *cancel=nullptr) {
    valid(d.stride==1);
    valid(std::all_of(d.cell.begin(),d.cell.end(),[](double x){return std::isfinite(x);}) && finite(d.origin));
    for (size_t i=0;i<d.atoms.size();++i) {
        if (!(i%4096)) checkpoint(cancel);
        const auto &a=d.atoms[i]; valid(finite({a.x,a.y,a.z}) && a.type<d.species.size());
    }
    for (size_t i=0;i<d.bonds.size();++i) {
        if (!(i%4096)) checkpoint(cancel);
        const auto &bond=d.bonds[i];
        valid(bond.a<d.atoms.size() && bond.b<d.atoms.size() && bond.order>=1 && bond.order<=4);
        (void)bondVector(d,bond);
    }
    auto validRows=[&](size_t size){valid(!size || size==d.atoms.size());};
    validRows(d.particleColors.size());
    for (const auto &[name,values]:d.scalarProperties) { (void)name; validRows(values.size()); }
    for (const auto &[name,values]:d.vectorProperties) { (void)name; validRows(values.size()); }
    validRows(v.display.hidden.size());
    valid(v.display.defaultPreset<=4 && std::isfinite(v.display.ballRadius) && v.display.ballRadius>=.02f && v.display.ballRadius<=5 &&
        std::isfinite(v.display.stickRadius) && v.display.stickRadius>=.01f && v.display.stickRadius<=v.display.ballRadius &&
        std::isfinite(v.display.cpkScale) && v.display.cpkScale>=.05f && v.display.cpkScale<=3 &&
        std::isfinite(v.display.lineWidth) && v.display.lineWidth>=.5f && v.display.lineWidth<=10);
    for(const auto &[index,preset]:v.display.presets) valid(index>=0 && size_t(index)<d.atoms.size() && preset<=4);
    for (auto hidden:v.display.hidden) valid(hidden<=1);
    valid(int(v.display.defaultLabel.kind)>=0 && int(v.display.defaultLabel.kind)<=6);
    for (const auto &[index,label]:v.display.labels)
        valid(index>=0 && size_t(index)<d.atoms.size() && int(label.kind)>=0 && int(label.kind)<=6);
    valid(std::isfinite(v.radius) && v.radius>0 && v.shape>=0 && v.shape<=6 && v.cameraMode>=0 && v.cameraMode<=7);
    valid(std::all_of(v.camera.begin(),v.camera.end(),[](float x){return std::isfinite(x);}) && v.camera[2]>0);
    valid(finite(v.fitLo) && finite(v.fitHi) && v.tool>=0 && v.tool<=9 && v.order>=1 && v.order<=3);
    valid(v.display.monitors.size()<=geometry::monitorLimit && v.display.activeMonitor>=-1 &&
        (v.display.activeMonitor<0 || size_t(v.display.activeMonitor)<v.display.monitors.size()));
    for(const auto &m:v.display.monitors) valid(geometry::valid(m,d.atoms.size()));
    valid(v.ringSize>=4 && v.ringSize<=6);
    valid(v.fragmentKey.size()<=256 && v.fragmentConnector>=0 && v.fragmentConnector<512);
    valid(v.propertyPage>=0 && v.propertyPage<=2 && (v.styles.empty() || v.styles.size()==d.species.size()));
    valid(std::isfinite(v.display.fontSize) && v.display.fontSize>=1 && v.display.fontSize<=200 &&
          v.display.labelBudget>=1 && v.display.labelBudget<=2000);
    for (auto index:v.selection) valid(index>=0 && size_t(index)<d.atoms.size());
    auto colorValid=[](const auto &color){return std::all_of(color.begin(),color.end(),
        [](float x){return std::isfinite(x) && x>=0 && x<=1;});};
    valid(colorValid(v.display.color) && colorValid(d.bondStyle.color) &&
          std::isfinite(d.bondStyle.width) && d.bondStyle.width>0 &&
          std::isfinite(d.bondStyle.radius) && d.bondStyle.radius>=0);
    for (const auto &style:v.styles) {
        valid(colorValid(style.color));
        for (auto x:style.visual) valid(std::isfinite(x));
        for (auto x:style.axes) valid(std::isfinite(x));
    }
}
inline void write(std::ostream &file,const Dataset &d,const View &view={},std::atomic<bool> *cancel=nullptr) {
    validate(d,view,cancel); Writer w{file,cancel};
    w.bytes(magic,8); w.value(uint32_t(5));
    w.value(uint64_t(d.species.size())); for (const auto &s:d.species) w.text(s);
    w.array(d.atoms); for (double x:d.cell) w.value(x); for (bool x:d.pbc) w.flag(x);
    w.vec(d.origin); w.text(d.comment);
    w.value(uint64_t(d.bonds.size())); for (const auto &b:d.bonds) {
        w.value(b.a); w.value(b.b); for (auto x:b.image) w.value(x); w.value(b.order);
    }
    w.flag(d.bondStyle.visible); w.value(d.bondStyle.width); w.value(d.bondStyle.radius);
    for (auto x:d.bondStyle.color) w.value(x);
    w.flag(d.bondStyle.colorByType); w.flag(d.bondStyle.showPeriodicImages); w.array(d.particleColors);
    w.value(uint64_t(d.scalarProperties.size())); for (const auto &[name,values]:d.scalarProperties) { w.text(name); w.array(values); }
    w.value(uint64_t(d.vectorProperties.size())); for (const auto &[name,values]:d.vectorProperties) { w.text(name); w.array(values); }
    w.value(uint64_t(d.propertyComponents.size())); for (const auto &[name,value]:d.propertyComponents) { w.text(name); w.text(value); }
    w.value(uint64_t(d.globalAttributes.size())); for (const auto &[name,value]:d.globalAttributes) { w.text(name); w.value(value); }
    w.value(uint64_t(d.tables.size())); for (const auto &table:d.tables) {
        w.text(table.name); w.value(uint64_t(table.columns.size())); for (const auto &s:table.columns) w.text(s);
        w.value(uint64_t(table.rows.size())); for (const auto &row:table.rows) {
            w.value(uint64_t(row.size())); for (const auto &s:row) w.text(s);
        }
    }
    w.flag(view.creation); w.flag(view.cell); w.flag(view.particles); w.flag(view.propertiesOpen);
    for (auto x:view.camera) w.value(x); w.value(view.cameraMode); w.flag(view.fitSelected);
    w.vec(view.fitLo); w.vec(view.fitHi); w.value(view.radius); w.value(view.shape);
    w.value(view.tool); w.value(view.order); w.value(view.propertyPage); w.flag(view.continuous);
    w.text(view.title); w.text(view.basedOn); w.text(view.element); w.array(view.selection);
    w.value(uint64_t(view.styles.size())); for (const auto &style:view.styles) {
        for (auto x:style.color) w.value(x); for (auto x:style.visual) w.value(x); for (auto x:style.axes) w.value(x);
    }
    const auto &display=view.display; w.array(display.hidden);
    auto label=[&](const creation::Label &l){w.value(int32_t(l.kind));w.text(l.text);};
    label(display.defaultLabel); w.value(uint64_t(display.labels.size()));
    for (const auto &[index,l]:display.labels) { w.value(int32_t(index)); label(l); }
    w.value(display.fontSize); for (auto x:display.color) w.value(x);
    w.flag(display.bold); w.flag(display.labelsVisible); w.value(int32_t(display.labelBudget));
    w.value(view.ringSize);
    w.text(view.fragmentKey); w.value(view.fragmentConnector);
    w.value(display.defaultPreset); w.value(display.ballRadius); w.value(display.stickRadius); w.value(display.cpkScale); w.value(display.lineWidth);
    w.value(uint64_t(display.presets.size()));
    for(const auto &[index,preset]:display.presets) { w.value(int32_t(index)); w.value(preset); }
    w.flag(display.monitorsVisible); w.value(display.activeMonitor); w.value(uint64_t(display.monitors.size()));
    for(const auto &m:display.monitors) { w.value(m.count); for(auto i:m.atoms) w.value(i); }
    w.bytes("DONE",4);
}
inline Content read(const std::filesystem::path &path,uint64_t atomBudget=UINT64_MAX,std::atomic<bool> *cancel=nullptr) {
    std::ifstream file(path,std::ios::binary); if (!file) throw std::runtime_error("Cannot open AtomX document");
    file.seekg(0,std::ios::end); const auto size=file.tellg(); valid(size>=12); file.seekg(0);
    Reader r{file,uint64_t(size),cancel}; char signature[8]; r.bytes(signature,8);
    valid(std::equal(signature,signature+8,magic));
    const auto version=r.value<uint32_t>();
    if (version<1 || version>5) throw std::runtime_error("Unsupported AtomX document version");
    Content result; auto &d=result.data; auto &v=result.view;
    const size_t types=r.count(8); d.species.reserve(types); for (size_t i=0;i<types;++i) d.species.push_back(r.text());
    const auto atomCount=r.value<uint64_t>();
    if (atomCount>atomBudget) throw std::runtime_error("AtomX document exceeds atom budget; increase the full-data budget before opening");
    valid(atomCount<=SIZE_MAX && atomCount<=r.remaining/sizeof(Atom));
    d.atoms.resize(size_t(atomCount)); r.bytes(d.atoms.data(),d.atoms.size()*sizeof(Atom)); d.sourceCount=atomCount;
    for (auto &x:d.cell) x=r.value<double>(); for (auto &x:d.pbc) x=r.flag(); d.origin=r.vec(); d.comment=r.text();
    const size_t bondCount=r.count(21); d.bonds.resize(bondCount);
    for (auto &b:d.bonds) { b.a=r.value<uint32_t>(); b.b=r.value<uint32_t>(); for (auto &x:b.image) x=r.value<int32_t>(); b.order=r.value<uint8_t>(); }
    d.bondStyle.visible=r.flag(); d.bondStyle.width=r.value<float>(); d.bondStyle.radius=r.value<float>();
    for (auto &x:d.bondStyle.color) x=r.value<float>(); d.bondStyle.colorByType=r.flag(); d.bondStyle.showPeriodicImages=r.flag();
    d.particleColors=r.array<Vec3>(atomCount);
    auto propertyMap=[&](auto &map) {
        const size_t n=r.count(16); for (size_t i=0;i<n;++i) {
            auto name=r.text(); using T=typename std::decay_t<decltype(map)>::mapped_type::value_type;
            auto values=r.array<T>(atomCount); valid(map.emplace(std::move(name),std::move(values)).second);
        }
    };
    propertyMap(d.scalarProperties); propertyMap(d.vectorProperties);
    const size_t components=r.count(16); for (size_t i=0;i<components;++i) { auto name=r.text(); auto value=r.text(); valid(d.propertyComponents.emplace(name,value).second); }
    const size_t attributes=r.count(16); for (size_t i=0;i<attributes;++i) { auto name=r.text(); auto value=r.value<double>(); valid(d.globalAttributes.emplace(name,value).second); }
    const size_t tables=r.count(24); d.tables.resize(tables); for (auto &table:d.tables) {
        table.name=r.text(); const size_t columns=r.count(8); for (size_t i=0;i<columns;++i) table.columns.push_back(r.text());
        const size_t rows=r.count(8); table.rows.resize(rows); for (auto &row:table.rows) {
            const size_t fields=r.count(8); for (size_t i=0;i<fields;++i) row.push_back(r.text());
        }
    }
    v.creation=r.flag(); v.cell=r.flag(); v.particles=r.flag(); v.propertiesOpen=r.flag();
    for (auto &x:v.camera) x=r.value<float>(); v.cameraMode=r.value<int32_t>(); v.fitSelected=r.flag();
    v.fitLo=r.vec(); v.fitHi=r.vec(); v.radius=r.value<float>(); v.shape=r.value<int32_t>();
    v.tool=r.value<int32_t>(); v.order=r.value<int32_t>(); v.propertyPage=r.value<int32_t>(); v.continuous=r.flag();
    v.title=r.text(); v.basedOn=r.text(); v.element=r.text(); v.selection=r.array<int32_t>(atomCount);
    const size_t styles=r.count(48,types); v.styles.resize(styles); for (auto &style:v.styles) {
        for (auto &x:style.color) x=r.value<float>(); for (auto &x:style.visual) x=r.value<float>(); for (auto &x:style.axes) x=r.value<float>();
    }
    v.display.hidden=r.array<uint8_t>(atomCount);
    auto label=[&]() { creation::Label l; l.kind=creation::LabelKind(r.value<int32_t>()); l.text=r.text(); return l; };
    v.display.defaultLabel=label(); const size_t labels=r.count(16,atomCount);
    for (size_t i=0;i<labels;++i) { auto index=r.value<int32_t>(); auto l=label(); valid(v.display.labels.emplace(index,std::move(l)).second); }
    v.display.fontSize=r.value<float>(); for (auto &x:v.display.color) x=r.value<float>();
    v.display.bold=r.flag(); v.display.labelsVisible=r.flag(); v.display.labelBudget=r.value<int32_t>();
    if (version>=2) v.ringSize=r.value<int32_t>();
    else {
        valid(v.tool<=4);
        for (const auto &bond:d.bonds) valid(bond.order<=3);
    }
    if (version>=3) { v.fragmentKey=r.text(); v.fragmentConnector=r.value<int32_t>(); }
    else valid(v.tool<=5);
    if(version>=4) {
        auto &display=v.display; display.defaultPreset=r.value<uint8_t>();
        display.ballRadius=r.value<float>(); display.stickRadius=r.value<float>(); display.cpkScale=r.value<float>(); display.lineWidth=r.value<float>();
        const size_t count=r.count(5,atomCount);
        for(size_t i=0;i<count;++i) { const int index=r.value<int32_t>(); const auto preset=r.value<uint8_t>(); valid(display.presets.emplace(index,preset).second); }
    }
    if(version>=5) {
        auto &display=v.display; display.monitorsVisible=r.flag(); display.activeMonitor=r.value<int32_t>();
        const size_t count=r.count(17,geometry::monitorLimit); display.monitors.resize(count);
        for(auto &m:display.monitors) { m.count=r.value<uint8_t>(); for(auto &i:m.atoms) i=r.value<int32_t>(); }
    } else valid(v.tool<=6);
    char end[4]; r.bytes(end,4); valid(std::string_view(end,4)=="DONE" && r.remaining==0);
    validate(d,v,cancel); d.bounds(); v.display.normalize(d.atoms.size()); return result;
}
} // namespace atomx::document
