#pragma once
#include "hydrogen_adjust.hpp"

namespace atomx::chemistry {
inline constexpr const char *hybridNames[]={"自动","SP · 直线","SP2 · 平面三角","SP3 · 四面体",
    "平面正方形","三角双锥","四方锥","八面体"};
struct Edit { std::vector<int> selection; std::optional<int> charge,hybrid; bool changed=false; };
inline double charge(const Dataset &data,size_t index) {
    return hydrogens::property(data,"AtomX.FormalCharge",index,hydrogens::property(data,"FormalCharge",index,0));
}
inline double hybrid(const Dataset &data,size_t index) {return hydrogens::property(data,"AtomX.Hybridization",index,0);}
// Validate all inputs before history or data mutation. Only requested columns
// are materialized; imported partial charge and unrelated scientific rows stay intact.
inline Edit prepare(const Dataset &data,std::vector<int> selection,std::optional<int> q,std::optional<int> h) {
    if(h && (*h<0 || *h>7))throw std::invalid_argument("杂化设置无效");
    if(!q && !h)throw std::invalid_argument("请选择要修改的化学属性");
    std::sort(selection.begin(),selection.end());selection.erase(std::unique(selection.begin(),selection.end()),selection.end());
    if(selection.empty())throw std::invalid_argument("请选择原子");
    Edit edit{std::move(selection),q,h};
    for(int i:edit.selection) {
        if(i<0 || size_t(i)>=data.atoms.size())throw std::invalid_argument("选中原子已改变");
        if(q)edit.changed|=charge(data,size_t(i))!=*q;
        if(h)edit.changed|=hybrid(data,size_t(i))!=*h;
    }
    return edit;
}
inline void apply(Dataset &data,const Edit &edit) {
    const auto checked=prepare(data,edit.selection,edit.charge,edit.hybrid);
    if(!checked.changed)return;
    auto column=[&](const char *name,bool fromFile) -> std::vector<double>& {
        auto it=data.scalarProperties.find(name);
        if(it==data.scalarProperties.end()) {
            std::vector<double> values(data.atoms.size(),0);
            if(fromFile)if(auto source=data.scalarProperties.find("FormalCharge");source!=data.scalarProperties.end())values=source->second;
            it=data.scalarProperties.emplace(name,std::move(values)).first;
        }
        return it->second;
    };
    if(edit.charge) {auto &rows=column("AtomX.FormalCharge",true);for(int i:edit.selection)rows[size_t(i)]=*edit.charge;}
    if(edit.hybrid) {auto &rows=column("AtomX.Hybridization",false);for(int i:edit.selection)rows[size_t(i)]=*edit.hybrid;}
}
}
