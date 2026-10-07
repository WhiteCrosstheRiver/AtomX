#pragma once
#include "creation_document.hpp"
#include "motion_groups.hpp"
#include <sstream>

namespace atomx::clipboard {
inline constexpr size_t atomLimit=100000, byteLimit=64*1024*1024;
struct Selection { document::Content content; std::vector<int> rows; };
// Sparse mapping: selecting a handful of atoms does not clone the full source.
inline Selection copy(const Dataset &src,const creation::Display &display,std::vector<int> rows) {
    if(src.sampled())throw std::runtime_error("Cannot copy a sampled structure");
    rows=constraints::selected(src,std::move(rows));
    if(rows.size()>atomLimit)throw std::runtime_error("Copy at most 100,000 atoms per operation");
    Selection out;out.rows=rows;auto &d=out.content.data;auto &v=out.content.view;auto &s=v.display;
    d.species=src.species;d.cell=src.cell;d.origin=src.origin;d.pbc=src.pbc;d.bondStyle=src.bondStyle;
    d.comment="Copied selection";v.creation=true;
    std::unordered_map<int,uint32_t> mapped; mapped.reserve(rows.size());
    s.ballRadius=display.ballRadius;s.stickRadius=display.stickRadius;s.cpkScale=display.cpkScale;s.lineWidth=display.lineWidth;
    s.fontSize=display.fontSize;s.color=display.color;s.bold=display.bold;s.labelsVisible=display.labelsVisible;
    s.monitorsVisible=display.monitorsVisible;
    for(int row:rows) {
        const auto i=uint32_t(d.atoms.size());mapped.emplace(row,i);d.atoms.push_back(src.atoms[size_t(row)]);
        if(!src.particleColors.empty())d.particleColors.push_back(src.particleColors.at(size_t(row)));
        s.colors[int(i)]=display.colorAt(size_t(row));s.presets[int(i)]=display.presetAt(size_t(row));
        s.labels[int(i)]=display.labelAt(row);
        if(display.isHidden(size_t(row))) {if(s.hidden.empty())s.hidden.resize(rows.size());s.hidden[i]=1;}
    }
    for(const auto &[name,values]:src.scalarProperties) if(!values.empty()) {
        if(values.size()!=src.atoms.size())throw std::runtime_error("Invalid source property rows");
        auto &dest=d.scalarProperties[name];dest.reserve(rows.size());for(int row:rows)dest.push_back(values[size_t(row)]);
    }
    for(const auto &[name,values]:src.vectorProperties) if(!values.empty()) {
        if(values.size()!=src.atoms.size())throw std::runtime_error("Invalid source property rows");
        auto &dest=d.vectorProperties[name];dest.reserve(rows.size());for(int row:rows)dest.push_back(values[size_t(row)]);
    }
    d.propertyComponents=src.propertyComponents;
    // Only the selected fragment's group names have meaning in the clipboard.
    auto labels=motion::names(src);std::map<int,std::string> used;
    for(const auto &group:motion::catalog(d)) {auto it=labels.find(group.id);used[group.id]=it==labels.end()?group.name:it->second;}
    motion::setNames(d,used);
    for(size_t row=0;row<src.bonds.size();++row) {
        auto b=src.bonds[row];auto a=mapped.find(int(b.a)),z=mapped.find(int(b.b));
        if(a==mapped.end() || z==mapped.end())continue;
        b.a=a->second;b.b=z->second;d.bonds.push_back(b);const auto key=creation::bondKey(b);
        const auto &label=display.bondLabels.at(src,row);if(!label.empty())s.bondLabels.labels[key]=label;
        if(display.bondVisibility.isHidden(src,row))s.bondVisibility.exceptions[key]=true;
    }
    for(auto m:display.monitors) {
        bool complete=true;for(int k=0;k<m.count;++k) {auto it=mapped.find(m.atoms[size_t(k)]);
            if(it==mapped.end()){complete=false;break;}m.atoms[size_t(k)]=int32_t(it->second);}
        if(complete)s.monitors.push_back(m);
    }
    d.sourceCount=d.atoms.size();d.bounds();s.normalize(d.atoms.size());s.normalizeBonds(d);
    document::validate(d,v);return out;
}
inline std::string encode(const document::Content &packet) {
    std::ostringstream stream(std::ios::binary);document::write(stream,packet.data,packet.view);
    auto bytes=stream.str();if(bytes.size()>byteLimit)throw std::runtime_error("Clipboard structure exceeds 64 MiB");return bytes;
}
inline document::Content decode(const std::string &bytes) {
    if(bytes.size()>byteLimit)throw std::runtime_error("Clipboard structure exceeds 64 MiB");
    std::istringstream stream(bytes,std::ios::binary);return document::read(stream,bytes.size(),atomLimit);
}
struct Paste { Dataset data;creation::Display display;std::vector<int> rows; };
// Prepare privately before publication: incompatible columns/cells cannot leave
// half an edit or an empty undo step. Paste is one history transaction.
inline Paste prepare(const Dataset &target,const creation::Display &display,const document::Content &packet) {
    const auto &p=packet.data;document::validate(p,packet.view);
    if(target.sampled() || p.atoms.empty() || p.atoms.size()>atomLimit || target.atoms.size()+p.atoms.size()>size_t(INT32_MAX))
        throw std::runtime_error("Invalid paste size or sampled target");
    const bool lattice=constraints::anyFractional(p) || std::any_of(p.bonds.begin(),p.bonds.end(),[](const auto &b){return b.image!=std::array<int32_t,3>{};});
    if(lattice && (p.cell!=target.cell || p.pbc!=target.pbc))throw std::runtime_error("Periodic bonds or fractional constraints require the same cell");
    for(const auto &[name,values]:p.scalarProperties) { (void)values;if(target.vectorProperties.contains(name))throw std::runtime_error("Property type conflict: "+name); }
    for(const auto &[name,values]:p.vectorProperties) { (void)values;if(target.scalarProperties.contains(name))throw std::runtime_error("Property type conflict: "+name); }
    Paste out{target,display,{}};auto &d=out.data;auto &s=out.display;const auto base=d.atoms.size(),n=p.atoms.size();
    std::vector<uint32_t> types;for(const auto &name:p.species)types.push_back(uint32_t(authoring::speciesIndex(d,name)));
    for(auto a:p.atoms) {a.type=types.at(a.type);out.rows.push_back(int(d.atoms.size()));d.atoms.push_back(a);}
    const double missing=std::numeric_limits<double>::quiet_NaN();
    auto scalarDefault=[&](const std::string &name){return name==constraints::cartesianProperty || name==motion::property?0.:missing;};
    for(const auto &[name,values]:p.scalarProperties) { (void)values; if(!d.scalarProperties.contains(name))d.scalarProperties[name].resize(base,scalarDefault(name)); }
    for(auto &[name,values]:d.scalarProperties) {
        if(values.empty())values.resize(base,scalarDefault(name));if(values.size()!=base)throw std::runtime_error("Invalid target property rows");
        auto it=p.scalarProperties.find(name);if(it==p.scalarProperties.end() || it->second.empty())values.resize(base+n,scalarDefault(name));
        else values.insert(values.end(),it->second.begin(),it->second.end());
    }
    auto vectorDefault=[&](const std::string &name){return name=="MoveMask"?Vec3{1,1,1}:Vec3{float(missing),float(missing),float(missing)};};
    for(const auto &[name,values]:p.vectorProperties) { (void)values; if(!d.vectorProperties.contains(name))d.vectorProperties[name].resize(base,vectorDefault(name)); }
    for(auto &[name,values]:d.vectorProperties) {
        if(values.empty())values.resize(base,vectorDefault(name));if(values.size()!=base)throw std::runtime_error("Invalid target property rows");
        auto it=p.vectorProperties.find(name);if(it==p.vectorProperties.end() || it->second.empty())values.resize(base+n,vectorDefault(name));
        else values.insert(values.end(),it->second.begin(),it->second.end());
    }
    auto names=motion::names(target);int next=1;for(const auto &group:motion::catalog(target)) {
        if(group.id==INT32_MAX)throw std::runtime_error("Motion group IDs exhausted");next=std::max(next,group.id+1);
    }
    const auto groups=motion::catalog(p);if(groups.size()+motion::catalog(target).size()>motion::groupLimit)throw std::runtime_error("Too many motion groups");
    std::map<int,int> remap;for(const auto &group:groups) {if(next==INT32_MAX)throw std::runtime_error("Motion group IDs exhausted");remap[group.id]=next;names[next++]=group.name;}
    if(!groups.empty())for(size_t i=base;i<base+n;++i) {auto &value=d.scalarProperties.at(motion::property)[i];const auto id=motion::id(value);if(id)value=remap.at(id);}
    motion::setNames(d,names);
    for(const auto &[key,value]:p.propertyComponents) if(!d.propertyComponents.contains(key))d.propertyComponents[key]=value;
    if(!d.particleColors.empty() || !p.particleColors.empty()) {
        if(d.particleColors.empty())d.particleColors.resize(base,Vec3{float(missing),float(missing),float(missing)});
        if(d.particleColors.size()!=base)throw std::runtime_error("Invalid target color rows");
        if(p.particleColors.empty())d.particleColors.resize(base+n,Vec3{float(missing),float(missing),float(missing)});
        else d.particleColors.insert(d.particleColors.end(),p.particleColors.begin(),p.particleColors.end());
    }
    s.hidden.resize(base+n);for(size_t i=0;i<n;++i) {
        s.hidden[base+i]=packet.view.display.isHidden(i);s.presets[int(base+i)]=packet.view.display.presetAt(i);
        s.colors[int(base+i)]=packet.view.display.colorAt(i);s.labels[int(base+i)]=packet.view.display.labelAt(int(i));
    }
    for(size_t row=0;row<p.bonds.size();++row) {
        auto b=p.bonds[row];b.a+=uint32_t(base);b.b+=uint32_t(base);d.bonds.push_back(b);const auto key=creation::bondKey(b);
        const auto &label=packet.view.display.bondLabels.at(p,row);
        if(label!=s.bondLabels.defaultLabel)s.bondLabels.labels[key]=label;
        const bool hidden=packet.view.display.bondVisibility.isHidden(p,row);
        if(hidden!=s.bondVisibility.defaultHidden)s.bondVisibility.exceptions[key]=hidden;
    }
    if(s.bondLabels.labels.size()>2000)throw std::runtime_error("Paste would exceed the local bond-label budget");
    if(s.monitors.size()+packet.view.display.monitors.size()>geometry::monitorLimit)throw std::runtime_error("Too many measurement monitors");
    for(auto m:packet.view.display.monitors) {for(int k=0;k<m.count;++k)m.atoms[size_t(k)]+=int32_t(base);s.monitors.push_back(m);}
    s.normalize(d.atoms.size());s.normalizeBonds(d);d.sourceCount=d.atoms.size();d.bounds();
    document::View check;check.display=s;document::validate(d,check);return out;
}
} // namespace atomx::clipboard
