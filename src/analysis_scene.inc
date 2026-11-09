// ---- Geometry drawn over the particles: trajectory lines and coordination polyhedra ----
inline constexpr size_t trajectorySegmentLimit = 2000000;
inline constexpr size_t polyhedronTriangleLimit = 1000000;
inline constexpr size_t polyhedronNeighborLimit = 128;

struct TrajectoryLineOptions {
    bool onlySelected = false;
    bool unwrap = true;
    bool customRange = false;
    int firstFrame = 0, lastFrame = 0;
    int every = 1;
    std::string sampledProperty;
};

// Follows the particles chosen in the first sampled frame. Unwrapping applies the minimum
// image convention between consecutive samples, so a particle must move less than half a
// cell between them. Positions are in the input length unit.
inline TrajectoryOutput sweepTrajectoryLines(const TrajectoryLineOptions &options, int frames, const FrameEvaluator &evaluate,
                                             const std::function<std::vector<uint8_t>(int)> &selectionAt, std::atomic<bool> *cancel) {
    if (options.every < 1 || options.every > 1000000) throw std::runtime_error("The sampling interval must be between 1 and 1,000,000 frames");
    const int first = options.customRange ? options.firstFrame : 0;
    const int last = options.customRange ? options.lastFrame : frames - 1;
    if (first < 0 || last >= frames || first > last)
        throw std::runtime_error("The frame range must lie within frames 0 to " + std::to_string(frames - 1));
    auto set = std::make_shared<TrajectoryLineSet>();
    for (int frame = first; frame <= last; frame += options.every) set->frames.push_back(frame);
    const size_t samples = set->frames.size();
    if (samples < 2) throw std::runtime_error("Generate trajectory lines needs at least two sampled frames");

    const Dataset reference = evaluate(first);
    if (reference.sampled()) throw std::runtime_error("Generate trajectory lines requires full data, not a sampled preview");
    std::vector<uint32_t> members;
    if (options.onlySelected) {
        const auto selected = selectionAt(first);
        if (selected.size() != reference.atoms.size()) throw std::runtime_error("Selection length does not match particle count");
        for (size_t i = 0; i < selected.size(); ++i)
            if (selected[i]) members.push_back(uint32_t(i));
        if (members.empty()) throw std::runtime_error("No particles are selected in the first sampled frame");
    } else {
        members.resize(reference.atoms.size());
        std::iota(members.begin(), members.end(), 0u);
    }
    if (members.empty()) throw std::runtime_error("Generate trajectory lines needs particles");
    if (members.size() > trajectorySegmentLimit / (samples - 1))
        throw std::runtime_error("Trajectory lines are limited to 2,000,000 segments; select fewer particles or sample fewer frames");

    set->lineCount = members.size();
    set->points.resize(members.size() * samples);
    if (!options.sampledProperty.empty()) set->values.resize(members.size() * samples);
    std::vector<std::array<double, 3>> previous(members.size()), unwrapped(members.size());
    double low = std::numeric_limits<double>::infinity(), high = -low;
    for (size_t sample = 0; sample < samples; ++sample) {
        if (cancel && *cancel) throw std::runtime_error("Cancelled");
        const Dataset current = sample == 0 ? Dataset{} : evaluate(set->frames[sample]);
        const Dataset &data = sample == 0 ? reference : current;
        if (data.sampled()) throw std::runtime_error("Generate trajectory lines requires full data, not a sampled preview");
        const auto mapping = matchReferenceParticles(reference, data, cancel);
        const bool periodic = options.unwrap && std::any_of(data.pbc.begin(), data.pbc.end(), [](bool p) { return p; });
        std::array<std::array<double, 3>, 3> inverse{};
        if (periodic) inverse = cellInverse(data.cell);
        std::optional<NumericParticlePropertyView> view;
        if (!options.sampledProperty.empty()) view = numericParticlePropertyView(data, options.sampledProperty);
        for (size_t line = 0; line < members.size(); ++line) {
            const size_t particle = mapping[members[line]];
            const Atom &atom = data.atoms[particle];
            const std::array<double, 3> raw{atom.x, atom.y, atom.z};
            if (sample == 0) unwrapped[line] = raw;
            else {
                std::array<double, 3> step{raw[0] - previous[line][0], raw[1] - previous[line][1], raw[2] - previous[line][2]};
                if (periodic)
                    for (size_t axis = 0; axis < 3; ++axis) {
                        if (!data.pbc[axis]) continue;
                        const double image = std::round(inverse[axis][0] * step[0] + inverse[axis][1] * step[1] + inverse[axis][2] * step[2]);
                        for (size_t xyz = 0; xyz < 3; ++xyz) step[xyz] -= image * data.cell[axis * 3 + xyz];
                    }
                for (size_t xyz = 0; xyz < 3; ++xyz) unwrapped[line][xyz] += step[xyz];
            }
            previous[line] = raw;
            const size_t slot = line * samples + sample;
            set->points[slot] = {float(unwrapped[line][0]), float(unwrapped[line][1]), float(unwrapped[line][2])};
            if (view) {
                const double value = view->value(data, particle);
                set->values[slot] = float(value);
                if (std::isfinite(value)) { low = std::min(low, value); high = std::max(high, value); }
            }
        }
    }
    if (low <= high) { set->valueMin = float(low); set->valueMax = float(high); }
    TrajectoryOutput output;
    output.attributes["TrajectoryLines.count"] = double(set->lineCount);
    output.attributes["TrajectoryLines.points"] = double(set->points.size());
    output.lines = std::move(set);
    return output;
}

// Calls emit(a, b, u) for every visible segment, with u in 0..1 from the sampled property or
// -1 without one. With wrapping on, a segment is cut where it crosses a periodic boundary and
// each piece is moved into the primary cell. Stops after limit segments and returns false.
template <class Emit>
inline bool forEachTrajectorySegment(const Dataset &data, size_t limit, Emit &&emit) {
    using Point = std::array<double, 3>;
    const auto &style = data.trajectoryLines;
    if (!style.set) return true;
    const TrajectoryLineSet &set = *style.set;
    const size_t samples = set.frames.size();
    size_t visible = samples;
    if (style.upToCurrent) {
        visible = 0;
        while (visible < samples && set.frames[visible] <= style.currentFrame) ++visible;
    }
    std::optional<std::array<std::array<double, 3>, 3>> inverse;
    if (style.wrap && std::any_of(data.pbc.begin(), data.pbc.end(), [](bool p) { return p; }))
        try { inverse = cellInverse(data.cell); } catch (const std::exception &) {}
    const Point origin{data.origin.x, data.origin.y, data.origin.z};
    const auto fraction = [&](const Point &p, size_t axis) {
        const auto &row = (*inverse)[axis];
        return row[0] * (p[0] - origin[0]) + row[1] * (p[1] - origin[1]) + row[2] * (p[2] - origin[2]);
    };
    const float span = set.valueMax - set.valueMin;
    size_t emitted = 0;
    for (size_t line = 0; line < set.lineCount; ++line)
        for (size_t k = 1; k < visible; ++k) {
            const Vec3 &from = set.points[line * samples + k - 1], &to = set.points[line * samples + k];
            const Point a{from.x, from.y, from.z}, b{to.x, to.y, to.z};
            float u = -1;
            if (!set.values.empty()) {
                const float mean = .5f * (set.values[line * samples + k - 1] + set.values[line * samples + k]);
                u = !std::isfinite(mean) ? 0.f : span > 0 ? std::clamp((mean - set.valueMin) / span, 0.f, 1.f) : .5f;
            }
            const auto at = [&](double t) { return Point{a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]), a[2] + t * (b[2] - a[2])}; };
            std::vector<double> cuts{0.0};
            if (inverse)
                for (size_t axis = 0; axis < 3; ++axis) {
                    if (!data.pbc[axis]) continue;
                    const double fa = fraction(a, axis), fb = fraction(b, axis);
                    if (fa == fb) continue;
                    const double lower = std::min(fa, fb), upper = std::max(fa, fb);
                    // A very long segment is left with its first boundaries only.
                    for (double m = std::floor(lower) + 1; m < upper && cuts.size() < 64; m += 1) cuts.push_back((m - fa) / (fb - fa));
                }
            cuts.push_back(1.0);
            std::sort(cuts.begin(), cuts.end());
            for (size_t piece = 0; piece + 1 < cuts.size(); ++piece) {
                if (!(cuts[piece + 1] > cuts[piece])) continue;
                Point p = at(cuts[piece]), q = at(cuts[piece + 1]);
                if (inverse) {
                    const Point middle = at(.5 * (cuts[piece] + cuts[piece + 1]));
                    for (size_t axis = 0; axis < 3; ++axis) {
                        if (!data.pbc[axis]) continue;
                        const double shift = std::floor(fraction(middle, axis));
                        for (size_t xyz = 0; xyz < 3; ++xyz) {
                            p[xyz] -= shift * data.cell[axis * 3 + xyz];
                            q[xyz] -= shift * data.cell[axis * 3 + xyz];
                        }
                    }
                }
                if (emitted++ >= limit) return false;
                emit(p, q, u);
            }
        }
    return true;
}

namespace scene_detail {
using Point = std::array<double, 3>;
inline Point subtract(const Point &a, const Point &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline Point cross(const Point &a, const Point &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline double dot(const Point &a, const Point &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline double length(const Point &a) { return std::sqrt(dot(a, a)); }

struct HullFace {
    std::array<uint32_t, 3> v;
    Point normal;
    double offset;
};

// Incremental convex hull. Triangles are wound counter-clockwise seen from outside. The
// result is empty when the points do not span a volume larger than the tolerance allows.
inline std::vector<std::array<uint32_t, 3>> convexHull(const std::vector<Point> &points, double tolerance) {
    if (points.size() < 4) return {};
    size_t i1 = 0;
    double best = -1;
    for (size_t i = 1; i < points.size(); ++i) {
        const double d = length(subtract(points[i], points[0]));
        if (d > best) { best = d; i1 = i; }
    }
    if (!(best > tolerance)) return {};
    const Point axis = subtract(points[i1], points[0]);
    size_t i2 = 0;
    best = -1;
    for (size_t i = 1; i < points.size(); ++i) {
        const double d = length(cross(axis, subtract(points[i], points[0]))) / length(axis);
        if (d > best) { best = d; i2 = i; }
    }
    if (!(best > tolerance)) return {};
    const Point planeNormal = cross(axis, subtract(points[i2], points[0]));
    const double planeLength = length(planeNormal);
    size_t i3 = 0;
    best = -1;
    for (size_t i = 1; i < points.size(); ++i) {
        const double d = std::abs(dot(planeNormal, subtract(points[i], points[0]))) / planeLength;
        if (d > best) { best = d; i3 = i; }
    }
    if (!(best > tolerance)) return {};

    const std::array<uint32_t, 4> seed{0u, uint32_t(i1), uint32_t(i2), uint32_t(i3)};
    Point interior{};
    for (const uint32_t s : seed)
        for (size_t c = 0; c < 3; ++c) interior[c] += points[s][c] / 4;
    const auto makeFace = [&](uint32_t a, uint32_t b, uint32_t c) {
        Point normal = cross(subtract(points[b], points[a]), subtract(points[c], points[a]));
        const double size = length(normal);
        if (size > 0) for (double &value : normal) value /= size;
        HullFace face{{a, b, c}, normal, dot(normal, points[a])};
        if (dot(normal, interior) - face.offset > 0) {
            std::swap(face.v[1], face.v[2]);
            for (double &value : face.normal) value = -value;
            face.offset = -face.offset;
        }
        return face;
    };
    std::vector<HullFace> faces{makeFace(seed[0], seed[1], seed[2]), makeFace(seed[0], seed[1], seed[3]),
                                makeFace(seed[0], seed[2], seed[3]), makeFace(seed[1], seed[2], seed[3])};
    for (uint32_t p = 0; p < points.size(); ++p) {
        if (p == seed[0] || p == seed[1] || p == seed[2] || p == seed[3]) continue;
        std::vector<HullFace> kept;
        std::set<std::pair<uint32_t, uint32_t>> visibleEdges;
        for (const HullFace &face : faces) {
            if (dot(face.normal, points[p]) - face.offset > tolerance) {
                visibleEdges.insert({face.v[0], face.v[1]});
                visibleEdges.insert({face.v[1], face.v[2]});
                visibleEdges.insert({face.v[2], face.v[0]});
            } else kept.push_back(face);
        }
        if (visibleEdges.empty()) continue;
        for (const auto &[a, b] : visibleEdges)
            if (!visibleEdges.contains({b, a})) kept.push_back(makeFace(a, b, p));
        faces = std::move(kept);
    }
    std::vector<std::array<uint32_t, 3>> triangles;
    triangles.reserve(faces.size());
    for (const HullFace &face : faces)
        if (dot(face.normal, face.normal) > .5) triangles.push_back(face.v);
    return triangles;
}
} // namespace scene_detail

// Position of a polyhedron corner, seen from its central particle across periodic images.
inline bool polyhedronCorner(const Dataset &data, const Bond &corner, std::array<double, 3> &position) {
    try {
        const auto vector = bondVector(data, corner);
        const Atom &center = data.atoms[corner.a];
        position = {center.x + vector[0], center.y + vector[1], center.z + vector[2]};
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

// The convex hull of the bonded neighbors of every selected particle. Particles with fewer
// than four non-coplanar neighbors have no polyhedron and are counted as skipped.
inline void computeCoordinationPolyhedra(Dataset &data, const std::vector<uint8_t> &selected, float transparency, bool showEdges,
                                         const std::array<float, 4> &edgeColor, std::atomic<bool> *cancel) {
    using namespace scene_detail;
    if (!(transparency >= 0 && transparency <= 1)) throw std::runtime_error("Transparency must be between 0 and 1");
    if (data.sampled()) throw std::runtime_error("Coordination polyhedra requires full data, not a sampled preview");
    if (data.bonds.empty()) throw std::runtime_error("Coordination polyhedra needs bonds; add Create bonds first");
    if (selected.size() != data.atoms.size()) throw std::runtime_error("Selection length does not match particle count");
    std::vector<std::vector<Bond>> around(data.atoms.size());
    for (const Bond &bond : data.bonds) {
        if (bond.a >= data.atoms.size() || bond.b >= data.atoms.size()) throw std::runtime_error("Bond references a particle outside the dataset");
        if (selected[bond.a]) around[bond.a].push_back({bond.a, bond.b, bond.image, 1});
        if (selected[bond.b]) around[bond.b].push_back({bond.b, bond.a, {-bond.image[0], -bond.image[1], -bond.image[2]}, 1});
    }
    PolyhedronMesh mesh;
    mesh.alpha = 1.f - transparency;
    mesh.showEdges = showEdges;
    mesh.edgeColor = {edgeColor[0], edgeColor[1], edgeColor[2]};
    uint64_t built = 0, skipped = 0;
    for (size_t center = 0; center < around.size(); ++center) {
        if ((center & 1023) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
        if (!selected[center]) continue;
        auto &neighbors = around[center];
        std::sort(neighbors.begin(), neighbors.end(), [](const Bond &x, const Bond &y) { return std::tie(x.b, x.image) < std::tie(y.b, y.image); });
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
        if (neighbors.size() > polyhedronNeighborLimit)
            throw std::runtime_error("A particle has more than 128 bonded neighbors; reduce the bond cutoff");
        std::vector<Point> points(neighbors.size());
        Point middle{};
        double largest = 0;
        for (size_t i = 0; i < neighbors.size(); ++i) {
            if (!polyhedronCorner(data, neighbors[i], points[i])) throw std::runtime_error("A bond has an invalid periodic image");
            for (size_t c = 0; c < 3; ++c) {
                middle[c] += points[i][c] / double(neighbors.size());
                largest = std::max(largest, std::abs(points[i][c]));
            }
        }
        double radius = 0;
        for (const Point &p : points) radius = std::max(radius, length(subtract(p, middle)));
        // Coordinates are single precision, so the tolerance grows with their magnitude.
        const auto triangles = convexHull(points, std::max(1e-6 * radius, 5e-7 * largest));
        if (triangles.empty()) { ++skipped; continue; }
        ++built;
        std::map<std::pair<uint32_t, uint32_t>, std::vector<Point>> edgeNormals;
        for (const auto &triangle : triangles) {
            Point normal = cross(subtract(points[triangle[1]], points[triangle[0]]), subtract(points[triangle[2]], points[triangle[0]]));
            const double size = length(normal);
            if (size > 0) for (double &value : normal) value /= size;
            for (size_t k = 0; k < 3; ++k) {
                mesh.corners.push_back(neighbors[triangle[k]]);
                const uint32_t a = triangle[k], b = triangle[(k + 1) % 3];
                edgeNormals[{std::min(a, b), std::max(a, b)}].push_back(normal);
            }
        }
        if (mesh.corners.size() / 3 > polyhedronTriangleLimit)
            throw std::runtime_error("Coordination polyhedra are limited to 1,000,000 triangles; select fewer particles");
        // Diagonals of flat faces such as those of a cube are not drawn as edges.
        for (const auto &[edge, normals] : edgeNormals) {
            if (normals.size() == 2 && dot(normals[0], normals[1]) > 1 - 1e-4) continue;
            mesh.edges.push_back(neighbors[edge.first]);
            mesh.edges.push_back(neighbors[edge.second]);
        }
    }
    data.polyhedra = std::move(mesh);
    data.globalAttributes["CoordinationPolyhedra.count"] = double(built);
    data.globalAttributes["CoordinationPolyhedra.skipped"] = double(skipped);
    data.globalAttributes["CoordinationPolyhedra.triangles"] = double(data.polyhedra.corners.size() / 3);
}
