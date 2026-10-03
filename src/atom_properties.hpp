#pragma once
#include "authoring.hpp"
#include <optional>

namespace atomx::properties {
// Structural identity, chemistry and AtomX metadata have dedicated editors.
inline bool editable(const std::string &name) {
    return !name.empty() && !name.starts_with("AtomX.") && name!="FormalCharge" &&
        name!="FormalSpin" && name!="Hybridization" && name!="id" && name!="Identifier" &&
        name!="Particle Identifier" && name!="Selection";
}
struct Summary {size_t present=0,missing=0;std::optional<double> common;};
struct Edit {std::string name;std::vector<int> selection;std::optional<double> value;bool changed=false;};
inline std::vector<int> selected(const Dataset &data,std::vector<int> rows) {
    std::sort(rows.begin(),rows.end());rows.erase(std::unique(rows.begin(),rows.end()),rows.end());
    if(rows.empty())throw std::invalid_argument("请选择原子");
    for(int i:rows)if(i<0 || size_t(i)>=data.atoms.size())throw std::invalid_argument("选中原子已改变");
    return rows;
}
inline const std::vector<double> *column(const Dataset &data,const std::string &name) {
    if(!editable(name) || data.vectorProperties.contains(name))throw std::invalid_argument("该属性请使用专用编辑工具");
    auto it=data.scalarProperties.find(name);
    if(it==data.scalarProperties.end()) {
        if(name!="Charge")throw std::invalid_argument("属性列不存在");
        return nullptr;
    }
    if(it->second.size()!=data.atoms.size())throw std::invalid_argument("属性行数与体系不一致");
    return &it->second;
}
inline Summary summarize(const Dataset &data,const std::string &name,const std::vector<int> &rows) {
    const auto selection=selected(data,rows);const auto *values=column(data,name);Summary s;bool same=true;
    for(int i:selection) {
        const double v=values?(*values)[size_t(i)]:NAN;
        if(!std::isfinite(v)){++s.missing;continue;}
        if(!s.present)s.common=v;else same&=*s.common==v;
        ++s.present;
    }
    if(!same || s.missing)s.common.reset();
    return s;
}
inline Edit prepare(const Dataset &data,std::string name,std::vector<int> rows,std::optional<double> value) {
    Edit e{std::move(name),selected(data,std::move(rows)),value};const auto *values=column(data,e.name);
    if(value && !std::isfinite(*value))throw std::invalid_argument("请输入有限数值");
    if(value && e.name=="Mass" && *value<=0)throw std::invalid_argument("Mass 必须大于零");
    for(int i:e.selection) {
        const double old=values?(*values)[size_t(i)]:NAN;
        e.changed|=value?(!std::isfinite(old) || old!=*value):!std::isnan(old);
    }
    return e;
}
inline void apply(Dataset &data,const Edit &edit) {
    const auto checked=prepare(data,edit.name,edit.selection,edit.value);
    if(!checked.changed)return;
    auto it=data.scalarProperties.find(edit.name);
    if(it==data.scalarProperties.end())it=data.scalarProperties.emplace(edit.name,
        std::vector<double>(data.atoms.size(),NAN)).first;
    for(int i:checked.selection)it->second[size_t(i)]=edit.value.value_or(NAN);
}
}
