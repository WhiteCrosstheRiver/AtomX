// Structure identification and local fingerprints: Chill+, local pair entropy and
// fcc planar faults. Included inside namespace atomx by core.hpp. OVITO is the
// workflow reference; each method follows the publication cited in src/references.hpp.
// Lengths are in the dataset's units (Angstrom for atomistic files).

// Copies the selected particles into a subset, keeping the cell. `original` maps
// subset indices back to the input. Without a selection the input is used as is.
inline const Dataset &selectedSubset(const Dataset &data, const std::vector<uint8_t> *selection,
                                     Dataset &subset, std::vector<uint32_t> &original) {
    if (!selection) return data;
    if (selection->size() != data.atoms.size())
        throw std::runtime_error("Selection length does not match particle count");
    subset.species = data.species;
    subset.cell = data.cell;
    subset.origin = data.origin;
    subset.pbc = data.pbc;
    for (size_t i = 0; i < data.atoms.size(); ++i)
        if ((*selection)[i]) { subset.atoms.push_back(data.atoms[i]); original.push_back(uint32_t(i)); }
    return subset;
}

// Cutoff neighbor lists that also work when a periodic cell is narrower than twice the
// cutoff: such cells are replicated internally, with the originals first, so periodic
// images of a particle appear as separate neighbors. Indices map back to the input.
inline NeighborVectorLists periodicNeighborLists(const Dataset &d, double cutoff, std::atomic<bool> *cancel) {
    validateNeighborAnalysisInput(d);
    if (!(cutoff > 0) || !std::isfinite(cutoff)) throw std::runtime_error("Cutoff must be finite and positive");
    std::array<int, 3> copies{1, 1, 1};
    bool replicate = false;
    if (std::any_of(d.pbc.begin(), d.pbc.end(), [](bool p) { return p; })) {
        const auto inverse = cellInverse(d.cell);
        for (size_t a = 0; a < 3; ++a) {
            if (!d.pbc[a]) continue;
            const double width = 1.0 / std::hypot(inverse[a][0], inverse[a][1], inverse[a][2]);
            const double length = std::hypot(d.cell[a * 3], d.cell[a * 3 + 1], d.cell[a * 3 + 2]);
            // Orthogonal cells are checked by their edge length, triclinic ones by face width.
            const double limit = std::min(width, length);
            if (limit < 2 * cutoff * (1 + 1e-9)) {
                copies[a] = int(std::ceil(2 * cutoff * (1 + 1e-6) / limit)) + 1;
                replicate = true;
            }
        }
    }
    if (!replicate) return neighborVectorLists(d, cutoff, cancel);
    const size_t count = d.atoms.size();
    const double total = double(count) * copies[0] * copies[1] * copies[2];
    if (total > 2000000) throw std::runtime_error("The periodic cell is too small for this cutoff; replicate it first");
    Dataset big;
    big.species = d.species;
    big.pbc = d.pbc;
    big.origin = d.origin;
    for (size_t a = 0; a < 3; ++a)
        for (size_t xyz = 0; xyz < 3; ++xyz) big.cell[a * 3 + xyz] = d.cell[a * 3 + xyz] * copies[a];
    big.atoms.reserve(size_t(total));
    for (int z = 0; z < copies[2]; ++z)
        for (int y = 0; y < copies[1]; ++y)
            for (int x = 0; x < copies[0]; ++x)
                for (const auto &atom : d.atoms) {
                    Atom copy = atom;
                    copy.x = float(atom.x + x * d.cell[0] + y * d.cell[3] + z * d.cell[6]);
                    copy.y = float(atom.y + x * d.cell[1] + y * d.cell[4] + z * d.cell[7]);
                    copy.z = float(atom.z + x * d.cell[2] + y * d.cell[5] + z * d.cell[8]);
                    big.atoms.push_back(copy);
                }
    auto lists = neighborVectorLists(big, cutoff, cancel);
    lists.resize(count);
    for (auto &list : lists)
        for (auto &neighbor : list) neighbor.index = uint32_t(neighbor.index % count);
    return lists;
}

inline void publishStructureCounts(Dataset &data, const char *tableName, const char *attributePrefix,
                                   const std::vector<const char *> &names, const std::vector<uint64_t> &counts) {
    DataTable table;
    table.name = tableName;
    table.columns = {"Structure", "Count", "Fraction", "Id"};
    for (size_t id = 0; id < names.size(); ++id) {
        data.globalAttributes[std::string(attributePrefix) + names[id]] = double(counts[id]);
        table.rows.push_back({names[id], std::to_string(counts[id]),
            data.atoms.empty() ? "0" : formatDataNumber(double(counts[id]) / double(data.atoms.size())),
            std::to_string(id)});
    }
    data.tables.push_back(std::move(table));
}

// ---- Chill+ (Nguyen & Molinero, J. Phys. Chem. B 119, 9369 (2015)) -----------

// Unnormalized local bond order q_3m(i) = sum_j Y_3m(r_ij), for m = 0..3. Negative
// orders follow from q_3,-m = (-1)^m conj(q_3m).
struct BondOrderQ3 { double re[4]{}, im[4]{}; };
inline BondOrderQ3 bondOrderQ3(const std::vector<NeighborVector> &neighbors) {
    BondOrderQ3 q;
    for (const auto &neighbor : neighbors) {
        const auto &v = neighbor.vector;
        const double length = std::hypot(v[0], v[1], v[2]);
        if (!(length > 0)) continue;
        const double cosTheta = std::clamp(v[2] / length, -1.0, 1.0);
        const double sinTheta = std::sqrt(std::max(0.0, 1 - cosTheta * cosTheta));
        const double phi = std::atan2(v[1], v[0]);
        for (int m = 0; m <= 3; ++m) {
            const double p = normalizedLegendre(3, m, cosTheta, sinTheta);
            q.re[m] += p * std::cos(m * phi);
            q.im[m] += p * std::sin(m * phi);
        }
    }
    return q;
}
// c(i,j) = Re sum_m q_3m(i) q_3m(j)* / (|q_3(i)| |q_3(j)|), summed over m = -3..3.
inline double q3Correlation(const BondOrderQ3 &a, const BondOrderQ3 &b) {
    double dot = 0, na = 0, nb = 0;
    for (int m = 0; m <= 3; ++m) {
        const double weight = m ? 2.0 : 1.0;
        dot += weight * (a.re[m] * b.re[m] + a.im[m] * b.im[m]);
        na += weight * (a.re[m] * a.re[m] + a.im[m] * a.im[m]);
        nb += weight * (b.re[m] * b.re[m] + b.im[m] * b.im[m]);
    }
    if (!(na > 0) || !(nb > 0)) return std::numeric_limits<double>::quiet_NaN();
    return dot / std::sqrt(na * nb);
}
enum class ChillPlusType : uint8_t { Other, HexagonalIce, CubicIce, InterfacialIce, Hydrate, InterfacialHydrate };
// Bonds are eclipsed for -0.35 < c < 0.25 and staggered for c < -0.8. Only particles
// with exactly four neighbors within the cutoff are classified (Table 1 of the paper).
inline ChillPlusType chillPlusClassify(size_t neighborCount, int eclipsed, int staggered) {
    if (neighborCount != 4) return ChillPlusType::Other;
    if (eclipsed == 4) return ChillPlusType::Hydrate;
    if (eclipsed == 3) return ChillPlusType::InterfacialHydrate;
    if (staggered == 4) return ChillPlusType::CubicIce;
    if (staggered == 3 && eclipsed == 1) return ChillPlusType::HexagonalIce;
    if (staggered == 3 || staggered == 2) return ChillPlusType::InterfacialIce;
    return ChillPlusType::Other;
}
inline void computeChillPlus(Dataset &data, double cutoff, const std::vector<uint8_t> *selection,
                             std::atomic<bool> *cancel) {
    if (!(cutoff > 0) || !std::isfinite(cutoff)) throw std::runtime_error("Chill+ needs a positive cutoff");
    Dataset subset;
    std::vector<uint32_t> original;
    const Dataset &input = selectedSubset(data, selection, subset, original);
    const auto lists = periodicNeighborLists(input, cutoff, cancel);
    std::vector<BondOrderQ3> q(lists.size());
    for (size_t i = 0; i < lists.size(); ++i) {
        if ((i & 4095) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
        q[i] = bondOrderQ3(lists[i]);
    }
    std::vector<double> structure(data.atoms.size(), 0.0);
    std::vector<uint64_t> counts(6, 0);
    for (size_t i = 0; i < lists.size(); ++i) {
        if ((i & 4095) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
        int eclipsed = 0, staggered = 0;
        for (const auto &neighbor : lists[i]) {
            const double c = q3Correlation(q[i], q[neighbor.index]);
            if (c > -0.35 && c < 0.25) ++eclipsed;
            else if (c < -0.8) ++staggered;
        }
        const auto kind = uint8_t(chillPlusClassify(lists[i].size(), eclipsed, staggered));
        structure[selection ? original[i] : i] = kind;
        ++counts[kind];
    }
    counts[0] += data.atoms.size() - lists.size();
    data.scalarProperties["Structure Type"] = std::move(structure);
    publishStructureCounts(data, "Chill+", "ChillPlus.counts.",
                           {"Other", "Hexagonal ice", "Cubic ice", "Interfacial ice", "Hydrate", "Interfacial hydrate"},
                           counts);
}

// ---- Local pair entropy (Piaggi & Parrinello, J. Chem. Phys. 147, 114112 (2017)) ----

struct LocalEntropyOptions {
    double cutoff = 5.0;        // Integration limit r_m and neighbor cutoff.
    double sigma = 0.2;         // Gaussian smoothing width of g_m(r).
    bool useLocalDensity = false;
    bool average = false;
    double averageCutoff = 5.0;
};
// s_i = -2 pi rho k_B integral_0^rm [g ln g - g + 1] r^2 dr in units of k_B, with
// g_m(r) = sum_j exp(-(r - r_ij)^2 / 2 sigma^2) / (4 pi rho r^2 sqrt(2 pi sigma^2)),
// integrated by the trapezoid rule on int(rm / sigma) + 1 evenly spaced points from 0.
// The grid and the r = 0 normalization follow OVITO's published modifier script, which
// mirrors the LAMMPS compute entropy/atom.
inline void computeLocalEntropy(Dataset &data, const LocalEntropyOptions &options, std::atomic<bool> *cancel) {
    if (!(options.cutoff > 0) || !std::isfinite(options.cutoff))
        throw std::runtime_error("Local entropy needs a positive cutoff");
    if (!(options.sigma > 0) || !(options.sigma < options.cutoff))
        throw std::runtime_error("Local entropy needs 0 < sigma < cutoff");
    if (options.cutoff / options.sigma > 100000)
        throw std::runtime_error("Local entropy sigma is too small for the cutoff");
    if (options.average && (!(options.averageCutoff > 0) || !std::isfinite(options.averageCutoff)))
        throw std::runtime_error("Local entropy needs a positive averaging cutoff");
    const double volume = std::abs(cellDeterminant(data.cell));
    if (!(volume > 0)) throw std::runtime_error("Local entropy needs a simulation cell to define the density");
    const size_t count = data.atoms.size();
    const double globalRho = double(count) / volume;
    if (double(count) * globalRho * 4.0 / 3 * 3.14159265358979323846 * std::pow(options.cutoff, 3) > 4e8)
        throw std::runtime_error("Local entropy would visit too many neighbor pairs; reduce the cutoff");
    std::vector<std::vector<float>> distances(count);
    {
        const auto lists = periodicNeighborLists(data, options.cutoff, cancel);
        for (size_t i = 0; i < count; ++i) {
            distances[i].reserve(lists[i].size());
            for (const auto &neighbor : lists[i]) distances[i].push_back(float(std::sqrt(neighbor.distanceSquared)));
        }
    }
    constexpr double pi = 3.14159265358979323846;
    const size_t bins = size_t(options.cutoff / options.sigma) + 1;
    std::vector<double> r(bins), prefactor(bins);
    for (size_t k = 0; k < bins; ++k) {
        r[k] = bins > 1 ? options.cutoff * double(k) / double(bins - 1) : 0.0;
        prefactor[k] = r[k] * r[k] * 4 * pi * globalRho * std::sqrt(2 * pi * options.sigma * options.sigma);
    }
    if (bins > 1) prefactor[0] = prefactor[1]; // Avoids the division by zero at r = 0.
    const double twoSigmaSquared = 2 * options.sigma * options.sigma;
    std::vector<double> entropy(count, 0.0), g(bins);
    for (size_t i = 0; i < count; ++i) {
        if ((i & 1023) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
        std::fill(g.begin(), g.end(), 0.0);
        for (float rij : distances[i])
            for (size_t k = 0; k < bins; ++k) {
                const double d = r[k] - double(rij);
                g[k] += std::exp(-d * d / twoSigmaSquared);
            }
        double rho = globalRho;
        if (options.useLocalDensity) {
            rho = double(distances[i].size()) / (4.0 / 3 * pi * options.cutoff * options.cutoff * options.cutoff);
        }
        double integral = 0, previous = 0;
        for (size_t k = 0; k < bins; ++k) {
            double gm = g[k] / prefactor[k];
            if (options.useLocalDensity) gm = rho > 0 ? gm * globalRho / rho : 0.0;
            const double rsq = r[k] * r[k];
            const double integrand = gm >= 1e-10 ? (gm * std::log(gm) - gm + 1.0) * rsq : rsq;
            if (k) integral += .5 * (integrand + previous) * (r[k] - r[k - 1]);
            previous = integrand;
        }
        entropy[i] = -2.0 * pi * rho * integral;
    }
    if (options.average) {
        // <s_i> = (s_i + sum_j s_j) / (N_i + 1) over neighbors within the averaging cutoff.
        const auto lists = periodicNeighborLists(data, options.averageCutoff, cancel);
        std::vector<double> averaged(count);
        for (size_t i = 0; i < count; ++i) {
            double sum = entropy[i];
            for (const auto &neighbor : lists[i]) sum += entropy[neighbor.index];
            averaged[i] = sum / double(lists[i].size() + 1);
        }
        entropy = std::move(averaged);
    }
    double mean = 0;
    for (double value : entropy) mean += value;
    data.globalAttributes["LocalEntropy.mean"] = count ? mean / double(count) : 0.0;
    data.scalarProperties["Entropy"] = std::move(entropy);
}

// ---- fcc planar faults --------------------------------------------------------
// hcp-like atoms come from adaptive common neighbor analysis (Stukowski 2012) of the
// 12 nearest neighbors. In an hcp environment the six 422 bonds lie in the basal plane
// and the six 421 bonds point three to each side of it. A side counts as faulted when
// all three out-of-plane neighbors there are hcp-like, and clean when none are.

enum class PlanarFaultType : uint8_t { NonHcp, Other, IntrinsicStackingFault, TwinBoundary, MultiLayer };
struct HcpEnvironment {
    bool hcp = false;
    std::array<uint32_t, 6> inPlane{};
    std::array<uint32_t, 3> above{}, below{};
    double basalArea = 0; // Projected area per atom in its close-packed plane.
};
inline HcpEnvironment hcpEnvironment(const std::vector<NeighborVector> &neighbors) {
    HcpEnvironment env;
    if (neighbors.size() < 12) return env;
    double mean = 0;
    for (size_t k = 0; k < 12; ++k) mean += std::sqrt(neighbors[k].distanceSquared);
    mean /= 12;
    const double cutoffSquared = std::pow(.5 * (1 + std::sqrt(2.0)) * mean, 2);
    const auto bonded = [&](size_t a, size_t b) {
        const auto &u = neighbors[a].vector, &v = neighbors[b].vector;
        return std::pow(u[0] - v[0], 2) + std::pow(u[1] - v[1], 2) + std::pow(u[2] - v[2], 2) < cutoffSquared;
    };
    std::array<size_t, 6> inPlane{}, outOfPlane{};
    size_t in = 0, out = 0;
    for (size_t j = 0; j < 12; ++j) {
        std::array<size_t, 12> common{};
        size_t n = 0;
        for (size_t k = 0; k < 12; ++k)
            if (k != j && bonded(j, k)) common[n++] = k;
        if (n != 4) return env;
        std::array<std::pair<size_t, size_t>, 6> bonds{};
        size_t nb = 0;
        for (size_t a = 0; a < n; ++a)
            for (size_t b = a + 1; b < n; ++b)
                if (bonded(common[a], common[b])) bonds[nb++] = {a, b};
        if (nb != 2) return env;
        // 421: the two bonds share no atom. 422: they form a chain of two.
        const bool chain = bonds[0].first == bonds[1].first || bonds[0].first == bonds[1].second ||
                           bonds[0].second == bonds[1].first || bonds[0].second == bonds[1].second;
        if (chain) { if (in == 6) return env; inPlane[in++] = j; }
        else { if (out == 6) return env; outOfPlane[out++] = j; }
    }
    if (in != 6 || out != 6) return env;
    // Basal-plane normal from the in-plane bond pair closest to perpendicular.
    const auto &u = neighbors[inPlane[0]].vector;
    std::array<double, 3> normal{};
    double best = 0;
    for (size_t k = 1; k < 6; ++k) {
        const auto &v = neighbors[inPlane[k]].vector;
        const std::array<double, 3> c{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        const double length = std::hypot(c[0], c[1], c[2]);
        if (length > best) { best = length; normal = {c[0] / length, c[1] / length, c[2] / length}; }
    }
    if (!(best > 0)) return env;
    size_t up = 0, down = 0;
    for (size_t k = 0; k < 6; ++k) {
        const auto &v = neighbors[outOfPlane[k]].vector;
        const double side = v[0] * normal[0] + v[1] * normal[1] + v[2] * normal[2];
        if (side > 0) { if (up == 3) return env; env.above[up++] = neighbors[outOfPlane[k]].index; }
        else { if (down == 3) return env; env.below[down++] = neighbors[outOfPlane[k]].index; }
    }
    double inPlaneDistance = 0;
    for (size_t k = 0; k < 6; ++k) {
        env.inPlane[k] = neighbors[inPlane[k]].index;
        inPlaneDistance += std::sqrt(neighbors[inPlane[k]].distanceSquared);
    }
    inPlaneDistance /= 6;
    env.basalArea = std::sqrt(3.0) / 2 * inPlaneDistance * inPlaneDistance;
    env.hcp = true;
    return env;
}
inline void computeFccPlanarFaults(Dataset &data, std::atomic<bool> *cancel) {
    const auto lists = nearestNeighborVectors(data, 12, cancel);
    const size_t count = data.atoms.size();
    std::vector<HcpEnvironment> env(count);
    for (size_t i = 0; i < count; ++i) {
        if ((i & 4095) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
        env[i] = hcpEnvironment(lists[i]);
    }
    const auto hcpCount = [&](const auto &indices) {
        int n = 0;
        for (uint32_t index : indices) n += env[index].hcp ? 1 : 0;
        return n;
    };
    std::vector<uint8_t> kind(count, uint8_t(PlanarFaultType::NonHcp));
    for (size_t i = 0; i < count; ++i) {
        if (!env[i].hcp) continue;
        const int above = hcpCount(env[i].above), below = hcpCount(env[i].below), inPlane = hcpCount(env[i].inPlane);
        PlanarFaultType type = PlanarFaultType::Other;
        if (above == 0 && below == 0) type = inPlane > 0 ? PlanarFaultType::TwinBoundary : PlanarFaultType::Other;
        else if ((above == 3 && below == 0) || (above == 0 && below == 3)) type = PlanarFaultType::IntrinsicStackingFault;
        else if (above == 3 && below == 3) type = PlanarFaultType::MultiLayer;
        kind[i] = uint8_t(type);
    }
    // The outer layers of a fault three or more layers thick border a multi-layer atom.
    const auto first = kind;
    for (size_t i = 0; i < count; ++i) {
        if (first[i] != uint8_t(PlanarFaultType::IntrinsicStackingFault)) continue;
        for (const auto *side : {&env[i].above, &env[i].below})
            for (uint32_t index : *side)
                if (first[index] == uint8_t(PlanarFaultType::MultiLayer)) kind[i] = uint8_t(PlanarFaultType::MultiLayer);
    }
    std::vector<double> property(count);
    std::vector<uint64_t> counts(5, 0);
    std::array<double, 5> area{};
    for (size_t i = 0; i < count; ++i) {
        property[i] = kind[i];
        ++counts[kind[i]];
        area[kind[i]] += env[i].basalArea;
    }
    // Each fault is two hcp-like layers thick for an ISF and one for a twin boundary.
    const double isfArea = area[2] / 2, twinArea = area[3];
    data.scalarProperties["Planar Fault Type"] = std::move(property);
    static constexpr const char *names[5]{"Non-hcp", "Indeterminate hcp-like", "Intrinsic stacking fault",
                                          "Coherent twin boundary", "Multi-layer stacking fault"};
    DataTable table;
    table.name = "Planar faults";
    table.columns = {"Type", "Atom Count", "Estimated Area", "Id"};
    for (size_t id = 0; id < 5; ++id) {
        data.globalAttributes[std::string("PlanarFaults.counts.") + names[id]] = double(counts[id]);
        const double estimated = id == 2 ? isfArea : id == 3 ? twinArea : 0.0;
        table.rows.push_back({names[id], std::to_string(counts[id]), id == 2 || id == 3 ? formatDataNumber(estimated) : "",
                              std::to_string(id)});
    }
    data.globalAttributes["PlanarFaults.area.Intrinsic stacking fault"] = isfArea;
    data.globalAttributes["PlanarFaults.area.Coherent twin boundary"] = twinArea;
    data.tables.push_back(std::move(table));
}
