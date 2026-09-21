#include "../src/analysis.hpp"
#include <iostream>
using namespace atomx;
void require(bool b, const char *s) {
    if (!b)
        throw std::runtime_error(s);
}
int main() {
    try {
        auto p = std::filesystem::temp_directory_path() / "atomx-core-fixture.xyz";
        {
            std::ofstream f(p);
            f << "4\nLattice=\"10 0 0 0 10 0 0 0 10\" Properties=species:S:1:pos:R:3 pbc=\"T F "
                 "T\"\nCu 0 1 2\nNi 3 4 5\nCu 6 7 8\nNi 11 2 -1\n1\nsecond\nC 9 8 7\n";
        }
        auto ix = indexXYZ(p);
        require(ix.size() == 2, "frame index");
        auto d = readXYZ(p, ix[0]);
        require(d.atoms.size() == 4 && d.species.size() == 2, "reader");
        require(d.cell[8] == 10 && d.pbc[0] && !d.pbc[1], "metadata");
        auto sample = readXYZ(p, ix[0], 2);
        require(sample.atoms.size() == 2 && sample.stride == 2 && sample.sourceCount == 4,
                "bounded sample");
        require(readXYZ(p, ix[1]).atoms[0].x == 9, "seek frame");
        auto r = evaluate(d, {{Op::SelectType, true, 0, 2, 1}, {Op::Delete}});
        require(r.data.atoms.size() == 2 && d.atoms.size() == 4,
                "selection deletion nondestructive");
        r = evaluate(d, {{Op::Slice, true, 4, 2}});
        require(r.data.atoms.size() == 2, "slice");
        r = evaluate(d, {{Op::Wrap}});
        require(r.data.atoms.back().x == 1 && r.data.atoms.back().z == 9, "periodic wrapping");
        r = evaluate(d, {{Op::Scale, true, 2}});
        require(r.data.cell[0] == 20 && r.data.atoms[1].x == 6, "scale");
        auto s = statistics(d, 0);
        require(s.min == 0 && s.max == 11 && s.mean == 5, "statistics");
        float sum = 0;
        for (auto v : s.histogram)
            sum += v;
        require(sum == 4, "histogram population");
        writeXYZ(p, d);
        auto round = readXYZ(p, indexXYZ(p)[0]);
        require(round.atoms.size() == 4 && round.pbc == d.pbc, "round trip");
        {
            std::ofstream f(p);
            f << "1\nProperties=id:I:1:pos:R:3:species:S:1\n99 1.5 2.5 3.5 Si\n";
        }
        auto custom = readXYZ(p, indexXYZ(p)[0]);
        require(custom.species[0] == "Si" && custom.atoms[0].y == 2.5, "reordered schema");
        {
            std::ofstream f(p);
            f << "2\ntruncated\nCu 0 0 0\n";
        }
        bool failed = false;
        try {
            indexXYZ(p);
        } catch (...) {
            failed = true;
        }
        require(failed, "truncated input rejected");
        auto fcc = crystal(4);
        fcc.pbc = {true, true, true};
        auto n = neighbors(fcc, .8f);
        require(n.clusters == 1 && n.meanCoordination == 12, "periodic FCC coordination");
        for (auto c : n.coordination)
            require(c == 12, "FCC nearest neighbors");
        Dataset pair;
        pair.species = {"X"};
        pair.atoms = {{.1f, 0, 0, 0}, {1.9f, 0, 0, 0}, {1, 1, 1, 0}};
        pair.cell = {2, 0, 0, 0, 2, 0, 0, 0, 2};
        pair.pbc = {true, true, true};
        pair.bounds();
        auto pn = neighbors(pair, .3f);
        require(pn.bonds == 1 && pn.clusters == 2 && pn.coordination[2] == 0,
                "minimum image cluster");
        auto twoBin = neighbors(pair, .9f);
        require(twoBin.bonds == 1, "deduplicate periodic bins");
        pair.stride = 2;
        failed = false;
        try {
            neighbors(pair, .3f);
        } catch (...) {
            failed = true;
        }
        require(failed, "sampled analysis rejected");
        std::filesystem::remove(p);
        std::cout << "PASS: index, seek, schema, metadata, sampling, selection, slice, wrap, "
                     "scale, histogram, roundtrip, malformed input, FCC coordination, periodic "
                     "clusters, sampled analysis rejection\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
