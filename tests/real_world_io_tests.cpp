// Regression tests for structure files as real tools write them (pymatgen /
// Materials Project, VESTA, VASP, parallel LAMMPS runs), not only AtomX's own
// round trips.
#include "../src/structure_io.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>

using namespace atomx;
namespace fs = std::filesystem;

static int failures = 0;
static void check(bool ok, const char *what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}
static fs::path fixture(const fs::path &dir, const std::string &name, const std::string &text) {
    auto p = dir / name;
    std::ofstream(p, std::ios::binary) << text;
    return p;
}
static Dataset load(const fs::path &p) { return io::read(p, io::index(p).at(0)); }
static std::string slurp(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
static bool throwsWith(const fs::path &p, const std::string &needle) {
    try {
        load(p);
    } catch (const std::exception &e) {
        return std::string(e.what()).find(needle) != std::string::npos;
    }
    return false;
}

int main() {
    auto dir = fs::temp_directory_path() / "atomx-real-world-io";
    fs::create_directories(dir);

    // pymatgen writes oxidation states into _atom_site_type_symbol.
    auto mp = fixture(dir, "mp.cif",
                      "data_Fe2O3\n_symmetry_space_group_name_H-M   'P 1'\n"
                      "_cell_length_a 5.035\n_cell_length_b 5.035\n_cell_length_c 13.747\n"
                      "_cell_angle_alpha 90\n_cell_angle_beta 90\n_cell_angle_gamma 120\n"
                      "_symmetry_Int_Tables_number 1\n"
                      "loop_\n _symmetry_equiv_pos_site_id\n _symmetry_equiv_pos_as_xyz\n  1  'x, y, z'\n"
                      "loop_\n _atom_site_type_symbol\n _atom_site_label\n _atom_site_fract_x\n"
                      " _atom_site_fract_y\n _atom_site_fract_z\n _atom_site_occupancy\n"
                      "  Fe3+ Fe0 0 0 0.3553 1\n  O2- O1 0.3059 0 0.25 1\n");
    auto d = load(mp);
    check(d.species.size() == 2 && d.species[0] == "Fe" && d.species[1] == "O",
          "CIF oxidation-state symbols become elements");
    check(d.cell[1] == 0 && d.cell[2] == 0 && d.cell[5] == 0 && d.cell[6] == 0 && d.cell[7] == 0,
          "hexagonal CIF cell has exact zeros");
    io::write(dir / "POSCAR", io::Format::POSCAR, d);
    check(slurp(dir / "POSCAR").find("Fe O") != std::string::npos, "POSCAR export uses element names");

    // VESTA / ICSD / COD files carry semicolon text blocks.
    auto text = fixture(dir, "text.cif",
                        "data_x\n_publ_section_title\n;\nSilicon\n  at 300 K\n;\n"
                        "_cell_length_a 5.431\n_cell_length_b 5.431\n_cell_length_c 5.431\n"
                        "_cell_angle_alpha 90\n_cell_angle_beta 90\n_cell_angle_gamma 90\n"
                        "loop_\n_atom_site_label\n_atom_site_fract_x\n_atom_site_fract_y\n"
                        "_atom_site_fract_z\nSi1 0 0 0\nSi2 .25 .25 .25\n");
    d = load(text);
    check(d.atoms.size() == 2 && d.cell[3] == 0, "CIF semicolon text field");
    check(throwsWith(fixture(dir, "nocell.cif",
                             "data_x\nloop_\n_atom_site_label\n_atom_site_fract_x\n"
                             "_atom_site_fract_y\n_atom_site_fract_z\nSi1 0 0 0\n"),
                     "_cell_length_a"),
          "CIF without cell names the missing field");

    // VASP: negative scale is the cell volume; filename variants are common.
    const char *poscar = "Si\n-160\n1 0 0\n0 1 0\n0 0 1\nSi\n1\nDirect\n0 0 0\n";
    d = load(fixture(dir, "POSCAR_relaxed", poscar));
    check(std::abs(d.cell[0] * d.cell[4] * d.cell[8] - 160) < 1e-9, "POSCAR volume scale");
    check(io::detect(fixture(dir, "CONTCAR-3", poscar)) == io::Format::POSCAR, "CONTCAR-3 detected");
    check(!isPOSCAR(dir / "poscar_like.xyz"), "POSCAR prefix does not override .xyz");

    // LAMMPS data: types keep their numbers, Masses name elements, common styles load.
    auto swapped = fixture(dir, "swap.data",
                           "x\n\n3 atoms\n2 atom types\n\n0 4 xlo xhi\n0 4 ylo yhi\n0 4 zlo zhi\n\n"
                           "Atoms # atomic\n\n1 2 0 0 0\n2 1 2 2 2\n3 2 1 1 1\n");
    d = load(swapped);
    io::write(dir / "out.data", io::Format::LammpsData, d);
    check(slurp(dir / "out.data").find("1 2 0 0 0") != std::string::npos,
          "LAMMPS data save keeps type numbers");
    auto full = fixture(dir, "full.data",
                        "x\n\n2 atoms\n2 atom types\n\n0 4 xlo xhi\n0 4 ylo yhi\n0 4 zlo zhi\n\n"
                        "Masses\n\n1 15.9994 # O\n2 1.008\n\n"
                        "Atoms # full\n\n1 7 1 -0.8 0 0 0\n2 7 2 0.4 1 0 0 0 0 0\n");
    d = load(full);
    check(d.species.size() == 2 && d.species[0] == "O" && d.species[1] == "H",
          "Masses comment and mass identify elements");
    check(d.scalarProperties["Charge"].size() == 2 && d.scalarProperties["Charge"][0] == -0.8 &&
              d.scalarProperties["Molecule Identifier"][1] == 7,
          "full style charge and molecule columns");
    io::write(dir / "out.data", io::Format::LammpsData, d);
    d = load(dir / "out.data");
    check(d.species[0] == "O" && d.species[1] == "H", "exported Masses restore element names");
    auto lj = fixture(dir, "lj.data",
                      "x\n\n1 atoms\n1 atom types\n\n0 4 xlo xhi\n0 4 ylo yhi\n0 4 zlo zhi\n\n"
                      "Masses\n\n1 1.0\n\nAtoms # atomic\n\n1 1 0 0 0\n");
    check(load(lj).species[0] == "Type_1", "reduced-unit mass 1.0 is not hydrogen");

    // Parallel LAMMPS runs write dump rows in arbitrary order.
    auto dump = fixture(dir, "unsorted.dump",
                        "ITEM: TIMESTEP\n0\nITEM: NUMBER OF ATOMS\n3\nITEM: BOX BOUNDS pp pp pp\n"
                        "0 4\n0 4\n0 4\nITEM: ATOMS id type x y z\n3 3 3 3 3\n1 2 1 1 1\n2 2 2 2 2\n");
    d = load(dump);
    auto &ids = d.scalarProperties["id"];
    check(ids == std::vector<double>{1, 2, 3} && d.atoms[0].x == 1 && d.atoms[2].x == 3,
          "dump atoms sorted by id");
    check(d.species.size() == 3 && d.species[2] == "Type_3" && d.atoms[2].type == 2,
          "dump Type_N species keep numeric order");

    // A non-ASCII file name must never throw in a system code page conversion.
    check(lowerExtension(fs::path(L"序列.VASP")) == ".vasp", "non-ASCII stem keeps its extension");
    check(lowerExtension(fs::path(L"a.数据")) == ".??", "non-ASCII extension does not throw");

    fs::remove_all(dir);
    if (failures)
        return EXIT_FAILURE;
    std::cout << "PASS: oxidation-state CIF, exact cells, CIF text fields, POSCAR volume scale and "
                 "names, LAMMPS type numbering/Masses/styles, unsorted dumps\n";
}
