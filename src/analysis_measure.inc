// ---- Measure and partial radial distribution functions -----------------------------
namespace measure_detail {
using Vector = std::array<double, 3>;
inline Vector subtract(const Vector &a, const Vector &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline Vector cross(const Vector &a, const Vector &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline double dot(const Vector &a, const Vector &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline double length(const Vector &a) { return std::sqrt(dot(a, a)); }

// Angle in degrees at a vertex between the incoming and the outgoing segment; NaN when
// either segment has zero length.
inline double vertexAngle(const Vector &incoming, const Vector &outgoing) {
    const Vector back{-incoming[0], -incoming[1], -incoming[2]};
    if (!(length(back) > 0) || !(length(outgoing) > 0)) return std::numeric_limits<double>::quiet_NaN();
    return std::atan2(length(cross(back, outgoing)), dot(back, outgoing)) * 180.0 / 3.14159265358979323846;
}

// Particle numbers separated by spaces or commas.
inline std::vector<int64_t> parsePath(const std::string &text) {
    std::vector<int64_t> numbers;
    size_t cursor = 0;
    while (cursor < text.size()) {
        if (text[cursor] == ' ' || text[cursor] == ',' || text[cursor] == '\t') { ++cursor; continue; }
        size_t end = cursor;
        while (end < text.size() && text[end] != ' ' && text[end] != ',' && text[end] != '\t') ++end;
        int64_t value = 0;
        const auto parsed = std::from_chars(text.data() + cursor, text.data() + end, value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + end)
            throw std::runtime_error("Measure: '" + text.substr(cursor, end - cursor) + "' is not a particle number");
        numbers.push_back(value);
        if (numbers.size() > 100000) throw std::runtime_error("Measure: a path is limited to 100,000 points");
        cursor = end;
    }
    return numbers;
}

inline std::string cell(double value) { return std::isfinite(value) ? formatDataNumber(value) : std::string(); }
} // namespace measure_detail

// Measures the path through the listed particles. Numbers are Particle Identifiers when the
// dataset has them, otherwise positions in the particle list starting at 1. Lengths are in
// the input length unit, angles in degrees and the area in length unit squared.
inline void computeMeasure(Dataset &data, const std::string &pathText, bool minimumImage, std::atomic<bool> *cancel) {
    using namespace measure_detail;
    const std::vector<int64_t> numbers = parsePath(pathText);
    if (numbers.empty()) return;
    if (numbers.size() < 2) throw std::runtime_error("Measure: a path needs at least two points");
    if (cancel && *cancel) throw std::runtime_error("Cancelled");

    const size_t count = data.atoms.size();
    std::vector<uint32_t> path(numbers.size());
    const auto ids = data.scalarProperties.find("Particle Identifier");
    if (ids != data.scalarProperties.end() && ids->second.size() == count) {
        std::unordered_map<int64_t, uint32_t> byIdentifier;
        byIdentifier.reserve(count * 2);
        for (size_t i = 0; i < count; ++i) byIdentifier.emplace(int64_t(ids->second[i]), uint32_t(i));
        for (size_t k = 0; k < numbers.size(); ++k) {
            const auto found = byIdentifier.find(numbers[k]);
            if (found == byIdentifier.end()) throw std::runtime_error("Measure: no particle has identifier " + std::to_string(numbers[k]));
            path[k] = found->second;
        }
    } else {
        for (size_t k = 0; k < numbers.size(); ++k) {
            if (numbers[k] < 1 || uint64_t(numbers[k]) > count) throw std::runtime_error("Measure: there is no particle number " + std::to_string(numbers[k]));
            path[k] = uint32_t(numbers[k] - 1);
        }
    }

    const size_t segments = path.size() - 1;
    std::vector<Bond> links(segments);
    for (size_t k = 0; k < segments; ++k) { links[k].a = path[k]; links[k].b = path[k + 1]; }
    if (minimumImage) external_detail::assignMinimumImages(data, links);
    std::vector<Vector> vectors(segments);
    std::vector<Vector> points(path.size());
    const Atom &start = data.atoms[path[0]];
    points[0] = {double(start.x), double(start.y), double(start.z)};
    double total = 0;
    for (size_t k = 0; k < segments; ++k) {
        const Atom &a = data.atoms[links[k].a], &b = data.atoms[links[k].b];
        Vector vector{double(b.x) - a.x, double(b.y) - a.y, double(b.z) - a.z};
        for (size_t axis = 0; axis < 3; ++axis)
            for (size_t component = 0; component < 3; ++component)
                vector[component] += double(links[k].image[axis]) * data.cell[axis * 3 + component];
        vectors[k] = vector;
        total += length(vector);
        points[k + 1] = {points[k][0] + vector[0], points[k][1] + vector[1], points[k][2] + vector[2]};
    }

    const bool closed = path.size() >= 3 && path.front() == path.back();
    DataTable table;
    table.name = "Measurements";
    table.columns = {"Particle", "Position.X", "Position.Y", "Position.Z", "Vector.X", "Vector.Y", "Vector.Z", "Distance", "Angle"};
    for (size_t row = 0; row < path.size(); ++row) {
        std::vector<std::string> cells{std::to_string(numbers[row]), formatDataNumber(points[row][0]), formatDataNumber(points[row][1]), formatDataNumber(points[row][2])};
        if (row == 0) {
            cells.insert(cells.end(), 5, std::string());
        } else {
            const Vector &in = vectors[row - 1];
            cells.push_back(formatDataNumber(in[0]));
            cells.push_back(formatDataNumber(in[1]));
            cells.push_back(formatDataNumber(in[2]));
            cells.push_back(formatDataNumber(length(in)));
            if (row < segments) cells.push_back(cell(vertexAngle(in, vectors[row])));
            else cells.push_back(closed ? cell(vertexAngle(in, vectors[0])) : std::string());
        }
        table.rows.push_back(std::move(cells));
    }
    data.tables.push_back(std::move(table));
    data.globalAttributes["Measurements.TotalDistance"] = total;
    data.globalAttributes["Measurements.SegmentCount"] = double(segments);
    if (closed) {
        // Newell's area vector: its length is the area projected on the plane that gives
        // the largest projection, so a non-planar outline yields the projected area and
        // the lobes of a self-intersecting outline can cancel.
        const size_t corners = segments;
        Vector center{};
        for (size_t k = 0; k < corners; ++k)
            for (size_t c = 0; c < 3; ++c) center[c] += points[k][c] / double(corners);
        Vector areaVector{};
        for (size_t k = 0; k < corners; ++k) {
            const Vector product = cross(subtract(points[k], center), subtract(points[(k + 1) % corners], center));
            for (size_t c = 0; c < 3; ++c) areaVector[c] += .5 * product[c];
        }
        data.globalAttributes["Measurements.Area"] = length(areaVector);
    }
}

// One g(r) column per unordered type pair: g_ab = pairs(a,b) V / (N_a N_b shell), and
// g_aa = 2 pairs(a,a) V / (N_a^2 shell), so that the sum of c_a c_b g_ab over ordered
// type pairs equals the total g(r) in the "Radial distribution function" table.
inline void computePartialRdf(Dataset &data, double cutoff, size_t bins, const std::vector<uint8_t> *selection, std::atomic<bool> *cancel) {
    const double volume = std::abs(cellDeterminant(data.cell));
    if (!(volume > 0)) throw std::runtime_error("Partial RDFs need a valid simulation cell");
    std::map<uint32_t, uint64_t> population;
    for (size_t i = 0; i < data.atoms.size(); ++i)
        if (!selection || (*selection)[i]) ++population[data.atoms[i].type];
    if (population.size() > 32) throw std::runtime_error("Partial RDFs are limited to 32 particle types");
    std::map<uint32_t, size_t> slot;
    std::vector<uint32_t> types;
    for (const auto &[type, particles] : population) { slot[type] = types.size(); types.push_back(type); }
    const size_t typeCount = types.size();
    const auto pairColumn = [&](size_t a, size_t b) {
        if (a > b) std::swap(a, b);
        return a * typeCount - a * (a + 1) / 2 + b;
    };
    const size_t columns = typeCount * (typeCount + 1) / 2;
    std::vector<uint64_t> histogram(columns * bins);
    forEachNeighborPair(data, cutoff, [&](uint32_t i, uint32_t j, double distanceSquared) {
        if (selection && (!(*selection)[i] || !(*selection)[j])) return;
        const size_t bin = std::min(bins - 1, size_t(std::sqrt(distanceSquared) / cutoff * double(bins)));
        ++histogram[pairColumn(slot.at(data.atoms[i].type), slot.at(data.atoms[j].type)) * bins + bin];
    }, cancel);

    const auto typeName = [&](uint32_t type) {
        return type < data.species.size() ? data.species[type] : "Type " + std::to_string(type);
    };
    DataTable table;
    table.name = "Partial radial distribution functions";
    table.columns = {"r"};
    for (size_t a = 0; a < typeCount; ++a)
        for (size_t b = a; b < typeCount; ++b) table.columns.push_back(typeName(types[a]) + "-" + typeName(types[b]));
    const double width = cutoff / double(bins);
    for (size_t bin = 0; bin < bins; ++bin) {
        const double inner = double(bin) * width, outer = inner + width;
        const double shell = 4.0 / 3.0 * 3.14159265358979323846 * (outer * outer * outer - inner * inner * inner);
        std::vector<std::string> row{std::to_string((double(bin) + .5) * width)};
        for (size_t a = 0; a < typeCount; ++a)
            for (size_t b = a; b < typeCount; ++b) {
                const double na = double(population[types[a]]), nb = double(population[types[b]]);
                const double factor = a == b ? 2.0 : 1.0;
                const double denominator = na * nb * shell;
                row.push_back(std::to_string(denominator > 0 ? factor * double(histogram[pairColumn(a, b) * bins + bin]) * volume / denominator : 0.0));
            }
        table.rows.push_back(std::move(row));
    }
    data.tables.push_back(std::move(table));
}
