#include "../src/core.hpp"
#include <iostream>
#include <chrono>
using namespace atomx;
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
int main() {
    auto dir = std::filesystem::temp_directory_path() /
               ("atomx-poscar-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    auto p = dir / "POSCAR";
    try {
        Dataset d;
        d.species = {"Cu", "Ni"};
        d.cell = {3, 1, 0, 0, 4, 1, 1, 0, 5};
        d.atoms = {{1, 2, 3, 1}, {4, 5, 6, 0}, {7, 8, 9, 1}};
        writePOSCAR(p, d);
        auto r = readInput(p);
        check(r.cell == d.cell && r.atoms.size() == 3, "cell/count roundtrip");
        check(r.species[r.atoms[0].type] == "Cu" && r.atoms[0].x == 4, "type grouping");
        check(r.species[r.atoms[1].type] == "Ni" && r.atoms[1].x == 1,
              "species-position association");
        auto fixture = [&](const std::string &body) {
            std::ofstream f(p);
            f << body;
        };
        fixture("scale\n2\n1 0 0\n0 1 0\n0 0 1\nSi\n1\nCartesian\n1 2 3\n");
        r = readInput(p);
        check(r.atoms[0].z == 6 && r.cell[0] == 2, "Cartesian scale");
        fixture("direct\n1\n3 1 0\n0 4 1\n1 0 5\nSi\n1\nDirect\n0.5 0.25 0.1\n");
        r = readInput(p);
        check(std::abs(r.atoms[0].x - 1.6f) < 1e-6f && std::abs(r.atoms[0].z - .75f) < 1e-6f,
              "triclinic direct coordinates");
        fixture("VASP4\n1\n1 0 0\n0 1 0\n0 0 1\n1 1\nDirect\n0 0 0\n.5 .5 .5\n");
        r = readInput(p);
        check(r.atoms.size() == 2 && r.species[1] == "X2", "VASP4 count parsing");
        fixture("bad\n1\n1 0 0\n0 1 0\n0 0 1\nSi\n1\nCartesian\nbad\n");
        bool failed = false;
        try {
            readInput(p);
        } catch (...) {
            failed = true;
        }
        check(failed, "malformed coordinate rejection");
        d.cell = {};
        failed = false;
        try {
            writePOSCAR(p, d);
        } catch (...) {
            failed = true;
        }
        check(failed, "singular export rejected");
        check(isPOSCAR(dir / "CONTCAR") && isPOSCAR(dir / "sample.VASP"), "filename detection");
        std::filesystem::remove(p);
        std::filesystem::remove(dir);
        std::cout << "PASS: POSCAR type association, triclinic roundtrip, scaling, VASP4, "
                     "validation, filenames\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
