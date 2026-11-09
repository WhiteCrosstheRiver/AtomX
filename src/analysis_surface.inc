// Text labels, isosurfaces of voxel grids and surface meshes of particle sets.
//
// Surface meshes follow the methods OVITO documents (alpha shapes after Edelsbrunner and
// Muecke with Stukowski's use for atomistic data, Gaussian density after Krone et al.), but
// every routine here is AtomX's own. Numerical constants that OVITO does not publish are
// AtomX choices and are named where they are defined. Lengths are in the input length unit.

inline constexpr size_t surfaceTriangleLimit = 2000000;
inline constexpr size_t surfaceDelaunayPointLimit = 1000000;
inline constexpr size_t surfaceVoxelLimit = 32000000;
inline constexpr size_t surfaceDensityEntryLimit = 100000000;
inline constexpr size_t textLabelLimit = 500000;

namespace surface_detail {
using P3 = std::array<double, 3>;
inline P3 add(const P3 &a, const P3 &b) { return {a[0]+b[0], a[1]+b[1], a[2]+b[2]}; }
inline P3 sub(const P3 &a, const P3 &b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
inline P3 mul(const P3 &a, double s) { return {a[0]*s, a[1]*s, a[2]*s}; }
inline double dot(const P3 &a, const P3 &b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
inline P3 cross(const P3 &a, const P3 &b) {
    return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
}
inline void throwIfCancelled(std::atomic<bool> *cancel) {
    if (cancel && *cancel) throw std::runtime_error("Cancelled");
}

// The simulation cell, or the Cartesian axes without periodicity when there is no valid cell.
struct CellFrame {
    P3 origin{};
    std::array<P3, 3> axes{P3{1, 0, 0}, P3{0, 1, 0}, P3{0, 0, 1}};
    std::array<std::array<double, 3>, 3> inverse{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    std::array<bool, 3> periodic{};
    bool hasCell = false;
    P3 fractional(const P3 &p) const {
        const P3 d = sub(p, origin);
        return {dot(inverse[0], d), dot(inverse[1], d), dot(inverse[2], d)};
    }
    // Distance between the two faces of the cell normal to this axis.
    double width(int axis) const {
        const P3 n = cross(axes[size_t((axis+1)%3)], axes[size_t((axis+2)%3)]);
        return std::abs(dot(axes[size_t(axis)], n)) / std::sqrt(dot(n, n));
    }
    double volume() const { return hasCell ? std::abs(dot(axes[0], cross(axes[1], axes[2]))) : 0; }
    bool fullyPeriodic() const { return periodic[0] && periodic[1] && periodic[2]; }
};
inline CellFrame frameOf(const Dataset &data) {
    CellFrame frame;
    if (std::abs(cellDeterminant(data.cell)) <= 1e-9) return frame;
    frame.hasCell = true;
    frame.origin = {data.origin.x, data.origin.y, data.origin.z};
    for (size_t axis = 0; axis < 3; ++axis) {
        frame.axes[axis] = {data.cell[axis*3], data.cell[axis*3+1], data.cell[axis*3+2]};
        frame.periodic[axis] = data.pbc[axis];
    }
    frame.inverse = cellInverse(data.cell);
    return frame;
}

// Triangles as three corners each. A corner names a shared vertex and carries the position of
// that vertex in the triangle's own periodic image, so a triangle across a boundary stays whole.
struct Corner { uint32_t vertex; P3 position; };
struct Soup { std::vector<Corner> corners; size_t vertexCount = 0; };

// Taubin fairing: a shrinking step and an inflating step per iteration. lambda 0.5 and the
// pass-band 0.1 are AtomX values within the range Taubin's paper discusses.
inline void smoothSoup(Soup &soup, int iterations, std::atomic<bool> *cancel) {
    if (iterations <= 0 || soup.corners.empty()) return;
    const double lambda = .5, mu = 1 / (.1 - 1 / lambda);
    std::vector<P3> shift(soup.vertexCount), sum(soup.vertexCount);
    std::vector<uint32_t> degree(soup.vertexCount);
    for (int iteration = 0; iteration < iterations; ++iteration)
        for (const double factor : {lambda, mu}) {
            throwIfCancelled(cancel);
            std::fill(sum.begin(), sum.end(), P3{});
            std::fill(degree.begin(), degree.end(), 0u);
            for (size_t k = 0; k + 2 < soup.corners.size(); k += 3)
                for (size_t e = 0; e < 3; ++e) {
                    const Corner &a = soup.corners[k+e], &b = soup.corners[k+(e+1)%3];
                    const P3 delta = add(sub(b.position, a.position), sub(shift[b.vertex], shift[a.vertex]));
                    sum[a.vertex] = add(sum[a.vertex], delta);
                    sum[b.vertex] = sub(sum[b.vertex], delta);
                    ++degree[a.vertex];
                    ++degree[b.vertex];
                }
            for (size_t v = 0; v < soup.vertexCount; ++v)
                if (degree[v]) shift[v] = add(shift[v], mul(sum[v], factor / double(degree[v])));
        }
    for (Corner &corner : soup.corners) corner.position = add(corner.position, shift[corner.vertex]);
}

// Computes the area, then cuts triangles at the periodic cell faces and folds the pieces back
// into the cell, so the mesh ends at the cell boundary as OVITO's does.
inline std::shared_ptr<const SurfaceMeshData> finishMesh(const Soup &soup, const CellFrame &frame, double &area,
                                                         std::atomic<bool> *cancel) {
    struct V { P3 position, normal; };
    std::vector<P3> vertexNormal(soup.vertexCount);
    area = 0;
    for (size_t k = 0; k + 2 < soup.corners.size(); k += 3) {
        const P3 n = cross(sub(soup.corners[k+1].position, soup.corners[k].position),
                           sub(soup.corners[k+2].position, soup.corners[k].position));
        area += .5 * std::sqrt(dot(n, n));
        for (size_t c = 0; c < 3; ++c) vertexNormal[soup.corners[k+c].vertex] = add(vertexNormal[soup.corners[k+c].vertex], n);
    }
    auto mesh = std::make_shared<SurfaceMeshData>();
    mesh->positions.reserve(soup.corners.size());
    mesh->normals.reserve(soup.corners.size());
    const auto toFloat = [](const P3 &p) { return std::array<float, 3>{float(p[0]), float(p[1]), float(p[2])}; };
    const auto emit = [&](const V &v) {
        const double length = std::sqrt(dot(v.normal, v.normal));
        mesh->positions.push_back(toFloat(v.position));
        mesh->normals.push_back(toFloat(length > 0 ? mul(v.normal, 1 / length) : P3{0, 0, 1}));
    };
    std::vector<std::vector<V>> work, next;
    const auto clip = [&](const std::vector<V> &polygon, int axis, double limit, bool keepBelow) {
        std::vector<V> out;
        const auto side = [&](const V &v) {
            const double s = frame.fractional(v.position)[size_t(axis)] - limit;
            return keepBelow ? -s : s;
        };
        for (size_t i = 0; i < polygon.size(); ++i) {
            const V &a = polygon[i], &b = polygon[(i+1)%polygon.size()];
            const double sa = side(a), sb = side(b);
            if (sa >= 0) out.push_back(a);
            if ((sa >= 0) != (sb >= 0)) {
                const double t = sa / (sa - sb);
                out.push_back({add(a.position, mul(sub(b.position, a.position), t)),
                               add(a.normal, mul(sub(b.normal, a.normal), t))});
            }
        }
        return out;
    };
    for (size_t k = 0; k + 2 < soup.corners.size(); k += 3) {
        if ((k & 65535) == 0) throwIfCancelled(cancel);
        work.clear();
        std::vector<V> triangle;
        for (size_t c = 0; c < 3; ++c) {
            const P3 normal = vertexNormal[soup.corners[k+c].vertex];
            triangle.push_back({soup.corners[k+c].position, normal});
        }
        work.push_back(std::move(triangle));
        for (int axis = 0; axis < 3; ++axis) {
            if (!frame.periodic[size_t(axis)]) continue;
            next.clear();
            for (auto &polygon : work) {
                double low = std::numeric_limits<double>::infinity(), high = -low;
                for (const V &v : polygon) {
                    const double s = frame.fractional(v.position)[size_t(axis)];
                    low = std::min(low, s);
                    high = std::max(high, s);
                }
                const double whole = std::floor(low);
                for (V &v : polygon) v.position = sub(v.position, mul(frame.axes[size_t(axis)], whole));
                const int pieces = std::clamp(int(std::ceil(high - whole)), 1, 64);
                if (pieces == 1) { next.push_back(std::move(polygon)); continue; }
                for (int piece = 0; piece < pieces; ++piece) {
                    auto cut = clip(clip(polygon, axis, double(piece), false), axis, double(piece + 1), true);
                    if (cut.size() < 3) continue;
                    for (V &v : cut) v.position = sub(v.position, mul(frame.axes[size_t(axis)], double(piece)));
                    next.push_back(std::move(cut));
                }
            }
            work.swap(next);
        }
        for (const auto &polygon : work)
            for (size_t i = 1; i + 1 < polygon.size(); ++i) {
                if (mesh->positions.size() / 3 >= surfaceTriangleLimit)
                    throw std::runtime_error("The surface has more than 2,000,000 triangles; use a coarser setting");
                emit(polygon[0]);
                emit(polygon[i]);
                emit(polygon[i+1]);
            }
    }
    return mesh;
}

inline double determinant(const P3 &a, const P3 &b, const P3 &c) { return dot(a, cross(b, c)); }

// Isosurface of a scalar field on a regular grid, by marching tetrahedra on the Kuhn split of
// each grid cube. The split is the same in every cube, so faces match without ambiguous cases.
// Adds the volume where the field is at or above the level to filledVolume. values holds
// n[0]*n[1]*n[2] grid points, x fastest. A periodic axis wraps its last layer to the first.
template <class T>
void marchField(const std::array<int, 3> &n, const P3 &first, const std::array<P3, 3> &step,
                const std::array<bool, 3> &periodic, const T *values, double level, Soup &soup,
                double &filledVolume, std::atomic<bool> *cancel) {
    static constexpr int kuhn[6][4] = {{0,1,3,7},{0,1,5,7},{0,2,3,7},{0,2,6,7},{0,4,5,7},{0,4,6,7}};
    const size_t nx = size_t(n[0]), ny = size_t(n[1]), nz = size_t(n[2]);
    const std::array<size_t, 3> cells{periodic[0] ? nx : nx - 1, periodic[1] ? ny : ny - 1, periodic[2] ? nz : nz - 1};
    const double tetVolume = std::abs(determinant(step[0], step[1], step[2])) / 6;
    std::unordered_map<uint64_t, uint32_t> vertices;
    size_t index[8];
    double value[8];
    P3 position[8];
    for (size_t z = 0; z < cells[2]; ++z) {
        throwIfCancelled(cancel);
        for (size_t y = 0; y < cells[1]; ++y)
            for (size_t x = 0; x < cells[0]; ++x) {
                int above = 0;
                for (int c = 0; c < 8; ++c) {
                    index[c] = ((x + size_t(c & 1)) % nx) + nx * (((y + size_t((c >> 1) & 1)) % ny) +
                               ny * ((z + size_t((c >> 2) & 1)) % nz));
                    value[c] = double(values[index[c]]);
                    above += value[c] >= level;
                }
                if (above == 0) continue;
                if (above == 8) { filledVolume += 6 * tetVolume; continue; }
                const P3 base = add(add(add(first, mul(step[0], double(x))), mul(step[1], double(y))), mul(step[2], double(z)));
                for (int c = 0; c < 8; ++c)
                    position[c] = add(add(add(base, mul(step[0], double(c & 1))), mul(step[1], double((c >> 1) & 1))),
                                      mul(step[2], double((c >> 2) & 1)));
                for (const auto &tet : kuhn) {
                    int inside[4], outside[4], ni = 0, no = 0;
                    for (const int c : tet) (value[c] >= level ? inside[ni++] : outside[no++]) = c;
                    if (ni == 0) continue;
                    if (ni == 4) { filledVolume += tetVolume; continue; }
                    const auto fraction = [&](int in, int out) { return (level - value[in]) / (value[out] - value[in]); };
                    // Corners of one Kuhn tetrahedron are ordered by bit inclusion, so the lower
                    // corner and the differing bits name a grid edge in every periodic image.
                    const auto crossing = [&](int in, int out) {
                        const int low = (in & out) == in ? in : out, high = low == in ? out : in;
                        const uint64_t key = uint64_t(index[low]) * 8 + uint64_t(low ^ high);
                        const auto [entry, created] = vertices.try_emplace(key, uint32_t(vertices.size()));
                        (void)created;
                        return Corner{entry->second, add(position[in], mul(sub(position[out], position[in]), fraction(in, out)))};
                    };
                    const auto addTriangle = [&](Corner a, Corner b, Corner c, const P3 &outward) {
                        if (soup.corners.size() / 3 >= surfaceTriangleLimit)
                            throw std::runtime_error("The surface has more than 2,000,000 triangles; use a coarser setting");
                        if (dot(cross(sub(b.position, a.position), sub(c.position, a.position)), outward) < 0) std::swap(b, c);
                        soup.corners.push_back(a);
                        soup.corners.push_back(b);
                        soup.corners.push_back(c);
                    };
                    if (ni == 1) {
                        const int i = inside[0];
                        const double a = fraction(i, outside[0]), b = fraction(i, outside[1]), c = fraction(i, outside[2]);
                        filledVolume += tetVolume * a * b * c;
                        addTriangle(crossing(i, outside[0]), crossing(i, outside[1]), crossing(i, outside[2]),
                                    sub(position[outside[0]], position[i]));
                    } else if (ni == 3) {
                        const int o = outside[0];
                        const double a = 1 - fraction(inside[0], o), b = 1 - fraction(inside[1], o), c = 1 - fraction(inside[2], o);
                        filledVolume += tetVolume * (1 - a * b * c);
                        addTriangle(crossing(inside[0], o), crossing(inside[1], o), crossing(inside[2], o),
                                    sub(position[o], position[inside[0]]));
                    } else {
                        const int a = inside[0], b = inside[1], c = outside[0], d = outside[1];
                        // Fractions in the frame a=0, b=e1, c=e2, d=e3 of the unit tetrahedron.
                        const double p = fraction(a, c), q = fraction(a, d), r = fraction(b, c), s = fraction(b, d);
                        const P3 P{0, p, 0}, Q{0, 0, q}, R{1 - r, r, 0}, S{1 - s, 0, s}, B{1, 0, 0};
                        filledVolume += tetVolume * (std::abs(determinant(P, Q, S)) + std::abs(determinant(P, S, R)) +
                                                     std::abs(determinant(R, S, B)));
                        const P3 outward = sub(add(position[c], position[d]), add(position[a], position[b]));
                        const Corner ac = crossing(a, c), ad = crossing(a, d), bc = crossing(b, c), bd = crossing(b, d);
                        addTriangle(ac, ad, bd, outward);
                        addTriangle(ac, bd, bc, outward);
                    }
                }
            }
    }
    soup.vertexCount = vertices.size();
}

// Incremental Delaunay tetrahedralization (Bowyer-Watson). Tetrahedra are kept positively
// oriented; a cavity whose boundary would give an inverted tetrahedron is grown until valid.
class Tessellation {
public:
    struct Tet {
        std::array<int, 4> v{}, n{};
        P3 center{};
        double radius2 = 0;
        bool alive = false;
    };
    std::vector<Tet> tets;
    std::vector<P3> points;
    size_t realCount = 0, skipped = 0;

    bool touchesSuper(const Tet &tet) const {
        for (const int v : tet.v) if (size_t(v) >= realCount) return true;
        return false;
    }
    double volume(const std::array<int, 4> &v) const {
        return determinant(sub(points[size_t(v[1])], points[size_t(v[0])]), sub(points[size_t(v[2])], points[size_t(v[0])]),
                           sub(points[size_t(v[3])], points[size_t(v[0])])) / 6;
    }
    void build(std::vector<P3> input, std::atomic<bool> *cancel) {
        realCount = input.size();
        points = std::move(input);
        if (!realCount) return;
        P3 low = points[0], high = points[0];
        for (const P3 &p : points)
            for (size_t a = 0; a < 3; ++a) { low[a] = std::min(low[a], p[a]); high[a] = std::max(high[a], p[a]); }
        const P3 middle = mul(add(low, high), .5);
        const double extent = std::max({high[0]-low[0], high[1]-low[1], high[2]-low[2], 1.0});
        const double distance = 30 * extent; // Only tetrahedra with a super vertex depend on this.
        for (const P3 &corner : {P3{1, 1, 1}, P3{1, -1, -1}, P3{-1, 1, -1}, P3{-1, -1, 1}})
            points.push_back(add(middle, mul(corner, distance)));
        std::array<int, 4> root{int(realCount), int(realCount)+1, int(realCount)+2, int(realCount)+3};
        if (volume(root) < 0) std::swap(root[2], root[3]);
        last = allocate(root);
        tets[size_t(last)].n.fill(-1);
        // Insert along a Morton curve so that each walk starts next to its target.
        std::vector<std::pair<uint64_t, uint32_t>> order(realCount);
        const auto spread = [](uint64_t x) {
            x &= 0x1fffff;
            x = (x | x << 32) & 0x1f00000000ffff;
            x = (x | x << 16) & 0x1f0000ff0000ff;
            x = (x | x << 8) & 0x100f00f00f00f00f;
            x = (x | x << 4) & 0x10c30c30c30c30c3;
            return (x | x << 2) & 0x1249249249249249;
        };
        for (size_t i = 0; i < realCount; ++i) {
            uint64_t key = 0;
            for (size_t a = 0; a < 3; ++a) {
                const double s = (points[i][a] - low[a]) / extent * 2097151.0;
                key |= spread(uint64_t(std::clamp(s, 0.0, 2097151.0))) << a;
            }
            order[i] = {key, uint32_t(i)};
        }
        std::sort(order.begin(), order.end());
        for (size_t i = 0; i < realCount; ++i) {
            if ((i & 4095) == 0) throwIfCancelled(cancel);
            insert(int(order[i].second));
        }
    }
    int allocate(const std::array<int, 4> &v) {
        int id;
        if (!freeSlots.empty()) { id = freeSlots.back(); freeSlots.pop_back(); }
        else { id = int(tets.size()); tets.emplace_back(); stamps.push_back(0); }
        Tet &tet = tets[size_t(id)];
        tet.v = v;
        tet.alive = true;
        const P3 a = points[size_t(v[0])], b = sub(points[size_t(v[1])], a), c = sub(points[size_t(v[2])], a), d = sub(points[size_t(v[3])], a);
        const double D = 2 * determinant(b, c, d);
        if (D > 0) {
            const P3 offset = mul(add(add(mul(cross(c, d), dot(b, b)), mul(cross(d, b), dot(c, c))), mul(cross(b, c), dot(d, d))), 1 / D);
            tet.center = add(a, offset);
            tet.radius2 = dot(offset, offset);
        } else {
            tet.center = a;
            tet.radius2 = std::numeric_limits<double>::infinity();
        }
        return id;
    }
private:
    std::vector<int> freeSlots, cavity;
    std::vector<uint32_t> stamps;
    uint32_t generation = 0, walkCounter = 0;
    int last = 0;
    struct Facet { int tet, face; };
    struct Pending { uint64_t key; int tet, face; };
    std::vector<Facet> boundary;
    std::vector<Pending> pending;

    bool inSphere(int id, const P3 &q) const {
        const Tet &tet = tets[size_t(id)];
        const P3 d = sub(q, tet.center);
        return dot(d, d) < tet.radius2;
    }
    int locate(int q) {
        int current = last;
        for (int step = 0; step < 200000; ++step) {
            const Tet &tet = tets[size_t(current)];
            const int start = int(walkCounter++ & 3);
            bool moved = false;
            for (int j = 0; j < 4 && !moved; ++j) {
                const int face = (start + j) & 3, next = tet.n[size_t(face)];
                if (next < 0) continue;
                auto replaced = tet.v;
                replaced[size_t(face)] = q;
                if (volume(replaced) < 0) { current = next; moved = true; }
            }
            if (!moved) break;
        }
        return current;
    }
    void insert(int q) {
        const P3 point = points[size_t(q)];
        int start = locate(q);
        if (!inSphere(start, point)) {
            // The walk can only end here through rounding; a full scan settles it.
            start = -1;
            for (size_t id = 0; id < tets.size() && start < 0; ++id)
                if (tets[id].alive && inSphere(int(id), point)) start = int(id);
            if (start < 0) { ++skipped; return; }
        }
        ++generation;
        cavity.assign(1, start);
        stamps[size_t(start)] = generation;
        for (size_t head = 0; head < cavity.size(); ++head) {
            const auto neighbors = tets[size_t(cavity[head])].n;
            for (const int next : neighbors)
                if (next >= 0 && stamps[size_t(next)] != generation && inSphere(next, point)) {
                    stamps[size_t(next)] = generation;
                    cavity.push_back(next);
                }
        }
        for (;;) {
            boundary.clear();
            bool grown = false;
            for (size_t c = 0; c < cavity.size() && !grown; ++c) {
                const Tet &tet = tets[size_t(cavity[c])];
                for (int face = 0; face < 4; ++face) {
                    const int next = tet.n[size_t(face)];
                    if (next >= 0 && stamps[size_t(next)] == generation) continue;
                    auto replaced = tet.v;
                    replaced[size_t(face)] = q;
                    if (volume(replaced) > 0) { boundary.push_back({cavity[c], face}); continue; }
                    if (next < 0 || cavity.size() > 100000) { ++skipped; return; }
                    stamps[size_t(next)] = generation;
                    cavity.push_back(next);
                    grown = true;
                    break;
                }
            }
            if (!grown) break;
        }
        pending.clear();
        for (const Facet &facet : boundary) {
            const auto oldVertices = tets[size_t(facet.tet)].v;
            const int outer = tets[size_t(facet.tet)].n[size_t(facet.face)];
            auto vertices = oldVertices;
            vertices[size_t(facet.face)] = q;
            const int created = allocate(vertices);
            Tet &tet = tets[size_t(created)];
            tet.n.fill(-1);
            tet.n[size_t(facet.face)] = outer;
            if (outer >= 0) {
                Tet &neighbor = tets[size_t(outer)];
                for (size_t k = 0; k < 4; ++k) {
                    bool shared = false;
                    for (size_t j = 0; j < 4; ++j)
                        if (int(j) != facet.face && neighbor.v[k] == vertices[j]) shared = true;
                    if (!shared) neighbor.n[k] = created;
                }
            }
            for (int face = 0; face < 4; ++face) {
                if (face == facet.face) continue;
                int other[2], used = 0;
                for (int j = 0; j < 4; ++j)
                    if (j != face && j != facet.face) other[used++] = vertices[size_t(j)];
                const uint64_t key = (uint64_t(uint32_t(std::min(other[0], other[1]))) << 32) | uint32_t(std::max(other[0], other[1]));
                bool matched = false;
                for (size_t p = 0; p < pending.size(); ++p)
                    if (pending[p].key == key) {
                        tet.n[size_t(face)] = pending[p].tet;
                        tets[size_t(pending[p].tet)].n[size_t(pending[p].face)] = created;
                        pending[p] = pending.back();
                        pending.pop_back();
                        matched = true;
                        break;
                    }
                if (!matched) pending.push_back({key, created, face});
            }
            last = created;
        }
        for (const int id : cavity) {
            tets[size_t(id)].alive = false;
            freeSlots.push_back(id);
        }
    }
};

inline double particleSurfaceRadius(const Dataset &data, uint32_t type) {
    if (type < data.species.size())
        if (const auto *element = elements::find(data.species[type]); element && element->covalent > 0)
            return element->covalent;
    return 1.0;
}
} // namespace surface_detail

struct SurfaceMeshOptions {
    bool gaussian = false, onlySelected = false, selectSurface = false;
    double probeRadius = 3;     // Alpha shape, length.
    int smoothing = 8;          // Alpha shape, fairing iterations.
    int resolution = 50;        // Gaussian, grid cells along the longest box direction.
    double radiusScale = 1;     // Gaussian, multiplies the particle radius.
    double isoValue = .6;       // Gaussian, dimensionless.
    std::array<float, 3> color{.45f, .68f, .88f};
    float transparency = 0;
};

inline void publishSurface(Dataset &data, std::shared_ptr<const SurfaceMeshData> mesh,
                           const std::array<float, 3> &color, float transparency) {
    data.surfaces.push_back({std::move(mesh), color, std::clamp(1.f - transparency, 0.f, 1.f)});
}

inline void publishSurfaceAttributes(Dataset &data, const surface_detail::CellFrame &frame, double area, double filled, size_t triangles) {
    auto &attributes = data.globalAttributes;
    const double volume = frame.volume();
    attributes["ConstructSurfaceMesh.surface_area"] = area;
    attributes["ConstructSurfaceMesh.filled_volume"] = filled;
    attributes["ConstructSurfaceMesh.triangles"] = double(triangles);
    if (volume > 0) {
        attributes["ConstructSurfaceMesh.cell_volume"] = volume;
        attributes["ConstructSurfaceMesh.specific_surface_area"] = area / volume;
        attributes["ConstructSurfaceMesh.filled_fraction"] = filled / volume;
    }
    // Empty space is only bounded when every direction is periodic.
    if (volume > 0 && frame.fullyPeriodic()) {
        attributes["ConstructSurfaceMesh.empty_volume"] = std::max(volume - filled, 0.0);
        attributes["ConstructSurfaceMesh.empty_fraction"] = std::max(volume - filled, 0.0) / volume;
    }
}

// Solid tetrahedra have a circumsphere smaller than the probe sphere; the surface is the boundary
// between solid and empty tetrahedra, so it passes through particle centers.
inline void constructAlphaShapeSurface(Dataset &data, std::vector<uint8_t> &selected, const SurfaceMeshOptions &options,
                                       std::atomic<bool> *cancel) {
    using namespace surface_detail;
    if (!(options.probeRadius > 0) || !std::isfinite(options.probeRadius))
        throw std::runtime_error("The probe sphere radius must be positive");
    const CellFrame frame = frameOf(data);
    // A solid tetrahedron owned by the cell, and its solid or empty neighbor, lie within two
    // circumsphere diameters of the cell, so a layer of four probe radii decides them all.
    const double skin = 4 * options.probeRadius;
    // Lifts lattices out of the cospherical case. The offset is seeded, far below the probe
    // radius, and identical on all images of a particle. It is not applied to the mesh.
    const double jitterSize = 1e-4 * options.probeRadius;
    std::mt19937_64 random(0x41746f6d58ULL);
    std::uniform_real_distribution<double> uniform(-jitterSize, jitterSize);
    struct Image { uint32_t particle; P3 position; };
    std::vector<Image> images;
    std::vector<P3> points;
    std::array<int, 3> reach{};
    for (size_t axis = 0; axis < 3; ++axis)
        if (frame.periodic[axis]) reach[axis] = int(std::ceil(skin / frame.width(int(axis))));
    const size_t count = data.atoms.size();
    for (size_t i = 0; i < count; ++i) {
        if ((i & 65535) == 0) throwIfCancelled(cancel);
        const P3 jitter{uniform(random), uniform(random), uniform(random)};
        if (options.onlySelected && !(i < selected.size() && selected[i])) continue;
        P3 base{data.atoms[i].x, data.atoms[i].y, data.atoms[i].z};
        P3 f = frame.fractional(base);
        for (size_t axis = 0; axis < 3; ++axis)
            if (frame.periodic[axis]) {
                const double whole = std::floor(f[axis]);
                base = sub(base, mul(frame.axes[axis], whole));
                f[axis] -= whole;
            }
        for (int a = -reach[0]; a <= reach[0]; ++a)
            for (int b = -reach[1]; b <= reach[1]; ++b)
                for (int c = -reach[2]; c <= reach[2]; ++c) {
                    const std::array<int, 3> shift{a, b, c};
                    bool withinSkin = true;
                    for (size_t axis = 0; axis < 3 && withinSkin; ++axis)
                        if (frame.periodic[axis]) {
                            const double layer = skin / frame.width(int(axis)), s = f[axis] + shift[axis];
                            withinSkin = s >= -layer && s <= 1 + layer;
                        }
                    if (!withinSkin) continue;
                    if (points.size() >= surfaceDelaunayPointLimit)
                        throw std::runtime_error("The alpha-shape surface is limited to 1,000,000 points including periodic images; use the Gaussian density method");
                    P3 position = base;
                    for (size_t axis = 0; axis < 3; ++axis)
                        position = add(position, mul(frame.axes[axis], double(shift[axis])));
                    images.push_back({uint32_t(i), position});
                    points.push_back(add(position, jitter));
                }
    }
    Soup soup;
    soup.vertexCount = count;
    double filled = 0;
    Tessellation tessellation;
    tessellation.build(std::move(points), cancel);
    if (tessellation.realCount) {
        const double limit = options.probeRadius * options.probeRadius;
        const auto isSolid = [&](int id) {
            const auto &tet = tessellation.tets[size_t(id)];
            return tet.alive && !tessellation.touchesSuper(tet) && tet.radius2 < limit;
        };
        for (size_t id = 0; id < tessellation.tets.size(); ++id) {
            if ((id & 65535) == 0) throwIfCancelled(cancel);
            if (!isSolid(int(id))) continue;
            const auto &tet = tessellation.tets[id];
            P3 middle{};
            for (const int v : tet.v) middle = add(middle, mul(tessellation.points[size_t(v)], .25));
            const P3 f = frame.fractional(middle);
            bool owned = true;
            for (size_t axis = 0; axis < 3; ++axis)
                if (frame.periodic[axis] && (f[axis] < 0 || f[axis] >= 1)) owned = false;
            if (!owned) continue;
            filled += tessellation.volume(tet.v);
            for (size_t face = 0; face < 4; ++face) {
                if (tet.n[face] >= 0 && isSolid(tet.n[face])) continue;
                std::array<size_t, 3> ring{};
                size_t used = 0;
                for (size_t k = 0; k < 4; ++k) if (k != face) ring[used++] = size_t(tet.v[k]);
                const P3 &a = images[ring[0]].position, &b = images[ring[1]].position, &c = images[ring[2]].position;
                if (soup.corners.size() / 3 >= surfaceTriangleLimit)
                    throw std::runtime_error("The surface has more than 2,000,000 triangles; use a coarser setting");
                const P3 outward = sub(a, images[size_t(tet.v[face])].position);
                if (dot(cross(sub(b, a), sub(c, a)), outward) < 0) std::swap(ring[1], ring[2]);
                for (const size_t corner : ring) soup.corners.push_back({images[corner].particle, images[corner].position});
            }
        }
    }
    if (options.selectSurface) {
        selected.assign(count, 0);
        for (const Corner &corner : soup.corners) selected[corner.vertex] = 1;
    }
    smoothSoup(soup, options.smoothing, cancel);
    double area = 0;
    auto mesh = finishMesh(soup, frame, area, cancel);
    publishSurfaceAttributes(data, frame, area, filled, mesh->positions.size() / 3);
    if (tessellation.skipped) data.globalAttributes["ConstructSurfaceMesh.skipped_points"] = double(tessellation.skipped);
    publishSurface(data, std::move(mesh), options.color, options.transparency);
}

// Sums a Gaussian of standard deviation radius * scale at each particle and takes an isosurface.
// The sum stops at four standard deviations, where a Gaussian has fallen below 3.4e-4.
inline void constructGaussianSurface(Dataset &data, const std::vector<uint8_t> &selected, const SurfaceMeshOptions &options,
                                     std::atomic<bool> *cancel) {
    using namespace surface_detail;
    if (options.resolution < 4 || options.resolution > 1024) throw std::runtime_error("The grid resolution must be between 4 and 1024");
    if (!(options.radiusScale > 0)) throw std::runtime_error("The radius scaling must be positive");
    if (!(options.isoValue > 0)) throw std::runtime_error("The iso value must be positive");
    const CellFrame frame = frameOf(data);
    struct Source { P3 fractional; double sigma; };
    std::vector<Source> sources;
    double largest = 0;
    for (size_t i = 0; i < data.atoms.size(); ++i) {
        if (options.onlySelected && !(i < selected.size() && selected[i])) continue;
        const double sigma = particleSurfaceRadius(data, data.atoms[i].type) * options.radiusScale;
        sources.push_back({frame.fractional({data.atoms[i].x, data.atoms[i].y, data.atoms[i].z}), sigma});
        largest = std::max(largest, sigma);
    }
    if (sources.empty()) {
        publishSurfaceAttributes(data, frame, 0, 0, 0);
        return;
    }
    const double reachSigmas = 4;
    std::array<double, 3> low{}, high{};
    for (size_t axis = 0; axis < 3; ++axis) {
        if (frame.periodic[axis]) { low[axis] = 0; high[axis] = 1; continue; }
        low[axis] = std::numeric_limits<double>::infinity();
        high[axis] = -low[axis];
        for (const Source &source : sources) {
            low[axis] = std::min(low[axis], source.fractional[axis]);
            high[axis] = std::max(high[axis], source.fractional[axis]);
        }
        const double margin = (reachSigmas + .5) * largest / frame.width(int(axis));
        low[axis] -= margin;
        high[axis] += margin;
    }
    double longest = 0;
    std::array<double, 3> span{};
    for (size_t axis = 0; axis < 3; ++axis) {
        span[axis] = (high[axis] - low[axis]) * std::sqrt(dot(frame.axes[axis], frame.axes[axis]));
        longest = std::max(longest, span[axis]);
    }
    const double spacing = longest / options.resolution;
    std::array<int, 3> cells{}, points{};
    std::array<P3, 3> step{};
    P3 first = frame.origin;
    size_t total = 1;
    for (size_t axis = 0; axis < 3; ++axis) {
        cells[axis] = std::max(1, int(std::lround(span[axis] / spacing)));
        points[axis] = frame.periodic[axis] ? cells[axis] : cells[axis] + 1;
        step[axis] = mul(frame.axes[axis], (high[axis] - low[axis]) / cells[axis]);
        first = add(first, mul(frame.axes[axis], low[axis]));
        total *= size_t(points[axis]);
        if (total > surfaceVoxelLimit) throw std::runtime_error("The density grid is limited to 32,000,000 points; lower the resolution");
    }
    std::vector<float> field(total, 0.f);
    const auto stepWidth = [&](size_t axis) {
        const P3 n = cross(step[(axis+1)%3], step[(axis+2)%3]);
        return std::abs(dot(step[axis], n)) / std::sqrt(dot(n, n));
    };
    const std::array<double, 3> width{stepWidth(0), stepWidth(1), stepWidth(2)};
    // Each grid layer is filled by one thread, from the particles that reach it.
    struct Entry { uint32_t source; int32_t layer; };
    std::vector<std::vector<Entry>> layers(static_cast<size_t>(points[2]));
    const auto gridCoordinate = [&](const Source &source, size_t axis) {
        double s = source.fractional[axis];
        if (frame.periodic[axis]) s -= std::floor(s);
        return (s - low[axis]) / (high[axis] - low[axis]) * cells[axis];
    };
    size_t entries = 0;
    for (size_t i = 0; i < sources.size(); ++i) {
        if ((i & 65535) == 0) throwIfCancelled(cancel);
        const double center = gridCoordinate(sources[i], 2), extent = reachSigmas * sources[i].sigma / width[2];
        int first_ = int(std::ceil(center - extent)), last_ = int(std::floor(center + extent));
        if (!frame.periodic[2]) { first_ = std::max(first_, 0); last_ = std::min(last_, points[2] - 1); }
        for (int g = first_; g <= last_; ++g) {
            const int wrapped = ((g % points[2]) + points[2]) % points[2];
            layers[size_t(wrapped)].push_back({uint32_t(i), g});
            if (++entries > surfaceDensityEntryLimit) throw std::runtime_error("The density grid is too fine for this many particles; lower the resolution");
        }
    }
    std::atomic<size_t> nextLayer{0};
    std::atomic<bool> stopped{false};
    const auto worker = [&]() {
        for (size_t layer = nextLayer++; layer < layers.size() && !stopped; layer = nextLayer++) {
            if (cancel && *cancel) { stopped = true; return; }
            const size_t base = layer * size_t(points[0]) * size_t(points[1]);
            for (const Entry &entry : layers[layer]) {
                const Source &source = sources[entry.source];
                const double u = gridCoordinate(source, 0), v = gridCoordinate(source, 1), w = gridCoordinate(source, 2);
                const double dz = double(entry.layer) - w, sigma = source.sigma;
                const double denominator = 1 / (2 * sigma * sigma), reach2 = reachSigmas * reachSigmas * sigma * sigma;
                int y0 = int(std::ceil(v - reachSigmas * sigma / width[1])), y1 = int(std::floor(v + reachSigmas * sigma / width[1]));
                int x0 = int(std::ceil(u - reachSigmas * sigma / width[0])), x1 = int(std::floor(u + reachSigmas * sigma / width[0]));
                if (!frame.periodic[1]) { y0 = std::max(y0, 0); y1 = std::min(y1, points[1] - 1); }
                if (!frame.periodic[0]) { x0 = std::max(x0, 0); x1 = std::min(x1, points[0] - 1); }
                for (int gy = y0; gy <= y1; ++gy) {
                    const size_t row = size_t(((gy % points[1]) + points[1]) % points[1]) * size_t(points[0]);
                    const P3 partial = add(mul(step[1], double(gy) - v), mul(step[2], dz));
                    for (int gx = x0; gx <= x1; ++gx) {
                        const P3 d = add(mul(step[0], double(gx) - u), partial);
                        const double r2 = dot(d, d);
                        if (r2 > reach2) continue;
                        field[base + row + size_t(((gx % points[0]) + points[0]) % points[0])] += float(std::exp(-r2 * denominator));
                    }
                }
            }
        }
    };
    {
        const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < threads; ++t) pool.emplace_back(worker);
        worker();
        for (auto &thread : pool) thread.join();
    }
    throwIfCancelled(cancel);
    Soup soup;
    double filled = 0;
    marchField(points, first, step, frame.periodic, field.data(), options.isoValue, soup, filled, cancel);
    double area = 0;
    auto mesh = finishMesh(soup, frame, area, cancel);
    publishSurfaceAttributes(data, frame, area, filled, mesh->positions.size() / 3);
    publishSurface(data, std::move(mesh), options.color, options.transparency);
}

inline void computeIsosurface(Dataset &data, double level, int smoothing, const std::array<float, 3> &color, float transparency,
                              std::atomic<bool> *cancel) {
    using namespace surface_detail;
    if (!data.voxelGrid) throw std::runtime_error("Create isosurface needs a voxel grid; add Spatial binning with more than one bin in every direction first");
    if (!std::isfinite(level)) throw std::runtime_error("The iso-level must be a finite number");
    const VoxelGrid &grid = *data.voxelGrid;
    const CellFrame frame = frameOf(data);
    std::array<P3, 3> step{};
    for (size_t axis = 0; axis < 3; ++axis) step[axis] = {grid.step[axis*3], grid.step[axis*3+1], grid.step[axis*3+2]};
    Soup soup;
    double volume = 0;
    marchField(grid.size, grid.first, step, grid.periodic, grid.values.data(), level, soup, volume, cancel);
    smoothSoup(soup, smoothing, cancel);
    double area = 0;
    auto mesh = finishMesh(soup, frame, area, cancel);
    data.globalAttributes["CreateIsosurface.surface_area"] = area;
    data.globalAttributes["CreateIsosurface.volume_above"] = volume;
    data.globalAttributes["CreateIsosurface.triangles"] = double(mesh->positions.size() / 3);
    publishSurface(data, std::move(mesh), color, transparency);
}

// ---- Text labels ----------------------------------------------------------------

namespace label_detail {
struct Piece { std::string literal, spec; };
// Splits a printf-style format. A specifier may have flags, width and precision and one of
// e E f F g G d i u x X s. Widths and precisions are bounded so a label has a known size.
inline std::vector<Piece> parseFormat(const std::string &format, std::string &tail) {
    std::vector<Piece> pieces;
    std::string literal;
    for (size_t i = 0; i < format.size();) {
        if (format[i] != '%') { literal += format[i++]; continue; }
        if (i + 1 < format.size() && format[i+1] == '%') { literal += '%'; i += 2; continue; }
        size_t j = i + 1;
        while (j < format.size() && std::string_view("-+ #0").find(format[j]) != std::string_view::npos) ++j;
        const auto number = [&]() {
            int value = 0;
            while (j < format.size() && std::isdigit(static_cast<unsigned char>(format[j])) && value < 10000)
                value = value * 10 + (format[j++] - '0');
            return value;
        };
        const int width = number();
        int precision = 0;
        if (j < format.size() && format[j] == '.') { ++j; precision = number(); }
        if (j >= format.size() || std::string_view("eEfFgGdiuxXs").find(format[j]) == std::string_view::npos)
            throw std::runtime_error("Unsupported text label format: " + format);
        if (width > 64 || precision > 32) throw std::runtime_error("Text label format width or precision is too large");
        pieces.push_back({std::move(literal), format.substr(i, j + 1 - i)});
        literal.clear();
        i = j + 1;
    }
    tail = std::move(literal);
    return pieces;
}
inline std::string formatNumber(const std::string &spec, double value) {
    char buffer[128];
    const char conversion = spec.back();
    if (conversion == 's') throw std::runtime_error("%s can only format text; use %g or %.2f for numbers");
    if (std::string_view("diuxX").find(conversion) != std::string_view::npos) {
        std::string integer = spec;
        integer.insert(integer.size() - 1, "ll");
        snprintf(buffer, sizeof(buffer), integer.c_str(), static_cast<long long>(std::llround(value)));
    } else {
        snprintf(buffer, sizeof(buffer), spec.c_str(), value);
    }
    return buffer;
}
} // namespace label_detail

struct TextLabelOptions {
    std::string property, format = "%.6g";
    bool onlySelected = false;
    float size = 13;
    std::array<float, 3> color{1, 1, 1};
    float offsetX = 0, offsetY = 0;
};

// One label per particle from a property and a format. Literal text is copied. A property that
// is text-like (Particle Type) or integral (Particle Identifier) replaces the first specifier
// with its own representation; a format without specifiers gives every particle the same text.
inline void computeTextLabels(Dataset &data, const std::vector<uint8_t> &selected, const TextLabelOptions &options,
                              std::atomic<bool> *cancel) {
    using namespace label_detail;
    std::string tail;
    const auto pieces = parseFormat(options.format, tail);
    const size_t count = data.atoms.size();
    const bool typeNames = options.property == "Particle Type", identifiers = options.property == "Particle Identifier";
    const auto vector = data.vectorProperties.find(options.property);
    const bool wholeVector = vector != data.vectorProperties.end();
    std::vector<double> scalar;
    if (!pieces.empty() && !typeNames && !wholeVector && !options.property.empty())
        scalar = particlePropertyValues(data, options.property, cancel);
    if (!pieces.empty() && options.property.empty()) throw std::runtime_error("Choose a property, or remove the format specifiers for constant text");
    if (wholeVector && !pieces.empty() && pieces.size() != 1 && pieces.size() != 3)
        throw std::runtime_error("A vector property needs one format specifier, or one per component");
    if (!wholeVector && pieces.size() > 1) throw std::runtime_error("A scalar property needs exactly one format specifier");
    TextLabelSet set;
    set.size = options.size;
    set.color = options.color;
    set.offsetX = options.offsetX;
    set.offsetY = options.offsetY;
    for (size_t i = 0; i < count; ++i) {
        if ((i & 65535) == 0) surface_detail::throwIfCancelled(cancel);
        if (options.onlySelected && !(i < selected.size() && selected[i])) continue;
        std::string text;
        if (pieces.empty()) text = tail;
        else if (typeNames || identifiers) {
            text = pieces[0].literal;
            if (typeNames) text += data.atoms[i].type < data.species.size() ? data.species[data.atoms[i].type] : std::to_string(data.atoms[i].type);
            else text += std::to_string(std::llround(scalar[i]));
            text += tail;
        } else if (wholeVector) {
            const Vec3 &value = vector->second[i];
            const double component[3]{value.x, value.y, value.z};
            if (pieces.size() == 3) {
                for (size_t c = 0; c < 3; ++c) text += pieces[c].literal + formatNumber(pieces[c].spec, component[c]);
            } else {
                text = pieces[0].literal;
                for (size_t c = 0; c < 3; ++c) text += (c ? ", " : "") + formatNumber(pieces[0].spec, component[c]);
            }
            text += tail;
        } else {
            text = pieces[0].literal + formatNumber(pieces[0].spec, scalar[i]) + tail;
        }
        if (text.empty()) continue;
        if (set.atoms.size() >= textLabelLimit) throw std::runtime_error("Text labels are limited to 500,000 particles; label only selected particles");
        set.atoms.push_back(uint32_t(i));
        set.text.push_back(std::move(text));
    }
    data.textLabels.push_back(std::move(set));
}
