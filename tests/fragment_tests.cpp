#include "../src/fragment_library.hpp"
#include <chrono>
#include <iostream>
using namespace atomx;
void check(bool value,const char *message) { if (!value) throw std::runtime_error(message); }
std::map<std::string,int> formula(const Dataset &d) {
    std::map<std::string,int> counts; for (auto a:d.atoms) ++counts[d.species[a.type]]; return counts;
}
int main() {
    const auto directory=std::filesystem::temp_directory_path()/
        ("atomx-fragments-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    try {
        const auto library=fragments::builtins();
        check(library.size()==11,"starter library has hydrocarbons, functional groups, aromatic ring and halogens");
        for (const auto &t:library) fragments::validate(t);
        const auto &methyl=library[0],&oh=library[2];
        check(formula(methyl.data)==std::map<std::string,int>{{"C",1},{"H",4}},"isolated methyl template is capped methane");
        Dataset empty;
        auto p=fragments::place(empty,methyl,methyl.connector,{4,3,2},{1,0,0},{0,0,1});
        fragments::preflight(empty,methyl,p); fragments::apply(empty,methyl,p);
        check(empty.atoms.size()==5 && empty.bonds.size()==4 && fragments::at(empty,0).x==4,"isolated placement retains connection hydrogen and explicit topology");
        auto original=empty;
        auto ethane=fragments::place(empty,methyl,1,{}, {1,0,0},{0,0,1},1);
        ethane=fragments::rotated(ethane,.72); fragments::preflight(empty,methyl,ethane);
        const auto selected=fragments::apply(empty,methyl,ethane);
        check(formula(empty)==std::map<std::string,int>{{"C",2},{"H",6}} && empty.bonds.size()==7 && selected.size()==4,
              "terminal hydrogen substitution forms ethane with one interfragment bond");
        check(authoring::fragment(empty,0).size()==8,"attached template has one connected component");
        empty=original; empty.scalarProperties["Charge"]={1,2,3,4,5};
        empty.vectorProperties["Velocity"]=std::vector<Vec3>(5,{1,2,3});
        empty.particleColors=std::vector<Vec3>(5,{.2f,.3f,.4f});
        auto methanol=fragments::place(empty,oh,1,{}, {1,0,0},{0,0,1},1);
        fragments::preflight(empty,oh,methanol); fragments::apply(empty,oh,methanol);
        check(formula(empty)==std::map<std::string,int>{{"C",1},{"H",4},{"O",1}} && empty.bonds.size()==5,"hydroxyl substitution yields methanol");
        check(empty.scalarProperties.at("Charge")[1]==3 && std::isnan(empty.scalarProperties.at("Charge").back()) &&
              empty.vectorProperties.at("Velocity").size()==6 && empty.particleColors.size()==6,
              "terminal substitution remaps scientific properties and fills unmeasured rows");
        auto custom=fragments::define(empty,5,"custom methanol","My library");
        const auto path=directory/"custom.atomx"; fragments::save(path,custom); auto reopened=fragments::load(path);
        check(reopened.name=="custom methanol" && reopened.category=="My library" && reopened.connector==5 &&
              reopened.data.bonds==custom.data.bonds && reopened.data.scalarProperties.at("Charge")[1]==3,
              "custom fragment survives disk round trip with terminal connector and scientific data");
        bool rejected=false;
        try { fragments::define(empty,0,"invalid","My library"); } catch (...) { rejected=true; }
        check(rejected,"nonterminal custom connector rejected");
        rejected=false; auto invalid=p; invalid.points[0].x=NAN;
        try { fragments::preflight(original,methyl,invalid); } catch (...) { rejected=true; }
        check(rejected && original.atoms.size()==5,"invalid geometry rejected before source changes");
        rejected=false;
        try { fragments::place(original,methyl,1,{}, {1,0,0},{0,0,1},0); } catch (...) { rejected=true; }
        check(rejected,"nonterminal target rejected instead of creating excessive arbitrary bonds");
        auto stale=fragments::place(original,methyl,1,{}, {1,0,0},{0,0,1},1);
        original.atoms[0].x+=1; rejected=false;
        try { fragments::preflight(original,methyl,stale); } catch (...) { rejected=true; }
        check(rejected,"gesture cannot attach to a changed anchor");
        rejected=false;
        try { fragments::save(path,custom); } catch (...) { rejected=true; }
        check(rejected && fragments::load(path).name==custom.name,"library saves never truncate existing entries");
        // Vectors follow the fragment's rigid rotation, while existing rows stay unchanged.
        auto directional=methyl; directional.data.vectorProperties["Force"]=std::vector<Vec3>(5,{1,0,0});
        Dataset target; auto rotated=fragments::rotated(fragments::place(target,directional,1,{}, {0,1,0},{0,0,1}),.6);
        fragments::preflight(target,directional,rotated); fragments::apply(target,directional,rotated);
        const auto force=target.vectorProperties.at("Force")[0];
        check(std::abs(force.x+std::sin(.6))<1e-5 && std::abs(force.y-std::cos(.6))<1e-5,"fragment vector properties rotate with geometry");
        std::filesystem::remove_all(directory); std::cout<<"fragment library tests: PASS\n"; return 0;
    } catch (const std::exception &e) {
        std::filesystem::remove_all(directory); std::cerr<<e.what()<<'\n'; return 1;
    }
}
