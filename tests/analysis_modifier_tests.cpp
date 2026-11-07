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

static void testReferenceMatching() {
    // Regression: equal counts with reordered identifiers must not pair by storage order.
    Dataset reference = fcc(3);
    auto &ids = reference.scalarProperties["Particle Identifier"];
    ids.resize(reference.atoms.size());
    std::iota(ids.begin(), ids.end(), 1.0);
    Dataset reordered = reference;
    std::reverse(reordered.atoms.begin(), reordered.atoms.end());
    auto &newIds = reordered.scalarProperties["Particle Identifier"];
    std::reverse(newIds.begin(), newIds.end());
    computeDisplacements(reordered, reference, Modifier{Op::DisplacementVectors});
    const auto &moved = reordered.scalarProperties.at("Displacement Magnitude");
    check(*std::max_element(moved.begin(), moved.end()) < 1e-6, "displacements follow identifiers, not storage order");
    const auto mapping = matchReferenceParticles(reordered, reference, nullptr);
    check(mapping.front() == reference.atoms.size() - 1, "the first reordered particle maps to the last reference particle");

    Dataset duplicates = reference;
    duplicates.scalarProperties["Particle Identifier"].assign(reference.atoms.size(), 7);
    Dataset copy = duplicates;
    check(matchReferenceParticles(copy, duplicates, nullptr)[4] == 4, "non-unique identifiers fall back to storage order");
}

static void testFrameDifference() {
    Dataset reference = fcc(2);
    const size_t n = reference.atoms.size();
    auto &ids = reference.scalarProperties["Particle Identifier"];
    ids.resize(n);
    std::iota(ids.begin(), ids.end(), 1.0);
    reference.scalarProperties["Energy"].assign(n, 1.5);
    reference.vectorProperties["Velocity"].assign(n, Vec3{1, 2, 3});
    reference.globalAttributes["Temperature"] = 300;
    reference.globalAttributes["OnlyHere"] = 1;

    Dataset current = reference;
    current.scalarProperties["Energy"][2] = 4;
    current.vectorProperties["Velocity"][1] = {2, 2, 1};
    current.globalAttributes["Temperature"] = 310;
    auto scalar = resultOf(current);
    computeFrameDifference(scalar, reference, "Energy", false, false, nullptr);
    near(scalar.data.scalarProperties.at("Energy (Difference)")[2], 2.5, 1e-12, "scalar difference");
    near(scalar.data.scalarProperties.at("Energy (Difference)")[3], 0, 0, "unchanged particle");
    near(attribute(scalar.data, "Temperature (Difference)"), 10, 1e-12, "global attribute difference");

    auto vector = resultOf(current);
    computeFrameDifference(vector, reference, "Velocity", false, false, nullptr);
    const auto &dv = vector.data.vectorProperties.at("Velocity (Difference)")[1];
    near(dv.x, 1, 1e-6, "vector X"); near(dv.y, 0, 1e-6, "vector Y"); near(dv.z, -2, 1e-6, "vector Z");
    check(vector.data.propertyComponents.count("Velocity (Difference)") == 1, "the vector output has components");

    auto component = resultOf(current);
    computeFrameDifference(component, reference, "Velocity.X", false, false, nullptr);
    near(component.data.scalarProperties.at("Velocity.X (Difference)")[1], 1, 1e-6, "vector component difference");

    // No periodic handling, as the manual states.
    Dataset wrapped = reference;
    wrapped.atoms[0].x += float(reference.cell[0]);
    auto plain = resultOf(wrapped);
    computeFrameDifference(plain, reference, "Position.X", false, false, nullptr);
    near(plain.data.scalarProperties.at("Position.X (Difference)")[0], reference.cell[0], 1e-5, "a wrapped jump stays a plain difference");

    // Particles removed since the reference are fine; reordering is matched by identifier.
    Dataset fewer = current;
    fewer.atoms.erase(fewer.atoms.begin());
    for (auto *values : {&fewer.scalarProperties["Particle Identifier"], &fewer.scalarProperties["Energy"]})
        values->erase(values->begin());
    fewer.vectorProperties["Velocity"].erase(fewer.vectorProperties["Velocity"].begin());
    auto removed = resultOf(fewer);
    computeFrameDifference(removed, reference, "Energy", false, false, nullptr);
    near(removed.data.scalarProperties.at("Energy (Difference)")[1], 2.5, 1e-12, "removal keeps identifier matching");

    // Particles new since the reference.
    Dataset grown = current;
    grown.atoms.push_back({1, 1, 1, 0});
    grown.scalarProperties["Particle Identifier"].push_back(9999);
    grown.scalarProperties["Energy"].push_back(50);
    grown.vectorProperties["Velocity"].push_back({0, 0, 0});
    auto strict = resultOf(grown);
    throwsWith([&] { computeFrameDifference(strict, reference, "Energy", false, false, nullptr); },
               "do not match", "new particles are an error by default");
    auto tolerant = resultOf(grown);
    computeFrameDifference(tolerant, reference, "Energy", true, true, nullptr);
    near(tolerant.data.scalarProperties.at("Energy (Difference)")[n], 0, 0, "a new particle reports 0");
    near(attribute(tolerant.data, "DifferenceBetweenFrames.new_particles"), 1, 0, "new particles are counted");
    check(tolerant.selected[n] == 1 && tolerant.selected[0] == 0, "only the new particle is selected");

    auto missing = resultOf(current);
    throwsWith([&] { computeFrameDifference(missing, reference, "", false, false, nullptr); }, "requires a particle property", "no property");
    Dataset noEnergy = reference;
    noEnergy.scalarProperties.erase("Energy");
    auto noReference = resultOf(current);
    bool threw = false;
    try { computeFrameDifference(noReference, noEnergy, "Energy", false, false, nullptr); } catch (const std::exception &) { threw = true; }
    check(threw, "a property absent from the reference is an error");
}

static std::vector<std::vector<std::string>> binned(const Dataset &d, const std::string &property, std::array<int, 3> bins,
                                                    int operation, const std::vector<uint8_t> *selection = nullptr,
                                                    bool derivative = false) {
    return binParticles(d, property, bins.data(), operation, selection, derivative, nullptr).rows;
}
static void testSpatialBinning() {
    Dataset d;
    d.species = {"X"};
    d.cell = {10, 0, 0, 0, 10, 0, 0, 0, 10};
    d.pbc = {true, true, true};
    d.atoms = {{1, 1, 1, 0}, {2, 2, 2, 0}, {7, 1, 1, 0}, {-1, 1, 1, 0}, {16, 1, 1, 0}};
    d.scalarProperties["T"] = {2, 4, 10, 20, 30};
    const auto counts = binned(d, "", {2, 1, 1}, 3);
    check(counts.size() == 2, "two bins give two rows");
    near(std::stod(counts[0][6]), 2, 0, "the low half holds two particles");
    near(std::stod(counts[1][6]), 3, 0, "wrapped particles land in the high half");
    near(std::stod(counts[0][7]), 2, 0, "population column");
    near(std::stod(counts[0][3]), 2.5, 1e-12, "bin center along the cell vector");
    near(std::stod(binned(d, "T", {2, 1, 1}, 2)[0][6]), 3, 1e-12, "mean");
    near(std::stod(binned(d, "T", {2, 1, 1}, 0)[1][6]), 10, 1e-12, "minimum");
    near(std::stod(binned(d, "T", {2, 1, 1}, 1)[1][6]), 30, 1e-12, "maximum");
    near(std::stod(binned(d, "T", {2, 1, 1}, 3)[1][6]), 60, 1e-12, "sum");
    near(std::stod(binned(d, "", {2, 1, 1}, 4)[0][6]), 2.0 / 500, 1e-12, "sum per bin volume is a number density");
    near(std::stod(binned(d, "", {5, 1, 1}, 2)[2][6]), 0, 0, "an empty bin reports 0");
    const std::vector<uint8_t> selected{1, 0, 0, 0, 1};
    near(std::stod(binned(d, "", {2, 1, 1}, 3, &selected)[0][6]), 1, 0, "only selected particles are binned");

    const auto grid = binned(d, "", {2, 2, 2}, 3);
    check(grid.size() == 8, "3D grids have every bin");
    double total = 0;
    for (const auto &row : grid) total += std::stod(row[6]);
    near(total, 5, 0, "every particle lands in exactly one 3D bin");

    Dataset slab = d;
    slab.pbc = {false, true, true};
    double kept = 0;
    for (const auto &row : binned(slab, "", {4, 1, 1}, 3)) kept += std::stod(row[6]);
    near(kept, 3, 0, "non-periodic particles outside the cell are not binned");

    // Bins follow sheared cell vectors: the same fractional layout gives the same counts.
    Dataset sheared = d;
    const double tilt[3][3]{{1, .5, 0}, {0, 1, 0}, {0, 0, 1}};
    transform(sheared, tilt);
    near(std::stod(binned(sheared, "", {2, 1, 1}, 3)[0][6]), 2, 0, "sheared bins follow the cell");

    // The derivative of a linear profile is its slope, away from the periodic seam.
    Dataset line;
    line.species = {"X"};
    line.cell = {10, 0, 0, 0, 10, 0, 0, 0, 10};
    for (int i = 0; i < 10; ++i) {
        line.atoms.push_back({i + .5f, 5, 5, 0});
        line.scalarProperties["V"].push_back(3.0 * (i + .5));
    }
    const auto gradient = binned(line, "V", {10, 1, 1}, 2, nullptr, true);
    near(std::stod(gradient[4][8]), 3, 1e-9, "central-difference derivative");
    near(std::stod(gradient[0][8]), 3, 1e-9, "one-sided derivative at a non-periodic edge");

    int noBins[3]{0, 1, 1}, tooMany[3]{1000, 1000, 2}, plane[3]{4, 4, 1};
    throwsWith([&] { binParticles(d, "", noBins, 3, nullptr, false, nullptr); }, "at least one bin", "zero bins");
    throwsWith([&] { binParticles(d, "", tooMany, 3, nullptr, false, nullptr); }, "1,000,000", "too many bins");
    throwsWith([&] { binParticles(d, "", plane, 3, nullptr, true, nullptr); }, "1D", "2D derivative");
    throwsWith([&] { binParticles(d, "", plane, 9, nullptr, false, nullptr); }, "Unknown", "bad operation");
    bool threw = false;
    try { binned(d, "Nothing", {2, 1, 1}, 3); } catch (const std::exception &) { threw = true; }
    check(threw, "an unknown property is an error");
}

static double structureFactorAt(const Dataset &d, double k) {
    const auto &rows = d.tables.back().rows;
    double best = 0, distance = 1e300;
    for (const auto &row : rows)
        if (const double gap = std::abs(std::stod(row[0]) - k); gap < distance) { distance = gap; best = std::stod(row[1]); }
    return best;
}
static void testStructureFactor() {
    constexpr double pi = 3.14159265358979323846, a = 3.6;
    Dataset d = fcc(4, a);
    const double n = double(d.atoms.size());
    computeStructureFactor(d, .5, 6, 400, nullptr, nullptr);
    near(attribute(d, "StructureFactor.peak_value"), n, n * 1e-4, "an FCC Bragg peak has S = N");
    near(attribute(d, "StructureFactor.peak_k"), 2 * pi * std::sqrt(3.0) / a, .02, "the first FCC peak is (111)");
    near(structureFactorAt(d, 2 * pi / a), 0, 1e-3, "the (100) reflection is forbidden in FCC");
    near(structureFactorAt(d, 4 * pi / a), n, n * 1e-4, "the (200) reflection is allowed");

    Dataset rotated = fcc(4, a);
    const double c = std::cos(.6), s = std::sin(.6), rotation[3][3]{{c, -s, 0}, {s, c, 0}, {0, 0, 1}};
    transform(rotated, rotation);
    computeStructureFactor(rotated, .5, 6, 400, nullptr, nullptr);
    near(attribute(rotated, "StructureFactor.peak_k"), attribute(d, "StructureFactor.peak_k"), .02, "S(k) ignores rigid rotation");

    // An uncorrelated ideal gas has S(k) = 1 on average.
    Dataset gas;
    gas.species = {"X"};
    gas.cell = {30, 0, 0, 0, 30, 0, 0, 0, 30};
    gas.pbc = {true, true, true};
    std::mt19937 random(3);
    std::uniform_real_distribution<float> uniform(0.f, 30.f);
    for (int i = 0; i < 1000; ++i) gas.atoms.push_back({uniform(random), uniform(random), uniform(random), 0});
    computeStructureFactor(gas, .5, 3, 8, nullptr, nullptr);
    double mean = 0;
    for (const auto &row : gas.tables.back().rows) mean += std::stod(row[1]);
    near(mean / double(gas.tables.back().rows.size()), 1, .08, "an ideal gas averages S = 1");

    Dataset half = fcc(4, a);
    std::vector<uint8_t> selection(half.atoms.size(), 0);
    for (size_t i = 0; i < selection.size(); i += 4) selection[i] = 1; // One sublattice: simple cubic.
    computeStructureFactor(half, .5, 6, 400, &selection, nullptr);
    near(structureFactorAt(half, 2 * pi / a), n / 4, n * 1e-3, "selecting one sublattice allows (100)");

    Dataset slab = fcc(2);
    slab.pbc[2] = false;
    throwsWith([&] { computeStructureFactor(slab, 0, 5, 10, nullptr, nullptr); }, "fully periodic", "a slab is rejected");
    Dataset good = fcc(2);
    throwsWith([&] { computeStructureFactor(good, 5, 2, 10, nullptr, nullptr); }, "minimum k", "reversed k range");
    throwsWith([&] { computeStructureFactor(good, 0, 5, 0, nullptr, nullptr); }, "bins", "zero bins");
    const std::vector<uint8_t> nothing(good.atoms.size(), 0);
    throwsWith([&] { computeStructureFactor(good, 0, 5, 10, &nothing, nullptr); }, "no particles", "an empty selection");
    throwsWith([&] { computeStructureFactor(good, 0, 500, 10, nullptr, nullptr); }, "too long", "work is bounded");
}

static Dataset cubicDiamond(int n, double a = 5.43) {
    return crystal({{0, 0, 0}, {0, .5, .5}, {.5, 0, .5}, {.5, .5, 0},
                    {.25, .25, .25}, {.25, .75, .75}, {.75, .25, .75}, {.75, .75, .25}}, {a, a, a}, n);
}
static Dataset hexagonalDiamond(int n, double bond = 2.35) {
    const double a = bond * std::sqrt(8.0 / 3.0), c = bond * 8.0 / 3.0;
    return crystal({{0, 1. / 3, 1. / 16}, {.5, 1. / 6, 9. / 16}, {0, 1. / 3, 7. / 16}, {.5, 1. / 6, 15. / 16},
                    {.5, 5. / 6, 1. / 16}, {0, 2. / 3, 9. / 16}, {.5, 5. / 6, 7. / 16}, {0, 2. / 3, 15. / 16}},
                   {a, a * std::sqrt(3.0), c}, n);
}
static std::array<uint64_t, 7> diamondCounts(Dataset &d, const std::vector<uint8_t> *selection = nullptr) {
    computeDiamondStructure(d, selection, nullptr);
    std::array<uint64_t, 7> counts{};
    for (double type : d.scalarProperties.at("Structure Type")) ++counts[size_t(type)];
    return counts;
}
static void testDiamond() {
    Dataset cubic = cubicDiamond(4);
    near(double(diamondCounts(cubic)[1]), double(cubic.atoms.size()), 0, "every atom of ideal cubic diamond");
    Dataset hexagonal = hexagonalDiamond(4);
    near(double(diamondCounts(hexagonal)[4]), double(hexagonal.atoms.size()), 0, "every atom of ideal hexagonal diamond");
    Dataset metal = fcc(4);
    near(double(diamondCounts(metal)[0]), double(metal.atoms.size()), 0, "FCC is not diamond");
    Dataset body = bcc(5);
    near(double(diamondCounts(body)[0]), double(body.atoms.size()), 0, "BCC is not diamond");

    // A vacancy: atoms that lose it can no longer be core atoms, and label as shells.
    Dataset vacancy = cubicDiamond(4);
    vacancy.atoms.erase(vacancy.atoms.begin() + 100);
    const auto shells = diamondCounts(vacancy);
    check(shells[1] > 0 && shells[1] < vacancy.atoms.size(), "a vacancy leaves perfect atoms and defective atoms");
    check(shells[2] + shells[3] + shells[0] == vacancy.atoms.size() - shells[1],
          "defective atoms are shell atoms or Other");
    check(shells[2] > 0, "atoms beside the vacancy are first-neighbor shell atoms");
    check(shells[4] + shells[5] + shells[6] == 0, "no hexagonal labels appear in a cubic crystal");

    // Thermal noise well below the shell spacing keeps the classification.
    Dataset thermal = cubicDiamond(4);
    std::mt19937 random(5);
    std::normal_distribution<float> noise(0.f, .05f);
    for (auto &a : thermal.atoms) { a.x += noise(random); a.y += noise(random); a.z += noise(random); }
    check(diamondCounts(thermal)[1] > .95 * double(thermal.atoms.size()), "thermal cubic diamond stays above 95%");

    // Selecting a diamond sublattice ignores the atoms around it.
    Dataset mixed = cubicDiamond(4);
    const size_t base = mixed.atoms.size();
    std::vector<uint8_t> selection(base, 1);
    const Dataset interstitials = crystal({{.5, .5, .5}}, {5.43, 5.43, 5.43}, 4);
    mixed.atoms.insert(mixed.atoms.end(), interstitials.atoms.begin(), interstitials.atoms.end());
    selection.resize(mixed.atoms.size(), 0);
    const auto filtered = diamondCounts(mixed, &selection);
    near(double(filtered[1]), double(base), 0, "selected sublattice is identified");
    near(double(filtered[0]), double(interstitials.atoms.size()), 0, "unselected atoms are Other");

    Dataset fewAtoms = cubicDiamond(4);
    fewAtoms.atoms.resize(3);
    near(double(diamondCounts(fewAtoms)[0]), 3, 0, "too few atoms is Other");
    Dataset none;
    computeDiamondStructure(none, nullptr, nullptr);
    check(none.scalarProperties.at("Structure Type").empty(), "an empty dataset is accepted");
    Dataset table = cubicDiamond(3);
    computeDiamondStructure(table, nullptr, nullptr);
    check(table.tables.back().rows.size() == 7, "a seven-row structure table is published");
    near(attribute(table, "IdentifyDiamond.counts.Cubic diamond"), double(table.atoms.size()), 0, "structure counts");
}

static void testBatchTwoPipeline() {
    Dataset reference = fcc(3);
    reference.scalarProperties["Energy"].assign(reference.atoms.size(), 1);
    Dataset current = reference;
    current.scalarProperties["Energy"].assign(reference.atoms.size(), 3);
    Modifier difference{Op::DifferenceBetweenFrames};
    difference.property = "Energy";
    Modifier select{Op::ExpressionSelect};
    select.property = "`Energy (Difference)` > 1";
    Modifier binning{Op::SpatialBinning};
    binning.property = "Energy (Difference)";
    binning.replicateN[0] = 3;
    binning.reduceOperation = 2;
    binning.cspOnlySelected = true;
    std::vector<Modifier> stack{difference, select, binning};
    std::function<const Dataset &(size_t)> provider = [&](size_t) -> const Dataset & { return reference; };
    auto r = evaluateFrom(resultOf(current), stack, 0, nullptr, nullptr, SIZE_MAX, {}, provider);
    check(std::count(r.selected.begin(), r.selected.end(), 1) == long(current.atoms.size()),
          "downstream nodes read the difference");
    near(std::stod(r.data.tables.back().rows[0][6]), 2, 1e-12, "binning reads the difference");

    Modifier sf{Op::StructureFactor};
    sf.value = 1; sf.upper = 4;
    Modifier diamond{Op::IdentifyDiamond};
    std::vector<Modifier> second{sf, diamond};
    auto analyzed = evaluateFrom(resultOf(cubicDiamond(2)), second, 0);
    check(analyzed.data.tables.size() == 2, "both analyses publish tables");

    std::vector<Modifier> needsReference{difference};
    throwsWith([&] { (void)evaluateFrom(resultOf(current), needsReference, 0); },
               "no reference configuration provider", "a missing reference is reported");
    for (Op op : {Op::DifferenceBetweenFrames, Op::SpatialBinning, Op::StructureFactor, Op::IdentifyDiamond}) {
        check(std::string(opName(op)) != "Color by type", "every new operation has a name");
        check(!modifierOutputs(Modifier{op}, 0).empty(), std::string(opName(op)) + " declares its outputs");
    }
    std::vector<Modifier> schema{difference, Modifier{Op::Histogram}};
    const auto choices = pipelineInputPropertyChoices(current, schema, 1);
    check(std::find(choices.begin(), choices.end(), "Energy (Difference)") != choices.end(),
          "the difference appears in downstream property choices");
}

// An in-memory trajectory standing in for the application's frame reader.
struct Trajectory {
    std::vector<Dataset> frames;
    int evaluations = 0;
    FrameEvaluator evaluator() {
        return [this](int frame) { ++evaluations; return frames.at(size_t(frame)); };
    }
    int size() const { return int(frames.size()); }
};
static Trajectory attributeTrajectory(std::initializer_list<double> values) {
    Trajectory t;
    for (double value : values) {
        Dataset d = fcc(2);
        d.globalAttributes["Energy"] = value;
        d.scalarProperties["Charge"].assign(d.atoms.size(), value);
        t.frames.push_back(std::move(d));
    }
    return t;
}
static void testTimeSeriesAndAverage() {
    Trajectory t = attributeTrajectory({1, 3, 8});
    const auto series = sweepTimeSeries("Energy", t.size(), t.evaluator(), nullptr);
    check(series.tables.size() == 1 && series.tables[0].rows.size() == 3, "one row per frame");
    near(std::stod(series.tables[0].rows[2][1]), 8, 0, "time series value");
    near(std::stod(series.tables[0].rows[2][0]), 2, 0, "time series frame");
    check(t.evaluations == 3, "the sweep visits each frame once");
    throwsWith([&] { sweepTimeSeries("Missing", t.size(), t.evaluator(), nullptr); }, "no global attribute", "unknown attribute");

    const auto attribute = sweepTimeAverage("Energy", false, t.size(), t.evaluator(), nullptr);
    near(attribute.attributes.at("Energy Average"), 4, 1e-12, "attribute average");
    const auto property = sweepTimeAverage("Charge", false, t.size(), t.evaluator(), nullptr);
    near(property.scalars.at("Charge Average")[0], 4, 1e-12, "particle property average");
    const auto replaced = sweepTimeAverage("Charge", true, t.size(), t.evaluator(), nullptr);
    check(replaced.scalars.count("Charge") == 1, "replace keeps the original name");
    const auto positions = sweepTimeAverage("Position.X", false, t.size(), t.evaluator(), nullptr);
    near(positions.scalars.at("Position.X Average")[1], t.frames[0].atoms[1].x, 1e-6, "position components can be averaged");
    throwsWith([&] { sweepTimeAverage("Missing", false, t.size(), t.evaluator(), nullptr); }, "no property or attribute", "unknown input");

    // Averages follow particle identity, and the output is re-applied in display order.
    Trajectory shuffled;
    for (int frame = 0; frame < 2; ++frame) {
        Dataset d;
        d.species = {"X"};
        d.atoms = {{0, 0, 0, 0}, {1, 0, 0, 0}};
        d.scalarProperties["Particle Identifier"] = frame ? std::vector<double>{2, 1} : std::vector<double>{1, 2};
        d.scalarProperties["Charge"] = frame ? std::vector<double>{20, 10} : std::vector<double>{10, 20};
        shuffled.frames.push_back(std::move(d));
    }
    const auto byIdentity = sweepTimeAverage("Charge", false, shuffled.size(), shuffled.evaluator(), nullptr);
    near(byIdentity.scalars.at("Charge Average")[0], 10, 1e-12, "the average tracks identifiers");
    Dataset displayed = shuffled.frames[1];
    applyTrajectoryOutput(displayed, byIdentity, nullptr);
    near(displayed.scalarProperties.at("Charge Average")[0], 20, 1e-12, "output follows the displayed order");

    Trajectory changing = attributeTrajectory({1, 2});
    changing.frames[1].atoms.pop_back();
    changing.frames[1].scalarProperties["Charge"].pop_back();
    throwsWith([&] { sweepTimeAverage("Charge", false, changing.size(), changing.evaluator(), nullptr); },
               "constant number", "changing particle counts are rejected");
}

static void testSmoothing() {
    Trajectory t;
    for (double x : {0.0, 1.0, 5.0, 3.0, 4.0}) {
        Dataset d;
        d.species = {"X"};
        d.cell = {100, 0, 0, 0, 100, 0, 0, 0, 100};
        d.atoms = {{float(x), 0, 0, 0}};
        t.frames.push_back(std::move(d));
    }
    Dataset middle = t.frames[2];
    smoothTrajectory(middle, 2, t.size(), 3, false, t.evaluator(), nullptr);
    near(middle.atoms[0].x, 3, 1e-5, "a three-frame centered mean");
    Dataset start = t.frames[0];
    smoothTrajectory(start, 0, t.size(), 3, false, t.evaluator(), nullptr);
    near(start.atoms[0].x, .5, 1e-5, "the window is cut short at the first frame");
    Dataset same = t.frames[2];
    smoothTrajectory(same, 2, t.size(), 1, false, t.evaluator(), nullptr);
    near(same.atoms[0].x, 5, 0, "a window of one changes nothing");

    // A particle crossing the boundary: 9.8, then 0.2 in a 10 long cell, is a step of 0.4.
    Trajectory wrapped;
    for (float x : {9.8f, .2f}) {
        Dataset d;
        d.species = {"X"};
        d.cell = {10, 0, 0, 0, 10, 0, 0, 0, 10};
        d.pbc = {true, true, true};
        d.atoms = {{x, 5, 5, 0}};
        wrapped.frames.push_back(std::move(d));
    }
    Dataset crossing = wrapped.frames[1];
    smoothTrajectory(crossing, 1, 2, 3, true, wrapped.evaluator(), nullptr);
    near(crossing.atoms[0].x, 0, 1e-5, "the minimum image keeps the mean continuous");
    Dataset naive = wrapped.frames[1];
    smoothTrajectory(naive, 1, 2, 3, false, wrapped.evaluator(), nullptr);
    near(naive.atoms[0].x, 5, 1e-5, "without it the mean jumps across the cell");
    throwsWith([&] { smoothTrajectory(same, 2, 5, 0, false, t.evaluator(), nullptr); }, "smoothing window", "zero window");
}

static void testDiffusion() {
    // A 3D random walk with step variance s per axis has MSD = 3 s tau, so D = s / 2.
    constexpr double sigma = .2, cell = 12;
    constexpr int particles = 1500, steps = 120;
    auto walk = [&](bool periodic) {
        std::mt19937 random(9);
        std::normal_distribution<double> step(0, sigma);
        std::uniform_real_distribution<double> place(0, cell);
        std::vector<std::array<double, 3>> position(particles);
        for (auto &p : position) p = {place(random), place(random), place(random)};
        Trajectory t;
        for (int frame = 0; frame < steps; ++frame) {
            Dataset d;
            d.species = {"X"};
            d.cell = {cell, 0, 0, 0, cell, 0, 0, 0, cell};
            d.pbc = {periodic, periodic, periodic};
            for (const auto &p : position) {
                Atom a{float(p[0]), float(p[1]), float(p[2]), 0};
                d.atoms.push_back(periodic ? wrapAtomInCell(d, a) : a);
            }
            t.frames.push_back(std::move(d));
            for (auto &p : position) for (double &x : p) x += step(random);
        }
        return t;
    };
    Trajectory open = walk(false);
    const auto free = sweepDiffusion(open.size(), 2, 1, 0, .3, open.evaluator(), nullptr);
    const double expected = sigma * sigma / 2;
    near(free.attributes.at("Diffusion.coefficient"), expected, .1 * expected, "random-walk diffusion coefficient");
    Trajectory wrapped = walk(true);
    const auto folded = sweepDiffusion(wrapped.size(), 2, 1, 0, .3, wrapped.evaluator(), nullptr);
    near(folded.attributes.at("Diffusion.coefficient"), free.attributes.at("Diffusion.coefficient"), .02 * expected,
         "wrapped coordinates give the same coefficient");
    const auto scaled = sweepDiffusion(open.size(), 2, .5, 0, .3, open.evaluator(), nullptr);
    near(scaled.attributes.at("Diffusion.coefficient"), 2 * free.attributes.at("Diffusion.coefficient"), 1e-6,
         "the time unit scales D");

    // Exact: one particle at constant velocity has MSD = v^2 tau^2.
    Trajectory ballistic;
    for (int frame = 0; frame < 6; ++frame) {
        Dataset d;
        d.species = {"X"};
        d.atoms = {{float(frame * 2), 0, 0, 0}};
        ballistic.frames.push_back(std::move(d));
    }
    const auto msd = sweepDiffusion(ballistic.size(), 1, 1, 0, 1, ballistic.evaluator(), nullptr);
    near(std::stod(msd.tables[0].rows[2][1]), 36, 1e-6, "MSD at lag 3");
    near(std::stod(msd.tables[0].rows[0][2]), 5, 0, "five origins contribute to lag 1");
    near(std::stod(msd.tables[0].rows[4][2]), 1, 0, "one origin contributes to the longest lag");

    Trajectory single;
    single.frames.push_back(fcc(2));
    const auto none = sweepDiffusion(1, 1, 1, 0, 1, single.evaluator(), nullptr);
    check(none.tables[0].rows.empty() && !none.attributes.count("Diffusion.coefficient"), "one frame gives no coefficient");
    throwsWith([&] { sweepDiffusion(2, 0, 1, 0, 1, single.evaluator(), nullptr); }, "origin stride", "zero stride");
    throwsWith([&] { sweepDiffusion(2, 1, 0, 0, 1, single.evaluator(), nullptr); }, "time per frame", "zero time step");
    throwsWith([&] { sweepDiffusion(2, 1, 1, .8, .2, single.evaluator(), nullptr); }, "fit window", "reversed fit window");
}

static Dataset graphOf(int count, std::initializer_list<std::pair<uint32_t, uint32_t>> edges) {
    Dataset d;
    d.species = {"X"};
    d.atoms.assign(size_t(count), Atom{});
    for (const auto &[a, b] : edges) { Bond bond; bond.a = a; bond.b = b; d.bonds.push_back(bond); }
    return d;
}
static double ringsOf(Dataset d, int size, int minSize = 3, int maxSize = 12) {
    computeRings(d, minSize, maxSize, nullptr);
    for (const auto &row : d.tables[d.tables.size() - 2].rows)
        if (std::stoi(row[0]) == size) return std::stod(row[1]);
    return -1;
}
static void testRings() {
    near(ringsOf(graphOf(3, {{0, 1}, {1, 2}, {2, 0}}), 3), 1, 0, "a triangle is one ring");
    near(ringsOf(graphOf(6, {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 0}}), 6), 1, 0, "a hexagon is one ring");
    near(ringsOf(graphOf(5, {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 0}}), 5), 1, 0, "an odd ring is found");
    near(ringsOf(graphOf(4, {{0, 1}, {1, 2}, {2, 3}}), 4), 0, 0, "a chain has no ring");
    near(ringsOf(graphOf(9, {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 6}, {6, 7}, {7, 8}, {8, 0}}), 8, 3, 8), 0, 0,
         "a ring above the maximum size is skipped");
    // Naphthalene: two hexagons sharing a bond. The outer 10-cycle has a shortcut, so it is no ring.
    const auto naphthalene = graphOf(10, {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 0}, {4, 6}, {6, 7}, {7, 8}, {8, 9}, {9, 5}});
    near(ringsOf(naphthalene, 6), 2, 0, "naphthalene has two six-rings");
    near(ringsOf(naphthalene, 10), 0, 0, "its outer cycle is not a shortest-path ring");
    // A cube has six square faces, and its four Petrie hexagons are shortest-path rings:
    // opposite corners are three bonds apart both around the ring and through the graph.
    const auto cube = graphOf(8, {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}});
    near(ringsOf(cube, 4), 6, 0, "a cube has six four-rings");
    near(ringsOf(cube, 6), 4, 0, "a cube has four Petrie hexagons");
    near(ringsOf(cube, 5), 0, 0, "a bipartite cube has no odd rings");
    // Smaller rings must survive a large maximum size.
    near(ringsOf(cube, 4, 3, 36), 6, 0, "the maximum size does not hide small rings");
    // A periodic honeycomb sheet: every hexagon, each counted once.
    constexpr int width = 6, height = 6;
    Dataset sheet;
    sheet.species = {"X"};
    sheet.atoms.assign(size_t(2 * width * height), Atom{});
    auto site = [&](int x, int y, int sublattice) {
        return uint32_t(2 * (((y + height) % height) * width + (x + width) % width) + sublattice);
    };
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (const auto [dx, dy] : {std::pair{0, 0}, std::pair{-1, 0}, std::pair{0, -1}}) {
                Bond bond; bond.a = site(x, y, 0); bond.b = site(x + dx, y + dy, 1); sheet.bonds.push_back(bond);
            }
    near(ringsOf(sheet, 6, 3, 8), width * height, 0, "a periodic honeycomb has one hexagon per cell");

    Dataset counted = graphOf(4, {{0, 1}, {1, 2}, {2, 0}, {2, 3}});
    computeRings(counted, 3, 6, nullptr);
    near(attribute(counted, "FindRings.ring_count"), 1, 0, "ring count attribute");
    near(counted.scalarProperties.at("Ring Count")[2], 1, 0, "ring members are counted");
    near(counted.scalarProperties.at("Ring Count")[3], 0, 0, "a dangling atom is in no ring");
    check(counted.tables.back().rows[0][1] == "0 1 2", "the ring list names its particles");

    Dataset duplicate = graphOf(3, {{0, 1}, {1, 0}, {1, 2}, {2, 0}, {0, 0}});
    near(ringsOf(duplicate, 3), 1, 0, "duplicate and self bonds are ignored");
    Dataset none = fcc(2);
    near(ringsOf(none, 3), 0, 0, "no bonds gives no rings");
    Dataset bad = graphOf(3, {{0, 9}});
    throwsWith([&] { computeRings(bad, 3, 6, nullptr); }, "outside the dataset", "a bad bond is rejected");
    Dataset fine = graphOf(3, {{0, 1}});
    throwsWith([&] { computeRings(fine, 2, 6, nullptr); }, "Ring sizes", "minimum below 3");
    throwsWith([&] { computeRings(fine, 3, 37, nullptr); }, "Ring sizes", "maximum above 36");
    throwsWith([&] { computeRings(fine, 8, 6, nullptr); }, "Ring sizes", "reversed range");
}

static void testTrajectoryPipeline() {
    Trajectory t = attributeTrajectory({2, 4});
    int calls = 0;
    std::optional<TrajectoryOutput> stored;
    ModifierFrameState state;
    state.frameCount = 2;
    state.frameBefore = [&](size_t, int frame) { ++calls; return t.frames.at(size_t(frame)); };
    state.cachedSweep = [&](size_t) { return stored; };
    state.storeSweep = [&](size_t, TrajectoryOutput output) { stored = std::move(output); };
    Modifier average{Op::TimeAveraging};
    average.property = "Energy";
    Modifier select{Op::ExpressionSelect};
    select.property = "`Charge` > 1";
    std::vector<Modifier> stack{average, select};
    auto run = [&](int frame) {
        return evaluateFrom(resultOf(t.frames.at(size_t(frame))), stack, 0, nullptr, nullptr, SIZE_MAX, {}, {}, frame, state);
    };
    const auto first = run(0);
    near(attribute(first.data, "Energy Average"), 3, 1e-12, "the pipeline publishes the average");
    check(calls == 2, "the first evaluation sweeps every frame");
    const auto second = run(1);
    near(attribute(second.data, "Energy Average"), 3, 1e-12, "the average does not depend on the frame");
    check(calls == 2, "changing frames reuses the sweep");

    Modifier unset{Op::TimeSeries};
    unset.property.clear();
    std::vector<Modifier> empty{unset};
    (void)evaluateFrom(resultOf(fcc(2)), empty, 0);
    check(true, "a node with nothing chosen yet is not an error");
    Modifier series{Op::TimeSeries};
    series.property = "Energy";
    std::vector<Modifier> noAccess{series};
    throwsWith([&] { (void)evaluateFrom(resultOf(fcc(2)), noAccess, 0); }, "no access to the trajectory", "missing trajectory access");
    for (Op op : {Op::TimeSeries, Op::TimeAveraging, Op::SmoothTrajectory, Op::DiffusionCoefficient, Op::FindRings}) {
        check(std::string(opName(op)) != "Color by type", "every new operation has a name");
        check(!modifierOutputs(Modifier{op}, 0).empty(), std::string(opName(op)) + " declares its outputs");
    }
}

static PipelineResult voronoiOf(Dataset d, VoronoiOptions options = {}, std::vector<uint8_t> selection = {}) {
    auto r = resultOf(std::move(d));
    if (!selection.empty()) r.selected = std::move(selection);
    computeVoronoi(r, options, nullptr);
    return r;
}
static bool allNear(const std::vector<double> &values, double expected, double tolerance) {
    return !values.empty() && std::all_of(values.begin(), values.end(),
        [&](double v) { return std::abs(v - expected) <= tolerance; });
}
static double sumOf(const std::vector<double> &values) { return std::accumulate(values.begin(), values.end(), 0.0); }
static Dataset randomGas(int count, std::array<bool, 3> pbc, unsigned seed, double size = 20) {
    Dataset d;
    d.species = {"X"};
    d.cell = {size, 0, 0, 0, size, 0, 0, 0, size};
    d.pbc = pbc;
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> place(0.f, float(size));
    for (int i = 0; i < count; ++i) d.atoms.push_back({place(random), place(random), place(random), 0});
    return d;
}
static void testVoronoi() {
    // Exact cells of the three cubic lattices.
    const double a = 3.6;
    auto f = voronoiOf(fcc(4, a)).data.scalarProperties;
    check(allNear(f.at("Atomic Volume"), a * a * a / 4, 1e-4), "FCC cells hold a^3/4");
    check(allNear(f.at("Coordination"), 12, 0), "FCC cells have 12 faces");
    check(allNear(f.at("Voronoi Index.4"), 12, 0) && allNear(f.at("Voronoi Index.3"), 0, 0),
          "FCC cells are rhombic dodecahedra <0,12>");
    check(allNear(f.at("Cavity Radius"), a / 2, 1e-4), "the FCC cavity radius is a/2");
    check(allNear(f.at("Max Face Order"), 4, 0), "FCC faces are quadrilaterals");

    const double b = 3.0;
    auto body = voronoiOf(bcc(5, b)).data.scalarProperties;
    check(allNear(body.at("Atomic Volume"), b * b * b / 2, 1e-4), "BCC cells hold a^3/2");
    check(allNear(body.at("Coordination"), 14, 0), "BCC cells have 14 faces");
    check(allNear(body.at("Voronoi Index.4"), 6, 0) && allNear(body.at("Voronoi Index.6"), 8, 0),
          "BCC cells are truncated octahedra <0,6,0,8>");
    check(allNear(body.at("Cavity Radius"), b * std::sqrt(5.0) / 4, 1e-4), "the BCC cavity radius is a sqrt(5)/4");

    auto cube = voronoiOf(simpleCubic(5, b)).data.scalarProperties;
    check(allNear(cube.at("Atomic Volume"), b * b * b, 1e-4), "simple cubic cells are cubes of volume a^3");
    check(allNear(cube.at("Coordination"), 6, 0) && allNear(cube.at("Voronoi Index.4"), 6, 0), "a cube has six square faces");
    check(allNear(cube.at("Cavity Radius"), b * std::sqrt(3.0) / 2, 1e-4), "the cube cavity radius is half its diagonal");

    // The cells tile the box, whatever the arrangement.
    const auto gas = voronoiOf(randomGas(400, {true, true, true}, 21));
    near(sumOf(gas.data.scalarProperties.at("Atomic Volume")), 8000, 8000 * 1e-6, "random cells tile the periodic box");
    near(attribute(gas.data, "Voronoi.total_volume"), 8000, 8000 * 1e-6, "total volume attribute");
    const auto &gasVolumes = gas.data.scalarProperties.at("Atomic Volume");
    check(*std::min_element(gasVolumes.begin(), gasVolumes.end()) > 0, "every random cell has a volume");

    Dataset tilted = randomGas(300, {true, true, true}, 5, 16);
    const double shear[3][3]{{1, .35, .2}, {0, 1, .25}, {0, 0, 1}};
    transform(tilted, shear);
    near(sumOf(voronoiOf(tilted).data.scalarProperties.at("Atomic Volume")), std::abs(cellDeterminant(tilted.cell)),
         1e-5 * 4096, "cells tile a triclinic periodic box");

    const auto walled = voronoiOf(randomGas(300, {false, false, false}, 8));
    near(sumOf(walled.data.scalarProperties.at("Atomic Volume")), 8000, 8000 * 1e-6, "walls close a non-periodic box");
    const auto slab = voronoiOf(randomGas(300, {true, true, false}, 9));
    near(sumOf(slab.data.scalarProperties.at("Atomic Volume")), 8000, 8000 * 1e-6, "a slab mixes walls and periodicity");

    // Moving or rotating everything changes nothing.
    Dataset moved = fcc(4, a);
    for (auto &atom : moved.atoms) { atom.x += .7f; atom.y -= 1.1f; atom.z += .3f; }
    check(allNear(voronoiOf(moved).data.scalarProperties.at("Atomic Volume"), a * a * a / 4, 1e-4), "translation invariance");
    Dataset rotated = fcc(4, a);
    const double c = std::cos(.4), s = std::sin(.4), rotation[3][3]{{c, -s, 0}, {s, c, 0}, {0, 0, 1}};
    transform(rotated, rotation);
    check(allNear(voronoiOf(rotated).data.scalarProperties.at("Coordination"), 12, 0), "rotation invariance");

    // Polydisperse cells: equal radii change nothing; larger particles take more room.
    Dataset equal = fcc(3, a);
    equal.scalarProperties["Radius"].assign(equal.atoms.size(), 1.2);
    VoronoiOptions radical;
    radical.useRadii = true;
    check(allNear(voronoiOf(equal, radical).data.scalarProperties.at("Atomic Volume"), a * a * a / 4, 1e-4),
          "equal radii reproduce the plain tessellation");
    Dataset mixed = randomGas(300, {true, true, true}, 13);
    auto &radii = mixed.scalarProperties["Radius"];
    for (size_t i = 0; i < mixed.atoms.size(); ++i) radii.push_back(i % 2 ? 1.5 : .5);
    const auto laguerre = voronoiOf(mixed, radical);
    const auto &laguerreVolumes = laguerre.data.scalarProperties.at("Atomic Volume");
    near(sumOf(laguerreVolumes), 8000, 8000 * 1e-6, "radical cells still tile the box");
    double big = 0, small = 0;
    for (size_t i = 0; i < laguerreVolumes.size(); ++i) (i % 2 ? big : small) += laguerreVolumes[i];
    check(big > 1.5 * small, "larger particles get larger radical cells");
    Dataset noRadius = fcc(3);
    throwsWith([&] { voronoiOf(noRadius, radical); }, "Radius particle property", "radii are required");

    // Use only selected: one sublattice of FCC is simple cubic.
    Dataset lattice = fcc(4, a);
    std::vector<uint8_t> sublattice(lattice.atoms.size(), 0);
    for (size_t i = 0; i < sublattice.size(); i += 4) sublattice[i] = 1;
    VoronoiOptions selected;
    selected.onlySelected = true;
    const auto sc = voronoiOf(lattice, selected, sublattice).data.scalarProperties;
    near(sc.at("Atomic Volume")[0], a * a * a, 1e-4, "selected sublattice cells");
    near(sc.at("Coordination")[0], 6, 0, "selected sublattice coordination");
    near(sc.at("Atomic Volume")[1], 0, 0, "unselected particles report zero");

    // Near-degenerate faces: in BCC the squares are 3.7% of the area, the hexagons 9.7%.
    VoronoiOptions filtered;
    filtered.relativeAreaThreshold = .05;
    check(allNear(voronoiOf(bcc(5, b), filtered).data.scalarProperties.at("Coordination"), 8, 0),
          "a relative threshold drops the small square faces");
    VoronoiOptions absolute;
    absolute.faceAreaThreshold = 1e6;
    check(allNear(voronoiOf(fcc(3, a), absolute).data.scalarProperties.at("Coordination"), 0, 0), "an absolute threshold");
    VoronoiOptions edges;
    edges.edgeLengthThreshold = 1e6;
    check(allNear(voronoiOf(fcc(3, a), edges).data.scalarProperties.at("Coordination"), 0, 0),
          "faces with fewer than three long edges are dropped");

    // Neighbor bonds: one per shared face, with correct periodic geometry.
    VoronoiOptions bonding;
    bonding.neighborBonds = true;
    const auto bonded = voronoiOf(fcc(4, a), bonding);
    check(bonded.data.bonds.size() == bonded.data.atoms.size() * 6, "FCC gives six bonds per atom");
    bool lengths = true;
    for (const auto &bond : bonded.data.bonds) {
        const auto &p = bonded.data.atoms[bond.a], &q = bonded.data.atoms[bond.b];
        double v[3]{double(q.x) - p.x, double(q.y) - p.y, double(q.z) - p.z};
        for (int axis = 0; axis < 3; ++axis)
            for (int xyz = 0; xyz < 3; ++xyz) v[xyz] += bond.image[size_t(axis)] * bonded.data.cell[size_t(axis) * 3 + size_t(xyz)];
        lengths = lengths && std::abs(std::hypot(v[0], v[1], v[2]) - a / std::sqrt(2.0)) < 1e-4;
    }
    check(lengths, "every bond, including periodic ones, spans the nearest-neighbor distance");

    const auto histogram = voronoiOf(fcc(3, a)).data.tables.back();
    check(histogram.rows.size() == 1 && histogram.rows[0][0] == "<0,12>" &&
          histogram.rows[0][1] == std::to_string(fcc(3, a).atoms.size()), "the index histogram counts one signature");

    Dataset twin = fcc(3);
    twin.atoms.push_back(twin.atoms[5]);
    throwsWith([&] { voronoiOf(twin); }, "share a position", "coincident particles are rejected");
    // A unit cell smaller than the Voronoi reach is resolved through internal replicas.
    const auto unit = voronoiOf(fcc(1, a), bonding).data;
    check(allNear(unit.scalarProperties.at("Atomic Volume"), a * a * a / 4, 1e-4), "a single FCC unit cell");
    check(allNear(unit.scalarProperties.at("Coordination"), 12, 0), "a single FCC unit cell has 12 faces");
    check(unit.bonds.size() == 24, "a single unit cell gets six bonds per atom, through periodic images");
    const auto sc1 = voronoiOf(simpleCubic(1, b)).data.scalarProperties;
    near(sc1.at("Atomic Volume")[0], b * b * b, 1e-4, "one atom in a periodic cube");
    near(sc1.at("Coordination")[0], 6, 0, "one atom in a periodic cube has six faces");
    Dataset outside = randomGas(20, {false, false, false}, 3);
    outside.atoms[0].x = -5;
    throwsWith([&] { voronoiOf(outside); }, "outside the simulation cell", "particles must lie inside walls");
    Dataset noCell = randomGas(10, {false, false, false}, 4);
    noCell.cell = {};
    throwsWith([&] { voronoiOf(noCell); }, "valid simulation cell", "a cell is required");
    VoronoiOptions badThreshold;
    badThreshold.relativeAreaThreshold = 1;
    throwsWith([&] { voronoiOf(fcc(2), badThreshold); }, "thresholds", "a relative threshold of 1 is rejected");

    // In a pipeline, downstream nodes read the cells.
    Modifier cells{Op::VoronoiAnalysis};
    cells.upper = 0;
    Modifier pick{Op::ExpressionSelect};
    pick.property = "Coordination == 14";
    std::vector<Modifier> stack{cells, pick};
    const auto evaluated = evaluateFrom(resultOf(bcc(4, b)), stack, 0);
    check(std::count(evaluated.selected.begin(), evaluated.selected.end(), 1) == long(evaluated.data.atoms.size()),
          "Expression selection reads the Voronoi coordination");
    check(std::string(opName(Op::VoronoiAnalysis)) == "Voronoi analysis" &&
          !modifierOutputs(Modifier{Op::VoronoiAnalysis}, 0).empty(), "Voronoi has a name and outputs");
}

static void testCancellation() {
    std::atomic<bool> cancel{true};
    {
        auto r = resultOf(fcc(3));
        throwsWith([&] { computeVoronoi(r, {}, &cancel); }, "Cancelled", "Voronoi cancels");
    }
    {
        Trajectory t = attributeTrajectory({1, 2});
        throwsWith([&] { sweepTimeSeries("Energy", 2, t.evaluator(), &cancel); }, "Cancelled", "time series cancels");
        throwsWith([&] { sweepTimeAverage("Energy", false, 2, t.evaluator(), &cancel); }, "Cancelled", "time averaging cancels");
        throwsWith([&] { sweepDiffusion(2, 1, 1, 0, 1, t.evaluator(), &cancel); }, "Cancelled", "diffusion cancels");
        Dataset d = t.frames[0];
        throwsWith([&] { smoothTrajectory(d, 0, 2, 3, false, t.evaluator(), &cancel); }, "Cancelled", "smoothing cancels");
        Dataset rings = graphOf(3, {{0, 1}, {1, 2}, {2, 0}});
        throwsWith([&] { computeRings(rings, 3, 6, &cancel); }, "Cancelled", "rings cancel");
    }
    {
        Dataset d = fcc(4);
        const int bins[3]{4, 1, 1};
        throwsWith([&] { (void)binParticles(d, "", bins, 3, nullptr, false, &cancel); }, "Cancelled", "binning cancels");
        throwsWith([&] { computeStructureFactor(d, 0, 4, 10, nullptr, &cancel); }, "Cancelled", "S(k) cancels");
        throwsWith([&] { computeDiamondStructure(d, nullptr, &cancel); }, "Cancelled", "diamond cancels");
        auto r = resultOf(d);
        throwsWith([&] { computeFrameDifference(r, d, "Position.X", false, false, &cancel); }, "Cancelled", "difference cancels");
    }
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
    testReferenceMatching();
    testFrameDifference();
    testSpatialBinning();
    testStructureFactor();
    testDiamond();
    testBatchTwoPipeline();
    testTimeSeriesAndAverage();
    testSmoothing();
    testDiffusion();
    testRings();
    testTrajectoryPipeline();
    testVoronoi();
    testCancellation();
    if (failures) {
        std::cerr << failures << " of " << checks << " checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "PASS: " << checks << " checks: Steinhardt, Ackland-Jones, atomic strain, Wigner-Seitz, "
                 "center of mass, shrink-wrap, frame differences, spatial binning, structure factor, "
                 "diamond identification, trajectory sweeps, diffusion, rings, Voronoi, pipeline composition, cancellation\n";
}
