#pragma once
#include "hydrogen_adjust.hpp"
#include <climits>

namespace atomx::insertion {
struct Edit {
    size_t atomCount=0,bondCount=0,index=0;
    Bond original{};
    Vec3 position{};
    std::string element;
    int order=1;
};
// Split a direct bond at its midpoint. Heavy atom coordinates and unrelated
// topology remain unchanged; the two replacement bonds use the sketch order.
inline Edit prepare(const Dataset &data,size_t index,const std::string &element,int order) {
    hydrogens::validateRows(data);
    if(!elements::find(element) || order<1 || order>3)throw std::invalid_argument("请选择有效元素和单 / 双 / 三键");
    if(index>=data.bonds.size())throw std::invalid_argument("请选择已有的直接键");
    const auto b=data.bonds[index];
    if(b.image!=std::array<int32_t,3>{})throw std::invalid_argument("跨周期键暂不支持插入原子");
    if(b.a>=data.atoms.size() || b.b>=data.atoms.size() || b.a==b.b || b.order<1 || b.order>4)
        throw std::invalid_argument("键的端点或键级无效");
    if(data.bonds.size()>=interactiveBondBudget || data.atoms.size()>=size_t(INT_MAX))
        throw std::invalid_argument("原子或键数已达到交互上限");
    const auto a=hydrogens::point(data,int(b.a)),c=hydrogens::point(data,int(b.b));
    const auto p=authoring::add(authoring::scale(a,.5),authoring::scale(c,.5));
    if(!hydrogens::finite(a) || !hydrogens::finite(c) || !hydrogens::finite(p) ||
        authoring::length(authoring::sub(a,c))<.002)
        throw std::invalid_argument("键长退化，无法插入原子");
    for(size_t i=0;i<data.atoms.size();++i) {
        const auto at=hydrogens::point(data,int(i));
        if(hydrogens::finite(at) && authoring::length(authoring::sub(at,p))<.001)
            throw std::invalid_argument("键中点已存在原子，请先处理重合位置");
    }
    return {data.atoms.size(),data.bonds.size(),index,b,p,element,order};
}
inline int apply(Dataset &data,const Edit &edit) {
    const auto checked=prepare(data,edit.index,edit.element,edit.order);
    if(checked.atomCount!=edit.atomCount || checked.bondCount!=edit.bondCount ||
        checked.original.a!=edit.original.a || checked.original.b!=edit.original.b ||
        checked.original.order!=edit.original.order || authoring::length(authoring::sub(checked.position,edit.position))>1e-6)
        throw std::invalid_argument("插入目标已改变，请重新选择键");
    const auto type=authoring::speciesIndex(data,edit.element);
    const int added=int(data.atoms.size());
    data.atoms.push_back({edit.position.x,edit.position.y,edit.position.z,type});
    hydrogens::extendRows(data,size_t(added));
    data.bonds[edit.index]={edit.original.a,uint32_t(added),{},uint8_t(edit.order)};
    data.bonds.push_back({uint32_t(added),edit.original.b,{},uint8_t(edit.order)});
    return added;
}
}
