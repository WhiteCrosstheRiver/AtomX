#include "../src/fragment_library.hpp"
#include "../src/fragment_fusion.hpp"
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
        // Joining existing fragments is independent of the template library.
        Dataset pair=methyl.data;
        fragments::apply(pair,methyl,fragments::place(pair,methyl,1,{6,2,1},{1,0,0},{0,0,1}));
        pair.atoms.push_back({20,20,20,authoring::speciesIndex(pair,"O")});
        pair.scalarProperties["Charge"]=std::vector<double>(11);
        pair.scalarProperties["AtomX.MotionGroup"]=std::vector<double>(11,2);
        pair.vectorProperties["Force"]=std::vector<Vec3>(11,{1,2,3});
        pair.particleColors=std::vector<Vec3>(11,{.2f,.3f,.4f});
        for (size_t i=0;i<11;++i) pair.scalarProperties["Charge"][i]=double(i);
        const auto untouched=pair;
        auto seed=std::make_shared<fragments::FusionSeed>(fragments::prepareFusion(pair,1));
        auto fusion=fragments::rotated(fragments::alignFusion(pair,seed,6,{0,0,1}),.72);
        fragments::preflight(pair,fusion); const auto moved=fragments::apply(pair,fusion);
        check(formula(pair)==std::map<std::string,int>{{"C",2},{"H",6},{"O",1}} && pair.bonds.size()==7 && moved.size()==4,
              "two existing capped fragments lose only their terminal hydrogens and gain one bond");
        check(authoring::fragment(pair,0).size()==8 && pair.atoms[8].x==20 &&
              pair.scalarProperties.at("Charge")==std::vector<double>{0,2,3,4,5,7,8,9,10} &&
              pair.scalarProperties.at("AtomX.MotionGroup")[0]==2 && pair.particleColors.size()==9,
              "fusion preserves unrelated atoms, scalar rows, colors and motion groups with identity compaction");
        check(pair.atoms[4].x==untouched.atoms[5].x && pair.atoms[4].y==untouched.atoms[5].y &&
              pair.vectorProperties.at("Force")[4].x==1 && pair.vectorProperties.at("Force")[8].z==3,
              "fixed target and unrelated vectors are unchanged");
        const auto transformed=fragments::fusionVector(fusion,{1,2,3});
        check(authoring::length(authoring::sub(pair.vectorProperties.at("Force")[0],transformed))<1e-6 &&
              std::abs(authoring::length(authoring::sub(fragments::at(pair,0),fragments::at(pair,1)))-1.09)<1e-4,
              "moving scientific vectors follow rigid rotation and internal bond lengths are retained");
        pair=untouched; rejected=false;
        try { fragments::alignFusion(pair,seed,2,{0,0,1}); } catch (...) { rejected=true; }
        check(rejected,"same connected component cannot be fused with itself");
        pair.bonds[0].order=2; rejected=false;
        try { fragments::apply(pair,fusion); } catch (...) { rejected=true; }
        check(rejected && pair.atoms.size()==11,"changed topology is rejected before mutation");
        pair=untouched; pair.bonds[1].image={1,0,0}; rejected=false;
        try { fragments::prepareFusion(pair,1); } catch (...) { rejected=true; }
        check(rejected,"moving fragments containing any periodic edge are rejected");
        pair=untouched; std::vector<uint8_t> hidden(11); hidden[6]=1; rejected=false;
        try { fragments::preflight(pair,fusion,hidden); } catch (...) { rejected=true; }
        check(rejected,"hidden target connector is rejected");
        pair.atoms[5].y+=1; rejected=false;
        try { fragments::apply(pair,fusion); } catch (...) { rejected=true; }
        check(rejected && pair.atoms.size()==11,"changed fixed neighbor invalidates the pending gesture");
        Dataset retained; retained.species={"C"}; retained.atoms={{0,0,0,0},{1.5,0,0,0},{8,0,0,0},{9.5,0,0,0}};
        retained.bonds={{0,1,{},1},{2,3,{},2}};
        auto retainedSeed=std::make_shared<fragments::FusionSeed>(fragments::prepareFusion(retained,1));
        fragments::apply(retained,fragments::alignFusion(retained,retainedSeed,2,{0,0,1}));
        check(retained.atoms.size()==4 && retained.bonds.size()==3 && retained.bonds[1].order==2,
              "nonhydrogen terminal atoms are both retained with existing bond orders");
        Dataset large; large.species={"C"};
        for (int i=0;i<600;++i) { large.atoms.push_back({float(i)*1.5f,0,0,0}); if(i) large.bonds.push_back({uint32_t(i-1),uint32_t(i),{},1}); }
        large.atoms.push_back({1000,0,0,0}); large.atoms.push_back({1001.5f,0,0,0}); large.bonds.push_back({600,601,{},1});
        auto largeSeed=std::make_shared<fragments::FusionSeed>(fragments::prepareFusion(large,0));
        auto largeFusion=fragments::alignFusion(large,largeSeed,600,{0,0,1});
        check(fragments::rotated(largeFusion,.3).seed==largeSeed && fragments::apply(large,largeFusion).size()==600,
              "existing-fragment fusion has no library size cap and drag previews share cached topology");
        std::filesystem::remove_all(directory); std::cout<<"fragment library and fusion tests: PASS\n"; return 0;
    } catch (const std::exception &e) {
        std::filesystem::remove_all(directory); std::cerr<<e.what()<<'\n'; return 1;
    }
}
