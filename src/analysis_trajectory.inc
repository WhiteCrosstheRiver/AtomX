// Analyses that sweep the whole trajectory, and ring statistics of the bond graph.
// Included inside namespace atomx by core.hpp, after analysis_modifiers_fields.inc.

// What a sweep produced. It does not depend on the displayed frame, so the application
// caches it per node and re-applies it when the frame changes.
struct TrajectoryOutput {
    std::vector<DataTable> tables;
    std::map<std::string, double> attributes;
    std::map<std::string, std::vector<double>> scalars;
    std::vector<double> identifiers; // Particle order of `scalars`, when the file has identifiers.
    std::shared_ptr<const TrajectoryLineSet> lines;
};
using FrameEvaluator = std::function<Dataset(int frame)>;

inline void applyTrajectoryOutput(Dataset &data, const TrajectoryOutput &output, std::atomic<bool> *cancel) {
    for (const auto &[name, value] : output.attributes) data.globalAttributes[name] = value;
    data.tables.insert(data.tables.end(), output.tables.begin(), output.tables.end());
    if (output.lines) data.trajectoryLines.set = output.lines;
    if (output.scalars.empty()) return;
    std::vector<uint32_t> order;
    if (const auto ids = data.scalarProperties.find("Particle Identifier");
        !output.identifiers.empty() && ids != data.scalarProperties.end() && ids->second.size() == data.atoms.size()) {
        Dataset reference;
        reference.atoms.resize(output.identifiers.size());
        reference.scalarProperties["Particle Identifier"] = output.identifiers;
        order = matchReferenceParticles(data, reference, cancel);
    }
    for (const auto &[name, values] : output.scalars) {
        if (values.size() != data.atoms.size())
            throw std::runtime_error("Time averaging requires a constant number of particles");
        auto &target = data.scalarProperties[name];
        target.resize(values.size());
        for (size_t i = 0; i < values.size(); ++i) target[i] = values[order.empty() ? i : order[i]];
    }
}

// ---- Time series ----------------------------------------------------------------

// One table row per frame, for a global attribute evaluated by the upstream pipeline.
inline TrajectoryOutput sweepTimeSeries(const std::string &attribute, int frames, const FrameEvaluator &evaluate,
                                        std::atomic<bool> *cancel) {
    TrajectoryOutput output;
    DataTable table;
    table.name = "Time series";
    table.columns = {"Frame", attribute};
    for (int frame = 0; frame < frames; ++frame) {
        if (cancel && *cancel) throw std::runtime_error("Cancelled");
        const Dataset data = evaluate(frame);
        const auto found = data.globalAttributes.find(attribute);
        if (found == data.globalAttributes.end())
            throw std::runtime_error("Frame " + std::to_string(frame) + " has no global attribute " + attribute);
        table.rows.push_back({std::to_string(frame), formatDataNumber(found->second)});
    }
    output.tables.push_back(std::move(table));
    return output;
}

// ---- Time averaging -------------------------------------------------------------

// Means a particle property or, failing that, a global attribute over all frames.
// Output is named "<input> Average", or replaces the input when replace is set.
inline TrajectoryOutput sweepTimeAverage(const std::string &input, bool replace, int frames,
                                         const FrameEvaluator &evaluate, std::atomic<bool> *cancel) {
    TrajectoryOutput output;
    const std::string name = replace ? input : input + " Average";
    double attributeSum = 0;
    std::vector<double> propertySum;
    std::vector<uint32_t> order;
    Dataset first;
    bool isProperty = false;
    for (int frame = 0; frame < frames; ++frame) {
        if (cancel && *cancel) throw std::runtime_error("Cancelled");
        Dataset data = evaluate(frame);
        if (frame == 0) {
            isProperty = input.rfind("Position.", 0) == 0 || data.scalarProperties.count(input) ||
                data.vectorProperties.count(input.substr(0, input.rfind('.')));
            if (!isProperty && !data.globalAttributes.count(input))
                throw std::runtime_error("Time averaging found no property or attribute named " + input);
            if (isProperty) {
                propertySum.assign(data.atoms.size(), 0);
                if (const auto ids = data.scalarProperties.find("Particle Identifier"); ids != data.scalarProperties.end())
                    output.identifiers = ids->second;
                first.atoms.resize(data.atoms.size());
                first.scalarProperties = {};
                if (!output.identifiers.empty()) first.scalarProperties["Particle Identifier"] = output.identifiers;
            }
        }
        if (!isProperty) {
            const auto found = data.globalAttributes.find(input);
            if (found == data.globalAttributes.end())
                throw std::runtime_error("Frame " + std::to_string(frame) + " has no global attribute " + input);
            attributeSum += found->second;
            continue;
        }
        if (data.atoms.size() != propertySum.size())
            throw std::runtime_error("Time averaging requires a constant number of particles");
        const auto values = particlePropertyValues(data, input, cancel);
        const auto mapping = matchReferenceParticles(data, first, cancel);
        for (size_t i = 0; i < values.size(); ++i) propertySum[mapping[i]] += values[i];
    }
    if (frames < 1) return output;
    if (isProperty) {
        for (double &value : propertySum) value /= frames;
        output.scalars[name] = std::move(propertySum);
    } else {
        output.attributes[name] = attributeSum / frames;
    }
    return output;
}

// ---- Smooth trajectory ----------------------------------------------------------

// Replaces positions by their mean over a window centered on the current frame. The
// window is cut short at the ends of the trajectory rather than shifted. With the
// minimum image convention each offset is taken relative to the current position, so
// wrapped trajectories stay continuous.
inline void smoothTrajectory(Dataset &data, int currentFrame, int frames, int window, bool minimumImage,
                             const FrameEvaluator &evaluate, std::atomic<bool> *cancel) {
    if (window < 1 || window > 1001) throw std::runtime_error("The smoothing window must be between 1 and 1001 frames");
    if (window == 1 || frames < 2) return;
    const int first = std::max(0, currentFrame - (window - 1) / 2);
    const int last = std::min(frames - 1, currentFrame + window / 2);
    const size_t count = data.atoms.size();
    const bool anyPeriodic = minimumImage && std::any_of(data.pbc.begin(), data.pbc.end(), [](bool p) { return p; });
    std::array<std::array<double, 3>, 3> inverse{};
    if (anyPeriodic && count) inverse = cellInverse(data.cell);
    std::vector<std::array<double, 3>> offset(count, std::array<double, 3>{});
    for (int frame = first; frame <= last; ++frame) {
        if (frame == currentFrame) continue;
        if (cancel && *cancel) throw std::runtime_error("Cancelled");
        const Dataset other = evaluate(frame);
        const auto mapping = matchReferenceParticles(data, other, cancel);
        for (size_t i = 0; i < count; ++i) {
            const Atom &a = other.atoms[mapping[i]];
            double d[3]{double(a.x) - data.atoms[i].x, double(a.y) - data.atoms[i].y, double(a.z) - data.atoms[i].z};
            if (anyPeriodic) {
                double fractional[3];
                for (int axis = 0; axis < 3; ++axis)
                    fractional[axis] = inverse[axis][0] * d[0] + inverse[axis][1] * d[1] + inverse[axis][2] * d[2];
                for (int axis = 0; axis < 3; ++axis) {
                    if (!data.pbc[size_t(axis)]) continue;
                    const double image = std::round(fractional[axis]);
                    for (int xyz = 0; xyz < 3; ++xyz) d[xyz] -= image * data.cell[size_t(axis) * 3 + size_t(xyz)];
                }
            }
            for (int xyz = 0; xyz < 3; ++xyz) offset[i][size_t(xyz)] += d[xyz];
        }
    }
    const double samples = last - first + 1;
    for (size_t i = 0; i < count; ++i) {
        data.atoms[i].x = float(data.atoms[i].x + offset[i][0] / samples);
        data.atoms[i].y = float(data.atoms[i].y + offset[i][1] / samples);
        data.atoms[i].z = float(data.atoms[i].z + offset[i][2] / samples);
    }
}

// ---- Diffusion coefficient ------------------------------------------------------

// Windowed mean-squared displacement with the Einstein relation MSD = 6 D tau + b. The
// fit has a free intercept and uses lag times between fitLow and fitHigh of the longest
// lag. timePerFrame sets the time unit, so D is in squared length per that unit.
inline TrajectoryOutput sweepDiffusion(int frames, int originStride, double timePerFrame, double fitLow, double fitHigh,
                                       const FrameEvaluator &evaluate, std::atomic<bool> *cancel) {
    if (originStride < 1) throw std::runtime_error("The origin stride must be at least 1");
    if (!(timePerFrame > 0)) throw std::runtime_error("The time per frame must be positive");
    if (!(fitLow >= 0) || !(fitHigh > fitLow) || fitHigh > 1)
        throw std::runtime_error("The fit window must satisfy 0 <= start < end <= 1");
    TrajectoryOutput output;
    DataTable table;
    table.name = "Mean squared displacement";
    table.columns = {"Lag time", "MSD", "Samples"};
    if (frames < 2) { output.tables.push_back(std::move(table)); return output; }
    std::vector<double> squared(size_t(frames), 0.0);
    std::vector<uint64_t> samples(size_t(frames), 0);
    std::vector<std::vector<std::array<double, 3>>> origins;
    std::vector<int> originFrames;
    std::vector<std::array<double, 3>> unwrapped;
    Dataset previous;
    for (int frame = 0; frame < frames; ++frame) {
        if (cancel && *cancel) throw std::runtime_error("Cancelled");
        Dataset data = evaluate(frame);
        const size_t count = data.atoms.size();
        if (frame == 0) {
            unwrapped.resize(count);
            for (size_t i = 0; i < count; ++i)
                unwrapped[i] = {double(data.atoms[i].x), double(data.atoms[i].y), double(data.atoms[i].z)};
        } else {
            // Particles are tracked by identifier; steps across a boundary use minimum image.
            const auto mapping = matchReferenceParticles(data, previous, cancel);
            if (count != unwrapped.size()) throw std::runtime_error("Diffusion coefficient requires a constant number of particles");
            const bool anyPeriodic = std::any_of(data.pbc.begin(), data.pbc.end(), [](bool p) { return p; });
            std::array<std::array<double, 3>, 3> inverse{};
            if (anyPeriodic && count) inverse = cellInverse(data.cell);
            std::vector<std::array<double, 3>> next(count);
            for (size_t i = 0; i < count; ++i) {
                const Atom &before = previous.atoms[mapping[i]];
                double d[3]{double(data.atoms[i].x) - before.x, double(data.atoms[i].y) - before.y, double(data.atoms[i].z) - before.z};
                if (anyPeriodic) {
                    double fractional[3];
                    for (int axis = 0; axis < 3; ++axis)
                        fractional[axis] = inverse[axis][0] * d[0] + inverse[axis][1] * d[1] + inverse[axis][2] * d[2];
                    for (int axis = 0; axis < 3; ++axis) {
                        if (!data.pbc[size_t(axis)]) continue;
                        const double image = std::round(fractional[axis]);
                        for (int xyz = 0; xyz < 3; ++xyz) d[xyz] -= image * data.cell[size_t(axis) * 3 + size_t(xyz)];
                    }
                }
                const auto &last = unwrapped[mapping[i]];
                next[i] = {last[0] + d[0], last[1] + d[1], last[2] + d[2]};
            }
            // Keep the first frame's particle order, which the origins are stored in.
            for (size_t i = 0; i < count; ++i) unwrapped[mapping[i]] = next[i];
            std::vector<Atom> ordered(count);
            for (size_t i = 0; i < count; ++i) ordered[mapping[i]] = data.atoms[i];
            data.atoms = std::move(ordered);
            if (const auto ids = previous.scalarProperties.find("Particle Identifier"); ids != previous.scalarProperties.end())
                data.scalarProperties["Particle Identifier"] = ids->second;
        }
        if (frame % originStride == 0) {
            if ((double(origins.size()) + 1) * double(count) * sizeof(std::array<double, 3>) > 2e9)
                throw std::runtime_error("Diffusion coefficient would need over 2 GB: raise the origin stride");
            origins.push_back(unwrapped);
            originFrames.push_back(frame);
        }
        for (size_t o = 0; o < origins.size(); ++o) {
            const int lag = frame - originFrames[o];
            if (lag <= 0) continue;
            if (cancel && *cancel) throw std::runtime_error("Cancelled");
            double total = 0;
            for (size_t i = 0; i < count; ++i)
                for (size_t xyz = 0; xyz < 3; ++xyz) {
                    const double d = unwrapped[i][xyz] - origins[o][i][xyz];
                    total += d * d;
                }
            squared[size_t(lag)] += total;
            samples[size_t(lag)] += count;
        }
        previous = std::move(data);
    }
    double sumT = 0, sumM = 0, sumTT = 0, sumTM = 0;
    int fitted = 0;
    const double longest = (frames - 1) * timePerFrame;
    for (int lag = 1; lag < frames; ++lag) {
        if (!samples[size_t(lag)]) continue;
        const double tau = lag * timePerFrame, msd = squared[size_t(lag)] / double(samples[size_t(lag)]);
        table.rows.push_back({formatDataNumber(tau), formatDataNumber(msd), std::to_string(samples[size_t(lag)])});
        if (tau >= fitLow * longest && tau <= fitHigh * longest) {
            sumT += tau; sumM += msd; sumTT += tau * tau; sumTM += tau * msd;
            ++fitted;
        }
    }
    const double denominator = fitted * sumTT - sumT * sumT;
    output.attributes["Diffusion.fit_points"] = fitted;
    if (fitted >= 2 && denominator > 0) {
        const double slope = (fitted * sumTM - sumT * sumM) / denominator;
        output.attributes["Diffusion.coefficient"] = slope / 6;
        output.attributes["Diffusion.intercept"] = (sumM - slope * sumT) / fitted;
    }
    output.tables.push_back(std::move(table));
    return output;
}

// ---- Find rings (King 1967; Franzblau 1991) -------------------------------------

// Shortest-path rings of the bond graph: a closed path where the distance along the ring
// between any two members equals their graph distance. Vertices are particles, so a ring
// that exists only through a periodic image of itself in a very small cell is not resolved.
inline void computeRings(Dataset &data, int minSize, int maxSize, std::atomic<bool> *cancel) {
    if (minSize < 3 || maxSize < minSize || maxSize > 36)
        throw std::runtime_error("Ring sizes must satisfy 3 <= minimum <= maximum <= 36");
    const size_t count = data.atoms.size();
    std::vector<std::vector<uint32_t>> graph(count);
    for (const auto &bond : data.bonds) {
        if (bond.a >= count || bond.b >= count)
            throw std::runtime_error("A bond refers to a particle outside the dataset");
        if (bond.a != bond.b) { graph[bond.a].push_back(bond.b); graph[bond.b].push_back(bond.a); }
    }
    for (auto &list : graph) {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    }
    uint64_t steps = 0;
    constexpr uint64_t stepLimit = 2000000000;
    constexpr size_t ringLimit = 1000000;
    std::vector<int> distance(count, -1);
    std::vector<uint32_t> touched;
    // Graph distances from `from` up to `depth`, in `distance`, reset through `touched`.
    const auto breadthFirst = [&](uint32_t from, int depth) {
        for (uint32_t v : touched) distance[v] = -1;
        touched.assign(1, from);
        distance[from] = 0;
        for (size_t head = 0; head < touched.size(); ++head) {
            const uint32_t v = touched[head];
            if (distance[v] == depth) continue;
            for (uint32_t w : graph[v])
                if (distance[w] < 0) { distance[w] = distance[v] + 1; touched.push_back(w); }
            if (++steps > stepLimit) throw std::runtime_error("Find rings exceeded its work limit: reduce the maximum ring size");
        }
    };
    const auto isShortestPathRing = [&](const std::vector<uint32_t> &ring) {
        const int size = int(ring.size());
        for (int a = 0; a < size; ++a) {
            breadthFirst(ring[size_t(a)], size / 2);
            for (int b = 0; b < size; ++b) {
                const int along = std::min(std::abs(a - b), size - std::abs(a - b));
                const int graphDistance = distance[ring[size_t(b)]];
                if (graphDistance < 0 || graphDistance < along) return false;
            }
        }
        return true;
    };
    std::vector<std::vector<uint32_t>> rings;
    std::vector<int> rootDistance(count, -1);
    std::vector<uint32_t> path;
    std::vector<uint8_t> onPath(count, 0);
    const std::function<void(uint32_t)> extend = [&](uint32_t v) {
        if (++steps > stepLimit) throw std::runtime_error("Find rings exceeded its work limit: reduce the maximum ring size");
        const int length = int(path.size()) - 1;
        const uint32_t root = path.front();
        for (uint32_t w : graph[v]) {
            if (w == root) {
                // The root is the smallest member, and the second vertex beats the last one,
                // so each ring is found once.
                if (length + 1 >= minSize && path[1] < v) {
                    if (isShortestPathRing(path)) {
                        rings.push_back(path);
                        if (rings.size() > ringLimit) throw std::runtime_error("Find rings found over 1,000,000 rings");
                    }
                }
                continue;
            }
            if (w < root || onPath[w] || length + 1 >= maxSize) continue;
            // Around a shortest-path ring the distance to the root climbs one step at a time,
            // may hold once at the far side of an odd ring, then falls one step at a time.
            const int here = rootDistance[v], there = rootDistance[w];
            const bool climbing = there == length + 1 && here == length;
            const bool falling = there >= 0 && (there == here - 1 || there == here);
            if (!climbing && !falling) continue;
            path.push_back(w);
            onPath[w] = 1;
            extend(w);
            onPath[w] = 0;
            path.pop_back();
        }
    };
    for (uint32_t root = 0; root < count; ++root) {
        if ((root & 255) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
        if (graph[root].size() < 2) continue;
        breadthFirst(root, maxSize / 2 + 1);
        for (uint32_t v : touched) rootDistance[v] = distance[v];
        const auto reached = touched;
        path.assign(1, root);
        onPath[root] = 1;
        extend(root);
        onPath[root] = 0;
        for (uint32_t v : reached) rootDistance[v] = -1;
    }
    std::vector<uint64_t> histogram(size_t(maxSize) + 1, 0);
    std::vector<double> membership(count, 0);
    DataTable list;
    list.name = "Rings";
    list.columns = {"Size", "Particles"};
    list.rows.reserve(rings.size());
    for (const auto &ring : rings) {
        ++histogram[ring.size()];
        std::string members;
        for (uint32_t v : ring) {
            ++membership[v];
            if (!members.empty()) members += ' ';
            members += std::to_string(v);
        }
        list.rows.push_back({std::to_string(ring.size()), std::move(members)});
    }
    DataTable sizes;
    sizes.name = "Ring size histogram";
    sizes.columns = {"Ring size", "Count"};
    for (int size = minSize; size <= maxSize; ++size)
        sizes.rows.push_back({std::to_string(size), std::to_string(histogram[size_t(size)])});
    data.tables.push_back(std::move(sizes));
    data.tables.push_back(std::move(list));
    data.scalarProperties["Ring Count"] = std::move(membership);
    data.globalAttributes["FindRings.ring_count"] = double(rings.size());
}
