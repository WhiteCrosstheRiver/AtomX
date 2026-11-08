// Spatial correlation function C(r) = <P1(0) P2(r)> between two particle properties.
// Included inside namespace atomx by core.hpp. The workflow follows OVITO's Spatial
// correlation function modifier (FFT of gridded property densities, radial averages in
// real and reciprocal space, optional direct neighbor sum); the code is AtomX's own.

struct CorrelationOptions {
    std::string property1, property2; // Empty means the constant 1 (gives the RDF and S(k)).
    double gridSpacing = 3.0;
    bool applyWindow = true;         // Hann window along non-periodic axes.
    bool directSummation = false;
    double neighborCutoff = 5.0;
    int neighborBins = 50;
    bool differenceCorrelation = false;
    bool normalizeByRdf = false;
    bool normalizeByCovariance = false;
};

// In-place discrete Fourier transform of every line of a 3D grid along one axis.
// Plain O(n^2) per line, which is cheap at the grid sizes allowed below.
inline void transformGridAxis(std::vector<std::complex<double>> &grid, const std::array<size_t, 3> &n, int axis,
                              bool inverse, std::atomic<bool> *cancel) {
    const size_t length = n[size_t(axis)];
    if (length < 2) return;
    constexpr double tau = 2 * 3.14159265358979323846;
    std::vector<std::complex<double>> twiddle(length), line(length), out(length);
    for (size_t k = 0; k < length; ++k)
        twiddle[k] = std::polar(1.0, (inverse ? tau : -tau) * double(k) / double(length));
    const size_t stride = axis == 0 ? 1 : axis == 1 ? n[0] : n[0] * n[1];
    const size_t lines = grid.size() / length;
    for (size_t lineIndex = 0; lineIndex < lines; ++lineIndex) {
        if ((lineIndex & 255) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
        // Decompose the line index into the two other grid coordinates.
        size_t start;
        if (axis == 0) start = lineIndex * n[0];
        else if (axis == 1) start = (lineIndex / n[0]) * n[0] * n[1] + lineIndex % n[0];
        else start = lineIndex;
        for (size_t j = 0; j < length; ++j) line[j] = grid[start + j * stride];
        for (size_t k = 0; k < length; ++k) {
            std::complex<double> sum = 0;
            size_t phase = 0;
            for (size_t j = 0; j < length; ++j) {
                sum += line[j] * twiddle[phase];
                phase += k;
                if (phase >= length) phase -= length;
            }
            out[k] = sum;
        }
        for (size_t j = 0; j < length; ++j) grid[start + j * stride] = out[j];
    }
}
inline void transformGrid(std::vector<std::complex<double>> &grid, const std::array<size_t, 3> &n, bool inverse,
                          std::atomic<bool> *cancel) {
    for (int axis = 0; axis < 3; ++axis) transformGridAxis(grid, n, axis, inverse, cancel);
}

inline std::string formatCorrelationNumber(double value) {
    std::ostringstream stream;
    stream << std::setprecision(10) << value;
    return stream.str();
}

inline void computeSpatialCorrelation(Dataset &data, const CorrelationOptions &options, std::atomic<bool> *cancel) {
    if (!(options.gridSpacing > 0) || !std::isfinite(options.gridSpacing))
        throw std::runtime_error("Spatial correlation needs a positive FFT grid spacing");
    if (data.sampled()) throw std::runtime_error("Spatial correlation requires complete, unsampled particle data");
    const size_t count = data.atoms.size();
    if (count == 0) throw std::runtime_error("Spatial correlation has no particles to analyze");
    const double volume = std::abs(cellDeterminant(data.cell));
    if (!(volume > 0)) throw std::runtime_error("Spatial correlation needs a simulation cell");
    const auto valuesOf = [&](const std::string &name) {
        return name.empty() ? std::vector<double>(count, 1.0) : particlePropertyValues(data, name, cancel);
    };
    const std::vector<double> p1 = valuesOf(options.property1), p2 = valuesOf(options.property2);
    double mean1 = 0, mean2 = 0, product = 0, square1 = 0, square2 = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!std::isfinite(p1[i]) || !std::isfinite(p2[i]))
            throw std::runtime_error("Spatial correlation input properties must be finite");
        mean1 += p1[i]; mean2 += p2[i]; product += p1[i] * p2[i];
        square1 += p1[i] * p1[i]; square2 += p2[i] * p2[i];
    }
    mean1 /= double(count); mean2 /= double(count); product /= double(count);
    square1 /= double(count); square2 /= double(count);

    const auto inverse = cellInverse(data.cell);
    // Face-to-face widths of the cell; each holds a whole number of grid cells.
    std::array<double, 3> width{};
    std::array<size_t, 3> cells{}, n{};
    double cellLength = std::numeric_limits<double>::infinity();
    for (int a = 0; a < 3; ++a) {
        width[size_t(a)] = 1.0 / std::hypot(inverse[size_t(a)][0], inverse[size_t(a)][1], inverse[size_t(a)][2]);
        cells[size_t(a)] = size_t(std::max(1.0, std::floor(width[size_t(a)] / options.gridSpacing)));
        if (cells[size_t(a)] > 1024) throw std::runtime_error("Spatial correlation FFT grid is too fine; increase the grid spacing");
        // Non-periodic axes are zero-padded so the FFT does not wrap around.
        n[size_t(a)] = data.pbc[size_t(a)] ? cells[size_t(a)] : 2 * cells[size_t(a)];
        cellLength = std::min(cellLength, width[size_t(a)] / double(cells[size_t(a)]));
    }
    const size_t total = n[0] * n[1] * n[2];
    if (total > (size_t(1) << 23) || double(total) * double(n[0] + n[1] + n[2]) * 7 > 1.5e10)
        throw std::runtime_error("Spatial correlation FFT grid is too large; increase the grid spacing");
    const size_t original = cells[0] * cells[1] * cells[2];
    constexpr double pi = 3.14159265358979323846, tau = 2 * pi;
    // Grids of P1, P2 and the particle count per cell, plus the window mask W, which is
    // 1 inside the cell (times the Hann window on windowed axes) and 0 in the padding.
    std::vector<std::complex<double>> g1(total), g2(total), g0(total), mask(total);
    const auto windowAt = [&](int a, double s) {
        return (!data.pbc[size_t(a)] && options.applyWindow) ? std::pow(std::sin(pi * std::clamp(s, 0.0, 1.0)), 2) : 1.0;
    };
    for (size_t i = 0; i < count; ++i) {
        if ((i & 65535) == 0 && cancel && *cancel) throw std::runtime_error("Cancelled");
        const auto s = fractionalPosition(data, data.atoms[i], inverse);
        size_t index = 0, factor = 1;
        double weight = 1;
        for (int a = 0; a < 3; ++a) {
            double reduced = s[size_t(a)];
            if (data.pbc[size_t(a)]) reduced -= std::floor(reduced);
            else reduced = std::clamp(reduced, 0.0, 1.0);
            const auto c = std::min(cells[size_t(a)] - 1, size_t(reduced * double(cells[size_t(a)])));
            weight *= windowAt(a, reduced);
            index += c * factor;
            factor *= n[size_t(a)];
        }
        g1[index] += weight * p1[i];
        g2[index] += weight * p2[i];
        g0[index] += weight;
    }
    double maskPower = 0;
    for (size_t z = 0; z < cells[2]; ++z)
        for (size_t y = 0; y < cells[1]; ++y)
            for (size_t x = 0; x < cells[0]; ++x) {
                const double w = windowAt(0, (x + .5) / double(cells[0])) * windowAt(1, (y + .5) / double(cells[1])) *
                                 windowAt(2, (z + .5) / double(cells[2]));
                mask[x + n[0] * (y + n[1] * z)] = w;
                maskPower += w * w;
            }
    for (auto *grid : {&g1, &g2, &g0, &mask}) transformGrid(*grid, n, false, cancel);

    // Reciprocal space: C(k) = Re(F1 F2*) / (N <W^2>), which is S(k) for unit properties.
    const auto wrapped = [&](size_t index, int a) {
        const auto length = int64_t(n[size_t(a)]);
        return int64_t(index) >= (length + 1) / 2 ? int64_t(index) - length : int64_t(index);
    };
    const double powerScale = maskPower / double(original);
    double kStep = std::numeric_limits<double>::infinity(), kMax = 0;
    for (int a = 0; a < 3; ++a) {
        const double b = tau * std::hypot(inverse[size_t(a)][0], inverse[size_t(a)][1], inverse[size_t(a)][2]) *
                         double(cells[size_t(a)]) / double(n[size_t(a)]);
        if (n[size_t(a)] > 1) kStep = std::min(kStep, b);
    }
    std::vector<std::complex<double>> r12(total), r00(total), rww(total);
    std::vector<std::pair<double, double>> reciprocal; // (|k|, C(k))
    for (size_t index = 0; index < total; ++index) {
        r12[index] = g1[index] * std::conj(g2[index]);
        r00[index] = g0[index] * std::conj(g0[index]);
        rww[index] = mask[index] * std::conj(mask[index]);
        if (index == 0 || !std::isfinite(kStep)) continue;
        const size_t x = index % n[0], y = (index / n[0]) % n[1], z = index / (n[0] * n[1]);
        const int64_t m[3]{wrapped(x, 0), wrapped(y, 1), wrapped(z, 2)};
        double k[3]{};
        for (int a = 0; a < 3; ++a)
            for (int xyz = 0; xyz < 3; ++xyz)
                k[xyz] += tau * double(m[a]) * double(cells[size_t(a)]) / double(n[size_t(a)]) * inverse[size_t(a)][size_t(xyz)];
        const double length = std::hypot(k[0], k[1], k[2]);
        kMax = std::max(kMax, length);
        reciprocal.push_back({length, r12[index].real() / (double(count) * powerScale)});
    }
    for (auto *grid : {&r12, &r00, &rww}) transformGrid(*grid, n, true, cancel);

    // Real space: C(d) = c12(d) M^2 / (N^2 A(d)), with A the mask autocorrelation, so that
    // unit properties give g(r) -> 1 at long range. Averaged in shells up to half the cell.
    double rMax = std::numeric_limits<double>::infinity();
    for (int a = 0; a < 3; ++a) rMax = std::min(rMax, .5 * width[size_t(a)]);
    const size_t realBins = std::max<size_t>(1, size_t(rMax / cellLength));
    const double realStep = rMax / double(realBins);
    std::vector<double> realSum(realBins, 0.0), rdfSum(realBins, 0.0);
    std::vector<uint64_t> realSamples(realBins, 0);
    const double scale = double(original) * double(original) / (double(count) * double(count) * double(total));
    for (size_t index = 0; index < total; ++index) {
        const double overlap = rww[index].real() / double(total);
        if (!(overlap > 1e-9 * double(original))) continue;
        const size_t x = index % n[0], y = (index / n[0]) % n[1], z = index / (n[0] * n[1]);
        const int64_t m[3]{wrapped(x, 0), wrapped(y, 1), wrapped(z, 2)};
        double d[3]{};
        for (int a = 0; a < 3; ++a)
            for (int xyz = 0; xyz < 3; ++xyz)
                d[xyz] += double(m[a]) / double(cells[size_t(a)]) * data.cell[size_t(a) * 3 + size_t(xyz)];
        const double r = std::hypot(d[0], d[1], d[2]);
        if (r >= rMax) continue;
        const auto bin = std::min(realBins - 1, size_t(r / realStep));
        realSum[bin] += r12[index].real() * scale / overlap;
        rdfSum[bin] += r00[index].real() * scale / overlap;
        ++realSamples[bin];
    }
    const double covariance = product - mean1 * mean2;
    const auto normalized = [&](double c, double rdf) {
        double value = c;
        if (options.normalizeByRdf) value = rdf != 0 ? value / rdf : 0.0;
        if (options.differenceCorrelation) value = .5 * (square1 + square2) - value;
        if (options.normalizeByCovariance)
            value = covariance != 0 ? (options.differenceCorrelation ? value : value - mean1 * mean2) / covariance : 0.0;
        return value;
    };
    DataTable real;
    real.name = "Spatial correlation (real space)";
    real.columns = {"Distance", "C(r)", "RDF", "Normalized"};
    for (size_t b = 0; b < realBins; ++b) {
        if (!realSamples[b]) continue;
        const double c = realSum[b] / double(realSamples[b]), rdf = rdfSum[b] / double(realSamples[b]);
        real.rows.push_back({formatCorrelationNumber((double(b) + .5) * realStep), formatCorrelationNumber(c),
                             formatCorrelationNumber(rdf), formatCorrelationNumber(normalized(c, rdf))});
    }
    DataTable reciprocalTable;
    reciprocalTable.name = "Spatial correlation (reciprocal space)";
    reciprocalTable.columns = {"k", "C(k)"};
    if (std::isfinite(kStep) && kMax > 0) {
        const size_t kBins = std::max<size_t>(1, size_t(std::ceil(kMax / kStep)));
        std::vector<double> sum(kBins, 0.0);
        std::vector<uint64_t> samples(kBins, 0);
        for (const auto &[k, c] : reciprocal) {
            const auto bin = std::min(kBins - 1, size_t(k / kStep));
            sum[bin] += c;
            ++samples[bin];
        }
        for (size_t b = 0; b < kBins; ++b)
            if (samples[b])
                reciprocalTable.rows.push_back({formatCorrelationNumber((double(b) + .5) * kStep),
                                                formatCorrelationNumber(sum[b] / double(samples[b]))});
    }
    data.tables.push_back(std::move(real));
    data.tables.push_back(std::move(reciprocalTable));

    if (options.directSummation) {
        if (!(options.neighborCutoff > 0) || !std::isfinite(options.neighborCutoff))
            throw std::runtime_error("Spatial correlation needs a positive neighbor cutoff");
        if (options.neighborBins < 1 || options.neighborBins > 100000)
            throw std::runtime_error("Spatial correlation needs 1 to 100000 neighbor bins");
        const size_t bins = size_t(options.neighborBins);
        const double step = options.neighborCutoff / double(bins);
        std::vector<double> sum(bins, 0.0), pairs(bins, 0.0);
        // Every ordered pair once: each particle's list holds all its neighbors and images.
        const auto lists = periodicNeighborLists(data, options.neighborCutoff, cancel);
        for (size_t i = 0; i < count; ++i)
            for (const auto &neighbor : lists[i]) {
                const auto bin = std::min(bins - 1, size_t(std::sqrt(neighbor.distanceSquared) / step));
                sum[bin] += p1[i] * p2[neighbor.index];
                pairs[bin] += 1;
            }
        const double rho = double(count) / volume;
        DataTable direct;
        direct.name = "Spatial correlation (neighbor sum)";
        direct.columns = {"Distance", "C(r)", "RDF", "Normalized"};
        for (size_t b = 0; b < bins; ++b) {
            const double lo = double(b) * step, hi = lo + step;
            const double shell = double(count) * rho * 4.0 / 3 * pi * (hi * hi * hi - lo * lo * lo);
            const double c = sum[b] / shell, rdf = pairs[b] / shell;
            direct.rows.push_back({formatCorrelationNumber(lo + .5 * step), formatCorrelationNumber(c),
                                   formatCorrelationNumber(rdf), formatCorrelationNumber(normalized(c, rdf))});
        }
        data.tables.push_back(std::move(direct));
    }
    data.globalAttributes["CorrelationFunction.mean1"] = mean1;
    data.globalAttributes["CorrelationFunction.mean2"] = mean2;
    data.globalAttributes["CorrelationFunction.covariance"] = product;
}
