#pragma once
#include "authoring.hpp"
#include "geometry_monitors.hpp"
#include "elements.hpp"
#include "creation_colors.hpp"
#include "bond_labels.hpp"
#include <cstdio>
#include <unordered_map>

namespace atomx::creation {
enum class LabelKind { None, Element, Index, ElementIndex, Cartesian, Fractional, Custom, Properties };
enum class LabelFieldKind { Element, Index, Cartesian, Fractional, ElementName, AtomicNumber, Mass,
    Scalar, Vector, VectorX, VectorY, VectorZ, VectorMagnitude };
struct LabelField {
    LabelFieldKind kind=LabelFieldKind::Element;
    std::string property;
    bool operator==(const LabelField &) const = default;
};
inline constexpr size_t labelFieldLimit=16;
struct Label {
    LabelKind kind = LabelKind::None;
    std::string text;
    std::vector<LabelField> fields;
    int32_t precision=6;
    bool operator==(const Label &) const = default;
};
inline bool validLabel(const Label &label) {
    if(int(label.kind)<0 || label.kind>LabelKind::Properties || label.precision<1 || label.precision>9 ||
        label.fields.size()>labelFieldLimit || (label.kind!=LabelKind::Properties && !label.fields.empty())) return false;
    if(label.kind==LabelKind::Properties && label.fields.empty()) return false;
    for(size_t i=0;i<label.fields.size();++i) {
        const auto &f=label.fields[i];
        if(int(f.kind)<0 || f.kind>LabelFieldKind::VectorMagnitude || f.property.size()>1024 ||
            (f.kind>=LabelFieldKind::Scalar ? f.property.empty():!f.property.empty())) return false;
        if(std::find(label.fields.begin(),label.fields.begin()+i,f)!=label.fields.begin()+i) return false;
    }
    return true;
}
inline std::string fieldTitle(const LabelField &f) {
    static constexpr const char *builtins[]={"Element","Index","Position","Fractional","ElementName","AtomicNumber","Mass"};
    const int k=int(f.kind);
    if(k>=0 && k<7) return builtins[k];
    if(f.kind==LabelFieldKind::Scalar) return f.property;
    if(f.kind==LabelFieldKind::Vector) return f.property+" (XYZ)";
    if(f.kind>=LabelFieldKind::VectorX && f.kind<=LabelFieldKind::VectorZ) return f.property+"."+"XYZ"[k-int(LabelFieldKind::VectorX)];
    if(f.kind==LabelFieldKind::VectorMagnitude) return "|"+f.property+"|";
    return "?";
}
struct Display {
    BondLabels bondLabels;
    ColorRule defaultColor;
    std::unordered_map<int,ColorRule> colors;
    bool hasColors() const { return defaultColor.kind!=ColorKind::Source || !colors.empty(); }
    const ColorRule &colorAt(size_t i) const {
        const auto it=colors.find(int(i)); return it==colors.end()?defaultColor:it->second;
    }
    void setColor(size_t count,const std::vector<int> &selected,const ColorRule &rule,bool all) {
        if(!validColor(rule)) throw std::invalid_argument("着色规则无效");
        if(!all && count && selected.size()==count && selected.front()==0 && selected.back()==int(count)-1 &&
            std::is_sorted(selected.begin(),selected.end()) && std::adjacent_find(selected.begin(),selected.end())==selected.end()) all=true;
        if(all) {defaultColor=rule;colors.clear();}
        else for(int i:selected) if(i>=0 && size_t(i)<count) {
            if(rule==defaultColor) colors.erase(i); else colors[i]=rule;
        }
    }
    std::vector<geometry::Monitor> monitors;
    int32_t activeMonitor=-1;
    bool monitorsVisible=true;
    uint8_t defaultPreset=0; // 0 original, 1 line, 2 stick, 3 ball/stick, 4 CPK.
    std::unordered_map<int,uint8_t> presets;
    float ballRadius=.4f,stickRadius=.2f,cpkScale=.7f,lineWidth=1.6f;
    bool sameGpuAppearance(const Display &other) const {
        return defaultColor==other.defaultColor && colors==other.colors && hidden==other.hidden && defaultPreset==other.defaultPreset && presets==other.presets &&
            ballRadius==other.ballRadius && stickRadius==other.stickRadius && cpkScale==other.cpkScale && lineWidth==other.lineWidth;
    }
    uint8_t presetAt(size_t index) const {
        const auto it=presets.find(int(index)); return it==presets.end()?defaultPreset:it->second;
    }
    void setPreset(size_t count,const std::vector<int> &selected,uint8_t preset,bool all) {
        if (preset>4) throw std::invalid_argument("显示样式无效");
        if (all) { defaultPreset=preset; presets.clear(); }
        else for(int index:selected) if(index>=0 && size_t(index)<count) {
            if(preset==defaultPreset) presets.erase(index); else presets[index]=preset;
        }
    }
    // Empty mask/default labels allocate nothing for the normal creation view.
    std::vector<uint8_t> hidden;
    size_t hiddenCount = 0;
    Label defaultLabel;
    std::unordered_map<int,Label> labels;
    std::vector<int> explicitLabels;
    float fontSize = 14;
    std::array<float,4> color{1,1,1,1};
    bool bold = false, labelsVisible = true;
    int labelBudget = 500;
    bool operator==(const Display &) const = default;

    bool isHidden(size_t index) const { return index<hidden.size() && hidden[index]!=0; }
    const Label &labelAt(int index) const {
        const auto found=labels.find(index);
        return found==labels.end()?defaultLabel:found->second;
    }
    void normalize(size_t count) {
        for(auto it=colors.begin();it!=colors.end();) {
            if(it->first<0 || size_t(it->first)>=count || it->second==defaultColor) it=colors.erase(it); else ++it;
        }
        for(size_t i=monitors.size();i-->0;) if(!geometry::valid(monitors[i],count)) {
            monitors.erase(monitors.begin()+i);
            if(activeMonitor==int(i)) activeMonitor=-1; else if(activeMonitor>int(i)) --activeMonitor;
        }
        if(activeMonitor<0 || size_t(activeMonitor)>=monitors.size()) activeMonitor=-1;
        for(auto it=presets.begin();it!=presets.end();) {
            if(it->first<0 || size_t(it->first)>=count || it->second==defaultPreset) it=presets.erase(it);
            else ++it;
        }
        if (!hidden.empty()) hidden.resize(count,0);
        hiddenCount=size_t(std::count(hidden.begin(),hidden.end(),uint8_t(1)));
        if (!hiddenCount) hidden.clear();
        explicitLabels.clear();
        for (auto it=labels.begin();it!=labels.end();) {
            if (it->first<0 || size_t(it->first)>=count || it->second==defaultLabel) it=labels.erase(it);
            else {
                if (it->second.kind!=LabelKind::None) explicitLabels.push_back(it->first);
                ++it;
            }
        }
        std::sort(explicitLabels.begin(),explicitLabels.end());
    }
    void visibility(size_t count,const std::vector<int> &selected,int mode) {
        if (mode==2) { hidden.clear(); hiddenCount=0; return; }
        hidden.resize(count,0);
        if (mode==1) {
            std::fill(hidden.begin(),hidden.end(),uint8_t(1));
            for (int index:selected) if (index>=0 && size_t(index)<count) hidden[size_t(index)]=0;
        } else for (int index:selected) if (index>=0 && size_t(index)<count) hidden[size_t(index)]=1;
        normalize(count);
    }
    void setLabels(size_t count,const std::vector<int> &selected,Label value,bool all) {
        if(!validLabel(value)) throw std::invalid_argument("原子标签规则无效");
        if (!all && selected.size()==count && count && selected.front()==0 &&
            selected.back()==int(count)-1 && std::is_sorted(selected.begin(),selected.end()) &&
            std::adjacent_find(selected.begin(),selected.end())==selected.end()) all=true;
        if (all) { defaultLabel=std::move(value); labels.clear(); }
        else for (int index:selected) if (index>=0 && size_t(index)<count) labels[index]=value;
        normalize(count);
    }
    // Keep annotations attached to the surviving original atoms on deletion.
    void eraseAtoms(size_t oldCount,const std::vector<int> &removed) {
        std::vector<int> remap(oldCount,-1);
        size_t cursor=0,out=0;
        for (size_t index=0;index<oldCount;++index) {
            if (cursor<removed.size() && size_t(removed[cursor])==index) { ++cursor; continue; }
            remap[index]=int(out++);
        }
        if (!hidden.empty()) {
            for (size_t index=0;index<oldCount;++index)
                if (remap[index]>=0) hidden[size_t(remap[index])]=isHidden(index)?1:0;
            hidden.resize(out);
        }
        std::unordered_map<int,Label> next;
        for (const auto &[index,label]:labels)
            if (index>=0 && size_t(index)<oldCount && remap[size_t(index)]>=0)
                next.emplace(remap[size_t(index)],label);
        labels=std::move(next);
        std::unordered_map<int,uint8_t> nextPresets;
        for(const auto &[index,preset]:presets)
            if(index>=0 && size_t(index)<oldCount && remap[size_t(index)]>=0) nextPresets.emplace(remap[size_t(index)],preset);
        presets=std::move(nextPresets);
        std::unordered_map<int,ColorRule> nextColors;
        for(const auto &[index,rule]:colors)
            if(index>=0 && size_t(index)<oldCount && remap[size_t(index)]>=0) nextColors.emplace(remap[size_t(index)],rule);
        colors=std::move(nextColors);
        bondLabels.eraseAtoms(remap);
        for(auto &monitor:monitors) for(size_t i=0;i<monitor.count;++i) {
            auto &index=monitor.atoms[i]; index=index>=0 && size_t(index)<oldCount?remap[size_t(index)]:-1;
        }
        normalize(out);
    }
    // Bounded arithmetic sampling: global labels never create an entry/string
    // per atom, and drawing does not walk all atoms of a large structure.
    std::vector<int> candidates(size_t count) const {
        std::vector<int> result;
        if (!labelsVisible || labelBudget<=0) return result;
        const size_t total=defaultLabel.kind==LabelKind::None?explicitLabels.size():count;
        const size_t limit=std::min(total,size_t(labelBudget));
        result.reserve(limit);
        for (size_t i=0;i<limit;++i) {
            const size_t sampled=i*total/limit;
            result.push_back(defaultLabel.kind==LabelKind::None?explicitLabels[sampled]:int(sampled));
        }
        return result;
    }
};
struct ColorResolver {
    const Dataset &data;
    const Display &display;
    ColorValues defaultValues;
    std::unordered_map<std::string,ColorValues> values;
    ColorResolver(const Dataset &d,const Display &s):data(d),display(s),defaultValues(d,s.defaultColor.property) {
        for(const auto &[index,r]:s.colors) { (void)index;
            if(r.kind>=ColorKind::Property) values.try_emplace(r.property,d,r.property);
        }
    }
    Vec3 at(size_t i) const {
        const auto &r=display.colorAt(i);
        if(r.kind==ColorKind::Source) return data.particleColors.size()==data.atoms.size()?data.particleColors[i]:Vec3{-1,-1,-1};
        if(r.kind==ColorKind::Element) return {-1,-1,-1};
        if(r.kind==ColorKind::Custom) return r.rgb;
        const double v=&r==&display.defaultColor?defaultValues.at(i):values.at(r.property).at(i);
        return mappedColor(r,v);
    }
};
inline std::string labelText(const Dataset &data,int index,const Label &label) {
    if (index<0 || size_t(index)>=data.atoms.size()) return {};
    const auto &atom=data.atoms[size_t(index)];
    const auto element=atom.type<data.species.size()?data.species[atom.type]:"?";
    switch (label.kind) {
    case LabelKind::None: return {};
    case LabelKind::Element: return element;
    case LabelKind::Index: return "#"+std::to_string(index);
    case LabelKind::ElementIndex: return element+" #"+std::to_string(index);
    case LabelKind::Custom: return label.text;
    case LabelKind::Properties: {
        // Read only this visible atom's current values. Never materialize a
        // complete property column or compute a range for a text overlay.
        auto number=[&](double value) {
            if(!std::isfinite(value)) return std::string("N/A");
            char text[64]; std::snprintf(text,sizeof(text),"%.*g",std::clamp(label.precision,1,9),value==0?0:value);
            return std::string(text);
        };
        auto tuple=[&](double x,double y,double z) { return "("+number(x)+", "+number(y)+", "+number(z)+")"; };
        std::string text=label.text;
        const auto *e=elements::find(element);
        for(size_t i=0;i<std::min(label.fields.size(),labelFieldLimit);++i) {
            const auto &field=label.fields[i]; std::string value="N/A";
            switch(field.kind) {
            case LabelFieldKind::Element: value=element; break;
            case LabelFieldKind::Index: value="#"+std::to_string(index); break;
            case LabelFieldKind::ElementName: if(e) value=e->name; break;
            case LabelFieldKind::AtomicNumber: if(e) value=std::to_string(e->z); break;
            case LabelFieldKind::Cartesian: value=tuple(atom.x,atom.y,atom.z); break;
            case LabelFieldKind::Fractional: {
                double x,y,z; if(authoring::fractional(data,{atom.x,atom.y,atom.z},x,y,z)) value=tuple(x,y,z); break;
            }
            case LabelFieldKind::Mass:
            case LabelFieldKind::Scalar: {
                const auto it=data.scalarProperties.find(field.kind==LabelFieldKind::Mass?"Mass":field.property);
                if(it!=data.scalarProperties.end()) { if(size_t(index)<it->second.size()) value=number(it->second[size_t(index)]); }
                else if(field.kind==LabelFieldKind::Mass && e && elements::atomicMass(*e)>0) value=number(elements::atomicMass(*e));
                break;
            }
            default: {
                const auto it=data.vectorProperties.find(field.property);
                if(it==data.vectorProperties.end() || size_t(index)>=it->second.size()) break;
                const auto v=it->second[size_t(index)];
                if(field.kind==LabelFieldKind::Vector) value=tuple(v.x,v.y,v.z);
                else if(field.kind==LabelFieldKind::VectorX) value=number(v.x);
                else if(field.kind==LabelFieldKind::VectorY) value=number(v.y);
                else if(field.kind==LabelFieldKind::VectorZ) value=number(v.z);
                else if(field.kind==LabelFieldKind::VectorMagnitude) value=number(std::hypot(double(v.x),double(v.y),double(v.z)));
                break;
            }
            }
            if(!text.empty()) text+='\n';
            text+=fieldTitle(field)+" = "+value;
        }
        return text;
    }
    default: {
        double x=atom.x,y=atom.y,z=atom.z;
        if (label.kind==LabelKind::Fractional && !authoring::fractional(data,{atom.x,atom.y,atom.z},x,y,z))
            return {};
        char text[100]; std::snprintf(text,sizeof(text),"(%.3f, %.3f, %.3f)",x,y,z);
        return text;
    }
    }
}
} // namespace atomx::creation
