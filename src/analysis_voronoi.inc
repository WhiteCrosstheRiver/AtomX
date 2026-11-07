// Voronoi analysis. Included inside namespace atomx by core.hpp, after
// analysis_trajectory.inc. Each cell is built by clipping a convex polyhedron with
// the bisector plane of every neighbor that can still reach it.

struct VoronoiOptions {
    double faceAreaThreshold = 0;     // Absolute, in squared length units.
    double relativeAreaThreshold = 0; // Fraction of the cell's total face area.
    double edgeLengthThreshold = 0;   // Shorter edges are not counted in a face's order.
    bool useRadii = false;            // Radical (Laguerre) tessellation from the Radius property.
    bool neighborBonds = false;
    bool onlySelected = false;
};

namespace voronoi_detail {
using Point = std::array<double, 3>;
inline Point sub(const Point &a, const Point &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline double dot(const Point &a, const Point &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline Point cross(const Point &a, const Point &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline double norm(const Point &a) { return std::sqrt(dot(a, a)); }

struct Face {
    int64_t neighbor;          // Index into the neighbor list, or negative for a wall.
    std::vector<Point> polygon; // Ordered vertices, relative to the cell center.
    Point normal;              // Outward unit normal.
};
struct Cell {
    std::vector<Face> faces;
    double radius() const {
        double r = 0;
        for (const auto &face : faces) for (const auto &p : face.polygon) r = std::max(r, norm(p));
        return r;
    }
};
// The cube [-half, half]^3 as six faces.
inline Cell box(double half) {
    Cell cell;
    const double h = half;
    const Point corner[8]{{-h, -h, -h}, {h, -h, -h}, {h, h, -h}, {-h, h, -h},
                          {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}};
    const int quads[6][4]{{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {2, 3, 7, 6}, {1, 2, 6, 5}, {0, 4, 7, 3}};
    const Point normals[6]{{0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {0, 1, 0}, {1, 0, 0}, {-1, 0, 0}};
    for (int f = 0; f < 6; ++f) {
        const auto &q = quads[f];
        cell.faces.push_back({-1, {corner[q[0]], corner[q[1]], corner[q[2]], corner[q[3]]}, normals[f]});
    }
    return cell;
}
// Keeps the part with normal.x <= offset. Returns false if nothing was cut away.
inline bool clip(Cell &cell, const Point &normal, double offset, int64_t id, double tolerance) {
    bool anyOutside = false;
    for (const auto &face : cell.faces)
        for (const auto &p : face.polygon)
            if (dot(normal, p) - offset > tolerance) { anyOutside = true; break; }
    if (!anyOutside) return false;
    std::vector<Face> kept;
    std::vector<Point> cap;
    for (auto &face : cell.faces) {
        std::vector<Point> out;
        const size_t n = face.polygon.size();
        for (size_t k = 0; k < n; ++k) {
            const Point &a = face.polygon[k], &b = face.polygon[(k + 1) % n];
            const double sa = dot(normal, a) - offset, sb = dot(normal, b) - offset;
            const bool ina = sa <= tolerance, inb = sb <= tolerance;
            if (ina) out.push_back(a);
            if (ina != inb) {
                const double t = sa / (sa - sb);
                const Point x{a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]), a[2] + t * (b[2] - a[2])};
                out.push_back(x);
                cap.push_back(x);
            } else if (ina && std::abs(sa) <= tolerance) {
                cap.push_back(a); // A vertex lying on the plane still bounds the new face.
            }
        }
        // Merge repeated points left by cuts through a vertex.
        std::vector<Point> clean;
        for (const auto &p : out)
            if (clean.empty() || norm(sub(p, clean.back())) > tolerance) clean.push_back(p);
        while (clean.size() > 1 && norm(sub(clean.front(), clean.back())) <= tolerance) clean.pop_back();
        if (clean.size() >= 3) kept.push_back({face.neighbor, std::move(clean), face.normal});
    }
    // The new face: the cut points, ordered by angle around their centroid.
    std::vector<Point> unique;
    for (const auto &p : cap) {
        bool seen = false;
        for (const auto &q : unique) if (norm(sub(p, q)) <= tolerance) { seen = true; break; }
        if (!seen) unique.push_back(p);
    }
    if (unique.size() >= 3) {
        Point center{};
        for (const auto &p : unique) for (int c = 0; c < 3; ++c) center[size_t(c)] += p[size_t(c)] / double(unique.size());
        Point u = sub(unique[0], center);
        u = {u[0] / norm(u), u[1] / norm(u), u[2] / norm(u)};
        const Point v = cross(normal, u);
        std::sort(unique.begin(), unique.end(), [&](const Point &a, const Point &b) {
            const Point da = sub(a, center), db = sub(b, center);
            return std::atan2(dot(da, v), dot(da, u)) < std::atan2(dot(db, v), dot(db, u));
        });
        kept.push_back({id, std::move(unique), normal});
    }
    cell.faces = std::move(kept);
    return true;
}
inline double polygonArea(const std::vector<Point> &polygon) {
    Point sum{};
    for (size_t k = 1; k + 1 < polygon.size(); ++k) {
        const Point c = cross(sub(polygon[k], polygon[0]), sub(polygon[k + 1], polygon[0]));
        for (int i = 0; i < 3; ++i) sum[size_t(i)] += c[size_t(i)];
    }
    return .5 * norm(sum);
}
} // namespace voronoi_detail

inline void computeVoronoi(PipelineResult &r, const VoronoiOptions &options, std::atomic<bool> *cancel) {
    using namespace voronoi_detail;
    Dataset &data = r.data;
    if (data.sampled()) throw std::runtime_error("Voronoi analysis requires complete, unsampled particle data");
    if (options.faceAreaThreshold < 0 || options.relativeAreaThreshold < 0 || options.relativeAreaThreshold >= 1 ||
        options.edgeLengthThreshold < 0)
        throw std::runtime_error("Voronoi thresholds must be non-negative, and the relative one below 1");
    const double volumeOfCell = std::abs(cellDeterminant(data.cell));
    if (!(volumeOfCell > 0)) throw std::runtime_error("Voronoi analysis requires a valid simulation cell");
    if (options.onlySelected && r.selected.size() != data.atoms.size())
        throw std::runtime_error("Selection length does not match particle count");
    // Unselected particles are ignored as if absent.
    Dataset subset;
    std::vector<uint32_t> original;
    const Dataset *input = &data;
    if (options.onlySelected) {
        subset.species = data.species; subset.cell = data.cell; subset.origin = data.origin; subset.pbc = data.pbc;
        for (size_t i = 0; i < data.atoms.size(); ++i)
            if (r.selected[i]) {
                subset.atoms.push_back(data.atoms[i]);
                original.push_back(uint32_t(i));
                for (const auto &[name, values] : data.scalarProperties)
                    if (name == "Radius") subset.scalarProperties[name].push_back(values[i]);
            }
        input = &subset;
    }
    const Dataset &source = *input;
    const size_t count = source.atoms.size();
    // A small periodic cell is analyzed inside a replica large enough for exact cells.
    // The original particles come first in the replica, so cells 0..count-1 are theirs.
    Dataset replica;
    const Dataset *working = &source;
    int copies[3]{1, 1, 1};
    const auto replicate = [&](const int factor[3]) {
        replica = Dataset{};
        replica.species = source.species;
        replica.origin = source.origin;
        replica.pbc = source.pbc;
        for (int axis = 0; axis < 3; ++axis)
            for (int xyz = 0; xyz < 3; ++xyz)
                replica.cell[size_t(axis) * 3 + size_t(xyz)] = source.cell[size_t(axis) * 3 + size_t(xyz)] * factor[axis];
        const auto radii = source.scalarProperties.find("Radius");
        for (int k = 0; k < factor[2]; ++k)
            for (int j = 0; j < factor[1]; ++j)
                for (int i = 0; i < factor[0]; ++i)
                    for (size_t p = 0; p < count; ++p) {
                        Atom atom = source.atoms[p];
                        atom.x = float(atom.x + i * source.cell[0] + j * source.cell[3] + k * source.cell[6]);
                        atom.y = float(atom.y + i * source.cell[1] + j * source.cell[4] + k * source.cell[7]);
                        atom.z = float(atom.z + i * source.cell[2] + j * source.cell[5] + k * source.cell[8]);
                        replica.atoms.push_back(atom);
                        if (radii != source.scalarProperties.end() && radii->second.size() == count)
                            replica.scalarProperties["Radius"].push_back(radii->second[p]);
                    }
        if (replica.atoms.size() > 2000000)
            throw std::runtime_error("The periodic cell is too small to resolve Voronoi cells within 2 million replicated particles");
        working = &replica;
    };
    std::vector<Cell> cells;
    std::vector<NeighborVectorLists::value_type> neighbors;
    std::vector<uint8_t> done;
    for (;;) {
    const Dataset &d = *working;
    const size_t workCount = d.atoms.size();
    std::vector<double> radius(workCount, 0.0);
    if (options.useRadii) {
        const auto found = d.scalarProperties.find("Radius");
        if (found == d.scalarProperties.end() || found->second.size() != workCount)
            throw std::runtime_error("Polydisperse Voronoi needs a Radius particle property");
        for (size_t i = 0; i < workCount; ++i) {
            radius[i] = found->second[i];
            if (!(radius[i] >= 0) || !std::isfinite(radius[i]))
                throw std::runtime_error("Particle radii must be finite and non-negative");
        }
    }
    double minRadius = 0, maxRadius = 0;
    if (count) {
        minRadius = *std::min_element(radius.begin(), radius.end());
        maxRadius = *std::max_element(radius.begin(), radius.end());
    }
    const double weightSpread = maxRadius * maxRadius - minRadius * minRadius;

    // Walls bound the cells along non-periodic directions; periodic ones need none.
    const auto inverse = cellInverse(d.cell);
    struct Wall { Point normal; double offset; };
    std::vector<Wall> walls;
    double extent = 0;
    for (int axis = 0; axis < 3; ++axis) {
        const Point a{d.cell[size_t(axis) * 3], d.cell[size_t(axis) * 3 + 1], d.cell[size_t(axis) * 3 + 2]};
        extent += norm(a);
        if (d.pbc[size_t(axis)]) continue;
        // The fractional coordinate along this axis is inverse[axis] . (x - origin).
        const Point g{inverse[size_t(axis)][0], inverse[size_t(axis)][1], inverse[size_t(axis)][2]};
        const double length = norm(g);
        walls.push_back({{g[0] / length, g[1] / length, g[2] / length}, 1.0 / length}); // Upper face, x . n <= o.
        walls.push_back({{-g[0] / length, -g[1] / length, -g[2] / length}, 0.0});       // Lower face.
    }
    double maxCutoff = std::numeric_limits<double>::infinity();
    for (int axis = 0; axis < 3; ++axis)
        if (d.pbc[size_t(axis)])
            maxCutoff = std::min(maxCutoff, .5 * std::hypot(d.cell[size_t(axis) * 3], d.cell[size_t(axis) * 3 + 1],
                                                            d.cell[size_t(axis) * 3 + 2]));
    if (!std::isfinite(maxCutoff)) maxCutoff = 2 * extent; // No periodic axis: the walls close every cell.

    cells.assign(count, Cell{});
    neighbors.assign(count, {});
    done.assign(count, 0);
    bool tooSmall = false;
    double cutoff = std::min(maxCutoff, 2.5 * std::cbrt(volumeOfCell / std::max<double>(1, double(count))));
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (cancel && *cancel) throw std::runtime_error("Cancelled");
        auto lists = workCount > 1 ? neighborVectorLists(d, cutoff, cancel) : NeighborVectorLists(workCount);
        // Coordinates are single precision, so planes through a cell corner (as in FCC)
        // land within about 1e-7 of the box size. Closer than this counts as touching.
        const double tolerance = 1e-6 * std::max(1.0, extent);
        const bool periodic = std::any_of(d.pbc.begin(), d.pbc.end(), [](bool p) { return p; });
        // Not "small": the Windows headers define that as a macro.
        std::atomic<bool> pending{false}, outgrown{false}, failed{false};
        std::string failure;
        std::mutex failureLock;
        std::atomic<size_t> next{0};
        // Cells are independent; the counter lets several workers share the loop.
        const auto work = [&] {
            try {
                for (size_t i = next++; i < count; i = next++) {
                    if (done[i]) continue;
                    if (failed || outgrown || (cancel && *cancel)) return;
                    auto &list = lists[i];
                    std::sort(list.begin(), list.end(), [](const NeighborVector &a, const NeighborVector &b) {
                        return a.distanceSquared < b.distanceSquared;
                    });
                    Cell cell = box(extent);
                    const Point position{double(d.atoms[i].x) - d.origin.x, double(d.atoms[i].y) - d.origin.y,
                                         double(d.atoms[i].z) - d.origin.z};
                    for (const auto &wall : walls) {
                        const double offset = wall.offset - dot(wall.normal, position);
                        if (offset < -tolerance)
                            throw std::runtime_error("A particle lies outside the simulation cell along a non-periodic direction");
                        clip(cell, wall.normal, offset, -2, tolerance);
                    }
                    double reach = cell.radius();
                    for (size_t k = 0; k < list.size(); ++k) {
                        const double distance = std::sqrt(list[k].distanceSquared);
                        if (!(distance > 0)) throw std::runtime_error("Two particles share a position; their Voronoi cells are undefined");
                        // No neighbor at least this far can cut a cell of the current radius.
                        if ((list[k].distanceSquared - weightSpread) / (2 * distance) > reach + tolerance) break;
                        const Point normal{list[k].vector[0] / distance, list[k].vector[1] / distance, list[k].vector[2] / distance};
                        const double offset = (list[k].distanceSquared + radius[i] * radius[i] -
                                               radius[list[k].index] * radius[list[k].index]) / (2 * distance);
                        if (clip(cell, normal, offset, int64_t(k), tolerance)) reach = cell.radius();
                    }
                    // Exact once no neighbor beyond the search radius could still reach the cell.
                    if ((cutoff * cutoff - weightSpread) / (2 * cutoff) >= reach) {
                        cells[i] = std::move(cell);
                        neighbors[i] = std::move(list);
                        done[i] = 1;
                    } else if (cutoff >= maxCutoff && periodic) {
                        outgrown = true; // Neighbors beyond half the cell could still cut it.
                    } else if (cutoff >= maxCutoff) {
                        throw std::runtime_error("Voronoi analysis could not close every cell");
                    } else {
                        pending = true;
                    }
                }
            } catch (const std::exception &e) {
                const std::lock_guard lock(failureLock);
                if (!failed.exchange(true)) failure = e.what();
            }
        };
        // One worker: measured on 4 cores, extra threads ran slower because clipping makes
        // many small allocations that contend on the heap. Revisit with pooled buffers.
        work();
        if (failed) throw std::runtime_error(failure);
        if (cancel && *cancel) throw std::runtime_error("Cancelled");
        tooSmall = outgrown;
        if (tooSmall || !pending) break;
        cutoff = std::min(maxCutoff, cutoff * 1.4);
    }
    if (tooSmall) {
        for (int axis = 0; axis < 3; ++axis) if (source.pbc[size_t(axis)]) copies[axis] *= 2;
        replicate(copies);
        continue;
    }
    if (std::find(done.begin(), done.end(), 0) != done.end())
        throw std::runtime_error("Voronoi analysis could not close every cell");
    break;
    }
    // Results refer to the original particles; replica neighbors map back by index.
    const Dataset &d = source;
    const auto inverse = cellInverse(source.cell);

    std::vector<double> volume(count), coordination(count), cavity(count), maxOrder(count);
    std::vector<std::array<uint32_t, 33>> indices(count);
    int highestOrder = 0;
    double totalVolume = 0;
    std::map<std::string, uint64_t> signatures;
    std::vector<Bond> bonds;
    for (size_t i = 0; i < count; ++i) {
        if ((i & 4095) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
        const Cell &cell = cells[i];
        double cellArea = 0, cellVolume = 0;
        std::vector<double> areas(cell.faces.size());
        for (size_t f = 0; f < cell.faces.size(); ++f) {
            const auto &polygon = cell.faces[f].polygon;
            areas[f] = polygonArea(polygon);
            cellArea += areas[f];
            // Divergence theorem with outward normals: exact even when a radical cell does
            // not contain its own particle.
            cellVolume += areas[f] * dot(cell.faces[f].normal, polygon[0]) / 3;
        }
        volume[i] = cellVolume;
        totalVolume += cellVolume;
        cavity[i] = cell.radius();
        int faces = 0, largest = 0;
        for (size_t f = 0; f < cell.faces.size(); ++f) {
            const auto &face = cell.faces[f];
            if (face.neighbor < 0) continue; // Walls are not neighbors.
            if (areas[f] <= options.faceAreaThreshold || areas[f] <= options.relativeAreaThreshold * cellArea) continue;
            int order = 0;
            for (size_t k = 0; k < face.polygon.size(); ++k)
                if (norm(sub(face.polygon[k], face.polygon[(k + 1) % face.polygon.size()])) > options.edgeLengthThreshold) ++order;
            if (order < 3) continue;
            ++faces;
            largest = std::max(largest, order);
            ++indices[i][size_t(std::min(order, 32))];
            if (options.neighborBonds) {
                const auto &n = neighbors[i][size_t(face.neighbor)];
                const uint32_t j = uint32_t(n.index % count);
                // The periodic image: vector minus the plain separation, in cell vectors.
                const Point plain{double(d.atoms[j].x) - d.atoms[i].x, double(d.atoms[j].y) - d.atoms[i].y,
                                  double(d.atoms[j].z) - d.atoms[i].z};
                const Point shift = sub({n.vector[0], n.vector[1], n.vector[2]}, plain);
                std::array<int32_t, 3> image{};
                for (int axis = 0; axis < 3; ++axis)
                    image[size_t(axis)] = int32_t(std::lround(dot({inverse[size_t(axis)][0], inverse[size_t(axis)][1],
                                                                  inverse[size_t(axis)][2]}, shift)));
                // Each face is seen from both sides; keep one of the two.
                const bool keep = i < j || (i == j && std::make_tuple(image[0], image[1], image[2]) > std::make_tuple(0, 0, 0));
                if (keep) {
                    Bond bond;
                    bond.a = options.onlySelected ? original[i] : uint32_t(i);
                    bond.b = options.onlySelected ? original[j] : j;
                    bond.image = image;
                    bonds.push_back(bond);
                }
            }
        }
        coordination[i] = faces;
        maxOrder[i] = largest;
        highestOrder = std::max(highestOrder, largest);
    }
    // Index components 3..highest order; 33 and above share the last slot.
    const int lastIndex = std::max(3, std::min(highestOrder, 32));
    for (size_t i = 0; i < count; ++i) {
        std::string signature = "<";
        for (int order = 3; order <= lastIndex; ++order)
            signature += std::to_string(indices[i][size_t(order)]) + (order < lastIndex ? "," : ">");
        ++signatures[signature];
    }
    auto expand = [&](std::vector<double> values) {
        if (!options.onlySelected) return values;
        std::vector<double> full(data.atoms.size(), 0.0);
        for (size_t k = 0; k < original.size(); ++k) full[original[k]] = values[k];
        return full;
    };
    for (int order = 3; order <= lastIndex; ++order) {
        std::vector<double> component(count);
        for (size_t i = 0; i < count; ++i) component[i] = indices[i][size_t(order)];
        data.scalarProperties["Voronoi Index." + std::to_string(order)] = expand(std::move(component));
    }
    data.scalarProperties["Atomic Volume"] = expand(std::move(volume));
    data.scalarProperties["Coordination"] = expand(std::move(coordination));
    data.scalarProperties["Cavity Radius"] = expand(std::move(cavity));
    data.scalarProperties["Max Face Order"] = expand(std::move(maxOrder));
    data.globalAttributes["Voronoi.max_face_order"] = double(highestOrder);
    data.globalAttributes["Voronoi.total_volume"] = totalVolume;
    if (options.neighborBonds) data.bonds.insert(data.bonds.end(), bonds.begin(), bonds.end());
    std::vector<std::pair<std::string, uint64_t>> ranked(signatures.begin(), signatures.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    DataTable table;
    table.name = "Voronoi index histogram";
    table.columns = {"Voronoi index (faces with 3, 4, ... edges)", "Count", "Fraction"};
    for (const auto &[signature, number] : ranked) {
        if (table.rows.size() == 200) break;
        table.rows.push_back({signature, std::to_string(number),
                              std::to_string(count ? double(number) / double(count) : 0)});
    }
    data.tables.push_back(std::move(table));
}
