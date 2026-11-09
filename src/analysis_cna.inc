// ---- Common neighbor analysis: adaptive, interval and bond-based modes ------------
// Fixed-cutoff CNA lives in core.hpp (analyzeCommonNeighbors). The adaptive and interval
// modes work on one particle's 12 or 14 nearest neighbor vectors, so periodic images of a
// particle in a small cell count as separate neighbors.
enum class CnaMode { FixedCutoff = 0, AdaptiveCutoff = 1, IntervalCutoff = 2, BondBased = 3 };

struct CnaOptions {
    CnaMode mode = CnaMode::AdaptiveCutoff;
    double cutoff = 3.2;     // Length units of the input; fixed-cutoff mode only.
    bool colorByType = true; // Publish a Color for every particle.
};

namespace cna_detail {
enum Signature { NoSignature, S421, S422, S444, S555, S666 };

// Bonds between the neighbors of one center; bonded[k] has bit j set when neighbors k and j
// are bonded. Together with the center these are the common neighbors of bond (center, k).
struct Shell {
    int count = 0;
    std::array<uint32_t, 14> bonded{};
};

// Longest chain of connected bonds, each bond used once. At most 6 bonds reach this.
inline int longestChain(const std::array<std::array<uint8_t, 2>, 6> &edges, int edgeCount) {
    int best = 0;
    const auto walk = [&](auto &&self, int vertex, uint32_t used, int length) -> void {
        best = std::max(best, length);
        for (int e = 0; e < edgeCount; ++e) {
            if (used & (1u << e)) continue;
            if (edges[e][0] == vertex) self(self, edges[e][1], used | (1u << e), length + 1);
            else if (edges[e][1] == vertex) self(self, edges[e][0], used | (1u << e), length + 1);
        }
    };
    for (int e = 0; e < edgeCount; ++e) {
        walk(walk, edges[e][0], 0, 0);
        walk(walk, edges[e][1], 0, 0);
    }
    return best;
}

inline Signature bondSignature(const Shell &shell, int k) {
    const uint32_t common = shell.bonded[size_t(k)];
    const int commonCount = std::popcount(common);
    if (commonCount < 4 || commonCount > 6) return NoSignature;
    std::array<std::array<uint8_t, 2>, 6> edges{};
    int edgeCount = 0;
    for (int j = 0; j < shell.count; ++j) {
        if (!(common & (1u << j))) continue;
        uint32_t higher = shell.bonded[size_t(j)] & common & ~((2u << j) - 1u);
        while (higher) {
            const int l = std::countr_zero(higher);
            higher &= higher - 1;
            if (edgeCount == 6) return NoSignature;
            edges[size_t(edgeCount++)] = {uint8_t(j), uint8_t(l)};
        }
    }
    const int chain = longestChain(edges, edgeCount);
    if (commonCount == 4 && edgeCount == 2) return chain == 1 ? S421 : chain == 2 ? S422 : NoSignature;
    if (commonCount == 4 && edgeCount == 4) return chain == 4 ? S444 : NoSignature;
    if (commonCount == 5 && edgeCount == 5) return chain == 5 ? S555 : NoSignature;
    if (commonCount == 6 && edgeCount == 6) return chain == 6 ? S666 : NoSignature;
    return NoSignature;
}

// 0 Other, 1 FCC, 2 HCP, 3 BCC, 4 icosahedral.
inline uint8_t classify(const Shell &shell) {
    std::array<int, 6> counts{};
    for (int k = 0; k < shell.count; ++k) {
        const Signature signature = bondSignature(shell, k);
        if (signature == NoSignature) return 0;
        ++counts[signature];
    }
    if (shell.count == 12) {
        if (counts[S421] == 12) return 1;
        if (counts[S421] == 6 && counts[S422] == 6) return 2;
        if (counts[S555] == 12) return 4;
    } else if (shell.count == 14 && counts[S444] == 6 && counts[S666] == 8) {
        return 3;
    }
    return 0;
}

inline double pairDistanceSquared(const NeighborVector &a, const NeighborVector &b) {
    const double x = a.vector[0] - b.vector[0], y = a.vector[1] - b.vector[1], z = a.vector[2] - b.vector[2];
    return x * x + y * y + z * z;
}

// Per-particle length scales as written by Larsen (arXiv:2003.08879), who corrects the bcc
// expression of Stukowski's original description.
inline double lengthScale(const std::vector<NeighborVector> &list, int count) {
    double first = 0, second = 0;
    if (count == 12) {
        for (int k = 0; k < 12; ++k) first += std::sqrt(list[size_t(k)].distanceSquared);
        return first / 12;
    }
    for (int k = 0; k < 8; ++k) first += std::sqrt(list[size_t(k)].distanceSquared);
    for (int k = 8; k < 14; ++k) second += std::sqrt(list[size_t(k)].distanceSquared);
    return (2 / std::sqrt(3.0) * first + second) / 14;
}

// a-CNA: one cutoff per particle, midway between the shells; 12-neighbor structures first.
inline uint8_t adaptive(const std::vector<NeighborVector> &list) {
    for (int count : {12, 14}) {
        if (int(list.size()) < count) break;
        const double cutoff = (1 + std::sqrt(2.0)) / 2 * lengthScale(list, count);
        const double squared = cutoff * cutoff;
        Shell shell;
        shell.count = count;
        for (int a = 0; a < count; ++a)
            for (int b = a + 1; b < count; ++b)
                if (pairDistanceSquared(list[size_t(a)], list[size_t(b)]) < squared) {
                    shell.bonded[size_t(a)] |= 1u << b;
                    shell.bonded[size_t(b)] |= 1u << a;
                }
        if (const uint8_t type = classify(shell)) return type;
    }
    return 0;
}

// i-CNA: every threshold that puts exactly 12 or 14 particles around the center and stays
// below Larsen's false-positive bound is examined; the bond graph only changes where a
// neighbor-neighbor distance is crossed. The structure with the widest interval wins.
inline uint8_t interval(const std::vector<NeighborVector> &list) {
    uint8_t bestType = 0;
    double bestWidth = 0;
    struct Event { double distance; uint8_t a, b; };
    std::vector<Event> events;
    for (int count : {12, 14}) {
        if (int(list.size()) < count) break;
        const double lower = std::sqrt(list[size_t(count - 1)].distanceSquared);
        double upper = (1 + 2 * std::sqrt(2.0)) / 3 * lengthScale(list, count);
        if (int(list.size()) > count) upper = std::min(upper, std::sqrt(list[size_t(count)].distanceSquared));
        if (!(lower < upper)) continue;
        Shell shell;
        shell.count = count;
        events.clear();
        for (int a = 0; a < count; ++a)
            for (int b = a + 1; b < count; ++b) {
                const double distance = std::sqrt(pairDistanceSquared(list[size_t(a)], list[size_t(b)]));
                if (distance <= lower) {
                    shell.bonded[size_t(a)] |= 1u << b;
                    shell.bonded[size_t(b)] |= 1u << a;
                } else if (distance < upper) {
                    events.push_back({distance, uint8_t(a), uint8_t(b)});
                }
            }
        std::sort(events.begin(), events.end(), [](const Event &x, const Event &y) { return x.distance < y.distance; });
        double start = lower, runWidth = 0;
        uint8_t runType = 0;
        for (size_t e = 0; e <= events.size(); ++e) {
            const double end = e < events.size() ? events[e].distance : upper;
            if (end > start) {
                const uint8_t type = classify(shell);
                runWidth = type == runType ? runWidth + (end - start) : end - start;
                runType = type;
                if (type && runWidth > bestWidth) { bestWidth = runWidth; bestType = type; }
                start = end;
            }
            if (e < events.size()) {
                shell.bonded[events[e].a] |= 1u << events[e].b;
                shell.bonded[events[e].b] |= 1u << events[e].a;
            }
        }
    }
    return bestType;
}

// AtomX's own display palette for the five structure types.
inline Vec3 structureColor(uint8_t type) {
    static constexpr std::array<std::array<float, 3>, 5> colors{{
        {.95f, .95f, .95f}, {.4f, 1.f, .4f}, {1.f, .4f, .4f}, {.4f, .4f, 1.f}, {.95f, .8f, .2f}}};
    const auto &c = colors[type < 5 ? type : 0];
    return Vec3{c[0], c[1], c[2]};
}

// Writes the three CNA indices of every bond into data.bondProperties. adjacency holds sorted
// neighbor lists; remap maps a dataset particle to its adjacency slot (-1 when excluded).
// Index 1 counts common neighbors, 2 the bonds among them and 3 the longest chain of those
// bonds, each used once. The chain is searched for up to 6 bonds and is -1 above that.
// Self bonds and bonds with an excluded particle get 0 0 0.
inline void publishBondIndices(Dataset &data, const std::vector<std::vector<uint32_t>> &adjacency,
                               const std::vector<int64_t> &remap, std::atomic<bool> *cancel) {
    const size_t bondCount = data.bonds.size();
    std::array<std::vector<double>, 3> columns;
    for (auto &column : columns) column.assign(bondCount, 0.0);
    std::atomic<size_t> next{0};
    std::atomic<bool> stopped{false};
    const auto work = [&] {
        std::vector<uint32_t> common;
        for (;;) {
            const size_t start = next.fetch_add(4096);
            if (start >= bondCount || stopped) return;
            if (cancel && *cancel) { stopped = true; return; }
            for (size_t row = start; row < std::min(bondCount, start + 4096); ++row) {
                const int64_t a = remap[data.bonds[row].a], b = remap[data.bonds[row].b];
                if (a < 0 || b < 0 || a == b) continue;
                common.clear();
                std::set_intersection(adjacency[size_t(a)].begin(), adjacency[size_t(a)].end(), adjacency[size_t(b)].begin(),
                                      adjacency[size_t(b)].end(), std::back_inserter(common));
                std::array<std::array<uint8_t, 2>, 6> edges{};
                int edgeCount = 0;
                for (size_t i = 0; i < common.size(); ++i)
                    for (size_t j = i + 1; j < common.size(); ++j) {
                        if (!std::binary_search(adjacency[common[i]].begin(), adjacency[common[i]].end(), common[j])) continue;
                        if (edgeCount < 6) edges[size_t(edgeCount)] = {uint8_t(i), uint8_t(j)};
                        ++edgeCount;
                    }
                columns[0][row] = double(common.size());
                columns[1][row] = double(edgeCount);
                columns[2][row] = edgeCount <= 6 ? double(longestChain(edges, edgeCount)) : -1.0;
            }
        }
    };
    const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    if (bondCount < 4096 || threads == 1) work();
    else {
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; ++t) pool.emplace_back(work);
        for (auto &thread : pool) thread.join();
    }
    if (stopped || (cancel && *cancel)) throw std::runtime_error("Cancelled");
    data.bondProperties["CNA indices.1"] = std::move(columns[0]);
    data.bondProperties["CNA indices.2"] = std::move(columns[1]);
    data.bondProperties["CNA indices.3"] = std::move(columns[2]);
}
} // namespace cna_detail

inline void computeCommonNeighborAnalysis(Dataset &data, const CnaOptions &options,
                                          const std::vector<uint8_t> *selection, std::atomic<bool> *cancel) {
    using namespace cna_detail;
    Dataset subset;
    std::vector<uint32_t> original;
    const Dataset &input = selectedSubset(data, selection, subset, original);
    const size_t count = input.atoms.size();
    std::vector<uint8_t> structure(count, 0);
    CNAResult bondResult;
    if (options.mode == CnaMode::FixedCutoff) {
        structure = analyzeCommonNeighbors(input, options.cutoff, cancel).structure;
    } else if (options.mode == CnaMode::BondBased) {
        if (data.bonds.empty())
            throw std::runtime_error("Bond-based CNA needs bonds; add Create bonds or load a file with bonds first");
        std::vector<int64_t> remap(data.atoms.size(), -1);
        for (size_t i = 0; i < count; ++i) remap[selection ? original[i] : i] = int64_t(i);
        std::vector<std::vector<uint32_t>> adjacency(count);
        for (const Bond &bond : data.bonds) {
            if (bond.a >= data.atoms.size() || bond.b >= data.atoms.size())
                throw std::runtime_error("Bond references a particle outside the dataset");
            const int64_t a = remap[bond.a], b = remap[bond.b];
            if (a < 0 || b < 0 || a == b) continue;
            adjacency[size_t(a)].push_back(uint32_t(b));
            adjacency[size_t(b)].push_back(uint32_t(a));
        }
        // The classifier validates the neighbor limits that the per-bond search relies on.
        bondResult = classifyCommonNeighborAdjacency(adjacency, cancel);
        structure = bondResult.structure;
        for (auto &neighbors : adjacency) {
            std::sort(neighbors.begin(), neighbors.end());
            neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
        }
        publishBondIndices(data, adjacency, remap, cancel);
    } else {
        const auto lists = nearestNeighborsWithImages(input, 15, cancel);
        std::atomic<size_t> next{0};
        std::atomic<bool> stopped{false};
        const bool useInterval = options.mode == CnaMode::IntervalCutoff;
        const auto work = [&] {
            for (;;) {
                const size_t start = next.fetch_add(1024);
                if (start >= count || stopped) return;
                if (cancel && *cancel) { stopped = true; return; }
                for (size_t i = start; i < std::min(count, start + 1024); ++i)
                    structure[i] = useInterval ? interval(lists[i]) : adaptive(lists[i]);
            }
        };
        const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
        if (count < 4096 || threads == 1) work();
        else {
            std::vector<std::thread> pool;
            for (unsigned t = 0; t < threads; ++t) pool.emplace_back(work);
            for (auto &thread : pool) thread.join();
        }
        if (stopped || (cancel && *cancel)) throw std::runtime_error("Cancelled");
    }

    const size_t total = data.atoms.size();
    std::vector<double> types(total, 0.0);
    std::array<uint64_t, 5> counts{};
    for (size_t i = 0; i < count; ++i) {
        types[selection ? original[i] : i] = double(structure[i]);
        ++counts[structure[i]];
    }
    counts[0] += total - count;
    if (options.colorByType) {
        data.particleColors.resize(total);
        for (size_t i = 0; i < total; ++i) data.particleColors[i] = structureColor(uint8_t(types[i]));
    }
    data.scalarProperties["Structure Type"] = std::move(types);
    static constexpr std::array<const char *, 5> names{"Other", "FCC", "HCP", "BCC", "Icosahedral"};
    DataTable table;
    table.name = "Structure analysis results";
    table.columns = {"Structure", "Count", "Fraction", "Id"};
    for (size_t id = 0; id < names.size(); ++id) {
        data.globalAttributes[std::string("CommonNeighborAnalysis.counts.") + names[id]] = double(counts[id]);
        table.rows.push_back({names[id], std::to_string(counts[id]),
                              total ? std::to_string(double(counts[id]) / double(total)) : "0", std::to_string(id)});
    }
    data.tables.push_back(std::move(table));
    if (options.mode == CnaMode::BondBased) {
        // Totals per distinct particle pair; the values per bond are in bondProperties.
        // Keys of the shared engine count path steps; the labels are the conventional indices.
        const std::array<std::pair<std::tuple<int, int, int>, const char *>, 5> known{{
            {{4, 2, 1}, "421"}, {{4, 2, 2}, "422"}, {{4, 4, 3}, "444"}, {{5, 5, 4}, "555"}, {{6, 6, 5}, "666"}}};
        DataTable signatures;
        signatures.name = "CNA bond signatures";
        signatures.columns = {"CNA indices", "Bonds"};
        uint64_t listed = 0, all = 0;
        for (const auto &[key, value] : bondResult.bondSignatureCounts) all += value / 2;
        for (const auto &[key, label] : known) {
            const auto found = bondResult.bondSignatureCounts.find(key);
            const uint64_t bonds = found == bondResult.bondSignatureCounts.end() ? 0 : found->second / 2;
            listed += bonds;
            signatures.rows.push_back({label, std::to_string(bonds)});
        }
        signatures.rows.push_back({"Other", std::to_string(all - listed)});
        data.tables.push_back(std::move(signatures));
    }
}
