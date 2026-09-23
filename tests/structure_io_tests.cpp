#include "../src/structure_io.hpp"
#include <chrono>
#include <iostream>
using namespace atomx;
void require(bool b, const char *s) {
    if (!b)
        throw std::runtime_error(s);
}
int main() {
    auto dir =
        std::filesystem::temp_directory_path() /
        ("atomx-io-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(dir);
    try {
        Dataset d;
        d.species = {"Cu", "Ni"};
        d.cell = {4, 0, 0, 1, 5, 0, .5, 1, 6};
        d.pbc = {true, false, true};
        d.atoms = {{1, 2, 3, 1}, {2, 3, 4, 0}};
        d.sourceCount = 2;
        d.scalarProperties["Energy"] = {-2.5, 3.25};
        d.vectorProperties["Force"] = {{1, 2, 3}, {4, 5, 6}};
        io::ExportOptions opt;
        opt.scalarProperties = {"Energy"};
        opt.vectorProperties = {"Force"};
        auto p = dir / "sample.xyz";
        io::write(p, io::Format::XYZ, d, opt);
        auto r = io::read(p, io::index(p)[0]);
        require(r.scalarProperties.at("Energy")[0] == -2.5 &&
                    r.vectorProperties.at("Force")[1].z == 6,
                "XYZ property roundtrip");
        auto trajectoryPath = dir / "color-range.xyz";
        Dataset trajectory; trajectory.species={"X"}; trajectory.atoms={{1,0,0,0},{3,0,0,0}};
        trajectory.scalarProperties["Q"]={-5,8}; trajectory.bounds();
        io::ExportOptions trajectoryOptions; trajectoryOptions.scalarProperties={"Q"};
        {
            std::ofstream trajectoryFile(trajectoryPath);
            io::writeFrame(trajectoryFile, io::Format::XYZ, trajectory, trajectoryOptions, 0);
            trajectory.atoms={{-4,0,0,0},{7,0,0,0}}; trajectory.scalarProperties["Q"]={2,12};
            io::writeFrame(trajectoryFile, io::Format::XYZ, trajectory, trajectoryOptions, 1);
        }
        auto trajectoryFrames=io::index(trajectoryPath);
        auto fileRange=colorRangeAcrossFrames(trajectoryFrames.size(),[&](size_t index) {
            const auto &frame=trajectoryFrames.at(index);
            return io::read(trajectoryPath,frame,frame.count);
        },{},"Q");
        require(fileRange.first==-5 && fileRange.second==12,
                "all-frame range scans full Extended XYZ trajectory frames");
        auto filtered = evaluate(r, {{Op::SelectType, true, 0, 2, 0}, {Op::Delete}});
        require(filtered.data.atoms.size() == 1 &&
                    filtered.data.scalarProperties.at("Energy")[0] == 3.25,
                "delete keeps property rows aligned");
        opt.extendedXYZ = false;
        io::write(p, io::Format::XYZ, d, opt);
        r = io::read(p, io::index(p)[0]);
        require(r.scalarProperties.empty() && r.atoms.size() == 2, "basic XYZ omits properties");
        opt.scalarProperties = {"does-not-exist"};
        io::write(p, io::Format::XYZ, d, opt);
        std::ostringstream formatCheck;
        io::writeFrame(formatCheck, io::Format::POSCAR, d, opt);
        require(!formatCheck.str().empty(), "hidden XYZ options do not affect other formats");
        opt = {};
        p = dir / "sample.dump";
        d.origin = {-1, -2, -3};
        io::write(p, io::Format::LammpsDump, d);
        r = io::read(p, io::index(p)[0]);
        require(r.cell == d.cell && r.pbc == d.pbc && r.origin.y == -2 && r.atoms[0].z == 3,
                "dump restricted triclinic cell and origin");
        {
            std::ofstream f(p);
            io::writeFrame(f, io::Format::LammpsDump, d, {}, 10);
            d.atoms[0].x = 2;
            io::writeFrame(f, io::Format::LammpsDump, d, {}, 20);
        }
        auto frames = io::index(p);
        require(frames.size() == 2, "dump frame index");
        r = io::read(p, frames[1], 1);
        require(r.atoms[0].x == 2 && r.sampled() && r.sourceCount == 2, "dump seek and budget");
        {
            std::ofstream f(p);
            f << "ITEM: TIMESTEP\n0\nITEM: NUMBER OF ATOMS\n1\nITEM: BOX BOUNDS pp ff pp\n-2 8\n0 "
                 "10\n0 10\nITEM: ATOMS type zs xs ys id\n1 .3 .1 .2 7\n";
        }
        r = io::read(p, io::index(p)[0]);
        require(r.atoms[0].x == -1 && r.atoms[0].y == 2 && r.atoms[0].z == 3,
                "scaled reordered dump columns");
        p = dir / "sample.gro";
        d.origin = {};
        d.atoms[0].x = 1;
        io::write(p, io::Format::GRO, d);
        r = io::read(p, io::index(p)[0]);
        require(std::abs(r.atoms[0].z - 3) < 1e-5 && r.cell == d.cell,
                "GRO nm conversion and nine-value cell");
        auto staticFrames = io::index(p);
        require(staticFrames.size() == 1 && staticFrames[0].count == 0,
                "static structure formats expose one frame with unknown indexed atom count");
        auto staticRange = colorRangeAcrossFrames(staticFrames.size(), [&](size_t index) {
            return io::read(p, staticFrames.at(index), 2000001);
        }, {}, "Position.X");
        auto minX = std::min(r.atoms[0].x, r.atoms[1].x);
        auto maxX = std::max(r.atoms[0].x, r.atoms[1].x);
        require(!r.sampled() && staticRange.first == minX && staticRange.second == maxX,
                "all-frame range scans a complete static GRO structure");
        p = dir / "sample.cif";
        io::write(p, io::Format::CIF, d);
        r = io::read(p, io::index(p)[0]);
        require(std::abs(r.atoms[1].x - 2) < 1e-5 && std::abs(r.atoms[0].z - 3) < 1e-5,
                "CIF triclinic roundtrip");
        p = dir / "sample.data";
        io::write(p, io::Format::LammpsData, d);
        r = io::read(p, io::index(p)[0]);
        require(r.cell == d.cell && r.atoms.size() == 2, "atomic data roundtrip");
        p = dir / "sample.vasp";
        opt = {};
        opt.fractionalPOSCAR = true;
        io::write(p, io::Format::POSCAR, d, opt);
        r = io::read(p, io::index(p)[0]);
        require(std::abs(r.atoms[0].x - 2) < 1e-5 && std::abs(r.atoms[0].z - 4) < 1e-5 &&
                    r.species[r.atoms[0].type] == "Cu",
                "fractional POSCAR row-vector inverse and grouping");
        p = dir / "sample.pdb";
        {
            std::ofstream f(p);
            f << "ATOM      1  CA  ALA A   1       1.000   2.000   3.000  1.00  0.00           C  "
                 "\nEND\n";
        }
        r = io::read(p, io::index(p)[0]);
        require(r.atoms.size() == 1 && r.species[0] == "C" && r.atoms[0].y == 2,
                "PDB fixed columns");
        p = dir / "selective.vasp";
        d.vectorProperties["MoveMask"] = {{1, 0, 1}, {0, 1, 0}};
        opt = {};
        io::write(p, io::Format::POSCAR, d, opt);
        r = io::read(p, io::index(p)[0]);
        require(r.vectorProperties.at("MoveMask")[0].y == 1 &&
                    r.vectorProperties.at("MoveMask")[1].y == 0,
                "selective dynamics follows grouped particle rows");
        bool failed = false;
        try {
            io::detect(dir / "sample.xyz.gz");
        } catch (...) {
            failed = true;
        }
        require(failed, "unsupported compression explicit");
        std::atomic<bool> cancelled{true};
        failed = false;
        try {
            io::read(p, {0, 0, ""}, 10, nullptr, &cancelled);
        } catch (...) {
            failed = true;
        }
        require(failed, "cancel read");
        p = dir / "preserve.vasp";
        {
            std::ofstream f(p);
            f << "keep";
        }
        d.cell = {};
        failed = false;
        try {
            io::write(p, io::Format::POSCAR, d);
        } catch (...) {
            failed = true;
        }
        std::ifstream f(p);
        std::string text;
        f >> text;
        require(failed && text == "keep", "invalid export preserves destination");
        f.close();
        for (const auto &e : std::filesystem::directory_iterator(dir))
            std::filesystem::remove(e.path());
        std::filesystem::remove(dir);
        std::cout << "PASS: native format reads, property alignment, multi-frame seek/color range, triclinic "
                     "roundtrips, export validation\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "; fixtures: " << dir << '\n';
        return 1;
    }
}
