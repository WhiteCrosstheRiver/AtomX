// Particle analyses modelled on the OVITO modifier catalog. Included inside
// namespace atomx by core.hpp. OVITO is the workflow reference only: every method
// here is implemented from the published paper cited in src/references.hpp.
// Lengths are in the dataset's units (Angstrom for atomistic files).

struct NeighborVector {
    uint32_t index;
    double distanceSquared;
    std::array<double, 3> vector; // Neighbor minus center, with its periodic image applied.
};
using NeighborVectorLists = std::vector<std::vector<NeighborVector>>;

inline NeighborVectorLists neighborVectorLists(const Dataset &d, double cutoff,
                                               std::atomic<bool> *cancel = nullptr) {
    NeighborVectorLists lists(d.atoms.size());
    forEachNeighborPair(d, cutoff, [&](uint32_t i, uint32_t j, double distanceSquared,
                                       std::array<int32_t, 3> image) {
        std::array<double, 3> v{double(d.atoms[j].x) - d.atoms[i].x,
                                double(d.atoms[j].y) - d.atoms[i].y,
                                double(d.atoms[j].z) - d.atoms[i].z};
        for (int axis = 0; axis < 3; ++axis)
            for (int xyz = 0; xyz < 3; ++xyz)
                v[xyz] += double(image[axis]) * d.cell[axis * 3 + xyz];
        lists[i].push_back({j, distanceSquared, v});
        lists[j].push_back({i, distanceSquared, {-v[0], -v[1], -v[2]}});
    }, cancel);
    return lists;
}

// The nearest `count` neighbors of every particle, closest first. The search radius
// grows until each particle has them, up to half the shortest periodic translation.
inline NeighborVectorLists nearestNeighborVectors(const Dataset &d, size_t count,
                                                  std::atomic<bool> *cancel = nullptr) {
    validateNeighborAnalysisInput(d);
    NeighborVectorLists lists(d.atoms.size());
    if (d.atoms.size() < 2) return lists;
    const size_t wanted = std::min(count, d.atoms.size() - 1);
    double maxCutoff = std::numeric_limits<double>::infinity();
    for (int axis = 0; axis < 3; ++axis)
        if (d.pbc[axis])
            maxCutoff = std::min(maxCutoff, .5 * std::hypot(d.cell[axis * 3], d.cell[axis * 3 + 1],
                                                            d.cell[axis * 3 + 2]));
    const double volume = std::abs(cellDeterminant(d.cell));
    double cutoff = volume > 0 ? 1.5 * std::cbrt(3 * double(count) * volume /
                                                 (4 * 3.14159265358979323846 * double(d.atoms.size())))
                               : 1.0;
    if (!(cutoff > 0) || !std::isfinite(cutoff)) cutoff = 1.0;
    for (int attempt = 0; attempt < 32; ++attempt) {
        cutoff = std::min(cutoff, maxCutoff);
        try {
            lists = neighborVectorLists(d, cutoff, cancel);
        } catch (const std::exception &e) {
            if (std::string(e.what()) == "Cancelled" || attempt == 0) throw;
            break; // Keep the last complete pass; short lists are reported by the caller.
        }
        const bool complete = std::all_of(lists.begin(), lists.end(),
            [&](const auto &list) { return list.size() >= wanted; });
        if (complete || cutoff >= maxCutoff) break;
        cutoff *= 1.3;
    }
    for (auto &list : lists) {
        const size_t keep = std::min(list.size(), count);
        std::partial_sort(list.begin(), list.begin() + keep, list.end(),
                          [](const NeighborVector &a, const NeighborVector &b) {
                              return a.distanceSquared < b.distanceSquared;
                          });
        list.resize(keep);
    }
    return lists;
}

// ---- Steinhardt order parameters (Steinhardt, Nelson & Ronchetti 1983) --------

// Normalized associated Legendre function: Y_lm = value * exp(i m phi).
inline double normalizedLegendre(int l, int m, double cosTheta, double sinTheta) {
    double pmm = std::sqrt(1.0 / (4 * 3.14159265358979323846));
    for (int k = 1; k <= m; ++k)
        pmm *= -std::sqrt((2.0 * k + 1) / (2.0 * k)) * sinTheta;
    if (l == m) return pmm;
    double previous = pmm, value = cosTheta * std::sqrt(2.0 * m + 3) * pmm;
    for (int degree = m + 2; degree <= l; ++degree) {
        const double d2 = double(degree) * degree, m2 = double(m) * m, p = degree - 1.0;
        const double a = std::sqrt((4 * d2 - 1) / (d2 - m2));
        const double b = std::sqrt((p * p - m2) / (4 * p * p - 1));
        const double next = a * (cosTheta * value - b * previous);
        previous = value;
        value = next;
    }
    return value;
}
inline double steinhardtParameter(const std::vector<NeighborVector> &neighbors, int l) {
    if (neighbors.empty()) return 0;
    double sum = 0;
    for (int m = 0; m <= l; ++m) {
        double re = 0, im = 0;
        for (const auto &neighbor : neighbors) {
            const auto &v = neighbor.vector;
            const double length = std::hypot(v[0], v[1], v[2]);
            if (!(length > 0)) continue;
            const double cosTheta = std::clamp(v[2] / length, -1.0, 1.0);
            const double sinTheta = std::sqrt(std::max(0.0, 1 - cosTheta * cosTheta));
            const double phi = std::atan2(v[1], v[0]);
            const double p = normalizedLegendre(l, m, cosTheta, sinTheta);
            re += p * std::cos(m * phi);
            im += p * std::sin(m * phi);
        }
        re /= double(neighbors.size());
        im /= double(neighbors.size());
        // |q_l,-m| = |q_l,m|, so positive orders count twice.
        sum += (m ? 2.0 : 1.0) * (re * re + im * im);
    }
    return std::sqrt(4 * 3.14159265358979323846 / (2 * l + 1) * sum);
}
inline void computeSteinhardt(Dataset &data, int neighborCount, std::atomic<bool> *cancel) {
    if (neighborCount < 1 || neighborCount > 64)
        throw std::runtime_error("Steinhardt order parameters needs 1 to 64 neighbors");
    const auto lists = nearestNeighborVectors(data, size_t(neighborCount), cancel);
    std::vector<double> q4(data.atoms.size()), q6(data.atoms.size());
    size_t underCoordinated = 0;
    double sum4 = 0, sum6 = 0;
    for (size_t i = 0; i < lists.size(); ++i) {
        if ((i & 4095) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
        if (lists[i].size() < size_t(neighborCount)) ++underCoordinated;
        q4[i] = steinhardtParameter(lists[i], 4);
        q6[i] = steinhardtParameter(lists[i], 6);
        sum4 += q4[i];
        sum6 += q6[i];
    }
    const double count = std::max<double>(1, double(data.atoms.size()));
    data.globalAttributes["SteinhardtOrderParameters.mean_q4"] = sum4 / count;
    data.globalAttributes["SteinhardtOrderParameters.mean_q6"] = sum6 / count;
    data.globalAttributes["SteinhardtOrderParameters.undercoordinated_particles"] = double(underCoordinated);
    data.scalarProperties["q4"] = std::move(q4);
    data.scalarProperties["q6"] = std::move(q6);
}

// ---- Ackland-Jones bond-angle analysis (Ackland & Jones 2006) -----------------

enum class AcklandStructure : uint8_t { Other, FCC, HCP, BCC, Icosahedral };

inline AcklandStructure acklandClassify(const std::vector<NeighborVector> &sorted) {
    if (sorted.size() < 6) return AcklandStructure::Other;
    double r0Squared = 0;
    for (size_t k = 0; k < 6; ++k) r0Squared += sorted[k].distanceSquared;
    r0Squared /= 6;
    size_t n0 = 0, n1 = 0;
    for (const auto &neighbor : sorted) {
        if (neighbor.distanceSquared < 1.45 * r0Squared) ++n0;
        if (neighbor.distanceSquared < 1.55 * r0Squared) ++n1;
    }
    // The published bond-angle bins, by cosine.
    static constexpr double upper[8]{-.945, -.915, -.755, -.195, .195, .245, .795, 1.0000001};
    int chi[8]{};
    for (size_t a = 0; a < n0; ++a)
        for (size_t b = a + 1; b < n0; ++b) {
            const auto &u = sorted[a].vector, &v = sorted[b].vector;
            const double cosine = (u[0] * v[0] + u[1] * v[1] + u[2] * v[2]) /
                                  std::sqrt(sorted[a].distanceSquared * sorted[b].distanceSquared);
            for (int bin = 0; bin < 8; ++bin)
                if (cosine < upper[bin]) { ++chi[bin]; break; }
        }
    double deltaBcc = .35 * chi[4] / std::max(1.0, double(chi[5] + chi[6] - chi[4]));
    double deltaCp = std::abs(1 - chi[6] / 24.0);
    double deltaFcc = .61 * (std::abs(chi[0] + chi[1] - 6.0) + chi[2]) / 6;
    double deltaHcp = (std::abs(chi[0] - 3.0) + std::abs(chi[0] + chi[1] + chi[2] + chi[3] - 9.0)) / 12;
    if (chi[0] == 7) deltaBcc = 0;
    else if (chi[0] == 6) deltaFcc = 0;
    else if (chi[0] <= 3) deltaHcp = 0;
    if (chi[7] > 0) return AcklandStructure::Other;
    if (chi[4] < 3)
        return n1 > 13 || n1 < 11 ? AcklandStructure::Other : AcklandStructure::Icosahedral;
    if (deltaBcc <= deltaCp)
        return n1 < 11 ? AcklandStructure::Other : AcklandStructure::BCC;
    if (n1 > 12 || n1 < 11) return AcklandStructure::Other;
    return deltaFcc < deltaHcp ? AcklandStructure::FCC : AcklandStructure::HCP;
}
inline void computeAcklandJones(Dataset &data, std::atomic<bool> *cancel) {
    const auto lists = nearestNeighborVectors(data, 16, cancel);
    std::vector<double> structure(data.atoms.size());
    std::array<uint64_t, 5> counts{};
    for (size_t i = 0; i < lists.size(); ++i) {
        if ((i & 4095) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
        const auto kind = uint8_t(acklandClassify(lists[i]));
        structure[i] = kind;
        ++counts[kind];
    }
    data.scalarProperties["Structure Type"] = std::move(structure);
    DataTable table;
    table.name = "Ackland-Jones analysis";
    table.columns = {"Structure", "Count", "Fraction", "Id"};
    static constexpr const char *names[5]{"Other", "FCC", "HCP", "BCC", "Icosahedral"};
    for (size_t id = 0; id < 5; ++id) {
        data.globalAttributes[std::string("AcklandJones.counts.") + names[id]] = double(counts[id]);
        table.rows.push_back({names[id], std::to_string(counts[id]),
            data.atoms.empty() ? "0" : std::to_string(double(counts[id]) / double(data.atoms.size())),
            std::to_string(id)});
    }
    data.tables.push_back(std::move(table));
}

// ---- Reference-configuration helpers ------------------------------------------

inline constexpr uint32_t noReferenceParticle = std::numeric_limits<uint32_t>::max();
// For each current particle, the index of the matching reference particle. Unique
// Particle Identifiers on both sides decide, because some codes reorder particles
// between frames; without them equal counts pair by storage order. With allowNew a
// particle absent from the reference maps to noReferenceParticle instead of failing.
inline std::vector<uint32_t> matchReferenceParticles(const Dataset &data, const Dataset &reference,
                                                     std::atomic<bool> *cancel, bool allowNew = false) {
    const size_t count = data.atoms.size(), referenceCount = reference.atoms.size();
    std::vector<uint32_t> mapping(count);
    const auto byOrder = [&] {
        for (size_t i = 0; i < count; ++i) mapping[i] = uint32_t(i);
        return mapping;
    };
    const auto referenceIds = reference.scalarProperties.find("Particle Identifier");
    const auto currentIds = data.scalarProperties.find("Particle Identifier");
    const bool haveIdentifiers = referenceIds != reference.scalarProperties.end() &&
        currentIds != data.scalarProperties.end() &&
        referenceIds->second.size() == referenceCount && currentIds->second.size() == count;
    std::unordered_map<double, uint32_t> byIdentifier;
    bool unique = haveIdentifiers;
    if (haveIdentifiers) {
        byIdentifier.reserve(referenceCount * 2);
        for (size_t i = 0; i < referenceCount && unique; ++i) {
            if ((i & 65535) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
            unique = byIdentifier.emplace(referenceIds->second[i], uint32_t(i)).second;
        }
    }
    if (!unique) {
        if (referenceCount == count) return byOrder();
        throw std::runtime_error("Particle counts of the reference and current configuration do not match.");
    }
    for (size_t i = 0; i < count; ++i) {
        if ((i & 65535) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
        const auto found = byIdentifier.find(currentIds->second[i]);
        if (found != byIdentifier.end()) mapping[i] = found->second;
        else if (allowNew) mapping[i] = noReferenceParticle;
        else throw std::runtime_error("Particle counts of the reference and current configuration do not match.");
    }
    return mapping;
}

// ---- Atomic strain (Shimizu et al. 2007; Falk & Langer 1998) -------------------

using Matrix3 = std::array<std::array<double, 3>, 3>;
inline bool invertMatrix3(const Matrix3 &m, Matrix3 &out) {
    const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    const double c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    const double c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
    double scale = 0;
    for (const auto &row : m) for (double v : row) scale = std::max(scale, std::abs(v));
    if (!(scale > 0) || !(std::abs(det) > 1e-10 * scale * scale * scale)) return false;
    out = {{{c00 / det, (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det, (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det},
            {c01 / det, (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det, (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det},
            {c02 / det, (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det, (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det}}};
    return true;
}
inline void computeAtomicStrain(Dataset &data, const Dataset &reference, double cutoff,
                                std::atomic<bool> *cancel) {
    if (data.sampled() || reference.sampled())
        throw std::runtime_error("Atomic strain requires complete, unsampled particle data");
    const auto mapping = matchReferenceParticles(data, reference, cancel);
    constexpr uint32_t missing = std::numeric_limits<uint32_t>::max();
    std::vector<uint32_t> currentOfReference(reference.atoms.size(), missing);
    for (size_t i = 0; i < mapping.size(); ++i) currentOfReference[mapping[i]] = uint32_t(i);
    const auto lists = neighborVectorLists(reference, cutoff, cancel);
    const bool anyPeriodic = std::any_of(data.pbc.begin(), data.pbc.end(), [](bool p) { return p; });
    std::array<std::array<double, 3>, 3> inverse{};
    if (anyPeriodic && !data.atoms.empty()) inverse = cellInverse(data.cell);
    const size_t count = data.atoms.size();
    std::vector<double> shear(count), volumetric(count), nonaffine(count);
    std::array<std::vector<double>, 6> tensor;
    for (auto &component : tensor) component.assign(count, 0);
    size_t invalid = 0;
    double shearSum = 0, volumetricSum = 0;
    for (size_t i = 0; i < count; ++i) {
        if ((i & 4095) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
        Matrix3 v{}, w{};
        std::vector<std::pair<std::array<double, 3>, std::array<double, 3>>> pairs;
        for (const auto &neighbor : lists[mapping[i]]) {
            const uint32_t j = currentOfReference[neighbor.index];
            if (j == missing) continue;
            double d[3]{double(data.atoms[j].x) - data.atoms[i].x,
                        double(data.atoms[j].y) - data.atoms[i].y,
                        double(data.atoms[j].z) - data.atoms[i].z};
            if (anyPeriodic) {
                double fractional[3];
                for (int axis = 0; axis < 3; ++axis)
                    fractional[axis] = inverse[axis][0] * d[0] + inverse[axis][1] * d[1] + inverse[axis][2] * d[2];
                for (int axis = 0; axis < 3; ++axis) {
                    if (!data.pbc[axis]) continue;
                    const double image = std::round(fractional[axis]);
                    for (int xyz = 0; xyz < 3; ++xyz) d[xyz] -= image * data.cell[axis * 3 + xyz];
                }
            }
            const auto &d0 = neighbor.vector;
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b) {
                    v[a][b] += d0[a] * d0[b];
                    w[a][b] += d[a] * d0[b];
                }
            pairs.push_back({d0, {d[0], d[1], d[2]}});
        }
        Matrix3 vInverse{};
        if (pairs.size() < 3 || !invertMatrix3(v, vInverse)) { ++invalid; continue; }
        Matrix3 f{}; // Deformation gradient: d = F d0 in the least-squares sense.
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b)
                for (int c = 0; c < 3; ++c) f[a][b] += w[a][c] * vInverse[c][b];
        Matrix3 e{}; // Green-Lagrangian strain E = (F^T F - I) / 2.
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) {
                for (int c = 0; c < 3; ++c) e[a][b] += f[c][a] * f[c][b];
                e[a][b] = .5 * (e[a][b] - (a == b ? 1 : 0));
            }
        const double xx = e[0][0], yy = e[1][1], zz = e[2][2];
        const double xy = e[0][1], xz = e[0][2], yz = e[1][2];
        shear[i] = std::sqrt(yz * yz + xz * xz + xy * xy +
                             ((yy - zz) * (yy - zz) + (xx - zz) * (xx - zz) + (xx - yy) * (xx - yy)) / 6);
        volumetric[i] = (xx + yy + zz) / 3;
        const double components[6]{xx, yy, zz, xy, xz, yz};
        for (int c = 0; c < 6; ++c) tensor[size_t(c)][i] = components[c];
        double residual = 0; // Falk-Langer D^2_min.
        for (const auto &[d0, d] : pairs)
            for (int a = 0; a < 3; ++a) {
                const double mapped = f[a][0] * d0[0] + f[a][1] * d0[1] + f[a][2] * d0[2];
                residual += (d[a] - mapped) * (d[a] - mapped);
            }
        nonaffine[i] = residual;
        shearSum += shear[i];
        volumetricSum += volumetric[i];
    }
    const double valid = std::max<double>(1, double(count - invalid));
    data.globalAttributes["AtomicStrain.invalid_particle_count"] = double(invalid);
    data.globalAttributes["AtomicStrain.mean_shear_strain"] = shearSum / valid;
    data.globalAttributes["AtomicStrain.mean_volumetric_strain"] = volumetricSum / valid;
    data.scalarProperties["Shear Strain"] = std::move(shear);
    data.scalarProperties["Volumetric Strain"] = std::move(volumetric);
    data.scalarProperties["Nonaffine Squared Displacement"] = std::move(nonaffine);
    static constexpr const char *names[6]{"XX", "YY", "ZZ", "XY", "XZ", "YZ"};
    for (size_t c = 0; c < 6; ++c)
        data.scalarProperties[std::string("Strain Tensor.") + names[c]] = std::move(tensor[c]);
}

// ---- Wigner-Seitz cell method for point defects (Nordlund et al. 1998) ---------

// Assigns each particle to its nearest reference site. A vacancy is a site with no
// particle; each particle beyond the first on a site counts as one interstitial.
inline void computeWignerSeitz(PipelineResult &r, const Dataset &reference, bool outputSites,
                               std::atomic<bool> *cancel) {
    Dataset &data = r.data;
    if (data.sampled() || reference.sampled())
        throw std::runtime_error("Wigner-Seitz analysis requires complete, unsampled particle data");
    const size_t sites = reference.atoms.size(), count = data.atoms.size();
    if (!sites) throw std::runtime_error("Wigner-Seitz analysis needs at least one reference site");
    // Sites and particles share one neighbor search in the reference cell.
    Dataset merged;
    merged.species = reference.species;
    merged.cell = reference.cell;
    merged.origin = reference.origin;
    merged.pbc = reference.pbc;
    merged.atoms = reference.atoms;
    merged.atoms.reserve(sites + count);
    const bool anyPeriodic = std::any_of(reference.pbc.begin(), reference.pbc.end(), [](bool p) { return p; });
    for (const auto &atom : data.atoms)
        merged.atoms.push_back(anyPeriodic ? wrapAtomInCell(reference, atom) : atom);
    double maxCutoff = std::numeric_limits<double>::infinity();
    for (int axis = 0; axis < 3; ++axis)
        if (reference.pbc[axis])
            maxCutoff = std::min(maxCutoff, .5 * std::hypot(reference.cell[axis * 3], reference.cell[axis * 3 + 1],
                                                            reference.cell[axis * 3 + 2]));
    const double volume = std::abs(cellDeterminant(reference.cell));
    double cutoff = volume > 0 ? 1.2 * std::cbrt(volume / double(sites)) : 2.0;
    constexpr uint32_t unassigned = std::numeric_limits<uint32_t>::max();
    std::vector<uint32_t> site(count, unassigned);
    std::vector<double> nearest(count);
    for (int attempt = 0; attempt < 32; ++attempt) {
        cutoff = std::min(cutoff, maxCutoff);
        std::fill(site.begin(), site.end(), unassigned);
        std::fill(nearest.begin(), nearest.end(), std::numeric_limits<double>::infinity());
        auto consider = [&](uint32_t siteIndex, uint32_t particle, double distanceSquared) {
            if (distanceSquared < nearest[particle] ||
                (distanceSquared == nearest[particle] && siteIndex < site[particle])) {
                nearest[particle] = distanceSquared;
                site[particle] = siteIndex;
            }
        };
        forEachNeighborPair(merged, cutoff, [&](uint32_t a, uint32_t b, double distanceSquared) {
            if (a < sites && b >= sites) consider(a, uint32_t(b - sites), distanceSquared);
            else if (b < sites && a >= sites) consider(b, uint32_t(a - sites), distanceSquared);
        }, cancel);
        if (std::find(site.begin(), site.end(), unassigned) == site.end() || cutoff >= maxCutoff) break;
        cutoff *= 1.5;
    }
    if (std::find(site.begin(), site.end(), unassigned) != site.end())
        throw std::runtime_error("Wigner-Seitz analysis could not assign every particle to a reference site");
    std::vector<uint32_t> occupancy(sites, 0);
    for (uint32_t s : site) ++occupancy[s];
    uint64_t vacancies = 0, interstitials = 0;
    std::map<uint32_t, uint64_t> histogram;
    for (uint32_t value : occupancy) {
        if (value == 0) ++vacancies;
        else interstitials += value - 1;
        ++histogram[value];
    }
    DataTable table;
    table.name = "Wigner-Seitz site occupancy";
    table.columns = {"Occupancy", "Site count"};
    for (const auto &[value, sitesWithValue] : histogram)
        table.rows.push_back({std::to_string(value), std::to_string(sitesWithValue)});
    if (outputSites) {
        // Show the reference sites, so vacancies stay visible as empty sites.
        auto tables = std::move(data.tables);
        auto attributes = std::move(data.globalAttributes);
        data = reference;
        data.tables = std::move(tables);
        data.globalAttributes = std::move(attributes);
        data.scalarProperties["Occupancy"].assign(occupancy.begin(), occupancy.end());
        r.selected.assign(sites, 0);
        r.colorSelected.assign(sites, 1);
    } else {
        std::vector<double> siteIndex(site.begin(), site.end()), siteOccupancy(count);
        for (size_t i = 0; i < count; ++i) siteOccupancy[i] = occupancy[site[i]];
        data.scalarProperties["Site Index"] = std::move(siteIndex);
        data.scalarProperties["Site Occupancy"] = std::move(siteOccupancy);
    }
    data.globalAttributes["WignerSeitz.vacancy_count"] = double(vacancies);
    data.globalAttributes["WignerSeitz.interstitial_count"] = double(interstitials);
    data.tables.push_back(std::move(table));
}

// ---- Hide selected -------------------------------------------------------------
// Sets Hidden = 1 on selected particles (already hidden ones stay hidden) and clears the
// selection, as OVITO's Hide selected modifier does. Hidden particles stay in the dataset
// for downstream nodes and export; only rendering skips them and their bonds.
inline void hideSelectedParticles(PipelineResult &r) {
    const size_t n = r.data.atoms.size();
    if (r.selected.size() != n) throw std::runtime_error("Selection length does not match particle count");
    auto &hidden = r.data.scalarProperties["Hidden"];
    if (hidden.size() != n) hidden.assign(n, 0.0);
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (r.selected[i]) hidden[i] = 1;
        else if (!std::isfinite(hidden[i])) hidden[i] = 0;
        count += hidden[i] != 0;
        r.selected[i] = 0;
    }
    r.data.globalAttributes["HideSelected.hidden_count"] = double(count);
}

// ---- Center of mass correction and shrink-wrap ---------------------------------

inline std::array<double, 3> centerOfMass(const Dataset &data, const std::vector<uint8_t> *selection) {
    const auto masses = data.scalarProperties.find("Mass");
    const bool weighted = masses != data.scalarProperties.end() &&
        masses->second.size() == data.atoms.size() &&
        std::any_of(masses->second.begin(), masses->second.end(), [](double m) { return m > 0; });
    std::array<double, 3> sum{};
    double total = 0;
    for (size_t i = 0; i < data.atoms.size(); ++i) {
        if (selection && !(*selection)[i]) continue;
        const double weight = weighted ? masses->second[i] : 1.0;
        sum[0] += weight * data.atoms[i].x;
        sum[1] += weight * data.atoms[i].y;
        sum[2] += weight * data.atoms[i].z;
        total += weight;
    }
    if (!(total > 0)) throw std::runtime_error("Center of mass correction has no particles to average");
    for (double &component : sum) component /= total;
    return sum;
}
// Positions are used as stored: wrapped trajectories must be unwrapped upstream.
inline void correctCenterOfMass(PipelineResult &r, const Dataset &reference, bool onlySelected,
                                std::atomic<bool> *cancel) {
    Dataset &data = r.data;
    const std::vector<uint8_t> *selection = onlySelected ? &r.selected : nullptr;
    if (selection && selection->size() != data.atoms.size())
        throw std::runtime_error("Selection length does not match particle count");
    const auto current = centerOfMass(data, selection);
    std::vector<uint8_t> referenceSelection;
    if (selection) {
        const auto mapping = matchReferenceParticles(data, reference, cancel);
        referenceSelection.assign(reference.atoms.size(), 0);
        for (size_t i = 0; i < mapping.size(); ++i)
            if ((*selection)[i]) referenceSelection[mapping[i]] = 1;
    }
    const auto target = centerOfMass(reference, selection ? &referenceSelection : nullptr);
    for (size_t i = 0; i < data.atoms.size(); ++i) {
        if ((i & 65535) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
        data.atoms[i].x = float(data.atoms[i].x + target[0] - current[0]);
        data.atoms[i].y = float(data.atoms[i].y + target[1] - current[1]);
        data.atoms[i].z = float(data.atoms[i].z + target[2] - current[2]);
    }
    static constexpr const char *axes[3]{"X", "Y", "Z"};
    for (int axis = 0; axis < 3; ++axis) {
        data.globalAttributes[std::string("CenterOfMassCorrection.current.") + axes[axis]] = current[size_t(axis)];
        data.globalAttributes[std::string("CenterOfMassCorrection.reference.") + axes[axis]] = target[size_t(axis)];
    }
}
// Sets the cell to the axis-aligned box of the particle centers, ignoring radii. A flat
// or single-particle system gets a 1-length-unit extent on those axes to stay invertible.
inline void shrinkWrapCell(Dataset &data) {
    if (data.atoms.empty())
        throw std::runtime_error("Shrink-wrap simulation box requires at least one particle");
    double lo[3], hi[3];
    for (int axis = 0; axis < 3; ++axis) {
        lo[axis] = std::numeric_limits<double>::infinity();
        hi[axis] = -lo[axis];
    }
    for (const auto &atom : data.atoms)
        for (int axis = 0; axis < 3; ++axis) {
            const double value = coordinate(atom, axis);
            if (!std::isfinite(value))
                throw std::runtime_error("Shrink-wrap simulation box found a non-finite particle coordinate");
            lo[axis] = std::min(lo[axis], value);
            hi[axis] = std::max(hi[axis], value);
        }
    data.origin = {float(lo[0]), float(lo[1]), float(lo[2])};
    data.cell = {};
    for (int axis = 0; axis < 3; ++axis) {
        const double extent = hi[axis] - lo[axis];
        data.cell[size_t(axis) * 4] = extent > 0 ? extent : 1.0;
    }
}
