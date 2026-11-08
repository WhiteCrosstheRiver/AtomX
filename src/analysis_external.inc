// ---- Nodes that read a second file: Load trajectory and Combine datasets -----------
struct ExternalFrame {
    Dataset data;
    int frameCount = 1;
};
// Installed by structure_io.hpp, which sits above this header. Implementations must be
// callable from pipeline worker threads.
using ExternalFrameReader = std::function<ExternalFrame(const std::string &path, int frame, std::atomic<bool> *cancel)>;
inline ExternalFrameReader &externalFrameReader() {
    static ExternalFrameReader reader;
    return reader;
}
inline constexpr size_t externalParticleLimit = 20000000;

namespace external_detail {
// Bond images follow the new positions: each bond takes the minimum-image convention.
inline void refreshBondImages(Dataset &data) {
    if (data.bonds.empty() || !std::any_of(data.pbc.begin(), data.pbc.end(), [](bool p) { return p; })) return;
    const auto &c = data.cell;
    const double m00 = c[4] * c[8] - c[7] * c[5], m01 = c[6] * c[5] - c[3] * c[8], m02 = c[3] * c[7] - c[6] * c[4];
    const double det = c[0] * m00 + c[1] * m01 + c[2] * m02;
    if (!(std::abs(det) > 0) || !std::isfinite(det)) return;
    const double m10 = c[7] * c[2] - c[1] * c[8], m11 = c[0] * c[8] - c[6] * c[2], m12 = c[6] * c[1] - c[0] * c[7];
    const double m20 = c[1] * c[5] - c[4] * c[2], m21 = c[3] * c[2] - c[0] * c[5], m22 = c[0] * c[4] - c[3] * c[1];
    for (Bond &bond : data.bonds) {
        if (bond.a >= data.atoms.size() || bond.b >= data.atoms.size())
            throw std::runtime_error("Bond references a particle outside the dataset");
        const Atom &a = data.atoms[bond.a], &b = data.atoms[bond.b];
        const double x = double(b.x) - a.x, y = double(b.y) - a.y, z = double(b.z) - a.z;
        const std::array<double, 3> fraction{(m00 * x + m10 * y + m20 * z) / det, (m01 * x + m11 * y + m21 * z) / det,
                                             (m02 * x + m12 * y + m22 * z) / det};
        for (size_t axis = 0; axis < 3; ++axis)
            bond.image[axis] = data.pbc[axis] ? -int32_t(std::lround(fraction[axis])) : 0;
    }
}
inline void requireWholeFile(const Dataset &file, const char *node) {
    if (file.sampled()) throw std::runtime_error(std::string(node) + ": the file exceeds the 20,000,000 particle limit");
}
} // namespace external_detail

// Replaces positions and the simulation cell with those of a trajectory frame and adds or
// updates its other particle properties. Topology (particle types, bonds) stays.
inline void applyLoadedTrajectory(Dataset &data, const Dataset &trajectory, std::atomic<bool> *cancel) {
    external_detail::requireWholeFile(trajectory, "Load trajectory");
    const size_t count = data.atoms.size();
    if (trajectory.atoms.size() != count)
        throw std::runtime_error("Load trajectory: the file has " + std::to_string(trajectory.atoms.size()) +
                                 " particles but the topology has " + std::to_string(count));
    std::vector<uint32_t> mapping(count);
    std::iota(mapping.begin(), mapping.end(), 0u);
    const auto ownIds = data.scalarProperties.find("Particle Identifier");
    const auto fileIds = trajectory.scalarProperties.find("Particle Identifier");
    if (ownIds != data.scalarProperties.end() && fileIds != trajectory.scalarProperties.end() &&
        ownIds->second.size() == count && fileIds->second.size() == count) {
        std::unordered_map<double, uint32_t> byIdentifier;
        byIdentifier.reserve(count * 2);
        for (size_t i = 0; i < count; ++i)
            if (!byIdentifier.emplace(fileIds->second[i], uint32_t(i)).second)
                throw std::runtime_error("Load trajectory: the file repeats a Particle Identifier");
        for (size_t i = 0; i < count; ++i) {
            if ((i & 65535) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
            const auto found = byIdentifier.find(ownIds->second[i]);
            if (found == byIdentifier.end())
                throw std::runtime_error("Load trajectory: Particle Identifier " + std::to_string(int64_t(ownIds->second[i])) +
                                         " is missing from the file");
            mapping[i] = found->second;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        const Atom &moved = trajectory.atoms[mapping[i]];
        data.atoms[i].x = moved.x;
        data.atoms[i].y = moved.y;
        data.atoms[i].z = moved.z;
    }
    for (const auto &[name, values] : trajectory.scalarProperties) {
        if (name == "Particle Identifier" || name == "Particle Type" || values.size() != count) continue;
        auto &output = data.scalarProperties[name];
        output.resize(count);
        for (size_t i = 0; i < count; ++i) output[i] = values[mapping[i]];
    }
    for (const auto &[name, values] : trajectory.vectorProperties) {
        if (values.size() != count) continue;
        auto &output = data.vectorProperties[name];
        output.resize(count);
        for (size_t i = 0; i < count; ++i) output[i] = values[mapping[i]];
        if (const auto components = trajectory.propertyComponents.find(name); components != trajectory.propertyComponents.end())
            data.propertyComponents[name] = components->second;
    }
    for (const auto &[name, value] : trajectory.globalAttributes) data.globalAttributes[name] = value;
    data.cell = trajectory.cell;
    data.origin = trajectory.origin;
    data.pbc = trajectory.pbc;
    external_detail::refreshBondImages(data);
    data.bounds();
}

// Appends the particles of a second dataset. The primary simulation cell is kept.
inline void combineDatasets(PipelineResult &r, const Dataset &other, std::atomic<bool> *cancel) {
    external_detail::requireWholeFile(other, "Combine datasets");
    Dataset &data = r.data;
    const size_t first = data.atoms.size(), added = other.atoms.size(), total = first + added;
    if (total > externalParticleLimit) throw std::runtime_error("Combine datasets: the result exceeds the 20,000,000 particle limit");
    if (cancel && *cancel) throw std::runtime_error("Cancelled");
    if (r.selected.size() != first || r.colorSelected.size() != first)
        throw std::runtime_error("Selection length does not match particle count");

    std::vector<uint32_t> typeMap(other.species.size());
    for (size_t t = 0; t < other.species.size(); ++t) {
        const auto found = std::find(data.species.begin(), data.species.end(), other.species[t]);
        typeMap[t] = uint32_t(found - data.species.begin());
        if (found == data.species.end()) data.species.push_back(other.species[t]);
    }
    data.atoms.reserve(total);
    for (const Atom &atom : other.atoms) {
        if (atom.type >= typeMap.size()) throw std::runtime_error("Combine datasets: the file has a particle type out of range");
        data.atoms.push_back({atom.x, atom.y, atom.z, typeMap[atom.type]});
    }

    const auto mergeScalars = [&](const std::string &name, const std::vector<double> *values) {
        auto &output = data.scalarProperties[name];
        output.resize(first, 0.0);
        output.resize(total, 0.0);
        if (values && values->size() == added) std::copy(values->begin(), values->end(), output.begin() + std::ptrdiff_t(first));
    };
    const auto mergeVectors = [&](const std::string &name, const std::vector<Vec3> *values) {
        auto &output = data.vectorProperties[name];
        output.resize(first, Vec3{});
        output.resize(total, Vec3{});
        if (values && values->size() == added) std::copy(values->begin(), values->end(), output.begin() + std::ptrdiff_t(first));
    };
    std::vector<std::string> scalarNames, vectorNames;
    for (const auto &[name, values] : data.scalarProperties) scalarNames.push_back(name);
    for (const auto &[name, values] : other.scalarProperties)
        if (!data.scalarProperties.contains(name)) scalarNames.push_back(name);
    for (const auto &[name, values] : data.vectorProperties) vectorNames.push_back(name);
    for (const auto &[name, values] : other.vectorProperties)
        if (!data.vectorProperties.contains(name)) vectorNames.push_back(name);

    // New identifiers keep added particles and molecules from colliding with existing ones.
    const auto maxOf = [&](const char *name) {
        const auto found = data.scalarProperties.find(name);
        double maximum = 0;
        if (found != data.scalarProperties.end())
            for (size_t i = 0; i < std::min(first, found->second.size()); ++i) maximum = std::max(maximum, found->second[i]);
        return maximum;
    };
    const bool hadIds = data.scalarProperties.contains("Particle Identifier");
    const double idBase = maxOf("Particle Identifier"), moleculeBase = maxOf("Molecule Identifier");
    for (const auto &name : scalarNames) {
        const auto found = other.scalarProperties.find(name);
        mergeScalars(name, found == other.scalarProperties.end() ? nullptr : &found->second);
    }
    for (const auto &name : vectorNames) {
        const auto found = other.vectorProperties.find(name);
        mergeVectors(name, found == other.vectorProperties.end() ? nullptr : &found->second);
    }
    if (auto ids = data.scalarProperties.find("Particle Identifier"); ids != data.scalarProperties.end()) {
        double base = idBase;
        if (!hadIds) {
            for (size_t i = 0; i < first; ++i) ids->second[i] = double(i + 1);
            base = double(first);
        }
        for (size_t i = 0; i < added; ++i) ids->second[first + i] = base + double(i + 1);
    }
    if (auto molecules = data.scalarProperties.find("Molecule Identifier"); molecules != data.scalarProperties.end())
        for (size_t i = 0; i < added; ++i) molecules->second[first + i] += moleculeBase;
    for (const auto &[name, components] : other.propertyComponents) data.propertyComponents.emplace(name, components);

    if (!data.particleColors.empty() || !other.particleColors.empty()) {
        data.particleColors.resize(first, Vec3{-1, -1, -1});
        if (other.particleColors.size() == added)
            data.particleColors.insert(data.particleColors.end(), other.particleColors.begin(), other.particleColors.end());
        else
            data.particleColors.resize(total, Vec3{-1, -1, -1});
    }
    for (Bond bond : other.bonds) {
        if (bond.a >= added || bond.b >= added) throw std::runtime_error("Combine datasets: a bond in the file references a missing particle");
        bond.a += uint32_t(first);
        bond.b += uint32_t(first);
        data.bonds.push_back(bond);
    }
    r.selected.resize(total, 0);
    r.colorSelected.resize(total, 1);
    data.sourceCount += added;
    data.globalAttributes["CombineDatasets.added_count"] = double(added);
    data.bounds();
}
