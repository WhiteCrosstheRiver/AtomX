#pragma once
#include "core.hpp"

namespace atomx::creation {
// Coordinates belong to primary-cell atoms. Shared endpoints are transformed
// once, including when several selected edges cross a periodic boundary.
inline std::vector<int> selectedAtoms(const Dataset &d,std::vector<int> atoms,const std::vector<int> &rows) {
    for(int row:rows) {
        if(row<0 || size_t(row)>=d.bonds.size())throw std::invalid_argument("Invalid bond selection");
        const auto &b=d.bonds[size_t(row)];
        if(b.a>=d.atoms.size() || b.b>=d.atoms.size())throw std::invalid_argument("Invalid bond endpoints");
        atoms.push_back(int(b.a));atoms.push_back(int(b.b));
    }
    for(int atom:atoms)if(atom<0 || size_t(atom)>=d.atoms.size())throw std::invalid_argument("Invalid atom selection");
    std::sort(atoms.begin(),atoms.end());atoms.erase(std::unique(atoms.begin(),atoms.end()),atoms.end());
    return atoms;
}
// Edit the actual bond rows, including periodic images. Never infer a direct
// bond from endpoints: a pair can have several different image connections.
inline std::vector<int> editBonds(Dataset &d,std::vector<int> rows,int order) {
    if(order<0 || order>4)throw std::invalid_argument("Invalid bond order");
    std::sort(rows.begin(),rows.end());rows.erase(std::unique(rows.begin(),rows.end()),rows.end());
    for(int row:rows)if(row<0 || size_t(row)>=d.bonds.size())throw std::invalid_argument("Invalid bond selection");
    std::vector<int> affected;
    for(int row:rows) {
        const auto &b=d.bonds[size_t(row)];
        affected.push_back(int(b.a));affected.push_back(int(b.b));
    }
    std::sort(affected.begin(),affected.end());affected.erase(std::unique(affected.begin(),affected.end()),affected.end());
    if(order)for(int row:rows)d.bonds[size_t(row)].order=uint8_t(order);
    else {
        size_t next=0,destination=0;
        for(size_t i=0;i<d.bonds.size();++i) {
            if(next<rows.size() && size_t(rows[next])==i) {++next;continue;}
            d.bonds[destination++]=d.bonds[i];
        }
        d.bonds.resize(destination);
    }
    return affected;
}
} // namespace atomx::creation
