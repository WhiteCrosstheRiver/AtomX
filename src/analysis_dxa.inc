// Dislocation extraction after Stukowski, Bulatov and Arsenlis (Modelling Simul. Mater. Sci.
// Eng. 20, 085007, 2012), written from the published description and OVITO's documentation.
// Each step follows that outline: classify atoms, map Delaunay edges to ideal lattice vectors,
// build the interface around bad crystal, find Burgers circuits on it, and sweep them along the
// line. This version finds perfect dislocations in FCC and BCC only. HCP atoms in FCC count as
// bad crystal, so stacking faults and partials are not resolved.

inline constexpr size_t dislocationSegmentLimit = 2000000;

namespace dxa_detail {
using surface_detail::P3;
using surface_detail::add;
using surface_detail::sub;
using surface_detail::mul;
using surface_detail::dot;
using surface_detail::cross;
using IVec = std::array<int, 3>; // A lattice vector in units of a/2.

inline IVec plus(const IVec &a, const IVec &b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline IVec negate(const IVec &a) { return {-a[0], -a[1], -a[2]}; }
inline bool isZero(const IVec &a) { return !a[0] && !a[1] && !a[2]; }

struct Options {
    int crystal = 0;    // 0 FCC, 1 BCC.
    int trialLength = 14, stretchability = 9;
    int smoothing = 4;
    double coarsening = 2;
    bool onlySelected = false;
    float width = 3;
};

struct Crystal {
    int crystal = 0;
    std::vector<uint32_t> cluster;   // 0 is not crystalline.
    std::vector<ptm_detail::Quat> orientation; // Aligned within a cluster.
    std::vector<double> scale;       // Sample length to template length.
    bool maps = true;                // Whether the rotation takes template to sample vectors.
    double templateLattice = 1;      // Lattice constant in template units.
    double spacing = 0;              // Typical nearest-neighbor distance.
    std::array<size_t, 3> counts{};  // Chosen structure, HCP, other.
};

// The rotation to the crystal frame, whichever way the matching reports it.
inline P3 toCrystal(const Crystal &c, size_t particle, const P3 &sample) {
    const auto m = ptm_detail::matrixOf(c.orientation[particle]);
    P3 out = c.maps ? P3{m[0] * sample[0] + m[3] * sample[1] + m[6] * sample[2], m[1] * sample[0] + m[4] * sample[1] + m[7] * sample[2], m[2] * sample[0] + m[5] * sample[1] + m[8] * sample[2]}
                    : P3{m[0] * sample[0] + m[1] * sample[1] + m[2] * sample[2], m[3] * sample[0] + m[4] * sample[1] + m[5] * sample[2], m[6] * sample[0] + m[7] * sample[1] + m[8] * sample[2]};
    return mul(out, c.scale[particle] / c.templateLattice);
}
inline P3 toSample(const Crystal &c, size_t particle, const P3 &crystal) {
    const auto m = ptm_detail::matrixOf(c.orientation[particle]);
    const P3 v = mul(crystal, c.templateLattice / c.scale[particle]);
    return c.maps ? P3{m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2], m[6] * v[0] + m[7] * v[1] + m[8] * v[2]}
                  : P3{m[0] * v[0] + m[3] * v[1] + m[6] * v[2], m[1] * v[0] + m[4] * v[1] + m[7] * v[2], m[2] * v[0] + m[5] * v[1] + m[8] * v[2]};
}

inline Crystal classify(const Dataset &data, const std::vector<uint8_t> &selected, const Options &options, std::atomic<bool> *cancel) {
    using ptm_detail::Quat;
    PtmOptions ptm;
    const PtmType wanted = options.crystal == 0 ? PtmType::FCC : PtmType::BCC;
    ptm.structures = options.crystal == 0 ? (1 << 0) | (1 << 1) : (1 << 2);
    NeighborVectorLists lists;
    const auto results = ptmAnalyze(data, ptm, cancel, &lists);
    const auto &library = ptm_detail::library();
    const auto &points = library.templates[size_t(wanted)];
    Crystal crystal;
    crystal.crystal = options.crystal;
    crystal.templateLattice = options.crystal == 0 ? std::sqrt(2.0) * points.nearestDistance : 2 / std::sqrt(3.0) * points.nearestDistance;
    const size_t count = data.atoms.size();
    crystal.cluster.assign(count, 0);
    crystal.orientation.assign(count, Quat{1, 0, 0, 0});
    crystal.scale.assign(count, 1.0);
    std::vector<uint8_t> good(count);
    std::vector<double> spacings;
    for (size_t i = 0; i < count; ++i) {
        const bool chosen = !options.onlySelected || (i < selected.size() && selected[i]);
        if (chosen && results[i].type == wanted) {
            good[i] = 1;
            ++crystal.counts[0];
            crystal.orientation[i] = results[i].orientation;
            crystal.scale[i] = points.nearestDistance / results[i].interatomicDistance;
            spacings.push_back(results[i].interatomicDistance);
        } else if (chosen && results[i].type == PtmType::HCP) ++crystal.counts[1];
        else ++crystal.counts[2];
    }
    if (spacings.empty()) return crystal;
    std::nth_element(spacings.begin(), spacings.begin() + std::ptrdiff_t(spacings.size() / 2), spacings.end());
    crystal.spacing = spacings[spacings.size() / 2];
    // The matching code reports one rotation direction; find out which by testing neighbors.
    for (size_t i = 0; i < count; ++i) {
        if (!good[i]) continue;
        const auto m = ptm_detail::matrixOf(results[i].orientation);
        double forward = 0, backward = 0;
        for (size_t k = 1; k < points.points.size(); ++k) {
            // The orientation was moved into the fundamental zone, so the neighbor is found
            // as the closest observed vector rather than through the mapping.
            const auto &t = points.points[k];
            const P3 a{m[0] * t[0] + m[1] * t[1] + m[2] * t[2], m[3] * t[0] + m[4] * t[1] + m[5] * t[2], m[6] * t[0] + m[7] * t[1] + m[8] * t[2]};
            const P3 b{m[0] * t[0] + m[3] * t[1] + m[6] * t[2], m[1] * t[0] + m[4] * t[1] + m[7] * t[2], m[2] * t[0] + m[5] * t[1] + m[8] * t[2]};
            double bestA = 1e30, bestB = 1e30;
            for (const auto &neighbor : lists[i]) {
                const P3 v = mul(P3{neighbor.vector[0], neighbor.vector[1], neighbor.vector[2]}, crystal.scale[i] * 1.0);
                const P3 da = sub(a, v), db = sub(b, v);
                bestA = std::min(bestA, dot(da, da));
                bestB = std::min(bestB, dot(db, db));
            }
            forward += bestA;
            backward += bestB;
        }
        crystal.maps = forward <= backward;
        break;
    }
    // Grow clusters across good neighbors with similar orientations, aligning each new atom
    // to its parent by the lattice symmetry. 15 degrees is an AtomX choice.
    const double joinLimit = std::cos(15.0 * std::acos(-1.0) / 180.0 / 2.0);
    std::vector<uint32_t> stack;
    uint32_t clusters = 0;
    for (size_t seed = 0; seed < count; ++seed) {
        if (!good[seed] || crystal.cluster[seed]) continue;
        if ((seed & 4095) == 0) surface_detail::throwIfCancelled(cancel);
        crystal.cluster[seed] = ++clusters;
        stack.assign(1, uint32_t(seed));
        while (!stack.empty()) {
            const uint32_t at = stack.back();
            stack.pop_back();
            for (const auto &neighbor : lists[at]) {
                const size_t other = neighbor.index;
                if (!good[other] || crystal.cluster[other]) continue;
                double best = -1;
                Quat aligned{};
                for (const Quat &g : library.cubic) {
                    const Quat candidate = ptm_detail::multiply(results[other].orientation, g);
                    double d = 0;
                    for (size_t k = 0; k < 4; ++k) d += crystal.orientation[at][k] * candidate[k];
                    if (std::abs(d) > best) { best = std::abs(d); aligned = d < 0 ? Quat{-candidate[0], -candidate[1], -candidate[2], -candidate[3]} : candidate; }
                }
                if (best < joinLimit) continue;
                crystal.orientation[other] = aligned;
                crystal.cluster[other] = clusters;
                stack.push_back(uint32_t(other));
            }
        }
    }
    return crystal;
}

// The nearest lattice vector, in units of a/2. False when nothing is close enough.
inline bool snap(const P3 &cubic, int crystal, IVec &out) {
    const P3 x = mul(cubic, 2);
    const int base[3]{int(std::lround(x[0])), int(std::lround(x[1])), int(std::lround(x[2]))};
    double best = 1e30;
    for (int a = -1; a <= 1; ++a) for (int b = -1; b <= 1; ++b) for (int c = -1; c <= 1; ++c) {
        const IVec n{base[0] + a, base[1] + b, base[2] + c};
        const bool valid = crystal == 0 ? ((n[0] + n[1] + n[2]) & 1) == 0 : (((n[0] ^ n[1]) & 1) == 0 && ((n[1] ^ n[2]) & 1) == 0);
        if (!valid) continue;
        const double dx = n[0] - x[0], dy = n[1] - x[1], dz = n[2] - x[2], squared = dx * dx + dy * dy + dz * dz;
        if (squared < best) { best = squared; out = n; }
    }
    // Residual below a quarter of a (AtomX tolerance); no edge beyond the third lattice shell.
    const double reach = crystal == 0 ? 1.3 : 1.5;
    const double length = 0.5 * std::sqrt(double(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]));
    return std::sqrt(best) * .5 < .25 && length > 0 && length <= reach;
}

struct Image { uint32_t particle; IVec shift; P3 position; };
struct Mesh {
    std::vector<uint32_t> tail, head;
    std::vector<IVec> shift, ideal;
    std::vector<P3> delta;
    std::vector<int> twin;
    std::vector<uint8_t> swept; // Per facet.
    std::vector<std::vector<int>> outgoing;
    static int facet(int e) { return e / 3; }
    static int next(int e) { return facet(e) * 3 + (e + 1) % 3; }
    static int previous(int e) { return facet(e) * 3 + (e + 2) % 3; }
};
struct Tracer {
    Mesh &mesh;
    int limit;
    // Advances the circuit over unswept facets to its left, recording its center.
    void sweep(std::vector<int> circuit, P3 anchor, std::vector<P3> &points, std::atomic<bool> *cancel) {
        size_t cursor = 0;
        const auto corners = [&](const std::vector<int> &c, const P3 &start) {
            std::vector<P3> at{start};
            for (size_t k = 0; k + 1 < c.size(); ++k) at.push_back(add(at.back(), mesh.delta[size_t(c[k])]));
            return at;
        };
        const auto record = [&]() {
            P3 sum{};
            for (const P3 &p : corners(circuit, anchor)) sum = add(sum, p);
            points.push_back(mul(sum, 1.0 / double(circuit.size())));
        };
        record();
        for (size_t iteration = 0; iteration < 20000000; ++iteration) {
            if ((iteration & 4095) == 0) surface_detail::throwIfCancelled(cancel);
            const size_t n = circuit.size();
            if (n < 3) break;
            // Moves the pair (i, i + 1) to the front so edits never split the closing vertex.
            const auto rotateTo = [&](size_t i) {
                anchor = corners(circuit, anchor)[i];
                std::rotate(circuit.begin(), circuit.begin() + std::ptrdiff_t(i), circuit.end());
            };
            bool done = false;
            for (size_t i = 0; i < n && !done; ++i)
                if (mesh.twin[size_t(circuit[i])] == circuit[(i + 1) % n]) {
                    rotateTo(i);
                    circuit.erase(circuit.begin(), circuit.begin() + 2);
                    done = true;
                }
            for (size_t i = 0; i < n && !done; ++i) {
                const int a = circuit[i], b = circuit[(i + 1) % n];
                const int facet = Mesh::facet(a);
                if (Mesh::facet(b) != facet || mesh.swept[size_t(facet)]) continue;
                const int closing = mesh.twin[size_t(Mesh::next(b))];
                if (closing < 0) continue;
                rotateTo(i);
                mesh.swept[size_t(facet)] = 1;
                circuit[0] = closing;
                circuit.erase(circuit.begin() + 1);
                done = true;
            }
            if (!done && int(n) < limit) {
                const auto at = corners(circuit, anchor);
                for (size_t step = 0; step < n && !done; ++step) {
                    const size_t i = (cursor + step) % n;
                    const int edge = circuit[i], facet = Mesh::facet(edge);
                    if (mesh.swept[size_t(facet)]) continue;
                    const int first = mesh.twin[size_t(Mesh::previous(edge))], second = mesh.twin[size_t(Mesh::next(edge))];
                    if (first < 0 || second < 0) continue;
                    const P3 apex = add(at[i], mesh.delta[size_t(first)]);
                    bool repeated = false;
                    for (const P3 &corner : at) { const P3 d = sub(corner, apex); repeated = repeated || dot(d, d) < 1e-6; }
                    if (repeated) continue;
                    mesh.swept[size_t(facet)] = 1;
                    circuit[i] = first;
                    circuit.insert(circuit.begin() + std::ptrdiff_t(i) + 1, second);
                    cursor = (i + 2) % circuit.size();
                    done = true;
                }
            }
            if (!done) break;
            if (circuit.size() >= 3) record();
        }
    }
};
inline std::string latticeText(const IVec &b) {
    int gcd = std::gcd(std::gcd(std::abs(b[0]), std::abs(b[1])), std::abs(b[2]));
    if (gcd == 0) gcd = 1;
    const int divide = gcd % 2 == 0 ? gcd : gcd;
    std::ostringstream text;
    const int numerator = gcd % 2 == 0 ? gcd / 2 : gcd;
    if (gcd % 2 != 0) text << numerator << "/2";
    else if (numerator != 1) text << numerator;
    text << "[" << b[0] / divide << " " << b[1] / divide << " " << b[2] / divide << "]";
    return text.str();
}
inline int familyOf(const IVec &b, int crystal) {
    std::array<int, 3> a{std::abs(b[0]), std::abs(b[1]), std::abs(b[2])};
    std::sort(a.begin(), a.end());
    if (crystal == 0 && a == std::array<int, 3>{0, 1, 1}) return 1;
    if (crystal == 1 && a == std::array<int, 3>{1, 1, 1}) return 1;
    if (a == std::array<int, 3>{0, 0, 2}) return 2;
    if (crystal == 1 && a == std::array<int, 3>{0, 2, 2}) return 3;
    return 0;
}
inline const char *familyName(int family, int crystal) {
    if (family == 1) return crystal == 0 ? "1/2<110>" : "1/2<111>";
    if (family == 2) return "<100>";
    if (family == 3) return "<110>";
    return "Other";
}
} // namespace dxa_detail

inline void computeDislocationAnalysis(PipelineResult &r, const dxa_detail::Options &options, std::atomic<bool> *cancel) {
    using namespace dxa_detail;
    Dataset &data = r.data;
    if (data.sampled()) throw std::runtime_error("Dislocation analysis requires complete, unsampled particle data");
    if (options.trialLength < 3 || options.trialLength > 64) throw std::runtime_error("The trial circuit length must be between 3 and 64");
    if (options.stretchability < 0 || options.stretchability > 64) throw std::runtime_error("The circuit stretchability must be between 0 and 64");
    if (options.smoothing < 0 || !(options.coarsening >= 0)) throw std::runtime_error("Line smoothing and coarsening must not be negative");
    const Crystal crystal = classify(data, r.selected, options, cancel);
    const size_t count = data.atoms.size();
    const char *name = options.crystal == 0 ? "FCC" : "BCC";
    std::vector<double> structure(count, 0.0), cluster(count, 0.0);
    for (size_t i = 0; i < count; ++i) { structure[i] = crystal.cluster[i] ? 1 : 0; cluster[i] = crystal.cluster[i]; }
    data.scalarProperties["Structure Type"] = std::move(structure);
    data.scalarProperties["Cluster"] = std::move(cluster);
    data.globalAttributes[std::string("DislocationAnalysis.counts.") + name] = double(crystal.counts[0]);
    data.globalAttributes["DislocationAnalysis.counts.HCP"] = double(crystal.counts[1]);
    data.globalAttributes["DislocationAnalysis.counts.OTHER"] = double(crystal.counts[2]);
    auto result = std::make_shared<DislocationSet>();
    result->width = options.width;
    double totalLength = 0;
    std::map<int, double> familyLength;
    if (crystal.counts[0]) {
        const surface_detail::CellFrame frame = surface_detail::frameOf(data);
        // A layer of five spacings holds every tetrahedron the interface in the cell can touch.
        const double skin = 5 * crystal.spacing, jitterSize = 1e-4 * crystal.spacing;
        std::mt19937_64 random(0x445841ULL);
        std::uniform_real_distribution<double> uniform(-jitterSize, jitterSize);
        std::vector<Image> images;
        std::vector<P3> points;
        IVec reach{};
        for (size_t axis = 0; axis < 3; ++axis)
            if (frame.periodic[axis]) reach[axis] = int(std::ceil(skin / frame.width(int(axis))));
        for (size_t i = 0; i < count; ++i) {
            if ((i & 65535) == 0) surface_detail::throwIfCancelled(cancel);
            const P3 jitter{uniform(random), uniform(random), uniform(random)};
            if (options.onlySelected && !(i < r.selected.size() && r.selected[i])) continue;
            P3 base{data.atoms[i].x, data.atoms[i].y, data.atoms[i].z}, f = frame.fractional(base);
            for (size_t axis = 0; axis < 3; ++axis)
                if (frame.periodic[axis]) { const double whole = std::floor(f[axis]); base = sub(base, mul(frame.axes[axis], whole)); f[axis] -= whole; }
            for (int a = -reach[0]; a <= reach[0]; ++a) for (int b = -reach[1]; b <= reach[1]; ++b) for (int c = -reach[2]; c <= reach[2]; ++c) {
                const IVec shift{a, b, c};
                bool inside = true;
                for (size_t axis = 0; axis < 3 && inside; ++axis)
                    if (frame.periodic[axis]) { const double layer = skin / frame.width(int(axis)), s = f[axis] + shift[axis]; inside = s >= -layer && s <= 1 + layer; }
                if (!inside) continue;
                if (points.size() >= surfaceDelaunayPointLimit) throw std::runtime_error("Dislocation analysis is limited to 1,000,000 points including periodic images");
                P3 position = base;
                for (size_t axis = 0; axis < 3; ++axis) position = add(position, mul(frame.axes[axis], double(shift[axis])));
                images.push_back({uint32_t(i), shift, position});
                points.push_back(add(position, jitter));
            }
        }
        surface_detail::Tessellation tessellation;
        tessellation.build(std::move(points), cancel);
        const auto edgeVector = [&](int u, int v, IVec &out) {
            const Image &a = images[size_t(u)], &b = images[size_t(v)];
            if (!crystal.cluster[a.particle] || crystal.cluster[a.particle] != crystal.cluster[b.particle]) return false;
            const P3 vector = sub(b.position, a.position);
            const bool direct = a.particle <= b.particle;
            IVec n{};
            if (!snap(toCrystal(crystal, direct ? a.particle : b.particle, direct ? vector : mul(vector, -1)), crystal.crystal, n)) return false;
            out = direct ? n : negate(n);
            return true;
        };
        std::vector<uint8_t> good(tessellation.tets.size());
        for (size_t id = 0; id < tessellation.tets.size(); ++id) {
            if ((id & 65535) == 0) surface_detail::throwIfCancelled(cancel);
            const auto &tet = tessellation.tets[id];
            if (!tet.alive || tessellation.touchesSuper(tet)) continue;
            IVec e[4][4]{};
            bool ok = true;
            for (int i = 0; i < 4 && ok; ++i) for (int j = i + 1; j < 4 && ok; ++j) {
                ok = edgeVector(tet.v[size_t(i)], tet.v[size_t(j)], e[i][j]);
                e[j][i] = negate(e[i][j]);
            }
            // A facet whose lattice vectors do not close lies inside a dislocation core.
            for (int i = 0; i < 4 && ok; ++i) for (int j = i + 1; j < 4 && ok; ++j) for (int k = j + 1; k < 4 && ok; ++k)
                ok = isZero(plus(plus(e[i][j], e[j][k]), e[k][i]));
            good[id] = ok;
        }
        struct Facet { std::array<int, 3> ring; int owner; int face; };
        std::vector<Facet> facets;
        std::map<std::array<int, 9>, int> facetIndex;
        const auto keyOf = [&](const std::array<int, 3> &ring) {
            std::array<std::pair<uint32_t, IVec>, 3> v;
            for (size_t k = 0; k < 3; ++k) v[k] = {images[size_t(ring[k])].particle, images[size_t(ring[k])].shift};
            std::sort(v.begin(), v.end());
            std::array<int, 9> key{};
            for (size_t k = 0; k < 3; ++k) key[k] = int(v[k].first);
            for (size_t k = 1; k < 3; ++k) for (size_t c = 0; c < 3; ++c) key[3 + (k - 1) * 3 + c] = v[k].second[c] - v[0].second[c];
            return key;
        };
        for (size_t id = 0; id < tessellation.tets.size(); ++id) {
            if ((id & 65535) == 0) surface_detail::throwIfCancelled(cancel);
            if (!good[id]) continue;
            const auto &tet = tessellation.tets[id];
            P3 middle{};
            for (const int v : tet.v) middle = add(middle, mul(tessellation.points[size_t(v)], .25));
            const P3 f = frame.fractional(middle);
            bool owned = true;
            for (size_t axis = 0; axis < 3; ++axis)
                if (frame.periodic[axis] && (f[axis] < 0 || f[axis] >= 1)) owned = false;
            if (!owned) continue;
            for (int face = 0; face < 4; ++face) {
                const int other = tet.n[size_t(face)];
                if (other >= 0 && good[size_t(other)]) continue;
                std::array<int, 3> ring{};
                int used = 0;
                for (int k = 0; k < 4; ++k) if (k != face) ring[size_t(used++)] = tet.v[size_t(k)];
                const P3 &a = tessellation.points[size_t(ring[0])], &b = tessellation.points[size_t(ring[1])], &c = tessellation.points[size_t(ring[2])];
                if (dot(cross(sub(b, a), sub(c, a)), sub(a, tessellation.points[size_t(tet.v[size_t(face)])])) < 0) std::swap(ring[1], ring[2]);
                const auto [entry, created] = facetIndex.try_emplace(keyOf(ring), int(facets.size()));
                if (created) facets.push_back({ring, int(id), face});
            }
        }
        Mesh mesh;
        const size_t edges = facets.size() * 3;
        mesh.tail.resize(edges); mesh.head.resize(edges); mesh.shift.resize(edges); mesh.ideal.resize(edges); mesh.delta.resize(edges);
        mesh.twin.assign(edges, -1);
        mesh.swept.assign(facets.size(), 0);
        mesh.outgoing.resize(count);
        for (size_t f = 0; f < facets.size(); ++f) {
            for (size_t k = 0; k < 3; ++k) {
                const int u = facets[f].ring[k], v = facets[f].ring[(k + 1) % 3];
                const size_t e = f * 3 + k;
                mesh.tail[e] = images[size_t(u)].particle;
                mesh.head[e] = images[size_t(v)].particle;
                for (size_t c = 0; c < 3; ++c) mesh.shift[e][c] = images[size_t(v)].shift[c] - images[size_t(u)].shift[c];
                mesh.delta[e] = sub(images[size_t(v)].position, images[size_t(u)].position);
                IVec ideal{};
                edgeVector(u, v, ideal);
                mesh.ideal[e] = ideal;
                mesh.outgoing[mesh.tail[e]].push_back(int(e));
            }
        }
        // The facet next to edge (u, w) is reached by turning through the bad tetrahedra around it.
        for (size_t f = 0; f < facets.size(); ++f) {
            if ((f & 65535) == 0) surface_detail::throwIfCancelled(cancel);
            for (size_t k = 0; k < 3; ++k) {
                const int u = facets[f].ring[k], w = facets[f].ring[(k + 1) % 3];
                int previousThird = facets[f].ring[(k + 2) % 3];
                int current = tessellation.tets[size_t(facets[f].owner)].n[size_t(facets[f].face)];
                int found = -1;
                for (int guard = 0; guard < 4096 && current >= 0; ++guard) {
                    const auto &tet = tessellation.tets[size_t(current)];
                    int fourth = -1, entry = -1;
                    for (int j = 0; j < 4; ++j) {
                        const int v = tet.v[size_t(j)];
                        if (v == previousThird) entry = j;
                        else if (v != u && v != w) fourth = v;
                    }
                    if (entry < 0 || fourth < 0) break;
                    const int next = tet.n[size_t(entry)];
                    if (next < 0) break;
                    if (good[size_t(next)]) {
                        const std::array<int, 3> ring{u, w, fourth};
                        const auto hit = facetIndex.find(keyOf(ring));
                        if (hit != facetIndex.end()) found = hit->second;
                        break;
                    }
                    current = next;
                    previousThird = fourth;
                }
                if (found < 0) continue;
                const size_t e = f * 3 + k;
                for (size_t j = 0; j < 3; ++j) {
                    const size_t candidate = size_t(found) * 3 + j;
                    if (mesh.tail[candidate] == mesh.head[e] && mesh.head[candidate] == mesh.tail[e] &&
                        mesh.shift[candidate] == negate(mesh.shift[e])) { mesh.twin[e] = int(candidate); break; }
                }
            }
        }
        // Keep only edges whose pairing agrees from both sides.
        for (size_t e = 0; e < edges; ++e)
            if (mesh.twin[e] >= 0 && mesh.twin[size_t(mesh.twin[e])] != int(e)) mesh.twin[e] = -1;

        struct Node { uint32_t particle; IVec shift; int depth; IVec burgers; int parent, edge; };
        std::vector<Node> nodes;
        const auto usable = [&](int e) {
            const int t = mesh.twin[size_t(e)];
            return t >= 0 && !mesh.swept[size_t(Mesh::facet(e))] && !mesh.swept[size_t(Mesh::facet(t))];
        };
        // Breadth-first search from a vertex for the first closed path with a Burgers vector.
        const auto findCircuit = [&](uint32_t root, int length, std::vector<int> &circuit) {
            nodes.assign(1, Node{root, {}, 0, {}, -1, -1});
            const int depthLimit = (length + 1) / 2;
            for (size_t head = 0; head < nodes.size(); ++head) {
                const Node current = nodes[head];
                if (current.depth >= depthLimit) continue;
                for (const int e : mesh.outgoing[current.particle]) {
                    if (!usable(e) || (current.edge >= 0 && mesh.twin[size_t(current.edge)] == e)) continue;
                    const IVec shift = plus(current.shift, mesh.shift[size_t(e)]);
                    const IVec burgers = plus(current.burgers, mesh.ideal[size_t(e)]);
                    int target = -1;
                    for (size_t n = 0; n < nodes.size() && target < 0; ++n)
                        if (nodes[n].particle == mesh.head[size_t(e)] && nodes[n].shift == shift) target = int(n);
                    if (target < 0) {
                        nodes.push_back({mesh.head[size_t(e)], shift, current.depth + 1, burgers, int(head), e});
                        continue;
                    }
                    const Node &other = nodes[size_t(target)];
                    if (other.depth + current.depth + 1 != length || nodes[head].parent == target) continue;
                    const IVec closure = plus(burgers, negate(other.burgers));
                    if (isZero(closure)) continue;
                    // Root to here, this edge, then back from the target to the root.
                    circuit.clear();
                    for (int n = int(head); nodes[size_t(n)].parent >= 0; n = nodes[size_t(n)].parent) circuit.push_back(nodes[size_t(n)].edge);
                    std::reverse(circuit.begin(), circuit.end());
                    circuit.push_back(e);
                    for (int n = target; nodes[size_t(n)].parent >= 0; n = nodes[size_t(n)].parent) circuit.push_back(mesh.twin[size_t(nodes[size_t(n)].edge)]);
                    // A valid circuit visits each vertex once.
                    std::vector<std::pair<uint32_t, IVec>> seen;
                    IVec at{};
                    bool simple = true;
                    for (const int c : circuit) {
                        const std::pair<uint32_t, IVec> corner{mesh.tail[size_t(c)], at};
                        simple = simple && std::find(seen.begin(), seen.end(), corner) == seen.end();
                        seen.push_back(corner);
                        at = plus(at, mesh.shift[size_t(c)]);
                    }
                    if (simple) return true;
                }
            }
            return false;
        };
        const int limit = options.trialLength + options.stretchability;
        std::vector<int> circuit;
        Tracer tracer{mesh, limit};
        for (int length = 3; length <= options.trialLength; ++length)
            for (uint32_t root = 0; root < mesh.outgoing.size(); ++root) {
                if ((root & 1023) == 0) surface_detail::throwIfCancelled(cancel);
                if (mesh.outgoing[root].empty()) continue;
                while (findCircuit(root, length, circuit)) {
                    IVec closure{};
                    for (const int c : circuit) closure = plus(closure, mesh.ideal[size_t(c)]);
                    // Corners around the seed circuit, relative to its first vertex.
                    const P3 anchor{data.atoms[root].x, data.atoms[root].y, data.atoms[root].z};
                    std::vector<P3> corners{anchor};
                    for (size_t k = 0; k + 1 < circuit.size(); ++k) corners.push_back(add(corners.back(), mesh.delta[size_t(circuit[k])]));
                    P3 center{};
                    for (const P3 &p : corners) center = add(center, p);
                    center = mul(center, 1.0 / double(corners.size()));
                    P3 area{};
                    for (size_t k = 0; k < corners.size(); ++k) area = add(area, cross(sub(corners[k], center), sub(corners[(k + 1) % corners.size()], center)));
                    std::vector<int> behind;
                    for (auto c = circuit.rbegin(); c != circuit.rend(); ++c) behind.push_back(mesh.twin[size_t(*c)]);
                    std::vector<P3> ahead, back;
                    tracer.sweep(circuit, anchor, ahead, cancel);
                    tracer.sweep(behind, anchor, back, cancel);
                    std::reverse(back.begin(), back.end());
                    if (!back.empty()) back.pop_back(); // The seed circuit appears in both halves.
                    back.insert(back.end(), ahead.begin(), ahead.end());
                    if (back.size() < 2) continue;
                    DislocationLine line;
                    // Coarsen to the requested spacing, then smooth with the ends held in place.
                    const double spacing = options.coarsening * crystal.spacing;
                    line.points.push_back(back.front());
                    for (const P3 &p : back) {
                        const P3 d = sub(p, line.points.back());
                        if (dot(d, d) >= spacing * spacing) line.points.push_back(p);
                    }
                    if (line.points.size() < 2 || back.back() != line.points.back()) line.points.push_back(back.back());
                    for (int iteration = 0; iteration < options.smoothing; ++iteration) {
                        auto previous = line.points;
                        for (size_t k = 1; k + 1 < previous.size(); ++k)
                            line.points[k] = add(mul(previous[k], .5), mul(add(previous[k - 1], previous[k + 1]), .25));
                    }
                    double lineLength = 0;
                    for (size_t k = 0; k + 1 < line.points.size(); ++k) { const P3 d = sub(line.points[k + 1], line.points[k]); lineLength += std::sqrt(dot(d, d)); }
                    if (!(lineLength > 0)) continue;
                    const P3 direction = sub(line.points.back(), line.points.front());
                    // Traversed counter-clockwise about the line, the mapped circuit fails to
                    // close by minus the Burgers vector (the FS/RH sign, which equals LH/SF).
                    const IVec burgers = dot(area, direction) >= 0 ? negate(closure) : closure;
                    line.length = lineLength;
                    line.burgersLattice = burgers;
                    line.family = familyOf(burgers, crystal.crystal);
                    line.familyName = familyName(line.family, crystal.crystal);
                    line.burgersText = latticeText(burgers);
                    line.cluster = int(crystal.cluster[root]);
                    line.burgers = toSample(crystal, root, mul(P3{double(burgers[0]), double(burgers[1]), double(burgers[2])}, .5));
                    totalLength += lineLength;
                    familyLength[line.family] += lineLength;
                    result->lines.push_back(std::move(line));
                }
            }
    }
    DataTable table;
    table.name = "Dislocations";
    table.columns = {"Dislocation", "Burgers vector", "Family", "Length", "Burgers.X", "Burgers.Y", "Burgers.Z", "Cluster"};
    for (size_t i = 0; i < result->lines.size(); ++i) {
        const auto &line = result->lines[i];
        table.rows.push_back({std::to_string(i + 1), line.burgersText, line.familyName, formatDataNumber(line.length), formatDataNumber(line.burgers[0]),
                              formatDataNumber(line.burgers[1]), formatDataNumber(line.burgers[2]), std::to_string(line.cluster)});
    }
    data.tables.push_back(std::move(table));
    data.globalAttributes["DislocationAnalysis.total_line_length"] = totalLength;
    data.globalAttributes["DislocationAnalysis.lines"] = double(result->lines.size());
    if (std::abs(cellDeterminant(data.cell)) > 1e-9) data.globalAttributes["DislocationAnalysis.cell_volume"] = std::abs(cellDeterminant(data.cell));
    for (int family = 0; family < 4; ++family)
        if (familyLength.contains(family)) data.globalAttributes[std::string("DislocationAnalysis.length.") + familyName(family, options.crystal)] = familyLength[family];
    data.dislocations = std::move(result);
}

// Segments of all lines folded into the periodic cell. Returns false when limit was reached.
template <class Emit>
bool forEachDislocationSegment(const Dataset &data, size_t limit, Emit &&emit) {
    if (!data.dislocations) return true;
    using surface_detail::P3;
    const surface_detail::CellFrame frame = surface_detail::frameOf(data);
    size_t emitted = 0;
    for (const auto &line : data.dislocations->lines)
        for (size_t k = 0; k + 1 < line.points.size(); ++k) {
            std::vector<std::array<P3, 2>> work{{line.points[k], line.points[k + 1]}}, next;
            for (size_t axis = 0; axis < 3; ++axis) {
                if (!frame.periodic[axis]) continue;
                next.clear();
                for (auto segment : work) {
                    double s0 = frame.fractional(segment[0])[axis], s1 = frame.fractional(segment[1])[axis];
                    const double whole = std::floor(std::min(s0, s1));
                    for (P3 &p : segment) p = surface_detail::sub(p, surface_detail::mul(frame.axes[axis], whole));
                    s0 -= whole;
                    s1 -= whole;
                    const int pieces = std::clamp(int(std::ceil(std::max(s0, s1))), 1, 64);
                    if (pieces == 1) { next.push_back(segment); continue; }
                    for (int piece = 0; piece < pieces; ++piece) {
                        const double from = std::clamp((piece - s0) / (s1 - s0), 0.0, 1.0), to = std::clamp((piece + 1 - s0) / (s1 - s0), 0.0, 1.0);
                        if (from == to) continue;
                        const P3 d = surface_detail::sub(segment[1], segment[0]);
                        P3 a = surface_detail::add(segment[0], surface_detail::mul(d, from)), b = surface_detail::add(segment[0], surface_detail::mul(d, to));
                        a = surface_detail::sub(a, surface_detail::mul(frame.axes[axis], double(piece)));
                        b = surface_detail::sub(b, surface_detail::mul(frame.axes[axis], double(piece)));
                        next.push_back({a, b});
                    }
                }
                work.swap(next);
            }
            for (const auto &segment : work) {
                if (emitted++ >= limit) return false;
                emit(segment[0], segment[1], line);
            }
        }
    return true;
}
