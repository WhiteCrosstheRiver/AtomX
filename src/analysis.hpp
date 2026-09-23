#pragma once
#include "core.hpp"
#include <numeric>
#include <tuple>
namespace atomx {
struct DXAResult {
    bool approximate = true;
    uint64_t analyzed = 0, defectAtoms = 0;
    std::vector<uint32_t> coreAtoms;
    std::unordered_map<std::string,uint64_t> structures;
    std::string message;
};

struct StructureAnalysis {
    std::vector<uint8_t> structure;
    std::unordered_map<std::string, uint64_t> counts;
    std::string propertyName;
};

// A deterministic coordination-based fallback for structure classifiers. It
// is intentionally conservative: exact crystallographic classifiers can be
// layered on this result later without changing the published property name.
inline StructureAnalysis classifyByCoordination(const Dataset &d, float cutoff,
                                                std::atomic<bool> *cancel = nullptr) {
    auto n = neighbors(d, cutoff, cancel);
    StructureAnalysis r;
    r.propertyName = "Structure Type";
    r.structure.resize(d.atoms.size());
    for (size_t i = 0; i < d.atoms.size(); ++i) {
        if (cancel && *cancel) throw std::runtime_error("Cancelled");
        uint8_t id = n.coordination[i] == 4 ? 4 : n.coordination[i] == 8 ? 3 :
                     n.coordination[i] == 12 ? 1 : 0;
        r.structure[i] = id;
    }
    r.counts["Other"] = 0; r.counts["FCC"] = 0; r.counts["BCC"] = 0;
    r.counts["Cubic diamond"] = 0;
    for (auto id : r.structure)
        ++r.counts[id == 1 ? "FCC" : id == 3 ? "BCC" : id == 4 ? "Cubic diamond" : "Other"];
    return r;
}

inline void publishStructure(Dataset &d, const StructureAnalysis &analysis) {
    std::vector<double> values(analysis.structure.begin(), analysis.structure.end());
    d.scalarProperties[analysis.propertyName] = std::move(values);
}
inline DXAResult dxaApproximate(const Dataset &d, float cutoff, bool selectedOnly=false,
                                const std::vector<uint8_t>* selection=nullptr) {
    if (d.sampled()) throw std::runtime_error("DXA requires full-resolution data; increase the import budget.");
    if (d.atoms.size() > 2000000) throw std::runtime_error("DXA currently limited to 2 million atoms.");
    auto n = neighbors(d, cutoff);
    DXAResult r; r.analyzed=d.atoms.size(); r.structures["Other"]=0; r.structures["FCC"]=0; r.structures["HCP"]=0; r.structures["BCC"]=0; r.structures["Cubic diamond"]=0;
    for (size_t i=0;i<d.atoms.size();++i) {
        if (selectedOnly && (!selection || !(*selection)[i])) continue;
        auto c=n.coordination[i]; if(c==12) r.structures["FCC"]++; else if(c==8) r.structures["BCC"]++; else if(c==4) r.structures["Cubic diamond"]++; else {r.structures["Other"]++;r.coreAtoms.push_back(uint32_t(i));}
    }
    r.defectAtoms=r.coreAtoms.size();
    r.message="Approximate DXA prepass: coordination-based defect cores. Burgers vectors, line network and defect mesh are not computed yet.";
    return r;
}
} // namespace atomx
