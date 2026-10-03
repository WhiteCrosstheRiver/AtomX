#pragma once
#include "bond_labels.hpp"

namespace atomx::creation {
// Store only exceptions to the global rule. Identity survives order changes,
// topology reordering and endpoint reversal; cache rows only at committed edits.
struct BondVisibility {
    bool defaultHidden=false;
    std::map<BondKey,bool> exceptions;
    std::map<size_t,bool> indexed;
    size_t hiddenCount=0;
    bool operator==(const BondVisibility &o) const {
        return defaultHidden==o.defaultHidden && exceptions==o.exceptions;
    }
    bool isHidden(size_t row) const {
        const auto it=indexed.find(row);return it==indexed.end()?defaultHidden:it->second;
    }
    bool isHidden(const Dataset &d,size_t row) const {
        if(exceptions.empty() || row>=d.bonds.size())return defaultHidden;
        const auto it=exceptions.find(bondKey(d.bonds[row]));return it==exceptions.end()?defaultHidden:it->second;
    }
    void normalize(const Dataset &d) {
        indexed.clear();hiddenCount=defaultHidden?d.bonds.size():0;
        for(auto it=exceptions.begin();it!=exceptions.end();)
            if(it->second==defaultHidden || it->first.a>=d.atoms.size() || it->first.b>=d.atoms.size())it=exceptions.erase(it);else ++it;
        if(exceptions.empty())return;
        std::set<BondKey> found;
        for(size_t i=0;i<d.bonds.size();++i) {
            const auto key=bondKey(d.bonds[i]);const auto it=exceptions.find(key);
            if(it!=exceptions.end()) {
                found.insert(key);
                if(d.bonds.size()<=interactiveBondBudget)indexed.emplace(i,it->second);
                if(defaultHidden)--hiddenCount;else ++hiddenCount;
            }
        }
        for(auto it=exceptions.begin();it!=exceptions.end();)
            if(!found.contains(it->first))it=exceptions.erase(it);else ++it;
    }
    void visibility(const Dataset &d,const std::vector<int> &rows,int mode) {
        if(mode<0 || mode>2)throw std::invalid_argument("键显示操作无效");
        for(int row:rows)if(row<0 || size_t(row)>=d.bonds.size())throw std::invalid_argument("键选择已失效");
        auto next=exceptions;
        bool nextDefault=defaultHidden;
        if(mode==2) {nextDefault=false;next.clear();}
        else {
            if(mode==1) {nextDefault=true;next.clear();}
            for(int row:rows) {
                const auto key=bondKey(d.bonds[size_t(row)]);const bool value=mode==0;
                if(value==nextDefault)next.erase(key);else next[key]=value;
                if(next.size()>interactiveBondBudget)throw std::runtime_error("局部键显示规则超过交互预算");
            }
        }
        defaultHidden=nextDefault;exceptions=std::move(next);normalize(d);
    }
    void eraseAtoms(const std::vector<int> &remap) {
        std::map<BondKey,bool> next;
        for(const auto &[key,hidden]:exceptions)
            if(key.a<remap.size() && key.b<remap.size() && remap[key.a]>=0 && remap[key.b]>=0) {
                auto mapped=key;mapped.a=uint32_t(remap[key.a]);mapped.b=uint32_t(remap[key.b]);next.emplace(mapped,hidden);
            }
        exceptions=std::move(next);indexed.clear();hiddenCount=0;
    }
};
} // namespace atomx::creation
