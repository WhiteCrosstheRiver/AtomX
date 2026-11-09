// Frame differences, binned fields, the structure factor and diamond identification.
// Included inside namespace atomx by core.hpp, after analysis_modifiers.inc.

// ---- Difference between frames --------------------------------------------------

// Plain subtraction of a property against the reference frame, with no periodic
// handling; Displacement vectors is the tool for positions. Global attributes present
// in both frames are differenced too.
inline void computeFrameDifference(PipelineResult &r, const Dataset &reference, const std::string &property,
                                   bool tolerateNew, bool selectNew, std::atomic<bool> *cancel) {
    Dataset &data = r.data;
    if (property.empty()) throw std::runtime_error("Difference between frames requires a particle property");
    const auto mapping = matchReferenceParticles(data, reference, cancel, tolerateNew);
    const size_t count = data.atoms.size();
    size_t added = 0;
    for (uint32_t index : mapping) if (index == noReferenceParticle) ++added;
    const std::string output = property + " (Difference)";
    const auto vectorHere = data.vectorProperties.find(property);
    if (vectorHere != data.vectorProperties.end()) {
        const auto there = reference.vectorProperties.find(property);
        if (there == reference.vectorProperties.end() || there->second.size() != reference.atoms.size())
            throw std::runtime_error("The reference frame has no particle property " + property);
        std::vector<Vec3> delta(count);
        for (size_t i = 0; i < count; ++i) {
            if ((i & 65535) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
            if (mapping[i] == noReferenceParticle) continue;
            const Vec3 &a = vectorHere->second[i], &b = there->second[mapping[i]];
            delta[i] = {a.x - b.x, a.y - b.y, a.z - b.z};
        }
        data.vectorProperties[output] = std::move(delta);
        data.propertyComponents[output] = "XYZ";
    } else {
        const auto current = particlePropertyValues(data, property, cancel);
        const auto previous = particlePropertyValues(reference, property, cancel);
        std::vector<double> delta(count);
        for (size_t i = 0; i < count; ++i) {
            if ((i & 65535) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
            if (mapping[i] != noReferenceParticle) delta[i] = current[i] - previous[mapping[i]];
        }
        data.scalarProperties[output] = std::move(delta);
    }
    std::vector<std::pair<std::string, double>> attributes;
    for (const auto &[name, value] : data.globalAttributes)
        if (const auto there = reference.globalAttributes.find(name); there != reference.globalAttributes.end())
            attributes.push_back({name + " (Difference)", value - there->second});
    for (const auto &[name, value] : attributes) data.globalAttributes[name] = value;
    data.globalAttributes["DifferenceBetweenFrames.new_particles"] = double(added);
    if (selectNew) {
        r.selected.assign(count, 0);
        for (size_t i = 0; i < count; ++i) r.selected[i] = mapping[i] == noReferenceParticle;
    }
}

// ---- Spatial binning ------------------------------------------------------------

// Bins follow the cell vectors, so they stay aligned in a sheared cell. operation is
// 0 min, 1 max, 2 mean, 3 sum, 4 sum / bin volume. Empty bins report 0. An empty
// property counts particles.
inline DataTable binParticles(const Dataset &data, const std::string &property, const int binCounts[3],
                              int operation, const std::vector<uint8_t> *selection, bool derivative,
                              std::atomic<bool> *cancel, std::shared_ptr<const VoxelGrid> *gridOutput = nullptr) {
    size_t total = 1;
    int dimensions = 0, firstAxis = 0;
    for (int axis = 2; axis >= 0; --axis) {
        if (binCounts[axis] < 1) throw std::runtime_error("Spatial binning needs at least one bin per direction");
        total *= size_t(binCounts[axis]);
        if (total > 1000000) throw std::runtime_error("Spatial binning is limited to 1,000,000 bins");
        if (binCounts[axis] > 1) { ++dimensions; firstAxis = axis; }
    }
    if (operation < 0 || operation > 4) throw std::runtime_error("Unknown spatial binning operation");
    if (derivative && dimensions != 1) throw std::runtime_error("The first derivative needs 1D binning");
    if (selection && selection->size() != data.atoms.size())
        throw std::runtime_error("Selection length does not match particle count");
    const auto inverse = cellInverse(data.cell);
    const bool counting = property.empty();
    const std::vector<double> values = counting ? std::vector<double>{} : particlePropertyValues(data, property, cancel);
    std::vector<double> sum(total), lowest(total, std::numeric_limits<double>::infinity()),
        highest(total, -std::numeric_limits<double>::infinity());
    std::vector<uint64_t> population(total);
    for (size_t i = 0; i < data.atoms.size(); ++i) {
        if ((i & 65535) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
        if (selection && !(*selection)[i]) continue;
        const double value = counting ? 1.0 : values[i];
        if (!std::isfinite(value)) continue;
        const auto f = fractionalPosition(data, data.atoms[i], inverse);
        size_t index = 0, stride = 1;
        bool inside = true;
        for (int axis = 0; axis < 3 && inside; ++axis) {
            double u = f[size_t(axis)];
            if (data.pbc[size_t(axis)]) u -= std::floor(u);
            else if (u < 0 || u > 1) inside = false; // Non-periodic: outside the cell is not binned.
            if (!inside) break;
            index += stride * std::min(size_t(u * binCounts[axis]), size_t(binCounts[axis] - 1));
            stride *= size_t(binCounts[axis]);
        }
        if (!inside) continue;
        sum[index] += value;
        lowest[index] = std::min(lowest[index], value);
        highest[index] = std::max(highest[index], value);
        ++population[index];
    }
    const double binVolume = std::abs(cellDeterminant(data.cell)) / double(total);
    std::vector<double> result(total);
    for (size_t b = 0; b < total; ++b) {
        if (!population[b]) continue;
        result[b] = operation == 0 ? lowest[b] : operation == 1 ? highest[b]
                  : operation == 2 ? sum[b] / double(population[b])
                  : operation == 3 ? sum[b] : sum[b] / binVolume;
    }
    if (gridOutput && dimensions == 3) {
        // Grid points sit at the bin centers. The histogram is read by Create isosurface.
        auto grid = std::make_shared<VoxelGrid>();
        grid->name = counting ? "Count" : property;
        grid->size = {binCounts[0], binCounts[1], binCounts[2]};
        std::array<double, 3> first{data.origin.x, data.origin.y, data.origin.z};
        for (size_t axis = 0; axis < 3; ++axis) {
            grid->periodic[axis] = data.pbc[axis];
            for (size_t k = 0; k < 3; ++k) {
                grid->step[axis * 3 + k] = data.cell[axis * 3 + k] / binCounts[axis];
                first[k] += .5 * grid->step[axis * 3 + k];
            }
        }
        grid->first = first;
        const auto [low, high] = std::minmax_element(result.begin(), result.end());
        grid->minimum = *low;
        grid->maximum = *high;
        grid->histogram.assign(64, 0);
        const double range = grid->maximum - grid->minimum;
        for (const double value : result)
            ++grid->histogram[range > 0 ? std::min(size_t((value - grid->minimum) / range * 64), size_t(63)) : 0];
        grid->values = result;
        *gridOutput = std::move(grid);
    }
    DataTable table;
    table.name = "Spatial binning";
    table.columns = {"Bin A", "Bin B", "Bin C", "Center A", "Center B", "Center C", "Value", "Particle count"};
    double length[3], step = 1;
    for (int axis = 0; axis < 3; ++axis)
        length[axis] = std::hypot(data.cell[size_t(axis) * 3], data.cell[size_t(axis) * 3 + 1],
                                  data.cell[size_t(axis) * 3 + 2]);
    if (derivative) {
        table.columns.push_back("First derivative");
        step = length[firstAxis] / binCounts[firstAxis];
    }
    table.rows.reserve(total);
    const bool wrap = data.pbc[size_t(firstAxis)];
    size_t b = 0;
    for (int c = 0; c < binCounts[2]; ++c)
        for (int y = 0; y < binCounts[1]; ++y)
            for (int a = 0; a < binCounts[0]; ++a, ++b) {
                if ((b & 4095) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
                std::vector<std::string> row{std::to_string(a), std::to_string(y), std::to_string(c),
                    formatDataNumber((a + .5) / binCounts[0] * length[0]),
                    formatDataNumber((y + .5) / binCounts[1] * length[1]),
                    formatDataNumber((c + .5) / binCounts[2] * length[2]),
                    formatDataNumber(result[b]), std::to_string(population[b])};
                if (derivative) {
                    // Central differences; periodic across a periodic edge, one-sided otherwise.
                    const size_t next = b + 1 < total ? b + 1 : wrap ? 0 : b;
                    const size_t previous = b > 0 ? b - 1 : wrap ? total - 1 : b;
                    const double span = wrap ? 2.0 : double(next - previous);
                    row.push_back(formatDataNumber(span > 0 ? (result[next] - result[previous]) / (span * step) : 0));
                }
                table.rows.push_back(std::move(row));
            }
    return table;
}

// ---- Structure factor, direct method --------------------------------------------

// S(k) = |sum exp(i k.r)|^2 / N, averaged over the allowed wave vectors of the periodic
// cell in each |k| bin. k is in inverse length, including the factor of 2 pi. Only
// half of k-space is summed because S(k) = S(-k).
inline void computeStructureFactor(Dataset &data, double kMin, double kMax, int bins,
                                   const std::vector<uint8_t> *selection, std::atomic<bool> *cancel) {
    if (!std::all_of(data.pbc.begin(), data.pbc.end(), [](bool p) { return p; }))
        throw std::runtime_error("Structure factor requires a fully periodic simulation cell");
    if (!(kMin >= 0) || !(kMax > kMin) || !std::isfinite(kMax))
        throw std::runtime_error("Structure factor needs 0 <= minimum k < maximum k");
    if (bins < 1 || bins > 4096) throw std::runtime_error("Structure factor needs 1 to 4096 bins");
    if (data.sampled()) throw std::runtime_error("Structure factor requires complete, unsampled particle data");
    if (selection && selection->size() != data.atoms.size())
        throw std::runtime_error("Selection length does not match particle count");
    const auto inverse = cellInverse(data.cell);
    std::vector<std::array<double, 3>> points;
    points.reserve(data.atoms.size());
    for (size_t i = 0; i < data.atoms.size(); ++i)
        if (!selection || (*selection)[i])
            points.push_back(fractionalPosition(data, data.atoms[i], inverse));
    if (points.empty()) throw std::runtime_error("Structure factor has no particles to analyze");
    constexpr double tau = 2 * 3.14159265358979323846;
    int reach[3];
    for (int axis = 0; axis < 3; ++axis)
        reach[axis] = int(std::ceil(kMax * std::hypot(data.cell[size_t(axis) * 3], data.cell[size_t(axis) * 3 + 1],
                                                      data.cell[size_t(axis) * 3 + 2]) / tau));
    const double estimate = 4.0 / 3 * 3.14159265358979323846 * reach[0] * reach[1] * reach[2] / 2 * double(points.size());
    if (estimate > 4e9)
        throw std::runtime_error("Structure factor would take too long: reduce the maximum k or the particle count");
    std::vector<double> sum(size_t(bins), 0.0);
    std::vector<uint64_t> samples(size_t(bins), 0);
    const double width = (kMax - kMin) / bins;
    for (int h = 0; h <= reach[0]; ++h) {
        for (int j = h ? -reach[1] : 0; j <= reach[1]; ++j) {
            if (cancel && *cancel) throw std::runtime_error("Cancelled");
            for (int l = (h || j) ? -reach[2] : 1; l <= reach[2]; ++l) {
                double k[3];
                for (size_t xyz = 0; xyz < 3; ++xyz)
                    k[xyz] = tau * (h * inverse[0][xyz] + j * inverse[1][xyz] + l * inverse[2][xyz]);
                const double length = std::hypot(k[0], k[1], k[2]);
                if (length < kMin || length >= kMax) continue;
                double re = 0, im = 0;
                for (const auto &p : points) {
                    const double phase = tau * (h * p[0] + j * p[1] + l * p[2]);
                    re += std::cos(phase);
                    im += std::sin(phase);
                }
                const size_t bin = std::min(size_t((length - kMin) / width), size_t(bins - 1));
                sum[bin] += (re * re + im * im) / double(points.size());
                ++samples[bin];
            }
        }
    }
    DataTable table;
    table.name = "Structure factor";
    table.columns = {"k", "S(k)", "Wave vectors"};
    double peak = 0, peakK = 0;
    for (size_t b = 0; b < size_t(bins); ++b) {
        if (!samples[b]) continue; // A periodic cell allows only discrete k values.
        const double value = sum[b] / double(samples[b]), center = kMin + (double(b) + .5) * width;
        table.rows.push_back({formatDataNumber(center), formatDataNumber(value), std::to_string(samples[b])});
        if (value > peak) { peak = value; peakK = center; }
    }
    data.globalAttributes["StructureFactor.peak_value"] = peak;
    data.globalAttributes["StructureFactor.peak_k"] = peakK;
    data.tables.push_back(std::move(table));
}

// ---- Identify diamond structure (Maras et al. 2016) ----------------------------

enum class DiamondStructure : uint8_t { Other, Cubic, CubicFirst, CubicSecond, Hexagonal, HexagonalFirst, HexagonalSecond };

// The 12 second neighbors come from the 4 first neighbors. A CNA over those 12 gives
// 421 for all of them in cubic diamond, and six 421 plus six 422 in hexagonal diamond.
inline DiamondStructure diamondCore(const NeighborVectorLists &lists, size_t center) {
    const auto &first = lists[center];
    if (first.size() < 4) return DiamondStructure::Other;
    std::vector<std::array<double, 3>> second;
    second.reserve(12);
    for (const auto &neighbor : first) {
        if (lists[neighbor.index].size() < 4) return DiamondStructure::Other;
        size_t added = 0;
        for (const auto &next : lists[neighbor.index]) {
            const std::array<double, 3> v{neighbor.vector[0] + next.vector[0], neighbor.vector[1] + next.vector[1],
                                          neighbor.vector[2] + next.vector[2]};
            if (std::hypot(v[0], v[1], v[2]) < 1e-3 * std::sqrt(neighbor.distanceSquared)) continue; // Back to the center.
            second.push_back(v);
            ++added;
        }
        if (added != 3) return DiamondStructure::Other;
    }
    double mean = 0;
    for (const auto &v : second) mean += std::hypot(v[0], v[1], v[2]);
    mean /= 12;
    const double cutoff = (1 + std::sqrt(2.0)) / 2 * mean, cutoff2 = cutoff * cutoff;
    bool bonded[12][12]{};
    for (size_t a = 0; a < 12; ++a)
        for (size_t b = a + 1; b < 12; ++b) {
            const auto &u = second[a], &v = second[b];
            const double dx = u[0] - v[0], dy = u[1] - v[1], dz = u[2] - v[2];
            bonded[a][b] = bonded[b][a] = dx * dx + dy * dy + dz * dz < cutoff2;
        }
    int n421 = 0, n422 = 0;
    for (size_t a = 0; a < 12; ++a) {
        size_t common[4];
        int commonCount = 0;
        for (size_t b = 0; b < 12; ++b)
            if (bonded[a][b]) {
                if (commonCount == 4) return DiamondStructure::Other;
                common[commonCount++] = b;
            }
        if (commonCount != 4) return DiamondStructure::Other;
        int bonds = 0, degree[4]{};
        for (int p = 0; p < 4; ++p)
            for (int q = p + 1; q < 4; ++q)
                if (bonded[common[p]][common[q]]) { ++bonds; ++degree[p]; ++degree[q]; }
        if (bonds != 2) return DiamondStructure::Other;
        // Two bonds among the common neighbors chain only if they share an atom.
        if (*std::max_element(degree, degree + 4) == 2) ++n422;
        else ++n421;
    }
    if (n421 == 12) return DiamondStructure::Cubic;
    if (n421 == 6 && n422 == 6) return DiamondStructure::Hexagonal;
    return DiamondStructure::Other;
}
inline void computeDiamondStructure(Dataset &data, const std::vector<uint8_t> *selection, std::atomic<bool> *cancel) {
    if (selection && selection->size() != data.atoms.size())
        throw std::runtime_error("Selection length does not match particle count");
    // Unselected particles are ignored as if they did not exist.
    Dataset subset;
    std::vector<uint32_t> original;
    const Dataset *input = &data;
    if (selection) {
        subset.species = data.species;
        subset.cell = data.cell;
        subset.origin = data.origin;
        subset.pbc = data.pbc;
        for (size_t i = 0; i < data.atoms.size(); ++i)
            if ((*selection)[i]) { subset.atoms.push_back(data.atoms[i]); original.push_back(uint32_t(i)); }
        input = &subset;
    }
    const auto lists = nearestNeighborVectors(*input, 4, cancel);
    std::vector<uint8_t> kinds(lists.size());
    for (size_t i = 0; i < lists.size(); ++i) {
        if ((i & 4095) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
        kinds[i] = uint8_t(diamondCore(lists, i));
    }
    // Shells around identified atoms: the first neighbors, then their neighbors.
    const auto mark = [&](DiamondStructure from, DiamondStructure to) {
        const auto snapshot = kinds;
        for (size_t i = 0; i < lists.size(); ++i)
            if (snapshot[i] == uint8_t(from))
                for (const auto &neighbor : lists[i])
                    if (kinds[neighbor.index] == 0) kinds[neighbor.index] = uint8_t(to);
    };
    mark(DiamondStructure::Cubic, DiamondStructure::CubicFirst);
    mark(DiamondStructure::Hexagonal, DiamondStructure::HexagonalFirst);
    mark(DiamondStructure::CubicFirst, DiamondStructure::CubicSecond);
    mark(DiamondStructure::HexagonalFirst, DiamondStructure::HexagonalSecond);
    std::vector<double> structure(data.atoms.size(), 0.0);
    std::array<uint64_t, 7> counts{};
    for (size_t i = 0; i < kinds.size(); ++i) {
        structure[selection ? original[i] : i] = kinds[i];
        ++counts[kinds[i]];
    }
    counts[0] += data.atoms.size() - kinds.size();
    data.scalarProperties["Structure Type"] = std::move(structure);
    DataTable table;
    table.name = "Identify diamond structure";
    table.columns = {"Structure", "Count", "Fraction", "Id"};
    static constexpr const char *names[7]{"Other", "Cubic diamond", "Cubic diamond (1st neighbor)",
        "Cubic diamond (2nd neighbor)", "Hexagonal diamond", "Hexagonal diamond (1st neighbor)",
        "Hexagonal diamond (2nd neighbor)"};
    for (size_t id = 0; id < 7; ++id) {
        data.globalAttributes[std::string("IdentifyDiamond.counts.") + names[id]] = double(counts[id]);
        table.rows.push_back({names[id], std::to_string(counts[id]),
            data.atoms.empty() ? "0" : std::to_string(double(counts[id]) / double(data.atoms.size())),
            std::to_string(id)});
    }
    data.tables.push_back(std::move(table));
}
