// VoroTop structure identification and Bader charge integration.
//
// VoroTop matches the topology of each Voronoi cell, written as a Weinberg vector, against a
// filter file the user supplies. Bader integration follows the weight method of Yu and Trinkle.
// All code is AtomX's own. Lengths are in the input length unit.

inline constexpr size_t voroTopFilterBytesLimit = 64u * 1024u * 1024u;
inline constexpr size_t baderRegionLimit = 2000000;
inline constexpr size_t voxelFileBytesLimit = size_t(4) * 1024u * 1024u * 1024u;

namespace vorotop_detail {
struct Filter {
    std::map<int, std::string> names;
    std::map<std::vector<int>, int> types;
};
inline Filter parseFilter(std::istream &in) {
    Filter filter;
    std::string line;
    size_t lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') continue;
        std::istringstream row(line.substr(start));
        if (line[start] == '*') {
            std::string star, name;
            int id = 0;
            if (!(row >> star >> id) || id < 1) throw std::runtime_error("Bad structure type on line " + std::to_string(lineNumber) + " of the VoroTop filter");
            std::getline(row >> std::ws, name);
            filter.names[id] = name.empty() ? "Type " + std::to_string(id) : name;
            continue;
        }
        int id = 0;
        char open = 0;
        if (!(row >> id >> open) || open != '(') throw std::runtime_error("Bad line " + std::to_string(lineNumber) + " in the VoroTop filter");
        if (!filter.names.contains(id)) throw std::runtime_error("The VoroTop filter uses an undeclared structure type on line " + std::to_string(lineNumber));
        std::vector<int> code;
        int value = 0;
        char separator = 0;
        while (row >> value >> separator) {
            code.push_back(value);
            if (separator == ')') break;
            if (separator != ',') throw std::runtime_error("Bad Weinberg vector on line " + std::to_string(lineNumber) + " of the VoroTop filter");
        }
        if (separator != ')' || code.empty()) throw std::runtime_error("Unterminated Weinberg vector on line " + std::to_string(lineNumber) + " of the VoroTop filter");
        filter.types[std::move(code)] = id;
    }
    if (filter.names.empty()) throw std::runtime_error("The VoroTop filter declares no structure types");
    return filter;
}
inline std::shared_ptr<const Filter> loadFilter(const std::filesystem::path &path) {
    // Holds the last filter, since every animation frame needs it again.
    static std::mutex lock;
    static std::filesystem::path cachedPath;
    static std::filesystem::file_time_type cachedTime;
    static std::shared_ptr<const Filter> cached;
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    if (error) throw std::runtime_error("Cannot open the VoroTop filter file");
    const std::lock_guard guard(lock);
    if (cached && cachedPath == path && cachedTime == time) return cached;
    if (std::filesystem::file_size(path, error) > voroTopFilterBytesLimit || error) throw std::runtime_error("The VoroTop filter file is larger than 64 MB");
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open the VoroTop filter file");
    auto filter = std::make_shared<Filter>(parseFilter(in));
    cachedPath = path;
    cachedTime = time;
    cached = filter;
    return cached;
}

// A closed polyhedron as half-edges. twin is the opposite direction; next follows the face.
struct Graph {
    std::vector<int> head, twin, next, tail;
    int vertices = 0;
};
// Faces are vertex rings, all counter-clockwise seen from the outside. Empty when not closed.
inline bool buildGraph(const std::vector<std::vector<int>> &faces, int vertices, Graph &graph) {
    graph = {};
    graph.vertices = vertices;
    std::map<std::pair<int, int>, int> byEnds;
    for (const auto &face : faces) {
        const int first = int(graph.head.size()), size = int(face.size());
        if (size < 3) return false;
        for (int k = 0; k < size; ++k) {
            const int from = face[size_t(k)], to = face[size_t((k + 1) % size)];
            if (!byEnds.try_emplace({from, to}, first + k).second) return false;
            graph.tail.push_back(from);
            graph.head.push_back(to);
            graph.next.push_back(first + (k + 1) % size);
        }
    }
    graph.twin.resize(graph.head.size());
    for (size_t e = 0; e < graph.head.size(); ++e) {
        const auto found = byEnds.find({graph.head[e], graph.tail[e]});
        if (found == byEnds.end()) return false;
        graph.twin[e] = found->second;
    }
    return true;
}
// Weinberg's walk from one directed edge. clockwise picks one of the two mirror orientations.
// Stops as soon as the code is known to be larger than best, returning false.
inline bool walk(const Graph &g, int start, bool clockwise, const std::vector<int> &best, std::vector<int> &code,
                 std::vector<int> &label, std::vector<uint8_t> &used) {
    std::fill(label.begin(), label.end(), 0);
    std::fill(used.begin(), used.end(), uint8_t(0));
    code.clear();
    // The edge after e in rotation around e's tail.
    std::vector<int> previous;
    const auto rotate = [&](int e) {
        if (!clockwise) return g.next[size_t(g.twin[size_t(e)])];
        // Inverse rotation: walk the face of e back to the edge that ends at e's tail.
        int c = e;
        while (g.next[size_t(c)] != e) c = g.next[size_t(c)];
        return g.twin[size_t(c)];
    };
    bool tied = !best.empty();
    const auto append = [&](int value) {
        if (tied) {
            const size_t at = code.size();
            if (at < best.size() && value > best[at]) return false;
            if (at >= best.size() || value < best[at]) tied = false;
        }
        code.push_back(value);
        return true;
    };
    int count = 1, current = start;
    label[size_t(g.tail[size_t(start)])] = 1;
    if (!append(1)) return false;
    for (;;) {
        used[size_t(current)] = 1;
        const int vertex = g.head[size_t(current)], back = g.twin[size_t(current)];
        int following = -1;
        if (!label[size_t(vertex)]) {
            label[size_t(vertex)] = ++count;
            if (!append(count)) return false;
            following = rotate(back);
        } else {
            if (!append(label[size_t(vertex)])) return false;
            if (!used[size_t(back)]) following = back;
            else {
                int candidate = rotate(back);
                while (candidate != back && used[size_t(candidate)]) candidate = rotate(candidate);
                if (candidate != back) following = candidate;
            }
        }
        if (following < 0) break;
        current = following;
    }
    return true;
}
// The lexicographically smallest Weinberg vector over all directed edges and both orientations.
inline std::vector<int> weinbergVector(const Graph &g) {
    std::vector<int> best, code, label(size_t(g.vertices)), used(0);
    std::vector<uint8_t> marks(g.head.size());
    for (int start = 0; start < int(g.head.size()); ++start)
        for (const bool clockwise : {false, true})
            if (walk(g, start, clockwise, best, code, label, marks) && (best.empty() || code < best)) best = code;
    return best;
}
// Merges the face polygons of a cell into shared vertices. Returns false for an open cell.
inline bool graphOfCell(const voronoi_detail::Cell &cell, double tolerance, Graph &graph) {
    std::vector<voronoi_detail::Point> points;
    std::vector<std::vector<int>> faces;
    const double squared = tolerance * tolerance;
    for (const auto &face : cell.faces) {
        if (face.neighbor < 0) return false; // Bounded by a wall, not by neighbors.
        std::vector<int> ring;
        for (const auto &p : face.polygon) {
            int id = -1;
            for (size_t k = 0; k < points.size() && id < 0; ++k) {
                const auto d = voronoi_detail::sub(points[k], p);
                if (voronoi_detail::dot(d, d) <= squared) id = int(k);
            }
            if (id < 0) { id = int(points.size()); points.push_back(p); }
            if (ring.empty() || ring.back() != id) ring.push_back(id);
        }
        while (ring.size() > 1 && ring.front() == ring.back()) ring.pop_back();
        if (ring.size() < 3) continue; // Collapsed to nothing at this tolerance.
        voronoi_detail::Point area{};
        for (size_t k = 1; k + 1 < face.polygon.size(); ++k) {
            const auto n = voronoi_detail::cross(voronoi_detail::sub(face.polygon[k], face.polygon[0]), voronoi_detail::sub(face.polygon[k + 1], face.polygon[0]));
            area = {area[0] + n[0], area[1] + n[1], area[2] + n[2]};
        }
        if (voronoi_detail::dot(area, face.normal) < 0) std::reverse(ring.begin(), ring.end());
        faces.push_back(std::move(ring));
    }
    return buildGraph(faces, int(points.size()), graph);
}
} // namespace vorotop_detail

inline void computeVoroTop(PipelineResult &r, const std::filesystem::path &filterPath, std::atomic<bool> *cancel) {
    using namespace vorotop_detail;
    if (filterPath.empty()) return; // Nothing to match against yet; the node passes data through.
    if (r.data.sampled()) throw std::runtime_error("VoroTop analysis requires complete, unsampled particle data");
    const auto filter = loadFilter(filterPath);
    PipelineResult scratch;
    scratch.data = r.data;
    scratch.selected = r.selected;
    scratch.colorSelected = r.colorSelected;
    std::vector<voronoi_detail::Cell> cells;
    computeVoronoi(scratch, VoronoiOptions{}, cancel, &cells);
    double extent = 0;
    for (size_t axis = 0; axis < 3; ++axis) extent += std::hypot(r.data.cell[axis * 3], r.data.cell[axis * 3 + 1], r.data.cell[axis * 3 + 2]);
    const double tolerance = 2e-6 * std::max(1.0, extent);
    const size_t count = r.data.atoms.size();
    std::vector<double> structure(count, 0.0);
    std::atomic<size_t> next{0};
    std::atomic<bool> stopped{false};
    const auto worker = [&] {
        Graph graph;
        for (size_t i = next++; i < count && !stopped; i = next++) {
            if (cancel && *cancel) { stopped = true; return; }
            if (!graphOfCell(cells[i], tolerance, graph)) continue;
            const auto found = filter->types.find(weinbergVector(graph));
            if (found != filter->types.end()) structure[i] = found->second;
        }
    };
    const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(worker);
    worker();
    for (auto &thread : pool) thread.join();
    surface_detail::throwIfCancelled(cancel);
    std::map<int, size_t> counts;
    for (const double value : structure) ++counts[int(value)];
    r.data.scalarProperties["Structure Type"] = std::move(structure);
    r.data.globalAttributes["VoroTop.counts.OTHER"] = double(counts[0]);
    for (const auto &[id, name] : filter->names) r.data.globalAttributes["VoroTop.counts." + name] = double(counts[id]);
}

// ---- Charge density files ------------------------------------------------------------

namespace bader_detail {
using surface_detail::P3;

inline void finishGrid(VoxelGrid &grid) {
    const auto [low, high] = std::minmax_element(grid.values.begin(), grid.values.end());
    grid.minimum = *low;
    grid.maximum = *high;
    grid.histogram.assign(64, 0);
    const double range = grid.maximum - grid.minimum;
    for (const double value : grid.values)
        ++grid.histogram[range > 0 ? std::min(size_t((value - grid.minimum) / range * 64), size_t(63)) : 0];
}

// A forward reader over a whole file in memory, much faster than stream extraction.
class Tokens {
public:
    explicit Tokens(std::string text) : data(std::move(text)) {}
    std::string line() {
        const size_t end = std::min(data.find('\n', at), data.size());
        std::string result = data.substr(at, end - at);
        at = std::min(end + 1, data.size());
        if (!result.empty() && result.back() == '\r') result.pop_back();
        return result;
    }
    bool number(double &value) {
        while (at < data.size() && std::isspace(static_cast<unsigned char>(data[at]))) ++at;
        if (at >= data.size()) return false;
        const char *first = data.data() + at, *last = data.data() + data.size();
        const auto parsed = std::from_chars(first, last, value);
        if (parsed.ec != std::errc{}) return false;
        at += size_t(parsed.ptr - first);
        return true;
    }
    double require() {
        double value = 0;
        if (!number(value)) throw std::runtime_error("The charge density file ends early or is malformed");
        return value;
    }
private:
    std::string data;
    size_t at = 0;
};
inline std::string readWhole(const std::filesystem::path &path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) throw std::runtime_error("Cannot open the charge density file");
    if (size > voxelFileBytesLimit) throw std::runtime_error("The charge density file is larger than 4 GB");
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open the charge density file");
    std::string text(size_t(size), '\0');
    in.read(text.data(), std::streamsize(size));
    return text;
}
inline void checkSize(const std::array<int, 3> &n) {
    size_t total = 1;
    for (const int v : n) {
        if (v < 1) throw std::runtime_error("The charge density grid has an invalid size");
        total *= size_t(v);
        if (total > surfaceVoxelLimit) throw std::runtime_error("The charge density grid is limited to 32,000,000 points");
    }
}
inline bool isWord(const std::string &text) {
    std::istringstream row(text);
    std::string first;
    row >> first;
    return !first.empty() && std::isalpha(static_cast<unsigned char>(first[0]));
}

// VASP CHGCAR or PARCHG: a POSCAR header, then a grid of density times cell volume with
// x running fastest. Grid point 0 is at the cell origin. Returns density per cubic length.
inline std::shared_ptr<VoxelGrid> readChgcar(Tokens &in, const Dataset &data, std::atomic<bool> *cancel) {
    in.line();
    std::string scaleLine = in.line();
    double scale = std::atof(scaleLine.c_str());
    std::array<P3, 3> lattice{};
    for (auto &row : lattice) { const std::string text = in.line(); std::istringstream values(text); values >> row[0] >> row[1] >> row[2]; if (!values) throw std::runtime_error("Bad lattice line in the CHGCAR file"); }
    const double rawVolume = std::abs(surface_detail::determinant(lattice[0], lattice[1], lattice[2]));
    if (scale < 0) scale = std::cbrt(-scale / rawVolume);
    if (!(scale > 0)) throw std::runtime_error("Bad scale factor in the CHGCAR file");
    for (auto &row : lattice) row = surface_detail::mul(row, scale);
    std::string counts = in.line();
    if (isWord(counts)) counts = in.line(); // VASP 5 lists the species first.
    size_t atoms = 0;
    { std::istringstream row(counts); size_t value = 0; while (row >> value) atoms += value; }
    if (!atoms) throw std::runtime_error("The CHGCAR file lists no atoms");
    std::string mode = in.line();
    if (!mode.empty() && (mode[0] == 's' || mode[0] == 'S')) mode = in.line();
    for (size_t i = 0; i < atoms; ++i) in.line();
    std::array<int, 3> n{};
    for (int &value : n) value = int(in.require());
    checkSize(n);
    auto grid = std::make_shared<VoxelGrid>();
    grid->name = "Charge density";
    grid->size = n;
    const double cellVolume = std::abs(surface_detail::determinant(lattice[0], lattice[1], lattice[2]));
    const size_t total = size_t(n[0]) * size_t(n[1]) * size_t(n[2]);
    grid->values.resize(total);
    for (size_t i = 0; i < total; ++i) {
        if ((i & 1048575) == 0) surface_detail::throwIfCancelled(cancel);
        grid->values[i] = in.require() / cellVolume;
    }
    for (size_t axis = 0; axis < 3; ++axis) {
        for (size_t k = 0; k < 3; ++k) {
            if (std::abs(lattice[axis][k] - data.cell[axis * 3 + k]) > 1e-3 * std::max(1.0, std::abs(lattice[axis][k])))
                throw std::runtime_error("The cell in the CHGCAR file does not match the loaded structure");
            grid->step[axis * 3 + k] = lattice[axis][k] / n[axis];
        }
        grid->periodic[axis] = data.pbc[axis];
    }
    grid->first = {data.origin.x, data.origin.y, data.origin.z};
    return grid;
}

// Gaussian cube: lengths in bohr when the grid counts are positive, in angstrom when negative;
// density in electrons per cubic bohr or per cubic angstrom accordingly. z runs fastest.
inline std::shared_ptr<VoxelGrid> readCube(Tokens &in, const Dataset &data, std::atomic<bool> *cancel) {
    constexpr double bohr = 0.529177210903;
    in.line();
    in.line();
    const int rawAtoms = int(in.require());
    P3 first{in.require(), in.require(), in.require()};
    const std::string rest = in.line();
    std::array<int, 3> n{};
    std::array<P3, 3> step{};
    bool angstrom = false;
    for (size_t axis = 0; axis < 3; ++axis) {
        const int raw = int(in.require());
        if (axis == 0) angstrom = raw < 0;
        n[axis] = std::abs(raw);
        for (double &value : step[axis]) value = in.require();
    }
    checkSize(n);
    const double length = angstrom ? 1 : bohr;
    for (size_t k = 0; k < 3; ++k) first[k] *= length;
    for (auto &row : step) row = surface_detail::mul(row, length);
    { std::istringstream extras(rest); int columns = 0; if ((extras >> columns) && columns > 1) throw std::runtime_error("Cube files with several values per voxel are not supported"); }
    for (int i = 0; i < std::abs(rawAtoms); ++i) for (int c = 0; c < 5; ++c) in.require();
    if (rawAtoms < 0) { // A negative count announces a list of molecular orbital numbers.
        const int entries = int(in.require());
        for (int i = 0; i < entries; ++i) in.require();
    }
    auto grid = std::make_shared<VoxelGrid>();
    grid->name = "Charge density";
    grid->size = n;
    const size_t total = size_t(n[0]) * size_t(n[1]) * size_t(n[2]);
    grid->values.resize(total);
    const double densityUnit = angstrom ? 1 : 1 / (bohr * bohr * bohr);
    size_t read = 0;
    for (int x = 0; x < n[0]; ++x) for (int y = 0; y < n[1]; ++y) for (int z = 0; z < n[2]; ++z) {
        if ((read++ & 1048575) == 0) surface_detail::throwIfCancelled(cancel);
        grid->values[size_t(x) + size_t(n[0]) * (size_t(y) + size_t(n[1]) * size_t(z))] = in.require() * densityUnit;
    }
    grid->first = first;
    for (size_t axis = 0; axis < 3; ++axis) {
        for (size_t k = 0; k < 3; ++k) {
            grid->step[axis * 3 + k] = step[axis][k];
            // Only a grid spanning the whole cell can repeat periodically.
            if (data.pbc[axis] && std::abs(step[axis][k] * n[axis] - data.cell[axis * 3 + k]) > 1e-3 * std::max(1.0, std::abs(data.cell[axis * 3 + k])))
                throw std::runtime_error("The grid in the cube file does not span the periodic cell of the loaded structure");
        }
        grid->periodic[axis] = data.pbc[axis];
    }
    return grid;
}
inline std::shared_ptr<const VoxelGrid> readChargeDensity(const std::filesystem::path &path, const Dataset &data, std::atomic<bool> *cancel) {
    Tokens tokens(readWhole(path));
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    auto grid = extension == ".cube" || extension == ".cub" ? readCube(tokens, data, cancel) : readChgcar(tokens, data, cancel);
    finishGrid(*grid);
    return grid;
}

// The grid neighbors sharing a Voronoi facet with a grid point, with facet area / distance.
struct Stencil { std::array<int, 3> offset; double conductance; };
inline std::vector<Stencil> gridStencil(const std::array<P3, 3> &step) {
    using namespace surface_detail;
    struct Candidate { std::array<int, 3> offset; P3 vector; double length; };
    std::vector<Candidate> candidates;
    for (int a = -3; a <= 3; ++a) for (int b = -3; b <= 3; ++b) for (int c = -3; c <= 3; ++c) {
        if (!a && !b && !c) continue;
        const P3 v = add(add(mul(step[0], a), mul(step[1], b)), mul(step[2], c));
        candidates.push_back({{a, b, c}, v, std::sqrt(dot(v, v))});
    }
    std::vector<Stencil> stencil;
    for (const Candidate &candidate : candidates) {
        const P3 normal = mul(candidate.vector, 1 / candidate.length), point = mul(candidate.vector, .5);
        const P3 helper = std::abs(normal[2]) < .9 ? P3{0, 0, 1} : P3{1, 0, 0};
        P3 u = cross(helper, normal);
        u = mul(u, 1 / std::sqrt(dot(u, u)));
        const P3 w = cross(normal, u);
        const double span = 8 * candidate.length + 8 * std::sqrt(dot(step[0], step[0]) + dot(step[1], step[1]) + dot(step[2], step[2]));
        std::vector<P3> polygon{add(add(point, mul(u, -span)), mul(w, -span)), add(add(point, mul(u, span)), mul(w, -span)),
                                add(add(point, mul(u, span)), mul(w, span)), add(add(point, mul(u, -span)), mul(w, span))};
        for (const Candidate &other : candidates) {
            if (&other == &candidate || polygon.size() < 3) continue;
            const P3 m = mul(other.vector, 1 / other.length);
            const double limit = other.length / 2;
            std::vector<P3> clipped;
            for (size_t i = 0; i < polygon.size(); ++i) {
                const P3 &p = polygon[i], &q = polygon[(i + 1) % polygon.size()];
                const double sp = limit - dot(p, m), sq = limit - dot(q, m);
                if (sp >= 0) clipped.push_back(p);
                if ((sp >= 0) != (sq >= 0)) clipped.push_back(add(p, mul(sub(q, p), sp / (sp - sq))));
            }
            polygon = std::move(clipped);
        }
        if (polygon.size() < 3) continue;
        P3 area{};
        for (size_t i = 1; i + 1 < polygon.size(); ++i) area = add(area, cross(sub(polygon[i], polygon[0]), sub(polygon[i + 1], polygon[0])));
        const double facet = .5 * std::sqrt(dot(area, area));
        if (facet > 1e-9 * candidate.length * candidate.length) stencil.push_back({candidate.offset, facet / candidate.length});
    }
    return stencil;
}
} // namespace bader_detail

// Basins are found from the highest density down. A point's weight in a basin is the sum, over
// uphill neighbors, of their weight times the normalized flux to them. Grid values are read as a
// density per cubic length unit, so integrated density is the sum of density x voxel volume.
inline void computeBaderCharges(Dataset &data, const std::filesystem::path &file, std::atomic<bool> *cancel) {
    using namespace bader_detail;
    if (!file.empty()) data.voxelGrid = readChargeDensity(file, data, cancel);
    if (!data.voxelGrid) throw std::runtime_error("Bader charge integration needs a voxel grid; pick a CHGCAR or cube file");
    const VoxelGrid &grid = *data.voxelGrid;
    std::array<P3, 3> step{};
    for (size_t axis = 0; axis < 3; ++axis) step[axis] = {grid.step[axis * 3], grid.step[axis * 3 + 1], grid.step[axis * 3 + 2]};
    const double voxelVolume = std::abs(surface_detail::determinant(step[0], step[1], step[2]));
    if (!(voxelVolume > 0)) throw std::runtime_error("The voxel grid is degenerate");
    const auto stencil = gridStencil(step);
    const int nx = grid.size[0], ny = grid.size[1], nz = grid.size[2];
    const size_t total = grid.values.size();
    std::vector<uint32_t> order(total);
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return grid.values[a] > grid.values[b]; });
    surface_detail::throwIfCancelled(cancel);
    struct Weight { int basin; float weight; };
    std::vector<int32_t> assigned(total, INT32_MIN);
    std::vector<std::vector<Weight>> shared;
    std::vector<uint32_t> peaks;
    std::vector<std::pair<int, double>> mix;
    for (size_t k = 0; k < total; ++k) {
        if ((k & 262143) == 0) surface_detail::throwIfCancelled(cancel);
        const uint32_t point = order[k];
        const int x = int(point % uint32_t(nx)), rest = int(point / uint32_t(nx)), y = rest % ny, z = rest / ny;
        const double here = grid.values[point];
        double sum = 0;
        mix.clear();
        const auto visit = [&](const auto &&handle) {
            for (const Stencil &s : stencil) {
                int ax = x + s.offset[0], ay = y + s.offset[1], az = z + s.offset[2];
                const auto wrap = [](int &value, int size, bool periodic) {
                    if (value >= 0 && value < size) return true;
                    if (!periodic) return false;
                    value = ((value % size) + size) % size;
                    return true;
                };
                if (!wrap(ax, nx, grid.periodic[0]) || !wrap(ay, ny, grid.periodic[1]) || !wrap(az, nz, grid.periodic[2])) continue;
                const size_t other = size_t(ax) + size_t(nx) * (size_t(ay) + size_t(ny) * size_t(az));
                const double rise = grid.values[other] - here;
                if (rise > 0) handle(other, s.conductance * rise);
            }
        };
        visit([&](size_t, double flux) { sum += flux; });
        if (sum <= 0) {
            if (peaks.size() >= baderRegionLimit) throw std::runtime_error("The density has more than 2,000,000 maxima");
            assigned[point] = int32_t(peaks.size());
            peaks.push_back(point);
            continue;
        }
        const auto addTo = [&](int basin, double weight) {
            for (auto &entry : mix) if (entry.first == basin) { entry.second += weight; return; }
            mix.emplace_back(basin, weight);
        };
        visit([&](size_t other, double flux) {
            const double fraction = flux / sum;
            const int32_t code = assigned[other];
            if (code >= 0) addTo(code, fraction);
            else for (const Weight &w : shared[size_t(-(code + 1))]) addTo(w.basin, fraction * w.weight);
        });
        if (mix.size() == 1) { assigned[point] = int32_t(mix[0].first); continue; }
        // Vanishing weights would otherwise spread along the boundary without limit.
        std::vector<Weight> kept;
        double keptTotal = 0;
        for (const auto &entry : mix) if (entry.second >= 1e-5) { kept.push_back({entry.first, float(entry.second)}); keptTotal += entry.second; }
        for (Weight &w : kept) w.weight = float(w.weight / keptTotal);
        if (kept.size() == 1) { assigned[point] = int32_t(kept[0].basin); continue; }
        assigned[point] = -int32_t(shared.size()) - 1;
        shared.push_back(std::move(kept));
    }
    std::vector<double> charge(peaks.size()), volume(peaks.size());
    for (size_t point = 0; point < total; ++point) {
        const int32_t code = assigned[point];
        const double amount = grid.values[point] * voxelVolume;
        if (code >= 0) { charge[size_t(code)] += amount; volume[size_t(code)] += voxelVolume; }
        else for (const Weight &w : shared[size_t(-(code + 1))]) { charge[size_t(w.basin)] += amount * w.weight; volume[size_t(w.basin)] += voxelVolume * w.weight; }
    }
    const size_t atoms = data.atoms.size();
    std::vector<double> atomCharge(atoms), atomVolume(atoms);
    const surface_detail::CellFrame frame = surface_detail::frameOf(data);
    DataTable table;
    table.name = "Bader charge integration";
    table.columns = {"Region", "Integrated Density", "Integrated Volume", "Position.X", "Position.Y", "Position.Z"};
    if (atoms) { table.columns.push_back("Mapped Particle Index"); table.columns.push_back("Mapped Particle Distance"); }
    double totalCharge = 0, totalVolume = 0;
    for (size_t region = 0; region < peaks.size(); ++region) {
        if ((region & 255) == 0) surface_detail::throwIfCancelled(cancel);
        const uint32_t point = peaks[region];
        const int x = int(point % uint32_t(nx)), rest = int(point / uint32_t(nx)), y = rest % ny, z = rest / ny;
        const P3 position = surface_detail::add(surface_detail::add(surface_detail::add(P3{grid.first[0], grid.first[1], grid.first[2]}, surface_detail::mul(step[0], x)), surface_detail::mul(step[1], y)), surface_detail::mul(step[2], z));
        std::vector<std::string> row{std::to_string(region + 1), formatDataNumber(charge[region]), formatDataNumber(volume[region]),
                                     formatDataNumber(position[0]), formatDataNumber(position[1]), formatDataNumber(position[2])};
        totalCharge += charge[region];
        totalVolume += volume[region];
        if (atoms) {
            double bestSquared = std::numeric_limits<double>::infinity();
            size_t best = 0;
            for (size_t i = 0; i < atoms; ++i) {
                P3 delta = surface_detail::sub(position, P3{data.atoms[i].x, data.atoms[i].y, data.atoms[i].z});
                if (frame.hasCell) {
                    P3 f{surface_detail::dot(frame.inverse[0], delta), surface_detail::dot(frame.inverse[1], delta), surface_detail::dot(frame.inverse[2], delta)};
                    for (size_t a = 0; a < 3; ++a) if (frame.periodic[a]) delta = surface_detail::sub(delta, surface_detail::mul(frame.axes[a], std::round(f[a])));
                }
                // Rounding is exact for orthogonal cells; the next images settle tilted ones.
                for (int a = frame.periodic[0] ? -1 : 0; a <= (frame.periodic[0] ? 1 : 0); ++a)
                    for (int b = frame.periodic[1] ? -1 : 0; b <= (frame.periodic[1] ? 1 : 0); ++b)
                        for (int c = frame.periodic[2] ? -1 : 0; c <= (frame.periodic[2] ? 1 : 0); ++c) {
                            const P3 image = surface_detail::add(surface_detail::add(surface_detail::add(delta, surface_detail::mul(frame.axes[0], a)), surface_detail::mul(frame.axes[1], b)), surface_detail::mul(frame.axes[2], c));
                            const double squared = surface_detail::dot(image, image);
                            if (squared < bestSquared) { bestSquared = squared; best = i; }
                        }
            }
            atomCharge[best] += charge[region];
            atomVolume[best] += volume[region];
            row.push_back(std::to_string(best));
            row.push_back(formatDataNumber(std::sqrt(bestSquared)));
        }
        table.rows.push_back(std::move(row));
    }
    data.tables.push_back(std::move(table));
    data.globalAttributes["Bader.regions"] = double(peaks.size());
    data.globalAttributes["Bader.integrated_density"] = totalCharge;
    data.globalAttributes["Bader.integrated_volume"] = totalVolume;
    if (atoms) {
        data.scalarProperties["Integrated Density"] = std::move(atomCharge);
        data.scalarProperties["Integrated Volume"] = std::move(atomVolume);
    }
}
