// Numerical checks for the OVITO-parity analysis modifiers. Expected values are
// published reference numbers or closed-form results, never AtomX's own output.
#include "../src/core.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <random>
#include <map>

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

// ---- Chill+ ---------------------------------------------------------------------
// Ice oxygens sit on the diamond (Ic) and lonsdaleite (Ih) lattices; 2.76 A O-O bonds.
static constexpr double iceBond = 2.76;
static Dataset cubicIce(int n) { return cubicDiamond(n, iceBond * 4 / std::sqrt(3.0)); }
static Dataset hexagonalIce(int n) { return hexagonalDiamond(n, iceBond); }
static std::array<uint64_t, 6> chillCounts(Dataset d, double cutoff = 3.5, const std::vector<uint8_t> *selection = nullptr) {
    computeChillPlus(d, cutoff, selection, nullptr);
    std::array<uint64_t, 6> counts{};
    for (double type : d.scalarProperties.at("Structure Type")) ++counts[size_t(type)];
    return counts;
}
static void testChillPlus() {
    const auto ic = chillCounts(cubicIce(2));
    check(ic[2] == 64, "every cubic ice oxygen is cubic ice: " + std::to_string(ic[2]));
    const auto ih = chillCounts(hexagonalIce(2));
    check(ih[1] == 64, "every hexagonal ice oxygen is hexagonal ice: " + std::to_string(ih[1]));
    // Staggered bonds of an ideal tetrahedral network correlate at exactly -1.
    const Dataset diamond = cubicIce(2);
    const auto lists = neighborVectorLists(diamond, 3.5);
    double worst = 0;
    for (const auto &neighbor : lists[0])
        worst = std::max(worst, std::abs(q3Correlation(bondOrderQ3(lists[0]), bondOrderQ3(lists[neighbor.index])) + 1));
    near(worst, 0, 1e-5, "ideal staggered bonds have c = -1");
    // In ice Ih the bond along c is eclipsed; the other three are staggered.
    const Dataset lonsdaleite = hexagonalIce(2);
    const auto hexLists = neighborVectorLists(lonsdaleite, 3.5);
    int eclipsed = 0, staggered = 0;
    for (const auto &neighbor : hexLists[0]) {
        const double c = q3Correlation(bondOrderQ3(hexLists[0]), bondOrderQ3(hexLists[neighbor.index]));
        if (c > -0.35 && c < 0.25) {
            ++eclipsed;
            check(std::abs(neighbor.vector[2]) > 2.7, "the eclipsed bond runs along c");
        } else if (c < -0.8) ++staggered;
    }
    check(eclipsed == 1 && staggered == 3, "ice Ih has one eclipsed and three staggered bonds");
    // The classification table of Nguyen & Molinero (2015).
    check(chillPlusClassify(4, 4, 0) == ChillPlusType::Hydrate, "four eclipsed bonds: hydrate");
    check(chillPlusClassify(4, 3, 1) == ChillPlusType::InterfacialHydrate, "three eclipsed bonds: interfacial hydrate");
    check(chillPlusClassify(4, 0, 4) == ChillPlusType::CubicIce, "four staggered bonds: cubic ice");
    check(chillPlusClassify(4, 1, 3) == ChillPlusType::HexagonalIce, "three staggered, one eclipsed: hexagonal ice");
    check(chillPlusClassify(4, 0, 3) == ChillPlusType::InterfacialIce, "three staggered alone: interfacial ice");
    check(chillPlusClassify(4, 1, 2) == ChillPlusType::InterfacialIce, "two staggered: interfacial ice");
    check(chillPlusClassify(4, 2, 1) == ChillPlusType::Other, "two eclipsed, one staggered: other");
    check(chillPlusClassify(3, 0, 3) == ChillPlusType::Other && chillPlusClassify(5, 0, 4) == ChillPlusType::Other,
          "only four-coordinated particles are classified");
    // Rotating the whole crystal changes nothing.
    Dataset rotated = cubicIce(2);
    const double t = .7, f[3][3]{{std::cos(t), -std::sin(t), 0}, {std::sin(t) * .6, std::cos(t) * .6, -.8},
                                 {std::sin(t) * .8, std::cos(t) * .8, .6}};
    transform(rotated, f);
    check(chillCounts(rotated)[2] == 64, "cubic ice is recognized at any orientation");
    check(chillCounts(cubicIce(1))[2] == 8, "a unit cell narrower than twice the cutoff uses its periodic images");
    // Too short a cutoff leaves no bonds; liquid-like disorder is mostly other.
    check(chillCounts(cubicIce(2), 2.0)[0] == 64, "no neighbors within the cutoff gives other");
    const auto gas = chillCounts(randomGas(400, {true, true, true}, 11, 22.6));
    check(gas[0] > 300, "random positions are mostly other: " + std::to_string(gas[0]));
    std::vector<uint8_t> half(64, 0);
    for (size_t i = 0; i < 32; ++i) half[i] = 1;
    const auto selected = chillCounts(cubicIce(2), 3.5, &half);
    check(selected[0] >= 32, "unselected particles are other");
    Dataset counted = cubicIce(2);
    computeChillPlus(counted, 3.5, nullptr, nullptr);
    near(attribute(counted, "ChillPlus.counts.Cubic ice"), 64, 0, "cubic ice count attribute");
    check(!counted.tables.empty() && counted.tables.back().name == "Chill+" && counted.tables.back().rows.size() == 6,
          "Chill+ publishes its count table");
    throwsWith([&] { chillCounts(cubicIce(2), 0); }, "positive cutoff", "Chill+ rejects a zero cutoff");
}

// ---- Local entropy ----------------------------------------------------------------
// The formula of Piaggi & Parrinello evaluated from a list of neighbor distances,
// independently of the AtomX neighbor search.
static double pairEntropy(const std::vector<std::pair<double, int>> &shells, double rho, double cutoff, double sigma,
                          double densityForG) {
    const double pi = 3.14159265358979323846;
    const int bins = int(cutoff / sigma) + 1;
    double integral = 0, previous = 0, previousR = 0;
    for (int k = 0; k < bins; ++k) {
        const double r = cutoff * k / (bins - 1);
        const double rr = k ? r : cutoff / (bins - 1); // r = 0 uses the next point's normalization.
        double g = 0;
        for (const auto &[distance, count] : shells)
            if (distance < cutoff) g += count * std::exp(-(r - distance) * (r - distance) / (2 * sigma * sigma));
        g /= 4 * pi * densityForG * rr * rr * std::sqrt(2 * pi * sigma * sigma);
        const double integrand = g >= 1e-10 ? (g * std::log(g) - g + 1) * r * r : r * r;
        if (k) integral += .5 * (integrand + previous) * (r - previousR);
        previous = integrand;
        previousR = r;
    }
    return -2 * pi * rho * integral;
}
static void testLocalEntropy() {
    const double a = 3.6;
    Dataset crystalFcc = fcc(4, a);
    LocalEntropyOptions options;
    computeLocalEntropy(crystalFcc, options, nullptr);
    const auto &s = crystalFcc.scalarProperties.at("Entropy");
    const double rho = 4 / (a * a * a);
    // fcc shells: 12 at a/sqrt2, 6 at a, 24 at a sqrt(3/2), 12 at a sqrt2 (beyond 5).
    const std::vector<std::pair<double, int>> shells{{a / std::sqrt(2.0), 12}, {a, 6}, {a * std::sqrt(1.5), 24},
                                                     {a * std::sqrt(2.0), 12}};
    const double expected = pairEntropy(shells, rho, 5, .2, rho);
    check(allNear(s, expected, 1e-6 * std::abs(expected)), "fcc entropy matches the formula: " + std::to_string(s[0]) +
          " vs " + std::to_string(expected));
    check(expected < 0, "crystal entropy is negative");
    near(attribute(crystalFcc, "LocalEntropy.mean"), expected, 1e-6 * std::abs(expected), "mean entropy attribute");
    Dataset smallCell = fcc(1, a);
    LocalEntropyOptions smallAverage;
    smallAverage.average = true;
    computeLocalEntropy(smallCell, smallAverage, nullptr);
    check(allNear(smallCell.scalarProperties.at("Entropy"), expected, 1e-6 * std::abs(expected)),
          "a single fcc unit cell gives the bulk entropy through its images");
    // With local density, rho counts the 42 neighbors inside the cutoff sphere.
    Dataset local = fcc(4, a);
    options.useLocalDensity = true;
    computeLocalEntropy(local, options, nullptr);
    const double localRho = 42 / (4.0 / 3 * 3.14159265358979323846 * 125);
    const double expectedLocal = pairEntropy(shells, localRho, 5, .2, localRho);
    near(local.scalarProperties.at("Entropy")[7], expectedLocal, 1e-6 * std::abs(expectedLocal), "local-density entropy");
    // Disorder raises the entropy toward zero; wider smoothing does too.
    Dataset gas = randomGas(1500, {true, true, true}, 5, 25);
    computeLocalEntropy(gas, {}, nullptr);
    check(attribute(gas, "LocalEntropy.mean") > expected + 1, "random positions are less ordered than fcc");
    Dataset smooth = fcc(4, a);
    LocalEntropyOptions wide;
    wide.sigma = .4;
    computeLocalEntropy(smooth, wide, nullptr);
    check(smooth.scalarProperties.at("Entropy")[0] > expected, "wider Gaussians give a less negative fingerprint");
    // Averaging: (s_i + sum s_j) / (N_i + 1) over neighbors, checked by brute force on a vacancy.
    Dataset vacancy = fcc(4, a);
    vacancy.atoms.erase(vacancy.atoms.begin() + 21);
    Dataset plain = vacancy;
    computeLocalEntropy(plain, {}, nullptr);
    const auto &raw = plain.scalarProperties.at("Entropy");
    LocalEntropyOptions averaged;
    averaged.average = true;
    averaged.averageCutoff = 3.0;
    computeLocalEntropy(vacancy, averaged, nullptr);
    const double box = 4 * a;
    double worst = 0;
    for (size_t i = 0; i < vacancy.atoms.size(); ++i) {
        double sum = raw[i];
        int n = 0;
        for (size_t j = 0; j < vacancy.atoms.size(); ++j) {
            if (i == j) continue;
            double d2 = 0;
            for (int axis = 0; axis < 3; ++axis) {
                double dx = double(coordinate(vacancy.atoms[j], axis)) - coordinate(vacancy.atoms[i], axis);
                dx -= box * std::round(dx / box);
                d2 += dx * dx;
            }
            if (d2 < 9) { sum += raw[j]; ++n; }
        }
        worst = std::max(worst, std::abs(vacancy.scalarProperties.at("Entropy")[i] - sum / (n + 1)));
    }
    near(worst, 0, 1e-9, "neighbor averaging matches the brute-force mean");
    LocalEntropyOptions bad;
    bad.sigma = 6;
    throwsWith([&] { Dataset d = fcc(4); computeLocalEntropy(d, bad, nullptr); }, "sigma", "sigma must be below the cutoff");
    throwsWith([&] { Dataset d = fcc(4); d.cell = {}; d.pbc = {false, false, false}; computeLocalEntropy(d, {}, nullptr); },
               "simulation cell", "entropy needs a cell for the density");
}

// ---- fcc planar faults -----------------------------------------------------------
// Close-packed layers in the order of `stacking` (A, B, C), periodic along z.
static constexpr double faultBond = 2.55;
static constexpr int faultAtomsPerLayer = 48;
static Dataset stackedLayers(const std::string &stacking) {
    const double d = faultBond, h = d * std::sqrt(2.0 / 3.0), rowY = d * std::sqrt(3.0);
    Dataset data;
    data.species = {"Cu"};
    data.pbc = {true, true, true};
    data.cell = {6 * d, 0, 0, 0, 4 * rowY, 0, 0, 0, h * double(stacking.size())};
    for (size_t layer = 0; layer < stacking.size(); ++layer) {
        const int shift = stacking[layer] - 'A';
        const double ox = shift * d / 2, oy = shift * rowY / 6;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 6; ++x)
                for (int site = 0; site < 2; ++site) {
                    double px = ox + (x + .5 * site) * d, py = oy + (y + .5 * site) * rowY;
                    px -= std::floor(px / data.cell[0]) * data.cell[0];
                    py -= std::floor(py / data.cell[4]) * data.cell[4];
                    data.atoms.push_back({float(px), float(py), float((double(layer) + .25) * h), 0});
                }
    }
    return data;
}
static std::array<uint64_t, 5> faultCounts(Dataset &d) {
    computeFccPlanarFaults(d, nullptr);
    std::array<uint64_t, 5> counts{};
    for (double type : d.scalarProperties.at("Planar Fault Type")) ++counts[size_t(type)];
    return counts;
}
static void testPlanarFaults() {
    const double layerArea = faultAtomsPerLayer * std::sqrt(3.0) / 2 * faultBond * faultBond;
    Dataset perfect = stackedLayers("ABCABCABCABC");
    check(faultCounts(perfect)[0] == perfect.atoms.size(), "perfect fcc has no hcp-like atoms");
    // Two twin boundaries, each a single hcp-like layer (layers 0 and 6).
    Dataset twin = stackedLayers("ABCABCACBACB");
    const auto twins = faultCounts(twin);
    check(twins[3] == 2 * faultAtomsPerLayer && twins[0] == 10 * faultAtomsPerLayer,
          "twin boundaries are single hcp-like layers: " + std::to_string(twins[3]));
    near(attribute(twin, "PlanarFaults.area.Coherent twin boundary"), 2 * layerArea, 1e-3 * layerArea, "twin area");
    check(twin.scalarProperties.at("Planar Fault Type")[0] == 3 &&
          twin.scalarProperties.at("Planar Fault Type")[size_t(6 * faultAtomsPerLayer)] == 3,
          "the mirror layers carry the twin type");
    // An intrinsic stacking fault: two adjacent hcp-like layers (7 and 8).
    Dataset isf = stackedLayers("ABCABCABABCABC");
    const auto faults = faultCounts(isf);
    check(faults[2] == 2 * faultAtomsPerLayer && faults[3] == 0 && faults[4] == 0,
          "an ISF is two hcp-like layers: " + std::to_string(faults[2]));
    near(attribute(isf, "PlanarFaults.area.Intrinsic stacking fault"), layerArea, 1e-3 * layerArea,
         "ISF area counts the fault once");
    const auto &table = isf.tables.back();
    check(table.name == "Planar faults" && table.rows.size() == 5 && table.rows[2][1] == std::to_string(2 * faultAtomsPerLayer),
          "the planar fault table lists atom counts");
    // Four stacked hcp-like layers (7 to 10): every one is multi-layer.
    Dataset multi = stackedLayers("ABCABCABABABCABC");
    const auto many = faultCounts(multi);
    check(many[4] == 4 * faultAtomsPerLayer && many[2] == 0, "thick faults are multi-layer: " + std::to_string(many[4]));
    // Three layers (6 to 8) is the thinnest multi-layer fault. Periodic stacking needs an
    // even number of hcp-like layers, so layer 0 closes the cycle as a twin boundary.
    Dataset three = stackedLayers("ABCABCACACBACB");
    const auto thin = faultCounts(three);
    check(thin[4] == 3 * faultAtomsPerLayer && thin[3] == faultAtomsPerLayer && thin[2] == 0,
          "three adjacent hcp-like layers are multi-layer, next to a separate twin");
    // PTM's hcp correspondence: template points 1-6 are the basal neighbors and 7-9, 10-12
    // the layers on opposite sides.
    const Dataset h = hcp(3);
    const auto lists = nearestNeighborsWithImages(h, 12, nullptr);
    const auto env = ptmClassify(lists, 0, PtmOptions{});
    check(env.type == PtmType::HCP, "an ideal hcp atom is hcp-like");
    bool basal = env.type == PtmType::HCP;
    for (size_t point = 1; point <= 12 && basal; ++point) {
        const double z = lists[0][size_t(env.mapping[point]) - 1].vector[2];
        basal = point <= 6 ? std::abs(z) < 1e-3 : std::abs(z) > .5;
    }
    double sides = 0;
    for (size_t point = 7; point <= 9; ++point) sides += lists[0][size_t(env.mapping[point]) - 1].vector[2];
    for (size_t point = 10; point <= 12; ++point) sides -= lists[0][size_t(env.mapping[point]) - 1].vector[2];
    check(basal && std::abs(sides) > 1, "basal neighbors lie in the plane and the two layers on opposite sides");
    // A reduced periodic cell (one stacking period of 6 layers) still has full neighbor shells.
    Dataset period = stackedLayers("ABCACB");
    const auto periodic = faultCounts(period);
    check(periodic[3] == 2 * faultAtomsPerLayer, "a 6-layer periodic cell with two twins: " + std::to_string(periodic[3]));
    // A small random displacement keeps the classification.
    Dataset shaken = stackedLayers("ABCABCABABCABC");
    std::mt19937 random(9);
    std::normal_distribution<float> jitter(0, .03f);
    for (auto &atom : shaken.atoms) { atom.x += jitter(random); atom.y += jitter(random); atom.z += jitter(random); }
    check(faultCounts(shaken)[2] == 2 * faultAtomsPerLayer, "thermal noise keeps the ISF");
}

// ---- Spatial correlation ---------------------------------------------------------
static const DataTable &tableNamed(const Dataset &d, const std::string &name) {
    for (const auto &table : d.tables) if (table.name == name) return table;
    static const DataTable empty;
    check(false, "missing table " + name);
    return empty;
}
static std::vector<double> column(const DataTable &table, size_t index) {
    std::vector<double> values;
    for (const auto &row : table.rows) values.push_back(std::stod(row.at(index)));
    return values;
}
static void testSpatialCorrelation() {
    // One particle at the center of every grid cell: the density is uniform, so C(r) = 1
    // everywhere and C(k) = 0 for every k != 0.
    Dataset lattice = simpleCubic(6, 3.0);
    for (auto &atom : lattice.atoms) { atom.x += 1.5f; atom.y += 1.5f; atom.z += 1.5f; }
    computeSpatialCorrelation(lattice, {}, nullptr);
    const auto real = column(tableNamed(lattice, "Spatial correlation (real space)"), 1);
    check(!real.empty() && allNear(real, 1, 1e-9), "a uniform grid density correlates to 1");
    const auto reciprocal = column(tableNamed(lattice, "Spatial correlation (reciprocal space)"), 1);
    check(!reciprocal.empty() && allNear(reciprocal, 0, 1e-9), "a uniform grid density has no k != 0 structure");

    // Rock salt with charges +-1: the neighbor sum normalized by the RDF alternates
    // -1, +1, -1 over the first three shells.
    const double a = 5.64;
    Dataset salt = crystal({{0, 0, 0}, {0, .5, .5}, {.5, 0, .5}, {.5, .5, 0},
                            {.5, 0, 0}, {0, .5, 0}, {0, 0, .5}, {.5, .5, .5}}, {a, a, a}, 3);
    std::vector<double> charge(salt.atoms.size());
    for (size_t i = 0; i < charge.size(); ++i) charge[i] = i % 8 < 4 ? 1 : -1;
    salt.scalarProperties["Charge"] = charge;
    CorrelationOptions options;
    options.property1 = options.property2 = "Charge";
    options.directSummation = true;
    options.normalizeByRdf = true;
    Dataset value = salt;
    computeSpatialCorrelation(value, options, nullptr);
    const auto &direct = tableNamed(value, "Spatial correlation (neighbor sum)");
    check(direct.rows.size() == 50, "50 neighbor bins by default");
    const auto normalized = column(direct, 3), rdf = column(direct, 2);
    const auto binOf = [](double r) { return size_t(r / 0.1); };
    near(normalized[binOf(a / 2)], -1, 1e-12, "first shell: unlike charges");
    near(normalized[binOf(a / std::sqrt(2.0))], 1, 1e-12, "second shell: like charges");
    near(normalized[binOf(a * std::sqrt(3.0) / 2)], -1, 1e-12, "third shell: unlike charges");
    const double rho = 8 / (a * a * a), step = .1, r1 = binOf(a / 2) * step;
    near(rdf[binOf(a / 2)], 6 / (rho * 4.0 / 3 * 3.14159265358979323846 * (std::pow(r1 + step, 3) - std::pow(r1, 3))),
         1e-5, "the RDF counts six first neighbors in their shell");
    near(attribute(value, "CorrelationFunction.mean1"), 0, 1e-12, "mean charge is zero");
    near(attribute(value, "CorrelationFunction.covariance"), 1, 1e-12, "<P1 P2> is one");
    Dataset unitSalt = crystal({{0, 0, 0}, {0, .5, .5}, {.5, 0, .5}, {.5, .5, 0},
                                {.5, 0, 0}, {0, .5, 0}, {0, 0, .5}, {.5, .5, .5}}, {a, a, a}, 1);
    unitSalt.scalarProperties["Charge"] = std::vector<double>{1, 1, 1, 1, -1, -1, -1, -1};
    computeSpatialCorrelation(unitSalt, options, nullptr);
    const auto unitNormalized = column(tableNamed(unitSalt, "Spatial correlation (neighbor sum)"), 3);
    near(unitNormalized[binOf(a / 2)], -1, 1e-12, "the neighbor sum sees images of a small cell");
    near(column(tableNamed(unitSalt, "Spatial correlation (neighbor sum)"), 2)[binOf(a / 2)], rdf[binOf(a / 2)],
         1e-5 * rdf[binOf(a / 2)], "a unit cell gives the bulk RDF");
    options.differenceCorrelation = true;
    Dataset difference = salt;
    computeSpatialCorrelation(difference, options, nullptr);
    near(column(tableNamed(difference, "Spatial correlation (neighbor sum)"), 3)[binOf(a / 2)], 2, 1e-12,
         "difference correlation (<P1^2> + <P2^2>)/2 - C");
    options.differenceCorrelation = false;
    options.normalizeByRdf = false;
    options.normalizeByCovariance = true;
    Dataset covariance = salt;
    computeSpatialCorrelation(covariance, options, nullptr);
    const auto byCovariance = tableNamed(covariance, "Spatial correlation (neighbor sum)");
    near(column(byCovariance, 3)[binOf(a / 2)], column(byCovariance, 1)[binOf(a / 2)], 1e-12,
         "covariance normalization with zero means and unit variance changes nothing");

    // Ideal-gas positions: g(r) and S(k) both tend to 1.
    Dataset gas = randomGas(6000, {true, true, true}, 21, 24);
    CorrelationOptions gasOptions;
    gasOptions.directSummation = true;
    gasOptions.gridSpacing = 2;
    computeSpatialCorrelation(gas, gasOptions, nullptr);
    const auto gasDirect = column(tableNamed(gas, "Spatial correlation (neighbor sum)"), 2);
    near(std::accumulate(gasDirect.begin() + 20, gasDirect.end(), 0.0) / double(gasDirect.size() - 20), 1, .02,
         "ideal-gas RDF from the neighbor sum");
    const auto gasReal = column(tableNamed(gas, "Spatial correlation (real space)"), 1);
    near(std::accumulate(gasReal.begin() + 1, gasReal.end(), 0.0) / double(gasReal.size() - 1), 1, .03,
         "ideal-gas C(r) from the FFT");
    const auto gasK = column(tableNamed(gas, "Spatial correlation (reciprocal space)"), 1);
    near(std::accumulate(gasK.begin(), gasK.end(), 0.0) / double(gasK.size()), 1, .1, "ideal-gas S(k) from the FFT");
    // Without periodicity the padding and window are divided out again.
    Dataset open = randomGas(6000, {false, false, false}, 22, 24);
    computeSpatialCorrelation(open, gasOptions, nullptr);
    const auto openReal = column(tableNamed(open, "Spatial correlation (real space)"), 1);
    near(std::accumulate(openReal.begin() + 1, openReal.end(), 0.0) / double(openReal.size() - 1), 1, .05,
         "windowed non-periodic C(r)");
    CorrelationOptions noWindow = gasOptions;
    noWindow.applyWindow = false;
    Dataset openPlain = randomGas(6000, {false, false, false}, 22, 24);
    computeSpatialCorrelation(openPlain, noWindow, nullptr);
    const auto plainReal = column(tableNamed(openPlain, "Spatial correlation (real space)"), 1);
    near(std::accumulate(plainReal.begin() + 1, plainReal.end(), 0.0) / double(plainReal.size() - 1), 1, .05,
         "zero-padded non-periodic C(r)");

    CorrelationOptions bad;
    bad.gridSpacing = 0;
    throwsWith([&] { Dataset d = fcc(3); computeSpatialCorrelation(d, bad, nullptr); }, "grid spacing", "spacing must be positive");
    CorrelationOptions missing;
    missing.property1 = "Nope";
    throwsWith([&] { Dataset d = fcc(3); computeSpatialCorrelation(d, missing, nullptr); }, "Unknown particle property",
               "unknown property");
    CorrelationOptions fine;
    fine.gridSpacing = 0.001;
    throwsWith([&] { Dataset d = fcc(3); computeSpatialCorrelation(d, fine, nullptr); }, "too fine", "grid size is bounded");

    // Through the pipeline, with the panel's field mapping.
    Modifier correlate{Op::SpatialCorrelation};
    correlate.property = "Charge";
    correlate.scatterYProperty = "Charge";
    correlate.value = 3;
    correlate.adaptive = true;
    correlate.clusterByBonds = true;
    correlate.upper = 5;
    correlate.rdfBins = 50;
    correlate.reduceOperation = 0;
    correlate.clusterSortBySize = true;
    std::vector<Modifier> stack{correlate};
    const auto evaluated = evaluateFrom(resultOf(salt), stack, 0);
    near(column(tableNamed(evaluated.data, "Spatial correlation (neighbor sum)"), 3)[binOf(a / 2)], -1, 1e-12,
         "the pipeline node maps its fields");
    Modifier chill{Op::ChillPlus};
    chill.value = 3.5f;
    Modifier entropy{Op::LocalEntropy};
    entropy.value = 5; entropy.upper = .2f; entropy.histogramRangeStart = 5;
    Modifier faults{Op::IdentifyFccPlanarFaults};
    std::vector<Modifier> structure{chill, entropy, faults};
    const auto all = evaluateFrom(resultOf(stackedLayers("ABCABCABABCABC")), structure, 0);
    check(all.data.scalarProperties.count("Structure Type") && all.data.scalarProperties.count("Entropy") &&
          all.data.scalarProperties.count("Planar Fault Type"), "the structure nodes chain in a pipeline");
    for (Op op : {Op::ChillPlus, Op::LocalEntropy, Op::IdentifyFccPlanarFaults, Op::SpatialCorrelation})
        check(!modifierOutputs(Modifier{op}, 0).empty() && std::string(opName(op)) != "Color by type",
              std::string("named with outputs: ") + opName(op));
}

// ---- Polyhedral template matching ------------------------------------------------
static constexpr int ptmAll = 0xff;
static Dataset ptmRun(Dataset d, int structures = ptmDefaultStructures, double cutoff = 0.1, bool everything = true,
                      const std::vector<uint8_t> *selection = nullptr) {
    PtmOptions options;
    options.structures = structures;
    options.rmsdCutoff = cutoff;
    options.outputRmsd = options.outputInteratomicDistance = options.outputOrientation =
        options.outputDeformationGradient = everything;
    computePolyhedralTemplateMatching(d, options, selection, nullptr);
    return d;
}
static size_t countType(const Dataset &d, PtmType type) {
    const auto &s = d.scalarProperties.at("Structure Type");
    return size_t(std::count(s.begin(), s.end(), double(type)));
}
static Dataset graphene(int nx, int ny, double bond = 1.42) {
    Dataset d;
    d.species = {"C"};
    const double w = bond * std::sqrt(3.0);
    d.cell = {w * nx, 0, 0, 0, 3 * bond * ny, 0, 0, 0, 20};
    d.pbc = {true, true, false};
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x)
            for (const auto &[px, py] : {std::pair{0.0, 0.0}, std::pair{0.0, bond}, std::pair{w / 2, 1.5 * bond},
                                        std::pair{w / 2, 2.5 * bond}})
                d.atoms.push_back({float(x * w + px), float(y * 3 * bond + py), 10.f, 0});
    return d;
}
static void testPtm() {
    // Every ideal structure is found with zero RMSD and its nearest-neighbor distance.
    const double a = 3.6;
    const Dataset f = ptmRun(fcc(4, a));
    check(countType(f, PtmType::FCC) == f.atoms.size(), "ideal fcc is FCC");
    check(allNear(f.scalarProperties.at("RMSD"), 0, 1e-6), "ideal fcc has zero RMSD");
    check(allNear(f.scalarProperties.at("Interatomic Distance"), a / std::sqrt(2.0), 1e-5), "fcc interatomic distance a/sqrt2");
    near(attribute(f, "PolyhedralTemplateMatching.counts.FCC"), double(f.atoms.size()), 0, "FCC count attribute");
    const Dataset h = ptmRun(hcp(3, 3.0));
    check(countType(h, PtmType::HCP) == h.atoms.size(), "ideal hcp is HCP on both lattice sites");
    check(allNear(h.scalarProperties.at("Interatomic Distance"), 3.0, 1e-5), "hcp interatomic distance a");
    const Dataset b = ptmRun(bcc(4, 3.0));
    check(countType(b, PtmType::BCC) == b.atoms.size(), "ideal bcc is BCC");
    check(allNear(b.scalarProperties.at("Interatomic Distance"), 3.0 * std::sqrt(3.0) / 2, 1e-5), "bcc distance a sqrt3/2");
    const Dataset sc = ptmRun(simpleCubic(5, 3.0), ptmAll);
    check(countType(sc, PtmType::SC) == sc.atoms.size(), "ideal simple cubic is SC");
    const Dataset ico = ptmRun(icosahedron(), ptmAll);
    check(ico.scalarProperties.at("Structure Type")[0] == double(PtmType::ICO), "an icosahedron center is ICO");
    const Dataset dc = ptmRun(cubicDiamond(3), ptmAll);
    check(countType(dc, PtmType::CubicDiamond) == dc.atoms.size(), "ideal cubic diamond");
    check(allNear(dc.scalarProperties.at("Interatomic Distance"), 5.43 * std::sqrt(3.0) / 4, 1e-4), "diamond bond length");
    const Dataset dh = ptmRun(hexagonalDiamond(3), ptmAll);
    check(countType(dh, PtmType::HexagonalDiamond) == dh.atoms.size(), "ideal hexagonal diamond");
    const Dataset g = ptmRun(graphene(6, 4), ptmAll);
    check(countType(g, PtmType::Graphene) == g.atoms.size(), "ideal graphene");
    check(allNear(g.scalarProperties.at("Interatomic Distance"), 1.42, 1e-4), "graphene bond length");
    // Disabled structures are not reported.
    check(countType(ptmRun(bcc(4)), PtmType::BCC) == 128 && countType(ptmRun(bcc(4), 1), PtmType::Other) == 128,
          "only the enabled structure types are assigned");
    // Orientation: identity for the template frame, the same on both hcp sites, and the
    // applied rotation for a rotated crystal.
    check(allNear(f.scalarProperties.at("Orientation.W"), 1, 1e-6) && allNear(f.scalarProperties.at("Orientation.X"), 0, 1e-6),
          "an unrotated fcc crystal has the identity orientation");
    check(allNear(h.scalarProperties.at("Orientation.W"), 1, 1e-6), "both hcp sites share the crystal orientation");
    Dataset rotated = fcc(4, a);
    const double angle = 20 * 3.14159265358979323846 / 180, n = std::sqrt(14.0);
    const double qw = std::cos(angle / 2), qx = std::sin(angle / 2) / n, qy = 2 * qx, qz = 3 * qx;
    const double R[3][3]{{1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qw * qz), 2 * (qx * qz + qw * qy)},
                         {2 * (qx * qy + qw * qz), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qw * qx)},
                         {2 * (qx * qz - qw * qy), 2 * (qy * qz + qw * qx), 1 - 2 * (qx * qx + qy * qy)}};
    transform(rotated, R);
    const Dataset turned = ptmRun(rotated);
    check(countType(turned, PtmType::FCC) == turned.atoms.size(), "a rotated crystal is still FCC");
    check(allNear(turned.scalarProperties.at("Orientation.W"), qw, 1e-5) &&
          allNear(turned.scalarProperties.at("Orientation.X"), qx, 1e-5) &&
          allNear(turned.scalarProperties.at("Orientation.Y"), qy, 1e-5) &&
          allNear(turned.scalarProperties.at("Orientation.Z"), qz, 1e-5),
          "the orientation is the applied rotation: w=" + std::to_string(turned.scalarProperties.at("Orientation.W")[0]));
    check(allNear(turned.scalarProperties.at("Elastic Deformation Gradient.XY"), R[0][1], 1e-5) &&
          allNear(turned.scalarProperties.at("Elastic Deformation Gradient.ZX"), R[2][0], 1e-5),
          "an unstrained rotated crystal has the rotation as its deformation gradient");
    // A uniaxial stretch: the deformation gradient keeps the 1.01 : 1 : 1 ratio.
    Dataset stretched = fcc(4, a);
    const double stretch[3][3]{{1.01, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    transform(stretched, stretch);
    const Dataset strained = ptmRun(stretched);
    const auto &fxx = strained.scalarProperties.at("Elastic Deformation Gradient.XX");
    const auto &fyy = strained.scalarProperties.at("Elastic Deformation Gradient.YY");
    near(fxx[0] / fyy[0], 1.01, 1e-5, "the elastic deformation gradient carries the stretch");
    near(strained.scalarProperties.at("Elastic Deformation Gradient.XY")[0], 0, 1e-5, "no shear from a stretch");
    // RMSD in closed form: moving a center by d (in nearest-neighbor units) against its
    // fixed fcc shell gives RMSD^2 = (12/13) (d^2/13) / (1 + d^2/13).
    Dataset moved = fcc(4, a);
    const double d = 0.1, nn = a / std::sqrt(2.0);
    moved.atoms[0].x += float(d * nn);
    const double expected = std::sqrt(12.0 / 13 * (d * d / 13) / (1 + d * d / 13));
    near(ptmRun(moved).scalarProperties.at("RMSD")[0], expected, 2e-4, "RMSD of a displaced center");
    // Thermal noise: a cutoff of 0.1 keeps the crystal, a tiny one rejects it.
    Dataset noisy = fcc(5, a);
    std::mt19937 random(4);
    std::normal_distribution<float> jitter(0, float(.03 * nn));
    for (auto &atom : noisy.atoms) { atom.x += jitter(random); atom.y += jitter(random); atom.z += jitter(random); }
    const Dataset warm = ptmRun(noisy);
    check(countType(warm, PtmType::FCC) > warm.atoms.size() * 95 / 100,
          "3% thermal noise stays FCC: " + std::to_string(countType(warm, PtmType::FCC)));
    const Dataset strict = ptmRun(noisy, ptmDefaultStructures, 1e-4);
    check(countType(strict, PtmType::Other) == strict.atoms.size(), "a tiny RMSD cutoff rejects noisy atoms");
    check(std::all_of(strict.scalarProperties.at("RMSD").begin(), strict.scalarProperties.at("RMSD").end(),
                      [](double v) { return v > 0; }), "rejected atoms keep their RMSD");
    check(countType(ptmRun(noisy, ptmDefaultStructures, 0), PtmType::FCC) == noisy.atoms.size(), "cutoff 0 disables the cutoff");
    // Random positions are mostly not crystalline.
    const Dataset gas = ptmRun(randomGas(500, {true, true, true}, 8, 18));
    check(countType(gas, PtmType::Other) > 400, "random positions are mostly Other");
    // Small periodic cells, selections, tables.
    check(countType(ptmRun(fcc(1, a)), PtmType::FCC) == 4, "a single fcc unit cell uses its periodic images");
    std::vector<uint8_t> half(256, 0);
    for (size_t i = 0; i < 128; ++i) half[i] = 1;
    const Dataset partial = ptmRun(fcc(4, a), ptmDefaultStructures, 0.1, false, &half);
    check(countType(partial, PtmType::Other) >= 128, "unselected particles are Other");
    check(!partial.scalarProperties.count("RMSD"), "optional outputs stay off unless requested");
    check(tableNamed(f, "Polyhedral template matching").rows.size() == 9 && tableNamed(f, "PTM RMSD histogram").rows.size() == 100,
          "PTM publishes the structure and RMSD tables");
    throwsWith([&] { ptmRun(fcc(2), 0); }, "at least one structure", "PTM needs a structure type");
    // Through the pipeline with the panel's field mapping.
    Modifier ptm{Op::PolyhedralTemplateMatching};
    ptm.value = .1f;
    ptm.expandNeighbors = ptmDefaultStructures;
    ptm.clusterByBonds = true;
    std::vector<Modifier> stack{ptm};
    const auto evaluated = evaluateFrom(resultOf(bcc(4)), stack, 0);
    check(countType(evaluated.data, PtmType::BCC) == 128 && evaluated.data.scalarProperties.count("Orientation.W") &&
          !evaluated.data.scalarProperties.count("RMSD"), "the PTM node maps its fields");
}


// ---- Grain segmentation ----------------------------------------------------------
// An fcc matrix with a spherical grain rotated by `angle` degrees about z.
static Dataset sphereGrain(double angle, int n = 10, double a = 3.6, double radius = 10) {
    Dataset d = fcc(n, a);
    const double L = n * a, c = L / 2, t = angle * 3.14159265358979323846 / 180;
    std::vector<Atom> kept;
    for (const auto &atom : d.atoms)
        if (std::hypot(atom.x - c, atom.y - c, atom.z - c) >= radius) kept.push_back(atom);
    const size_t matrix = kept.size();
    const std::array<std::array<double, 3>, 4> basis{{{0, 0, 0}, {0, .5, .5}, {.5, 0, .5}, {.5, .5, 0}}};
    const int reach = int(radius / a) + 2;
    for (int z = -reach; z <= reach; ++z)
        for (int y = -reach; y <= reach; ++y)
            for (int x = -reach; x <= reach; ++x)
                for (const auto &b : basis) {
                    const double px = (x + b[0]) * a, py = (y + b[1]) * a, pz = (z + b[2]) * a;
                    if (std::hypot(px, py, pz) >= radius) continue;
                    const double rx = std::cos(t) * px - std::sin(t) * py, ry = std::sin(t) * px + std::cos(t) * py;
                    bool clash = false;
                    for (size_t i = 0; i < matrix && !clash; ++i)
                        clash = std::hypot(kept[i].x - (c + rx), kept[i].y - (c + ry), kept[i].z - (c + pz)) < 1.6;
                    if (!clash) kept.push_back({float(c + rx), float(c + ry), float(c + pz), 0});
                }
    d.atoms = std::move(kept);
    return d;
}
static Dataset grainsOf(Dataset d, GrainSegmentationOptions options = {}, int structures = ptmDefaultStructures) {
    PtmOptions ptm;
    ptm.structures = structures;
    ptm.outputOrientation = true;
    computePolyhedralTemplateMatching(d, ptm, nullptr, nullptr);
    computeGrainSegmentation(d, options, nullptr);
    return d;
}
static size_t grainCount(const Dataset &d) { return size_t(attribute(d, "GrainSegmentation.grain_count")); }
static size_t unassigned(const Dataset &d) {
    const auto &g = d.scalarProperties.at("Grain");
    return size_t(std::count(g.begin(), g.end(), -1.0));
}
static void testGrainSegmentation() {
    // A perfect crystal is one grain that wraps through the periodic cell.
    const Dataset single = grainsOf(fcc(6));
    check(grainCount(single) == 1 && unassigned(single) == 0, "a perfect crystal is one grain");
    const auto &grains = tableNamed(single, "grains");
    check(grains.rows.size() == 1 && grains.rows[0][1] == std::to_string(single.atoms.size()) && grains.rows[0][3] == "FCC",
          "the grain table lists the whole crystal as FCC");
    if (!grains.rows.empty()) {
        check(grains.rows[0][8] == "nan", "a grain connected to its own periodic image has no defined center");
        near(std::abs(std::stod(grains.rows[0][7])), 1, 1e-6, "the mean orientation of an unrotated crystal is the identity");
    }
    check(single.particleColors.size() == single.atoms.size() && single.particleColors[0].x >= 0,
          "particles are colored by grain");

    // A rotated spherical grain inside a matrix: two grains with every algorithm.
    const Dataset sphere = sphereGrain(25);
    for (int algorithm = 0; algorithm < 3; ++algorithm) {
        GrainSegmentationOptions options;
        options.algorithm = GrainAlgorithm(algorithm);
        options.logThreshold = algorithm == 2 ? std::log(5.0) : 12;
        const Dataset g = grainsOf(sphere, options);
        check(grainCount(g) == 2, "a rotated sphere is a second grain, algorithm " + std::to_string(algorithm) + ": " +
                                      std::to_string(grainCount(g)));
        check(unassigned(g) == 0, "orphan adoption assigns every atom, algorithm " + std::to_string(algorithm));
        const auto &table = tableNamed(g, "grains");
        if (table.rows.size() == 2) {
            const double inner = std::stod(table.rows[1][1]);
            check(inner > 250 && inner < 450, "the sphere grain has about 360 atoms: " + table.rows[1][1]);
            const double cx = std::stod(table.rows[1][8]), cy = std::stod(table.rows[1][9]), cz = std::stod(table.rows[1][10]);
            near(std::hypot(cx - 18, cy - 18, cz - 18), 0, .5, "the sphere grain is centered");
            near(std::stod(table.rows[1][11]), 10, 1.5, "the sphere grain radius");
            near(std::stod(table.rows[1][12]), std::sqrt(.6) * 10, 1, "the sphere radius of gyration");
            // Misorientation of the mean orientations: 25 degrees about z.
            const ptm_detail::Quat q0{std::stod(table.rows[0][7]), std::stod(table.rows[0][4]), std::stod(table.rows[0][5]), std::stod(table.rows[0][6])};
            const ptm_detail::Quat q1{std::stod(table.rows[1][7]), std::stod(table.rows[1][4]), std::stod(table.rows[1][5]), std::stod(table.rows[1][6])};
            near(grain_detail::misorientation(q0, q1, ptm_detail::library().cubic), 25, .5, "grain misorientation");
        }
        const auto &merge = tableNamed(g, "grains-merge");
        check(merge.columns.size() == 2 && merge.rows.size() > 100, "the merge sequence is published");
    }
    // Without adoption the grain-boundary atoms stay unassigned; a huge minimum dissolves all.
    GrainSegmentationOptions strict;
    strict.adoptOrphans = false;
    const Dataset bare = grainsOf(sphere, strict);
    check(unassigned(bare) > 0 && grainCount(bare) == 2, "without adoption boundary atoms have no grain");
    strict.minGrainSize = 100000;
    const Dataset none = grainsOf(sphere, strict);
    check(grainCount(none) == 0 && unassigned(none) == sphere.atoms.size(), "grains below the minimum size dissolve");
    // A low manual threshold splits the matrix into many fragments.
    GrainSegmentationOptions low;
    low.algorithm = GrainAlgorithm::GraphClusteringManual;
    low.logThreshold = -6;
    low.minGrainSize = 5;
    check(grainCount(grainsOf(sphere, low)) > 2, "a low merge threshold over-segments");

    // Stacking faults: the hcp layers of an ISF belong to the surrounding fcc grain, while
    // twin boundaries separate the twins.
    GrainSegmentationOptions small;
    small.minGrainSize = 20;
    const Dataset isf = grainsOf(stackedLayers("ABCABCABABCABC"), small);
    check(grainCount(isf) == 1 && unassigned(isf) == 0, "an intrinsic stacking fault stays inside one grain: " +
                                                            std::to_string(grainCount(isf)));
    small.handleStackingFaults = false;
    const Dataset split = grainsOf(stackedLayers("ABCABCABABCABC"), small);
    check(grainCount(split) == 2 && tableNamed(split, "grains").rows.size() == 2 && tableNamed(split, "grains").rows[1][3] == "HCP",
          "without stacking-fault handling the hcp layers form their own phase");
    small.handleStackingFaults = true;
    const Dataset twin = grainsOf(stackedLayers("ABCABCACBACB"), small);
    check(grainCount(twin) == 2 && unassigned(twin) == 0, "twin boundaries separate two twin grains: " +
                                                              std::to_string(grainCount(twin)));
    if (tableNamed(twin, "grains").rows.size() == 2) {
        const auto &t = tableNamed(twin, "grains").rows;
        const ptm_detail::Quat q0{std::stod(t[0][7]), std::stod(t[0][4]), std::stod(t[0][5]), std::stod(t[0][6])};
        const ptm_detail::Quat q1{std::stod(t[1][7]), std::stod(t[1][4]), std::stod(t[1][5]), std::stod(t[1][6])};
        near(grain_detail::misorientation(q0, q1, ptm_detail::library().cubic), 60, .5, "twins are 60 degrees apart");
    }
    // The cubic-to-hexagonal frame maps the fcc (111) neighbors onto the hcp basal ones.
    {
        const auto &lib = ptm_detail::library();
        const auto m = ptm_detail::matrixOf(grain_detail::conjugate(grain_detail::cubicToHexagonal(false)));
        size_t matched = 0;
        for (const auto &h : lib.templates[2].points) {
            if (std::abs(h[2]) > 1e-9 || ptm_detail::norm(h) < .5) continue;
            for (const auto &f : lib.templates[1].points) {
                const auto r = ptm_detail::rotateBy(m, h);
                if (ptm_detail::norm({r[0] - f[0], r[1] - f[1], r[2] - f[2]}) < 1e-9) ++matched;
            }
        }
        check(matched == 6, "the six basal hcp neighbors are fcc (111) neighbors");
    }
    // Errors and cancellation.
    Dataset plain = fcc(3);
    throwsWith([&] { computeGrainSegmentation(plain, {}, nullptr); }, "Polyhedral template matching", "grains need PTM");
    Dataset ready = ptmRun(fcc(4));
    std::atomic<bool> cancel{true};
    throwsWith([&] { computeGrainSegmentation(ready, {}, &cancel); }, "Cancelled", "grain segmentation cancels");
}

// ---- Elastic strain --------------------------------------------------------------
static Dataset elasticOf(Dataset d, ElasticLattice lattice, double a0, bool pushForward = true, double ratio = std::sqrt(8.0 / 3.0)) {
    ElasticStrainOptions options;
    options.lattice = lattice;
    options.latticeConstant = a0;
    options.axialRatio = ratio;
    options.pushForward = pushForward;
    options.outputDeformationGradient = true;
    computeElasticStrain(d, options, nullptr);
    return d;
}
static double maxAbs(const Dataset &d, const std::string &name) {
    double m = 0;
    for (double v : d.scalarProperties.at(name)) m = std::max(m, std::abs(v));
    return m;
}
static double maxStrain(const Dataset &d) {
    double m = 0;
    for (const char *c : {"XX", "YY", "ZZ", "XY", "XZ", "YZ"}) m = std::max(m, maxAbs(d, std::string("Elastic Strain.") + c));
    return m;
}
static double meanOf(const Dataset &d, const std::string &name) {
    const auto &v = d.scalarProperties.at(name);
    double s = 0;
    for (double x : v) s += x;
    return v.empty() ? 0 : s / double(v.size());
}
static void testElasticStrain() {
    // Ideal crystals at their lattice constant carry no strain.
    struct Case { Dataset d; ElasticLattice lattice; double a0; int code; const char *what; };
    std::vector<Case> cases;
    cases.push_back({fcc(4, 3.6), ElasticLattice::FCC, 3.6, 1, "fcc"});
    cases.push_back({bcc(4, 3.0), ElasticLattice::BCC, 3.0, 3, "bcc"});
    cases.push_back({hcp(4, 3.0), ElasticLattice::HCP, 3.0, 2, "hcp"});
    cases.push_back({cubicDiamond(3, 5.43), ElasticLattice::CubicDiamond, 5.43, 4, "cubic diamond"});
    cases.push_back({hexagonalDiamond(3, 2.35), ElasticLattice::HexagonalDiamond, 2.35 * std::sqrt(8.0 / 3.0), 5, "hexagonal diamond"});
    for (auto &c : cases) {
        const Dataset e = elasticOf(c.d, c.lattice, c.a0);
        const auto &code = e.scalarProperties.at("Structure Type");
        check(std::all_of(code.begin(), code.end(), [&](double v) { return v == c.code; }),
              std::string("ideal ") + c.what + " has structure code " + std::to_string(c.code));
        near(maxStrain(e), 0, 1e-5, std::string("ideal ") + c.what + " is unstrained");
        near(e.scalarProperties.at("Elastic Deformation Gradient.XX")[0], 1, 1e-5, std::string(c.what) + " F is the identity");
    }
    // Uniform dilation by 1%: Euler-Almansi (1 - 1/s^2)/2 and Green-Lagrange (s^2 - 1)/2.
    Dataset dilated = fcc(4, 3.6 * 1.01);
    near(meanOf(elasticOf(dilated, ElasticLattice::FCC, 3.6), "Volumetric Strain"), (1 - 1 / (1.01 * 1.01)) / 2, 1e-5,
         "dilation, spatial frame");
    near(meanOf(elasticOf(dilated, ElasticLattice::FCC, 3.6, false), "Volumetric Strain"), (1.01 * 1.01 - 1) / 2, 1e-5,
         "dilation, material frame");
    // A wrong lattice constant reads as a uniform strain.
    near(meanOf(elasticOf(fcc(4, 3.6), ElasticLattice::FCC, 3.5, false), "Volumetric Strain"),
         (std::pow(3.6 / 3.5, 2) - 1) / 2, 1e-5, "lattice constant mismatch");
    // Uniaxial stretch of a rotated crystal: the spatial-frame strain is along x only.
    Dataset rotated = fcc(4, 3.6);
    const double t = 30 * 3.14159265358979323846 / 180;
    const double rotation[3][3]{{std::cos(t), -std::sin(t), 0}, {std::sin(t), std::cos(t), 0}, {0, 0, 1}};
    const double stretch[3][3]{{1.02, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    transform(rotated, rotation);
    transform(rotated, stretch);
    const Dataset s = elasticOf(rotated, ElasticLattice::FCC, 3.6);
    near(meanOf(s, "Elastic Strain.XX"), (1 - 1 / (1.02 * 1.02)) / 2, 1e-5, "uniaxial strain XX");
    near(maxAbs(s, "Elastic Strain.YY"), 0, 1e-5, "uniaxial strain YY");
    near(maxAbs(s, "Elastic Strain.XY"), 0, 1e-5, "uniaxial strain XY");
    near(meanOf(s, "Volumetric Strain"), (1 - 1 / (1.02 * 1.02)) / 6, 1e-5, "uniaxial volumetric strain");
    // Green-Lagrange of the same crystal: the material-frame trace is (1.02^2 - 1) / 2.
    const Dataset g = elasticOf(rotated, ElasticLattice::FCC, 3.6, false);
    near(3 * meanOf(g, "Volumetric Strain"), (1.02 * 1.02 - 1) / 2, 1e-5, "material-frame trace");
    // hcp with c/a = 1.6 is unstrained only when the ratio is given.
    Dataset squat = crystal({{0, 0, 0}, {.5, .5, 0}, {.5, 1.0 / 6, .5}, {0, 2.0 / 3, .5}}, {3.0, 3.0 * std::sqrt(3.0), 3.0 * 1.6}, 4);
    near(maxStrain(elasticOf(squat, ElasticLattice::HCP, 3.0, true, 1.6)), 0, 1e-5, "hcp at its own c/a is unstrained");
    near(meanOf(elasticOf(squat, ElasticLattice::HCP, 3.0, true), "Elastic Strain.ZZ"),
         (1 - std::pow(std::sqrt(8.0 / 3.0) / 1.6, 2)) / 2, 1e-4, "hcp c/a mismatch is a ZZ strain");
    // hcp-like stacking-fault atoms in fcc are crystalline (code 2) and unstrained.
    const Dataset fault = elasticOf(stackedLayers("ABCABCABABCABC"), ElasticLattice::FCC, faultBond * std::sqrt(2.0));
    const auto &codes = fault.scalarProperties.at("Structure Type");
    check(size_t(std::count(codes.begin(), codes.end(), 2.0)) == 2 * faultAtomsPerLayer, "stacking-fault atoms are hcp");
    near(maxStrain(fault), 0, 1e-5, "a stacking fault is not elastic strain");
    // The wrong lattice leaves atoms as Other with zero strain.
    const Dataset wrong = elasticOf(bcc(4), ElasticLattice::FCC, 3.0);
    check(attribute(wrong, "ElasticStrain.counts.Other") == double(wrong.atoms.size()) && maxStrain(wrong) == 0,
          "atoms of another lattice are Other");
    Dataset bad = fcc(3);
    ElasticStrainOptions invalid;
    invalid.latticeConstant = 0;
    throwsWith([&] { computeElasticStrain(bad, invalid, nullptr); }, "lattice constant", "elastic strain needs a0");
    std::atomic<bool> cancel{true};
    Dataset big = fcc(4);
    throwsWith([&] { computeElasticStrain(big, {}, &cancel); }, "Cancelled", "elastic strain cancels");
}
// ---- Hide selected ---------------------------------------------------------------
static void testHideSelected() {
    PipelineResult r = resultOf(fcc(2));
    r.selected[0] = r.selected[3] = 1;
    r.data.scalarProperties["Hidden"] = std::vector<double>(r.data.atoms.size(), 0.0);
    r.data.scalarProperties["Hidden"][5] = 2; // Hidden upstream by Compute property.
    hideSelectedParticles(r);
    const auto &hidden = r.data.scalarProperties.at("Hidden");
    check(hidden[0] == 1 && hidden[3] == 1 && hidden[5] == 2 && hidden[1] == 0, "selected particles become hidden, earlier ones stay");
    check(std::count(r.selected.begin(), r.selected.end(), 1) == 0, "hide selected clears the selection");
    near(attribute(r.data, "HideSelected.hidden_count"), 3, 0, "hidden particle count");
    check(r.data.atoms.size() == 32, "hidden particles stay in the dataset");
    // Through the pipeline: Delete selected after hiding deletes nothing.
    Modifier select{Op::ExpressionSelect};
    select.property = "Position.X < 1";
    std::vector<Modifier> stack{select, Modifier{Op::HideSelected}, Modifier{Op::Delete}};
    const auto out = evaluateFrom(resultOf(fcc(2)), stack, 0);
    const auto &h = out.data.scalarProperties.at("Hidden");
    check(out.data.atoms.size() == 32 && std::count(h.begin(), h.end(), 1.0) == 8,
          "hidden particles are kept and a later Delete selected sees no selection");
    // Hidden flows through property-aware nodes like any particle property.
    Modifier again{Op::ExpressionSelect};
    again.property = "Hidden > 0";
    std::vector<Modifier> reselect{select, Modifier{Op::HideSelected}, again};
    const auto picked = evaluateFrom(resultOf(fcc(2)), reselect, 0);
    check(std::count(picked.selected.begin(), picked.selected.end(), 1) == 8, "Hidden can be selected downstream");
    PipelineResult bad = resultOf(fcc(1));
    bad.selected.pop_back();
    throwsWith([&] { hideSelectedParticles(bad); }, "Selection length", "hide selected checks the selection");
}

// ---- Common neighbor analysis modes ----------------------------------------------
static Modifier cnaNode(int mode, float cutoff = 0) {
    Modifier m{Op::CommonNeighborAnalysis};
    m.type = mode;
    m.value = cutoff;
    return m;
}
static double cnaCount(const Dataset &d, const char *name) {
    return attribute(d, std::string("CommonNeighborAnalysis.counts.") + name);
}
static Dataset jittered(Dataset d, double sigma, unsigned seed) {
    std::mt19937 random(seed);
    std::normal_distribution<float> noise(0.f, float(sigma));
    for (auto &atom : d.atoms) { atom.x += noise(random); atom.y += noise(random); atom.z += noise(random); }
    return d;
}

static void testCommonNeighborModes() {
    const auto run = [](const Dataset &d, int mode) { return evaluateFrom(resultOf(d), {cnaNode(mode)}, 0).data; };
    for (int mode : {1, 2}) {
        const std::string label = mode == 1 ? "adaptive CNA " : "interval CNA ";
        const Dataset a = fcc(3), b = hcp(3), c = bcc(3);
        near(cnaCount(run(a, mode), "FCC"), double(a.atoms.size()), 0, label + "finds ideal fcc");
        near(cnaCount(run(b, mode), "HCP"), double(b.atoms.size()), 0, label + "finds ideal hcp");
        near(cnaCount(run(c, mode), "BCC"), double(c.atoms.size()), 0, label + "finds ideal bcc");
        const auto simple = simpleCubic(3);
        near(cnaCount(run(simple, mode), "Other"), double(simple.atoms.size()), 0, label + "leaves simple cubic as Other");
        const auto ico = run(icosahedron(), mode);
        check(ico.scalarProperties.at("Structure Type")[0] == 4, label + "finds the icosahedral center");
        // No cutoff: scaling the crystal changes nothing.
        const Dataset large = fcc(3, 7.2);
        near(cnaCount(run(large, mode), "FCC"), double(large.atoms.size()), 0, label + "does not depend on the length scale");
        // A 4-atom periodic cell sees its own images as neighbors.
        const Dataset tiny = fcc(1);
        near(cnaCount(run(tiny, mode), "FCC"), 4, 0, label + "works in a very small periodic cell");
        Dataset few;
        few.species = {"X"};
        few.atoms = {{0, 0, 0, 0}, {1, 0, 0, 0}, {0, 1, 0, 0}};
        near(cnaCount(run(few, mode), "Other"), 3, 0, label + "marks a small cluster as Other");
        const auto gentle = jittered(fcc(4), .05, 11);
        near(cnaCount(run(gentle, mode), "FCC"), double(gentle.atoms.size()), 0, label + "tolerates small displacements");
    }
    // Stronger noise: testing every threshold cannot recognize fewer atoms than one fixed choice.
    const auto hot = jittered(fcc(5), .17, 5);
    const double adaptiveFcc = cnaCount(run(hot, 1), "FCC"), intervalFcc = cnaCount(run(hot, 2), "FCC");
    check(adaptiveFcc > 0 && intervalFcc > adaptiveFcc,
          "interval CNA recognizes more of a noisy crystal: " + std::to_string(intervalFcc) + " vs " + std::to_string(adaptiveFcc));

    // Fixed mode keeps its cutoff requirement; variable modes ignore the cutoff field.
    const Dataset crystal = fcc(3);
    near(cnaCount(evaluateFrom(resultOf(crystal), {cnaNode(0, 3.0f)}, 0).data, "FCC"), double(crystal.atoms.size()), 0, "fixed CNA is unchanged");
    throwsWith([&] { (void)evaluateFrom(resultOf(crystal), {cnaNode(0, 0.f)}, 0); }, "Cutoff must be", "fixed CNA needs a cutoff");
    throwsWith([&] { (void)evaluateFrom(resultOf(crystal), {cnaNode(9)}, 0); }, "Unknown common neighbor", "an unknown mode is rejected");

    // Bond-based mode ignores distances and needs bonds.
    Modifier bonds{Op::CreateBonds};
    bonds.value = 3.0f;
    auto bonded = evaluateFrom(resultOf(crystal), {bonds, cnaNode(3)}, 0).data;
    near(cnaCount(bonded, "FCC"), double(crystal.atoms.size()), 0, "bond-based CNA finds fcc");
    const auto table = std::find_if(bonded.tables.begin(), bonded.tables.end(), [](const DataTable &t) { return t.name == "CNA bond signatures"; });
    check(table != bonded.tables.end() && table->rows[0][0] == "421" && table->rows[0][1] == std::to_string(6 * crystal.atoms.size()),
          "bond signatures count each bond once");
    throwsWith([&] { (void)evaluateFrom(resultOf(crystal), {cnaNode(3)}, 0); }, "needs bonds", "bond-based CNA reports missing bonds");
    Dataset stretched = bonded;
    for (auto &atom : stretched.atoms) atom.x *= 1.3f;
    near(cnaCount(evaluateFrom(resultOf(stretched), {cnaNode(3)}, 0).data, "FCC"), double(crystal.atoms.size()), 0, "bond-based CNA does not use positions");

    // Unselected particles are left out of the analysis and reported as Other.
    PipelineResult partial = resultOf(fcc(3));
    std::fill(partial.selected.begin(), partial.selected.end(), uint8_t(1));
    partial.selected[0] = 0;
    Modifier onlySelected = cnaNode(1);
    onlySelected.cspOnlySelected = true;
    const auto selectedResult = evaluateFrom(partial, {onlySelected}, 0).data;
    const auto &types = selectedResult.scalarProperties.at("Structure Type");
    check(types[0] == 0, "an unselected particle is Other");
    near(cnaCount(selectedResult, "Other"), 13, 0, "the twelve neighbors of the removed particle lose their fcc shell");

    // Colors are optional and follow the structure type.
    near(double(evaluateFrom(resultOf(crystal), {cnaNode(1)}, 0).data.particleColors.size()), 0, 0, "colors are off unless requested");
    Modifier colored = cnaNode(1);
    colored.overlapUseRadii = true;
    const auto paint = evaluateFrom(partial, {colored}, 0).data;
    check(paint.particleColors.size() == paint.atoms.size() && paint.particleColors[1].x == .4f && paint.particleColors[1].y == 1.f, "fcc particles get the fcc color");

    std::atomic<bool> cancelled{true};
    throwsWith([&] { (void)evaluateFrom(resultOf(crystal), {cnaNode(2)}, 0, &cancelled); }, "Cancelled", "interval CNA can be cancelled");
}

// ---- Load trajectory and Combine datasets ------------------------------------------
static std::map<std::string, std::vector<Dataset>> fakeFiles;
static void installFakeFiles() {
    externalFrameReader() = [](const std::string &path, int frame, std::atomic<bool> *) {
        const auto &frames = fakeFiles.at(path);
        return ExternalFrame{frames[std::min(size_t(std::max(frame, 0)), frames.size() - 1)], int(frames.size())};
    };
}
static Modifier fileNode(Op op, const char *path) {
    Modifier m{op};
    m.property = path;
    return m;
}

static void testLoadTrajectory() {
    installFakeFiles();
    Dataset topology = fcc(1);
    topology.scalarProperties["Particle Identifier"] = {10, 20, 30, 40};
    topology.scalarProperties["Charge"] = {1, 2, 3, 4};
    topology.species = {"A", "B"};
    topology.atoms[1].type = 1;
    topology.bonds.push_back({0, 1, {0, 0, 0}, 1});

    // Frame 1 lists the particles in reverse, moves them, changes the cell and adds a property.
    Dataset first = topology, second = topology;
    second.scalarProperties.erase("Charge");
    second.bonds.clear();
    second.cell = {9, 0, 0, 0, 9, 0, 0, 0, 9};
    std::reverse(second.atoms.begin(), second.atoms.end());
    for (auto &atom : second.atoms) { atom.x += 1; atom.type = 0; }
    second.scalarProperties["Particle Identifier"] = {40, 30, 20, 10};
    second.scalarProperties["Energy"] = {-4, -3, -2, -1};
    second.vectorProperties["Velocity"] = {Vec3{4, 0, 0}, Vec3{3, 0, 0}, Vec3{2, 0, 0}, Vec3{1, 0, 0}};
    second.globalAttributes["Timestep"] = 500;
    fakeFiles["traj"] = {first, second};

    const auto at = [&](int frame) {
        return evaluateFrom(resultOf(topology), {fileNode(Op::LoadTrajectory, "traj")}, 0, nullptr, nullptr, SIZE_MAX, {}, {}, frame).data;
    };
    const auto moved = at(1);
    near(moved.atoms[0].x, topology.atoms[0].x + 1, 1e-6, "positions pair by Particle Identifier");
    near(moved.scalarProperties.at("Energy")[0], -1, 0, "file properties follow the identifier match");
    near(moved.vectorProperties.at("Velocity")[0].x, 1, 0, "vector properties follow the identifier match");
    near(moved.scalarProperties.at("Charge")[3], 4, 0, "topology-only properties stay");
    check(moved.atoms[1].type == 1 && moved.species.size() == 2, "topology particle types stay");
    check(moved.bonds.size() == 1, "topology bonds stay");
    near(moved.cell[0], 9, 0, "the file's simulation cell replaces the old one");
    near(attribute(moved, "Timestep"), 500, 0, "file attributes are published");
    near(attribute(moved, "LoadTrajectory.frame_count"), 2, 0, "the node reports the file's frame count");
    near(at(0).atoms[0].x, topology.atoms[0].x, 1e-6, "frame 0 reads the file's first frame");
    near(at(7).atoms[0].x, topology.atoms[0].x + 1, 1e-6, "frames past the end use the file's last frame");

    // Without identifiers on both sides, storage order decides.
    Dataset plain = topology;
    plain.scalarProperties.erase("Particle Identifier");
    const auto ordered = evaluateFrom(resultOf(plain), {fileNode(Op::LoadTrajectory, "traj")}, 0, nullptr, nullptr, SIZE_MAX, {}, {}, 1).data;
    near(ordered.atoms[0].x, second.atoms[0].x, 1e-6, "storage order pairs particles without identifiers");

    Dataset wrong = fcc(2);
    throwsWith([&] { (void)evaluateFrom(resultOf(wrong), {fileNode(Op::LoadTrajectory, "traj")}, 0); }, "32", "a particle-count mismatch names both counts");
    Dataset unknown = second;
    unknown.scalarProperties["Particle Identifier"][0] = 99;
    fakeFiles["unknown"] = {unknown};
    throwsWith([&] { (void)evaluateFrom(resultOf(topology), {fileNode(Op::LoadTrajectory, "unknown")}, 0); }, "missing from the file", "an unmatched identifier is reported");
    Dataset repeated = second;
    repeated.scalarProperties["Particle Identifier"][1] = 40;
    fakeFiles["repeated"] = {repeated};
    throwsWith([&] { (void)evaluateFrom(resultOf(topology), {fileNode(Op::LoadTrajectory, "repeated")}, 0); }, "repeats", "repeated identifiers are reported");
    Dataset sampled = second;
    sampled.stride = 2;
    fakeFiles["sampled"] = {sampled};
    throwsWith([&] { (void)evaluateFrom(resultOf(topology), {fileNode(Op::LoadTrajectory, "sampled")}, 0); }, "particle limit", "a sampled file is refused");

    // No file chosen: the node does nothing.
    const auto idle = evaluateFrom(resultOf(topology), {fileNode(Op::LoadTrajectory, "")}, 0).data;
    check(idle.atoms[0].x == topology.atoms[0].x && !idle.globalAttributes.contains("LoadTrajectory.frame_count"), "an empty path passes data through");

    // A bond crossing the boundary after the move takes the nearest image.
    Dataset pair;
    pair.species = {"X"};
    pair.cell = {10, 0, 0, 0, 10, 0, 0, 0, 10};
    pair.pbc = {true, true, true};
    pair.atoms = {{1, 5, 5, 0}, {2, 5, 5, 0}};
    pair.bonds.push_back({0, 1, {0, 0, 0}, 1});
    Dataset across = pair;
    across.bonds.clear();
    across.atoms[1].x = 9;
    fakeFiles["across"] = {across};
    const auto wrapped = evaluateFrom(resultOf(pair), {fileNode(Op::LoadTrajectory, "across")}, 0).data;
    check(wrapped.bonds[0].image[0] == -1, "bond images are refreshed for the new positions");
    near(bondVector(wrapped, wrapped.bonds[0])[0], -2, 1e-6, "the refreshed bond has the minimum-image length");
}

static void testCombineDatasets() {
    installFakeFiles();
    Dataset primary = fcc(1);
    primary.species = {"Cu"};
    for (auto &atom : primary.atoms) atom.type = 0;
    primary.scalarProperties["Particle Identifier"] = {5, 6, 7, 8};
    primary.scalarProperties["Molecule Identifier"] = {1, 1, 2, 2};
    primary.scalarProperties["Charge"] = {1, 1, 1, 1};
    primary.bonds.push_back({0, 1, {0, 0, 0}, 1});

    Dataset other;
    other.species = {"O", "Cu"};
    other.cell = {50, 0, 0, 0, 50, 0, 0, 0, 50};
    other.atoms = {{20, 0, 0, 0}, {21, 0, 0, 1}};
    other.scalarProperties["Particle Identifier"] = {5, 6};
    other.scalarProperties["Molecule Identifier"] = {1, 2};
    other.scalarProperties["Mass"] = {16, 63};
    other.vectorProperties["Velocity"] = {Vec3{1, 2, 3}, Vec3{4, 5, 6}};
    other.bonds.push_back({0, 1, {0, 0, 1}, 2});
    fakeFiles["other"] = {other};

    PipelineResult input = resultOf(primary);
    input.selected[2] = 1;
    const auto merged = evaluateFrom(input, {fileNode(Op::CombineDatasets, "other")}, 0);
    const Dataset &d = merged.data;
    check(d.atoms.size() == 6 && merged.selected.size() == 6 && merged.colorSelected.size() == 6, "particles and selection are extended");
    check(merged.selected[2] == 1 && merged.selected[4] == 0, "the selection is kept and added particles are unselected");
    check(d.species.size() == 2 && d.species[1] == "O", "new types are appended and shared names merge");
    check(d.atoms[4].type == 1 && d.atoms[5].type == 0, "added particle types are remapped by name");
    const auto &ids = d.scalarProperties.at("Particle Identifier");
    check(ids[0] == 5 && ids[4] == 9 && ids[5] == 10, "added particles get new unique identifiers");
    const auto &molecules = d.scalarProperties.at("Molecule Identifier");
    check(molecules[3] == 2 && molecules[4] == 3 && molecules[5] == 4, "added molecules get new identifiers");
    check(d.scalarProperties.at("Charge")[4] == 0 && d.scalarProperties.at("Charge")[0] == 1, "properties absent from the file are zero there");
    check(d.scalarProperties.at("Mass")[0] == 0 && d.scalarProperties.at("Mass")[5] == 63, "properties new in the file are zero on the primary");
    check(d.vectorProperties.at("Velocity")[0].x == 0 && d.vectorProperties.at("Velocity")[5].z == 6, "vector properties merge the same way");
    check(d.bonds.size() == 2 && d.bonds[1].a == 4 && d.bonds[1].b == 5 && d.bonds[1].order == 2 && d.bonds[1].image[2] == 1, "added bonds are renumbered and keep order and image");
    check(d.cell == primary.cell, "the primary simulation cell is kept");
    near(attribute(d, "CombineDatasets.added_count"), 2, 0, "the added count is published");

    // Identifiers on one side only still end up unique.
    Dataset noIds = primary;
    noIds.scalarProperties.erase("Particle Identifier");
    const auto assigned = evaluateFrom(resultOf(noIds), {fileNode(Op::CombineDatasets, "other")}, 0).data.scalarProperties.at("Particle Identifier");
    check(assigned[0] == 1 && assigned[3] == 4 && assigned[4] == 5 && assigned[5] == 6, "identifiers are made unique when only the file has them");

    // Colors: missing ones keep the default marker.
    Dataset painted = primary;
    painted.particleColors.assign(4, Vec3{1, 0, 0});
    const auto colored = evaluateFrom(resultOf(painted), {fileNode(Op::CombineDatasets, "other")}, 0).data;
    check(colored.particleColors.size() == 6 && colored.particleColors[0].x == 1 && colored.particleColors[5].x == -1, "added particles use default colors");

    // An empty dataset takes the file's particles and keeps its own cell.
    Dataset empty;
    empty.cell = primary.cell;
    const auto filled = evaluateFrom(resultOf(empty), {fileNode(Op::CombineDatasets, "other")}, 0).data;
    check(filled.atoms.size() == 2 && filled.species.size() == 2 && filled.cell == primary.cell, "an empty dataset takes the file's particles");

    // Corresponding frames; a shorter file repeats its last one.
    Dataset later = other;
    later.atoms.push_back({22, 0, 0, 0});
    later.scalarProperties.clear();
    later.vectorProperties.clear();
    later.bonds.clear();
    fakeFiles["frames"] = {other, later};
    const auto atFrame = [&](int frame) {
        return evaluateFrom(resultOf(primary), {fileNode(Op::CombineDatasets, "frames")}, 0, nullptr, nullptr, SIZE_MAX, {}, {}, frame).data.atoms.size();
    };
    check(atFrame(0) == 6 && atFrame(1) == 7 && atFrame(5) == 7, "matching frames are combined and the last one repeats");

    Dataset badType = other;
    badType.atoms[0].type = 9;
    fakeFiles["badType"] = {badType};
    throwsWith([&] { (void)evaluateFrom(resultOf(primary), {fileNode(Op::CombineDatasets, "badType")}, 0); }, "out of range", "an invalid particle type is reported");
    Dataset badBond = other;
    badBond.bonds[0].b = 7;
    fakeFiles["badBond"] = {badBond};
    throwsWith([&] { (void)evaluateFrom(resultOf(primary), {fileNode(Op::CombineDatasets, "badBond")}, 0); }, "missing particle", "an invalid bond is reported");
    check(evaluateFrom(resultOf(primary), {fileNode(Op::CombineDatasets, "")}, 0).data.atoms.size() == 4, "an empty path passes data through");
}

// ---- Measure and partial RDF -------------------------------------------------------
static Modifier measureNode(const char *path, bool periodic = true) {
    Modifier m{Op::Measure};
    m.property = path;
    m.adaptive = periodic;
    return m;
}

static void testMeasure() {
    Dataset d;
    d.species = {"X"};
    d.cell = {10, 0, 0, 0, 10, 0, 0, 0, 10};
    d.pbc = {true, true, true};
    d.atoms = {{1, 1, 1, 0}, {4, 1, 1, 0}, {4, 5, 1, 0}, {9, 1, 1, 0}};

    // A 3-4-5 right triangle, closed.
    auto closed = evaluateFrom(resultOf(d), {measureNode("1 2 3 1", false)}, 0).data;
    near(attribute(closed, "Measurements.TotalDistance"), 12, 1e-5, "closed path length");
    near(attribute(closed, "Measurements.Area"), 6, 1e-5, "closed path area");
    const auto &rows = tableNamed(closed, "Measurements").rows;
    check(rows.size() == 4 && rows[0][7].empty() && rows[0][8].empty(), "the first row has no segment or angle");
    near(std::stod(rows[1][7]), 3, 1e-5, "first segment length");
    near(std::stod(rows[2][7]), 4, 1e-5, "second segment length");
    near(std::stod(rows[1][8]), 90, 1e-4, "interior angle at the right-angle vertex");
    near(std::stod(rows[3][7]), 5, 1e-5, "closing segment length");
    near(std::stod(rows[3][8]), std::atan2(4.0, 3.0) * 180 / 3.14159265358979323846, 1e-4, "the last row holds the start-point angle");

    // An open path has no area and no end angles.
    auto open = evaluateFrom(resultOf(d), {measureNode("1, 2 3", false)}, 0).data;
    check(!open.globalAttributes.contains("Measurements.Area"), "an open path has no area");
    const auto &openRows = tableNamed(open, "Measurements").rows;
    check(openRows.size() == 3 && openRows[2][8].empty() && !openRows[2][7].empty(), "the last open row has a segment and no angle");

    // Across the periodic boundary.
    near(attribute(evaluateFrom(resultOf(d), {measureNode("1 4", true)}, 0).data, "Measurements.TotalDistance"), 2, 1e-5, "minimum image connects across the boundary");
    near(attribute(evaluateFrom(resultOf(d), {measureNode("1 4", false)}, 0).data, "Measurements.TotalDistance"), 8, 1e-5, "the direct distance without periodic boundaries");

    // Identifiers take priority over list positions.
    Dataset ids = d;
    ids.scalarProperties["Particle Identifier"] = {40, 30, 20, 10};
    near(attribute(evaluateFrom(resultOf(ids), {measureNode("40 30", false)}, 0).data, "Measurements.TotalDistance"), 3, 1e-5, "particles are found by identifier");
    throwsWith([&] { (void)evaluateFrom(resultOf(ids), {measureNode("1 30", false)}, 0); }, "identifier 1", "an unknown identifier is reported");

    throwsWith([&] { (void)evaluateFrom(resultOf(d), {measureNode("1", false)}, 0); }, "at least two", "one point is rejected");
    throwsWith([&] { (void)evaluateFrom(resultOf(d), {measureNode("1 9", false)}, 0); }, "number 9", "a particle number out of range is reported");
    throwsWith([&] { (void)evaluateFrom(resultOf(d), {measureNode("1 x", false)}, 0); }, "not a particle", "text that is not a number is reported");
    const auto repeated = evaluateFrom(resultOf(d), {measureNode("1 1 2", false)}, 0).data;
    check(tableNamed(repeated, "Measurements").rows[1][8].empty(), "a zero-length segment has no angle");
    check(!evaluateFrom(resultOf(d), {measureNode("", false)}, 0).data.globalAttributes.contains("Measurements.TotalDistance"), "an empty path passes data through");

    // A loop that goes around the periodic cell: area uses connected corners only.
    Dataset square = d;
    square.atoms = {{9, 9, 1, 0}, {1, 9, 1, 0}, {1, 1, 1, 0}, {9, 1, 1, 0}};
    near(attribute(evaluateFrom(resultOf(square), {measureNode("1 2 3 4 1", true)}, 0).data, "Measurements.Area"), 4, 1e-5, "a closed path across the boundary keeps its true area");
}

// ---- Per-bond properties -----------------------------------------------------------
static size_t bondRowsWith(const Dataset &d, int common, int bonds, int chain) {
    size_t found = 0;
    const auto &a = d.bondProperties.at("CNA indices.1"), &b = d.bondProperties.at("CNA indices.2"), &c = d.bondProperties.at("CNA indices.3");
    for (size_t i = 0; i < d.bonds.size(); ++i) found += a[i] == common && b[i] == bonds && c[i] == chain;
    return found;
}
static Dataset taggedChain() {
    Dataset d;
    d.species = {"X"};
    d.cell = {100, 0, 0, 0, 100, 0, 0, 0, 100};
    d.atoms = {{0, 0, 0, 0}, {2, 0, 0, 0}, {4, 0, 0, 0}, {6, 0, 0, 0}};
    d.bonds = {{0, 1, {}, 1}, {1, 2, {}, 1}, {2, 3, {}, 1}};
    d.bondProperties["Tag"] = {10, 20, 30};
    d.bounds();
    return d;
}
static void testBondProperties() {
    Modifier createFcc{Op::CreateBonds};
    createFcc.value = 3.0f;
    const auto fccBonds = evaluateFrom(resultOf(fcc(3)), {createFcc, cnaNode(3)}, 0).data;
    check(fccBonds.bondProperties.size() == 3 && fccBonds.bonds.size() == 6 * fccBonds.atoms.size(), "bond-based CNA publishes three columns");
    near(double(bondRowsWith(fccBonds, 4, 2, 1)), double(fccBonds.bonds.size()), 0, "every fcc bond is 421");

    Modifier createWide{Op::CreateBonds};
    createWide.value = 3.5f;
    // Four repeats: bond-based matching is by particle, so a cell only three bonds long
    // would turn a particle and its images along that axis into a false common neighbor.
    const auto hcpBonds = evaluateFrom(resultOf(hcp(4)), {createWide, cnaNode(3)}, 0).data;
    near(double(bondRowsWith(hcpBonds, 4, 2, 1)), double(3 * hcpBonds.atoms.size()), 0, "half of the hcp bonds are 421");
    near(double(bondRowsWith(hcpBonds, 4, 2, 2)), double(3 * hcpBonds.atoms.size()), 0, "half of the hcp bonds are 422");
    const auto hcpTable = std::find_if(hcpBonds.tables.begin(), hcpBonds.tables.end(), [](const DataTable &t) { return t.name == "CNA bond signatures"; });
    check(hcpTable != hcpBonds.tables.end() && hcpTable->rows[1][1] == std::to_string(bondRowsWith(hcpBonds, 4, 2, 2)), "the per-bond values agree with the signature table");

    const auto bccBonds = evaluateFrom(resultOf(bcc(4)), {createWide, cnaNode(3)}, 0).data;
    near(double(bondRowsWith(bccBonds, 4, 4, 4)), double(3 * bccBonds.atoms.size()), 0, "second-shell bcc bonds are 444");
    near(double(bondRowsWith(bccBonds, 6, 6, 6)), double(4 * bccBonds.atoms.size()), 0, "first-shell bcc bonds are 666");

    // Seven particles, all bonded: every pair has 5 common neighbors with 10 bonds among them.
    Dataset clique;
    clique.species = {"X"};
    clique.cell = {100, 0, 0, 0, 100, 0, 0, 0, 100};
    for (int i = 0; i < 7; ++i) clique.atoms.push_back({float(i), 0, 0, 0});
    for (uint32_t i = 0; i < 7; ++i)
        for (uint32_t j = i + 1; j < 7; ++j) clique.bonds.push_back({i, j, {}, 1});
    near(double(bondRowsWith(evaluateFrom(resultOf(clique), {cnaNode(3)}, 0).data, 5, 10, -1)), 21, 0, "the chain index is -1 above six common bonds");

    PipelineResult partial = resultOf(fcc(3));
    std::fill(partial.selected.begin(), partial.selected.end(), uint8_t(1));
    partial.selected[0] = 0;
    Modifier onlySelected = cnaNode(3);
    onlySelected.cspOnlySelected = true;
    const auto partialBonds = evaluateFrom(partial, {createFcc, onlySelected}, 0).data;
    near(double(bondRowsWith(partialBonds, 0, 0, 0)), 12, 0, "bonds of an excluded particle get 0 0 0");

    // Columns follow their bonds through every node that rewrites the bond list.
    PipelineResult withoutSecond = resultOf(taggedChain());
    std::fill(withoutSecond.selected.begin(), withoutSecond.selected.end(), uint8_t(0));
    withoutSecond.selected[1] = 1;
    const auto deleted = evaluateFrom(withoutSecond, {Modifier{Op::Delete}}, 0).data;
    check(deleted.bonds.size() == 1 && deleted.bondProperties.at("Tag") == std::vector<double>{30}, "Delete keeps the values of surviving bonds");
    Modifier replicate{Op::Replicate};
    replicate.replicateN[0] = 2;
    check(evaluateFrom(resultOf(taggedChain()), {replicate}, 0).data.bondProperties.at("Tag") == std::vector<double>{10, 20, 30, 10, 20, 30}, "Replicate repeats the values");
    Modifier sameBonds{Op::CreateBonds};
    sameBonds.value = 2.5f;
    sameBonds.discardExistingBonds = false;
    check(evaluateFrom(resultOf(taggedChain()), {sameBonds}, 0).data.bondProperties.at("Tag") == std::vector<double>{10, 20, 30}, "duplicates found by Create bonds do not disturb the values");
    Modifier moreBonds = sameBonds;
    moreBonds.value = 4.5f;
    check(evaluateFrom(resultOf(taggedChain()), {moreBonds}, 0).data.bondProperties.at("Tag") == std::vector<double>{10, 20, 30, 0, 0}, "new bonds get 0");
    Modifier replaceBonds = moreBonds;
    replaceBonds.discardExistingBonds = true;
    check(evaluateFrom(resultOf(taggedChain()), {replaceBonds}, 0).data.bondProperties.empty(), "discarding the bonds removes their properties");
    installFakeFiles();
    Dataset extra;
    extra.species = {"X"};
    extra.cell = {50, 0, 0, 0, 50, 0, 0, 0, 50};
    extra.atoms = {{20, 0, 0, 0}, {22, 0, 0, 0}};
    extra.bonds = {{0, 1, {}, 1}};
    fakeFiles["bonded"] = {extra};
    Modifier combine{Op::CombineDatasets};
    combine.property = "bonded";
    check(evaluateFrom(resultOf(taggedChain()), {combine}, 0).data.bondProperties.at("Tag") == std::vector<double>{10, 20, 30, 0}, "bonds from a combined file get 0");
    PipelineResult stale = resultOf(taggedChain());
    stale.data.bonds.pop_back();
    check(evaluateFrom(stale, {}, 0).data.bondProperties.empty(), "a column that no longer matches the bonds is dropped");

    // Histogram and Reduce property.
    Modifier lengths{Op::Histogram};
    lengths.type = 4;
    lengths.clusterByBonds = true;
    lengths.property = "Length";
    const auto lengthResult = evaluateFrom(resultOf(fcc(3)), {createFcc, lengths}, 0).data;
    near(lengthResult.globalAttributes.at("Histogram.samples"), double(6 * lengthResult.atoms.size()), 0, "Histogram counts every bond");
    check(tableNamed(lengthResult, "Histogram: Bond.Length").rows.size() == 4, "the table names the bond property");
    Modifier chains = lengths;
    chains.type = 2;
    chains.property = "CNA indices.3";
    const auto chainResult = evaluateFrom(resultOf(bcc(4)), {createWide, cnaNode(3), chains}, 0).data;
    const DataTable &chainTable = tableNamed(chainResult, "Histogram: Bond.CNA indices.3");
    check(chainTable.rows[0][1] == std::to_string(3 * chainResult.atoms.size()) && chainTable.rows[1][1] == std::to_string(4 * chainResult.atoms.size()),
          "Histogram reads a stored bond property");
    PipelineResult threeSelected = resultOf(taggedChain());
    std::fill(threeSelected.selected.begin(), threeSelected.selected.end(), uint8_t(1));
    threeSelected.selected[3] = 0;
    Modifier selectedOnly = lengths;
    selectedOnly.histogramSelectedOnly = true;
    near(evaluateFrom(threeSelected, {selectedOnly}, 0).data.globalAttributes.at("Histogram.samples"), 2, 0, "a selected bond needs both particles selected");
    Modifier rangeOnBonds = lengths;
    rangeOnBonds.histogramSelectRange = true;
    rangeOnBonds.histogramRangeStart = 0;
    rangeOnBonds.histogramRangeEnd = 1;
    throwsWith([&] { (void)evaluateFrom(resultOf(taggedChain()), {rangeOnBonds}, 0); }, "particles only", "value-range selection is refused on bonds");
    Modifier unknown = lengths;
    unknown.property = "Nope";
    throwsWith([&] { (void)evaluateFrom(resultOf(taggedChain()), {unknown}, 0); }, "Unknown bond property", "an unknown bond property is reported");
    throwsWith([&] { (void)evaluateFrom(resultOf(fcc(1)), {lengths}, 0); }, "needs bonds", "a bond histogram needs bonds");
    Modifier longest{Op::ReduceProperty};
    longest.clusterByBonds = true;
    longest.property = "Length";
    longest.reduceOperation = 1;
    near(evaluateFrom(resultOf(taggedChain()), {longest}, 0).data.globalAttributes.at("ReduceProperty.Bond.Length.max"), 2, 1e-9, "Reduce property reads bond lengths");

    const auto outputs = modifierOutputs(cnaNode(3), 0);
    check(std::any_of(outputs.begin(), outputs.end(), [](const DataObject &o) { return o.kind == DataObject::Kind::Bonds && o.name == "CNA indices"; }), "the node lists its bond output");
    const std::vector<Modifier> stack{createFcc, cnaNode(3), lengths};
    check(pipelineInputBondPropertyChoices(fcc(1), stack, 1) == std::vector<std::string>{"Length"}, "upstream of CNA only Length is offered");
    check(pipelineInputBondPropertyChoices(fcc(1), stack, 2).size() == 4, "downstream of bond-based CNA its columns are offered");
}

// ---- Trajectory lines and coordination polyhedra ------------------------------------
static PipelineResult runOverFrames(const std::vector<Dataset> &frames, const Modifier &node, int current,
                                    const std::vector<uint8_t> *firstSelection = nullptr) {
    ModifierFrameState state;
    state.frameCount = int(frames.size());
    state.frameBefore = [&](size_t, int frame) { return frames.at(size_t(frame)); };
    if (firstSelection) state.selectionBefore = [&](size_t, int) { return *firstSelection; };
    return evaluateFrom(resultOf(frames.at(size_t(current))), {node}, 0, nullptr, nullptr, SIZE_MAX, {}, {}, current, state);
}
static std::vector<Dataset> movingPair() {
    // Particle 0 moves +0.5 per frame across the x boundary of a 10-unit periodic cell.
    std::vector<Dataset> frames;
    for (int f = 0; f < 4; ++f) {
        Dataset d;
        d.species = {"X"};
        d.pbc = {true, true, true};
        d.cell = {10, 0, 0, 0, 10, 0, 0, 0, 10};
        float x = 9.0f + .5f * float(f);
        if (x >= 10) x -= 10;
        d.atoms = {{x, 1, 1, 0}, {3, float(f), 5, 0}};
        d.scalarProperties["Particle Identifier"] = {1, 2};
        d.scalarProperties["Energy"] = {double(f), 10.0 * f};
        d.bounds();
        frames.push_back(std::move(d));
    }
    return frames;
}
static size_t countSegments(const Dataset &d) {
    size_t n = 0;
    forEachTrajectorySegment(d, 1000, [&](const auto &, const auto &, float) { ++n; });
    return n;
}
static void testTrajectoryLines() {
    const auto frames = movingPair();
    Modifier node{Op::GenerateTrajectoryLines};
    node.property.clear();
    node.adaptive = true;
    node.cspNeighbors = 1;
    auto lines = runOverFrames(frames, node, 2).data;
    check(lines.trajectoryLines.set && lines.trajectoryLines.set->lineCount == 2 && lines.trajectoryLines.set->points.size() == 8, "one line per particle and one point per frame");
    near(attribute(lines, "TrajectoryLines.count"), 2, 0, "line count attribute");
    near(lines.trajectoryLines.set->points[3].x, 10.5, 1e-5, "unwrapping continues across the boundary");
    near(double(countSegments(lines)), 6, 0, "three segments per line");
    near(double(lines.atoms.size()), 2, 0, "particles are untouched");

    Modifier raw = node;
    raw.adaptive = false;
    near(runOverFrames(frames, raw, 0).data.trajectoryLines.set->points[3].x, .5, 1e-5, "wrapped input stays wrapped when unwrapping is off");

    // Particle order changes between frames: lines follow the identifiers.
    auto shuffled = frames;
    std::swap(shuffled[1].atoms[0], shuffled[1].atoms[1]);
    shuffled[1].scalarProperties["Particle Identifier"] = {2, 1};
    near(runOverFrames(shuffled, node, 0).data.trajectoryLines.set->points[1].x, 9.5, 1e-5, "lines follow particle identifiers");

    Modifier chosen = node;
    chosen.cspOnlySelected = true;
    const std::vector<uint8_t> second{0, 1}, none{0, 0};
    const auto picked = runOverFrames(frames, chosen, 3, &second).data;
    check(picked.trajectoryLines.set->lineCount == 1 && picked.trajectoryLines.set->points[0].x == 3.f, "the selection of the first frame picks the particles");
    throwsWith([&] { (void)runOverFrames(frames, chosen, 0, &none); }, "No particles are selected", "an empty selection is reported");

    Modifier range = node;
    range.clusterByBonds = true;
    range.replicateN[0] = 1;
    range.replicateN[1] = 3;
    range.cspNeighbors = 2;
    check(runOverFrames(frames, range, 0).data.trajectoryLines.set->frames == std::vector<int>{1, 3}, "frame range and sampling choose the frames");
    range.replicateN[1] = 9;
    throwsWith([&] { (void)runOverFrames(frames, range, 0); }, "frame range", "a range past the end is reported");
    Modifier sparse = node;
    sparse.cspNeighbors = 10;
    throwsWith([&] { (void)runOverFrames(frames, sparse, 0); }, "at least two", "one sampled frame is not a line");

    Modifier sampled = node;
    sampled.property = "Energy";
    const auto colored = runOverFrames(frames, sampled, 0).data;
    near(colored.trajectoryLines.set->valueMax, 30, 1e-5, "sampled values span all frames");
    near(colored.trajectoryLines.set->values[7], 30, 1e-5, "values are stored per point");
    std::vector<float> shades;
    forEachTrajectorySegment(colored, 1000, [&](const auto &, const auto &, float u) { shades.push_back(u); });
    near(shades.back(), 25.0 / 30.0, 1e-5, "a segment takes the mean of its end values");
    Modifier missing = node;
    missing.property = "Nope";
    throwsWith([&] { (void)runOverFrames(frames, missing, 0); }, "Nope", "an unknown sampled property is reported");

    Modifier partial = node;
    partial.overlapUseRadii = true;
    near(double(countSegments(runOverFrames(frames, partial, 0).data)), 0, 0, "nothing is drawn at the first frame");
    near(double(countSegments(runOverFrames(frames, partial, 2).data)), 4, 0, "segments grow with the current frame");

    Modifier wrapped = node;
    wrapped.histogramSelectRange = true;
    // Offset so that one segment crosses x = 10 instead of ending exactly on it.
    auto crossing = frames;
    for (auto &f : crossing) { f.atoms[0].x += .25f; if (f.atoms[0].x >= 10) f.atoms[0].x -= 10; }
    const auto folded = runOverFrames(crossing, wrapped, 0).data;
    std::vector<std::array<double, 2>> firstLine;
    forEachTrajectorySegment(folded, 1000, [&](const auto &a, const auto &b, float) { if (a[1] < 1.5 && b[1] < 1.5 && a[2] < 2) firstLine.push_back({a[0], b[0]}); });
    check(firstLine.size() == 4, "a segment is cut at the periodic boundary");
    bool inside = true;
    for (const auto &piece : firstLine) inside = inside && piece[0] >= -1e-6 && piece[0] <= 10 + 1e-6 && piece[1] >= -1e-6 && piece[1] <= 10 + 1e-6;
    check(inside, "wrapped pieces stay in the cell");

    throwsWith([&] { (void)runOverFrames({frames[0]}, node, 0); }, "at least two", "a single frame cannot make lines");
    Dataset huge;
    huge.species = {"X"};
    huge.atoms.resize(trajectorySegmentLimit + 1);
    throwsWith([&] { (void)runOverFrames({huge, huge}, node, 0); }, "2,000,000", "the segment budget is enforced before reading frames");
    std::atomic<bool> stop{true};
    throwsWith([&] { (void)sweepTrajectoryLines({}, 4, [&](int f) { return frames[size_t(f)]; }, {}, &stop); }, "Cancelled", "the sweep can be cancelled");
}

// A central particle (index 0) plus neighbors at the given offsets, bonded to the center.
static PipelineResult shellAround(const std::vector<std::array<float, 3>> &offsets, std::array<float, 3> center = {10, 10, 10}) {
    Dataset d;
    d.species = {"X"};
    d.cell = {40, 0, 0, 0, 40, 0, 0, 0, 40};
    d.atoms.push_back({center[0], center[1], center[2], 0});
    for (uint32_t i = 0; i < offsets.size(); ++i) {
        d.atoms.push_back({center[0] + offsets[i][0], center[1] + offsets[i][1], center[2] + offsets[i][2], 0});
        d.bonds.push_back({0, i + 1, {}, 1});
    }
    d.bounds();
    PipelineResult r = resultOf(d);
    std::fill(r.selected.begin(), r.selected.end(), uint8_t(0));
    r.selected[0] = 1;
    return r;
}
static const std::vector<std::array<float, 3>> octahedron{{2, 0, 0}, {-2, 0, 0}, {0, 2, 0}, {0, -2, 0}, {0, 0, 2}, {0, 0, -2}};
static bool facesPointOutward(const Dataset &d) {
    const auto &c = d.polyhedra.corners;
    for (size_t k = 0; k + 2 < c.size(); k += 3) {
        std::array<double, 3> p[3];
        if (!polyhedronCorner(d, c[k], p[0]) || !polyhedronCorner(d, c[k + 1], p[1]) || !polyhedronCorner(d, c[k + 2], p[2])) return false;
        const std::array<double, 3> u{p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]}, v{p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
        const std::array<double, 3> n{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        const Atom &a = d.atoms[c[k].a];
        if (n[0] * (p[0][0] - a.x) + n[1] * (p[0][1] - a.y) + n[2] * (p[0][2] - a.z) <= 0) return false;
    }
    return true;
}
static void testCoordinationPolyhedra() {
    Modifier node{Op::CoordinationPolyhedra};
    node.value = .3f;
    node.adaptive = true;
    const auto run = [&](const PipelineResult &r) { return evaluateFrom(r, {node}, 0).data; };
    const auto octa = run(shellAround(octahedron));
    near(attribute(octa, "CoordinationPolyhedra.count"), 1, 0, "one polyhedron");
    near(double(octa.polyhedra.corners.size() / 3), 8, 0, "an octahedron has 8 faces");
    near(double(octa.polyhedra.edges.size() / 2), 12, 0, "an octahedron has 12 edges");
    near(octa.polyhedra.alpha, .7, 1e-6, "transparency sets the opacity");
    check(facesPointOutward(octa), "triangles wind outward");

    std::vector<std::array<float, 3>> cube;
    for (float x : {-1.f, 1.f}) for (float y : {-1.f, 1.f}) for (float z : {-1.f, 1.f}) cube.push_back({x, y, z});
    const auto box = run(shellAround(cube));
    near(double(box.polyhedra.corners.size() / 3), 12, 0, "a cube is 12 triangles");
    near(double(box.polyhedra.edges.size() / 2), 12, 0, "face diagonals are not edges");
    check(facesPointOutward(box), "cube triangles wind outward");

    // A neighbor strictly inside the hull adds nothing.
    auto withInner = octahedron;
    withInner.push_back({.2f, .1f, .1f});
    near(double(run(shellAround(withInner)).polyhedra.corners.size() / 3), 8, 0, "interior neighbors are not corners");

    const auto square = run(shellAround({{2, 0, 0}, {-2, 0, 0}, {0, 2, 0}, {0, -2, 0}}));
    near(attribute(square, "CoordinationPolyhedra.skipped"), 1, 0, "coplanar neighbors are skipped");
    check(square.polyhedra.corners.empty(), "no faces for a flat shell");
    near(attribute(run(shellAround({{2, 0, 0}, {0, 2, 0}, {0, 0, 2}})), "CoordinationPolyhedra.skipped"), 1, 0, "three neighbors are skipped");

    PipelineResult noneSelected = shellAround(octahedron);
    std::fill(noneSelected.selected.begin(), noneSelected.selected.end(), uint8_t(0));
    near(attribute(run(noneSelected), "CoordinationPolyhedra.count"), 0, 0, "without a selection there are no polyhedra");
    PipelineResult unbonded = shellAround(octahedron);
    unbonded.data.bonds.clear();
    throwsWith([&] { (void)run(unbonded); }, "needs bonds", "missing bonds are reported");
    Modifier bad = node;
    bad.value = 2;
    throwsWith([&] { (void)evaluateFrom(shellAround(octahedron), {bad}, 0); }, "Transparency", "an out-of-range transparency is rejected");

    // Across a periodic boundary the shell stays attached to its center.
    PipelineResult wrapped = shellAround(octahedron, {1, 10, 10});
    wrapped.data.pbc = {true, true, true};
    wrapped.data.atoms[2].x = 39;
    wrapped.data.bonds[1].image = {-1, 0, 0};
    const auto across = run(wrapped);
    near(double(across.polyhedra.corners.size() / 3), 8, 0, "a shell across the boundary is complete");
    double lowest = 1e9;
    for (const Bond &corner : across.polyhedra.corners) { std::array<double, 3> p; check(polyhedronCorner(across, corner, p), "corner resolves"); lowest = std::min(lowest, p[0]); }
    near(lowest, -1, 1e-5, "the corner sits at the image next to the center");

    // A bond listed from the neighbor's side counts in the opposite image direction.
    PipelineResult reversed = wrapped;
    Bond &flip = reversed.data.bonds[1];
    std::swap(flip.a, flip.b);
    flip.image = {1, 0, 0};
    lowest = 1e9;
    const auto flipped = run(reversed);
    for (const Bond &corner : flipped.polyhedra.corners) { std::array<double, 3> p; check(polyhedronCorner(flipped, corner, p), "reversed corner resolves"); lowest = std::min(lowest, p[0]); }
    near(lowest, -1, 1e-5, "a reversed bond reaches the same image");

    // Later nodes keep the polyhedra consistent.
    Modifier move{Op::Rotate};
    move.axis = 2;
    move.value = 90;
    const auto turned = evaluateFrom(shellAround(octahedron), {node, move}, 0).data;
    std::array<double, 3> corner;
    check(polyhedronCorner(turned, turned.polyhedra.corners[0], corner), "a rotated corner resolves");
    const Atom &middle = turned.atoms[0];
    near(std::hypot(corner[0] - middle.x, corner[1] - middle.y, corner[2] - middle.z), 2, 1e-4, "polyhedra follow a rotation");

    Modifier replicate{Op::Replicate};
    replicate.replicateN[0] = 2;
    const auto doubled = evaluateFrom(shellAround(octahedron), {node, replicate}, 0).data;
    near(double(doubled.polyhedra.corners.size() / 3), 16, 0, "Replicate copies polyhedra");
    near(double(doubled.polyhedra.corners.back().a), 7, 0, "copies belong to the copied center");
    check(facesPointOutward(doubled), "replicated polyhedra keep their winding");

    // Two shells: deleting one corner removes only the polyhedron that used it.
    PipelineResult pair = shellAround(octahedron);
    Dataset &two = pair.data;
    const uint32_t base = uint32_t(two.atoms.size());
    two.atoms.push_back({30, 30, 30, 0});
    for (uint32_t i = 0; i < octahedron.size(); ++i) {
        two.atoms.push_back({30 + octahedron[i][0], 30 + octahedron[i][1], 30 + octahedron[i][2], 0});
        two.bonds.push_back({base, base + i + 1, {}, 1});
    }
    pair.selected.assign(two.atoms.size(), 0);
    pair.colorSelected.assign(two.atoms.size(), 1);
    pair.selected[0] = pair.selected[base] = 1;
    two.bounds();
    Modifier clearAndPick{Op::ExpressionSelect};
    clearAndPick.property = "Position.X > 11.5 && Position.X < 12.5";
    const auto remaining = evaluateFrom(pair, {node, clearAndPick, Modifier{Op::Delete}}, 0).data;
    near(double(remaining.polyhedra.corners.size() / 3), 8, 0, "a polyhedron that lost a corner is removed");
    near(double(remaining.polyhedra.corners[0].a), double(base - 1), 0, "the surviving polyhedron is reindexed");
    check(facesPointOutward(remaining), "the surviving polyhedron is intact");

    const auto outputs = modifierOutputs(node, 0);
    check(outputs[0].kind == DataObject::Kind::Surface, "the node lists a surface output");
}

static void testPartialRdf() {
    // A rock-salt arrangement of two types.
    Dataset d = simpleCubic(6, 2.0);
    d.species = {"Na", "Cl"};
    for (size_t i = 0; i < d.atoms.size(); ++i) {
        const int ix = int(std::lround(d.atoms[i].x / 2)), iy = int(std::lround(d.atoms[i].y / 2)), iz = int(std::lround(d.atoms[i].z / 2));
        d.atoms[i].type = uint32_t((ix + iy + iz) % 2);
    }
    Modifier rdf{Op::RadialDistribution};
    rdf.value = 5.5f;
    rdf.rdfBins = 55;
    rdf.clusterByBonds = true;
    const auto out = evaluateFrom(resultOf(d), {rdf}, 0).data;
    const auto &partial = tableNamed(out, "Partial radial distribution functions");
    const auto &total = tableNamed(out, "Radial distribution function");
    check(partial.columns.size() == 4 && partial.columns[1] == "Na-Na" && partial.columns[2] == "Na-Cl" && partial.columns[3] == "Cl-Cl", "partial columns are named by type pair");
    // Nearest neighbors at 2.0 are unlike; second neighbors at 2.83 are like.
    const auto peak = [&](double radius, size_t column) { return std::stod(partial.rows[size_t(radius / .1)][column]); };
    check(peak(2.0, 2) > 0 && peak(2.0, 1) == 0 && peak(2.0, 3) == 0, "the first shell holds only unlike pairs");
    check(peak(2.83, 2) == 0 && peak(2.83, 1) > 0 && peak(2.83, 3) > 0, "the second shell holds only like pairs");
    bool sumMatches = partial.rows.size() == total.rows.size();
    for (size_t row = 0; sumMatches && row < partial.rows.size(); ++row) {
        // Equal concentrations: g = (g_aa + 2 g_ab + g_bb) / 4.
        const double sum = (std::stod(partial.rows[row][1]) + 2 * std::stod(partial.rows[row][2]) + std::stod(partial.rows[row][3])) / 4;
        sumMatches = std::abs(sum - std::stod(total.rows[row][2])) < 1e-4 * std::max(1.0, sum);
    }
    check(sumMatches, "the weighted partial RDFs add up to the total RDF");

    Modifier plain = rdf;
    plain.clusterByBonds = false;
    const auto noPartial = evaluateFrom(resultOf(d), {plain}, 0).data;
    check(std::none_of(noPartial.tables.begin(), noPartial.tables.end(), [](const DataTable &t) { return t.name == "Partial radial distribution functions"; }), "partial RDFs are optional");

    // Only selected: unselected types disappear from the table.
    PipelineResult partialInput = resultOf(d);
    for (size_t i = 0; i < d.atoms.size(); ++i) partialInput.selected[i] = d.atoms[i].type == 0 ? 1 : 0;
    Modifier selectedOnly = rdf;
    selectedOnly.neighborOnlySelected = true;
    const auto chosen = evaluateFrom(partialInput, {selectedOnly}, 0).data;
    check(tableNamed(chosen, "Partial radial distribution functions").columns.size() == 2, "only selected particle types get columns");
}

static void testCancellation() {
    std::atomic<bool> cancel{true};
    {
        Dataset d = fcc(12);
        throwsWith([&] { computePolyhedralTemplateMatching(d, {}, nullptr, &cancel); }, "Cancelled", "PTM cancels");
        check(!d.scalarProperties.count("Structure Type"), "cancelled PTM publishes nothing");
    }
    {
        Dataset d = cubicIce(2);
        throwsWith([&] { computeChillPlus(d, 3.5, nullptr, &cancel); }, "Cancelled", "Chill+ cancels");
        Dataset e = fcc(4);
        throwsWith([&] { computeLocalEntropy(e, {}, &cancel); }, "Cancelled", "local entropy cancels");
        Dataset f = stackedLayers("ABCABCABABCABC");
        throwsWith([&] { computeFccPlanarFaults(f, &cancel); }, "Cancelled", "planar faults cancel");
        Dataset g = fcc(4);
        throwsWith([&] { computeSpatialCorrelation(g, {}, &cancel); }, "Cancelled", "spatial correlation cancels");
        check(!d.scalarProperties.count("Structure Type") && !e.scalarProperties.count("Entropy") &&
              g.tables.empty(), "cancelled structure analyses publish nothing");
    }
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
    testChillPlus();
    testLocalEntropy();
    testPlanarFaults();
    testSpatialCorrelation();
    testPtm();
    testGrainSegmentation();
    testElasticStrain();
    testHideSelected();
    testCommonNeighborModes();
    testLoadTrajectory();
    testCombineDatasets();
    testMeasure();
    testPartialRdf();
    testBondProperties();
    testTrajectoryLines();
    testCoordinationPolyhedra();
    testCancellation();
    if (failures) {
        std::cerr << failures << " of " << checks << " checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "PASS: " << checks << " checks: Steinhardt, Ackland-Jones, atomic strain, Wigner-Seitz, "
                 "center of mass, shrink-wrap, frame differences, spatial binning, structure factor, "
                 "diamond identification, trajectory sweeps, diffusion, rings, Voronoi, Chill+, local entropy, planar faults, spatial correlation, PTM, grain segmentation, elastic strain, hide selected, CNA modes, load trajectory, combine datasets, measure, partial RDF, "
                 "pipeline composition, cancellation\n";
}
