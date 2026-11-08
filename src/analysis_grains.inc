// Grain segmentation and elastic strain, both built on the PTM classification in
// analysis_ptm.inc. Included inside namespace atomx by core.hpp. OVITO's Grain
// segmentation and Elastic strain calculation modifiers are the workflow references;
// the numerical choices here are AtomX's own and are listed in src/references.hpp.

namespace grain_detail {
using ptm_detail::Quat;
inline Quat conjugate(const Quat &q) { return {q[0], -q[1], -q[2], -q[3]}; }
inline double quatDot(const Quat &a, const Quat &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; }
inline Quat normalized(Quat q) {
    const double n = std::sqrt(quatDot(q, q));
    if (n > 0) for (auto &c : q) c /= n;
    return q;
}
// Rotation matrix (row-major) to a unit quaternion (w, x, y, z).
inline Quat fromMatrix(const std::array<double, 9> &m) {
    Quat q;
    const double trace = m[0] + m[4] + m[8];
    if (trace > 0) {
        const double s = 2 * std::sqrt(1 + trace);
        q = {s / 4, (m[7] - m[5]) / s, (m[2] - m[6]) / s, (m[3] - m[1]) / s};
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const double s = 2 * std::sqrt(1 + m[0] - m[4] - m[8]);
        q = {(m[7] - m[5]) / s, s / 4, (m[1] + m[3]) / s, (m[2] + m[6]) / s};
    } else if (m[4] > m[8]) {
        const double s = 2 * std::sqrt(1 + m[4] - m[0] - m[8]);
        q = {(m[2] - m[6]) / s, (m[1] + m[3]) / s, s / 4, (m[5] + m[7]) / s};
    } else {
        const double s = 2 * std::sqrt(1 + m[8] - m[0] - m[4]);
        q = {(m[3] - m[1]) / s, (m[2] + m[6]) / s, (m[5] + m[7]) / s, s / 4};
    }
    return normalized(q);
}
// Rotation taking the cubic template frame (FCC or cubic diamond) to the hexagonal one
// (HCP or hexagonal diamond): [111] goes to z and one in-plane direction to the matching
// hexagonal neighbor direction. q_cubic = q_hexagonal * rotation, up to hexagonal symmetry.
inline const Quat &cubicToHexagonal(bool diamond) {
    static const Quat fcc = [] {
        const double a = 1 / std::sqrt(2.0), b = 1 / std::sqrt(6.0), c = 1 / std::sqrt(3.0);
        return fromMatrix({a, -a, 0, b, b, -2 * b, c, c, c});
    }();
    static const Quat dia = [] {
        const double a = 1 / std::sqrt(2.0), b = 1 / std::sqrt(6.0), c = 1 / std::sqrt(3.0);
        return fromMatrix({0, a, -a, -2 * b, b, b, c, c, c});
    }();
    return diamond ? dia : fcc;
}
// Smallest rotation angle (degrees) between two orientations under a lattice rotation group.
inline double misorientation(const Quat &a, const Quat &b, const std::vector<Quat> &group) {
    double best = 0;
    for (const auto &g : group) best = std::max(best, std::abs(quatDot(a, ptm_detail::multiply(b, g))));
    return 2 * std::acos(std::min(1.0, best)) * 180 / 3.14159265358979323846;
}
inline bool isCubicPartner(int type) { return type == 1 || type == 6; }
inline bool isHexagonalPartner(int type) { return type == 2 || type == 7; }
// FCC pairs with HCP, cubic with hexagonal diamond.
inline bool stackingPartners(int a, int b) {
    return (a == 1 && b == 2) || (a == 2 && b == 1) || (a == 6 && b == 7) || (a == 7 && b == 6);
}
// Misorientation between a cubic-phase and a hexagonal-phase atom, expressed in the
// cubic frame: the hexagonal orientation is tried with each of its 12 symmetry operations.
inline double crossMisorientation(const Quat &cubic, const Quat &hexagonal, bool diamond) {
    const auto &lib = ptm_detail::library();
    double best = 180;
    for (const auto &h : lib.hexagonal) {
        const Quat equivalent = ptm_detail::multiply(ptm_detail::multiply(hexagonal, h), cubicToHexagonal(diamond));
        best = std::min(best, misorientation(cubic, equivalent, lib.cubic));
    }
    return best;
}
inline const std::vector<Quat> &groupOf(int type) {
    const auto &lib = ptm_detail::library();
    if (type == 2 || type == 7 || type == 8) return lib.hexagonal;
    if (type == 4) return lib.icosahedral;
    return lib.cubic;
}
inline Vec3 grainColor(size_t id) {
    // Golden-ratio hue steps with fixed saturation and value: deterministic and distinct.
    const double h = std::fmod(0.13 + double(id) * 0.6180339887498949, 1.0) * 6, s = .65, v = .92;
    const int sector = int(h);
    const double f = h - sector, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    double r = v, g = t, b = p;
    switch (sector % 6) {
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        case 5: r = v; g = p; b = q; break;
        default: break;
    }
    return {float(r), float(g), float(b)};
}
struct UnionFind {
    std::vector<uint32_t> parent;
    explicit UnionFind(size_t n) : parent(n) { for (size_t i = 0; i < n; ++i) parent[i] = uint32_t(i); }
    uint32_t find(uint32_t i) {
        while (parent[i] != i) { parent[i] = parent[parent[i]]; i = parent[i]; }
        return i;
    }
    bool unite(uint32_t a, uint32_t b) {
        a = find(a); b = find(b);
        if (a == b) return false;
        parent[b] = a;
        return true;
    }
};
} // namespace grain_detail

enum class GrainAlgorithm { GraphClusteringAuto, GraphClusteringManual, MinimumSpanningTree };
struct GrainSegmentationOptions {
    GrainAlgorithm algorithm = GrainAlgorithm::GraphClusteringAuto;
    double logThreshold = 0;   // Natural log of the largest merge distance kept (manual modes).
    size_t minGrainSize = 100; // Atoms; smaller clusters are dissolved into orphans.
    bool adoptOrphans = true;
    bool handleStackingFaults = true;
    bool colorParticles = true;
};
struct GrainMerge {
    double distance;      // NPS distance (graph clustering) or misorientation in degrees (MST).
    uint32_t a, b;        // Representative atoms of the two merged clusters.
    uint32_t smallerSize; // Crystalline atoms in the smaller cluster.
};

// Neighbor-graph edges between crystalline atoms whose phases may share a grain, with
// their misorientation in degrees. Each unordered pair appears once.
struct GrainEdge { uint32_t a, b; double misorientation; };

inline void computeGrainSegmentation(Dataset &data, const GrainSegmentationOptions &options, std::atomic<bool> *cancel) {
    using namespace grain_detail;
    const size_t n = data.atoms.size();
    const auto property = [&](const char *name) -> const std::vector<double> & {
        const auto it = data.scalarProperties.find(name);
        if (it == data.scalarProperties.end() || it->second.size() != n)
            throw std::runtime_error("Grain segmentation needs the Structure Type and Orientation outputs of "
                                     "Polyhedral template matching (enable Output orientations)");
        return it->second;
    };
    const auto &structure = property("Structure Type");
    const auto &ox = property("Orientation.X"), &oy = property("Orientation.Y"), &oz = property("Orientation.Z"),
               &ow = property("Orientation.W");
    if (!std::isfinite(options.logThreshold) && options.algorithm != GrainAlgorithm::GraphClusteringAuto)
        throw std::runtime_error("Grain segmentation needs a finite merge threshold");
    if (options.minGrainSize == 0) throw std::runtime_error("Minimum grain size must be at least 1");
    if (n > 20000000) throw std::runtime_error("Grain segmentation is limited to 20 million particles");
    std::vector<int> phase(n, 0);
    std::vector<Quat> orientation(n, Quat{1, 0, 0, 0});
    for (size_t i = 0; i < n; ++i) {
        const double t = structure[i];
        if (!(t >= 0 && t <= 8) || t != std::floor(t))
            throw std::runtime_error("Structure Type values must be PTM structure codes 0-8");
        phase[i] = int(t);
        if (!phase[i]) continue;
        const Quat q{ow[i], ox[i], oy[i], oz[i]};
        const double length = std::sqrt(quatDot(q, q));
        if (!(length > .5)) phase[i] = 0; // No orientation: treat as non-crystalline.
        else orientation[i] = normalized(q);
    }
    static constexpr std::array<size_t, 9> shell{12, 12, 12, 14, 12, 6, 4, 4, 3};
    const auto lists = nearestNeighborsWithImages(data, 14, cancel);
    const auto neighborsOf = [&](size_t i) { return std::min(lists[i].size(), shell[size_t(phase[i])]); };

    // Edges. A hexagonal-phase atom at a coherent twin boundary has cubic neighbors on both
    // sides with different orientations; its cross-phase edges are dropped so that the twins
    // stay separate grains and the boundary atom is adopted by one of them later.
    constexpr double twinTolerance = 15; // Degrees between cubic neighbors of one hexagonal atom.
    std::vector<uint8_t> crossAllowed(n, 1);
    if (options.handleStackingFaults)
        for (size_t i = 0; i < n; ++i) {
            if (!isHexagonalPartner(phase[i])) continue;
            const Quat *first = nullptr;
            for (size_t k = 0; k < neighborsOf(i) && crossAllowed[i]; ++k) {
                const auto j = lists[i][k].index;
                if (!stackingPartners(phase[i], phase[j])) continue;
                if (!first) first = &orientation[j];
                else if (misorientation(*first, orientation[j], ptm_detail::library().cubic) > twinTolerance) crossAllowed[i] = 0;
            }
        }
    // Twin-boundary cores are left out of clustering entirely; orphan adoption gives them to
    // one of the adjacent twins.
    for (size_t i = 0; i < n; ++i) if (!crossAllowed[i]) phase[i] = 0;
    std::vector<GrainEdge> edges;
    for (size_t i = 0; i < n; ++i) {
        if ((i & 16383) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
        if (!phase[i]) continue;
        for (size_t k = 0; k < neighborsOf(i); ++k) {
            const uint32_t j = lists[i][k].index;
            if (j == i || !phase[j]) continue;
            // Keep each pair once: from the lower index, or from i when j does not list i.
            bool listedBack = false;
            for (size_t m = 0; m < neighborsOf(j) && !listedBack; ++m) listedBack = lists[j][m].index == i;
            if (listedBack && j < i) continue;
            double angle;
            if (phase[i] == phase[j]) angle = misorientation(orientation[i], orientation[j], groupOf(phase[i]));
            else if (options.handleStackingFaults && stackingPartners(phase[i], phase[j])) {
                const uint32_t hex = isHexagonalPartner(phase[i]) ? uint32_t(i) : j, cub = hex == i ? j : uint32_t(i);
                if (!crossAllowed[hex]) continue;
                angle = crossMisorientation(orientation[cub], orientation[hex], phase[cub] == 6);
            } else continue;
            edges.push_back({uint32_t(i), j, angle});
        }
    }

    // Hierarchical clustering into a merge list.
    std::vector<GrainMerge> merges;
    std::vector<uint32_t> clusterSize(n, 0);
    for (size_t i = 0; i < n; ++i) clusterSize[i] = phase[i] ? 1 : 0;
    if (options.algorithm == GrainAlgorithm::MinimumSpanningTree) {
        std::sort(edges.begin(), edges.end(), [](const GrainEdge &a, const GrainEdge &b) {
            return a.misorientation < b.misorientation || (a.misorientation == b.misorientation && (a.a < b.a || (a.a == b.a && a.b < b.b)));
        });
        UnionFind sets(n);
        for (const auto &e : edges) {
            const uint32_t ra = sets.find(e.a), rb = sets.find(e.b);
            if (ra == rb) continue;
            merges.push_back({e.misorientation, ra, rb, std::min(clusterSize[ra], clusterSize[rb])});
            sets.unite(ra, rb);
            clusterSize[ra] += clusterSize[rb];
        }
    } else {
        // Node pair sampling (Bonald et al.): edge weight exp(-theta^2/3) with theta in degrees,
        // node weight = summed incident edge weight, distance d(a,b) = w(a) w(b) / (W w(a,b)),
        // merged with the nearest-neighbor chain algorithm (d is reducible).
        std::vector<std::unordered_map<uint32_t, double>> adjacency(n);
        std::vector<double> weight(n, 0);
        double total = 0;
        for (const auto &e : edges) {
            const double w = std::exp(-e.misorientation * e.misorientation / 3);
            if (!(w > 0)) continue;
            adjacency[e.a][e.b] += w;
            adjacency[e.b][e.a] += w;
            weight[e.a] += w;
            weight[e.b] += w;
            total += 2 * w;
        }
        std::vector<uint8_t> active(n, 0);
        for (size_t i = 0; i < n; ++i) active[i] = !adjacency[i].empty();
        const auto distance = [&](uint32_t a, uint32_t b, double w) { return weight[a] * weight[b] / (total * w); };
        std::vector<uint32_t> chain;
        size_t steps = 0;
        // A chain that empties after a merge restarts from the same node until its whole
        // connected component has been merged into one cluster.
        for (uint32_t start = 0; start < n; ++start)
          while (active[start]) {
            chain.assign(1, start);
            while (!chain.empty()) {
                if ((++steps & 16383) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
                const uint32_t a = chain.back();
                if (adjacency[a].empty()) { active[a] = 0; chain.pop_back(); continue; }
                const uint32_t previous = chain.size() > 1 ? chain[chain.size() - 2] : UINT32_MAX;
                uint32_t best = UINT32_MAX;
                double bestDistance = std::numeric_limits<double>::infinity();
                for (const auto &[c, w] : adjacency[a]) {
                    const double d = distance(a, c, w);
                    if (d < bestDistance || (d == bestDistance && (c == previous || (best != previous && c < best)))) {
                        bestDistance = d;
                        best = c;
                    }
                }
                if (best != previous) { chain.push_back(best); continue; }
                // Reciprocal nearest neighbors: merge the smaller adjacency into the larger.
                chain.pop_back();
                chain.pop_back();
                uint32_t keep = a, gone = previous;
                if (adjacency[keep].size() < adjacency[gone].size()) std::swap(keep, gone);
                merges.push_back({bestDistance, keep, gone, std::min(clusterSize[keep], clusterSize[gone])});
                adjacency[keep].erase(gone);
                for (const auto &[c, w] : adjacency[gone]) {
                    if (c == keep) continue;
                    adjacency[keep][c] += w;
                    auto &back = adjacency[c];
                    back.erase(gone);
                    back[keep] += w;
                }
                adjacency[gone] = {};
                active[gone] = 0;
                weight[keep] += weight[gone];
                clusterSize[keep] += clusterSize[gone];
            }
        }
    }
    const auto logOf = [](double d) { return std::log(std::max(d, 1e-12)); };

    // Automatic threshold (AtomX heuristic): fit log d = alpha + beta log(merge size) over
    // the merge sequence, then cut just below the first merge of two clusters of at least
    // the minimum grain size whose residual is a robust outlier (> max(4 sigma, ln 20)).
    double threshold = options.logThreshold;
    if (options.algorithm == GrainAlgorithm::GraphClusteringAuto) {
        threshold = std::numeric_limits<double>::infinity();
        if (merges.size() >= 3) {
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (const auto &m : merges) {
                const double x = std::log(double(std::max<uint32_t>(m.smallerSize, 1))), y = logOf(m.distance);
                sx += x; sy += y; sxx += x * x; sxy += x * y;
            }
            const double count = double(merges.size()), denominator = count * sxx - sx * sx;
            const double beta = std::abs(denominator) > 1e-12 ? (count * sxy - sx * sy) / denominator : 0;
            const double alpha = (sy - beta * sx) / count;
            std::vector<double> residual(merges.size());
            for (size_t k = 0; k < merges.size(); ++k)
                residual[k] = logOf(merges[k].distance) - alpha - beta * std::log(double(std::max<uint32_t>(merges[k].smallerSize, 1)));
            auto sorted = residual;
            std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
            const double median = sorted[sorted.size() / 2];
            for (auto &v : sorted) v = std::abs(v - median);
            std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
            const double sigma = 1.4826 * sorted[sorted.size() / 2];
            const double limit = median + std::max(4 * sigma, std::log(20.0));
            for (size_t k = 0; k < merges.size(); ++k)
                if (merges[k].smallerSize >= options.minGrainSize && residual[k] > limit)
                    threshold = std::min(threshold, logOf(merges[k].distance));
            if (std::isfinite(threshold)) threshold = std::nextafter(threshold, -std::numeric_limits<double>::infinity());
        }
    }

    // Cut: merges below the threshold join clusters (reducibility makes this a dendrogram cut).
    UnionFind sets(n);
    for (const auto &m : merges)
        if (logOf(m.distance) < threshold) sets.unite(m.a, m.b);
    std::vector<uint32_t> rootCount(n, 0);
    for (size_t i = 0; i < n; ++i) if (phase[i]) ++rootCount[sets.find(uint32_t(i))];
    std::vector<uint32_t> roots;
    for (size_t i = 0; i < n; ++i) if (rootCount[i] >= options.minGrainSize) roots.push_back(uint32_t(i));
    std::stable_sort(roots.begin(), roots.end(), [&](uint32_t a, uint32_t b) { return rootCount[a] > rootCount[b]; });
    std::vector<int32_t> rootGrain(n, -1);
    for (size_t g = 0; g < roots.size(); ++g) rootGrain[roots[g]] = int32_t(g);
    std::vector<int32_t> grain(n, -1);
    for (size_t i = 0; i < n; ++i) if (phase[i]) grain[i] = rootGrain[sets.find(uint32_t(i))];
    const size_t grainCount = roots.size();

    // Grain phase and mean orientation, from the crystalline members (before adoption).
    std::vector<std::array<size_t, 9>> phaseCount(grainCount, std::array<size_t, 9>{});
    for (size_t i = 0; i < n; ++i) if (grain[i] >= 0) ++phaseCount[size_t(grain[i])][size_t(phase[i])];
    std::vector<int> grainPhase(grainCount, 0);
    for (size_t g = 0; g < grainCount; ++g)
        grainPhase[g] = int(std::max_element(phaseCount[g].begin() + 1, phaseCount[g].end()) - phaseCount[g].begin());
    std::vector<Quat> mean(grainCount, Quat{1, 0, 0, 0}), reference(grainCount, Quat{0, 0, 0, 0});
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<Quat> sum(grainCount, Quat{0, 0, 0, 0});
        for (size_t i = 0; i < n; ++i) {
            if (grain[i] < 0) continue;
            const size_t g = size_t(grain[i]);
            if (phase[i] != grainPhase[g]) continue;
            if (reference[g][0] == 0 && reference[g][1] == 0 && reference[g][2] == 0 && reference[g][3] == 0) reference[g] = orientation[i];
            Quat bestQ = orientation[i];
            double bestDot = -1;
            for (const auto &s : groupOf(phase[i])) {
                const Quat q = ptm_detail::multiply(orientation[i], s);
                const double d = quatDot(q, reference[g]);
                if (std::abs(d) > bestDot) { bestDot = std::abs(d); bestQ = d < 0 ? Quat{-q[0], -q[1], -q[2], -q[3]} : q; }
            }
            for (size_t c = 0; c < 4; ++c) sum[g][c] += bestQ[c];
        }
        for (size_t g = 0; g < grainCount; ++g) {
            mean[g] = normalized(sum[g]);
            reference[g] = mean[g];
        }
    }
    for (size_t g = 0; g < grainCount; ++g) mean[g] = ptm_detail::fundamentalZone(mean[g], groupOf(grainPhase[g]));

    // Orphan adoption: multi-source Dijkstra over the neighbor lists (graph distance).
    if (options.adoptOrphans && grainCount) {
        std::vector<double> reach(n, std::numeric_limits<double>::infinity());
        using Item = std::pair<double, uint32_t>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
        for (size_t i = 0; i < n; ++i) if (grain[i] >= 0) { reach[i] = 0; queue.push({0.0, uint32_t(i)}); }
        size_t steps = 0;
        while (!queue.empty()) {
            if ((++steps & 65535) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
            const auto [d, u] = queue.top();
            queue.pop();
            if (d > reach[u]) continue;
            for (size_t k = 0; k < std::min<size_t>(lists[u].size(), 12); ++k) {
                const auto &nb = lists[u][k];
                const uint32_t v = nb.index;
                const double next = d + std::sqrt(nb.distanceSquared);
                if (reach[v] <= next) continue; // Grain members sit at 0 and keep their grain.
                reach[v] = next;
                grain[v] = grain[u];
                queue.push({next, v});
            }
        }
        // Atoms that list a grain atom but are not listed by any: adopt from their own list.
        for (size_t i = 0; i < n; ++i) {
            if (grain[i] >= 0) continue;
            double best = std::numeric_limits<double>::infinity();
            for (const auto &nb : lists[i])
                if (grain[nb.index] >= 0 && nb.distanceSquared < best) { best = nb.distanceSquared; grain[i] = grain[nb.index]; }
        }
    }

    // Sizes, centers of mass and radii from unwrapped member positions.
    std::vector<size_t> size(grainCount, 0);
    for (size_t i = 0; i < n; ++i) if (grain[i] >= 0) ++size[size_t(grain[i])];
    const auto masses = data.scalarProperties.find("Mass");
    const bool weighted = masses != data.scalarProperties.end() && masses->second.size() == n &&
                          std::any_of(masses->second.begin(), masses->second.end(), [](double m) { return m > 0; });
    std::vector<std::vector<std::pair<uint32_t, std::array<double, 3>>>> links(n);
    for (size_t i = 0; i < n; ++i) {
        if (grain[i] < 0) continue;
        for (const auto &nb : lists[i]) {
            if (grain[nb.index] != grain[i] || nb.index == i) continue;
            links[i].push_back({nb.index, nb.vector});
            links[nb.index].push_back({uint32_t(i), {-nb.vector[0], -nb.vector[1], -nb.vector[2]}});
        }
    }
    double shortest = std::numeric_limits<double>::infinity();
    for (size_t a = 0; a < 3; ++a)
        if (data.pbc[a]) shortest = std::min(shortest, std::hypot(data.cell[a * 3], data.cell[a * 3 + 1], data.cell[a * 3 + 2]));
    std::vector<std::array<double, 3>> unwrapped(n);
    std::vector<uint8_t> seen(n, 0), infinite(grainCount, 0);
    for (size_t s = 0; s < n; ++s) {
        if (grain[s] < 0 || seen[s]) continue;
        seen[s] = 1;
        unwrapped[s] = {data.atoms[s].x, data.atoms[s].y, data.atoms[s].z};
        std::vector<uint32_t> stack{uint32_t(s)};
        while (!stack.empty()) {
            const uint32_t u = stack.back();
            stack.pop_back();
            for (const auto &[v, vec] : links[u]) {
                const std::array<double, 3> p{unwrapped[u][0] + vec[0], unwrapped[u][1] + vec[1], unwrapped[u][2] + vec[2]};
                if (seen[v]) {
                    if (std::isfinite(shortest) && std::hypot(p[0] - unwrapped[v][0], p[1] - unwrapped[v][1], p[2] - unwrapped[v][2]) > .5 * shortest)
                        infinite[size_t(grain[u])] = 1;
                    continue;
                }
                seen[v] = 1;
                unwrapped[v] = p;
                stack.push_back(v);
            }
        }
    }
    std::vector<std::array<double, 4>> center(grainCount, std::array<double, 4>{});
    for (size_t i = 0; i < n; ++i) {
        if (grain[i] < 0) continue;
        const double m = weighted ? std::max(0.0, masses->second[i]) : 1.0;
        auto &c = center[size_t(grain[i])];
        for (size_t k = 0; k < 3; ++k) c[k] += m * unwrapped[i][k];
        c[3] += m;
    }
    std::vector<double> radius(grainCount, 0), gyration(grainCount, 0);
    for (auto &c : center) if (c[3] > 0) for (size_t k = 0; k < 3; ++k) c[k] /= c[3];
    for (size_t i = 0; i < n; ++i) {
        if (grain[i] < 0) continue;
        const size_t g = size_t(grain[i]);
        const double m = weighted ? std::max(0.0, masses->second[i]) : 1.0;
        const double d2 = std::pow(unwrapped[i][0] - center[g][0], 2) + std::pow(unwrapped[i][1] - center[g][1], 2) +
                          std::pow(unwrapped[i][2] - center[g][2], 2);
        radius[g] = std::max(radius[g], std::sqrt(d2));
        gyration[g] += m * d2;
    }
    for (size_t g = 0; g < grainCount; ++g) gyration[g] = center[g][3] > 0 ? std::sqrt(gyration[g] / center[g][3]) : 0;

    // Outputs.
    data.scalarProperties["Grain"] = std::vector<double>(grain.begin(), grain.end());
    if (options.colorParticles) {
        if (data.particleColors.size() != n) data.particleColors.assign(n, Vec3{-1, -1, -1});
        for (size_t i = 0; i < n; ++i)
            if (grain[i] >= 0) data.particleColors[i] = grainColor(size_t(grain[i]));
    }
    DataTable table;
    table.name = "grains";
    table.columns = {"Grain Identifier", "Grain Size", "Color", "Structure Type", "Orientation.X", "Orientation.Y",
                     "Orientation.Z", "Orientation.W", "Center of Mass.X", "Center of Mass.Y", "Center of Mass.Z",
                     "Radius", "Radius of Gyration"};
    const auto undefined = [&](size_t g, double v) { return infinite[g] ? std::string("nan") : formatDataNumber(v); };
    for (size_t g = 0; g < grainCount; ++g) {
        const Vec3 c = grainColor(g);
        table.rows.push_back({std::to_string(g), std::to_string(size[g]),
            formatDataNumber(c.x) + " " + formatDataNumber(c.y) + " " + formatDataNumber(c.z),
            ptmTypeNames[size_t(grainPhase[g])], formatDataNumber(mean[g][1]), formatDataNumber(mean[g][2]),
            formatDataNumber(mean[g][3]), formatDataNumber(mean[g][0]), undefined(g, center[g][0]),
            undefined(g, center[g][1]), undefined(g, center[g][2]), undefined(g, radius[g]), undefined(g, gyration[g])});
    }
    data.tables.push_back(std::move(table));
    // Merge sequence for the scatter plot of log merge distance against merge size.
    std::sort(merges.begin(), merges.end(), [](const GrainMerge &a, const GrainMerge &b) { return a.distance < b.distance; });
    DataTable sequence;
    sequence.name = "grains-merge";
    sequence.columns = {"Log merge distance", "Merge size"};
    constexpr size_t maxRows = 200000;
    size_t singles = 0;
    for (const auto &m : merges) singles += m.smallerSize <= 1;
    const size_t stride = merges.size() > maxRows && singles ? (singles + maxRows / 2 - 1) / (maxRows / 2) : 1;
    size_t single = 0;
    for (const auto &m : merges) {
        if (m.smallerSize <= 1 && (single++ % stride) != 0) continue;
        if (sequence.rows.size() >= maxRows) break;
        sequence.rows.push_back({formatDataNumber(logOf(m.distance)), std::to_string(m.smallerSize)});
    }
    data.tables.push_back(std::move(sequence));
    data.globalAttributes["GrainSegmentation.grain_count"] = double(grainCount);
    data.globalAttributes["GrainSegmentation.merge_threshold"] = threshold;
}

// ---- Elastic strain ------------------------------------------------------------------

enum class ElasticLattice { FCC, HCP, BCC, CubicDiamond, HexagonalDiamond };
struct ElasticStrainOptions {
    ElasticLattice lattice = ElasticLattice::FCC;
    double latticeConstant = 1;                 // a0 of the ideal unit cell, length units.
    double axialRatio = std::sqrt(8.0 / 3.0);   // c/a for hexagonal lattices.
    bool outputStrain = true, outputDeformationGradient = false;
    bool pushForward = true; // Euler-Almansi (spatial frame) instead of Green-Lagrange.
};

inline void computeElasticStrain(Dataset &data, const ElasticStrainOptions &options, std::atomic<bool> *cancel) {
    if (!(options.latticeConstant > 0) || !std::isfinite(options.latticeConstant))
        throw std::runtime_error("Elastic strain needs a positive lattice constant");
    if (!(options.axialRatio > 0) || !std::isfinite(options.axialRatio))
        throw std::runtime_error("Elastic strain needs a positive c/a ratio");
    const double a0 = options.latticeConstant, ideal = std::sqrt(8.0 / 3.0);
    // PTM types searched for each input lattice (the lattice and its stacking partner),
    // and OVITO's structure codes for the output.
    PtmOptions ptm;
    ptm.outputDeformationGradient = true;
    const bool hexagonalInput = options.lattice == ElasticLattice::HCP || options.lattice == ElasticLattice::HexagonalDiamond;
    switch (options.lattice) {
        case ElasticLattice::FCC: case ElasticLattice::HCP: ptm.structures = (1 << 0) | (1 << 1); break;
        case ElasticLattice::BCC: ptm.structures = 1 << 2; break;
        default: ptm.structures = (1 << 5) | (1 << 6); break;
    }
    const auto results = ptmAnalyze(data, ptm, cancel);
    const size_t n = results.size();
    // Ideal nearest-neighbor distance of each PTM type in units of a0.
    const auto idealDistance = [&](PtmType t) {
        switch (t) {
            case PtmType::FCC: case PtmType::HCP: return hexagonalInput ? a0 : a0 / std::sqrt(2.0);
            case PtmType::BCC: return a0 * std::sqrt(3.0) / 2;
            default: return hexagonalInput ? a0 * std::sqrt(3.0 / 8.0) : a0 * std::sqrt(3.0) / 4;
        }
    };
    std::vector<double> code(n, 0), volumetric(n, 0);
    std::vector<std::array<double, 9>> gradient(n, std::array<double, 9>{});
    std::vector<std::array<double, 6>> strain(n, std::array<double, 6>{});
    std::array<uint64_t, 6> counts{};
    for (size_t i = 0; i < n; ++i) {
        const auto &r = results[i];
        int outCode = 0;
        switch (r.type) {
            case PtmType::FCC: outCode = 1; break;
            case PtmType::HCP: outCode = 2; break;
            case PtmType::BCC: outCode = 3; break;
            case PtmType::CubicDiamond: outCode = 4; break;
            case PtmType::HexagonalDiamond: outCode = 5; break;
            default: break;
        }
        ++counts[size_t(outCode)];
        code[i] = outCode;
        if (!outCode) continue;
        double scale = r.interatomicDistance / idealDistance(r.type);
        // A non-ideal c/a stretches the ideal hexagonal cell along its c axis (template z).
        const double cStretch = hexagonalInput && (outCode == 2 || outCode == 5) ? options.axialRatio / ideal : 1;
        double F[9];
        for (size_t row = 0; row < 3; ++row)
            for (size_t col = 0; col < 3; ++col)
                F[row * 3 + col] = r.deformation[row * 3 + col] * scale / (col == 2 ? cStretch : 1);
        double det = F[0] * (F[4] * F[8] - F[5] * F[7]) - F[1] * (F[3] * F[8] - F[5] * F[6]) + F[2] * (F[3] * F[7] - F[4] * F[6]);
        if (!(std::abs(det) > 1e-12)) { code[i] = 0; --counts[size_t(outCode)]; ++counts[0]; continue; }
        for (size_t c = 0; c < 9; ++c) gradient[i][c] = F[c];
        double E[9];
        if (!options.pushForward) {
            // Green-Lagrange: E = (F^T F - I) / 2, material (ideal lattice) frame.
            for (size_t a = 0; a < 3; ++a)
                for (size_t b = 0; b < 3; ++b) {
                    double v = 0;
                    for (size_t k = 0; k < 3; ++k) v += F[k * 3 + a] * F[k * 3 + b];
                    E[a * 3 + b] = .5 * (v - (a == b));
                }
        } else {
            // Euler-Almansi: e = (I - (F F^T)^-1) / 2, spatial frame.
            double B[9];
            for (size_t a = 0; a < 3; ++a)
                for (size_t b = 0; b < 3; ++b) {
                    double v = 0;
                    for (size_t k = 0; k < 3; ++k) v += F[a * 3 + k] * F[b * 3 + k];
                    B[a * 3 + b] = v;
                }
            const double d = B[0] * (B[4] * B[8] - B[5] * B[7]) - B[1] * (B[3] * B[8] - B[5] * B[6]) + B[2] * (B[3] * B[7] - B[4] * B[6]);
            const double inv[9]{(B[4] * B[8] - B[5] * B[7]) / d, (B[2] * B[7] - B[1] * B[8]) / d, (B[1] * B[5] - B[2] * B[4]) / d,
                                (B[5] * B[6] - B[3] * B[8]) / d, (B[0] * B[8] - B[2] * B[6]) / d, (B[2] * B[3] - B[0] * B[5]) / d,
                                (B[3] * B[7] - B[4] * B[6]) / d, (B[1] * B[6] - B[0] * B[7]) / d, (B[0] * B[4] - B[1] * B[3]) / d};
            for (size_t c = 0; c < 9; ++c) E[c] = .5 * ((c % 4 == 0) - inv[c]);
        }
        strain[i] = {E[0], E[4], E[8], E[1], E[2], E[5]};
        volumetric[i] = (E[0] + E[4] + E[8]) / 3;
    }
    data.scalarProperties["Structure Type"] = std::move(code);
    data.scalarProperties["Volumetric Strain"] = std::move(volumetric);
    if (options.outputStrain) {
        static constexpr const char *names[6]{"XX", "YY", "ZZ", "XY", "XZ", "YZ"};
        for (size_t c = 0; c < 6; ++c) {
            std::vector<double> values(n);
            for (size_t i = 0; i < n; ++i) values[i] = strain[i][c];
            data.scalarProperties[std::string("Elastic Strain.") + names[c]] = std::move(values);
        }
    }
    if (options.outputDeformationGradient) {
        static constexpr const char *names[9]{"XX", "XY", "XZ", "YX", "YY", "YZ", "ZX", "ZY", "ZZ"};
        for (size_t c = 0; c < 9; ++c) {
            std::vector<double> values(n);
            for (size_t i = 0; i < n; ++i) values[i] = gradient[i][c];
            data.scalarProperties[std::string("Elastic Deformation Gradient.") + names[c]] = std::move(values);
        }
    }
    static constexpr const char *labels[6]{"Other", "FCC", "HCP", "BCC", "Cubic diamond", "Hexagonal diamond"};
    DataTable table;
    table.name = "Elastic strain structures";
    table.columns = {"Structure", "Count", "Fraction", "Id"};
    for (size_t id = 0; id < 6; ++id) {
        data.globalAttributes[std::string("ElasticStrain.counts.") + labels[id]] = double(counts[id]);
        table.rows.push_back({labels[id], std::to_string(counts[id]),
            n ? formatDataNumber(double(counts[id]) / double(n)) : "0", std::to_string(id)});
    }
    data.tables.push_back(std::move(table));
    double meanVolumetric = 0;
    const auto &vol = data.scalarProperties["Volumetric Strain"];
    const size_t crystalline = n - counts[0];
    for (size_t i = 0; i < n; ++i) meanVolumetric += vol[i];
    data.globalAttributes["ElasticStrain.mean_volumetric_strain"] = crystalline ? meanVolumetric / double(crystalline) : 0.0;
}
