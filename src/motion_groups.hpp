#pragma once
#include "authoring.hpp"

// Explicit membership is a particle property, so deletion, history and native
// documents use the same index mapping as other scientific properties.
namespace atomx::motion {
inline constexpr const char *property="AtomX.MotionGroup", *tableName="AtomX.MotionGroups";
inline constexpr size_t groupLimit=512;
struct Group { int id=0; std::string name; size_t count=0; };
inline int id(double value) {
    if (std::isnan(value) || value==0) return 0; // New atoms are unassigned.
    if (!std::isfinite(value) || value<1 || value>INT32_MAX || std::floor(value)!=value)
        throw std::invalid_argument("运动分组编号无效");
    return int(value);
}
inline std::map<int,std::string> names(const Dataset &d) {
    std::map<int,std::string> result;
    for (const auto &t:d.tables) if (t.name==tableName) {
        if (t.columns!=std::vector<std::string>{"ID","Name"}) throw std::invalid_argument("运动分组名称表无效");
        for (const auto &row:t.rows) {
            if (row.size()!=2 || row[1].empty() || row[1].size()>256) throw std::invalid_argument("运动分组名称无效");
            size_t end=0; const int key=std::stoi(row[0],&end);
            if (key<=0 || end!=row[0].size() || !result.emplace(key,row[1]).second)
                throw std::invalid_argument("运动分组名称表编号无效");
        }
    }
    return result;
}
inline const std::vector<double> *memberships(const Dataset &d) {
    const auto found=d.scalarProperties.find(property);
    if (found==d.scalarProperties.end()) return nullptr;
    if (found->second.size()!=d.atoms.size()) throw std::invalid_argument("运动分组属性长度不匹配");
    return &found->second;
}
inline std::vector<Group> catalog(const Dataset &d) {
    const auto *values=memberships(d); if (!values) return {};
    const auto labels=names(d); std::map<int,size_t> counts;
    for (double value:*values) if (const int key=id(value)) ++counts[key];
    if (counts.size()>groupLimit) throw std::invalid_argument("运动分组最多 512 组");
    std::vector<Group> out;
    for (const auto &[key,count]:counts) {
        const auto name=labels.find(key);
        out.push_back({key,name==labels.end()?"分组 "+std::to_string(key):name->second,count});
    }
    return out;
}
inline std::vector<int> members(const Dataset &d,int key) {
    std::vector<int> out; const auto *values=memberships(d);
    if (values && key>0) for (size_t i=0;i<values->size();++i) if (id((*values)[i])==key) out.push_back(int(i));
    return out;
}
inline void setNames(Dataset &d,const std::map<int,std::string> &labels) {
    d.tables.erase(std::remove_if(d.tables.begin(),d.tables.end(),[](const auto &t){return t.name==tableName;}),d.tables.end());
    if (labels.empty()) return;
    DataTable table{tableName,{"ID","Name"},{}};
    for (const auto &[key,name]:labels) table.rows.push_back({std::to_string(key),name});
    d.tables.push_back(std::move(table));
}
inline int prepare(const Dataset &d,const std::vector<int> &selected,const std::string &name) {
    if (d.sampled() || selected.empty() || name.empty() || name.size()>256)
        throw std::invalid_argument("请选择原子并填写分组名称，采样体系不能分组");
    const auto groups=catalog(d); if (groups.size()>=groupLimit) throw std::invalid_argument("运动分组最多 512 组");
    const auto *values=memberships(d); int next=1;
    for (auto group:groups) {
        if (group.id==INT32_MAX) throw std::invalid_argument("运动分组编号已达上限");
        next=std::max(next,group.id+1);
    }
    for (int index:selected) {
        if (index<0 || size_t(index)>=d.atoms.size()) throw std::invalid_argument("所选原子已改变");
        if (values && id((*values)[size_t(index)])) throw std::invalid_argument("所选原子已属于运动分组，请先取消该分组");
    }
    return next;
}
inline void create(Dataset &d,const std::vector<int> &selected,const std::string &name,int key) {
    auto &values=d.scalarProperties[property]; if (values.empty()) values.resize(d.atoms.size(),0);
    for (int index:selected) values[size_t(index)]=key;
    auto labels=names(d); labels[key]=name; setNames(d,labels);
}
inline void erase(Dataset &d,int key) {
    auto found=d.scalarProperties.find(property);
    if (found!=d.scalarProperties.end()) for (auto &value:found->second) if (id(value)==key) value=0;
    auto labels=names(d); labels.erase(key); setNames(d,labels);
}
inline void rename(Dataset &d,int key,const std::string &name) {
    if (name.empty() || name.size()>256 || members(d,key).empty()) throw std::invalid_argument("分组或名称无效");
    auto labels=names(d); labels[key]=name; setNames(d,labels);
}
struct Assignment { std::vector<double> values; std::map<int,std::string> labels; size_t skipped=0; };
// Called only on explicit assignment, off the GUI thread. No all-pairs search.
inline Assignment automatic(size_t count,const std::vector<Bond> &bonds) {
    if (count>size_t(INT32_MAX)) throw std::invalid_argument("体系过大，无法分组");
    std::vector<uint32_t> parent(count),sizes(count,1); std::vector<uint8_t> periodic(count,0);
    for (size_t i=0;i<count;++i) parent[i]=uint32_t(i);
    auto root=[&](uint32_t i) { while (i!=parent[i]) { parent[i]=parent[parent[i]]; i=parent[i]; } return i; };
    for (const auto &b:bonds) {
        if (b.a>=count || b.b>=count) throw std::invalid_argument("显式键索引无效");
        auto a=root(b.a),z=root(b.b);
        if (sizes[a]<sizes[z]) std::swap(a,z);
        const bool boundary=b.image!=std::array<int32_t,3>{};
        if (a!=z) { parent[z]=a; sizes[a]+=sizes[z]; }
        periodic[a]=periodic[a]||periodic[z]||boundary;
    }
    Assignment out; out.values.resize(count,0); std::map<uint32_t,int> assigned;
    for (size_t i=0;i<count;++i) {
        const auto r=root(uint32_t(i));
        if (periodic[r]) { ++out.skipped; continue; }
        if (!assigned.contains(r)) {
            if (assigned.size()>=groupLimit) throw std::invalid_argument("独立片段超过 512 组，请按选择分组");
            const int key=int(assigned.size())+1; assigned[r]=key; out.labels[key]="片段 "+std::to_string(key);
        }
        out.values[i]=assigned.at(r);
    }
    return out;
}
inline void apply(Dataset &d,Assignment assignment) {
    d.scalarProperties[property]=std::move(assignment.values); setNames(d,assignment.labels);
}
inline std::optional<Vec3> massCenter(const Dataset &d,const std::vector<int> &selection) {
    const auto found=d.scalarProperties.find("Mass");
    if (found==d.scalarProperties.end() || found->second.size()!=d.atoms.size()) return {};
    double x=0,y=0,z=0,total=0;
    for (int index:selection) {
        if (index<0 || size_t(index)>=d.atoms.size()) return {};
        const auto m=found->second[size_t(index)]; if (!std::isfinite(m) || m<=0) return {};
        const auto &a=d.atoms[size_t(index)]; x+=a.x*m; y+=a.y*m; z+=a.z*m; total+=m;
    }
    if (total<=0 || !std::isfinite(total)) return {};
    const Vec3 center{float(x/total),float(y/total),float(z/total)};
    if (!std::isfinite(center.x)||!std::isfinite(center.y)||!std::isfinite(center.z)) return {};
    return center;
}
} // namespace atomx::motion
