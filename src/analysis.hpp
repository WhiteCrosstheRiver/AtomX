#pragma once
#include "core.hpp"
#include <numeric>
#include <tuple>
namespace atomx {
struct NeighborAnalysis {
    std::vector<uint32_t> coordination, cluster;
    uint32_t clusters = 0;
    uint64_t bonds = 0;
    double meanCoordination = 0;
    std::array<float, 128> pairHistogram{}, rdf{};
    float cutoff = 0;
    bool rdfValid = false;
};
struct DXAResult {
    bool approximate = true;
    uint64_t analyzed = 0, defectAtoms = 0;
    std::vector<uint32_t> coreAtoms;
    std::unordered_map<std::string,uint64_t> structures;
    std::string message;
};

inline NeighborAnalysis neighbors(const Dataset &, float, std::atomic<bool> *);

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
// Linked spatial bins; orthogonal minimum-image convention. Refuse sampled data:
// missing neighbors would make coordination and connected components misleading.
inline NeighborAnalysis neighbors(const Dataset &d, float cutoff,
                                  std::atomic<bool> *cancel = nullptr) {
    NeighborAnalysis r;
    r.cutoff = cutoff;
    r.coordination.resize(d.atoms.size());
    r.cluster.resize(d.atoms.size());
    std::iota(r.cluster.begin(), r.cluster.end(), 0);
    auto root = [&](uint32_t i) {
        while (i != r.cluster[i]) {
            r.cluster[i] = r.cluster[r.cluster[i]];
            i = r.cluster[i];
        }
        return i;
    };
    forEachNeighborPair(d, cutoff, [&](uint32_t i, uint32_t j, double r2) {
        r.coordination[i]++;
        r.coordination[j]++;
        r.bonds++;
        auto binIndex = std::min(127, int(std::sqrt(r2) / cutoff * 128));
        r.pairHistogram[binIndex]++;
        auto ri = root(i), rj = root(j);
        if (ri != rj) r.cluster[std::max(ri, rj)] = std::min(ri, rj);
    }, cancel);
    std::unordered_map<uint32_t, uint32_t> ids;
    for (uint32_t i = 0; i < r.cluster.size(); i++) {
        auto rt = root(i);
        auto [it, inserted] = ids.emplace(rt, uint32_t(ids.size()) + 1);
        r.cluster[i] = rt;
    }
    // Resolve all roots before replacing union-find storage with public component IDs.
    for (auto &c : r.cluster)
        c = ids.at(c);
    r.clusters = uint32_t(ids.size());
    r.meanCoordination = d.atoms.empty() ? 0 : 2. * r.bonds / d.atoms.size();
    // Bulk RDF normalization is meaningful here only for a fully periodic box.
    // Count directed neighbors (2 per pair), normalized by N * density * shell volume.
    double volume = d.cell[0] * d.cell[4] * d.cell[8];
    r.rdfValid = d.pbc[0] && d.pbc[1] && d.pbc[2] && volume > 0 && !d.atoms.empty();
    if (r.rdfValid) {
        double density = d.atoms.size() / volume;
        for (int i = 0; i < 128; ++i) {
            double lo = double(cutoff)*i/128, hi = double(cutoff)*(i+1)/128;
            double shell = (4.0/3.0)*3.141592653589793*(hi*hi*hi-lo*lo*lo);
            r.rdf[i] = float(2.0*r.pairHistogram[i] / (d.atoms.size()*density*shell));
        }
    }
    return r;
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
