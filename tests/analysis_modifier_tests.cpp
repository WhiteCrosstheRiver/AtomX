// Numerical checks for the OVITO-parity analysis modifiers. Expected values are
// published reference numbers or closed-form results, never AtomX's own output.
#include "../src/core.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <random>

using namespace atomx;
static int failures = 0, checks = 0;
static void check(bool ok, const std::string &what) {
    ++checks;
    if (!ok) { std::cerr << "FAIL: " << what << '\n'; ++failures; }
}
static void near(double value, double expected, double tolerance, const std::string &what) {
    check(std::abs(value - expected) <= tolerance,
          what + ": got " + std::to_string(value) + ", expected " + std::to_string(expected));
}
template <class F> static void throwsWith(F &&call, const std::string &text, const std::string &what) {
    try { call(); check(false, what + ": no error raised"); }
    catch (const std::exception &e) {
        check(std::string(e.what()).find(text) != std::string::npos, what + ": wrong error: " + e.what());
    }
}

static Dataset crystal(std::vector<std::array<double, 3>> basis, std::array<double, 3> cellSize, int n) {
    Dataset d;
    d.species = {"X"};
    d.pbc = {true, true, true};
    d.cell = {cellSize[0] * n, 0, 0, 0, cellSize[1] * n, 0, 0, 0, cellSize[2] * n};
    for (int z = 0; z < n; ++z) for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x)
        for (const auto &b : basis)
            d.atoms.push_back({float((x + b[0]) * cellSize[0]), float((y + b[1]) * cellSize[1]),
                               float((z + b[2]) * cellSize[2]), 0});
    return d;
}
static Dataset fcc(int n, double a = 3.6) {
    return crystal({{0, 0, 0}, {0, .5, .5}, {.5, 0, .5}, {.5, .5, 0}}, {a, a, a}, n);
}
static Dataset bcc(int n, double a = 3.0) { return crystal({{0, 0, 0}, {.5, .5, .5}}, {a, a, a}, n); }
static Dataset simpleCubic(int n, double a = 3.0) { return crystal({{0, 0, 0}}, {a, a, a}, n); }
static Dataset hcp(int n, double a = 3.0) {
    // Ideal c/a, as a 4-atom orthorhombic cell.
    const double b = a * std::sqrt(3.0), c = a * std::sqrt(8.0 / 3.0);
    return crystal({{0, 0, 0}, {.5, .5, 0}, {.5, 1.0 / 6, .5}, {0, 2.0 / 3, .5}}, {a, b, c}, n);
}
// One center with the 12 vertices of a regular icosahedron, without periodicity.
static Dataset icosahedron() {
    Dataset d;
    d.species = {"X"};
    const double t = (1 + std::sqrt(5.0)) / 2, s = 2.55 / std::sqrt(1 + t * t);
    d.atoms.push_back({0, 0, 0, 0});
    for (int a : {-1, 1}) for (int b : {-1, 1}) {
        d.atoms.push_back({0, float(a * s), float(b * t * s), 0});
        d.atoms.push_back({float(a * s), float(b * t * s), 0, 0});
        d.atoms.push_back({float(b * t * s), 0, float(a * s), 0});
    }
    return d;
}
static std::array<double, 2> steinhardtOf(Dataset d, int neighbors, size_t particle = 0) {
    computeSteinhardt(d, neighbors, nullptr);
    return {d.scalarProperties.at("q4")[particle], d.scalarProperties.at("q6")[particle]};
}
static double attribute(const Dataset &d, const std::string &name) {
    const auto found = d.globalAttributes.find(name);
    check(found != d.globalAttributes.end(), "missing global attribute " + name);
    return found == d.globalAttributes.end() ? 0 : found->second;
}
static void transform(Dataset &d, const double f[3][3]) {
    auto apply = [&](double x, double y, double z, double out[3]) {
        for (int a = 0; a < 3; ++a) out[a] = f[a][0] * x + f[a][1] * y + f[a][2] * z;
    };
    for (auto &atom : d.atoms) {
        double out[3];
        apply(atom.x, atom.y, atom.z, out);
        atom.x = float(out[0]); atom.y = float(out[1]); atom.z = float(out[2]);
    }
    const auto cell = d.cell;
    for (int axis = 0; axis < 3; ++axis) {
        double out[3];
        apply(cell[axis * 3], cell[axis * 3 + 1], cell[axis * 3 + 2], out);
        for (int c = 0; c < 3; ++c) d.cell[axis * 3 + c] = out[c];
    }
}
static PipelineResult resultOf(Dataset d) {
    const size_t count = d.atoms.size();
    return {std::move(d), std::vector<uint8_t>(count), std::vector<uint8_t>(count, 1)};
}

static void testNeighbors() {
    const auto d = fcc(4);
    const auto lists = nearestNeighborVectors(d, 12);
    const double first = 3.6 / std::sqrt(2.0);
    bool ok = lists.size() == d.atoms.size();
    for (const auto &list : lists) {
        ok = ok && list.size() == 12;
        for (const auto &n : list) {
            ok = ok && std::abs(std::sqrt(n.distanceSquared) - first) < 1e-4;
            ok = ok && std::abs(std::hypot(n.vector[0], n.vector[1], n.vector[2]) - first) < 1e-4;
        }
    }
    check(ok, "FCC has exactly 12 neighbors at a/sqrt(2), across periodic boundaries");
    // Each vector must land on the actual neighbor position, images included.
    bool consistent = true;
    for (size_t i = 0; i < lists.size(); ++i)
        for (const auto &n : lists[i])
            for (int axis = 0; axis < 3; ++axis) {
                const double target = coordinate(d.atoms[i], axis) + n.vector[size_t(axis)];
                const double offset = target - coordinate(d.atoms[n.index], axis);
                const double length = d.cell[size_t(axis) * 4];
                consistent = consistent && std::abs(offset - std::round(offset / length) * length) < 1e-4;
            }
    check(consistent, "neighbor vectors point at the neighbor's periodic image");
    Dataset pair; pair.species = {"X"}; pair.atoms = {{0, 0, 0, 0}, {1, 0, 0, 0}};
    check(nearestNeighborVectors(pair, 12)[0].size() == 1, "an isolated pair returns the one neighbor it has");
    Dataset single; single.species = {"X"}; single.atoms = {{0, 0, 0, 0}};
    check(nearestNeighborVectors(single, 12)[0].empty(), "a single particle has no neighbors");
    check(nearestNeighborVectors(Dataset{}, 12).empty(), "an empty dataset has no neighbor lists");
}

static void testSteinhardt() {
    // Published values: Steinhardt et al. (1983); Mickel et al., J. Chem. Phys. 138, 044501 (2013).
    constexpr double tolerance = 2e-4;
    auto f = steinhardtOf(fcc(4), 12);
    near(f[0], .19094, tolerance, "FCC q4"); near(f[1], .57452, tolerance, "FCC q6");
    auto h = steinhardtOf(hcp(3), 12);
    near(h[0], .09722, tolerance, "HCP q4"); near(h[1], .48476, tolerance, "HCP q6");
    auto b8 = steinhardtOf(bcc(5), 8);
    near(b8[0], .50918, tolerance, "BCC-8 q4"); near(b8[1], .62854, tolerance, "BCC-8 q6");
    auto b14 = steinhardtOf(bcc(5), 14);
    near(b14[0], .03637, tolerance, "BCC-14 q4"); near(b14[1], .51069, tolerance, "BCC-14 q6");
    auto s = steinhardtOf(simpleCubic(5), 6);
    near(s[0], .76376, tolerance, "SC q4"); near(s[1], .35355, tolerance, "SC q6");
    auto ico = steinhardtOf(icosahedron(), 12, 0);
    near(ico[0], 0, tolerance, "icosahedral q4 vanishes"); near(ico[1], .66332, tolerance, "icosahedral q6");

    // q_l is rotationally invariant, and independent of the lattice constant.
    Dataset rotated = fcc(4);
    const double c = std::cos(.7), si = std::sin(.7), k = std::cos(.4), q = std::sin(.4);
    const double rotation[3][3]{{c * k, -si, c * q}, {si * k, c, si * q}, {-q, 0, k}};
    transform(rotated, rotation);
    auto r = steinhardtOf(rotated, 12);
    near(r[0], f[0], 2e-4, "q4 is rotation invariant in a triclinic cell");
    near(r[1], f[1], 2e-4, "q6 is rotation invariant in a triclinic cell");
    auto scaled = steinhardtOf(fcc(4, 5.0), 12);
    near(scaled[1], f[1], 1e-5, "q6 is independent of the lattice constant");

    Dataset d = fcc(3);
    computeSteinhardt(d, 12, nullptr);
    near(attribute(d, "SteinhardtOrderParameters.mean_q6"), .57452, tolerance, "mean q6 of a perfect crystal");
    near(attribute(d, "SteinhardtOrderParameters.undercoordinated_particles"), 0, 0, "no undercoordinated particles");
    Dataset few = icosahedron(); few.atoms.resize(3);
    computeSteinhardt(few, 12, nullptr);
    near(attribute(few, "SteinhardtOrderParameters.undercoordinated_particles"), 3, 0, "short lists are counted");
    Dataset none;
    computeSteinhardt(none, 12, nullptr);
    check(none.scalarProperties.at("q6").empty(), "an empty dataset gives empty properties");
    Dataset bad = fcc(3);
    throwsWith([&] { computeSteinhardt(bad, 0, nullptr); }, "1 to 64", "zero neighbors is rejected");
    throwsWith([&] { computeSteinhardt(bad, 65, nullptr); }, "1 to 64", "65 neighbors is rejected");
}

static uint8_t acklandOf(Dataset d, size_t particle = 0) {
    computeAcklandJones(d, nullptr);
    return uint8_t(d.scalarProperties.at("Structure Type")[particle]);
}
static void testAcklandJones() {
    constexpr uint8_t Other = 0, FCC = 1, HCP = 2, BCC = 3, ICO = 4;
    for (auto [name, data, expected] : {std::tuple{"FCC", fcc(4), FCC}, std::tuple{"HCP", hcp(3), HCP},
                                        std::tuple{"BCC", bcc(5), BCC}}) {
        Dataset d = data;
        computeAcklandJones(d, nullptr);
        const auto &types = d.scalarProperties.at("Structure Type");
        check(std::all_of(types.begin(), types.end(), [e = expected](double t) { return t == e; }),
              std::string("every particle of ideal ") + name + " is classified correctly");
        near(attribute(d, std::string("AcklandJones.counts.") + name), double(d.atoms.size()), 0,
             std::string(name) + " count");
        near(attribute(d, "AcklandJones.counts.Other"), 0, 0, std::string(name) + " has no Other");
    }
    check(acklandOf(icosahedron(), 0) == ICO, "the icosahedron center is Icosahedral");
    check(acklandOf(simpleCubic(5)) == Other, "simple cubic is Other");

    // Thermal noise far below the bin widths must not change the class.
    for (auto [name, base, expected] : {std::tuple{"FCC", fcc(5), FCC}, std::tuple{"BCC", bcc(6), BCC}}) {
        Dataset d = base;
        std::mt19937 random(7);
        std::normal_distribution<float> noise(0.f, .05f);
        for (auto &a : d.atoms) { a.x += noise(random); a.y += noise(random); a.z += noise(random); }
        computeAcklandJones(d, nullptr);
        const auto &types = d.scalarProperties.at("Structure Type");
        const auto hits = std::count(types.begin(), types.end(), double(expected));
        check(hits > long(.9 * double(types.size())), std::string("thermal ") + name + " stays above 90%: " +
              std::to_string(hits) + " of " + std::to_string(types.size()));
    }
    Dataset d = fcc(4);
    computeAcklandJones(d, nullptr);
    check(d.tables.size() == 1 && d.tables[0].rows.size() == 5, "a five-row structure table is published");
    Dataset few = icosahedron(); few.atoms.resize(4);
    check(acklandOf(few) == Other, "too few neighbors is Other, not an error");
    Dataset empty;
    computeAcklandJones(empty, nullptr);
    check(empty.scalarProperties.at("Structure Type").empty(), "an empty dataset is accepted");
}

static void testAtomicStrain() {
    const Dataset reference = fcc(4);
    constexpr double cutoff = 3.0, tolerance = 2e-5;
    auto strained = [&](const double f[3][3]) {
        Dataset d = reference;
        transform(d, f);
        computeAtomicStrain(d, reference, cutoff, nullptr);
        return d;
    };
    const double identity[3][3]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Dataset same = strained(identity);
    near(same.scalarProperties.at("Shear Strain")[5], 0, 1e-6, "undeformed shear strain");
    near(same.scalarProperties.at("Volumetric Strain")[5], 0, 1e-6, "undeformed volumetric strain");
    near(same.scalarProperties.at("Nonaffine Squared Displacement")[5], 0, 1e-6, "undeformed D2min");
    near(attribute(same, "AtomicStrain.invalid_particle_count"), 0, 0, "a crystal has no invalid particles");

    const double e = .02, uniaxial[3][3]{{1 + e, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Dataset stretch = strained(uniaxial);
    const double exx = ((1 + e) * (1 + e) - 1) / 2;
    near(stretch.scalarProperties.at("Strain Tensor.XX")[7], exx, tolerance, "uniaxial Exx");
    near(stretch.scalarProperties.at("Strain Tensor.YY")[7], 0, tolerance, "uniaxial Eyy");
    near(stretch.scalarProperties.at("Volumetric Strain")[7], exx / 3, tolerance, "uniaxial volumetric strain");
    near(stretch.scalarProperties.at("Shear Strain")[7], exx / std::sqrt(3.0), tolerance, "uniaxial von Mises shear");
    near(stretch.scalarProperties.at("Nonaffine Squared Displacement")[7], 0, 1e-6, "an affine stretch has no D2min");

    const double g = .03, shearing[3][3]{{1, g, 0}, {0, 1, 0}, {0, 0, 1}};
    Dataset sheared = strained(shearing);
    near(sheared.scalarProperties.at("Strain Tensor.XY")[3], g / 2, tolerance, "simple shear Exy in a tilted cell");
    near(sheared.scalarProperties.at("Strain Tensor.YY")[3], g * g / 2, tolerance, "simple shear second-order Eyy");
    near(sheared.scalarProperties.at("Strain Tensor.XX")[3], 0, tolerance, "simple shear Exx");
    const double shearValue = std::sqrt(g * g / 4 + 2 * std::pow(g * g / 2, 2) / 6);
    near(sheared.scalarProperties.at("Shear Strain")[3], shearValue, tolerance, "simple shear invariant");

    const double h = .01, hydrostatic[3][3]{{1 + h, 0, 0}, {0, 1 + h, 0}, {0, 0, 1 + h}};
    Dataset expanded = strained(hydrostatic);
    near(expanded.scalarProperties.at("Shear Strain")[9], 0, tolerance, "hydrostatic expansion has no shear");
    near(expanded.scalarProperties.at("Volumetric Strain")[9], ((1 + h) * (1 + h) - 1) / 2, tolerance,
         "hydrostatic volumetric strain");

    // Rigid rotation is strain-free. Only the deformed copy is rotated, so no periodicity.
    Dataset cluster = fcc(4);
    cluster.pbc = {false, false, false};
    Dataset rotated = cluster;
    const double c = std::cos(.5), s = std::sin(.5), rotation[3][3]{{c, -s, 0}, {s, c, 0}, {0, 0, 1}};
    transform(rotated, rotation);
    computeAtomicStrain(rotated, cluster, cutoff, nullptr);
    const auto &rigid = rotated.scalarProperties.at("Shear Strain");
    check(*std::max_element(rigid.begin(), rigid.end()) < 2e-5, "rigid rotation produces no shear strain");

    // A local displacement is non-affine near the defect and invisible far away.
    Dataset local = reference;
    local.atoms[0].x += .2f;
    computeAtomicStrain(local, reference, cutoff, nullptr);
    const auto &residual = local.scalarProperties.at("Nonaffine Squared Displacement");
    check(residual[0] > 1e-3, "the displaced particle has a large D2min");
    const size_t far = size_t(std::distance(reference.atoms.begin(), std::find_if(reference.atoms.begin(),
        reference.atoms.end(), [](const Atom &a) { return a.x > 6 && a.y > 6 && a.z > 6 && a.x < 8; })));
    near(residual[far], 0, 1e-6, "a distant particle is unaffected");

    // A particle crossing the boundary is the same neighbor under minimum image.
    Dataset wrapped = reference;
    for (auto &a : wrapped.atoms) if (a.x > 7.2f) a.x -= float(reference.cell[0]);
    computeAtomicStrain(wrapped, reference, cutoff, nullptr);
    near(wrapped.scalarProperties.at("Shear Strain")[11], 0, 1e-5, "wrapping does not create strain");

    // Permuted particles match through Particle Identifier when counts differ.
    Dataset withIds = reference;
    withIds.scalarProperties["Particle Identifier"].resize(reference.atoms.size());
    std::iota(withIds.scalarProperties["Particle Identifier"].begin(),
              withIds.scalarProperties["Particle Identifier"].end(), 1.0);
    Dataset fewer = withIds;
    fewer.atoms.pop_back();
    fewer.scalarProperties["Particle Identifier"].pop_back();
    std::swap(fewer.atoms[0], fewer.atoms[1]);
    std::swap(fewer.scalarProperties["Particle Identifier"][0], fewer.scalarProperties["Particle Identifier"][1]);
    computeAtomicStrain(fewer, withIds, cutoff, nullptr);
    near(fewer.scalarProperties.at("Shear Strain")[0], 0, 1e-6, "identifier matching keeps strain zero");

    Dataset pair; pair.species = {"X"}; pair.atoms = {{0, 0, 0, 0}, {2, 0, 0, 0}};
    Dataset pairCopy = pair;
    computeAtomicStrain(pair, pairCopy, cutoff, nullptr);
    near(attribute(pair, "AtomicStrain.invalid_particle_count"), 2, 0, "a dimer cannot define a 3D deformation");
    near(pair.scalarProperties.at("Shear Strain")[0], 0, 0, "invalid particles report zero strain");
    Dataset sheet;
    sheet.species = {"X"};
    for (int y = 0; y < 5; ++y) for (int x = 0; x < 5; ++x) sheet.atoms.push_back({float(x), float(y), 0, 0});
    Dataset sheetCopy = sheet;
    computeAtomicStrain(sheet, sheetCopy, 1.6, nullptr);
    near(attribute(sheet, "AtomicStrain.invalid_particle_count"), 25, 0, "coplanar neighbors are invalid");

    Dataset mismatch = reference;
    mismatch.atoms.pop_back();
    throwsWith([&] { computeAtomicStrain(mismatch, reference, cutoff, nullptr); },
               "do not match", "a count mismatch without identifiers is rejected");
    Dataset badCutoff = reference;
    throwsWith([&] { computeAtomicStrain(badCutoff, reference, 0, nullptr); }, "Cutoff", "zero cutoff is rejected");
    Dataset sampled = reference;
    sampled.stride = 2;
    sampled.sourceCount = sampled.atoms.size() * 2;
    throwsWith([&] { computeAtomicStrain(sampled, reference, cutoff, nullptr); }, "unsampled", "sampled data is rejected");
}

static void testWignerSeitz() {
    const Dataset sites = fcc(4);
    auto run = [&](Dataset d, bool outputSites = false) {
        auto r = resultOf(std::move(d));
        computeWignerSeitz(r, sites, outputSites, nullptr);
        return r;
    };
    auto counts = [&](const PipelineResult &r) {
        return std::array<double, 2>{attribute(r.data, "WignerSeitz.vacancy_count"),
                                     attribute(r.data, "WignerSeitz.interstitial_count")};
    };
    const auto perfect = counts(run(sites));
    near(perfect[0], 0, 0, "a perfect crystal has no vacancies");
    near(perfect[1], 0, 0, "a perfect crystal has no interstitials");

    Dataset missing = sites;
    missing.atoms.erase(missing.atoms.begin() + 17);
    const auto vacancy = counts(run(missing));
    near(vacancy[0], 1, 0, "one removed atom is one vacancy");
    near(vacancy[1], 0, 0, "removal adds no interstitial");

    Dataset extra = sites;
    extra.atoms.push_back({1.8f, 1.8f, 1.8f, 0}); // The octahedral void, nearest to a corner-free site.
    const auto interstitial = counts(run(extra));
    near(interstitial[0], 0, 0, "an added atom creates no vacancy");
    near(interstitial[1], 1, 0, "an added atom is one interstitial");

    Dataset frenkel = sites;
    frenkel.atoms[0].x += 1.75f; // Moved next to another site: a Frenkel pair.
    frenkel.atoms[0].y += 1.75f;
    const auto pair = counts(run(frenkel));
    near(pair[0], 1, 0, "a Frenkel pair has one vacancy");
    near(pair[1], 1, 0, "a Frenkel pair has one interstitial");

    Dataset thermal = sites;
    std::mt19937 random(11);
    std::normal_distribution<float> noise(0.f, .08f);
    for (auto &a : thermal.atoms) { a.x += noise(random); a.y += noise(random); a.z += noise(random); }
    const auto vibrating = counts(run(thermal));
    near(vibrating[0] + vibrating[1], 0, 0, "thermal vibration creates no defects");

    Dataset shifted = sites;
    for (auto &a : shifted.atoms) a.x -= float(sites.cell[0]); // One full period outside the cell.
    const auto outside = counts(run(shifted));
    near(outside[0] + outside[1], 0, 0, "unwrapped coordinates map through the periodic boundary");

    auto bySite = run(missing, true);
    check(bySite.data.atoms.size() == sites.atoms.size(), "site output shows every reference site");
    check(bySite.selected.size() == sites.atoms.size() && bySite.colorSelected.size() == sites.atoms.size(),
          "site output resizes the selection state");
    const auto &occupancy = bySite.data.scalarProperties.at("Occupancy");
    near(std::accumulate(occupancy.begin(), occupancy.end(), 0.0), double(missing.atoms.size()), 0,
         "occupancies sum to the particle count");
    near(occupancy[17], 0, 0, "the removed atom's site is empty");
    auto byParticle = run(missing, false);
    check(byParticle.data.atoms.size() == missing.atoms.size(), "particle output keeps the particles");
    near(byParticle.data.scalarProperties.at("Site Index")[20], 21, 0, "site indices follow the matching lattice site");
    check(byParticle.data.tables.size() == 1 && byParticle.data.tables[0].rows.size() == 2,
          "occupancy table lists the values 0 and 1");

    Dataset noSites;
    Dataset any = sites;
    auto r = resultOf(any);
    throwsWith([&] { computeWignerSeitz(r, noSites, false, nullptr); }, "at least one reference site", "no sites is rejected");
}

static void testCenterOfMass() {
    Dataset reference = fcc(3);
    Dataset moved = reference;
    for (auto &a : moved.atoms) { a.x += 1.5f; a.y -= .5f; a.z += .25f; }
    auto r = resultOf(moved);
    correctCenterOfMass(r, reference, false, nullptr);
    bool restored = true;
    for (size_t i = 0; i < reference.atoms.size(); ++i)
        restored = restored && std::abs(r.data.atoms[i].x - reference.atoms[i].x) < 1e-4 &&
                   std::abs(r.data.atoms[i].y - reference.atoms[i].y) < 1e-4 &&
                   std::abs(r.data.atoms[i].z - reference.atoms[i].z) < 1e-4;
    check(restored, "a uniform drift is removed");
    const auto expected = centerOfMass(reference, nullptr);
    near(attribute(r.data, "CenterOfMassCorrection.reference.X"), expected[0], 1e-6, "reference center is reported");
    near(attribute(r.data, "CenterOfMassCorrection.current.X"), expected[0] + 1.5, 1e-4, "current center is reported");

    auto same = resultOf(reference);
    correctCenterOfMass(same, reference, false, nullptr);
    near(same.data.atoms[3].x, reference.atoms[3].x, 1e-6, "at the reference frame nothing moves");

    Dataset heavy;
    heavy.species = {"X"};
    heavy.atoms = {{0, 0, 0, 0}, {4, 0, 0, 0}};
    heavy.scalarProperties["Mass"] = {3, 1};
    near(centerOfMass(heavy, nullptr)[0], 1, 1e-9, "mass weighting");
    heavy.scalarProperties["Mass"] = {0, 0};
    near(centerOfMass(heavy, nullptr)[0], 2, 1e-9, "zero masses fall back to equal weights");

    // Selected-only: the group's drift sets the shift applied to every particle.
    Dataset two;
    two.species = {"X"};
    two.atoms = {{0, 0, 0, 0}, {10, 0, 0, 0}};
    Dataset twoMoved = two;
    twoMoved.atoms[0].x = 2;
    twoMoved.atoms[1].x = 15;
    auto selected = resultOf(twoMoved);
    selected.selected = {1, 0};
    correctCenterOfMass(selected, two, true, nullptr);
    near(selected.data.atoms[0].x, 0, 1e-6, "the selected particle returns to its reference");
    near(selected.data.atoms[1].x, 13, 1e-6, "other particles get the same shift");

    auto nothing = resultOf(two);
    throwsWith([&] { correctCenterOfMass(nothing, two, true, nullptr); }, "no particles", "an empty selection is rejected");
    auto empty = resultOf(Dataset{});
    throwsWith([&] { correctCenterOfMass(empty, Dataset{}, false, nullptr); }, "no particles", "empty data is rejected");
}

static void testShrinkWrap() {
    Dataset d;
    d.species = {"X"};
    d.cell = {100, 0, 0, 0, 100, 0, 0, 0, 100};
    d.pbc = {true, false, true};
    d.atoms = {{1, 2, 3, 0}, {4, -2, 5, 0}, {2, 0, 9, 0}};
    shrinkWrapCell(d);
    near(d.origin.x, 1, 1e-6, "origin x"); near(d.origin.y, -2, 1e-6, "origin y"); near(d.origin.z, 3, 1e-6, "origin z");
    near(d.cell[0], 3, 1e-6, "extent x"); near(d.cell[4], 4, 1e-6, "extent y"); near(d.cell[8], 6, 1e-6, "extent z");
    near(d.cell[1], 0, 0, "the wrapped cell is axis aligned");
    check(d.pbc[0] && !d.pbc[1] && d.pbc[2], "periodic flags are unchanged");
    Dataset flat;
    flat.species = {"X"};
    flat.atoms = {{0, 0, 0, 0}, {2, 3, 0, 0}};
    shrinkWrapCell(flat);
    near(flat.cell[8], 1, 0, "a flat axis stays invertible");
    check(std::abs(cellDeterminant(flat.cell)) > 0, "the cell is invertible");
    Dataset triclinic = fcc(2);
    const double tilt[3][3]{{1, .3, 0}, {0, 1, 0}, {0, 0, 1}};
    transform(triclinic, tilt);
    shrinkWrapCell(triclinic);
    near(triclinic.cell[3], 0, 0, "a tilted cell becomes orthogonal");
    Dataset empty;
    throwsWith([&] { shrinkWrapCell(empty); }, "at least one particle", "empty data is rejected");
    Dataset broken;
    broken.atoms = {{std::nanf(""), 0, 0, 0}};
    throwsWith([&] { shrinkWrapCell(broken); }, "non-finite", "non-finite coordinates are rejected");
}

static void testPipeline() {
    // The real pipeline evaluator: nodes compose, order matters, and downstream sees outputs.
    Dataset d = fcc(4);
    const Dataset reference = fcc(4);
    Dataset deformed = d;
    const double stretch[3][3]{{1.02, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    transform(deformed, stretch);
    Modifier strain{Op::AtomicStrain};
    strain.value = 3;
    Modifier steinhardt{Op::SteinhardtOrderParameters};
    Modifier ackland{Op::AcklandJonesAnalysis};
    Modifier select{Op::ExpressionSelect};
    select.property = "q6 > 0.5";
    std::vector<Modifier> stack{strain, steinhardt, ackland, select};
    std::function<const Dataset &(size_t)> provider = [&](size_t) -> const Dataset & { return reference; };
    auto evaluated = evaluateFrom(resultOf(deformed), stack, 0, nullptr, nullptr, SIZE_MAX, {}, provider);
    check(evaluated.data.scalarProperties.count("Shear Strain") == 1, "strain output survives the pipeline");
    check(std::count(evaluated.selected.begin(), evaluated.selected.end(), 1) == long(d.atoms.size()),
          "Expression selection reads q6 from an earlier node");
    near(attribute(evaluated.data, "AcklandJones.counts.FCC"), double(d.atoms.size()), 0,
         "a 2 percent stretch stays FCC in the pipeline");

    throwsWith([&] {
        std::vector<Modifier> needsReference{strain};
        (void)evaluateFrom(resultOf(deformed), needsReference, 0);
    }, "no reference configuration provider", "a missing reference is reported at the node");

    // Wigner-Seitz site output replaces the particles; later nodes must see the new count.
    Dataset missing = reference;
    missing.atoms.pop_back();
    Modifier sites{Op::WignerSeitzAnalysis};
    sites.adaptive = true;
    Modifier empties{Op::ExpressionSelect};
    empties.property = "Occupancy == 0";
    std::vector<Modifier> defectStack{sites, empties};
    auto defects = evaluateFrom(resultOf(missing), defectStack, 0, nullptr, nullptr, SIZE_MAX, {}, provider);
    check(defects.data.atoms.size() == reference.atoms.size(), "site output flows downstream");
    check(std::count(defects.selected.begin(), defects.selected.end(), 1) == 1, "the vacancy is selectable by Occupancy");

    // Shrink-wrap updates the cell that the following node reads.
    std::vector<Modifier> wrapStack{Modifier{Op::ShrinkWrapBox}, Modifier{Op::Delete}};
    auto wrapped = evaluateFrom(resultOf(fcc(2)), wrapStack, 0);
    near(wrapped.data.cell[0], 3.6 * 2 - 1.8, 1e-5, "shrink-wrap publishes the new cell");

    // Schema: downstream selectors must offer what each node publishes, and earlier nodes must not.
    std::vector<Modifier> schema{Modifier{Op::SteinhardtOrderParameters}, Modifier{Op::AtomicStrain}, Modifier{Op::Histogram}};
    const auto before = pipelineInputPropertyChoices(d, schema, 0), after = pipelineInputPropertyChoices(d, schema, 2);
    auto has = [](const std::vector<std::string> &list, const char *name) {
        return std::find(list.begin(), list.end(), name) != list.end();
    };
    check(!has(before, "q6") && has(after, "q6") && has(after, "Shear Strain"), "property choices follow pipeline order");
    Modifier siteNode{Op::WignerSeitzAnalysis};
    check(pipelineInputPropertyChoices(d, std::vector<Modifier>{siteNode}, 1).size() ==
          pipelineInputPropertyChoices(d, std::vector<Modifier>{}, 0).size() + 2, "particle mode publishes two properties");

    for (Op op : {Op::SteinhardtOrderParameters, Op::AcklandJonesAnalysis, Op::AtomicStrain,
                  Op::WignerSeitzAnalysis, Op::CenterOfMassCorrection, Op::ShrinkWrapBox}) {
        check(std::string(opName(op)) != "Color by type", "every new operation has a name");
        check(!modifierOutputs(Modifier{op}, 0).empty(), std::string(opName(op)) + " declares its outputs");
    }
}

static void testCancellation() {
    std::atomic<bool> cancel{true};
    Dataset d = fcc(4);
    const Dataset reference = fcc(4);
    throwsWith([&] { computeSteinhardt(d, 12, &cancel); }, "Cancelled", "Steinhardt cancels");
    throwsWith([&] { computeAcklandJones(d, &cancel); }, "Cancelled", "Ackland-Jones cancels");
    throwsWith([&] { computeAtomicStrain(d, reference, 3, &cancel); }, "Cancelled", "Atomic strain cancels");
    auto r = resultOf(d);
    throwsWith([&] { computeWignerSeitz(r, reference, false, &cancel); }, "Cancelled", "Wigner-Seitz cancels");
    auto c = resultOf(d);
    throwsWith([&] { correctCenterOfMass(c, reference, false, &cancel); }, "Cancelled", "Center of mass cancels");
    check(d.scalarProperties.count("q6") == 0, "a cancelled analysis publishes nothing");
}

int main() {
    testNeighbors();
    testSteinhardt();
    testAcklandJones();
    testAtomicStrain();
    testWignerSeitz();
    testCenterOfMass();
    testShrinkWrap();
    testPipeline();
    testCancellation();
    if (failures) {
        std::cerr << failures << " of " << checks << " checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "PASS: " << checks << " checks: Steinhardt, Ackland-Jones, atomic strain, Wigner-Seitz, "
                 "center of mass, shrink-wrap, pipeline composition, cancellation\n";
}
