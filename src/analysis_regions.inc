// Ambient occlusion shading and meshes of LAMMPS region commands.
//
// Ambient occlusion follows the idea OVITO documents (parallel light from many directions,
// each particle lit by its exposed pixels) but runs on threads of AtomX's own software
// rasterizer. Region commands are interpreted from the syntax in the LAMMPS manual.

inline constexpr size_t ambientBufferBytesLimit = 256u * 1024u * 1024u;

inline void computeAmbientOcclusion(Dataset &data, double intensity, int samples, int resolution, std::atomic<bool> *cancel) {
    using namespace surface_detail;
    if (!(intensity >= 0 && intensity <= 1)) throw std::runtime_error("Shading intensity must be between 0 and 100%");
    if (samples < 1 || samples > 1024) throw std::runtime_error("The number of exposure samples must be between 1 and 1024");
    if (resolution < 16 || resolution > 2048) throw std::runtime_error("The render buffer resolution must be between 16 and 2048");
    const size_t count = data.atoms.size();
    auto &brightness = data.scalarProperties["Ambient Occlusion"];
    brightness.assign(count, 1.0);
    if (!count || intensity == 0) return;
    std::vector<double> radii(count);
    P3 middle{};
    double largestRadius = 0;
    for (size_t i = 0; i < count; ++i) {
        radii[i] = particleSurfaceRadius(data, data.atoms[i].type);
        largestRadius = std::max(largestRadius, radii[i]);
        middle = add(middle, P3{data.atoms[i].x / double(count), data.atoms[i].y / double(count), data.atoms[i].z / double(count)});
    }
    double extent = 0;
    for (const Atom &atom : data.atoms) {
        const P3 d = sub(P3{atom.x, atom.y, atom.z}, middle);
        extent = std::max(extent, dot(d, d));
    }
    extent = std::sqrt(extent) + largestRadius;
    const double pixelsPerLength = resolution / (2 * extent * 1.001);
    const size_t pixels = size_t(resolution) * size_t(resolution);
    const unsigned threads = unsigned(std::clamp<size_t>(std::min<size_t>(std::thread::hardware_concurrency(),
        ambientBufferBytesLimit / (pixels * (sizeof(float) + sizeof(uint32_t)))), 1, 16));
    std::vector<std::vector<uint32_t>> perThread(threads, std::vector<uint32_t>(count, 0u));
    std::atomic<int> nextSample{0};
    std::atomic<bool> stopped{false};
    const auto worker = [&](unsigned thread) {
        std::vector<float> depth(pixels);
        std::vector<uint32_t> owner(pixels);
        auto &exposure = perThread[thread];
        for (int sample = nextSample++; sample < samples && !stopped; sample = nextSample++) {
            if (cancel && *cancel) { stopped = true; return; }
            // Evenly spread directions on the whole sphere (a Fibonacci lattice).
            const double z = 1 - (2.0 * sample + 1) / samples, ring = std::sqrt(std::max(0.0, 1 - z * z));
            const double phi = sample * 2.399963229728653;
            const P3 light{ring * std::cos(phi), ring * std::sin(phi), z};
            const P3 helper = std::abs(light[2]) < .9 ? P3{0, 0, 1} : P3{1, 0, 0};
            P3 u = cross(helper, light);
            u = mul(u, 1 / std::sqrt(dot(u, u)));
            const P3 v = cross(light, u);
            std::fill(depth.begin(), depth.end(), -std::numeric_limits<float>::infinity());
            std::fill(owner.begin(), owner.end(), UINT32_MAX);
            for (size_t i = 0; i < count; ++i) {
                const P3 d = sub(P3{data.atoms[i].x, data.atoms[i].y, data.atoms[i].z}, middle);
                const double cx = (dot(d, u) + extent * 1.001) * pixelsPerLength, cy = (dot(d, v) + extent * 1.001) * pixelsPerLength;
                const double h = dot(d, light), r = radii[i] * pixelsPerLength, r2 = r * r;
                const int x0 = std::clamp(int(std::floor(cx - r)), 0, resolution - 1), x1 = std::clamp(int(std::ceil(cx + r)), 0, resolution - 1);
                const int y0 = std::clamp(int(std::floor(cy - r)), 0, resolution - 1), y1 = std::clamp(int(std::ceil(cy + r)), 0, resolution - 1);
                bool wrote = false;
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x) {
                        const double dx = x + .5 - cx, dy = y + .5 - cy, in = r2 - dx * dx - dy * dy;
                        if (in < 0) continue;
                        const float height = float(h + std::sqrt(in) / pixelsPerLength);
                        const size_t at = size_t(y) * size_t(resolution) + size_t(x);
                        wrote = true;
                        if (height > depth[at]) { depth[at] = height; owner[at] = uint32_t(i); }
                    }
                if (!wrote) {
                    // Smaller than a pixel: it still claims the pixel at its center.
                    const size_t at = size_t(std::clamp(int(cy), 0, resolution - 1)) * size_t(resolution) + size_t(std::clamp(int(cx), 0, resolution - 1));
                    if (float(h) > depth[at]) { depth[at] = float(h); owner[at] = uint32_t(i); }
                }
            }
            for (const uint32_t id : owner) if (id != UINT32_MAX) ++exposure[id];
        }
    };
    {
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < threads; ++t) pool.emplace_back(worker, t);
        worker(0);
        for (auto &thread : pool) thread.join();
    }
    throwIfCancelled(cancel);
    const double pi = std::acos(-1.0);
    double highest = 0;
    for (size_t i = 0; i < count; ++i) {
        uint64_t total = 0;
        for (const auto &exposure : perThread) total += exposure[i];
        const double r = radii[i] * pixelsPerLength;
        brightness[i] = double(total) / (samples * std::max(pi * r * r, 1.0));
        highest = std::max(highest, brightness[i]);
    }
    // The best-lit particle keeps its color, the others are scaled relative to it.
    for (double &value : brightness) {
        const double lit = highest > 0 ? std::min(value / highest, 1.0) : 1.0;
        value = 1 - intensity + intensity * lit;
    }
}

// ---- LAMMPS regions ---------------------------------------------------------------

namespace region_detail {
using surface_detail::P3;
using surface_detail::add;
using surface_detail::sub;
using surface_detail::mul;
using surface_detail::dot;
using surface_detail::cross;

enum class Style { Block, Cone, Cylinder, Ellipsoid, Plane, Prism, Sphere };
struct Value { double number = 0; bool infinite = false, edge = false; };
struct Region {
    Style style = Style::Block;
    std::array<Value, 9> values{};
    int axis = 2; // Cone and cylinder.
    bool inside = true;
    std::array<bool, 7> open{};
    bool rotated = false;
    double theta = 0;
    P3 point{}, direction{0, 0, 1}, move{};
};

inline P3 rotateAbout(const P3 &value, const P3 &axis, double angle) {
    const double c = std::cos(angle), s = std::sin(angle);
    return add(add(mul(value, c), mul(cross(axis, value), s)), mul(axis, dot(axis, value) * (1 - c)));
}
// Rotation about the given point first, then the move (LAMMPS applies the move to the point).
inline P3 toWorld(const Region &region, P3 p) {
    if (region.rotated) p = add(region.point, rotateAbout(sub(p, region.point), region.direction, region.theta));
    return add(p, region.move);
}
inline P3 toLocal(const Region &region, P3 p) {
    p = sub(p, region.move);
    if (region.rotated) p = add(region.point, rotateAbout(sub(p, region.point), region.direction, -region.theta));
    return p;
}
inline Value parseValue(const std::string &token, bool allowUnbounded) {
    Value value;
    if (token == "INF" || token == "EDGE") {
        if (!allowUnbounded) throw std::runtime_error("INF and EDGE are not allowed for this region argument: " + token);
        value.infinite = token == "INF";
        value.edge = token == "EDGE";
        return value;
    }
    if (token.rfind("v_", 0) == 0) throw std::runtime_error("Regions cannot use variables; replace " + token + " by a number");
    size_t used = 0;
    try { value.number = std::stod(token, &used); }
    catch (const std::exception &) { used = 0; }
    if (used != token.size() || !std::isfinite(value.number)) throw std::runtime_error("Expected a number in the region command: " + token);
    return value;
}
inline double number(const std::string &token) { return parseValue(token, false).number; }

// Splits text into commands, honoring # comments and a trailing & as line continuation.
inline std::vector<std::vector<std::string>> splitCommands(const std::string &text) {
    std::vector<std::vector<std::string>> commands;
    std::vector<std::string> current;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (const size_t hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        std::istringstream words(line);
        std::string word;
        std::vector<std::string> tokens;
        while (words >> word) tokens.push_back(word);
        const bool continues = !tokens.empty() && tokens.back() == "&";
        if (continues) tokens.pop_back();
        current.insert(current.end(), tokens.begin(), tokens.end());
        if (!continues && !current.empty()) { commands.push_back(std::move(current)); current.clear(); }
    }
    if (!current.empty()) commands.push_back(std::move(current));
    return commands;
}
inline Region parseRegion(const std::vector<std::string> &tokens) {
    if (tokens[0] != "region") throw std::runtime_error("Only region commands are accepted: " + tokens[0]);
    if (tokens.size() < 3) throw std::runtime_error("A region command needs an ID and a style");
    Region region;
    const std::string &style = tokens[2];
    size_t needed = 0;
    if (style == "block") { region.style = Style::Block; needed = 6; }
    else if (style == "cone") { region.style = Style::Cone; needed = 7; }
    else if (style == "cylinder") { region.style = Style::Cylinder; needed = 6; }
    else if (style == "ellipsoid") { region.style = Style::Ellipsoid; needed = 6; }
    else if (style == "plane") { region.style = Style::Plane; needed = 6; }
    else if (style == "prism") { region.style = Style::Prism; needed = 9; }
    else if (style == "sphere") { region.style = Style::Sphere; needed = 4; }
    else throw std::runtime_error("Unsupported region style: " + style);
    if (tokens.size() < 3 + needed) throw std::runtime_error("Too few arguments for region style " + style);
    size_t next = 3;
    if (region.style == Style::Cone || region.style == Style::Cylinder) {
        const std::string &dim = tokens[next];
        if (dim != "x" && dim != "y" && dim != "z") throw std::runtime_error("The axis of a " + style + " must be x, y or z");
        region.axis = dim == "x" ? 0 : dim == "y" ? 1 : 2;
        region.values[0].number = number(tokens[next + 1]);
        region.values[1].number = number(tokens[next + 2]);
        size_t slot = 2;
        const size_t radii = region.style == Style::Cone ? 2 : 1;
        for (size_t k = 0; k < radii; ++k) region.values[slot++].number = number(tokens[next + 3 + k]);
        region.values[slot++] = parseValue(tokens[next + 3 + radii], true);
        region.values[slot++] = parseValue(tokens[next + 4 + radii], true);
    } else {
        const bool unbounded = region.style == Style::Block || region.style == Style::Prism;
        for (size_t k = 0; k < needed; ++k) region.values[k] = parseValue(tokens[next + k], unbounded && k < 6);
    }
    next += needed;
    while (next < tokens.size()) {
        const std::string &keyword = tokens[next];
        const auto require = [&](size_t count) {
            if (next + count >= tokens.size()) throw std::runtime_error("Too few arguments for the region keyword " + keyword);
        };
        if (keyword == "side") {
            require(1);
            if (tokens[next + 1] != "in" && tokens[next + 1] != "out") throw std::runtime_error("side must be in or out");
            region.inside = tokens[next + 1] == "in";
            next += 2;
        } else if (keyword == "units") {
            require(1);
            if (tokens[next + 1] != "box") throw std::runtime_error("Only units box is supported; AtomX has no LAMMPS lattice");
            next += 2;
        } else if (keyword == "open") {
            require(1);
            const int face = std::atoi(tokens[next + 1].c_str());
            if (face < 1 || face > 6) throw std::runtime_error("open needs a face number from 1 to 6");
            region.open[size_t(face)] = true;
            next += 2;
        } else if (keyword == "move") {
            require(3);
            for (size_t k = 0; k < 3; ++k) region.move[k] = tokens[next + 1 + k] == "NULL" ? 0 : number(tokens[next + 1 + k]);
            next += 4;
        } else if (keyword == "rotate") {
            require(7);
            region.rotated = true;
            region.theta = number(tokens[next + 1]);
            for (size_t k = 0; k < 3; ++k) {
                region.point[k] = number(tokens[next + 2 + k]);
                region.direction[k] = number(tokens[next + 5 + k]);
            }
            const double length = std::sqrt(dot(region.direction, region.direction));
            if (!(length > 0)) throw std::runtime_error("The rotation axis must not be zero");
            region.direction = mul(region.direction, 1 / length);
            next += 8;
        } else {
            throw std::runtime_error("Unsupported region keyword: " + keyword);
        }
    }
    const auto positive = [](double value, const char *what) {
        if (!(value > 0)) throw std::runtime_error(std::string(what) + " must be positive");
    };
    if (region.style == Style::Sphere) positive(region.values[3].number, "The sphere radius");
    if (region.style == Style::Cylinder) positive(region.values[2].number, "The cylinder radius");
    if (region.style == Style::Ellipsoid) for (size_t k = 3; k < 6; ++k) positive(region.values[k].number, "The ellipsoid axes");
    if (region.style == Style::Cone) {
        if (region.values[2].number < 0 || region.values[3].number < 0 || (region.values[2].number == 0 && region.values[3].number == 0))
            throw std::runtime_error("One cone radius must be positive and neither may be negative");
    }
    if (region.style == Style::Plane && !(dot(P3{region.values[3].number, region.values[4].number, region.values[5].number}, P3{region.values[3].number, region.values[4].number, region.values[5].number}) > 0))
        throw std::runtime_error("The plane normal must not be zero");
    return region;
}
struct Box { P3 low, high; };
// The axis-aligned bounds of the simulation cell, or of the particles without a cell.
inline Box boxOf(const Dataset &data) {
    Box box{{data.lo.x, data.lo.y, data.lo.z}, {data.hi.x, data.hi.y, data.hi.z}};
    if (std::abs(cellDeterminant(data.cell)) <= 1e-9) return box;
    const P3 origin{data.origin.x, data.origin.y, data.origin.z};
    box.low = box.high = origin;
    for (int corner = 1; corner < 8; ++corner) {
        P3 p = origin;
        for (size_t axis = 0; axis < 3; ++axis)
            if (corner >> axis & 1) p = add(p, P3{data.cell[axis * 3], data.cell[axis * 3 + 1], data.cell[axis * 3 + 2]});
        for (size_t k = 0; k < 3; ++k) { box.low[k] = std::min(box.low[k], p[k]); box.high[k] = std::max(box.high[k], p[k]); }
    }
    return box;
}
// The bound used for drawing: INF and EDGE stop at the box.
inline double drawn(const Value &value, double boxBound) { return value.infinite || value.edge ? boxBound : value.number; }
// The bound used for particle tests: INF is unbounded, EDGE is the box as LAMMPS defines it.
inline double tested(const Value &value, double boxBound, double unbounded) { return value.infinite ? unbounded : value.edge ? boxBound : value.number; }

inline bool contains(const Region &region, const Box &box, P3 p) {
    p = toLocal(region, p);
    const auto &v = region.values;
    const double big = 1e20;
    bool in = false;
    switch (region.style) {
    case Style::Block:
        in = true;
        for (size_t a = 0; a < 3; ++a)
            in = in && p[a] >= tested(v[a * 2], box.low[a], -big) && p[a] <= tested(v[a * 2 + 1], box.high[a], big);
        break;
    case Style::Prism: {
        const P3 low{tested(v[0], box.low[0], -big), tested(v[2], box.low[1], -big), tested(v[4], box.low[2], -big)};
        const P3 high{tested(v[1], box.high[0], big), tested(v[3], box.high[1], big), tested(v[5], box.high[2], big)};
        // Edge vectors A = (lx, 0, 0), B = (xy, ly, 0), C = (xz, yz, lz) from the low corner.
        const double xy = v[6].number, xz = v[7].number, yz = v[8].number;
        const double fz = (p[2] - low[2]) / (high[2] - low[2]);
        const double fy = (p[1] - low[1] - yz * fz) / (high[1] - low[1]);
        const double x = p[0] - xy * fy - xz * fz;
        in = fz >= 0 && fz <= 1 && fy >= 0 && fy <= 1 && x >= low[0] && x <= high[0];
        break;
    }
    case Style::Sphere: {
        const P3 d = sub(p, P3{v[0].number, v[1].number, v[2].number});
        in = dot(d, d) <= v[3].number * v[3].number;
        break;
    }
    case Style::Ellipsoid: {
        P3 d = sub(p, P3{v[0].number, v[1].number, v[2].number});
        for (size_t a = 0; a < 3; ++a) d[a] /= v[3 + a].number;
        in = dot(d, d) <= 1;
        break;
    }
    case Style::Plane:
        in = dot(sub(p, P3{v[0].number, v[1].number, v[2].number}), P3{v[3].number, v[4].number, v[5].number}) >= 0;
        break;
    case Style::Cylinder:
    case Style::Cone: {
        const size_t axis = size_t(region.axis), first = axis == 0 ? 1 : 0, second = axis == 2 ? 1 : 2;
        const size_t loSlot = region.style == Style::Cone ? 4 : 3;
        const double lo = tested(v[loSlot], box.low[axis], -big), hi = tested(v[loSlot + 1], box.high[axis], big);
        if (p[axis] < lo || p[axis] > hi) break;
        double radius = v[2].number;
        if (region.style == Style::Cone) radius = hi > lo ? v[2].number + (v[3].number - v[2].number) * (p[axis] - lo) / (hi - lo) : v[2].number;
        const double a = p[first] - v[0].number, b = p[second] - v[1].number;
        in = a * a + b * b <= radius * radius;
        break;
    }
    }
    return region.inside ? in : !in;
}

struct Builder {
    const Region &region;
    SurfaceMeshData &mesh;
    void vertex(const P3 &p, const P3 &n) {
        const P3 world = toWorld(region, p);
        const P3 normal = region.rotated ? rotateAbout(n, region.direction, region.theta) : n;
        mesh.positions.push_back({float(world[0]), float(world[1]), float(world[2])});
        mesh.normals.push_back({float(normal[0]), float(normal[1]), float(normal[2])});
    }
    void triangle(const P3 &a, const P3 &na, const P3 &b, const P3 &nb, const P3 &c, const P3 &nc) {
        vertex(a, na); vertex(b, nb); vertex(c, nc);
    }
    void quad(const P3 &a, const P3 &b, const P3 &c, const P3 &d, const P3 &n) {
        triangle(a, n, b, n, c, n);
        triangle(a, n, c, n, d, n);
    }
};
inline P3 unit(const P3 &p) { const double l = std::sqrt(dot(p, p)); return l > 0 ? mul(p, 1 / l) : P3{0, 0, 1}; }

inline void buildBox(Builder &b, const Region &region, const P3 &origin, const P3 &ax, const P3 &ay, const P3 &az) {
    const P3 o = origin, x = add(o, ax), y = add(o, ay), z = add(o, az), xy = add(x, ay), xz = add(x, az), yz = add(y, az), xyz = add(xy, az);
    const P3 nx = unit(cross(ay, az)), ny = unit(cross(az, ax)), nz = unit(cross(ax, ay));
    if (!region.open[1]) b.quad(o, z, yz, y, mul(nx, -1));
    if (!region.open[2]) b.quad(x, xy, xyz, xz, nx);
    if (!region.open[3]) b.quad(o, x, xz, z, mul(ny, -1));
    if (!region.open[4]) b.quad(y, yz, xyz, xy, ny);
    if (!region.open[5]) b.quad(o, y, xy, x, mul(nz, -1));
    if (!region.open[6]) b.quad(z, xz, xyz, yz, nz);
}
inline void buildEllipsoid(Builder &b, const P3 &center, const P3 &axes) {
    const double pi = std::acos(-1.0);
    constexpr int rings = 32, segments = 64;
    const auto point = [&](int ring, int segment) {
        const double t = pi * ring / rings, p = 2 * pi * segment / segments;
        return P3{std::sin(t) * std::cos(p), std::sin(t) * std::sin(p), std::cos(t)};
    };
    const auto at = [&](const P3 &u) { return add(center, P3{u[0] * axes[0], u[1] * axes[1], u[2] * axes[2]}); };
    const auto normal = [&](const P3 &u) { return unit(P3{u[0] / axes[0], u[1] / axes[1], u[2] / axes[2]}); };
    for (int ring = 0; ring < rings; ++ring)
        for (int segment = 0; segment < segments; ++segment) {
            const P3 a = point(ring, segment), c = point(ring + 1, segment), d = point(ring + 1, segment + 1), e = point(ring, segment + 1);
            if (ring) b.triangle(at(a), normal(a), at(c), normal(c), at(e), normal(e));
            if (ring + 1 < rings) b.triangle(at(c), normal(c), at(d), normal(d), at(e), normal(e));
        }
}
inline void buildConical(Builder &b, const Region &region, const Box &box) {
    const double pi = std::acos(-1.0);
    constexpr int segments = 96;
    const auto &v = region.values;
    const size_t axis = size_t(region.axis), first = axis == 0 ? 1 : 0, second = axis == 2 ? 1 : 2;
    const bool cone = region.style == Style::Cone;
    const size_t slot = cone ? 4 : 3;
    const double lo = drawn(v[slot], box.low[axis]), hi = drawn(v[slot + 1], box.high[axis]);
    const double radiusLow = v[2].number, radiusHigh = cone ? v[3].number : v[2].number;
    const auto around = [&](int k, double radius, double height) {
        const double angle = 2 * pi * k / segments;
        P3 p{};
        p[first] = v[0].number + radius * std::cos(angle);
        p[second] = v[1].number + radius * std::sin(angle);
        p[axis] = height;
        return p;
    };
    const auto outward = [&](int k) {
        const double angle = 2 * pi * k / segments, slope = hi > lo ? (radiusLow - radiusHigh) / (hi - lo) : 0;
        P3 n{};
        n[first] = std::cos(angle);
        n[second] = std::sin(angle);
        n[axis] = slope;
        return unit(n);
    };
    P3 down{}, up{}, lowCenter{}, highCenter{};
    down[axis] = -1;
    up[axis] = 1;
    lowCenter[first] = highCenter[first] = v[0].number;
    lowCenter[second] = highCenter[second] = v[1].number;
    lowCenter[axis] = lo;
    highCenter[axis] = hi;
    for (int k = 0; k < segments; ++k) {
        const P3 a = around(k, radiusLow, lo), c = around(k + 1, radiusLow, lo), e = around(k + 1, radiusHigh, hi), f = around(k, radiusHigh, hi);
        if (!region.open[3]) {
            const P3 n0 = outward(k), n1 = outward(k + 1);
            if (radiusLow > 0) b.triangle(a, n0, c, n1, e, n1);
            if (radiusHigh > 0) b.triangle(a, n0, e, n1, f, n0);
        }
        if (!region.open[1] && radiusLow > 0) b.triangle(lowCenter, down, c, down, a, down);
        if (!region.open[2] && radiusHigh > 0) b.triangle(highCenter, up, f, up, e, up);
    }
}
// The part of the infinite plane inside the box, as a fan of triangles.
inline void buildPlane(Builder &b, const Region &region, const Box &box) {
    const auto &v = region.values;
    const P3 point{v[0].number, v[1].number, v[2].number}, normal = unit(P3{v[3].number, v[4].number, v[5].number});
    const P3 helper = std::abs(normal[2]) < .9 ? P3{0, 0, 1} : P3{1, 0, 0};
    const P3 u = unit(cross(helper, normal)), w = cross(normal, u);
    double span = 1;
    for (size_t a = 0; a < 3; ++a) span += std::max(std::abs(box.high[a] - point[a]), std::abs(box.low[a] - point[a]));
    span *= 2;
    std::vector<P3> polygon{add(add(point, mul(u, -span)), mul(w, -span)), add(add(point, mul(u, span)), mul(w, -span)),
                            add(add(point, mul(u, span)), mul(w, span)), add(add(point, mul(u, -span)), mul(w, span))};
    for (size_t axis = 0; axis < 3; ++axis)
        for (const bool upper : {false, true}) {
            const auto side = [&](const P3 &p) { return upper ? box.high[axis] - p[axis] : p[axis] - box.low[axis]; };
            std::vector<P3> clipped;
            for (size_t i = 0; i < polygon.size(); ++i) {
                const P3 &p = polygon[i], &q = polygon[(i + 1) % polygon.size()];
                const double sp = side(p), sq = side(q);
                if (sp >= 0) clipped.push_back(p);
                if ((sp >= 0) != (sq >= 0)) clipped.push_back(add(p, mul(sub(q, p), sp / (sp - sq))));
            }
            polygon = std::move(clipped);
        }
    for (size_t i = 1; i + 1 < polygon.size(); ++i) b.triangle(polygon[0], normal, polygon[i], normal, polygon[i + 1], normal);
}
inline std::shared_ptr<const SurfaceMeshData> buildMesh(const Region &region, const Box &box) {
    auto mesh = std::make_shared<SurfaceMeshData>();
    Builder builder{region, *mesh};
    const auto &v = region.values;
    switch (region.style) {
    case Style::Block: {
        const P3 low{drawn(v[0], box.low[0]), drawn(v[2], box.low[1]), drawn(v[4], box.low[2])};
        const P3 high{drawn(v[1], box.high[0]), drawn(v[3], box.high[1]), drawn(v[5], box.high[2])};
        buildBox(builder, region, low, {high[0] - low[0], 0, 0}, {0, high[1] - low[1], 0}, {0, 0, high[2] - low[2]});
        break;
    }
    case Style::Prism: {
        const P3 low{drawn(v[0], box.low[0]), drawn(v[2], box.low[1]), drawn(v[4], box.low[2])};
        const P3 high{drawn(v[1], box.high[0]), drawn(v[3], box.high[1]), drawn(v[5], box.high[2])};
        buildBox(builder, region, low, {high[0] - low[0], 0, 0}, {v[6].number, high[1] - low[1], 0}, {v[7].number, v[8].number, high[2] - low[2]});
        break;
    }
    case Style::Sphere: buildEllipsoid(builder, {v[0].number, v[1].number, v[2].number}, {v[3].number, v[3].number, v[3].number}); break;
    case Style::Ellipsoid: buildEllipsoid(builder, {v[0].number, v[1].number, v[2].number}, {v[3].number, v[4].number, v[5].number}); break;
    case Style::Plane: buildPlane(builder, region, box); break;
    case Style::Cone:
    case Style::Cylinder: buildConical(builder, region, box); break;
    }
    return mesh;
}
} // namespace region_detail

// Each command gives one surface. Regions are not wrapped into periodic cells.
inline void computeLammpsRegions(Dataset &data, std::vector<uint8_t> &selected, const std::string &commands, bool selectParticles,
                                 const std::array<float, 3> &color, float transparency, std::atomic<bool> *cancel) {
    using namespace region_detail;
    if (data.sampled()) throw std::runtime_error("Render LAMMPS regions requires full data, not a sampled preview");
    std::vector<Region> regions;
    for (const auto &tokens : splitCommands(commands)) regions.push_back(parseRegion(tokens));
    const Box box = boxOf(data);
    size_t triangles = 0;
    for (const Region &region : regions) {
        auto mesh = buildMesh(region, box);
        triangles += mesh->positions.size() / 3;
        publishSurface(data, std::move(mesh), color, transparency);
    }
    size_t inside = 0;
    if (selectParticles) {
        selected.assign(data.atoms.size(), 0);
        for (size_t i = 0; i < data.atoms.size(); ++i) {
            if ((i & 65535) == 0) surface_detail::throwIfCancelled(cancel);
            const P3 p{data.atoms[i].x, data.atoms[i].y, data.atoms[i].z};
            for (const Region &region : regions)
                if (contains(region, box, p)) { selected[i] = 1; ++inside; break; }
        }
        data.globalAttributes["RenderLammpsRegions.selected"] = double(inside);
    }
    data.globalAttributes["RenderLammpsRegions.count"] = double(regions.size());
    data.globalAttributes["RenderLammpsRegions.triangles"] = double(triangles);
}
