#pragma once
#include "core.hpp"
#include <map>
#include <compare>
#include <cstdio>

namespace atomx::creation {
// Identity excludes order: changing a single bond into a double bond keeps its label.
// 64-bit image components make reversed INT32_MIN translations well-defined.
struct BondKey {
    uint32_t a=0,b=0;
    std::array<int64_t,3> image{};
    auto operator<=>(const BondKey &) const = default;
};
inline BondKey bondKey(const Bond &b) {
    BondKey forward{b.a,b.b,{b.image[0],b.image[1],b.image[2]}};
    BondKey reverse{b.b,b.a,{-int64_t(b.image[0]),-int64_t(b.image[1]),-int64_t(b.image[2])}};
    return std::min(forward,reverse);
}
enum class BondField : uint8_t { Length, Order, Endpoints, Midpoint, Image };
struct BondLabel {
    std::string text;
    std::vector<BondField> fields;
    int32_t precision=4;
    bool operator==(const BondLabel &) const = default;
    bool empty() const { return text.empty() && fields.empty(); }
};
inline bool validBondLabel(const BondLabel &l) {
    if(l.text.size()>1024 || l.precision<1 || l.precision>9 || l.fields.size()>5) return false;
    for(size_t i=0;i<l.fields.size();++i)
        if(l.fields[i]>BondField::Image || std::find(l.fields.begin(),l.fields.begin()+i,l.fields[i])!=l.fields.begin()+i) return false;
    return true;
}
inline std::string bondLabelText(const Dataset &d,size_t index,const BondLabel &l) {
    if(index>=d.bonds.size() || l.empty()) return {};
    const auto &b=d.bonds[index];
    std::array<double,3> v;
    try { v=bondVector(d,b); } catch(...) { return {}; }
    auto number=[&](double value) { char s[64]; std::snprintf(s,sizeof(s),"%.*g",std::clamp(l.precision,1,9),value==0?0:value); return std::string(s); };
    std::string text=l.text;
    auto append=[&](const std::string &s) { if(!text.empty())text+='\n';text+=s; };
    for(const auto field:l.fields) switch(field) {
        case BondField::Length: append(number(std::hypot(v[0],v[1],v[2]))+" Å"); break;
        case BondField::Order: append(b.order==4?"Order = 1.5 (aromatic)":"Order = "+std::to_string(b.order)); break;
        case BondField::Endpoints: append("#"+std::to_string(b.a)+" - #"+std::to_string(b.b)); break;
        case BondField::Midpoint: {
            const auto &a=d.atoms[b.a];
            append("Midpoint = ("+number(a.x+v[0]*.5)+", "+number(a.y+v[1]*.5)+", "+number(a.z+v[2]*.5)+") Å"); break;
        }
        case BondField::Image: append("Image = ("+std::to_string(b.image[0])+", "+std::to_string(b.image[1])+", "+std::to_string(b.image[2])+")"); break;
    }
    return text;
}
struct BondLabels {
    BondLabel defaultLabel;
    std::map<BondKey,BondLabel> labels;
    float fontSize=14;
    std::array<float,4> color{1,1,1,1};
    bool bold=false,visible=true;
    int32_t budget=500;
    // Derived once at a committed edit. Idle frames never scan the bond array.
    std::map<size_t,BondLabel> indexed;
    std::vector<size_t> explicitLabels;
    bool operator==(const BondLabels &o) const {
        return defaultLabel==o.defaultLabel && labels==o.labels && fontSize==o.fontSize &&
            color==o.color && bold==o.bold && visible==o.visible && budget==o.budget;
    }
    const BondLabel &at(size_t i) const { auto it=indexed.find(i);return it==indexed.end()?defaultLabel:it->second; }
    const BondLabel &at(const Dataset &d,size_t i) const {
        if(labels.empty() || i>=d.bonds.size())return defaultLabel;
        const auto it=labels.find(bondKey(d.bonds[i]));return it==labels.end()?defaultLabel:it->second;
    }
    void normalize(const Dataset &d) {
        indexed.clear(); explicitLabels.clear();
        for(auto it=labels.begin();it!=labels.end();) {
            if(it->second==defaultLabel || it->first.a>=d.atoms.size() || it->first.b>=d.atoms.size())it=labels.erase(it);else ++it;
        }
        if(labels.empty())return;
        std::set<BondKey> found;
        for(size_t i=0;i<d.bonds.size();++i) {
            const auto key=bondKey(d.bonds[i]);const auto it=labels.find(key);
            if(it!=labels.end()) {found.insert(key);if(indexed.size()<2000) { indexed.emplace(i,it->second);if(!it->second.empty())explicitLabels.push_back(i); } }
        }
        for(auto it=labels.begin();it!=labels.end();)if(!found.contains(it->first))it=labels.erase(it);else ++it;
    }
    void set(const Dataset &d,const std::vector<int> &atoms,const BondLabel &rule,bool all) {
        if(!validBondLabel(rule))throw std::invalid_argument("键标签规则无效");
        if(all) {defaultLabel=rule;labels.clear();}
        else {
            const std::unordered_set<int> selected(atoms.begin(),atoms.end());
            auto next=labels;
            for(const auto &b:d.bonds)if(selected.contains(int(b.a)) && selected.contains(int(b.b))) {
                const auto key=bondKey(b);if(rule==defaultLabel)next.erase(key);else next[key]=rule;
                if(next.size()>2000)throw std::runtime_error("局部键标签超过 2000 条，请缩小选择或应用到整个体系");
            }
            // Check before publication; failures leave the original rules intact.
            labels=std::move(next);
        }
        normalize(d);
    }
    void eraseAtoms(const std::vector<int> &remap) {
        std::map<BondKey,BondLabel> next;
        for(const auto &[k,l]:labels) if(k.a<remap.size() && k.b<remap.size() && remap[k.a]>=0 && remap[k.b]>=0) {
            auto key=k;key.a=uint32_t(remap[k.a]);key.b=uint32_t(remap[k.b]);next.emplace(key,l);
        }
        labels=std::move(next);indexed.clear();explicitLabels.clear();
    }
    std::vector<size_t> candidates(size_t count) const {
        std::vector<size_t> out;if(!visible || budget<=0)return out;
        const size_t total=defaultLabel.empty()?explicitLabels.size():count;
        const size_t limit=std::min(total,size_t(budget));out.reserve(limit);
        for(size_t i=0;i<limit;++i) { const auto sample=i*(total/limit)+(i*(total%limit))/limit;
            out.push_back(defaultLabel.empty()?explicitLabels[sample]:sample); }
        return out;
    }
};
} // namespace atomx::creation
