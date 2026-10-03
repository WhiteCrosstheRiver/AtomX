#pragma once
#include "core.hpp"

namespace atomx::constraints {
inline constexpr const char *cartesianProperty="AtomX.FixedCartesian";
// MoveMask is the POSCAR selective-dynamics mask: 1 permits motion along a
// lattice vector, 0 fixes that fractional component. It is not a spatial vector.
inline bool periodic(const Dataset &d) {
    if(!std::any_of(d.pbc.begin(),d.pbc.end(),[](bool p){return p;}) ||
        !std::all_of(d.cell.begin(),d.cell.end(),[](double x){return std::isfinite(x);}))return false;
    try{(void)cellInverse(d.cell);return true;}catch(const std::exception &){return false;}
}
inline void validate(const Dataset &d) {
    if(d.vectorProperties.contains(cartesianProperty) || d.scalarProperties.contains("MoveMask"))
        throw std::invalid_argument("约束属性类型不正确");
    if(auto it=d.scalarProperties.find(cartesianProperty);it!=d.scalarProperties.end()) {
        if(it->second.size()!=d.atoms.size())throw std::invalid_argument("约束属性行数不一致");
        for(double x:it->second)if(x!=0 && x!=1)throw std::invalid_argument("整原子固定标志必须为零或一");
    }
    if(auto it=d.vectorProperties.find("MoveMask");it!=d.vectorProperties.end()) {
        if(it->second.size()!=d.atoms.size())throw std::invalid_argument("约束属性行数不一致");
        for(auto v:it->second)for(float x:{v.x,v.y,v.z})if(x!=0 && x!=1)
            throw std::invalid_argument("晶格方向约束标志必须为零或一");
    }
}
inline bool cartesian(const Dataset &d,size_t i) {
    const auto it=d.scalarProperties.find(cartesianProperty);
    return it!=d.scalarProperties.end() && it->second.at(i)==1;
}
inline Vec3 mask(const Dataset &d,size_t i) {
    const auto it=d.vectorProperties.find("MoveMask");
    return it==d.vectorProperties.end()?Vec3{1,1,1}:it->second.at(i);
}
inline bool anyCartesian(const Dataset &d) {
    const auto it=d.scalarProperties.find(cartesianProperty);
    return it!=d.scalarProperties.end() && std::any_of(it->second.begin(),it->second.end(),[](double x){return x!=0;});
}
inline bool anyFractional(const Dataset &d) {
    const auto it=d.vectorProperties.find("MoveMask");
    return it!=d.vectorProperties.end() && std::any_of(it->second.begin(),it->second.end(),[](Vec3 v){return v.x!=1 || v.y!=1 || v.z!=1;});
}
// Keep is deliberately distinct from Free: a mixed multi-selection must not
// silently overwrite components that the user did not edit.
enum Choice {Keep,Free,Fixed};
struct Draft {int cartesian=Keep;std::array<int,3> fractional{Keep,Keep,Keep};};
struct Summary {int cartesian=0;std::array<int,3> fractional{};}; // -1 = mixed
struct Edit {std::vector<int> rows;Draft draft;bool changed=false;};
inline std::vector<int> selected(const Dataset &d,std::vector<int> rows) {
    std::sort(rows.begin(),rows.end());rows.erase(std::unique(rows.begin(),rows.end()),rows.end());
    if(rows.empty())throw std::invalid_argument("请选择原子");
    for(int i:rows)if(i<0 || size_t(i)>=d.atoms.size())throw std::invalid_argument("选中原子已改变");
    return rows;
}
inline Summary summarize(const Dataset &d,const std::vector<int> &rows) {
    validate(d);const auto chosen=selected(d,rows);Summary s;
    s.cartesian=cartesian(d,size_t(chosen.front()));
    auto first=mask(d,size_t(chosen.front()));s.fractional={int(first.x==0),int(first.y==0),int(first.z==0)};
    for(int i:chosen) {
        if(int(cartesian(d,size_t(i)))!=s.cartesian)s.cartesian=-1;
        const auto v=mask(d,size_t(i));const float values[]{v.x,v.y,v.z};
        for(int k=0;k<3;++k)if(int(values[k]==0)!=s.fractional[k])s.fractional[k]=-1;
    }
    return s;
}
inline Edit prepare(const Dataset &d,std::vector<int> rows,const Draft &draft) {
    validate(d);Edit e{selected(d,std::move(rows)),draft};
    for(int c:{draft.cartesian,draft.fractional[0],draft.fractional[1],draft.fractional[2]})
        if(c<Keep || c>Fixed)throw std::invalid_argument("约束选项无效");
    if(!periodic(d) && std::any_of(draft.fractional.begin(),draft.fractional.end(),[](int c){return c==Fixed;}))
        throw std::invalid_argument("晶格方向约束需要有效的三维周期晶胞");
    for(int i:e.rows) {
        e.changed|=draft.cartesian!=Keep && cartesian(d,size_t(i))!=(draft.cartesian==Fixed);
        const auto v=mask(d,size_t(i));const float values[]{v.x,v.y,v.z};
        for(int k=0;k<3;++k)e.changed|=draft.fractional[k]!=Keep && values[k]!=(draft.fractional[k]==Fixed?0:1);
    }
    return e;
}
inline void apply(Dataset &d,const Edit &edit) {
    const auto e=prepare(d,edit.rows,edit.draft);if(!e.changed)return;
    if(e.draft.cartesian!=Keep) {
        auto [it,added]=d.scalarProperties.try_emplace(cartesianProperty,d.atoms.size(),0.);
        (void)added;for(int i:e.rows)it->second[size_t(i)]=e.draft.cartesian==Fixed?1:0;
    }
    if(std::any_of(e.draft.fractional.begin(),e.draft.fractional.end(),[](int c){return c!=Keep;})) {
        auto [it,added]=d.vectorProperties.try_emplace("MoveMask",d.atoms.size(),Vec3{1,1,1});(void)added;
        for(int i:e.rows) {
            auto &v=it->second[size_t(i)];float *values[]{&v.x,&v.y,&v.z};
            for(int k=0;k<3;++k)if(e.draft.fractional[k]!=Keep)*values[k]=e.draft.fractional[k]==Fixed?0.f:1.f;
        }
    }
}
}
