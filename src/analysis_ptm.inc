// Polyhedral template matching (Larsen, Schmidt & Schiotz, Modelling Simul. Mater. Sci.
// Eng. 24, 055007 (2016)). Included inside namespace atomx by core.hpp.
//
// Templates and their scaling (mean neighbor distance 1, central atom at the origin)
// follow the OVITO reference templates. RMSD, scale and interatomic distance use the
// definitions of the MIT-licensed reference PTM library (P. M. Larsen): both point sets,
// central atom included, are centered on their barycentre, the observed set is scaled by
// s = E / G2 and RMSD = sqrt(|G1 - s E| / N). AtomX finds the neighbor-to-template
// correspondence by its own search instead of the library's convex-hull graph matching,
// and orders neighbors by distance rather than by Voronoi solid angle.

namespace ptm_detail {
using Vec = std::array<double, 3>;
using Quat = std::array<double, 4>; // (w, x, y, z)

inline double dot(const Vec &a, const Vec &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline Vec cross(const Vec &a, const Vec &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline double norm(const Vec &a) { return std::sqrt(dot(a, a)); }
inline Quat multiply(const Quat &a, const Quat &b) {
    return {a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
            a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
            a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
            a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]};
}
inline std::array<double, 9> matrixOf(const Quat &q) {
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    return {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
            2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
            2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)};
}
inline Vec rotateBy(const std::array<double, 9> &m, const Vec &v) {
    return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
            m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}
inline Quat axisAngle(const Vec &axis, double angle) {
    const double s = std::sin(angle / 2), n = norm(axis);
    return {std::cos(angle / 2), axis[0] / n * s, axis[1] / n * s, axis[2] / n * s};
}

// Largest eigenpair of a symmetric 4x4 matrix by cyclic Jacobi rotations.
inline double largestEigen(std::array<std::array<double, 4>, 4> a, Quat &vector) {
    std::array<std::array<double, 4>, 4> v{};
    for (int i = 0; i < 4; ++i) v[size_t(i)][size_t(i)] = 1;
    for (int sweep = 0; sweep < 50; ++sweep) {
        double off = 0;
        for (int p = 0; p < 4; ++p)
            for (int q = p + 1; q < 4; ++q) off += a[size_t(p)][size_t(q)] * a[size_t(p)][size_t(q)];
        if (off < 1e-26) break;
        for (int p = 0; p < 4; ++p)
            for (int q = p + 1; q < 4; ++q) {
                const double apq = a[size_t(p)][size_t(q)];
                if (std::abs(apq) < 1e-300) continue;
                const double theta = (a[size_t(q)][size_t(q)] - a[size_t(p)][size_t(p)]) / (2 * apq);
                const double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1));
                const double c = 1 / std::sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < 4; ++k) {
                    const double akp = a[size_t(k)][size_t(p)], akq = a[size_t(k)][size_t(q)];
                    a[size_t(k)][size_t(p)] = c * akp - s * akq;
                    a[size_t(k)][size_t(q)] = s * akp + c * akq;
                }
                for (int k = 0; k < 4; ++k) {
                    const double apk = a[size_t(p)][size_t(k)], aqk = a[size_t(q)][size_t(k)];
                    a[size_t(p)][size_t(k)] = c * apk - s * aqk;
                    a[size_t(q)][size_t(k)] = s * apk + c * aqk;
                }
                for (int k = 0; k < 4; ++k) {
                    const double vkp = v[size_t(k)][size_t(p)], vkq = v[size_t(k)][size_t(q)];
                    v[size_t(k)][size_t(p)] = c * vkp - s * vkq;
                    v[size_t(k)][size_t(q)] = s * vkp + c * vkq;
                }
            }
    }
    int best = 0;
    for (int i = 1; i < 4; ++i)
        if (a[size_t(i)][size_t(i)] > a[size_t(best)][size_t(best)]) best = i;
    for (int i = 0; i < 4; ++i) vector[size_t(i)] = v[size_t(i)][size_t(best)];
    return a[size_t(best)][size_t(best)];
}

struct Fit {
    double rmsd = std::numeric_limits<double>::infinity();
    double scale = 0;    // Maps observed lengths onto template lengths.
    Quat rotation{1, 0, 0, 0}; // Rotates the template onto the observed environment.
    std::array<int8_t, 17> mapping{}; // Template point -> observed point.
};
// RMSD of an observed set against a template under a fixed correspondence, as in the
// reference library: barycentres removed, optimal rotation by Horn's quaternion method.
inline Fit fitCorrespondence(const std::vector<Vec> &observed, const std::vector<Vec> &ideal,
                             const std::array<int8_t, 17> &mapping) {
    const size_t n = ideal.size();
    Vec co{}, ct{};
    for (size_t i = 0; i < n; ++i)
        for (int k = 0; k < 3; ++k) { co[size_t(k)] += observed[size_t(mapping[i])][size_t(k)]; ct[size_t(k)] += ideal[i][size_t(k)]; }
    for (int k = 0; k < 3; ++k) { co[size_t(k)] /= double(n); ct[size_t(k)] /= double(n); }
    double S[3][3]{}, G1 = 0, G2 = 0;
    for (size_t i = 0; i < n; ++i) {
        Vec a, b;
        for (int k = 0; k < 3; ++k) {
            a[size_t(k)] = ideal[i][size_t(k)] - ct[size_t(k)];
            b[size_t(k)] = observed[size_t(mapping[i])][size_t(k)] - co[size_t(k)];
        }
        G1 += dot(a, a);
        G2 += dot(b, b);
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) S[j][k] += a[size_t(j)] * b[size_t(k)];
    }
    const std::array<std::array<double, 4>, 4> N{{
        {S[0][0] + S[1][1] + S[2][2], S[1][2] - S[2][1], S[2][0] - S[0][2], S[0][1] - S[1][0]},
        {S[1][2] - S[2][1], S[0][0] - S[1][1] - S[2][2], S[0][1] + S[1][0], S[2][0] + S[0][2]},
        {S[2][0] - S[0][2], S[0][1] + S[1][0], -S[0][0] + S[1][1] - S[2][2], S[1][2] + S[2][1]},
        {S[0][1] - S[1][0], S[2][0] + S[0][2], S[1][2] + S[2][1], -S[0][0] - S[1][1] + S[2][2]}}};
    Fit fit;
    fit.mapping = mapping;
    if (!(G2 > 0)) return fit;
    const double E = largestEigen(N, fit.rotation);
    fit.scale = E / G2;
    fit.rmsd = std::sqrt(std::abs(G1 - fit.scale * E) / double(n));
    const double length = std::sqrt(dot(Vec{fit.rotation[1], fit.rotation[2], fit.rotation[3]},
                                        Vec{fit.rotation[1], fit.rotation[2], fit.rotation[3]}) +
                                    fit.rotation[0] * fit.rotation[0]);
    for (auto &c : fit.rotation) c /= length;
    return fit;
}

struct Template {
    int type = 0;
    size_t inner = 0;                // Points 1..inner form the first shell used for anchoring.
    std::vector<Vec> points;         // Central atom first.
    std::vector<size_t> anchors;     // One first-shell point per orbit of the site symmetry.
    double nearestDistance = 1;      // |template[1]|, the interatomic distance in template units.
    const std::vector<Quat> *lattice = nullptr; // Rotation group of the lattice, for the fundamental zone.
};

// Correspondence search: align a pair of observed first-shell neighbors with every
// template pair at a similar angle, assign the remaining neighbors to the nearest
// template points, and keep the best bijective assignment.
inline Fit match(const std::vector<Vec> &observed, const Template &t, bool allAnchors = false,
                 double angleTolerance = 20 * 3.14159265358979323846 / 180) {
    Fit best;
    const size_t n = t.points.size();
    if (observed.size() != n) return best;
    const Vec &a = observed[1];
    size_t bIndex = 0;
    double bestCross = -1;
    for (size_t i = 2; i <= t.inner; ++i) {
        const double c = norm(cross(a, observed[i])) / (norm(a) * norm(observed[i]));
        if (c > bestCross) { bestCross = c; bIndex = i; }
    }
    if (!bIndex || bestCross < 1e-6) return best;
    const Vec &b = observed[bIndex];
    const double angle = std::acos(std::clamp(dot(a, b) / (norm(a) * norm(b)), -1.0, 1.0));
    double observedLength = 0, idealLength = 0;
    for (size_t i = 1; i < n; ++i) { observedLength += norm(observed[i]); idealLength += norm(t.points[i]); }
    const double s0 = observedLength / idealLength;
    const auto frame = [](const Vec &u, const Vec &v) {
        const double lu = norm(u);
        const Vec e1{u[0] / lu, u[1] / lu, u[2] / lu};
        Vec e3 = cross(u, v);
        const double l3 = norm(e3);
        e3 = {e3[0] / l3, e3[1] / l3, e3[2] / l3};
        return std::array<Vec, 3>{e1, cross(e3, e1), e3};
    };
    const auto fo = frame(a, b);
    std::vector<size_t> anchorList;
    if (allAnchors) for (size_t i = 1; i <= t.inner; ++i) anchorList.push_back(i);
    else anchorList = t.anchors;
    std::array<int8_t, 17> mapping{};
    std::array<bool, 17> used{};
    std::vector<Vec> rotated(n);
    for (size_t ta : anchorList)
        for (size_t tb = 1; tb <= t.inner; ++tb) {
            if (tb == ta) continue;
            const Vec &pa = t.points[ta], &pb = t.points[tb];
            const double templateAngle = std::acos(std::clamp(dot(pa, pb) / (norm(pa) * norm(pb)), -1.0, 1.0));
            if (std::abs(templateAngle - angle) > angleTolerance) continue;
            if (norm(cross(pa, pb)) < 1e-9) continue;
            const auto ft = frame(pa, pb);
            // R = Fo * Ft^T maps the template frame onto the observed one.
            std::array<double, 9> R{};
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    for (int k = 0; k < 3; ++k) R[size_t(r * 3 + c)] += fo[size_t(k)][size_t(r)] * ft[size_t(k)][size_t(c)];
            for (size_t j = 1; j < n; ++j) {
                rotated[j] = rotateBy(R, t.points[j]);
                for (auto &c : rotated[j]) c *= s0;
            }
            used.fill(false);
            mapping[0] = 0;
            bool bijective = true;
            for (size_t j = 1; j < n && bijective; ++j) {
                // Template point j takes its nearest observed point.
                size_t nearest = 0;
                double nearestDistance = std::numeric_limits<double>::infinity();
                for (size_t i = 1; i < n; ++i) {
                    const Vec d{observed[i][0] - rotated[j][0], observed[i][1] - rotated[j][1], observed[i][2] - rotated[j][2]};
                    const double dd = dot(d, d);
                    if (dd < nearestDistance) { nearestDistance = dd; nearest = i; }
                }
                if (used[nearest]) bijective = false;
                used[nearest] = true;
                mapping[j] = int8_t(nearest);
            }
            if (!bijective) continue;
            const Fit fit = fitCorrespondence(observed, t.points, mapping);
            if (fit.rmsd < best.rmsd) best = fit;
        }
    return best;
}

// Proper rotations that map a template onto itself, found with the matcher itself.
inline std::vector<Quat> siteSymmetry(const Template &t) {
    std::vector<Quat> group;
    const size_t n = t.points.size();
    // A first-shell pair (1, b) can map onto any pair (p, q) of equal lengths and angle;
    // keep the rotations that map the whole point set onto itself.
    const Vec &a = t.points[1];
    size_t bIndex = 0;
    for (size_t i = 2; i <= t.inner; ++i)
        if (norm(cross(a, t.points[i])) > 1e-6) { bIndex = i; break; }
    const Vec &b = t.points[bIndex];
    const auto frame = [](const Vec &u, const Vec &v) {
        const double lu = norm(u);
        const Vec e1{u[0] / lu, u[1] / lu, u[2] / lu};
        Vec e3 = cross(u, v);
        const double l3 = norm(e3);
        e3 = {e3[0] / l3, e3[1] / l3, e3[2] / l3};
        return std::array<Vec, 3>{e1, cross(e3, e1), e3};
    };
    const auto fa = frame(a, b);
    const double ab = dot(a, b), la = norm(a), lb = norm(b);
    for (size_t p = 1; p < n; ++p)
        for (size_t q = 1; q < n; ++q) {
            if (p == q) continue;
            const Vec &u = t.points[p], &v = t.points[q];
            if (std::abs(norm(u) - la) > 1e-6 || std::abs(norm(v) - lb) > 1e-6 || std::abs(dot(u, v) - ab) > 1e-6) continue;
            const auto fu = frame(u, v);
            std::array<double, 9> R{};
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    for (int k = 0; k < 3; ++k) R[size_t(r * 3 + c)] += fu[size_t(k)][size_t(r)] * fa[size_t(k)][size_t(c)];
            bool maps = true;
            for (size_t j = 1; j < n && maps; ++j) {
                const Vec r = rotateBy(R, t.points[j]);
                bool found = false;
                for (size_t k = 1; k < n && !found; ++k) {
                    const Vec d{r[0] - t.points[k][0], r[1] - t.points[k][1], r[2] - t.points[k][2]};
                    found = dot(d, d) < 1e-10;
                }
                maps = found;
            }
            if (!maps) continue;
            // Quaternion from the rotation matrix.
            Quat qu;
            const double trace = R[0] + R[4] + R[8];
            if (trace > 0) {
                const double s = std::sqrt(trace + 1) * 2;
                qu = {s / 4, (R[7] - R[5]) / s, (R[2] - R[6]) / s, (R[3] - R[1]) / s};
            } else if (R[0] > R[4] && R[0] > R[8]) {
                const double s = std::sqrt(1 + R[0] - R[4] - R[8]) * 2;
                qu = {(R[7] - R[5]) / s, s / 4, (R[1] + R[3]) / s, (R[2] + R[6]) / s};
            } else if (R[4] > R[8]) {
                const double s = std::sqrt(1 + R[4] - R[0] - R[8]) * 2;
                qu = {(R[2] - R[6]) / s, (R[1] + R[3]) / s, s / 4, (R[5] + R[7]) / s};
            } else {
                const double s = std::sqrt(1 + R[8] - R[0] - R[4]) * 2;
                qu = {(R[3] - R[1]) / s, (R[2] + R[6]) / s, (R[5] + R[7]) / s, s / 4};
            }
            bool duplicate = false;
            for (const auto &g : group)
                duplicate |= std::abs(std::abs(g[0] * qu[0] + g[1] * qu[1] + g[2] * qu[2] + g[3] * qu[3]) - 1) < 1e-8;
            if (!duplicate) group.push_back(qu);
        }
    return group;
}
inline std::vector<size_t> orbitAnchors(const Template &t, const std::vector<Quat> &group) {
    std::vector<int> orbit(t.points.size(), -1);
    std::vector<size_t> anchors;
    for (size_t p = 1; p <= t.inner; ++p) {
        if (orbit[p] >= 0) continue;
        anchors.push_back(p);
        for (const auto &g : group) {
            const Vec r = rotateBy(matrixOf(g), t.points[p]);
            for (size_t k = 1; k <= t.inner; ++k) {
                const Vec d{r[0] - t.points[k][0], r[1] - t.points[k][1], r[2] - t.points[k][2]};
                if (dot(d, d) < 1e-10) orbit[k] = int(p);
            }
        }
    }
    return anchors;
}

struct Library {
    std::vector<Quat> cubic, hexagonal, icosahedral;
    std::array<Template, 9> templates; // Indexed by structure type 1..8.
};
inline const Library &library() {
    static const Library lib = [] {
        Library l;
        const double r2 = std::sqrt(0.5);
        auto &fcc = l.templates[1];
        fcc.type = 1;
        fcc.points = {{0, 0, 0}};
        for (int a = 0; a < 3; ++a)
            for (int s1 : {1, -1})
                for (int s2 : {1, -1}) {
                    Vec v{};
                    v[size_t(a)] = s1 * r2;
                    v[size_t((a + 1) % 3)] = s2 * r2;
                    fcc.points.push_back(v);
                }
        auto &hcp = l.templates[2];
        hcp.type = 2;
        hcp.points = {{0, 0, 0}};
        const double h = std::sqrt(2.0 / 3.0), s3 = std::sqrt(3.0);
        for (int k = 0; k < 6; ++k) hcp.points.push_back({std::cos(k * 3.14159265358979323846 / 3), std::sin(k * 3.14159265358979323846 / 3), 0});
        for (double z : {h, -h})
            for (const Vec &v : {Vec{0, -1 / s3, 0}, Vec{.5, s3 / 6, 0}, Vec{-.5, s3 / 6, 0}}) hcp.points.push_back({v[0], v[1], z});
        auto &bcc = l.templates[3];
        bcc.type = 3;
        const double c = 7 * s3 / 3 - 3.5;
        bcc.points = {{0, 0, 0}};
        for (int x : {1, -1}) for (int y : {1, -1}) for (int z : {1, -1}) bcc.points.push_back({x * c, y * c, z * c});
        for (int a = 0; a < 3; ++a)
            for (int s : {1, -1}) { Vec v{}; v[size_t(a)] = 2 * c * s; bcc.points.push_back(v); }
        auto &ico = l.templates[4];
        ico.type = 4;
        ico.points = {{0, 0, 0}, {0, 0, 1}, {0, 0, -1}};
        const double zr = 1 / std::sqrt(5.0), rr = std::sqrt(1 - zr * zr);
        for (int k = 0; k < 5; ++k) {
            const double up = (18 + 72 * k) * 3.14159265358979323846 / 180, down = (-90 + 72 * k) * 3.14159265358979323846 / 180;
            ico.points.push_back({rr * std::cos(up), rr * std::sin(up), zr});
            ico.points.push_back({rr * std::cos(down), rr * std::sin(down), -zr});
        }
        auto &sc = l.templates[5];
        sc.type = 5;
        sc.points = {{0, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        // Diamond: four bonds of length b, then each bonded neighbor's three further bonds.
        const double b = s3 * 4 / (6 * std::sqrt(2.0) + s3);
        auto &dcub = l.templates[6];
        dcub.type = 6;
        dcub.points = {{0, 0, 0}};
        const double e = b / s3;
        const std::array<Vec, 4> bonds{Vec{e, e, e}, Vec{e, -e, -e}, Vec{-e, -e, e}, Vec{-e, e, -e}};
        for (const auto &v : bonds) dcub.points.push_back(v);
        for (const auto &v : bonds)
            for (const auto &w : bonds) {
                const Vec u{-w[0], -w[1], -w[2]};
                if (dot(u, Vec{-v[0], -v[1], -v[2]}) > b * b * .99) continue; // The bond back to the center.
                dcub.points.push_back({v[0] + u[0], v[1] + u[1], v[2] + u[2]});
            }
        auto &dhex = l.templates[7];
        dhex.type = 7;
        dhex.points = {{0, 0, 0}};
        const double rb = b * 2 * std::sqrt(2.0) / 3, deg = 3.14159265358979323846 / 180;
        const std::array<double, 3> lower{-90 * deg, 30 * deg, 150 * deg};
        for (double t : lower) dhex.points.push_back({rb * std::cos(t), rb * std::sin(t), -b / 3});
        dhex.points.push_back({0, 0, b});
        for (double t : lower) {
            const Vec j{rb * std::cos(t), rb * std::sin(t), -b / 3};
            dhex.points.push_back({j[0], j[1], j[2] - b});
            for (double o : {t + 60 * deg, t - 60 * deg})
                dhex.points.push_back({j[0] + rb * std::cos(o), j[1] + rb * std::sin(o), 0});
        }
        for (double t : lower) dhex.points.push_back({rb * std::cos(t), rb * std::sin(t), b + b / 3}); // Eclipsed along c.
        auto &graphene = l.templates[8];
        graphene.type = 8;
        const double g = -3.0 / 11 + 6 * s3 / 11; // Bond length when the mean neighbor distance is 1.
        graphene.points = {{0, 0, 0}};
        const std::array<double, 3> spokes{90 * deg, -30 * deg, 210 * deg};
        for (double t : spokes) graphene.points.push_back({g * std::cos(t), g * std::sin(t), 0});
        for (double t : spokes) {
            const Vec j{g * std::cos(t), g * std::sin(t), 0};
            for (double o : {t + 60 * deg, t - 60 * deg})
                graphene.points.push_back({j[0] + g * std::cos(o), j[1] + g * std::sin(o), 0});
        }
        const std::array<size_t, 9> inner{0, 12, 12, 14, 12, 6, 4, 4, 3};
        for (size_t type = 1; type <= 8; ++type) {
            auto &t = l.templates[type];
            t.inner = inner[type];
            t.nearestDistance = norm(t.points[1]);
        }
        l.cubic = siteSymmetry(fcc);
        l.icosahedral = siteSymmetry(ico);
        for (int k = 0; k < 6; ++k) {
            l.hexagonal.push_back(axisAngle({0, 0, 1}, k * 60 * deg));
            l.hexagonal.push_back(axisAngle({std::cos(k * 30 * deg), std::sin(k * 30 * deg), 0}, 180 * deg));
        }
        for (size_t type = 1; type <= 8; ++type) {
            auto &t = l.templates[type];
            t.anchors = orbitAnchors(t, siteSymmetry(t));
            t.lattice = (type == 2 || type == 7 || type == 8) ? &l.hexagonal : type == 4 ? &l.icosahedral : &l.cubic;
        }
        return l;
    }();
    return lib;
}
// The symmetry-equivalent orientation closest to the identity (largest |w|), with w >= 0.
inline Quat fundamentalZone(const Quat &q, const std::vector<Quat> &group, Quat *applied = nullptr) {
    Quat best = q, bestGroup{1, 0, 0, 0};
    double bestW = -1;
    for (const auto &g : group) {
        const Quat r = multiply(q, g);
        if (std::abs(r[0]) > bestW + 1e-12) { bestW = std::abs(r[0]); best = r; bestGroup = g; }
    }
    if (best[0] < 0) for (auto &c : best) c = -c;
    if (applied) *applied = bestGroup;
    return best;
}
} // namespace ptm_detail

enum class PtmType : uint8_t { Other, FCC, HCP, BCC, ICO, SC, CubicDiamond, HexagonalDiamond, Graphene };
inline constexpr std::array<const char *, 9> ptmTypeNames{"Other", "FCC", "HCP", "BCC", "ICO", "SC",
                                                          "Cubic diamond", "Hexagonal diamond", "Graphene"};
inline constexpr int ptmDefaultStructures = (1 << 0) | (1 << 1) | (1 << 2); // FCC, HCP, BCC.

struct PtmOptions {
    double rmsdCutoff = 0.1;          // 0 disables the cutoff.
    int structures = ptmDefaultStructures; // Bit (type - 1) enables a structure type.
    bool outputRmsd = false, outputInteratomicDistance = false, outputOrientation = false,
         outputDeformationGradient = false;
};
struct PtmEnvironmentResult {
    PtmType type = PtmType::Other;
    double rmsd = std::numeric_limits<double>::infinity();
    double interatomicDistance = 0;
    ptm_detail::Quat orientation{0, 0, 0, 0};
    std::array<double, 9> deformation{};
};

// Nearest-neighbor lists that stay complete in periodic cells too small for the search
// radius: such cells are replicated internally and indices map back to the input.
inline NeighborVectorLists nearestNeighborsWithImages(const Dataset &d, size_t count, std::atomic<bool> *cancel) {
    validateNeighborAnalysisInput(d);
    const double volume = std::abs(cellDeterminant(d.cell));
    std::array<int, 3> copies{1, 1, 1};
    bool replicate = false;
    if (volume > 0 && !d.atoms.empty() && std::any_of(d.pbc.begin(), d.pbc.end(), [](bool p) { return p; })) {
        const double reach = 1.5 * std::cbrt(3 * double(count + 1) * volume / (4 * 3.14159265358979323846 * double(d.atoms.size())));
        for (size_t a = 0; a < 3; ++a) {
            if (!d.pbc[a]) continue;
            const double length = std::hypot(d.cell[a * 3], d.cell[a * 3 + 1], d.cell[a * 3 + 2]);
            if (.5 * length < reach) { copies[a] = int(std::ceil(2 * reach / length)); replicate = copies[a] > 1 || replicate; }
        }
    }
    if (!replicate) return nearestNeighborVectors(d, count, cancel);
    const size_t n = d.atoms.size();
    if (double(n) * copies[0] * copies[1] * copies[2] > 2000000)
        throw std::runtime_error("The periodic cell is too small for this analysis; replicate it first");
    Dataset big;
    big.species = d.species;
    big.pbc = d.pbc;
    big.origin = d.origin;
    for (size_t a = 0; a < 3; ++a)
        for (size_t xyz = 0; xyz < 3; ++xyz) big.cell[a * 3 + xyz] = d.cell[a * 3 + xyz] * copies[a];
    for (int z = 0; z < copies[2]; ++z)
        for (int y = 0; y < copies[1]; ++y)
            for (int x = 0; x < copies[0]; ++x)
                for (const auto &atom : d.atoms)
                    big.atoms.push_back({float(atom.x + x * d.cell[0] + y * d.cell[3] + z * d.cell[6]),
                                         float(atom.y + x * d.cell[1] + y * d.cell[4] + z * d.cell[7]),
                                         float(atom.z + x * d.cell[2] + y * d.cell[5] + z * d.cell[8]), atom.type});
    auto lists = nearestNeighborVectors(big, count, cancel);
    lists.resize(n);
    for (auto &list : lists)
        for (auto &neighbor : list) neighbor.index = uint32_t(neighbor.index % n);
    return lists;
}

// Classifies one particle from its neighbor lists (closest first).
inline PtmEnvironmentResult ptmClassify(const NeighborVectorLists &lists, size_t particle, const PtmOptions &options) {
    using namespace ptm_detail;
    const auto &lib = library();
    PtmEnvironmentResult result;
    const auto &list = lists[particle];
    Fit best;
    const Template *bestTemplate = nullptr;
    std::vector<Vec> observed, bestObserved;
    const auto consider = [&](const Template &t) {
        const Fit fit = match(observed, t);
        if (fit.rmsd < best.rmsd) { best = fit; bestTemplate = &t; bestObserved = observed; }
    };
    for (size_t type : {size_t(5), size_t(1), size_t(2), size_t(4), size_t(3)}) {
        if (!(options.structures & (1 << (type - 1)))) continue;
        const auto &t = lib.templates[type];
        const size_t neighbors = t.points.size() - 1;
        if (list.size() < neighbors) continue;
        observed.assign(1, Vec{0, 0, 0});
        for (size_t k = 0; k < neighbors; ++k) observed.push_back(list[k].vector);
        consider(t);
    }
    // Diamond and graphene: the first shell, then each first neighbor's own neighbors
    // other than the bond back to the center.
    for (size_t type : {size_t(6), size_t(7), size_t(8)}) {
        if (!(options.structures & (1 << (type - 1)))) continue;
        const auto &t = lib.templates[type];
        const size_t inner = t.inner, outer = type == 8 ? 2 : 3;
        if (list.size() < inner) continue;
        observed.assign(1, Vec{0, 0, 0});
        for (size_t k = 0; k < inner; ++k) observed.push_back(list[k].vector);
        bool complete = true;
        for (size_t k = 0; k < inner && complete; ++k) {
            const auto &first = list[k];
            size_t added = 0;
            for (const auto &second : lists[first.index]) {
                if (added == outer) break;
                const Vec back{first.vector[0] + second.vector[0], first.vector[1] + second.vector[1],
                               first.vector[2] + second.vector[2]};
                if (dot(back, back) < 1e-6 * first.distanceSquared) continue;
                observed.push_back(back);
                ++added;
            }
            complete = added == outer;
        }
        if (complete) consider(t);
    }
    if (!bestTemplate) return result;
    result.rmsd = best.rmsd;
    if (options.rmsdCutoff > 0 && best.rmsd > options.rmsdCutoff) return result;
    result.type = PtmType(bestTemplate->type);
    result.interatomicDistance = bestTemplate->nearestDistance / best.scale;
    Quat applied{1, 0, 0, 0};
    result.orientation = fundamentalZone(best.rotation, *bestTemplate->lattice, &applied);
    if (options.outputDeformationGradient) {
        // F minimizes sum |F t_i - s x_i|^2 over the matched points, expressed in the
        // template frame of the reported orientation.
        Vec ct{}, co{};
        const size_t n = bestTemplate->points.size();
        for (size_t i = 0; i < n; ++i)
            for (int k = 0; k < 3; ++k) {
                ct[size_t(k)] += bestTemplate->points[i][size_t(k)] / double(n);
                co[size_t(k)] += bestObserved[size_t(best.mapping[i])][size_t(k)] / double(n);
            }
        double A[3][3]{}, B[3][3]{}; // A = sum a a^T, B = sum b a^T.
        for (size_t i = 0; i < n; ++i) {
            Vec a, b;
            for (int k = 0; k < 3; ++k) {
                a[size_t(k)] = bestTemplate->points[i][size_t(k)] - ct[size_t(k)];
                b[size_t(k)] = best.scale * (bestObserved[size_t(best.mapping[i])][size_t(k)] - co[size_t(k)]);
            }
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) { A[r][c] += a[size_t(r)] * a[size_t(c)]; B[r][c] += b[size_t(r)] * a[size_t(c)]; }
        }
        const double det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) -
                           A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
                           A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
        if (std::abs(det) > 1e-12) {
            // Planar templates (graphene) leave A singular; their gradient stays undefined (0).
            double inv[3][3];
            inv[0][0] = (A[1][1] * A[2][2] - A[1][2] * A[2][1]) / det;
            inv[0][1] = (A[0][2] * A[2][1] - A[0][1] * A[2][2]) / det;
            inv[0][2] = (A[0][1] * A[1][2] - A[0][2] * A[1][1]) / det;
            inv[1][0] = (A[1][2] * A[2][0] - A[1][0] * A[2][2]) / det;
            inv[1][1] = (A[0][0] * A[2][2] - A[0][2] * A[2][0]) / det;
            inv[1][2] = (A[0][2] * A[1][0] - A[0][0] * A[1][2]) / det;
            inv[2][0] = (A[1][0] * A[2][1] - A[1][1] * A[2][0]) / det;
            inv[2][1] = (A[0][1] * A[2][0] - A[0][0] * A[2][1]) / det;
            inv[2][2] = (A[0][0] * A[1][1] - A[0][1] * A[1][0]) / det;
            double F[3][3]{};
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    for (int k = 0; k < 3; ++k) F[r][c] += B[r][k] * inv[k][c];
            // F S, so that a perfect crystal gives the matrix of the reported orientation.
            const auto S = matrixOf(applied);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) {
                    double v = 0;
                    for (int k = 0; k < 3; ++k) v += F[r][k] * S[size_t(k * 3 + c)];
                    result.deformation[size_t(r * 3 + c)] = v;
                }
        }
    }
    return result;
}

inline void computePolyhedralTemplateMatching(Dataset &data, const PtmOptions &options,
                                              const std::vector<uint8_t> *selection, std::atomic<bool> *cancel) {
    if (!(options.rmsdCutoff >= 0) || !std::isfinite(options.rmsdCutoff))
        throw std::runtime_error("PTM needs a non-negative RMSD cutoff");
    if ((options.structures & 0xff) == 0) throw std::runtime_error("PTM needs at least one structure type");
    Dataset subset;
    std::vector<uint32_t> original;
    const Dataset &input = selectedSubset(data, selection, subset, original);
    size_t needed = 0;
    for (size_t type = 1; type <= 8; ++type)
        if (options.structures & (1 << (type - 1)))
            needed = std::max(needed, ptm_detail::library().templates[type].inner);
    const auto lists = nearestNeighborsWithImages(input, needed, cancel);
    const size_t count = lists.size();
    std::vector<PtmEnvironmentResult> results(count);
    std::atomic<size_t> next{0};
    std::atomic<bool> stopped{false};
    const auto work = [&] {
        for (;;) {
            const size_t start = next.fetch_add(1024);
            if (start >= count || stopped) return;
            if (cancel && *cancel) { stopped = true; return; }
            for (size_t i = start; i < std::min(count, start + 1024); ++i) results[i] = ptmClassify(lists, i, options);
        }
    };
    const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    if (count < 4096 || threads == 1) work();
    else {
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; ++t) pool.emplace_back(work);
        for (auto &thread : pool) thread.join();
    }
    if (stopped || (cancel && *cancel)) throw std::runtime_error("Cancelled");

    const size_t total = data.atoms.size();
    const auto at = [&](size_t i) { return selection ? original[i] : i; };
    std::vector<double> structure(total, 0.0);
    std::array<uint64_t, 9> counts{};
    for (size_t i = 0; i < count; ++i) {
        structure[at(i)] = double(results[i].type);
        ++counts[size_t(results[i].type)];
    }
    counts[0] += total - count;
    data.scalarProperties["Structure Type"] = std::move(structure);
    // RMSD of the best match, whether or not it passed the cutoff; 0 where nothing matched.
    double maxRmsd = 0;
    for (const auto &r : results) if (std::isfinite(r.rmsd)) maxRmsd = std::max(maxRmsd, r.rmsd);
    if (options.outputRmsd) {
        std::vector<double> rmsd(total, 0.0);
        for (size_t i = 0; i < count; ++i) rmsd[at(i)] = std::isfinite(results[i].rmsd) ? results[i].rmsd : 0.0;
        data.scalarProperties["RMSD"] = std::move(rmsd);
    }
    if (options.outputInteratomicDistance) {
        std::vector<double> distance(total, 0.0);
        for (size_t i = 0; i < count; ++i) distance[at(i)] = results[i].interatomicDistance;
        data.scalarProperties["Interatomic Distance"] = std::move(distance);
    }
    if (options.outputOrientation) {
        // Stored as (x, y, z, w) like OVITO; (0, 0, 0, 0) where no structure matched.
        static constexpr const char *components[4]{"Orientation.X", "Orientation.Y", "Orientation.Z", "Orientation.W"};
        for (size_t c = 0; c < 4; ++c) {
            std::vector<double> values(total, 0.0);
            for (size_t i = 0; i < count; ++i) values[at(i)] = results[i].orientation[(c + 1) % 4];
            data.scalarProperties[components[c]] = std::move(values);
        }
    }
    if (options.outputDeformationGradient) {
        static constexpr const char *components[9]{"XX", "XY", "XZ", "YX", "YY", "YZ", "ZX", "ZY", "ZZ"};
        for (size_t c = 0; c < 9; ++c) {
            std::vector<double> values(total, 0.0);
            for (size_t i = 0; i < count; ++i) values[at(i)] = results[i].deformation[c];
            data.scalarProperties[std::string("Elastic Deformation Gradient.") + components[c]] = std::move(values);
        }
    }
    DataTable table;
    table.name = "Polyhedral template matching";
    table.columns = {"Structure", "Count", "Fraction", "Id"};
    for (size_t id = 0; id < 9; ++id) {
        data.globalAttributes[std::string("PolyhedralTemplateMatching.counts.") + ptmTypeNames[id]] = double(counts[id]);
        table.rows.push_back({ptmTypeNames[id], std::to_string(counts[id]),
            total ? formatDataNumber(double(counts[id]) / double(total)) : "0", std::to_string(id)});
    }
    data.tables.push_back(std::move(table));
    // Histogram of the best-match RMSD of every analyzed particle, as in OVITO's panel.
    DataTable histogram;
    histogram.name = "PTM RMSD histogram";
    histogram.columns = {"RMSD", "Count"};
    const size_t bins = 100;
    std::vector<uint64_t> binned(bins, 0);
    const double width = maxRmsd > 0 ? maxRmsd / double(bins) : 1.0;
    for (const auto &r : results)
        if (std::isfinite(r.rmsd)) ++binned[std::min(bins - 1, size_t(r.rmsd / width))];
    for (size_t b = 0; b < bins; ++b)
        histogram.rows.push_back({formatDataNumber((double(b) + .5) * width), std::to_string(binned[b])});
    data.tables.push_back(std::move(histogram));
    data.globalAttributes["PolyhedralTemplateMatching.rmsd_cutoff"] = options.rmsdCutoff;
}
