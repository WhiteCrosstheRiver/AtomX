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
        auto verifyStaticColorRange = [&](const std::filesystem::path &staticPath) {
            auto indexed = io::index(staticPath);
            require(indexed.size() == 1 && indexed[0].count == 0,
                    "static input formats expose one frame with unknown indexed atom count");
            auto full = io::read(staticPath, indexed[0], 2000001);
            require(!full.sampled() && full.atoms.size() == full.sourceCount,
                    "static all-frame color range reads the full structure");
            auto values = particlePropertyValues(full, "Position.X");
            auto expected = std::minmax_element(values.begin(), values.end());
            auto actual = colorRangeAcrossFrames(indexed.size(), [&](size_t frame) {
                return io::read(staticPath, indexed.at(frame), 2000001);
            }, {}, "Position.X");
            double lo = *expected.first, hi = *expected.second;
            if (lo == hi) { lo -= .5; hi += .5; }
            require(actual.first == lo && actual.second == hi,
                    "all-frame color range scans a complete static structure");
        };
        io::ExportOptions opt;
        {
            auto native=d;
            native.bonds={{0,1,{1,0,0},3}}; native.bondStyle.radius=.12f;
            native.comment="原子结构\n二进制\r\n";
            native.scalarProperties["Unknown"]={NAN,3};
            native.particleColors={{.2f,.3f,.4f},{-1,-1,-1}};
            native.globalAttributes["Energy"]=-5;
            native.propertyComponents["Force"]="X,Y,Z";
            native.tables={{"科学分析",{"r","g(r)"},{{"1.0","2.0"}}}};
            io::ExportOptions nativeOptions;
            auto &view=nativeOptions.documentView;
            view.creation=true; view.camera={.8f,.3f,.5f,.1f,-.1f}; view.title="晶体 · 创作";
            view.display.visibility(2,{0},0);
            view.display.setLabels(2,{1},{creation::LabelKind::Custom,"测试原子\n换行"},false);
            view.display.defaultPreset=3; view.display.presets[1]=4; view.display.ballRadius=.55f; view.display.cpkScale=.8f;
            view.display.monitors={{2,{0,1,-1,-1}}}; view.display.activeMonitor=0; view.display.monitorsVisible=false;
            view.selection={1}; view.order=3; view.continuous=false; view.ringSize=5; view.autoHydrogens=true;
            const auto nativePath=dir/"model.atomx";
            io::write(nativePath,io::Format::AtomX,native,nativeOptions);
            auto decoded=document::read(nativePath);
            require(decoded.data.bonds==native.bonds && decoded.data.cell==native.cell && decoded.data.pbc==native.pbc &&
                    decoded.data.atoms[1].x==native.atoms[1].x && decoded.data.comment==native.comment,
                    "native document preserves explicit bond orders, images, cell and binary-safe strings");
            require(decoded.view.display==view.display && decoded.view.selection==view.selection &&
                    decoded.view.camera==view.camera && decoded.view.title==view.title && !decoded.view.continuous && decoded.view.ringSize==5 && decoded.view.autoHydrogens,
                    "native document preserves UTF-8 labels, visibility, selection and camera");
            require(decoded.data.tables[0].name==native.tables[0].name &&
                    decoded.data.scalarProperties.at("Energy")==native.scalarProperties.at("Energy") &&
                    std::isnan(decoded.data.scalarProperties.at("Unknown")[0]) && decoded.data.vectorProperties.at("Force")[1].z==6 &&
                    decoded.data.propertyComponents==native.propertyComponents && decoded.data.globalAttributes==native.globalAttributes &&
                    decoded.data.particleColors[0].y==.3f,"native document preserves scientific properties, tables and missing values");
            require(io::detect(nativePath)==io::Format::AtomX && io::read(nativePath,io::index(nativePath)[0]).bonds==native.bonds,
                    "ordinary structure loading recognizes native documents and preserves topology");
            bool rejected=false;
            try { (void)document::read(nativePath,1); } catch (...) { rejected=true; }
            require(rejected,"native document refuses a sampled import that would lose topology and annotations");
            std::ifstream bytesIn(nativePath,std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(bytesIn)),{}); bytesIn.close();
            auto rejectBytes=[&](std::string corrupted) {
                const auto badPath=dir/"bad.atomx";
                { std::ofstream output(badPath,std::ios::binary); output.write(corrupted.data(),std::streamsize(corrupted.size())); }
                bool invalid=false; try { (void)document::read(badPath); } catch (...) { invalid=true; }
                require(invalid,"malformed document rejected before publication");
            };
            rejectBytes(bytes.substr(0,bytes.size()-1));
            auto version=bytes; version[8]=9; rejectBytes(version);
            auto block=bytes; for (int i=12;i<20;++i) block[size_t(i)]=char(-1); rejectBytes(block);
            auto invalidAuto=bytes; invalidAuto[invalidAuto.size()-5]=2; rejectBytes(invalidAuto);
            auto version7=bytes; version7[8]=7; version7.erase(version7.size()-5,1);
            const auto version7Path=dir/"v7.atomx";
            { std::ofstream out(version7Path,std::ios::binary); out.write(version7.data(),std::streamsize(version7.size())); }
            require(!document::read(version7Path).view.autoHydrogens && document::read(version7Path).view.display==view.display,
                "old documents default automatic chemistry to off");
            const size_t labelBytes=20+16*view.display.labels.size();
            const size_t colorBytes=44+view.display.defaultColor.property.size()+8;
            auto version6=version7; version6[8]=6; version6.erase(version6.size()-4-colorBytes,colorBytes);
            const auto version6Path=dir/"v6.atomx";
            { std::ofstream out(version6Path,std::ios::binary); out.write(version6.data(),std::streamsize(version6.size())); }
            require(document::read(version6Path).view.display==view.display,"v6 labels remain compatible with source colors");
            auto version5=version6; version5[8]=5; version5.erase(version5.size()-4-labelBytes,labelBytes);
            const auto version5Path=dir/"v5.atomx";
            { std::ofstream out(version5Path,std::ios::binary); out.write(version5.data(),std::streamsize(version5.size())); }
            require(document::read(version5Path).view.display==view.display,"v5 retains monitors and legacy labels without composite fields");
            const size_t monitorBytes=13+17*view.display.monitors.size();
            auto version4=version5; version4[8]=4; version4.erase(version4.size()-4-monitorBytes,monitorBytes);
            const auto version4Path=dir/"v4.atomx";
            { std::ofstream out(version4Path,std::ios::binary); out.write(version4.data(),std::streamsize(version4.size())); }
            const auto legacy4=document::read(version4Path);
            require(legacy4.view.display.presets==view.display.presets && legacy4.view.display.monitors.empty(),"v4 retains appearance and defaults to no monitors");
            auto invalidMonitor=version5; invalidMonitor[invalidMonitor.size()-4-17]=5; rejectBytes(invalidMonitor);
            auto invalidMonitorIndex=version5; for(int i=0;i<4;++i) invalidMonitorIndex[invalidMonitorIndex.size()-4-16+i]=char(-1); rejectBytes(invalidMonitorIndex);
            auto version3=version4; version3[8]=3;
            const size_t styleBytes=25+5*view.display.presets.size();
            version3.erase(version3.size()-4-styleBytes,styleBytes);
            const auto version3Path=dir/"v3.atomx";
            { std::ofstream out(version3Path,std::ios::binary); out.write(version3.data(),std::streamsize(version3.size())); }
            require(document::read(version3Path).view.display.defaultPreset==0 && document::read(version3Path).view.display.presets.empty(),
                    "version 3 documents use original appearance without style overrides");
            auto badPreset=version5; badPreset[badPreset.size()-4-monitorBytes-styleBytes]=5; rejectBytes(badPreset);
            auto badIndex=version5; for(int i=0;i<4;++i) badIndex[badIndex.size()-monitorBytes-9+i]=char(-1); rejectBytes(badIndex);
            auto version2=version3; version2[8]=2;
            const size_t fragmentBytes=8+view.fragmentKey.size()+4;
            version2.erase(version2.size()-4-fragmentBytes,fragmentBytes);
            const auto version2Path=dir/"v2.atomx";
            { std::ofstream out(version2Path,std::ios::binary); out.write(version2.data(),std::streamsize(version2.size())); }
            require(document::read(version2Path).view.ringSize==5 && document::read(version2Path).view.fragmentKey=="builtin/methyl",
                    "version 2 documents retain ring settings and default fragment selection");
            auto legacy=version2; legacy[8]=1; legacy.erase(legacy.size()-8,4);
            const auto legacyPath=dir/"legacy.atomx";
            { std::ofstream out(legacyPath,std::ios::binary); out.write(legacy.data(),std::streamsize(legacy.size())); }
            require(document::read(legacyPath).view.ringSize==6 && document::read(legacyPath).data.bonds==native.bonds,
                    "version 1 documents remain readable with default ring size");
            auto aromatic=native; aromatic.bonds[0].order=4;
            auto ringOptions=nativeOptions; ringOptions.documentView.tool=5;
            const auto ringPath=dir/"aromatic.atomx";
            io::write(ringPath,io::Format::AtomX,aromatic,ringOptions);
            require(document::read(ringPath).data.bonds[0].order==4 && document::read(ringPath).view.tool==5,
                    "native document retains aromatic topology and ring tool settings");
            auto fragmentOptions=nativeOptions; fragmentOptions.documentView.tool=6;
            fragmentOptions.documentView.fragmentKey="builtin/phenyl"; fragmentOptions.documentView.fragmentConnector=6;
            const auto fragmentPath=dir/"fragment-tool.atomx";
            io::write(fragmentPath,io::Format::AtomX,native,fragmentOptions);
            const auto fragmentRestored=document::read(fragmentPath);
            require(fragmentRestored.view.tool==6 && fragmentRestored.view.fragmentKey=="builtin/phenyl" && fragmentRestored.view.fragmentConnector==6,
                    "version 4 persists active fragment key and changed terminal connector");
            rejectBytes(bytes+"trailing data");
            auto compositeOptions=nativeOptions; auto &composite=compositeOptions.documentView.display;
            using F=creation::LabelFieldKind;
            composite.setLabels(2,{}, {creation::LabelKind::Properties,"测量",{{F::Scalar,"Energy"},{F::VectorMagnitude,"Force"}},7},true);
            composite.setLabels(2,{1},{creation::LabelKind::Properties,"",{{F::Mass,{}}},4},false);
            const auto compositePath=dir/"property-labels.atomx";
            io::write(compositePath,io::Format::AtomX,native,compositeOptions);
            const auto compositeRead=document::read(compositePath);
            require(compositeRead.view.display==composite && creation::labelText(compositeRead.data,0,compositeRead.view.display.labelAt(0)).find("Energy = -2.5")!=std::string::npos,
                "v6 restores composite property names, order, prefix, precision and sparse selected overrides");
            std::ifstream compositeIn(compositePath,std::ios::binary); std::string compositeBytes((std::istreambuf_iterator<char>(compositeIn)),{});
            compositeBytes.erase(compositeBytes.size()-5,1); compositeBytes[8]=6; compositeBytes.erase(compositeBytes.size()-4-colorBytes,colorBytes);
            auto badField=compositeBytes; badField[badField.size()-4-9]=char(255); rejectBytes(badField);
            auto badPrecision=compositeBytes; badPrecision[badPrecision.size()-4-21]=0; rejectBytes(badPrecision);
            creation::ColorRule color; color.kind=creation::ColorKind::Property; color.property="Energy";color.low=-4;color.high=2;color.gradient=8;color.reverse=true;
            composite.setColor(2,{},color,true); color.kind=creation::ColorKind::Custom;color.rgb={.1f,.8f,.3f};
            composite.setColor(2,{1},color,false);
            io::write(compositePath,io::Format::AtomX,native,compositeOptions);
            require(document::read(compositePath).view.display==composite,"v7 restores global color range and sparse custom overrides");
            std::ifstream colorIn(compositePath,std::ios::binary); std::string colorData((std::istreambuf_iterator<char>(colorIn)),{});
            colorData.erase(colorData.size()-5,1);colorData[8]=7;auto badColor=colorData;badColor[badColor.size()-4-(44+color.property.size())]=char(255);rejectBytes(badColor);
            auto invalid=native; invalid.bonds[0].a=999;
            rejected=false; try { io::write(nativePath,io::Format::AtomX,invalid,nativeOptions); } catch (...) { rejected=true; }
            require(rejected && document::read(nativePath).data.bonds==native.bonds,
                    "invalid native export preserves an existing destination");
            std::atomic<bool> cancel{true}; rejected=false;
            try { (void)document::read(nativePath,2,&cancel); } catch (...) { rejected=true; }
            require(rejected,"native binary reads honor cancellation");
        }
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
        verifyStaticColorRange(p);
        p = dir / "sample.cif";
        io::write(p, io::Format::CIF, d);
        r = io::read(p, io::index(p)[0]);
        require(std::abs(r.atoms[1].x - 2) < 1e-5 && std::abs(r.atoms[0].z - 3) < 1e-5,
                "CIF triclinic roundtrip");
        verifyStaticColorRange(p);
        p = dir / "sample.data";
        io::write(p, io::Format::LammpsData, d);
        r = io::read(p, io::index(p)[0]);
        require(r.cell == d.cell && r.atoms.size() == 2, "atomic data roundtrip");
        verifyStaticColorRange(p);
        p = dir / "sample.vasp";
        opt = {};
        opt.fractionalPOSCAR = true;
        io::write(p, io::Format::POSCAR, d, opt);
        r = io::read(p, io::index(p)[0]);
        require(std::abs(r.atoms[0].x - 2) < 1e-5 && std::abs(r.atoms[0].z - 4) < 1e-5 &&
                    r.species[r.atoms[0].type] == "Cu",
                "fractional POSCAR row-vector inverse and grouping");
        verifyStaticColorRange(p);
        p = dir / "sample.pdb";
        {
            std::ofstream f(p);
            f << "ATOM      1  CA  ALA A   1       1.000   2.000   3.000  1.00  0.00           C  "
                 "\nEND\n";
        }
        r = io::read(p, io::index(p)[0]);
        require(r.atoms.size() == 1 && r.species[0] == "C" && r.atoms[0].y == 2,
                "PDB fixed columns");
        verifyStaticColorRange(p);
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
        // Drag-and-drop routes through the same io::detect acceptance check as
        // the Open dialog: every supported drop extension must be detected
        // without content sniffing, unknown extensions must reject.
        for (const char *ext : {".xyz", ".extxyz", ".cif", ".data", ".lmp",
                                ".dump", ".lammpstrj", ".pdb", ".ent", ".gro"}) {
            const auto dropped = dir / ("drop-test" + std::string(ext));
            {
                std::ofstream f(dropped);
                f << "stub\n";
            }
            failed = false;
            try {
                io::detect(dropped);
            } catch (...) {
                failed = true;
            }
            require(!failed, (std::string("dropped file accepted by extension: ") + ext).c_str());
        }
        {
            const auto dropped = dir / "drop-test.txt";
            {
                std::ofstream f(dropped);
                f << "not a structure\n";
            }
            failed = false;
            try {
                io::detect(dropped);
            } catch (...) {
                failed = true;
            }
            require(failed, "unsupported dropped extension rejected");
        }
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
